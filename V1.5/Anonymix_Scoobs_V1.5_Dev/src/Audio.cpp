// PortAudio capture and playback, with PCM16 processing for saved or played recordings. Callbacks may run on a separate thread.

#include "Audio.h"
#include "PitchJitter.h"
#include "AudioDiagnostics.h"
#include "Dsp.h"
#include "Stream.h"
#include "Timbre.h"
#include "Prosody.h"
#include "Formant.h"
#include "Wav.h"
#include <memory>
#include <thread>

// The live callback records here; the SDL thread reads the same counters for its report.
AudioDiagnostics g_audioDiagnostics;

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

                float t = static_cast<float>(crossfadePhase) / (CROSSFADE_SIZE - 1);
                float fadeOut = 0.5f * (1.0f + cosf(3.14159265f * t));
                float fadeIn = 0.5f * (1.0f - cosf(3.14159265f * t));

                float oldSample = nextSegment[crossfadePhase];
                float newSample = ReadDelayInterpolated(readPosNext);

                output = oldSample * fadeOut + newSample * fadeIn;

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


// The preview and WAV export share this ordered DSP chain but never share processor
// histories. Every PortAudio callback works in 128-frame pieces without allocation.
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
        const float ratio = baseRatio * (prosodyEnabled
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
static thread selectedSaveThread;
static atomic<bool> selectedSaveBusy{ false };
static atomic<int> selectedSaveResult{ 0 }; // 0 pending/idle, 1 success, 2 failure.
static atomic<uint64_t> selectedSaveCounter{ 0 };

static int SelectedPlaybackCallback(const void*, void* outputBuffer, unsigned long frameCount,
    const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*) {
    auto* output = static_cast<float*>(outputBuffer);
    if (output == nullptr) return paContinue;
    if (!paIsPlayingSelected.load(memory_order_acquire) || !selectedPlaybackProcessor) {
        fill_n(output, frameCount * paMicrophoneChannels, 0.0f);
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
            for (int channel = 0; channel < paMicrophoneChannels; ++channel)
                output[(offset + i) * paMicrophoneChannels + channel] = sample;
            ++selectedPlaybackFramesEmitted;
        }
    }
    if (selectedPlaybackFramesEmitted >= outputTotal)
        paIsPlayingSelected.store(false, memory_order_release);
    return paContinue;
}

// Abort a playback stream to release its previous output device immediately. The caller
// changes callback-visible buffers or read positions only after Pa_CloseStream joins it.
static void CloseRecordedPlayback(PaStream*& stream, const char* name) {
    if (stream == nullptr) return;
    const PaError abortResult = Pa_AbortStream(stream);
    if (abortResult != paNoError && abortResult != paStreamIsStopped)
        SDL_Log("%s abort: %s", name, Pa_GetErrorText(abortResult));
    const PaError closeResult = Pa_CloseStream(stream);
    if (closeResult != paNoError)
        SDL_Log("%s close: %s", name, Pa_GetErrorText(closeResult));
    stream = nullptr;
}

// Join the previous callback before starting a new preview of the captured take.
static void StopAllRecordedPlaybacks() {
    StopPortAudioPlaybackSelected();
}

