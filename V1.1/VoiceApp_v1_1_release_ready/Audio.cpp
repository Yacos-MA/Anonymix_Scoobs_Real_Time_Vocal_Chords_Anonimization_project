// PortAudio capture and playback, with PCM16 processing for saved or played recordings. Callbacks may run on a separate thread.

#include "Audio.h"
#include "Dsp.h"
#include "Stream.h"

// Capture callback: append signed 16-bit sample bytes only while recording is enabled.
// This older capture callback is defined below but is not attached by InitPortAudio; the running
// duplex stream uses paUnifiedCallback in Stream.cpp.
static int paRecordingCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags, void* userData);

// Write the original recorded PCM data to an output stream.
static int paPlaybackCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags, void* userData);

// Play recorded audio with live slider-controlled processing.
static int paPlaybackAnonymizedCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags, void* userData);

// Stateful window and overlap-add processor used for recorded playback and export.
struct RealtimeFormantShifter {
    // The 512-sample analysis window advances by 128 samples, so adjacent windows overlap by 75%.
    static constexpr size_t FRAME_SIZE = 512;
    static constexpr size_t HOP_SIZE = 128;

    vector<float> inputBuffer;
    vector<float> outputBuffer;
    vector<float> overlapBuffer;
    vector<float> window;

    size_t samplesInBuffer = 0;

    RealtimeFormantShifter() {
        inputBuffer.resize(FRAME_SIZE, 0.0f);
        outputBuffer.resize(FRAME_SIZE, 0.0f);
        overlapBuffer.resize(FRAME_SIZE, 0.0f);

        // Precompute a Hann window to taper each frame before the frequency-domain calculation.
        window.resize(FRAME_SIZE);
        for (size_t i = 0; i < FRAME_SIZE; ++i) {
            window[i] = 0.5f * (1.0f - cos(2.0f * 3.14159265f * i / (FRAME_SIZE - 1)));
        }

        SDL_Log("=== Formant shifter final version ===");
        SDL_Log("  Frame: %zu samples (%.1f ms)", FRAME_SIZE, (FRAME_SIZE * 1000.0f) / 16000.0f);
        SDL_Log("  Hop: %zu samples (75%% overlap)", HOP_SIZE);
        SDL_Log("  Method: spectral shifting with a short window and overlap-add");
    }

    // Process incoming samples in frame-sized batches. Internal history and overlap remain across
    // calls.
    void ProcessFrame(const float* input, float* output, size_t numSamples, float shiftRatio) {

        for (size_t i = 0; i < numSamples; ++i) {
            if (samplesInBuffer < FRAME_SIZE) {
                inputBuffer[samplesInBuffer++] = input[i];
            }
        }

        // Before a full analysis frame arrives, the implementation copies input directly to
        // output.
        if (samplesInBuffer < FRAME_SIZE) {
            for (size_t i = 0; i < numSamples; ++i) {
                output[i] = input[i];
            }
            return;
        }

        // Window the frame, transform it to complex frequency bins, and remap positive bins
        // according to shiftRatio.
        vector<float> windowedFrame(FRAME_SIZE);
        for (size_t i = 0; i < FRAME_SIZE; ++i) {
            windowedFrame[i] = inputBuffer[i] * window[i];
        }

        vector<complex<float>> spectrum(FRAME_SIZE);
        for (size_t i = 0; i < FRAME_SIZE; ++i) {
            spectrum[i] = complex<float>(windowedFrame[i], 0.0f);
        }

        FFT_CooleyTukey(spectrum);

        vector<complex<float>> shiftedSpectrum(FRAME_SIZE, complex<float>(0.0f, 0.0f));

        for (size_t bin = 0; bin <= FRAME_SIZE / 2; ++bin) {
            // A ratio can move a bin beyond Nyquist; those bins are discarded. Neighboring bins
            // may map to the same destination.
            float targetBinFloat = static_cast<float>(bin) * shiftRatio;

            if (targetBinFloat > FRAME_SIZE / 2) continue;

            size_t targetBinLow = static_cast<size_t>(floor(targetBinFloat));
            size_t targetBinHigh = min(targetBinLow + 1, FRAME_SIZE / 2);
            float frac = targetBinFloat - targetBinLow;

            complex<float> value = spectrum[bin];

            if (frac < 0.5f) {
                shiftedSpectrum[targetBinLow] = value;
            }
            else {
                if (targetBinHigh <= FRAME_SIZE / 2) {
                    shiftedSpectrum[targetBinHigh] = value;
                }
            }
        }

        // Keep the DC component and mirror positive-frequency bins with conjugates so the inverse
        // result stays real.
        shiftedSpectrum[0] = spectrum[0];

        for (size_t i = 1; i < FRAME_SIZE / 2; ++i) {
            shiftedSpectrum[FRAME_SIZE - i] = conj(shiftedSpectrum[i]);
        }

        // Reuse the forward FFT as an inverse through conjugation, then divide by FRAME_SIZE.
        for (auto& c : shiftedSpectrum) c = conj(c);
        FFT_CooleyTukey(shiftedSpectrum);

        float invFFTSize = 1.0f / FRAME_SIZE;

        // Windowed synthesis accumulates the prior overlap before exposing the current block.
        constexpr float olaFactor = 0.3333f;

        for (size_t i = 0; i < FRAME_SIZE; ++i) {
            float synthSample = conj(shiftedSpectrum[i]).real() * invFFTSize;

            outputBuffer[i] = overlapBuffer[i] + synthSample * window[i] * olaFactor;
        }

        for (size_t i = 0; i < numSamples; ++i) {
            output[i] = outputBuffer[i];
        }

        // Carry the unplayed tail into the next call and shift the analysis window forward by one
        // hop.
        copy(outputBuffer.begin() + HOP_SIZE, outputBuffer.end(), overlapBuffer.begin());
        fill(overlapBuffer.begin() + (FRAME_SIZE - HOP_SIZE), overlapBuffer.end(), 0.0f);

        copy(inputBuffer.begin() + HOP_SIZE, inputBuffer.end(), inputBuffer.begin());
        samplesInBuffer -= HOP_SIZE;
    }
};

