// PortAudio device selection, PCM16 collection, recorded playback and WAV export. The duplex
// callback executes on an audio thread; vector growth and file writing run elsewhere.

#include "Audio.h"
#include "PitchJitter.h"
#include "Dsp.h"
#include "Stream.h"
#include "Timbre.h"
#include "Prosody.h"
#include "Formant.h"
#include "Wav.h"
#include "RecordingQueue.h"
#include "WasapiPortAudio.h"
#include <memory>
#include <thread>
#include <condition_variable>

atomic<int> g_requestedWasapiMode{0}; // Shared by default; UI changes this before reopen.
atomic<int> g_activeWasapiMode{-2}; // -2 unavailable, -1 other host, 0 Shared, 1 Exclusive.

// The duplex callback is the sole queue producer. One worker per recording is the sole queue
// consumer; it owns vector growth and copies its batches into the saved PCM buffer.
static RecordingQueue recordingQueue;
static thread recordingCollector;
atomic<unsigned> g_duplexCallbacksInFlight{0};
static atomic<bool> recordingCollectorFailed{false};
static atomic<bool> recordingIncomplete{false};
static mutex recordingWakeMutex;
static condition_variable recordingWake;

void QueuePortAudioRecording(const float* samples, size_t frames) noexcept {
    // The duplex callback registered its lifetime before reading the capture flag. Stop waits
    // for that lifetime to end before the queue is reset or the collector is joined.
    if (paIsRecording.load()) recordingQueue.Push(samples, frames);
}

static void CollectPortAudioRecording() noexcept {
    array<int16_t, 4096> batch{};
    try {
        for (;;) {
            const size_t count = recordingQueue.Pull(batch.data(), batch.size());
            if (count != 0) {
                lock_guard<mutex> lock(paAudioBufferMutex);
                const size_t first = paAudioBuffer.size();
                paAudioBuffer.resize(first + count * 2);
                for (size_t i = 0; i < count; ++i) {
                    const int sample = batch[i];
                    paAudioBuffer[first + i * 2] = static_cast<Uint8>(sample & 0xFF);
                    paAudioBuffer[first + i * 2 + 1] =
                        static_cast<Uint8>((sample >> 8) & 0xFF);
                }
                continue;
            }
            if (!paIsRecording.load() && g_duplexCallbacksInFlight.load() == 0 &&
                recordingQueue.Empty()) break;
            // No callback notification or system call: poll outside the audio thread. A queue
            // holding 5.46 seconds of mono PCM provides ample room for this 10 ms interval.
            unique_lock<mutex> lock(recordingWakeMutex);
            recordingWake.wait_for(lock, chrono::milliseconds(10), [] {
                return !paIsRecording.load();
            });
        }
    } catch (...) {
        // A failed vector growth must never unwind through std::thread into std::terminate.
        recordingCollectorFailed.store(true, memory_order_release);
        paIsRecording.store(false);
    }
}

// Little-endian helpers keep PCM16 byte buffers independent of pointer alignment and endianness.
static int16_t ReadPcm16Sample(const vector<Uint8>& bytes, size_t sampleIndex) noexcept {
    const size_t offset = sampleIndex * 2;
    const int bits = static_cast<int>(bytes[offset]) |
        (static_cast<int>(bytes[offset + 1]) << 8);
    return static_cast<int16_t>(bits < 32768 ? bits : bits - 65536);
}

static void WritePcm16Sample(vector<Uint8>& bytes, size_t sampleIndex, float sample) noexcept {
    const float scaled = clamp(sample * 32768.0f, -32768.0f, 32767.0f);
    const int value = static_cast<int>(lround(scaled));
    const size_t offset = sampleIndex * 2;
    bytes[offset] = static_cast<Uint8>(value & 0xFF);
    bytes[offset + 1] = static_cast<Uint8>((value >> 8) & 0xFF);
}

// Stateful variable-delay pitch processor with a crossfade between read positions.
struct RealtimePitchShifter {
    // The pitch processor writes a circular delay line and keeps its fractional read head between
    // samples.
    // Preserve the original 32 ms delay and 6 ms transition at the new sample rate.
    static constexpr size_t DELAY_BUFFER_SIZE = static_cast<size_t>(PA_SAMPLE_RATE * 0.032f);
    static constexpr size_t CROSSFADE_SIZE = static_cast<size_t>(PA_SAMPLE_RATE * 0.006f);
    static constexpr float EDGE_MARGIN = PA_SAMPLE_RATE * 0.0015f;

    vector<float> delayLine;
    vector<float> nextSegment;
    array<float, CROSSFADE_SIZE> fadeOut;
    array<float, CROSSFADE_SIZE> fadeIn;

    size_t writePos = 0;
    float readPos = 0.0f;

