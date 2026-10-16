// Continuous microphone-to-output processing and PortAudio unified callback. Live processor
// instances are private to this translation unit; the shared formant algorithm is in Formant.h.

#include "Stream.h"
#include "PitchJitter.h"
#include "AudioDiagnostics.h"
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

    size_t writePos = 0;
    float readPos = 0.0f;
    size_t xfadePhase = 0;
    bool isCrossfading = false;
    float currentRatio = 1.0f;

public:
    StreamPitchShifter() {
        delayLine.fill(0.0f);
        xfadeBuffer.fill(0.0f);
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
                float t = static_cast<float>(xfadePhase) / (XFADE_SIZE - 1);
                float fadeOut = 0.5f * (1.0f + cosf(3.14159265f * t));
                float fadeIn = 1.0f - fadeOut;

                size_t idx = static_cast<size_t>(readPos);
                float newSample = delayLine[idx % DELAY_SIZE];

                output = xfadeBuffer[xfadePhase] * fadeOut + newSample * fadeIn;

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

// Own temporary arrays reused by the live callback; their capacity is STREAM_BUFFER_SIZE.
struct StreamBuffers {
    array<float, STREAM_BUFFER_SIZE> tempPitch;
    array<float, STREAM_BUFFER_SIZE> tempFormant;
    array<float, STREAM_BUFFER_SIZE> output;
    TimbreShaper timbre;
    ProsodyContour prosody;
    PitchJitter jitter;

    StreamBuffers() {
        tempPitch.fill(0.0f);
        tempFormant.fill(0.0f);
        output.fill(0.0f);
    }
};

// Private processor state is owned by this module; App.cpp only calls the activation and cleanup
// functions.
static StreamBuffers* g_streamBuffers = nullptr;

static PaStream* paStreamDuplex = nullptr;

// PortAudio reports estimated ADC and DAC times for the first positions in one device block.
// The resulting interval describes the device path, not the latency of the processed voice.
static double ReadEstimatedDeviceInterval(const PaStreamCallbackTimeInfo* timeInfo) noexcept {
    if (timeInfo == nullptr) return 0.0;
    return timeInfo->outputBufferDacTime - timeInfo->inputBufferAdcTime;
}

// Handle one duplex audio block. Capture mode stores PCM16 and mutes output; stream mode processes
// and emits floats.
int paUnifiedCallback(
    const void* inputBuffer,
    void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    // Some host APIs may deliver a larger block than the requested size. Split it before
    // indexing the fixed processor arrays and measure the original device callback once.
    if (framesPerBuffer > STREAM_BUFFER_SIZE) {
        const auto fullCallbackStart = chrono::steady_clock::now();
        const auto* input = static_cast<const float*>(inputBuffer);
        auto* output = static_cast<float*>(outputBuffer);
        bool measureSubblock = false;
        for (unsigned long offset = 0; offset < framesPerBuffer; offset += STREAM_BUFFER_SIZE) {
            const auto count = min(static_cast<unsigned long>(STREAM_BUFFER_SIZE),
                framesPerBuffer - offset);
            paUnifiedCallback(input != nullptr ? input + offset * paMicrophoneChannels : nullptr,
                output != nullptr ? output + offset * paMicrophoneChannels : nullptr,
                count, timeInfo, 0, &measureSubblock);
        }
        const bool measureDeviceBlock = userData == nullptr ||
            *static_cast<const bool*>(userData);
        if (g_streamModeActive.load(memory_order_acquire) && measureDeviceBlock) {
            const auto duration = chrono::duration_cast<chrono::microseconds>(
                chrono::steady_clock::now() - fullCallbackStart).count();
            g_audioDiagnostics.recordLiveBlock(framesPerBuffer,
                static_cast<uint64_t>(duration), PA_SAMPLE_RATE,
                (statusFlags & paInputOverflow) != 0,
                (statusFlags & paOutputUnderflow) != 0,
                ReadEstimatedDeviceInterval(timeInfo));
        }
        return paContinue;
    }

    // Measure the complete callback, including capture cleanup, DSP and output writing. The
    // timer uses a monotonic clock and never logs or allocates on this thread.
    const auto callbackStart = chrono::steady_clock::now();
    const float* in = static_cast<const float*>(inputBuffer);
    float* out = static_cast<float*>(outputBuffer);

    // Absent input produces silence when output exists. The sample cleanup below otherwise assumes
    // valid interleaved floats.
    if (in == nullptr) {
        if (out != nullptr) {
            memset(out, 0, framesPerBuffer * paMicrophoneChannels * sizeof(float));
        }
        return paContinue;
    }

    static float hp_x1 = 0.0f, hp_y1 = 0.0f;
    static const float hp_alpha = RescaleLegacySmoothing(0.950f);

    static float rms_squared = 0.0f;
    static const float rms_alpha = RescaleLegacySmoothing(0.8825f);

    static float noise_floor = 0.0f;
    static const float noise_smoothing = RescaleLegacySmoothing(0.9995f);
    static bool noise_initialized = false;
    static uint64_t calibration_samples = 0;

    // The noise estimate is learned from the first callback blocks. These
    // static variables preserve filter and envelope state across blocks.
    constexpr float expander_threshold = 0.015f;
    constexpr float expander_ratio = 2.0f;
    constexpr float expander_attack = 0.001f;
    constexpr float expander_release = 0.050f;
    static float expander_envelope = 1.0f;

    // Calibrate for the inherited 0.6 seconds regardless of device callback block size.
    if (!noise_initialized) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            float sample = fabs(in[i]);
            noise_floor = max(noise_floor, sample);
        }

        calibration_samples += framesPerBuffer;

        if (calibration_samples >= static_cast<uint64_t>(PA_SAMPLE_RATE * 0.6f)) {
            noise_floor *= 1.8f;
            noise_initialized = true;
        }
    }

    // A device callback is split into at most STREAM_BUFFER_SIZE frames above. Keep cleaned
    // samples on the stack so the live path never allocates a vector per device block.
    array<float, STREAM_BUFFER_SIZE> cleanedSamples{};

    for (unsigned long i = 0; i < framesPerBuffer; ++i) {
        // The capture and visualization path currently uses the first channel
        // even when PortAudio supplies interleaved stereo input.
        float sample = in[i * paMicrophoneChannels];

        // One-pole high-pass state removes slow/DC changes before RMS and noise gating.
        float hp_output = hp_alpha * (hp_y1 + sample - hp_x1);
        hp_x1 = sample;
        hp_y1 = hp_output;
        sample = hp_output;

        // Exponentially smooth squared amplitude to avoid a gate reacting to individual sample
        // peaks.
        rms_squared = rms_alpha * rms_squared + (1.0f - rms_alpha) * (sample * sample);
        float rms_current = sqrt(rms_squared);

        if (noise_initialized && rms_current < noise_floor * 0.2f) {
            noise_floor = noise_smoothing * noise_floor + (1.0f - noise_smoothing) * rms_current;
        }

        float gate_threshold = noise_floor * 4.0f;
        float abs_sample = fabs(sample);

        if (abs_sample < gate_threshold) {
            // Attenuate noise gradually near the floor rather than switching
            // the signal off at one threshold.
            float ratio = abs_sample / gate_threshold;
            float attenuation = (exp(ratio * 2.0f) - 1.0f) / (exp(2.0f) - 1.0f);
            sample *= attenuation;
        }

        // Below the threshold, attack and release constants smooth attenuation in the low-level
        // expander.
        if (rms_current < expander_threshold) {
            float target_gain = 1.0f - ((expander_threshold - rms_current) / expander_threshold) * (1.0f - 1.0f / expander_ratio);

            float attack_coeff = exp(-1.0f / (expander_attack * PA_SAMPLE_RATE));
            float release_coeff = exp(-1.0f / (expander_release * PA_SAMPLE_RATE));

            if (target_gain < expander_envelope) {
                expander_envelope = attack_coeff * expander_envelope + (1.0f - attack_coeff) * target_gain;
            }
            else {
                expander_envelope = release_coeff * expander_envelope + (1.0f - release_coeff) * target_gain;
            }

            sample *= expander_envelope;
        }
        else {
            expander_envelope = 1.0f;
        }

        cleanedSamples[i] = sample;
    }

    // Feed both display rings in either audio mode. Each ring has fixed capacity, so
    // the callback neither grows vectors nor erases their prefixes at 48 kHz.
    {
        lock_guard<mutex> lock(waveformMutex);
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            waveformBuffer[waveformWritePos] = cleanedSamples[i];
            waveformWritePos = (waveformWritePos + 1) % WAVEFORM_SAMPLES;
        }
        waveformDataReady.store(true, memory_order_release);
    }
    {
        lock_guard<mutex> lock(paFftMutex);
        if (!paFftInputBuffer.empty()) {
            for (unsigned long i = 0; i < framesPerBuffer; ++i) {
                paFftInputBuffer[paFftWritePos] = cleanedSamples[i];
                paFftWritePos = (paFftWritePos + 1) % paFftInputBuffer.size();
            }
            paFftSamplesAvailable = min(paFftInputBuffer.size(),
                paFftSamplesAvailable + static_cast<size_t>(framesPerBuffer));
        }
    }

    const bool liveMode = g_streamModeActive.load(memory_order_acquire);
    // The outer call measures a large host block once. Recursive 256-frame sub-blocks pass
    // false through userData so this branch does not count them as extra device callbacks.
    const bool measureDeviceBlock = userData == nullptr ||
        *static_cast<const bool*>(userData);
    // Host callbacks larger than 256 frames are split above. Every resulting sub-block
    // must run DSP; the measurement flag controls reporting only.
    if (liveMode) {
        // Activation initializes these private processors in the SDL thread.
        // A missing processor yields silence instead of dereferencing it.
        if (!g_streamBuffers || !g_streamPitchShifter || !g_streamFormantShifter) {
            if (out != nullptr) {
                memset(out, 0, framesPerBuffer * paMicrophoneChannels * sizeof(float));
            }
            return paContinue;
        }

        auto t1_capture = chrono::steady_clock::now();

        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            // Copy the cleaned mono samples into the preallocated pitch processor array.
            g_streamBuffers->tempPitch[i] = cleanedSamples[i];
        }

        auto t2_readyDSP = chrono::steady_clock::now();

        // The new contour follows speech-energy onsets. It modulates the existing pitch
        // processor without introducing another delay line into the live path.
        const uint8_t selected = g_selectedEffects.load(memory_order_acquire);
        const float pitch = g_pitchShiftSemitones.load(memory_order_acquire);
        const float prosody = (selected & EFFECT_PROSODY)
            ? g_prosodyPercent.load(memory_order_acquire) : 0.0f;
        const float jitterAmount = (selected & EFFECT_JITTER)
            ? g_jitterAmount.load(memory_order_acquire) : 0.0f;
        const float constantPitchRatio = pow(2.0f,
            ((selected & EFFECT_PITCH) ? pitch : 0.0f) / 12.0f);
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            const ProsodyStep step = g_streamBuffers->prosody.Advance(cleanedSamples[i], prosody);
            const float jitterRatio = g_streamBuffers->jitter.NextRatio(jitterAmount);
            if ((selected & EFFECT_PITCH) || prosody != 0.0f || jitterAmount > 0.0f) {
                const float targetRatio = constantPitchRatio * (prosody == 0.0f
                    ? 1.0f : pow(2.0f, step.semitones / 12.0f)) * jitterRatio;
                g_streamBuffers->tempPitch[i] = g_streamPitchShifter->ProcessSampleRatio(
                    g_streamBuffers->tempPitch[i], targetRatio);
            }
            g_streamBuffers->output[i] = step.gain;
        }

        if (selected & EFFECT_FORMANT)
            g_streamFormantShifter->ProcessFrame(g_streamBuffers->tempPitch.data(),
                g_streamBuffers->tempFormant.data(), framesPerBuffer,
                g_formantShiftRatio.load(memory_order_acquire));
        else
            copy_n(g_streamBuffers->tempPitch.data(), framesPerBuffer,
                g_streamBuffers->tempFormant.data());

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

        auto t3_readyOutput = chrono::steady_clock::now();

        // Stream mode duplicates each processed mono sample across configured output channels.
        if (out != nullptr) {
            for (unsigned long i = 0; i < framesPerBuffer; ++i) {
                // Soft saturation bounds unexpected DSP peaks before the audio device.
                float sample = tanhf(g_streamBuffers->tempFormant[i] * 0.85f);

                for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                    out[i * paMicrophoneChannels + ch] = sample;
                }
            }
        }

        auto t4_sent = chrono::steady_clock::now();

        // The metric timestamps are inside this callback, so their differences exclude physical
        // device round-trip time.
        if (g_latencyMetrics) {
            g_latencyMetrics->recordSample(t1_capture, t2_readyDSP,
                t3_readyOutput, t4_sent);
        }

    }
    else {

        // Capture writes a single signed 16-bit sample per frame as low byte followed by high
        // byte.
        if (paIsRecording) {
            lock_guard<mutex> recordingLock(paAudioBufferMutex);
            // StopPortAudioRecording clears the flag before waiting on this mutex. Check again
            // under the lock so a stopped capture can be copied without concurrent writes.
            if (paIsRecording.load(memory_order_acquire)) {
                for (unsigned long i = 0; i < framesPerBuffer; ++i) {
                    float sample = cleanedSamples[i];
                    sample = max(-1.0f, min(1.0f, sample));
                    int16_t sample16 = static_cast<int16_t>(sample * 32767.0f);

                    paAudioBuffer.push_back(sample16 & 0xFF);
                    paAudioBuffer.push_back((sample16 >> 8) & 0xFF);
                }
            }
        }

        if (out != nullptr) {
            memset(out, 0, framesPerBuffer * paMicrophoneChannels * sizeof(float));
        }
    }

    if (liveMode) {
        const auto callbackEnd = chrono::steady_clock::now();
        const auto processingMicroseconds = chrono::duration_cast<chrono::microseconds>(
            callbackEnd - callbackStart).count();
        if (measureDeviceBlock) g_audioDiagnostics.recordLiveBlock(framesPerBuffer,
            static_cast<uint64_t>(processingMicroseconds), PA_SAMPLE_RATE,
            (statusFlags & paInputOverflow) != 0,
            (statusFlags & paOutputUnderflow) != 0,
            ReadEstimatedDeviceInterval(timeInfo));
    }
    return paContinue;
}