// One retained processor instance serves these recorded-audio paths; its frame history persists
// across separate calls.
static RealtimeFormantShifter* g_formantShifter = nullptr;

// Convert PCM16 to float, process a recorded buffer, then write PCM16 back in place.
void ApplyRealtimeFormantShift(vector<Uint8>& audioData, float shiftRatio) {
    if (audioData.empty()) return;

    // Allocate the recorded-audio processor on first use. These global objects are separate from
    // Stream.cpp processors.
    if (!g_formantShifter) {
        g_formantShifter = new RealtimeFormantShifter();
    }

    // The vector is interpreted as packed signed PCM16. Its length in samples is half its byte
    // length.
    int16_t* samples = reinterpret_cast<int16_t*>(audioData.data());
    size_t numSamples = audioData.size() / sizeof(int16_t);

    SDL_Log(">>> FORMANT SHIFT (ratio: %.3f = %+.1f%%) <<<",
        shiftRatio, (shiftRatio - 1.0f) * 100.0f);

    vector<float> inputFloat(numSamples);
    for (size_t i = 0; i < numSamples; ++i) {
        inputFloat[i] = static_cast<float>(samples[i]);
    }

    vector<float> outputFloat(numSamples, 0.0f);
    size_t processedSamples = 0;

    // Submit at most one hop at a time; the final call may contain fewer than 128 samples.
    while (processedSamples < numSamples) {
        size_t blockSize = min(static_cast<size_t>(RealtimeFormantShifter::HOP_SIZE),
            numSamples - processedSamples);

        g_formantShifter->ProcessFrame(
            inputFloat.data() + processedSamples,
            outputFloat.data() + processedSamples,
            blockSize,
            shiftRatio
        );

        processedSamples += blockSize;
    }

    float maxVal = 0.0f;
    for (size_t i = 0; i < numSamples; ++i) {
        maxVal = max(maxVal, fabs(outputFloat[i]));
    }

    // Scale the processed float result before conversion, then clamp to the signed 16-bit range.
    float scale = (maxVal > 1.0f) ? (28000.0f / maxVal) : 0.85f;

    for (size_t i = 0; i < numSamples; ++i) {
        float val = outputFloat[i] * scale;
        val = max(-32768.0f, min(32767.0f, val));
        samples[i] = static_cast<int16_t>(val);
    }

    SDL_Log(">>> END (Peak: %.1f, Scale: %.2f) <<<", maxVal, scale);
}

// Stateful variable-delay pitch processor with a crossfade between read positions.
struct RealtimePitchShifter {
    // The pitch processor writes a circular delay line and keeps its fractional read head between
    // samples.
    static constexpr size_t DELAY_BUFFER_SIZE = 512;
    static constexpr size_t CROSSFADE_SIZE = 96;

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

    float ProcessSample(float input, float pitchShiftSemitones) {

        delayLine[writePos] = input;
        writePos = (writePos + 1) % DELAY_BUFFER_SIZE;

        // Convert semitones to a frequency ratio and smooth the transition across calls.
        float targetRatio = pow(2.0f, pitchShiftSemitones / 12.0f);
        currentPitchRatio = 0.995f * currentPitchRatio + 0.005f * targetRatio;

        float output;

        if (!isCrossfading) {

            output = ReadDelayInterpolated(readPos);

            readPos += currentPitchRatio;
            if (readPos >= DELAY_BUFFER_SIZE) readPos -= DELAY_BUFFER_SIZE;

            float distance = static_cast<float>(writePos) - readPos;
            if (distance < 0.0f) distance += DELAY_BUFFER_SIZE;

            // When read and write heads approach, capture the old segment and prepare a new read
            // position.
            if ((distance < CROSSFADE_SIZE + 24.0f ||
                distance > DELAY_BUFFER_SIZE - CROSSFADE_SIZE - 24.0f) &&
                cooldownFrames == 0) {

                isCrossfading = true;
                crossfadePhase = 0;

                // The saved segment supplies the fade-out portion while the new read head supplies
                // fade-in.
                nextSegment.resize(CROSSFADE_SIZE);
                for (size_t i = 0; i < CROSSFADE_SIZE; ++i) {
                    float pos = readPos + i * currentPitchRatio;
                    if (pos >= DELAY_BUFFER_SIZE) pos -= DELAY_BUFFER_SIZE;
                    nextSegment[i] = ReadDelayInterpolated(pos);
                }

                readPosPrev = readPos;
                readPosNext = static_cast<float>(writePos) - (DELAY_BUFFER_SIZE / 2.0f);
                if (readPosNext < 0.0f) readPosNext += DELAY_BUFFER_SIZE;

                cooldownFrames = 128;
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
                nextSegment.clear();

                output = ReadDelayInterpolated(readPos);
                readPos += currentPitchRatio;
                if (readPos >= DELAY_BUFFER_SIZE) readPos -= DELAY_BUFFER_SIZE;
            }
        }

        // Delay the next read-head reset to avoid repeated overlapping crossfades.
        if (cooldownFrames > 0) cooldownFrames--;

        return output;
    }

    void ProcessBuffer(float* buffer, size_t numSamples, float pitchShiftSemitones) {
        for (size_t i = 0; i < numSamples; ++i) {
            buffer[i] = ProcessSample(buffer[i], pitchShiftSemitones);
        }
    }
};

static RealtimePitchShifter* g_realtimePitchShifter = nullptr;