    size_t crossfadePhase = 0;
    bool isCrossfading = false;

    float readPosPrev = 0.0f;
    float readPosNext = 0.0f;

    float currentPitchRatio = 1.0f;
    int cooldownFrames = 0;

    RealtimePitchShifter() {
        delayLine.resize(DELAY_BUFFER_SIZE, 0.0f);
        nextSegment.resize(CROSSFADE_SIZE, 0.0f);
        for (size_t i = 0; i < CROSSFADE_SIZE; ++i) {
            const float t = static_cast<float>(i) / (CROSSFADE_SIZE - 1);
            const float cosine = cosf(3.14159265f * t);
            fadeOut[i] = 0.5f * (1.0f + cosine);
            fadeIn[i] = 0.5f * (1.0f - cosine);
        }
        readPos = DELAY_BUFFER_SIZE / 2.0f;
    }

    // Four neighboring delay samples produce cubic interpolation at a fractional read position.
    inline float ReadDelayInterpolated(float position) const {
        while (position < 0.0f) position += DELAY_BUFFER_SIZE;
        while (position >= DELAY_BUFFER_SIZE) position -= DELAY_BUFFER_SIZE;

        size_t idx = static_cast<size_t>(position);
        float frac = position - idx;

        size_t im1 = (idx + DELAY_BUFFER_SIZE - 1) % DELAY_BUFFER_SIZE;
        size_t i0 = idx;
        size_t i1 = (idx + 1) % DELAY_BUFFER_SIZE;
        size_t i2 = (idx + 2) % DELAY_BUFFER_SIZE;

        float ym1 = delayLine[im1];
        float y0 = delayLine[i0];
        float y1 = delayLine[i1];
        float y2 = delayLine[i2];

        float c0 = y0;
        float c1 = 0.5f * (y1 - ym1);
        float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);

        return c0 + c1 * frac + c2 * frac * frac + c3 * frac * frac * frac;
    }

    float ProcessSampleRatio(float input, float targetRatio) {

        delayLine[writePos] = input;
        writePos = (writePos + 1) % DELAY_BUFFER_SIZE;

        // Convert semitones to a frequency ratio and smooth the transition across calls.
        static const float smoothing = RescaleLegacySmoothing(0.995f);
        currentPitchRatio = smoothing * currentPitchRatio + (1.0f - smoothing) * targetRatio;

        float output;

        if (!isCrossfading) {

            output = ReadDelayInterpolated(readPos);

            readPos += currentPitchRatio;
            if (readPos >= DELAY_BUFFER_SIZE) readPos -= DELAY_BUFFER_SIZE;

            float distance = static_cast<float>(writePos) - readPos;
            if (distance < 0.0f) distance += DELAY_BUFFER_SIZE;

            // When read and write heads approach, capture the old segment and prepare a new read
            // position.
            if ((distance < CROSSFADE_SIZE + EDGE_MARGIN ||
                distance > DELAY_BUFFER_SIZE - CROSSFADE_SIZE - EDGE_MARGIN) &&
                cooldownFrames == 0) {

                isCrossfading = true;
                crossfadePhase = 0;

                // The saved segment supplies the fade-out portion while the new read head supplies
                // fade-in.
                for (size_t i = 0; i < CROSSFADE_SIZE; ++i) {
                    float pos = readPos + i * currentPitchRatio;
                    if (pos >= DELAY_BUFFER_SIZE) pos -= DELAY_BUFFER_SIZE;
                    nextSegment[i] = ReadDelayInterpolated(pos);
                }

                readPosPrev = readPos;
                readPosNext = static_cast<float>(writePos) - (DELAY_BUFFER_SIZE / 2.0f);
                if (readPosNext < 0.0f) readPosNext += DELAY_BUFFER_SIZE;

                cooldownFrames = static_cast<int>(PA_SAMPLE_RATE * 0.008f);
            }
        }
        else {

            // A cosine crossfade limits discontinuities during the delay-line read-head reset.
            if (crossfadePhase < CROSSFADE_SIZE) {

                float oldSample = nextSegment[crossfadePhase];
                float newSample = ReadDelayInterpolated(readPosNext);

                output = oldSample * fadeOut[crossfadePhase] +
                    newSample * fadeIn[crossfadePhase];

                readPosNext += currentPitchRatio;
                if (readPosNext >= DELAY_BUFFER_SIZE) readPosNext -= DELAY_BUFFER_SIZE;

                crossfadePhase++;
            }
            else {

                isCrossfading = false;
                readPos = readPosNext;

                output = ReadDelayInterpolated(readPos);
                readPos += currentPitchRatio;
                if (readPos >= DELAY_BUFFER_SIZE) readPos -= DELAY_BUFFER_SIZE;
            }
        }

        // Delay the next read-head reset to avoid repeated overlapping crossfades.
        if (cooldownFrames > 0) cooldownFrames--;

        return output;
    }

    float ProcessSample(float input, float pitchShiftSemitones) {
        return ProcessSampleRatio(input, pow(2.0f, pitchShiftSemitones / 12.0f));
    }

    void ProcessBuffer(float* buffer, size_t numSamples, float pitchShiftSemitones) {
        const float ratio = pow(2.0f, pitchShiftSemitones / 12.0f);
        for (size_t i = 0; i < numSamples; ++i) {
            buffer[i] = ProcessSampleRatio(buffer[i], ratio);
        }
    }
};


