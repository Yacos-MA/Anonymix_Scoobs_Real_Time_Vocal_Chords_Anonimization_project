// Continuous microphone-to-output processing and PortAudio unified callback. Processors here are private to this translation unit.

#include "Stream.h"
#include "Dsp.h"

// Keep delay-line and interpolation state between duplex blocks for live pitch adjustment.
class StreamPitchShifter {
private:
    // At 16 kHz the pitch delay line spans 10 ms; XFADE_SIZE sets the read-head transition
    // duration.
    static constexpr size_t DELAY_SIZE = 160;
    static constexpr size_t XFADE_SIZE = 48;

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

    inline float ProcessSample(float input, float semitones) {
        // A semitone shift becomes a read-rate ratio. Smooth changes so slider
        // movement does not abruptly jump the read head through stored audio.
        delayLine[writePos] = input;
        writePos = (writePos + 1) % DELAY_SIZE;

        float targetRatio = pow(2.0f, semitones / 12.0f);
        currentRatio = 0.99f * currentRatio + 0.01f * targetRatio;

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

            if (distance < XFADE_SIZE + 10.0f || distance > DELAY_SIZE - XFADE_SIZE - 10.0f) {
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

    // Process samples sequentially to preserve the delay-line state across the current block.
    void ProcessBuffer(float* buffer, size_t numSamples, float semitones) {
        for (size_t i = 0; i < numSamples; ++i) {
            buffer[i] = ProcessSample(buffer[i], semitones);
        }
    }
};

// Keep the separate live ratio/delay processor state between duplex blocks.
// This processor uses its own variable delay and crossfade. It does not explicitly analyze a
// spectral envelope.
class StreamFormantShifter {
private:
    static constexpr size_t DELAY_SIZE = 256;
    static constexpr size_t XFADE_SIZE = 96;

    array<float, DELAY_SIZE> delayLine;
    array<float, XFADE_SIZE> xfadeBuffer;

    size_t writePos = 0;
    float readPos = 0.0f;
    size_t xfadePhase = 0;
    bool isCrossfading = false;
    float currentRatio = 1.0f;

public:
    StreamFormantShifter() {
        delayLine.fill(0.0f);
        xfadeBuffer.fill(0.0f);
        readPos = DELAY_SIZE / 2.0f;

        SDL_Log("=== StreamFormantShifter MODE SAMPLE-PAR-SAMPLE ===");
        SDL_Log("  Delay: %zu samples (%.1f ms)", DELAY_SIZE, (DELAY_SIZE * 1000.0f) / 16000.0f);
        SDL_Log("  Architecture: IDENTICAL to the pitch shifter");
    }

    // Read a fractional circular-buffer position with four-point interpolation to reduce stepping
    // artifacts.
    inline float ReadDelayInterpolated(float position) const {
        while (position < 0.0f) position += DELAY_SIZE;
        while (position >= DELAY_SIZE) position -= DELAY_SIZE;

        size_t idx = static_cast<size_t>(position);
        float frac = position - idx;

        size_t im1 = (idx + DELAY_SIZE - 1) % DELAY_SIZE;
        size_t i0 = idx;
        size_t i1 = (idx + 1) % DELAY_SIZE;
        size_t i2 = (idx + 2) % DELAY_SIZE;

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

    // Advance the formant-named read head at shiftRatio while continuing to write one input sample
    // each call.
    float ProcessSample(float input, float shiftRatio) {

        delayLine[writePos] = input;
        writePos = (writePos + 1) % DELAY_SIZE;

        currentRatio = 0.995f * currentRatio + 0.005f * shiftRatio;

        float output;

        if (!isCrossfading) {

            output = ReadDelayInterpolated(readPos);

            readPos += currentRatio;
            if (readPos >= DELAY_SIZE) readPos -= DELAY_SIZE;

            float distance = static_cast<float>(writePos) - readPos;
            if (distance < 0.0f) distance += DELAY_SIZE;

            if ((distance < XFADE_SIZE + 24.0f ||
                distance > DELAY_SIZE - XFADE_SIZE - 24.0f)) {

                isCrossfading = true;
                xfadePhase = 0;

                for (size_t i = 0; i < XFADE_SIZE; ++i) {
                    float pos = readPos + i * currentRatio;
                    if (pos >= DELAY_SIZE) pos -= DELAY_SIZE;
                    xfadeBuffer[i] = ReadDelayInterpolated(pos);
                }

                readPos = static_cast<float>(writePos) - (DELAY_SIZE / 2.0f);
                if (readPos < 0.0f) readPos += DELAY_SIZE;
            }
        }
        else {

            if (xfadePhase < XFADE_SIZE) {
                float t = static_cast<float>(xfadePhase) / (XFADE_SIZE - 1);
                float fadeOut = 0.5f * (1.0f + cosf(3.14159265f * t));
                float fadeIn = 1.0f - fadeOut;

                float oldSample = xfadeBuffer[xfadePhase];
                float newSample = ReadDelayInterpolated(readPos);

                output = oldSample * fadeOut + newSample * fadeIn;

                readPos += currentRatio;
                if (readPos >= DELAY_SIZE) readPos -= DELAY_SIZE;

                xfadePhase++;
            }
            else {
                isCrossfading = false;
                output = ReadDelayInterpolated(readPos);
                readPos += currentRatio;
                if (readPos >= DELAY_SIZE) readPos -= DELAY_SIZE;
            }
        }

        return output;
    }

    // Process a whole block without resetting the delay-line state between callbacks.
    void ProcessFrame(const float* input, float* output, size_t numSamples, float shiftRatio) {
        for (size_t i = 0; i < numSamples; ++i) {
            output[i] = ProcessSample(input[i], shiftRatio);
        }
    }
};

static StreamPitchShifter* g_streamPitchShifter = nullptr;
static StreamFormantShifter* g_streamFormantShifter = nullptr;

// Own temporary arrays reused by the live callback; their capacity is STREAM_BUFFER_SIZE.
struct StreamBuffers {
    array<float, STREAM_BUFFER_SIZE> tempPitch;
    array<float, STREAM_BUFFER_SIZE> tempFormant;
    array<float, STREAM_BUFFER_SIZE> output;

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
    constexpr float hp_alpha = 0.950f;

    static float rms_squared = 0.0f;
    constexpr float rms_alpha = 0.8825f;

    static float noise_floor = 0.0f;
    static float noise_smoothing = 0.9995f;
    static bool noise_initialized = false;
    static int calibration_counter = 0;

    // The noise estimate is learned from the first callback blocks. These
    // static variables preserve filter and envelope state across blocks.
    constexpr float expander_threshold = 0.015f;
    constexpr float expander_ratio = 2.0f;
    constexpr float expander_attack = 0.001f;
    constexpr float expander_release = 0.050f;
    static float expander_envelope = 1.0f;

    // Estimate a startup peak from the first 30 callback blocks and use it to set the noise gate
    // reference.
    if (!noise_initialized && calibration_counter < 30) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            float sample = fabs(in[i]);
            noise_floor = max(noise_floor, sample);
        }

        calibration_counter++;

        if (calibration_counter == 30) {
            noise_floor *= 1.8f;
            noise_initialized = true;
        }
    }

// This allocation is in the callback; the current callback also takes locks and grows shared vectors.
    vector<float> cleanedSamples(framesPerBuffer);

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

    if (g_streamModeActive.load(memory_order_acquire)) {
        // Activation initializes these private processors in the SDL thread.
        // A missing processor yields silence instead of dereferencing it.
        if (!g_streamBuffers || !g_streamPitchShifter || !g_streamFormantShifter) {
            if (out != nullptr) {
                memset(out, 0, framesPerBuffer * paMicrophoneChannels * sizeof(float));
            }
            return paContinue;
        }

        auto t1_capture = chrono::high_resolution_clock::now();

        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            // Copy the cleaned mono samples into the preallocated pitch processor array.
            g_streamBuffers->tempPitch[i] = cleanedSamples[i];
        }

        auto t2_readyDSP = chrono::high_resolution_clock::now();

        // Read the current pitch setting atomically and run pitch before the separate formant-
        // named stage.
        g_streamPitchShifter->ProcessBuffer(
            g_streamBuffers->tempPitch.data(),
            framesPerBuffer,
            g_pitchShiftSemitones.load(memory_order_acquire)
        );

        g_streamFormantShifter->ProcessFrame(
            g_streamBuffers->tempPitch.data(),
            g_streamBuffers->tempFormant.data(),
            framesPerBuffer,
            g_formantShiftRatio.load(memory_order_acquire)
        );

        // Jitter and volume are applied after pitch/formant processing and before output clamping.
        float jitter = g_jitterAmount.load(memory_order_acquire);
        static mt19937 rngLatency(random_device{}());

        if (jitter > 0.0f) {
            uniform_real_distribution<float> distLatency(-jitter, jitter);
            for (unsigned long i = 0; i < framesPerBuffer; ++i) {
                g_streamBuffers->tempFormant[i] *= (1.0f + distLatency(rngLatency));
            }
        }

        float volume = g_outputVolume.load(memory_order_acquire);
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            g_streamBuffers->tempFormant[i] *= volume;
        }