// Process recorded PCM16 data with the playback pitch shifter; distinct from the duplex processor.
void ApplyRealtimePitchShift(vector<Uint8>& audioData, float semitones) {
    // An empty buffer or zero shift is left untouched. Nonzero shifts reuse state from the stored
    // processor.
    if (audioData.empty() || semitones == 0.0f) return;

    if (!g_realtimePitchShifter) {
        g_realtimePitchShifter = new RealtimePitchShifter();
    }

    int16_t* samples = reinterpret_cast<int16_t*>(audioData.data());
    size_t numSamples = audioData.size() / sizeof(int16_t);

    SDL_Log(">>> PITCH SHIFT: HANN WINDOW, NO RESONANCE <<<");
    SDL_Log("  Shift: %.2f ST | Samples: %zu", semitones, numSamples);

    vector<float> floatBuffer(numSamples);
    for (size_t i = 0; i < numSamples; ++i) {
        floatBuffer[i] = static_cast<float>(samples[i]);
    }

    // Measure original and processed RMS levels so pitch shifting can keep approximately the same
    // loudness.
    float inputRMS = 0.0f;
    float inputPeak = 0.0f;
    for (size_t i = 0; i < numSamples; ++i) {
        inputRMS += floatBuffer[i] * floatBuffer[i];
        inputPeak = max(inputPeak, fabs(floatBuffer[i]));
    }
    inputRMS = sqrt(inputRMS / numSamples);

    SDL_Log("  RMS before: %.1f | Peak before: %.0f", inputRMS, inputPeak);

    g_realtimePitchShifter->ProcessBuffer(floatBuffer.data(), numSamples, semitones);

    float outputRMS = 0.0f;
    float outputPeak = 0.0f;
    for (size_t i = 0; i < numSamples; ++i) {
        outputRMS += floatBuffer[i] * floatBuffer[i];
        outputPeak = max(outputPeak, fabs(floatBuffer[i]));
    }
    outputRMS = sqrt(outputRMS / numSamples);

    SDL_Log("  RMS after: %.1f | Peak after: %.0f", outputRMS, outputPeak);

    float rmsRatio = (outputRMS > 0.01f) ? (inputRMS / outputRMS) : 1.0f;
    // The inherited RMS compensation is intentionally limited to one percent.
    rmsRatio = max(0.99f, min(1.01f, rmsRatio));

    SDL_Log("  RMS compensation: %.4f (limited to ±1%%)", rmsRatio);

    for (size_t i = 0; i < numSamples; ++i) {
        floatBuffer[i] *= rmsRatio;
    }

    outputPeak = 0.0f;
    for (size_t i = 0; i < numSamples; ++i) {
        outputPeak = max(outputPeak, fabs(floatBuffer[i]));
    }

    float scale = 1.0f;
    // Leave headroom under 32767; reduce gain if the processed peak crosses the threshold.
    const float maxAllowedPeak = 30000.0f;

    if (outputPeak > maxAllowedPeak) {
        scale = 28000.0f / outputPeak;
        SDL_Log("  GENTLE NORMALIZATION: %.4f (peak: %.0f)", scale, outputPeak);
    }

    for (size_t i = 0; i < numSamples; ++i) {
        float val = floatBuffer[i] * scale;
        val = max(-32768.0f, min(32767.0f, val));
        samples[i] = static_cast<int16_t>(val);
    }

    SDL_Log(">>> END (Peak final: %.0f / 32767 = %.1f%%) <<<",
        outputPeak * scale, (outputPeak * scale / 32767.0f) * 100.0f);
}

// This legacy implementation repeats the capture cleanup and is not registered by the current
// duplex open.
static int paRecordingCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    const float* in = (const float*)inputBuffer;
    (void)outputBuffer;

    if (in == nullptr) {
        return paContinue;
    }

    static int frameCounter = 0;

    // Filter and noise-estimator state persists between calls; this old callback takes its first
    // channel only.
    static float hp_x1 = 0.0f, hp_y1 = 0.0f;
    constexpr float hp_alpha = 0.950f;

    static float rms_squared = 0.0f;
    constexpr float rms_alpha = 0.8825f;

    static float noise_floor = 0.0f;
    static float noise_smoothing = 0.9995f;
    static bool noise_initialized = false;
    static int calibration_counter = 0;

    constexpr float expander_threshold = 0.015f;
    constexpr float expander_ratio = 2.0f;
    constexpr float expander_attack = 0.001f;
    constexpr float expander_release = 0.050f;

    static float expander_envelope = 1.0f;

    if (!noise_initialized && calibration_counter < 30) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            float sample = fabs(in[i]);
            noise_floor = max(noise_floor, sample);
        }

        calibration_counter++;

        if (calibration_counter == 30) {
            noise_floor *= 1.8f;
            noise_initialized = true;
            SDL_Log(">>> Noise floor calibrated: %.6f (60 ms @ 32 frames) <<<", noise_floor);
        }
    }

    // This vector allocation and later vector growth happen inside a callback, adding unbounded
    // real-time cost.
    vector<float> cleanedSamples(framesPerBuffer);

    for (unsigned long i = 0; i < framesPerBuffer; ++i) {
        float sample = in[i * paMicrophoneChannels];

        float hp_output = hp_alpha * (hp_y1 + sample - hp_x1);
        hp_x1 = sample;
        hp_y1 = hp_output;
        sample = hp_output;

        rms_squared = rms_alpha * rms_squared + (1.0f - rms_alpha) * (sample * sample);
        float rms_current = sqrt(rms_squared);

        if (noise_initialized && rms_current < noise_floor * 0.2f) {
            noise_floor = noise_smoothing * noise_floor + (1.0f - noise_smoothing) * rms_current;
        }

        float gate_threshold = noise_floor * 4.0f;
        float abs_sample = fabs(sample);

        if (abs_sample < gate_threshold) {

            float ratio = abs_sample / gate_threshold;
            float attenuation = (exp(ratio * 2.0f) - 1.0f) / (exp(2.0f) - 1.0f);
            sample *= attenuation;
        }

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

    for (unsigned long i = 0; i < framesPerBuffer; ++i) {
        // Write the display ring; the rendering thread reads it without a complete lock protocol.
        waveformBuffer[waveformWritePos] = cleanedSamples[i];
        waveformWritePos = (waveformWritePos + 1) % WAVEFORM_SAMPLES;
    }

    {
        lock_guard<mutex> lock(paFftMutex);
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            paFftInputBuffer.push_back(cleanedSamples[i]);
        }
        while (paFftInputBuffer.size() > PA_FFT_SIZE * 2) {
            paFftInputBuffer.erase(paFftInputBuffer.begin(),
                paFftInputBuffer.begin() + (paFftInputBuffer.size() - PA_FFT_SIZE));
        }
    }

    // Store low and high bytes of one signed PCM16 sample per frame; this storage is mono even
    // with two input channels.
    if (paIsRecording) {
        for (unsigned long i = 0; i < framesPerBuffer; ++i) {
            float sample = cleanedSamples[i];
            sample = max(-1.0f, min(1.0f, sample));
            int16_t sample16 = static_cast<int16_t>(sample * 32767.0f);
            paAudioBuffer.push_back(sample16 & 0xFF);
            paAudioBuffer.push_back((sample16 >> 8) & 0xFF);
        }
    }

    if (++frameCounter % 100 == 0) {
        float rms = sqrt(rms_squared);

        SDL_Log("PortAudio low-latency mode (32 frames): RMS=%.6f | Noise=%.6f | Gate=%.6f | Expander=%.3f",
            rms, noise_floor, noise_floor * 4.0f, expander_envelope);
    }

    waveformDataReady = true;
    return paContinue;
}