// Recorded preview and WAV export share the ordered effect chain but use separate processor
// histories. Only this selected-audio path processes at most 128 frames per DSP call; live
// microphone processing uses the 256-frame capacity declared in Common.h.
static constexpr size_t SELECTED_DSP_BLOCK_SIZE = 128;

struct SelectedAudioProcessor {
    RealtimePitchShifter pitch;
    StreamFormantShifter formant;
    TimbreShaper timbre;
    ProsodyContour prosody;
    PitchJitter jitter;
};

struct SelectedEffectSettings {
    uint8_t mask = 0;
    float pitch = 0.0f;
    float formant = 1.0f;
    float timbre = 0.0f;
    float prosody = 0.0f;
    float jitter = 0.0f;
    float volume = 1.0f;
};

static SelectedEffectSettings ReadSelectedSettings(uint8_t mask) noexcept {
    return { mask,
        g_pitchShiftSemitones.load(memory_order_relaxed),
        g_formantShiftRatio.load(memory_order_relaxed),
        g_timbrePercent.load(memory_order_relaxed),
        g_prosodyPercent.load(memory_order_relaxed),
        g_jitterAmount.load(memory_order_relaxed),
        g_outputVolume.load(memory_order_relaxed) };
}

static void ProcessSelectedBlock(const float* input, float* output, size_t count,
    const SelectedEffectSettings& settings, SelectedAudioProcessor& processor) {
    array<float, SELECTED_DSP_BLOCK_SIZE> pitched{};
    array<float, SELECTED_DSP_BLOCK_SIZE> shaped{};
    array<float, SELECTED_DSP_BLOCK_SIZE> accent{};
    const bool pitchEnabled = (settings.mask & EFFECT_PITCH) != 0;
    const bool prosodyEnabled = (settings.mask & EFFECT_PROSODY) &&
        settings.prosody != 0.0f;
    const bool jitterEnabled = (settings.mask & EFFECT_JITTER) &&
        settings.jitter > 0.0f;
    const float baseRatio = pow(2.0f, (pitchEnabled ? settings.pitch : 0.0f) / 12.0f);

    for (size_t i = 0; i < count; ++i) {
        const ProsodyStep step = processor.prosody.Advance(input[i],
            (settings.mask & EFFECT_PROSODY) ? settings.prosody : 0.0f);
        accent[i] = step.gain;
        // Jitter now perturbs the pitch ratio, rather than multiplying each
        // output sample by almost imperceptible random volume noise. Apply it
        // through the same existing pitch processor used by prosody.
        const float jitterRatio = processor.jitter.NextRatio(
            jitterEnabled ? settings.jitter : 0.0f);
        const float ratio = baseRatio * (prosodyEnabled && step.semitones != 0.0f
            ? pow(2.0f, step.semitones / 12.0f) : 1.0f) * jitterRatio;
        pitched[i] = (pitchEnabled || prosodyEnabled || jitterEnabled)
            ? processor.pitch.ProcessSampleRatio(input[i], ratio)
            : input[i];
    }

    if (settings.mask & EFFECT_FORMANT)
        processor.formant.ProcessFrame(pitched.data(), shaped.data(), count, settings.formant);
    else
        copy_n(pitched.data(), count, shaped.data());

    if (settings.mask & EFFECT_TIMBRE)
        processor.timbre.ProcessBuffer(shaped.data(), count, settings.timbre);

    for (size_t i = 0; i < count; ++i)
        output[i] = clamp(shaped[i] * accent[i] * settings.volume, -0.95f, 0.95f);
}

static PaStream* selectedPlaybackStream = nullptr;
static vector<Uint8> selectedPlaybackBuffer;
static size_t selectedPlaybackPosition = 0;
static size_t selectedPlaybackFramesEmitted = 0;
static size_t selectedPlaybackTailFrames = 0;
static uint8_t selectedPlaybackMask = 0;
static unique_ptr<SelectedAudioProcessor> selectedPlaybackProcessor;
static int selectedPlaybackOutputChannels = 1;
static atomic<unsigned> inlinePlaybackCallbacksInFlight{0};
static thread selectedSaveThread;
static atomic<bool> selectedSaveBusy{ false };
static atomic<int> selectedSaveResult{ 0 }; // 0 pending/idle, 1 success, 2 failure.
static atomic<uint64_t> selectedSaveCounter{ 0 };