        auto t3_readyOutput = chrono::high_resolution_clock::now();

        // Stream mode duplicates each processed mono sample across configured output channels.
        if (out != nullptr) {
            for (unsigned long i = 0; i < framesPerBuffer; ++i) {
                float sample = g_streamBuffers->tempFormant[i];

                for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                    out[i * paMicrophoneChannels + ch] = sample;
                }
            }
        }

        auto t4_sent = chrono::high_resolution_clock::now();

        // The metric timestamps are inside this callback, so their differences exclude physical
        // device round-trip time.
        if (g_latencyMetrics) {
            g_latencyMetrics->t1_microphoneCapture = t1_capture;
            g_latencyMetrics->t2_readyForDSP = t2_readyDSP;
            g_latencyMetrics->t3_readyForOutput = t3_readyOutput;
            g_latencyMetrics->t4_sentToSpeaker = t4_sent;
            g_latencyMetrics->recordSample();
        }

    }
    else {

        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            // Display ring writes are shared with SDL rendering without a full lock protocol.
            waveformBuffer[waveformWritePos] = cleanedSamples[i];
            waveformWritePos = (waveformWritePos + 1) % WAVEFORM_SAMPLES;
        }

        {

            // Only FFT input appends use this mutex; other shared capture and UI state is still
            // not fully synchronized.
            lock_guard<mutex> lock(paFftMutex);
            for (unsigned long i = 0; i < framesPerBuffer; ++i) {
                paFftInputBuffer.push_back(cleanedSamples[i]);
            }
            while (paFftInputBuffer.size() > PA_FFT_SIZE * 2) {
                paFftInputBuffer.erase(paFftInputBuffer.begin(),
                    paFftInputBuffer.begin() + (paFftInputBuffer.size() - PA_FFT_SIZE));
            }
        }

        // Capture writes a single signed 16-bit sample per frame as low byte followed by high
        // byte.
        if (paIsRecording) {
            for (unsigned long i = 0; i < framesPerBuffer; ++i) {
                float sample = cleanedSamples[i];
                sample = max(-1.0f, min(1.0f, sample));
                int16_t sample16 = static_cast<int16_t>(sample * 32767.0f);

                paAudioBuffer.push_back(sample16 & 0xFF);
                paAudioBuffer.push_back((sample16 >> 8) & 0xFF);
            }
        }

        if (out != nullptr) {
            memset(out, 0, framesPerBuffer * paMicrophoneChannels * sizeof(float));
        }
    }

    waveformDataReady = true;
    return paContinue;
}

// Create duplex processors and buffers on demand before enabling stream mode.
bool InitStreamMode() {
    SDL_Log("=== Stream mode initialization (estimated 20 ms latency) ===");

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
        SDL_Log("  Stream buffers allocated (320 samples)");
    }

    SDL_Log(">>> Stream mode initialized successfully <<<");
    SDL_Log("  Estimated latency: ~20ms");

    SDL_Log("  Quality: optimal (windows 256 samples)");

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

// Release the private live processors and their temporary buffers. SDL_AppQuit
// currently calls this before CleanupPortAudio stops the callback; teardown
// therefore has a potential use-after-free race and is not thread safe.
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