// Playback callbacks receive float32 output buffers and convert captured PCM16 bytes back to
// normalized floats.
static int paPlaybackCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    float* out = (float*)outputBuffer;
    (void)inputBuffer;

    // Inherited error path dereferences out even though it is null. This is a known defect, not a
    // safe silence fallback.
    if (out == nullptr) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            out[i] = 0.0f;
        }
        return paContinue;
    }

    // When stopped or empty, fill the entire requested output block with silence.
    if (!paIsPlaying || paAudioBuffer.empty()) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            out[i] = 0.0f;
        }
        return paContinue;
    }

    int16_t* samples = reinterpret_cast<int16_t*>(paAudioBuffer.data());
    size_t totalSamples = paAudioBuffer.size() / sizeof(int16_t);

    // The UI updates the atomic volume setting; copy it once per block before writing the samples.
    float volume = g_outputVolume.load(memory_order_acquire);

    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        if (paPlaybackPosition < totalSamples) {

            float sample = static_cast<float>(samples[paPlaybackPosition]) / 32767.0f;

            sample *= volume;

            sample = max(-1.0f, min(1.0f, sample));

            for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                out[frame * paMicrophoneChannels + ch] = sample;
            }

            // Playback advances according to configured channels although the recording path
            // stores only one sample per frame.
            paPlaybackPosition += paMicrophoneChannels;
        }
        else {

            for (unsigned long j = frame; j < framesPerBuffer; ++j) {
                for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                    out[j * paMicrophoneChannels + ch] = 0.0f;
                }
            }
            paIsPlaying = false;
            SDL_Log(">>> PortAudio callback: playback completed <<<");
            break;
        }
    }

    return paContinue;
}

// Pitch playback reads a copy of the capture, then applies live slider settings to each output
// block.
static int paPlaybackAnonymizedCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    float* out = (float*)outputBuffer;
    (void)inputBuffer;

    // This null-output check also writes through out in the inherited implementation.
    if (out == nullptr) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            out[i] = 0.0f;
        }
        return paContinue;
    }

    if (!paIsPlayingAnonymized || paProcessedBuffer.empty()) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            out[i] = 0.0f;
        }
        return paContinue;
    }

    int16_t* samples = reinterpret_cast<int16_t*>(paProcessedBuffer.data());
    size_t totalSamples = paProcessedBuffer.size() / sizeof(int16_t);

    // Read the current pitch, jitter and volume atomics once at the start of the block.
    float pitchShift = g_pitchShiftSemitones.load(memory_order_acquire);
    float jitter = g_jitterAmount.load(memory_order_acquire);
    float volume = g_outputVolume.load(memory_order_acquire);

    if (!g_realtimePitchShifter) {
        g_realtimePitchShifter = new RealtimePitchShifter();
    }

    static mt19937 rng(random_device{}());

    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        if (paPlaybackAnonymizedPosition < totalSamples) {

            float sample = static_cast<float>(samples[paPlaybackAnonymizedPosition]) / 32767.0f;

            sample = g_realtimePitchShifter->ProcessSample(sample, pitchShift);

            uniform_real_distribution<float> dist(-jitter, jitter);
            sample *= (1.0f + dist(rng));

            sample *= volume;

            sample = max(-1.0f, min(1.0f, sample));

            for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                out[frame * paMicrophoneChannels + ch] = sample;
            }

            paPlaybackAnonymizedPosition += paMicrophoneChannels;
        }
        else {
            for (unsigned long j = frame; j < framesPerBuffer; ++j) {
                for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                    out[j * paMicrophoneChannels + ch] = 0.0f;
                }
            }
            paIsPlayingAnonymized = false;
            SDL_Log(">>> Anonymized PortAudio callback: playback completed <<<");
            break;
        }
    }

    return paContinue;
}