static int SelectedPlaybackCallback(const void*, void* outputBuffer, unsigned long frameCount,
    const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*) {
    auto* output = static_cast<float*>(outputBuffer);
    if (output == nullptr) return paContinue;
    if (!paIsPlayingSelected.load(memory_order_acquire) || !selectedPlaybackProcessor) {
        fill_n(output, frameCount * selectedPlaybackOutputChannels, 0.0f);
        return paContinue;
    }

    // Selector changes are heard on the next callback; the playback slider values are live too.
    const auto settings = ReadSelectedSettings(g_selectedEffects.load(memory_order_acquire));
    if (settings.mask & EFFECT_FORMANT)
        selectedPlaybackTailFrames = StreamFormantShifter::MAX_DELAY;
    const size_t total = selectedPlaybackBuffer.size() / 2;
    // The live delay line needs input silence after the take to play its remaining samples.
    const size_t outputTotal = total + selectedPlaybackTailFrames;
    array<float, SELECTED_DSP_BLOCK_SIZE> input{};
    array<float, SELECTED_DSP_BLOCK_SIZE> processed{};
    for (size_t offset = 0; offset < frameCount; offset += input.size()) {
        const size_t count = min(input.size(), static_cast<size_t>(frameCount) - offset);
        const size_t valid = min(count, total - selectedPlaybackPosition);
        for (size_t i = 0; i < count; ++i)
            input[i] = i < valid
                ? ReadPcm16Sample(selectedPlaybackBuffer, selectedPlaybackPosition++) / 32768.0f
                : 0.0f;
        ProcessSelectedBlock(input.data(), processed.data(), count, settings,
            *selectedPlaybackProcessor);
        for (size_t i = 0; i < count; ++i) {
            const float sample = selectedPlaybackFramesEmitted < outputTotal
                ? processed[i] : 0.0f;
            for (int channel = 0; channel < selectedPlaybackOutputChannels; ++channel)
                output[(offset + i) * selectedPlaybackOutputChannels + channel] = sample;
            ++selectedPlaybackFramesEmitted;
        }
    }
    if (selectedPlaybackFramesEmitted >= outputTotal)
        paIsPlayingSelected.store(false, memory_order_release);
    // Let PortAudio stop scheduling this callback once the final padded block is submitted.
    // The device worker closes the completed stream on the next application update.
    return selectedPlaybackFramesEmitted >= outputTotal ? paComplete : paContinue;
}

void RenderRecordedPlaybackInDuplex(void* output, unsigned long frames) noexcept {
    // Stop disables the playback flag before waiting for callbacks already using its buffer.
    // New callbacks can still enter, but then observe false and cannot touch that buffer.
    inlinePlaybackCallbacksInFlight.fetch_add(1, memory_order_acq_rel);
    if (paIsPlayingSelected.load(memory_order_acquire))
        (void)SelectedPlaybackCallback(nullptr, output, frames, nullptr, 0, nullptr);
    else if (output != nullptr)
        fill_n(static_cast<float*>(output), frames * paOutputChannels, 0.0f);
    inlinePlaybackCallbacksInFlight.fetch_sub(1, memory_order_acq_rel);
}

// Abort a playback stream to release its previous output device immediately. The caller
// changes callback-visible buffers or read positions only after Pa_CloseStream joins it.
static void CloseRecordedPlayback(PaStream*& stream) {
    if (stream == nullptr) return;
    Pa_AbortStream(stream);
    Pa_CloseStream(stream);
    stream = nullptr;
}

// Join the previous callback before starting a new preview of the captured take.
static void StopAllRecordedPlaybacks() {
    StopPortAudioPlaybackSelected();
}