bool StartPortAudioPlaybackSelected() {
    if (!IsPortAudioDeviceWorkerThread())
        return RunPortAudioDeviceAction(StartPortAudioPlaybackSelected);
    if (paIsRecording.load(memory_order_acquire)) {
        SDL_Log("Stop recording before previewing the selected effects");
        return false;
    }
    if (paIsPlayingSelected.load(memory_order_acquire)) return false;
    StopAllRecordedPlaybacks();
    {
        lock_guard<mutex> lock(paAudioBufferMutex);
        selectedPlaybackBuffer = paAudioBuffer;
    }
    if (selectedPlaybackBuffer.empty()) {
        SDL_Log("Record a take before pressing Play");
        return false;
    }

    const auto device = Pa_GetDefaultOutputDevice();
    const auto* info = device != paNoDevice ? Pa_GetDeviceInfo(device) : nullptr;
    if (info == nullptr) {
        SDL_Log("No output device is available for playback");
        return false;
    }
    PaStreamParameters output{};
    output.device = device;
    output.channelCount = paMicrophoneChannels;
    output.sampleFormat = paFloat32;
    output.suggestedLatency = info->defaultLowOutputLatency;
    selectedPlaybackMask = g_selectedEffects.load(memory_order_acquire);
    selectedPlaybackProcessor = make_unique<SelectedAudioProcessor>();
    selectedPlaybackPosition = 0;
    selectedPlaybackFramesEmitted = 0;
    selectedPlaybackTailFrames = 0;
    PaError err = Pa_OpenStream(&selectedPlaybackStream, nullptr, &output, PA_SAMPLE_RATE,
        paFramesPerBufferUnspecified, paClipOff, SelectedPlaybackCallback, nullptr);
    if (err != paNoError) {
        SDL_Log("Cannot open selected playback: %s", Pa_GetErrorText(err));
        selectedPlaybackStream = nullptr;
        selectedPlaybackProcessor.reset();
        return false;
    }
    paIsPlayingSelected.store(true, memory_order_release);
    err = Pa_StartStream(selectedPlaybackStream);
    if (err != paNoError) {
        paIsPlayingSelected.store(false, memory_order_release);
        SDL_Log("Cannot start selected playback: %s", Pa_GetErrorText(err));
        CloseRecordedPlayback(selectedPlaybackStream, "Selected playback");
        selectedPlaybackProcessor.reset();
        return false;
    }
    SDL_Log("Selected playback started: effects mask %u, %zu mono frames; formant %s (ratio %.3f)",
        static_cast<unsigned>(selectedPlaybackMask), selectedPlaybackBuffer.size() / 2,
        (selectedPlaybackMask & EFFECT_FORMANT) ? "ON" : "OFF",
        g_formantShiftRatio.load(memory_order_relaxed));
    return true;
}

void StopPortAudioPlaybackSelected() {
    if (!IsPortAudioDeviceWorkerThread()) {
        RunPortAudioDeviceAction(StopPortAudioPlaybackSelected);
        return;
    }
    paIsPlayingSelected.store(false, memory_order_release);
    CloseRecordedPlayback(selectedPlaybackStream, "Selected playback");
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
    SDL_Log("Selected WAV %s: %s (effects mask %u)",
        result ? "saved" : "failed", filename.c_str(), static_cast<unsigned>(settings.mask));
    return result;
}

bool SavePortAudioSelectedRecording() {
    if (selectedSaveBusy.load(memory_order_acquire)) {
        SDL_Log("A WAV export is already running");
        return false;
    }
    vector<Uint8> snapshot;
    {
        lock_guard<mutex> lock(paAudioBufferMutex);
        if (paIsRecording.load(memory_order_acquire)) {
            SDL_Log("Stop recording before saving a WAV file");
            return false;
        }
        snapshot = paAudioBuffer;
    }
    if (snapshot.empty()) {
        SDL_Log("Record a take before pressing Save");
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
            catch (...) { SDL_Log("Selected WAV export failed with an exception"); }
            selectedSaveBusy.store(false, memory_order_release);
            selectedSaveResult.store(saved ? 1 : 2, memory_order_release);
        });
    } catch (...) {
        selectedSaveBusy.store(false, memory_order_release);
        SDL_Log("Could not launch the WAV export worker");
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
    SDL_Log("=== StartPortAudioRecording called ===");

    if (paIsRecording) {
        SDL_Log("PortAudio recording already active");
        return false;
    }

    // Capture and recorded preview cannot run concurrently from the same controls.
    StopPortAudioPlaybackSelected();

    // Preallocate the first minute outside the callback so normal capture does not
    // repeatedly grow its PCM vector on the audio thread.
    {
        lock_guard<mutex> recordingLock(paAudioBufferMutex);
        paAudioBuffer.clear();
        if (paAudioBuffer.capacity() < static_cast<size_t>(PA_SAMPLE_RATE) * 2 * 60)
            paAudioBuffer.reserve(static_cast<size_t>(PA_SAMPLE_RATE) * 2 * 60);
        SDL_Log("PortAudio buffer cleared (new capacity: %zu)", paAudioBuffer.capacity());
        paIsRecording.store(true, memory_order_release);
    }

    SDL_Log(">>> PortAudio recording STARTED <<<");
    return true;
}