// Playback variant using the formant processor.
// Formant playback collects a frame of PCM16-derived floats and processes it before interleaving
// output.
static int paPlaybackAnonymizedFormantCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    float* out = (float*)outputBuffer;
    (void)inputBuffer;

    // The same invalid null-output write exists on this callback path.
    if (out == nullptr) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            out[i] = 0.0f;
        }
        return paContinue;
    }

    if (!paIsPlayingAnonymizedFormant || paProcessedFormantBuffer.empty()) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            out[i] = 0.0f;
        }
        return paContinue;
    }

    int16_t* samples = reinterpret_cast<int16_t*>(paProcessedFormantBuffer.data());
    size_t totalSamples = paProcessedFormantBuffer.size() / sizeof(int16_t);

    float formantShift = g_formantShiftRatio.load(memory_order_acquire);
    float jitter = g_jitterAmount.load(memory_order_acquire);
    float volume = g_outputVolume.load(memory_order_acquire);

    if (!g_formantShifter) {
        g_formantShifter = new RealtimeFormantShifter();
    }

    static mt19937 rng(random_device{}());
    // Static frames retain their initial allocation size; a future callback-size change requires a
    // capacity audit.
    static vector<float> frameBuffer(framesPerBuffer);

    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        if (paPlaybackAnonymizedFormantPosition < totalSamples) {
            frameBuffer[frame] = static_cast<float>(samples[paPlaybackAnonymizedFormantPosition]);
            paPlaybackAnonymizedFormantPosition += paMicrophoneChannels;
        }
        else {
            frameBuffer[frame] = 0.0f;
        }
    }

    static vector<float> outputFrameBuffer(framesPerBuffer);
    // The frame processor runs in the playback callback; its allocations and shared state affect
    // real-time behavior.
    g_formantShifter->ProcessFrame(frameBuffer.data(), outputFrameBuffer.data(),
        framesPerBuffer, formantShift);

    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        if (paPlaybackAnonymizedFormantPosition - frame * paMicrophoneChannels <= totalSamples) {
            float sample = outputFrameBuffer[frame] / 32767.0f;

            uniform_real_distribution<float> dist(-jitter, jitter);
            sample *= (1.0f + dist(rng));

            sample *= volume;

            sample = max(-1.0f, min(1.0f, sample));

            for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                out[frame * paMicrophoneChannels + ch] = sample;
            }
        }
        else {
            for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
                out[frame * paMicrophoneChannels + ch] = 0.0f;
            }
            paIsPlayingAnonymizedFormant = false;
            SDL_Log(">>> Formant callback: playback completed <<<");
            break;
        }
    }

    if (paPlaybackAnonymizedFormantPosition >= totalSamples) {
        paIsPlayingAnonymizedFormant = false;
        SDL_Log(">>> Formant callback: playback completed <<<");
    }

    return paContinue;
}

// Playback variant using both pitch and formant processors.
// Combined playback applies pitch per sample, then formant per frame, then jitter and volume.
static int paPlaybackAnonymizedPitchFormantCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    float* out = (float*)outputBuffer;
    (void)inputBuffer;

    if (out == nullptr || !paIsPlayingAnonymizedPitchFormant || paProcessedPitchFormantBuffer.empty()) {
        if (out != nullptr) {
            for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
                out[i] = 0.0f;
            }
        }
        return paContinue;
    }

    int16_t* samples = reinterpret_cast<int16_t*>(paProcessedPitchFormantBuffer.data());
    size_t totalSamples = paProcessedPitchFormantBuffer.size() / sizeof(int16_t);

    float pitchShift = g_pitchShiftSemitones.load(memory_order_acquire);
    float formantShift = g_formantShiftRatio.load(memory_order_acquire);
    float jitter = g_jitterAmount.load(memory_order_acquire);
    float volume = g_outputVolume.load(memory_order_acquire);

    if (!g_realtimePitchShifter) g_realtimePitchShifter = new RealtimePitchShifter();
    if (!g_formantShifter) g_formantShifter = new RealtimeFormantShifter();

    static mt19937 rng(random_device{}());
    static vector<float> frameBuffer(framesPerBuffer);
    static vector<float> outputFrameBuffer(framesPerBuffer);

    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        if (paPlaybackAnonymizedPitchFormantPosition < totalSamples) {
            float sample = static_cast<float>(samples[paPlaybackAnonymizedPitchFormantPosition]);

            frameBuffer[frame] = g_realtimePitchShifter->ProcessSample(sample, pitchShift);

            paPlaybackAnonymizedPitchFormantPosition += paMicrophoneChannels;
        }
        else {
            frameBuffer[frame] = 0.0f;
        }
    }

    // This second call belongs to combined playback; both variants share the same retained formant
    // processor.
    g_formantShifter->ProcessFrame(frameBuffer.data(), outputFrameBuffer.data(),
        framesPerBuffer, formantShift);

    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        float sample = outputFrameBuffer[frame] / 32767.0f;

        uniform_real_distribution<float> dist(-jitter, jitter);
        sample *= (1.0f + dist(rng));

        sample *= volume;

        sample = max(-1.0f, min(1.0f, sample));

        for (int ch = 0; ch < paMicrophoneChannels; ++ch) {
            out[frame * paMicrophoneChannels + ch] = sample;
        }
    }

    if (paPlaybackAnonymizedPitchFormantPosition >= totalSamples) {
        paIsPlayingAnonymizedPitchFormant = false;
        SDL_Log(">>> Pitch and formant callback: playback completed <<<");
    }

    return paContinue;
}

// Open or start the formant-only recorded playback variant.
bool StartPortAudioPlaybackAnonymizedFormant()
{
    SDL_Log("=== StartPortAudioPlaybackAnonymizedFormant called ===");

    if (paIsPlayingAnonymizedFormant) {
        SDL_Log("Cannot play: formant playback already active");
        return false;
    }

    if (paAudioBuffer.empty()) {
        SDL_Log("Cannot play: PortAudio buffer is empty");
        return false;
    }

    // Keep original capture bytes for later playback and WAV saves; processing occurs inside the
    // callback.
    paProcessedFormantBuffer = paAudioBuffer;

    SDL_Log("Buffer copied: %zu bytes (real-time DSP in callback)", paProcessedFormantBuffer.size());

    // Open and start this playback stream only on its first use; later starts reset the read
    // position.
    if (paPlaybackAnonymizedFormantStream == nullptr) {
        PaStreamParameters outputParams;
        outputParams.device = Pa_GetDefaultOutputDevice();

        // This branch only logs the absence of output. The following Pa_GetDeviceInfo use still
        // expects a valid device.
        if (outputParams.device == paNoDevice) {
            SDL_Log("No output device found");
            return false;
        }

        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParams.device);
        outputParams.channelCount = paMicrophoneChannels;
        outputParams.sampleFormat = paFloat32;
        outputParams.suggestedLatency = deviceInfo->defaultLowOutputLatency;
        outputParams.hostApiSpecificStreamInfo = NULL;

        PaError err = Pa_OpenStream(&paPlaybackAnonymizedFormantStream, NULL, &outputParams,
            PA_SAMPLE_RATE, 128, paClipOff, paPlaybackAnonymizedFormantCallback, NULL);

        if (err != paNoError) {
            SDL_Log("Error Pa_OpenStream playback formant: %s", Pa_GetErrorText(err));
            return false;
        }

        err = Pa_StartStream(paPlaybackAnonymizedFormantStream);
        if (err != paNoError) {
            SDL_Log("Error Pa_StartStream playback formant: %s", Pa_GetErrorText(err));
            Pa_CloseStream(paPlaybackAnonymizedFormantStream);
            paPlaybackAnonymizedFormantStream = nullptr;
            return false;
        }
    }

    paPlaybackAnonymizedFormantPosition = 0;
    paIsPlayingAnonymizedFormant = true;

    SDL_Log(">>> FORMANT SHIFT playback started (real-time DSP with sliders) <<<");
    return true;
}