bool StartPortAudioPlaybackSelected() {
    if (!IsPortAudioDeviceWorkerThread())
        return RunPortAudioDeviceAction(StartPortAudioPlaybackSelected);
    if (paIsRecording.load(memory_order_acquire) || recordingCollector.joinable()) {
        return false;
    }
    if (recordingIncomplete.load(memory_order_acquire)) {
        return false;
    }
    if (paIsPlayingSelected.load(memory_order_acquire)) return false;
    StopAllRecordedPlaybacks();
    {
        lock_guard<mutex> lock(paAudioBufferMutex);
        selectedPlaybackBuffer = paAudioBuffer;
    }
    if (selectedPlaybackBuffer.empty()) {
        return false;
    }

    // The live duplex stream prefers WASAPI. Apply the same preference to recorded playback,
    // whose old Pa_GetDefaultOutputDevice() choice could silently use MME instead. Fall back to
    // that original device if WASAPI cannot open; both cases retain the 48 kHz format check.
    PaDeviceIndex wasapiOutput = paNoDevice;
#ifdef _WIN32
    const PaHostApiIndex wasapiIndex = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
    if (wasapiIndex >= 0) {
        if (const PaHostApiInfo* host = Pa_GetHostApiInfo(wasapiIndex))
            wasapiOutput = host->defaultOutputDevice;
    }
#endif
    const PaDeviceIndex defaultOutput = Pa_GetDefaultOutputDevice();
    const array<PaDeviceIndex, 2> candidates{ wasapiOutput, defaultOutput };
    selectedPlaybackMask = g_selectedEffects.load(memory_order_acquire);
    selectedPlaybackProcessor = make_unique<SelectedAudioProcessor>();
    selectedPlaybackPosition = 0;
    selectedPlaybackFramesEmitted = 0;
    selectedPlaybackTailFrames = 0;
    if (g_activeWasapiMode.load(memory_order_acquire) == 1) {
        // The duplex stream already owns this output exclusively. Preview through that stream
        // so Play still works without attempting to open a competing output stream.
        if (paStream == nullptr || Pa_IsStreamActive(paStream) != 1) {
            selectedPlaybackProcessor.reset();
            return false;
        }
        selectedPlaybackOutputChannels = paOutputChannels;
        paIsPlayingSelected.store(true, memory_order_release);
        return true;
    }
    const PaDeviceInfo* openedOutput = nullptr;
    const PaStreamInfo* playbackInfo = nullptr;
    for (size_t attempt = 0; attempt < candidates.size(); ++attempt) {
        const PaDeviceIndex candidate = candidates[attempt];
        if (candidate == paNoDevice || (attempt == 1 && candidate == wasapiOutput)) continue;
        const PaDeviceInfo* info = Pa_GetDeviceInfo(candidate);
        if (info == nullptr) continue;
        PaStreamParameters output{};
        output.device = candidate;
        output.channelCount = paMicrophoneChannels;
        selectedPlaybackOutputChannels = output.channelCount;
        output.sampleFormat = paFloat32;
        output.suggestedLatency = info->defaultLowOutputLatency;
        const PaError format = Pa_IsFormatSupported(nullptr, &output, PA_SAMPLE_RATE);
        if (format != paFormatIsSupported) {
            continue;
        }
        const PaError open = Pa_OpenStream(&selectedPlaybackStream, nullptr, &output,
            PA_SAMPLE_RATE, paFramesPerBufferUnspecified, paClipOff,
            SelectedPlaybackCallback, nullptr);
        if (open != paNoError) {
            selectedPlaybackStream = nullptr;
            continue;
        }
        playbackInfo = Pa_GetStreamInfo(selectedPlaybackStream);
        if (playbackInfo != nullptr && std::abs(playbackInfo->sampleRate - PA_SAMPLE_RATE) <= 0.5) {
            paIsPlayingSelected.store(true, memory_order_release);
            const PaError started = Pa_StartStream(selectedPlaybackStream);
            if (started == paNoError) {
                openedOutput = info;
                break;
            }
            paIsPlayingSelected.store(false, memory_order_release);
        }
        CloseRecordedPlayback(selectedPlaybackStream);
    }
    if (openedOutput == nullptr) {
        selectedPlaybackProcessor.reset();
        return false;
    }
    return true;
}

void StopPortAudioPlaybackSelected() {
    if (!IsPortAudioDeviceWorkerThread()) {
        RunPortAudioDeviceAction(StopPortAudioPlaybackSelected);
        return;
    }
    paIsPlayingSelected.store(false, memory_order_release);
    CloseRecordedPlayback(selectedPlaybackStream);
    // Pa_CloseStream does not join callbacks when preview shares the Exclusive duplex stream.
    while (inlinePlaybackCallbacksInFlight.load(memory_order_acquire) != 0)
        this_thread::yield();
    selectedPlaybackProcessor.reset();
    selectedPlaybackPosition = 0;
    selectedPlaybackFramesEmitted = 0;
    selectedPlaybackTailFrames = 0;
}

