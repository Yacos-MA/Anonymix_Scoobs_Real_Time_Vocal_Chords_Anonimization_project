// Continuous microphone-to-output processing and PortAudio unified callback. Live processor
// instances are private to this translation unit; the shared formant algorithm is in Formant.h.

#include "Stream.h"
#include "Audio.h"
#include "PitchJitter.h"
#include "Dsp.h"
#include "Timbre.h"
#include "Prosody.h"
#include "Formant.h"

// Keep delay-line and interpolation state between duplex blocks for live pitch adjustment.
class StreamPitchShifter {
private:
    // Keep the inherited pitch delay and crossfade durations at 10 ms and 3 ms.
    static constexpr size_t DELAY_SIZE = static_cast<size_t>(PA_SAMPLE_RATE * 0.010f);
    static constexpr size_t XFADE_SIZE = static_cast<size_t>(PA_SAMPLE_RATE * 0.003f);
    static constexpr float EDGE_MARGIN = PA_SAMPLE_RATE * 0.000625f;

    array<float, DELAY_SIZE> delayLine;
    array<float, XFADE_SIZE> xfadeBuffer;
    array<float, XFADE_SIZE> fadeOut;

    size_t writePos = 0;
    float readPos = 0.0f;
    size_t xfadePhase = 0;
    bool isCrossfading = false;
    float currentRatio = 1.0f;

public:
    StreamPitchShifter() {
        delayLine.fill(0.0f);
        xfadeBuffer.fill(0.0f);
        // Integer fade phases repeat: prepare their weights before starting the stream.
        for (size_t i = 0; i < XFADE_SIZE; ++i) {
            const float t = static_cast<float>(i) / (XFADE_SIZE - 1);
            fadeOut[i] = 0.5f * (1.0f + cosf(3.14159265f * t));
        }
        readPos = DELAY_SIZE / 2.0f;
    }

    inline float ProcessSampleRatio(float input, float targetRatio) {
        // A semitone shift becomes a read-rate ratio. Smooth changes so slider
        // movement does not abruptly jump the read head through stored audio.
        delayLine[writePos] = input;
        writePos = (writePos + 1) % DELAY_SIZE;

        static const float smoothing = RescaleLegacySmoothing(0.99f);
        currentRatio = smoothing * currentRatio + (1.0f - smoothing) * targetRatio;

        float output;

        if (!isCrossfading) {
            // Linear interpolation reads between adjacent delay-line samples.
            size_t idx = static_cast<size_t>(readPos);
            float frac = readPos - idx;

            float s0 = delayLine[idx % DELAY_SIZE];
            float s1 = delayLine[(idx + 1) % DELAY_SIZE];
            output = s0 + frac * (s1 - s0);

            readPos += currentRatio;
            if (readPos >= DELAY_SIZE) readPos -= DELAY_SIZE;

            float distance = static_cast<float>(writePos) - readPos;
            if (distance < 0.0f) distance += DELAY_SIZE;

            if (distance < XFADE_SIZE + EDGE_MARGIN ||
                distance > DELAY_SIZE - XFADE_SIZE - EDGE_MARGIN) {
                // Fade across the reset when the read and write heads approach.
                isCrossfading = true;
                xfadePhase = 0;

                for (size_t i = 0; i < XFADE_SIZE; ++i) {
                    float pos = readPos + i * currentRatio;
                    if (pos >= DELAY_SIZE) pos -= DELAY_SIZE;
                    size_t idx = static_cast<size_t>(pos);
                    xfadeBuffer[i] = delayLine[idx % DELAY_SIZE];
                }

                readPos = static_cast<float>(writePos) - (DELAY_SIZE / 2.0f);
                if (readPos < 0.0f) readPos += DELAY_SIZE;
            }
        }
        else {
            // Blend the old read segment into the newly positioned read head.
            if (xfadePhase < XFADE_SIZE) {
                const float outWeight = fadeOut[xfadePhase];
                const float fadeIn = 1.0f - outWeight;

                size_t idx = static_cast<size_t>(readPos);
                float newSample = delayLine[idx % DELAY_SIZE];

                output = xfadeBuffer[xfadePhase] * outWeight + newSample * fadeIn;

                readPos += currentRatio;
                if (readPos >= DELAY_SIZE) readPos -= DELAY_SIZE;

                xfadePhase++;
            }
            else {
                isCrossfading = false;

                size_t idx = static_cast<size_t>(readPos);
                output = delayLine[idx % DELAY_SIZE];

                readPos += currentRatio;
                if (readPos >= DELAY_SIZE) readPos -= DELAY_SIZE;
            }
        }

        return output;
    }