void StopPortAudioPlaybackAnonymizedFormant()
{
    if (!paIsPlayingAnonymizedFormant) return;

    paIsPlayingAnonymizedFormant = false;
    paPlaybackAnonymizedFormantPosition = 0;

    SDL_Log("Formant playback stopped");
}

// Open or start the combined pitch/formant playback variant.
bool StartPortAudioPlaybackAnonymizedPitchFormant()
{
    SDL_Log("=== StartPortAudioPlaybackAnonymizedPitchFormant called ===");

    if (paIsPlayingAnonymizedPitchFormant) {
        SDL_Log("Cannot play: pitch and formant playback already active");
        return false;
    }

    if (paAudioBuffer.empty()) {
        SDL_Log("Cannot play: PortAudio buffer is empty");
        return false;
    }

    // The combined variant has its own byte copy and playback position.
    paProcessedPitchFormantBuffer = paAudioBuffer;

    SDL_Log("Buffer copied: %zu bytes (real-time DSP in callback)", paProcessedPitchFormantBuffer.size());

    if (paPlaybackAnonymizedPitchFormantStream == nullptr) {
        PaStreamParameters outputParams;
        outputParams.device = Pa_GetDefaultOutputDevice();

        if (outputParams.device == paNoDevice) {
            SDL_Log("No output device found");
            return false;
        }

        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParams.device);
        outputParams.channelCount = paMicrophoneChannels;
        outputParams.sampleFormat = paFloat32;
        outputParams.suggestedLatency = deviceInfo->defaultLowOutputLatency;
        outputParams.hostApiSpecificStreamInfo = NULL;

        PaError err = Pa_OpenStream(&paPlaybackAnonymizedPitchFormantStream, NULL, &outputParams,
            PA_SAMPLE_RATE, 128, paClipOff, paPlaybackAnonymizedPitchFormantCallback, NULL);

        if (err != paNoError) {
            SDL_Log("Error Pa_OpenStream playback Pitch+Formant: %s", Pa_GetErrorText(err));
            return false;
        }

        err = Pa_StartStream(paPlaybackAnonymizedPitchFormantStream);
        if (err != paNoError) {
            SDL_Log("Error Pa_StartStream playback Pitch+Formant: %s", Pa_GetErrorText(err));
            Pa_CloseStream(paPlaybackAnonymizedPitchFormantStream);
            paPlaybackAnonymizedPitchFormantStream = nullptr;
            return false;
        }
    }

    paPlaybackAnonymizedPitchFormantPosition = 0;
    paIsPlayingAnonymizedPitchFormant = true;

    SDL_Log(">>> COMBINED PITCH+FORMANT playback started (real-time DSP with sliders) <<<");
    return true;
}

void StopPortAudioPlaybackAnonymizedPitchFormant()
{
    if (!paIsPlayingAnonymizedPitchFormant) return;

    paIsPlayingAnonymizedPitchFormant = false;
    paPlaybackAnonymizedPitchFormantPosition = 0;

    SDL_Log("Pitch and formant playback stopped");
}

// Reset the capture buffer and enable recording on the existing unified stream.
bool StartPortAudioRecording()
{
    SDL_Log("=== StartPortAudioRecording called ===");

    if (paIsRecording) {
        SDL_Log("PortAudio recording already active");
        return false;
    }

    // A new recording discards the previous take. shrink_to_fit may deallocate storage before
    // callback writes begin.
    paAudioBuffer.clear();
    paAudioBuffer.shrink_to_fit();
    SDL_Log("PortAudio buffer cleared (new capacity: %zu)", paAudioBuffer.capacity());

    paIsRecording = true;

    SDL_Log(">>> PortAudio recording STARTED <<<");
    return true;
}

// Disable capture while retaining the PCM16 buffer for playback and saving.
void StopPortAudioRecording()
{
    if (!paIsRecording) return;

    paIsRecording = false;
    SDL_Log("PortAudio recording stopped - %zu bytes captured", paAudioBuffer.size());
}

// Open or start playback of the captured original audio.
bool StartPortAudioPlayback()
{
    SDL_Log("=== StartPortAudioPlayback called ===");

    if (paIsPlaying) {
        SDL_Log("Cannot play: PortAudio playback already active");
        return false;
    }

    if (paAudioBuffer.empty()) {
        SDL_Log("Cannot play: PortAudio buffer is empty");
        return false;
    }

    SDL_Log("PortAudio buffer contains %zu bytes (%zu samples, %d channels)",
        paAudioBuffer.size(),
        paAudioBuffer.size() / sizeof(int16_t),
        paMicrophoneChannels);

    // The original playback stream is retained between uses, while each Start resets its position
    // and flag.
    if (paPlaybackStream == nullptr) {
        PaStreamParameters outputParams;
        outputParams.device = Pa_GetDefaultOutputDevice();

        if (outputParams.device == paNoDevice) {
            SDL_Log("No output device found");
            return false;
        }

        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParams.device);
        outputParams.channelCount = paMicrophoneChannels;
        outputParams.sampleFormat = paFloat32;
        outputParams.suggestedLatency = deviceInfo->defaultLowOutputLatency;
        outputParams.hostApiSpecificStreamInfo = NULL;

        PaError err = Pa_OpenStream(&paPlaybackStream, NULL, &outputParams,
            PA_SAMPLE_RATE, 128, paClipOff, paPlaybackCallback, NULL);

        if (err != paNoError) {
            SDL_Log("Error Pa_OpenStream playback: %s", Pa_GetErrorText(err));
            return false;
        }

        err = Pa_StartStream(paPlaybackStream);
        if (err != paNoError) {
            SDL_Log("Error Pa_StartStream playback: %s", Pa_GetErrorText(err));
            Pa_CloseStream(paPlaybackStream);
            paPlaybackStream = nullptr;
            return false;
        }

        SDL_Log("Stream PortAudio playback created successfully (%d channels)", paMicrophoneChannels);
    }

    paPlaybackPosition = 0;
    paIsPlaying = true;

    SDL_Log(">>> PortAudio playback STARTED (volume controlled by slider) <<<");
    return true;
}