// Process and write a private snapshot on a background worker so the SDL frame loop
// continues repainting while long recordings are exported.
static bool ExportSelectedSnapshot(vector<Uint8> snapshot,
    const SelectedEffectSettings& settings, uint64_t saveNumber) {
    SelectedAudioProcessor processor;
    array<float, SELECTED_DSP_BLOCK_SIZE> input{};
    array<float, SELECTED_DSP_BLOCK_SIZE> output{};
    const size_t total = snapshot.size() / 2;
    // At a neutral ratio the live read head is half a delay line behind input. Drain the
    // processor and discard that initial silence so the saved WAV retains the take duration.
    const size_t delay = (settings.mask & EFFECT_FORMANT)
        ? StreamFormantShifter::NOMINAL_DELAY : 0;
    for (size_t start = 0; start < total + delay; start += input.size()) {
        const size_t count = min(input.size(), total + delay - start);
        for (size_t i = 0; i < count; ++i)
            input[i] = start + i < total
                ? ReadPcm16Sample(snapshot, start + i) / 32768.0f : 0.0f;
        ProcessSelectedBlock(input.data(), output.data(), count, settings, processor);
        for (size_t i = 0; i < count; ++i) {
            if (start + i >= delay)
                WritePcm16Sample(snapshot, start + i - delay, output[i]);
        }
    }
    const string filename = GenerateWAVFilename(
        "audio_selected_" + to_string(static_cast<unsigned>(settings.mask)) + "_" +
        to_string(saveNumber) + "_mono");
    const bool result = SaveWAV_Adaptive(filename, snapshot, 1, PA_SAMPLE_RATE);
    return result;
}

bool SavePortAudioSelectedRecording() {
    if (selectedSaveBusy.load(memory_order_acquire)) {
        return false;
    }
    vector<Uint8> snapshot;
    {
        lock_guard<mutex> lock(paAudioBufferMutex);
        if (paIsRecording.load(memory_order_acquire) || recordingCollector.joinable()) {
            return false;
        }
        if (recordingIncomplete.load(memory_order_acquire)) {
            return false;
        }
        snapshot = paAudioBuffer;
    }
    if (snapshot.empty()) {
        return false;
    }
    if (selectedSaveThread.joinable()) selectedSaveThread.join();
    const auto settings = ReadSelectedSettings(g_selectedEffects.load(memory_order_acquire));
    const uint64_t saveNumber = selectedSaveCounter.fetch_add(1, memory_order_relaxed);
    selectedSaveBusy.store(true, memory_order_release);
    selectedSaveResult.store(0, memory_order_release);
    try {
        selectedSaveThread = thread([snapshot = move(snapshot), settings, saveNumber]() mutable {
            bool saved = false;
            try { saved = ExportSelectedSnapshot(move(snapshot), settings, saveNumber); }
            catch (...) { /* The completion result reports failure to the UI. */ }
            selectedSaveBusy.store(false, memory_order_release);
            selectedSaveResult.store(saved ? 1 : 2, memory_order_release);
        });
    } catch (...) {
        selectedSaveBusy.store(false, memory_order_release);
        return false;
    }
    return true;
}

int TakeSelectedSaveResult() noexcept {
    return selectedSaveResult.exchange(0, memory_order_acq_rel);
}

void JoinSelectedSaveWorker() {
    if (selectedSaveThread.joinable()) selectedSaveThread.join();
}

// Reset the capture buffer and enable recording on the existing unified stream.
bool StartPortAudioRecording()
{
    if (!IsPortAudioDeviceWorkerThread())
        return RunPortAudioDeviceAction(StartPortAudioRecording);
    if (paStream == nullptr) return false;

    if (paIsRecording.load()) {
        return false;
    }

    // Capture and recorded preview cannot run concurrently from the same controls.
    StopPortAudioPlaybackSelected();

    // No previous producer or consumer can still touch the queue at this point. All vector
    // allocation, including this reserve, happens outside the PortAudio callback.
    if (recordingCollector.joinable()) recordingCollector.join();
    recordingQueue.Reset();
    recordingCollectorFailed.store(false, memory_order_release);
    recordingIncomplete.store(false, memory_order_release);
    try {
        lock_guard<mutex> recordingLock(paAudioBufferMutex);
        paAudioBuffer.clear();
        if (paAudioBuffer.capacity() < static_cast<size_t>(PA_SAMPLE_RATE) * 2 * 60)
            paAudioBuffer.reserve(static_cast<size_t>(PA_SAMPLE_RATE) * 2 * 60);
        paIsRecording.store(true);
        recordingCollector = thread(CollectPortAudioRecording);
    } catch (...) {
        paIsRecording.store(false);
        while (g_duplexCallbacksInFlight.load() != 0) this_thread::yield();
        return false;
    }

    return true;
}