    inline float ProcessSample(float input, float semitones) {
        return ProcessSampleRatio(input, pow(2.0f, semitones / 12.0f));
    }

    // Process samples sequentially to preserve the delay-line state across the current block.
    void ProcessBuffer(float* buffer, size_t numSamples, float semitones) {
        const float ratio = pow(2.0f, semitones / 12.0f);
        for (size_t i = 0; i < numSamples; ++i) {
            buffer[i] = ProcessSampleRatio(buffer[i], ratio);
        }
    }
};

// Keep the live formant processor state between duplex blocks. Playback and export each own
// another instance of the same variable-delay algorithm defined in Formant.h.


static StreamPitchShifter* g_streamPitchShifter = nullptr;
static StreamFormantShifter* g_streamFormantShifter = nullptr;

// The selected effect can be neutral while its delay line still holds several milliseconds of
// audio. Crossfade the live output to the direct signal over 10 ms at neutral settings. Keep
// the processor running so activating the effect uses recent audio instead of a cold buffer.
static float MixWithNeutralBypass(float direct, float delayed, float& wet,
    bool useDelayed) noexcept {
    constexpr float step = 1.0f / (PA_SAMPLE_RATE * 0.010f);
    wet = clamp(wet + (useDelayed ? step : -step), 0.0f, 1.0f);
    if (wet <= 0.0f) return direct;
    if (wet >= 1.0f) return delayed;
    const float smoothWet = wet * wet * (3.0f - 2.0f * wet);
    return direct + smoothWet * (delayed - direct);
}

// Own temporary arrays reused by the live callback; their capacity is STREAM_BUFFER_SIZE.
struct StreamBuffers {
    array<float, STREAM_BUFFER_SIZE> tempPitch;
    array<float, STREAM_BUFFER_SIZE> tempFormant;
    array<float, STREAM_BUFFER_SIZE> output;
    TimbreShaper timbre;
    ProsodyContour prosody;
    PitchJitter jitter;
    float pitchWet = 0.0f;
    float formantWet = 0.0f;

    StreamBuffers() {
        tempPitch.fill(0.0f);
        tempFormant.fill(0.0f);
        output.fill(0.0f);
    }
};

// Private processor state is owned by this module; App.cpp only calls the activation and cleanup
// functions.
static StreamBuffers* g_streamBuffers = nullptr;

static atomic<unsigned> g_captureDeviceGeneration{0};

void ResetCaptureProcessingForNewDevice() noexcept {
    g_captureDeviceGeneration.fetch_add(1, memory_order_release);
}

struct CaptureProcessingState {
    unsigned deviceGeneration = ~0u;
    float highPassInput = 0.0f;
    float highPassOutput = 0.0f;
    float smoothedPower = 0.0f;
    float expanderEnvelope = 1.0f;
    float secondChannelMix = 0.0f;
    float exclusiveInputGain = 1.0f;
};

// Only the PortAudio duplex callback reads and writes this state. A new stream requests a
// generation change; the callback performs its own reset before touching audio samples.
static CaptureProcessingState g_captureProcessing;

static PaStream* paStreamDuplex = nullptr;

// Register from the start of each callback, before reading the recording flag. The RAII
// lifetime also covers early returns and host blocks recursively divided into DSP chunks.
struct DuplexCallbackLifetime final {
    bool recordingAtEntry;
    DuplexCallbackLifetime() noexcept {
        g_duplexCallbacksInFlight.fetch_add(1);
        recordingAtEntry = paIsRecording.load();
    }
    ~DuplexCallbackLifetime() { g_duplexCallbacksInFlight.fetch_sub(1); }
};