void StopPortAudioPlayback()
{
    if (!paIsPlaying) return;

    paIsPlaying = false;
    paPlaybackPosition = 0;

    SDL_Log("PortAudio playback stopped");
}

// Open or start slider-controlled anonymized playback.
bool StartPortAudioPlaybackAnonymized()
{
    SDL_Log("=== StartPortAudioPlaybackAnonymized called ===");

    if (paIsPlayingAnonymized) {
        SDL_Log("Cannot play: anonymized playback already active");
        return false;
    }

    if (paAudioBuffer.empty()) {
        SDL_Log("Cannot play: PortAudio buffer is empty");
        return false;
    }

    // Pitch playback uses a byte copy; the slider-controlled processor changes samples during the
    // playback callback.
    paProcessedBuffer = paAudioBuffer;

    SDL_Log("Buffer copied: %zu bytes (real-time DSP in callback)", paProcessedBuffer.size());

    if (paPlaybackAnonymizedStream == nullptr) {
        PaStreamParameters outputParams;
        outputParams.device = Pa_GetDefaultOutputDevice();

        if (outputParams.device == paNoDevice) {
            SDL_Log("No output device found");
            return false;
        }

        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParams.device);
        outputParams.channelCount = paMicrophoneChannels;
        outputParams.sampleFormat = paFloat32;
        outputParams.suggestedLatency = deviceInfo->defaultLowOutputLatency;
        outputParams.hostApiSpecificStreamInfo = NULL;

        PaError err = Pa_OpenStream(&paPlaybackAnonymizedStream, NULL, &outputParams,
            PA_SAMPLE_RATE, 128, paClipOff, paPlaybackAnonymizedCallback, NULL);

        if (err != paNoError) {
            SDL_Log("Error Pa_OpenStream anonymized playback: %s", Pa_GetErrorText(err));
            return false;
        }

        err = Pa_StartStream(paPlaybackAnonymizedStream);
        if (err != paNoError) {
            SDL_Log("Error Pa_StartStream anonymized playback: %s", Pa_GetErrorText(err));
            Pa_CloseStream(paPlaybackAnonymizedStream);
            paPlaybackAnonymizedStream = nullptr;
            return false;
        }

        SDL_Log("Stream PortAudio anonymized playback created successfully");
    }

    paPlaybackAnonymizedPosition = 0;
    paIsPlayingAnonymized = true;

    SDL_Log(">>> Anonymized PortAudio playback STARTED (real-time DSP with sliders) <<<");
    return true;
}