// Disable capture while retaining the PCM16 buffer for playback and saving.
void StopPortAudioRecording()
{
    if (!paIsRecording.load() && !recordingCollector.joinable()) return;

    paIsRecording.store(false);
    // A callback registers before reading the capture flag. Waiting for complete callbacks
    // prevents an older block from arriving after Stop drains or Start resets the queue.
    while (g_duplexCallbacksInFlight.load() != 0) this_thread::yield();
    recordingWake.notify_one();
    if (recordingCollector.joinable()) recordingCollector.join();
    const uint64_t dropped = recordingQueue.DroppedFrames();
    const bool incomplete = dropped != 0 || recordingCollectorFailed.load(memory_order_acquire);
    recordingIncomplete.store(incomplete, memory_order_release);
    lock_guard<mutex> recordingLock(paAudioBufferMutex);
}

// Select PortAudio devices and open the unified duplex stream; stream initialization is separate
// from UI setup.
bool InitPortAudio()
{
    g_activeWasapiMode.store(-2, memory_order_release);
    // PortAudio must be initialized before querying devices or opening any stream.
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        return false;
    }

    const double sampleRate = PA_SAMPLE_RATE;
    paMicrophoneChannels = 1;
    paOutputChannels = 1;

    // Keep PortAudio defaults as a fallback if WASAPI cannot open a working 48 kHz pair.
    const PaDeviceIndex defaultInput = Pa_GetDefaultInputDevice();
    const PaDeviceIndex defaultOutput = Pa_GetDefaultOutputDevice();

    // Request shorter shared-mode buffering first when a WASAPI endpoint advertises a
    // default low latency above 5 ms. PortAudio may round the request up to a period
    // supported by the driver; the requested value must never be reported as achieved.
    // A failed low-latency attempt retries the device's known working default-low profile.
    constexpr double desiredWasapiLatencySeconds = 0.005;

    // Try one matching input/output device pair at 48 kHz. The recording buffer and live
    // output contains one or two channels per frame; a stereo endpoint receives duplicated mono.
    // Keep the stream only when opening and starting both succeed.
    const auto tryDuplex = [&](PaDeviceIndex microphone, PaDeviceIndex speaker,
        bool isWasapi, bool exclusive, int inputChannels, int outputChannels) -> bool {
        const PaDeviceInfo* inputInfo = Pa_GetDeviceInfo(microphone);
        const PaDeviceInfo* outputInfo = Pa_GetDeviceInfo(speaker);
        if (inputInfo == nullptr || outputInfo == nullptr) {
            return false;
        }
        if (inputChannels > inputInfo->maxInputChannels ||
            outputChannels > outputInfo->maxOutputChannels) return false;

        PaStreamParameters inputParams{};
        inputParams.device = microphone;
        inputParams.channelCount = inputChannels;
        inputParams.sampleFormat = paFloat32;
        inputParams.suggestedLatency = inputInfo->defaultLowInputLatency;

        PaStreamParameters outputParams{};
        outputParams.device = speaker;
        outputParams.channelCount = outputChannels;
        outputParams.sampleFormat = paFloat32;
        outputParams.suggestedLatency = outputInfo->defaultLowOutputLatency;

#ifdef _WIN32
        // The stream descriptors only need to live until Pa_OpenStream returns. Both directions
        // request the same share mode; a failed Exclusive open is retried as Shared below.
        auto inputWasapi = MakeExclusiveWasapiStreamInfo();
        auto outputWasapi = MakeExclusiveWasapiStreamInfo(outputChannels == 2);
        if (exclusive) {
            inputParams.hostApiSpecificStreamInfo = &inputWasapi;
            outputParams.hostApiSpecificStreamInfo = &outputWasapi;
        }
#else
        if (exclusive) return false;
#endif


        // A 48 kHz device stream is required; never retry at another processing rate.
        PaError attempt = Pa_IsFormatSupported(&inputParams, &outputParams, sampleRate);
        if (attempt != paFormatIsSupported) {
            return false;
        }

        const bool shortProfile = isWasapi && !exclusive &&
            (inputInfo->defaultLowInputLatency > desiredWasapiLatencySeconds ||
             outputInfo->defaultLowOutputLatency > desiredWasapiLatencySeconds);
        for (int profile = 0; profile < (shortProfile ? 2 : 1); ++profile) {
            const bool requestShort = shortProfile && profile == 0;
            inputParams.suggestedLatency = requestShort
                ? min(inputInfo->defaultLowInputLatency, desiredWasapiLatencySeconds)
                : inputInfo->defaultLowInputLatency;
            outputParams.suggestedLatency = requestShort
                ? min(outputInfo->defaultLowOutputLatency, desiredWasapiLatencySeconds)
                : outputInfo->defaultLowOutputLatency;

            // An unspecified host block avoids forcing PortAudio to add a fixed-size
            // conversion buffer. The existing callback splits large host blocks itself.
            attempt = Pa_OpenStream(&paStream, &inputParams, &outputParams, sampleRate,
                paFramesPerBufferUnspecified, paClipOff, paUnifiedCallback, nullptr);
            if (attempt != paNoError) {
                paStream = nullptr;
                continue;
            }

            // Pa_StartStream can invoke the callback immediately, so publish its channel
            // stride first. Failed attempts are reset before another stream is started.
            paOutputChannels = outputChannels;
            paMicrophoneChannels = inputChannels;
            // The first callback can run before Pa_StartStream returns. Publish Exclusive
            // before it starts so capture uses the matching gate policy from sample one.
            g_activeWasapiMode.store(isWasapi ? (exclusive ? 1 : 0) : -1,
                memory_order_release);
            ResetCaptureProcessingForNewDevice();
            attempt = Pa_StartStream(paStream);
            if (attempt != paNoError) {
                Pa_CloseStream(paStream);
                paStream = nullptr;
                g_activeWasapiMode.store(-2, memory_order_release);
                continue;
            }
            // Reject an unexpected rate here so the normal WASAPI profile can still
            // be attempted before falling back to PortAudio's default devices.
            const PaStreamInfo* openedInfo = Pa_GetStreamInfo(paStream);
            if (openedInfo == nullptr ||
                std::abs(openedInfo->sampleRate - sampleRate) > 0.5) {
                Pa_StopStream(paStream);
                Pa_CloseStream(paStream);
                paStream = nullptr;
                g_activeWasapiMode.store(-2, memory_order_release);
                continue;
            }
            g_activeWasapiMode.store(isWasapi ? (exclusive ? 1 : 0) : -1,
                memory_order_release);
            return true;
        }
        return false;
    };

    bool opened = false;