// Create duplex processors and buffers on demand before enabling stream mode.
bool InitStreamMode() {
    SDL_Log("=== Stream mode initialization at %.0f Hz ===", PA_SAMPLE_RATE);

    // InitStreamMode creates missing processors and arrays on demand. The internal pointers never
    // belong in App.cpp.
    if (!g_streamPitchShifter) {
        g_streamPitchShifter = new StreamPitchShifter();
        SDL_Log("  StreamPitchShifter created (delay: 10ms)");
    }

    if (!g_streamFormantShifter) {
        g_streamFormantShifter = new StreamFormantShifter();
        SDL_Log("  StreamFormantShifter created (window: 16ms)");
    }

    if (!g_streamBuffers) {
        g_streamBuffers = new StreamBuffers();
        SDL_Log("  Stream buffers allocated (%zu samples)", STREAM_BUFFER_SIZE);
    }

    SDL_Log(">>> Stream mode initialized successfully <<<");
    SDL_Log("  DSP chunk capacity: %.2f ms (not end-to-end latency)",
        STREAM_BUFFER_SIZE * 1000.0 / PA_SAMPLE_RATE);

    return true;
}

// Initialize processors if necessary, then enable the atomic stream-mode flag.
void ActivateStreamMode() {
    if (!g_streamPitchShifter || !g_streamFormantShifter || !g_streamBuffers) {
        if (!InitStreamMode()) {
            SDL_Log("ERROR: Failed to initialize stream mode");
            return;
        }
    }

    // Publish activation after initialization so the callback can see the mode and private
    // processor state.
    g_streamModeActive.store(true, memory_order_release);
    SDL_Log(">>> STREAM MODE ACTIVATED <<<");
}

// Disable duplex processing through the public stream API.
void DeactivateStreamMode() {
    // Switch back to recording-mode output behavior; the processors stay allocated until cleanup.
    g_streamModeActive.store(false, memory_order_release);
    SDL_Log(">>> STREAM MODE DEACTIVATED <<<");
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