void StopPortAudioPlaybackAnonymized()
{
    if (!paIsPlayingAnonymized) return;

    paIsPlayingAnonymized = false;
    paPlaybackAnonymizedPosition = 0;

    SDL_Log("Anonymized PortAudio playback stopped");
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

    PaStreamParameters inputParams;
    inputParams.device = Pa_GetDefaultInputDevice();

    if (inputParams.device == paNoDevice) {
        SDL_Log("No microphone found");
        Pa_Terminate();
        return false;
    }

    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(inputParams.device);
    SDL_Log("=== DEVICE INFO ===");
    SDL_Log("Name: %s", deviceInfo->name);
    SDL_Log("Maximum input channels: %d", deviceInfo->maxInputChannels);
    SDL_Log("Default sample rate: %.0f Hz", deviceInfo->defaultSampleRate);

    double sampleRate = PA_SAMPLE_RATE;
    bool stereoSuccess = false;

    // The trial attempts two-channel input but inspects the capture buffer while recording is
    // disabled. It cannot establish true stereo reliably.
    if (deviceInfo->maxInputChannels >= 2) {
        SDL_Log("Attempting STEREO open (2 channels)...");

        inputParams.channelCount = 2;
        inputParams.sampleFormat = paFloat32;

        inputParams.suggestedLatency = 0.003;
        inputParams.hostApiSpecificStreamInfo = NULL;

        err = Pa_OpenStream(&paStream, &inputParams, NULL, sampleRate, 320,
            paClipOff, paUnifiedCallback, NULL);

        if (err == paNoError) {

            // A successful duplex open attaches paUnifiedCallback. Only after Start can the device
            // invoke it.
            err = Pa_StartStream(paStream);
            if (err == paNoError) {

                // Waiting here does not populate paAudioBuffer unless recording is active. The
                // stereo inference below remains unreliable.
                Pa_Sleep(200);

                bool channelsAreDifferent = false;

                // The trial assumes interleaved stereo bytes in paAudioBuffer, which the unified
                // capture callback does not produce.
                if (paAudioBuffer.size() >= 4096) {
                    int16_t* samples = reinterpret_cast<int16_t*>(paAudioBuffer.data());
                    size_t numSamples = paAudioBuffer.size() / sizeof(int16_t);

                    int differenceCount = 0;
                    int totalSamples = 0;

                    for (size_t i = 0; i < numSamples - 1; i += 2) {
                        int16_t leftChannel = samples[i];
                        int16_t rightChannel = samples[i + 1];

                        int16_t tolerance = abs(leftChannel) / 20;

                        if (abs(leftChannel - rightChannel) > tolerance) {
                            differenceCount++;
                        }

                        totalSamples++;
                    }

                    if (totalSamples > 0 && (differenceCount * 100 / totalSamples) > 10) {
                        channelsAreDifferent = true;
                    }

                    SDL_Log("Stereo analysis: %d/%d different samples (%.1f%%)",
                        differenceCount, totalSamples,
                        totalSamples > 0 ? (differenceCount * 100.0f / totalSamples) : 0.0f);
                }

                Pa_StopStream(paStream);
                Pa_CloseStream(paStream);
                paStream = nullptr;

                paAudioBuffer.clear();

                if (channelsAreDifferent) {
                    SDL_Log(">>> TRUE STEREO DETECTED (independent channels) <<<");
                    stereoSuccess = true;
                    paMicrophoneChannels = 2;
                }
                else {
                    SDL_Log(">>> DUPLICATED MONO DETECTED (identical channels) <<<");
                    SDL_Log("Fallback to MONO...");
                    stereoSuccess = false;
                }
            }
            else {
                SDL_Log("Failed to start stereo stream: %s", Pa_GetErrorText(err));
                Pa_CloseStream(paStream);
                paStream = nullptr;
                stereoSuccess = false;
            }
        }
        else {
            SDL_Log("Failed to open STEREO: %s", Pa_GetErrorText(err));
            SDL_Log("Automatic fallback to MONO...");
            stereoSuccess = false;
        }
    }
    else {
        SDL_Log("Hardware supports only one channel --> MONO only");
    }

    PaStreamParameters outputParams;
    outputParams.device = Pa_GetDefaultOutputDevice();

    if (outputParams.device == paNoDevice) {
        SDL_Log("WARNING: No output device found");

    }

    const PaDeviceInfo* outputDeviceInfo = Pa_GetDeviceInfo(outputParams.device);
    outputParams.channelCount = paMicrophoneChannels;
    outputParams.sampleFormat = paFloat32;

    outputParams.suggestedLatency = 0.003;
    outputParams.hostApiSpecificStreamInfo = NULL;

    SDL_Log("Output device: %s (%d channels)",
        outputDeviceInfo->name, outputParams.channelCount);

    if (!stereoSuccess) {
        SDL_Log("Opening MONO DUPLEX (1 channel in/out)...");

        inputParams.channelCount = 1;
        inputParams.sampleFormat = paFloat32;
        inputParams.suggestedLatency = deviceInfo->defaultLowInputLatency;
        inputParams.hostApiSpecificStreamInfo = NULL;

        outputParams.channelCount = 1;

        err = Pa_OpenStream(&paStream,
            &inputParams,
            &outputParams,
            sampleRate,
            320,
            paClipOff,
            paUnifiedCallback,
            NULL);

        if (err != paNoError) {
            SDL_Log("Error Pa_OpenStream MONO DUPLEX: %s", Pa_GetErrorText(err));
            Pa_Terminate();
            return false;
        }

        paMicrophoneChannels = 1;
        SDL_Log(">>> MONO DUPLEX confirmed (input + output) <<<");
    }
    else {

        SDL_Log("Opening STEREO DUPLEX (2 channels in/out)...");

        inputParams.channelCount = 2;
        inputParams.sampleFormat = paFloat32;
        inputParams.suggestedLatency = deviceInfo->defaultLowInputLatency;
        inputParams.hostApiSpecificStreamInfo = NULL;

        outputParams.channelCount = 2;

        err = Pa_OpenStream(&paStream,
            &inputParams,
            &outputParams,
            sampleRate,
            320,
            paClipOff,
            paUnifiedCallback,
            NULL);

        if (err != paNoError) {
            SDL_Log("ERROR: Cannot open stereo duplex: %s", Pa_GetErrorText(err));
            Pa_Terminate();
            return false;
        }
    }

    err = Pa_StartStream(paStream);
    if (err != paNoError) {
        SDL_Log("Error Pa_StartStream: %s", Pa_GetErrorText(err));
        Pa_CloseStream(paStream);
        Pa_Terminate();
        return false;
    }

    SDL_Log(">>> PortAudio DUPLEX initialized successfully <<<");
    SDL_Log("  Input device: %s", deviceInfo->name);
    SDL_Log("  Output device: %s", outputDeviceInfo->name);
    SDL_Log("  Format: %s (%d channels)",
        paMicrophoneChannels == 2 ? "TRUE STEREO" : "MONO",
        paMicrophoneChannels);
    SDL_Log("  Sample rate: %.0f Hz", sampleRate);
    SDL_Log("  Latency: %zu samples (~%.1f ms)",
        STREAM_BUFFER_SIZE,
        (STREAM_BUFFER_SIZE * 1000.0f) / sampleRate);

    return true;
}

// Close playback and duplex streams before terminating PortAudio.
void CleanupPortAudio()
{
    // The retained recorded-audio shifters are not deleted by this inherited
    // cleanup path; their allocations remain until process termination.
    // Stop and close the duplex stream before closing each optional playback stream and
    // terminating PortAudio.
    if (paStream) {
        Pa_StopStream(paStream);
        Pa_CloseStream(paStream);
        paStream = nullptr;
    }

    if (paPlaybackStream) {
        Pa_StopStream(paPlaybackStream);
        Pa_CloseStream(paPlaybackStream);
        paPlaybackStream = nullptr;
    }

    if (paPlaybackAnonymizedStream) {
        Pa_StopStream(paPlaybackAnonymizedStream);
        Pa_CloseStream(paPlaybackAnonymizedStream);
        paPlaybackAnonymizedStream = nullptr;
    }

    if (paPlaybackAnonymizedFormantStream) {
        Pa_StopStream(paPlaybackAnonymizedFormantStream);
        Pa_CloseStream(paPlaybackAnonymizedFormantStream);
        paPlaybackAnonymizedFormantStream = nullptr;
    }

    if (paPlaybackAnonymizedPitchFormantStream) {
        Pa_StopStream(paPlaybackAnonymizedPitchFormantStream);
        Pa_CloseStream(paPlaybackAnonymizedPitchFormantStream);
        paPlaybackAnonymizedPitchFormantStream = nullptr;
    }

    Pa_Terminate();
}