// Disable capture while retaining the PCM16 buffer for playback and saving.
void StopPortAudioRecording()
{
    if (!paIsRecording) return;

    paIsRecording.store(false, memory_order_release);
    // Wait for a block that had already entered the capture section to complete.
    lock_guard<mutex> recordingLock(paAudioBufferMutex);
    SDL_Log("PortAudio recording stopped - %zu bytes captured", paAudioBuffer.size());
}

// Select PortAudio devices and open the unified duplex stream; stream initialization is separate
// from UI setup.
bool InitPortAudio()
{
    // PortAudio must be initialized before querying devices or opening any stream.
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        SDL_Log("Error Pa_Initialize: %s", Pa_GetErrorText(err));
        return false;
    }

    const double sampleRate = PA_SAMPLE_RATE;
    paMicrophoneChannels = 1;

    // Keep the known working PortAudio defaults as a fallback. On Windows these can select
    // MME, whose reported output buffering was 91 ms on the tested machine. Set this to false
    // for an A/B test using the same default-device behavior as the first V1.2 prototype.
    constexpr bool preferWasapiForLiveAudio = true;
    const PaDeviceIndex defaultInput = Pa_GetDefaultInputDevice();
    const PaDeviceIndex defaultOutput = Pa_GetDefaultOutputDevice();
    if (defaultInput == paNoDevice || defaultOutput == paNoDevice) {
        SDL_Log("A default microphone or output device is missing");
        Pa_Terminate();
        return false;
    }

    const PaDeviceInfo* deviceInfo = nullptr;
    const PaDeviceInfo* outputDeviceInfo = nullptr;

    // Try one matching input/output device pair at 48 kHz. The recording buffer and live
    // output both contain one sample per frame, so both PortAudio channels remain mono.
    // Only retain the device pointers and stream when opening and starting both succeed.
    const auto tryDuplex = [&](PaDeviceIndex microphone, PaDeviceIndex speaker,
        const char* selection) -> bool {
        const PaDeviceInfo* inputInfo = Pa_GetDeviceInfo(microphone);
        const PaDeviceInfo* outputInfo = Pa_GetDeviceInfo(speaker);
        if (inputInfo == nullptr || outputInfo == nullptr) {
            SDL_Log("%s: cannot query device information", selection);
            return false;
        }

        PaStreamParameters inputParams{};
        inputParams.device = microphone;
        inputParams.channelCount = 1;
        inputParams.sampleFormat = paFloat32;
        inputParams.suggestedLatency = inputInfo->defaultLowInputLatency;

        PaStreamParameters outputParams{};
        outputParams.device = speaker;
        outputParams.channelCount = 1;
        outputParams.sampleFormat = paFloat32;
        outputParams.suggestedLatency = outputInfo->defaultLowOutputLatency;

        SDL_Log("Trying %s: %s -> %s at %.0f Hz (mono)",
            selection, inputInfo->name, outputInfo->name, sampleRate);

        // A 48 kHz device stream is required; never retry at another processing rate.
        PaError attempt = Pa_IsFormatSupported(&inputParams, &outputParams, sampleRate);
        if (attempt != paFormatIsSupported) {
            SDL_Log("%s does not support this duplex format: %s",
                selection, Pa_GetErrorText(attempt));
            return false;
        }

        // Let the host choose its callback size; paUnifiedCallback divides larger blocks into
        // 256-frame DSP chunks without requesting an extra host conversion buffer.
        attempt = Pa_OpenStream(&paStream, &inputParams, &outputParams, sampleRate,
            paFramesPerBufferUnspecified, paClipOff, paUnifiedCallback, nullptr);
        if (attempt != paNoError) {
            SDL_Log("%s cannot open the duplex stream: %s", selection, Pa_GetErrorText(attempt));
            paStream = nullptr;
            return false;
        }

        attempt = Pa_StartStream(paStream);
        if (attempt != paNoError) {
            SDL_Log("%s cannot start the duplex stream: %s", selection, Pa_GetErrorText(attempt));
            Pa_CloseStream(paStream);
            paStream = nullptr;
            return false;
        }
        deviceInfo = inputInfo;
        outputDeviceInfo = outputInfo;
        SDL_Log("Selected live audio devices: %s", selection);
        return true;
    };

    bool opened = false;