// Handle one duplex audio block. Recording mode enqueues cleaned samples for PCM16 collection;
// Stream mode processes floats for output. In Exclusive, the idle output may play a recorded take.
int paUnifiedCallback(
    const void* inputBuffer,
    void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags,
    void*)
{
    DuplexCallbackLifetime lifetime;
    // Some host APIs may deliver a larger block than the requested size. Split it before
    // indexing the fixed processor arrays.
    if (framesPerBuffer > STREAM_BUFFER_SIZE) {
        const auto* input = static_cast<const float*>(inputBuffer);
        auto* output = static_cast<float*>(outputBuffer);
        for (unsigned long offset = 0; offset < framesPerBuffer; offset += STREAM_BUFFER_SIZE) {
            const auto count = min(static_cast<unsigned long>(STREAM_BUFFER_SIZE),
                framesPerBuffer - offset);
            paUnifiedCallback(input != nullptr ? input + offset * paMicrophoneChannels : nullptr,
                output != nullptr ? output + offset * paOutputChannels : nullptr,
                count, timeInfo, 0, nullptr);
        }
        return paContinue;
    }

    const float* in = static_cast<const float*>(inputBuffer);
    float* out = static_cast<float*>(outputBuffer);

    // Absent input produces silence when output exists. The sample cleanup below otherwise assumes
    // valid interleaved floats.
    if (in == nullptr) {
        if (out != nullptr) {
            if (g_activeWasapiMode.load(memory_order_acquire) == 1)
                RenderRecordedPlaybackInDuplex(out, framesPerBuffer);
            else
                memset(out, 0, framesPerBuffer * paOutputChannels * sizeof(float));
        }
        return paContinue;
    }

    const unsigned generation = g_captureDeviceGeneration.load(memory_order_acquire);
    if (g_captureProcessing.deviceGeneration != generation) {
        g_captureProcessing = CaptureProcessingState{};
        g_captureProcessing.deviceGeneration = generation;
    }
    auto& capture = g_captureProcessing;
    static const float hp_alpha = RescaleLegacySmoothing(0.950f);

    static const float rms_alpha = RescaleLegacySmoothing(0.8825f);

    // The gentle low-level expander is the only capture attenuation. The removed gate
    // treated the largest sample in the first 0.6 seconds as noise: speaking during
    // device startup could then cause it to suppress later Shared-mode speech.
    constexpr float expander_threshold = 0.015f;
    constexpr float expander_ratio = 2.0f;
    constexpr float expander_attack = 0.001f;
    constexpr float expander_release = 0.050f;
    static const float attack_coeff = exp(-1.0f / (expander_attack * PA_SAMPLE_RATE));
    static const float release_coeff = exp(-1.0f / (expander_release * PA_SAMPLE_RATE));

    // The microphone array can expose a much quieter channel in Exclusive mode. Prefer the
    // stronger channel for this block, with a 4 ms crossfade to avoid switching clicks. Do
    // not add the channels: an out-of-phase microphone array could cancel speech completely.
    double firstPower = 0.0;
    double secondPower = 0.0;
    if (paMicrophoneChannels == 2) {
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            const float first = in[2 * i];
            const float second = in[2 * i + 1];
            firstPower += static_cast<double>(first) * first;
            secondPower += static_cast<double>(second) * second;
        }
    } else {
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            const float first = in[i];
            firstPower += static_cast<double>(first) * first;
        }
    }
    const float channelTarget = paMicrophoneChannels == 2 &&
        secondPower > firstPower * 1.5 ? 1.0f :
        (firstPower > secondPower * 1.5 ? 0.0f :
            (capture.secondChannelMix >= 0.5f ? 1.0f : 0.0f));

    const bool exclusiveCapture = g_activeWasapiMode.load(memory_order_relaxed) == 1;
    const double strongestChannelPower = paMicrophoneChannels == 2 ?
        max(firstPower, secondPower) : firstPower;
    // Exclusive bypasses some device/Windows input processing on certain microphone arrays.
    // Increase only a genuinely quiet Exclusive input, without assuming a fixed microphone
    // gain. A 42x ceiling and a soft sample limiter bound unexpected speech peaks. Smooth
    // decreases quickly and increases slowly; the callback never allocates or locks here.
    const float inputGainAtStart = capture.exclusiveInputGain;
    if (exclusiveCapture) {
        const float blockRms = sqrtf(static_cast<float>(strongestChannelPower /
            max(1ul, framesPerBuffer)));
        const float wanted = clamp(0.060f / max(blockRms, 0.0015f), 1.0f, 42.0f);
        const float seconds = wanted < inputGainAtStart ? 0.040f : 0.250f;
        const float remaining = expf(-static_cast<float>(framesPerBuffer) /
            (PA_SAMPLE_RATE * seconds));
        capture.exclusiveInputGain = wanted +
            (inputGainAtStart - wanted) * remaining;
    } else {
        capture.exclusiveInputGain = 1.0f;
    }

        // A device callback is split into at most STREAM_BUFFER_SIZE frames above. Keep cleaned
    // samples on the stack so the live path never allocates a vector per device block.
    array<float, STREAM_BUFFER_SIZE> cleanedSamples{};

    const bool applyExpander = !exclusiveCapture;
    for (unsigned long i = 0; i < framesPerBuffer; ++i) {
        const float first = in[i * paMicrophoneChannels];
        const float second = paMicrophoneChannels == 2 ? in[i * 2 + 1] : first;
        capture.secondChannelMix += (channelTarget - capture.secondChannelMix) *
            (1.0f / (PA_SAMPLE_RATE * 0.004f));
        float sample = first + (second - first) * capture.secondChannelMix;

        // One-pole high-pass state removes slow/DC changes before RMS and expansion.
        float hp_output = hp_alpha * (capture.highPassOutput + sample - capture.highPassInput);
        capture.highPassInput = sample;
        capture.highPassOutput = hp_output;
        sample = hp_output;

        if (exclusiveCapture) {
            const float progress = static_cast<float>(i + 1) / framesPerBuffer;
            const float gain = inputGainAtStart +
                (capture.exclusiveInputGain - inputGainAtStart) * progress;
            const float scaled = sample * gain;
            const float absolute = fabsf(scaled);
            // Preserve normal speech samples exactly; only unexpected peaks enter the
            // smooth 0.75..0.95 knee instead of clipping the PCM recording buffer.
            sample = absolute <= 0.75f ? scaled :
                copysignf(0.75f + 0.20f * tanhf((absolute - 0.75f) / 0.20f),
                    scaled);
        }

        // Exponentially smooth squared amplitude so the mild expander does not follow peaks.
        capture.smoothedPower = rms_alpha * capture.smoothedPower +
            (1.0f - rms_alpha) * (sample * sample);
        float rms_current = sqrt(capture.smoothedPower);

        // Below 0.015 RMS, gently reduce Shared-mode noise by at most 6 dB. This stage
        // never learns a threshold from a potentially spoken calibration window.
        if (applyExpander && rms_current < expander_threshold) {
            float target_gain = 1.0f - ((expander_threshold - rms_current) / expander_threshold) * (1.0f - 1.0f / expander_ratio);

            if (target_gain < capture.expanderEnvelope) {
                capture.expanderEnvelope = attack_coeff * capture.expanderEnvelope +
                    (1.0f - attack_coeff) * target_gain;
            }
            else {
                capture.expanderEnvelope = release_coeff * capture.expanderEnvelope +
                    (1.0f - release_coeff) * target_gain;
            }

            sample *= capture.expanderEnvelope;
        }
        else {
            capture.expanderEnvelope = 1.0f;
        }

        cleanedSamples[i] = sample;
    }

    // Feed both display rings in either audio mode. Each ring has fixed capacity, so
    // the callback neither grows vectors nor erases their prefixes at 48 kHz.
    {
        unique_lock<mutex> lock(waveformMutex, try_to_lock);
        // A display snapshot must never delay microphone capture or speaker output.
        if (lock.owns_lock()) {
            for (unsigned long i = 0; i < framesPerBuffer; ++i) {
                waveformBuffer[waveformWritePos] = cleanedSamples[i];
                waveformWritePos = (waveformWritePos + 1) % WAVEFORM_SAMPLES;
            }
            waveformDataReady.store(true, memory_order_release);
        }
    }
    {
        unique_lock<mutex> lock(paFftMutex, try_to_lock);
        if (lock.owns_lock() && !paFftInputBuffer.empty()) {
            // The host block has already been split into at most 256 frames.
            // Copy its two possible contiguous ring spans while holding the display lock;
            // this avoids a variable-length modulo operation for every audio sample.
            const size_t capacity = paFftInputBuffer.size();
            const size_t firstCount = min(static_cast<size_t>(framesPerBuffer),
                capacity - paFftWritePos);
            copy_n(cleanedSamples.data(), firstCount,
                paFftInputBuffer.data() + paFftWritePos);
            copy_n(cleanedSamples.data() + firstCount,
                framesPerBuffer - firstCount, paFftInputBuffer.data());
            paFftWritePos = (paFftWritePos + framesPerBuffer) % capacity;
            paFftSamplesAvailable = min(paFftInputBuffer.size(),
                paFftSamplesAvailable + static_cast<size_t>(framesPerBuffer));
        }
    }

    const bool liveMode = g_streamModeActive.load(memory_order_acquire);
    // Host callbacks larger than 256 frames are split above. Every resulting sub-block
    // still runs the complete processing chain.
    if (liveMode) {
        // Activation initializes these private processors in the SDL thread.
        // A missing processor yields silence instead of dereferencing it.
        if (!g_streamBuffers || !g_streamPitchShifter || !g_streamFormantShifter) {
            if (out != nullptr) {
                memset(out, 0, framesPerBuffer * paOutputChannels * sizeof(float));
            }
            return paContinue;
        }

        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            // Copy the cleaned mono samples into the preallocated pitch processor array.
            g_streamBuffers->tempPitch[i] = cleanedSamples[i];
        }

        // The new contour follows speech-energy onsets. It modulates the existing pitch
        // processor without introducing another delay line into the live path.
        const uint8_t selected = g_selectedEffects.load(memory_order_acquire);
        const float pitch = g_pitchShiftSemitones.load(memory_order_acquire);
        const float prosody = (selected & EFFECT_PROSODY)
            ? g_prosodyPercent.load(memory_order_acquire) : 0.0f;
        const float jitterAmount = (selected & EFFECT_JITTER)
            ? g_jitterAmount.load(memory_order_acquire) : 0.0f;
        const bool pitchNeedsDelay = ((selected & EFFECT_PITCH) && fabsf(pitch) > 0.01f) ||
            prosody != 0.0f || jitterAmount > 0.0f;
        const float constantPitchRatio = pow(2.0f,
            ((selected & EFFECT_PITCH) ? pitch : 0.0f) / 12.0f);
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            const ProsodyStep step = g_streamBuffers->prosody.Advance(cleanedSamples[i], prosody);
            const float jitterRatio = g_streamBuffers->jitter.NextRatio(jitterAmount);
            const float targetRatio = constantPitchRatio * (step.semitones == 0.0f
                ? 1.0f : pow(2.0f, step.semitones / 12.0f)) * jitterRatio;
            const float delayedPitch = g_streamPitchShifter->ProcessSampleRatio(
                cleanedSamples[i], targetRatio);
            g_streamBuffers->tempPitch[i] = MixWithNeutralBypass(
                cleanedSamples[i], delayedPitch, g_streamBuffers->pitchWet, pitchNeedsDelay);
            g_streamBuffers->output[i] = step.gain;
        }

        const float formantRatio = (selected & EFFECT_FORMANT)
            ? g_formantShiftRatio.load(memory_order_acquire) : 1.0f;
        const bool formantNeedsDelay = (selected & EFFECT_FORMANT) &&
            fabsf(formantRatio - 1.0f) > 0.005f;
        g_streamFormantShifter->ProcessFrame(g_streamBuffers->tempPitch.data(),
            g_streamBuffers->tempFormant.data(), framesPerBuffer, formantRatio);
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            g_streamBuffers->tempFormant[i] = MixWithNeutralBypass(
                g_streamBuffers->tempPitch[i], g_streamBuffers->tempFormant[i],
                g_streamBuffers->formantWet, formantNeedsDelay);
        }

        // Mix a fixed-frequency timbre shelf into the processed voice. Reading one atomic per
        // device sub-block is sufficient; smoothing happens per sample in the shaper.
        if (selected & EFFECT_TIMBRE)
            g_streamBuffers->timbre.ProcessBuffer(g_streamBuffers->tempFormant.data(),
                framesPerBuffer, g_timbrePercent.load(memory_order_acquire));

        // Pitch jitter was applied above, before the formant/timbre stages.
        float volume = g_outputVolume.load(memory_order_acquire);
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            g_streamBuffers->tempFormant[i] *= volume * g_streamBuffers->output[i];
        }

        // Stream mode duplicates each processed mono sample across configured output channels.
        if (out != nullptr) {
            for (unsigned long i = 0; i < framesPerBuffer; ++i) {
                // Soft saturation bounds unexpected DSP peaks before the audio device.
                float sample = tanhf(g_streamBuffers->tempFormant[i] * 0.85f);

                for (int ch = 0; ch < paOutputChannels; ++ch) {
                    out[i * paOutputChannels + ch] = sample;
                }
            }
        }

    }
    else {

        // Do not include a device block that started before recording was enabled. Stop waits
        // for every earlier callback before allowing the collector to finish.
        if (lifetime.recordingAtEntry && paIsRecording.load())
            QueuePortAudioRecording(cleanedSamples.data(), framesPerBuffer);

        if (out != nullptr) {
            if (g_activeWasapiMode.load(memory_order_acquire) == 1)
                RenderRecordedPlaybackInDuplex(out, framesPerBuffer);
            else
                memset(out, 0, framesPerBuffer * paOutputChannels * sizeof(float));
        }
    }

    return paContinue;
}