#ifdef _WIN32
    {
        // Host API defaults are global PaDeviceIndex values, unlike indexes used while
        // enumerating the devices within a host API. Shared mode leaves other apps usable.
        const PaHostApiIndex wasapiIndex = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
        if (wasapiIndex >= 0) {
            const PaHostApiInfo* wasapi = Pa_GetHostApiInfo(wasapiIndex);
            if (wasapi != nullptr && wasapi->defaultInputDevice != paNoDevice &&
                wasapi->defaultOutputDevice != paNoDevice) {
                const PaDeviceIndex microphone = wasapi->defaultInputDevice;
                const PaDeviceIndex speaker = wasapi->defaultOutputDevice;
                if (g_requestedWasapiMode.load(memory_order_acquire) == 1) {
                    // A mono Exclusive stream can be mapped to a center channel on a
                    // stereo endpoint, making the voice quiet even though it opened.
                    // Use stereo front speakers on stereo-capable devices. Only request
                    // mono from an endpoint that advertises no second output channel.
                    const PaDeviceInfo* speakerInfo = Pa_GetDeviceInfo(speaker);
                    const int exclusiveChannels = speakerInfo != nullptr &&
                        speakerInfo->maxOutputChannels >= 2 ? 2 : 1;
                    const PaDeviceInfo* microphoneInfo = Pa_GetDeviceInfo(microphone);
                    const int exclusiveInputChannels = microphoneInfo != nullptr &&
                        microphoneInfo->maxInputChannels >= 2 ? 2 : 1;
                    opened = tryDuplex(microphone, speaker, true, true,
                        exclusiveInputChannels, exclusiveChannels);
                    if (!opened && exclusiveInputChannels == 2)
                        opened = tryDuplex(microphone, speaker, true, true,
                            1, exclusiveChannels);
                    if (!opened) {
                        g_requestedWasapiMode.store(0, memory_order_release);
                    }
                }
                if (!opened)
                    opened = tryDuplex(microphone, speaker, true, false, 1, 1);
            }
        }
    }
#endif
    if (!opened && defaultInput != paNoDevice && defaultOutput != paNoDevice) {
        opened = tryDuplex(defaultInput, defaultOutput, false, false, 1, 1);
    }
    if (!opened) {
        Pa_Terminate();
        return false;
    }


    // Reject a device that does not confirm the application's fixed 48 kHz rate.
    if (const PaStreamInfo* streamInfo = Pa_GetStreamInfo(paStream)) {
        if (std::abs(streamInfo->sampleRate - sampleRate) > 0.5) {
            Pa_StopStream(paStream);
            Pa_CloseStream(paStream);
            paStream = nullptr;
            Pa_Terminate();
            return false;
        }
    }
    return true;
}

// Close playback and duplex streams before terminating PortAudio.
void CleanupPortAudio()
{
    // PortAudio device operations remain on the dedicated worker thread.
    StopPortAudioRecording();
    StopPortAudioPlaybackSelected();

    CloseRecordedPlayback(paStream);

    g_activeWasapiMode.store(-2, memory_order_release);
    Pa_Terminate();
}