#ifdef _WIN32
    if (preferWasapiForLiveAudio) {
        // Host API defaults are global PaDeviceIndex values, unlike indexes used while
        // enumerating the devices within a host API. Shared mode leaves other apps usable.
        const PaHostApiIndex wasapiIndex = Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
        if (wasapiIndex >= 0) {
            const PaHostApiInfo* wasapi = Pa_GetHostApiInfo(wasapiIndex);
            if (wasapi != nullptr && wasapi->defaultInputDevice != paNoDevice &&
                wasapi->defaultOutputDevice != paNoDevice) {
                opened = tryDuplex(wasapi->defaultInputDevice,
                    wasapi->defaultOutputDevice, "WASAPI shared");
            } else {
                SDL_Log("WASAPI has no default microphone/output pair");
            }
        } else {
            SDL_Log("WASAPI is unavailable in this PortAudio build");
        }
    }
#endif
    if (!opened) {
        SDL_Log("Trying the original PortAudio default-device configuration");
        opened = tryDuplex(defaultInput, defaultOutput, "PortAudio defaults");
    }
    if (!opened) {
        SDL_Log("Cannot start a 48 kHz mono duplex stream with available devices");
        Pa_Terminate();
        return false;
    }

    SDL_Log("=== DEVICE INFO ===");
    SDL_Log("Name: %s", deviceInfo->name);
    SDL_Log("Maximum input channels: %d", deviceInfo->maxInputChannels);
    SDL_Log("Default sample rate: %.0f Hz", deviceInfo->defaultSampleRate);

    SDL_Log(">>> PortAudio DUPLEX initialized successfully <<<");
    // These are device/host API estimates returned after the stream has started. They describe
    // input/output buffering, not a measured acoustic microphone-to-speaker round trip.
    if (const PaStreamInfo* streamInfo = Pa_GetStreamInfo(paStream)) {
        SDL_Log("  Reported device input latency: %.2f ms", streamInfo->inputLatency * 1000.0);
        SDL_Log("  Reported device output latency: %.2f ms", streamInfo->outputLatency * 1000.0);
        SDL_Log("  Actual stream sample rate: %.0f Hz", streamInfo->sampleRate);
        if (std::abs(streamInfo->sampleRate - sampleRate) > 0.5) {
            SDL_Log("Unexpected sample rate; stopping the duplex stream");
            Pa_StopStream(paStream);
            Pa_CloseStream(paStream);
            paStream = nullptr;
            Pa_Terminate();
            return false;
        }
    }
    if (const PaHostApiInfo* hostApi = Pa_GetHostApiInfo(deviceInfo->hostApi)) {
        SDL_Log("  Audio host API: %s", hostApi->name);
    }
    SDL_Log("  Input device: %s", deviceInfo->name);
    SDL_Log("  Output device: %s", outputDeviceInfo->name);
    SDL_Log("  Format: MONO (%d channel)", paMicrophoneChannels);
    SDL_Log("  Sample rate: %.0f Hz", sampleRate);
    SDL_Log("  DSP chunk capacity: %zu frames (%.2f ms); host callback size varies",
        STREAM_BUFFER_SIZE, (STREAM_BUFFER_SIZE * 1000.0f) / sampleRate);

    return true;
}

// Close playback and duplex streams before terminating PortAudio.
void CleanupPortAudio()
{
    // PortAudio device operations remain on the dedicated worker thread.
    StopPortAudioPlaybackSelected();

    CloseRecordedPlayback(paStream, "Live duplex audio");

    Pa_Terminate();
}