// Create duplex processors and buffers on demand before enabling stream mode.
bool InitStreamMode() {

    // InitStreamMode creates missing processors and arrays on demand. The internal pointers never
    // belong in App.cpp.
    if (!g_streamPitchShifter) {
        g_streamPitchShifter = new StreamPitchShifter();
    }

    if (!g_streamFormantShifter) {
        g_streamFormantShifter = new StreamFormantShifter();
    }

    if (!g_streamBuffers) {
        g_streamBuffers = new StreamBuffers();
    }


    return true;
}

// Initialize processors if necessary, then enable the atomic stream-mode flag.
void ActivateStreamMode() {
    if (!g_streamPitchShifter || !g_streamFormantShifter || !g_streamBuffers) {
        if (!InitStreamMode()) {
            return;
        }
    }

    // Publish activation after initialization so the callback can see the mode and private
    // processor state.
    g_streamModeActive.store(true, memory_order_release);
}

// Disable duplex processing through the public stream API.
void DeactivateStreamMode() {
    // Switch back to recording-mode output behavior; the processors stay allocated until cleanup.
    g_streamModeActive.store(false, memory_order_release);
}

// App.cpp first stops and closes whichever live device backend is active. Only after the
// callback has stopped may these callback-visible processor objects be released.
void CleanupStreamMode() {
    if (g_streamPitchShifter) {
        delete g_streamPitchShifter;
        g_streamPitchShifter = nullptr;
    }

    if (g_streamFormantShifter) {
        delete g_streamFormantShifter;
        g_streamFormantShifter = nullptr;
    }

    if (g_streamBuffers) {
        delete g_streamBuffers;
        g_streamBuffers = nullptr;
    }
}
