// PortAudio capture and playback, with PCM16 processing for saved or played recordings. Callbacks may run on a separate thread.

#include "Audio.h"
#include "AudioDiagnostics.h"
#include "Dsp.h"
#include "Stream.h"
#include "Timbre.h"
#include "Wav.h"
#include <memory>

// The live callback records here; the SDL thread reads the same counters for its report.
AudioDiagnostics g_audioDiagnostics;

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

// A separate processor belongs to recorded playback; it must never share live filter history.
static unique_ptr<TimbreShaper> timbrePlaybackProcessor;

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

// Apply exactly the same filter as live stream mode, using a fresh processor for each saved take.
void ApplyRecordedTimbre(vector<Uint8>& pcm16, float sliderPercent) {
    TimbreShaper processor;
    array<float, STREAM_BUFFER_SIZE> block{};
    const size_t totalSamples = pcm16.size() / 2;
    for (size_t offset = 0; offset < totalSamples; offset += block.size()) {
        const size_t count = min(block.size(), totalSamples - offset);
        for (size_t i = 0; i < count; ++i)
            block[i] = ReadPcm16Sample(pcm16, offset + i) / 32768.0f;
        processor.ProcessBuffer(block.data(), count, sliderPercent);
        for (size_t i = 0; i < count; ++i)
            WritePcm16Sample(pcm16, offset + i, block[i]);
    }
}

bool SavePortAudioTimbreRecording() {
    vector<Uint8> snapshot;
    {
        lock_guard<mutex> recordingLock(paAudioBufferMutex);
        if (paIsRecording.load(memory_order_acquire)) {
            SDL_Log("Stop recording before exporting the timbre take");
            return false;
        }
        snapshot = paAudioBuffer;
    }
    if (snapshot.empty()) {
        SDL_Log("Cannot save timbre: record a voice sample first");
        return false;
    }
    const float setting = g_timbrePercent.load(memory_order_acquire);
    const float gain = g_outputVolume.load(memory_order_acquire);
    ApplyRecordedTimbre(snapshot, setting);
    for (size_t i = 0; i < snapshot.size() / 2; ++i)
        WritePcm16Sample(snapshot, i, ReadPcm16Sample(snapshot, i) / 32768.0f * gain);
    const string filename = GenerateWAVFilename("audio_timbre_mono");
    const bool saved = SaveWAV_Adaptive(filename, snapshot, 1, PA_SAMPLE_RATE);
    SDL_Log("Timbre WAV %s: %s (slider %.1f%%, volume %.1f%%)",
        saved ? "saved" : "failed", filename.c_str(), setting, gain * 100.0f);
    return saved;
}

// Timbre playback reads the captured take in blocks, keeping its IIR history and allowing the
// single timbre slider to change during playback. No allocations or log calls occur here.
static int paPlaybackTimbreCallback(const void*, void* outputBuffer,
    unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags,
    void*)
{
    float* out = static_cast<float*>(outputBuffer);
    if (out == nullptr) return paContinue;

    const size_t outputSamples = framesPerBuffer * static_cast<size_t>(paMicrophoneChannels);
    if (!paIsPlayingTimbre.load(memory_order_acquire) || !timbrePlaybackProcessor) {
        fill_n(out, outputSamples, 0.0f);
        return paContinue;
    }

    const size_t totalSamples = paProcessedTimbreBuffer.size() / 2;
    const float timbrePercent = g_timbrePercent.load(memory_order_acquire);
    const float volume = g_outputVolume.load(memory_order_acquire);
    array<float, STREAM_BUFFER_SIZE> block{};
    for (size_t offset = 0; offset < framesPerBuffer; offset += block.size()) {
        const size_t count = min(block.size(), static_cast<size_t>(framesPerBuffer) - offset);
        for (size_t i = 0; i < count; ++i) {
            block[i] = paPlaybackTimbrePosition < totalSamples
                ? ReadPcm16Sample(paProcessedTimbreBuffer, paPlaybackTimbrePosition++) / 32768.0f
                : 0.0f;
        }
        timbrePlaybackProcessor->ProcessBuffer(block.data(), count, timbrePercent);
        for (size_t i = 0; i < count; ++i) {
            const float sample = clamp(block[i] * volume, -1.0f, 1.0f);
            for (int channel = 0; channel < paMicrophoneChannels; ++channel)
                out[(offset + i) * paMicrophoneChannels + channel] = sample;
        }
    }
    if (paPlaybackTimbrePosition >= totalSamples)
        paIsPlayingTimbre.store(false, memory_order_release);
    return paContinue;
}

// Stateful window and overlap-add processor used for recorded playback and export.
struct RealtimeFormantShifter {
    // At 48 kHz the inherited 512-sample recorded-playback window is about 10.7 ms.
    // Its 128-sample hop preserves the original 75% overlap and playback callback cadence.
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
        SDL_Log("  Frame: %zu samples (%.1f ms)", FRAME_SIZE, (FRAME_SIZE * 1000.0f) / PA_SAMPLE_RATE);
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
                nextSegment.resize(CROSSFADE_SIZE);
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
    static const float hp_alpha = RescaleLegacySmoothing(0.950f);

    static float rms_squared = 0.0f;
    static const float rms_alpha = RescaleLegacySmoothing(0.8825f);

    static float noise_floor = 0.0f;
    static const float noise_smoothing = RescaleLegacySmoothing(0.9995f);
    static bool noise_initialized = false;
    static uint64_t calibration_samples = 0;

    constexpr float expander_threshold = 0.015f;
    constexpr float expander_ratio = 2.0f;
    constexpr float expander_attack = 0.001f;
    constexpr float expander_release = 0.050f;

    static float expander_envelope = 1.0f;

    if (!noise_initialized) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            float sample = fabs(in[i]);
            noise_floor = max(noise_floor, sample);
        }

        calibration_samples += framesPerBuffer;

        if (calibration_samples >= static_cast<uint64_t>(PA_SAMPLE_RATE * 0.6f)) {
            noise_floor *= 1.8f;
            noise_initialized = true;
            SDL_Log(">>> Noise floor calibrated after approximately 0.6 seconds: %.6f <<<", noise_floor);
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

    // PortAudio may supply no output pointer on an abnormal callback; never dereference it.
    if (out == nullptr) return paContinue;

    // When stopped or empty, fill the entire requested output block with silence.
    if (!paIsPlaying || paPlaybackOriginalBuffer.empty()) {
        for (unsigned long i = 0; i < framesPerBuffer * paMicrophoneChannels; ++i) {
            out[i] = 0.0f;
        }
        return paContinue;
    }

    const size_t totalSamples = paPlaybackOriginalBuffer.size() / sizeof(int16_t);

    // The UI updates the atomic volume setting; copy it once per block before writing the samples.
    float volume = g_outputVolume.load(memory_order_acquire);

    for (unsigned long frame = 0; frame < framesPerBuffer; ++frame) {
        if (paPlaybackPosition < totalSamples) {

            float sample = static_cast<float>(ReadPcm16Sample(paPlaybackOriginalBuffer,
                paPlaybackPosition)) / 32767.0f;

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

    if (out == nullptr) return paContinue;

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

    if (out == nullptr) return paContinue;

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

// Open or start the formant-only recorded playback variant.
bool StartPortAudioPlaybackAnonymizedFormant()
{
    if (!IsPortAudioDeviceWorkerThread())
        return RunPortAudioDeviceAction(StartPortAudioPlaybackAnonymizedFormant);
    SDL_Log("=== StartPortAudioPlaybackAnonymizedFormant called ===");

    if (paIsPlayingAnonymizedFormant) {
        SDL_Log("Cannot play: formant playback already active");
        return false;
    }

    StopPortAudioPlaybackAnonymizedFormant();
    {
        lock_guard<mutex> recordingLock(paAudioBufferMutex);
        if (paIsRecording.load(memory_order_acquire)) {
            SDL_Log("Stop recording before playing formant audio");
            return false;
        }
        paProcessedFormantBuffer = paAudioBuffer;
    }
    if (paProcessedFormantBuffer.empty()) {
        SDL_Log("Cannot play: PortAudio buffer is empty");
        return false;
    }

    // Keep original capture bytes for later playback and WAV saves; processing occurs inside the
    // callback.
    SDL_Log("Buffer copied: %zu bytes (real-time DSP in callback)", paProcessedFormantBuffer.size());

    // Always query the current default output and create a new stream. A retained stream
    // remains bound to the device selected when it was first opened.
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
        if (deviceInfo == nullptr) return false;
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

        paPlaybackAnonymizedFormantPosition = 0;
        paIsPlayingAnonymizedFormant = true;
        err = Pa_StartStream(paPlaybackAnonymizedFormantStream);
        if (err != paNoError) {
            paIsPlayingAnonymizedFormant = false;
            SDL_Log("Error Pa_StartStream playback formant: %s", Pa_GetErrorText(err));
            Pa_CloseStream(paPlaybackAnonymizedFormantStream);
            paPlaybackAnonymizedFormantStream = nullptr;
            return false;
        }
    }

    SDL_Log(">>> FORMANT SHIFT playback started (real-time DSP with sliders) <<<");
    return true;
}

void StopPortAudioPlaybackAnonymizedFormant()
{
    if (!IsPortAudioDeviceWorkerThread()) {
        RunPortAudioDeviceAction(StopPortAudioPlaybackAnonymizedFormant);
        return;
    }
    paIsPlayingAnonymizedFormant = false;
    CloseRecordedPlayback(paPlaybackAnonymizedFormantStream, "Formant playback");
    paPlaybackAnonymizedFormantPosition = 0;

    SDL_Log("Formant playback stopped");
}

// Open or start the combined pitch/formant playback variant.
bool StartPortAudioPlaybackAnonymizedPitchFormant()
{
    if (!IsPortAudioDeviceWorkerThread())
        return RunPortAudioDeviceAction(StartPortAudioPlaybackAnonymizedPitchFormant);
    SDL_Log("=== StartPortAudioPlaybackAnonymizedPitchFormant called ===");

    if (paIsPlayingAnonymizedPitchFormant) {
        SDL_Log("Cannot play: pitch and formant playback already active");
        return false;
    }

    StopPortAudioPlaybackAnonymizedPitchFormant();
    {
        lock_guard<mutex> recordingLock(paAudioBufferMutex);
        if (paIsRecording.load(memory_order_acquire)) {
            SDL_Log("Stop recording before playing pitch and formant audio");
            return false;
        }
        paProcessedPitchFormantBuffer = paAudioBuffer;
    }
    if (paProcessedPitchFormantBuffer.empty()) {
        SDL_Log("Cannot play: PortAudio buffer is empty");
        return false;
    }

    // The combined variant has its own byte copy and playback position.
    SDL_Log("Buffer copied: %zu bytes (real-time DSP in callback)", paProcessedPitchFormantBuffer.size());

    if (paPlaybackAnonymizedPitchFormantStream == nullptr) {
        PaStreamParameters outputParams;
        outputParams.device = Pa_GetDefaultOutputDevice();

        if (outputParams.device == paNoDevice) {
            SDL_Log("No output device found");
            return false;
        }

        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParams.device);
        if (deviceInfo == nullptr) return false;
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

        paPlaybackAnonymizedPitchFormantPosition = 0;
        paIsPlayingAnonymizedPitchFormant = true;
        err = Pa_StartStream(paPlaybackAnonymizedPitchFormantStream);
        if (err != paNoError) {
            paIsPlayingAnonymizedPitchFormant = false;
            SDL_Log("Error Pa_StartStream playback Pitch+Formant: %s", Pa_GetErrorText(err));
            Pa_CloseStream(paPlaybackAnonymizedPitchFormantStream);
            paPlaybackAnonymizedPitchFormantStream = nullptr;
            return false;
        }
    }

    SDL_Log(">>> COMBINED PITCH+FORMANT playback started (real-time DSP with sliders) <<<");
    return true;
}

void StopPortAudioPlaybackAnonymizedPitchFormant()
{
    if (!IsPortAudioDeviceWorkerThread()) {
        RunPortAudioDeviceAction(StopPortAudioPlaybackAnonymizedPitchFormant);
        return;
    }
    paIsPlayingAnonymizedPitchFormant = false;
    CloseRecordedPlayback(paPlaybackAnonymizedPitchFormantStream, "Pitch and formant playback");
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
    {
        lock_guard<mutex> recordingLock(paAudioBufferMutex);
        paAudioBuffer.clear();
        paAudioBuffer.shrink_to_fit();
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

// Open or start playback of the captured original audio.
bool StartPortAudioPlayback()
{
    if (!IsPortAudioDeviceWorkerThread())
        return RunPortAudioDeviceAction(StartPortAudioPlayback);
    SDL_Log("=== StartPortAudioPlayback called ===");

    if (paIsPlaying) {
        SDL_Log("Cannot play: PortAudio playback already active");
        return false;
    }

    StopPortAudioPlayback();
    {
        lock_guard<mutex> recordingLock(paAudioBufferMutex);
        if (paIsRecording.load(memory_order_acquire)) {
            SDL_Log("Stop recording before playing the original audio");
            return false;
        }
        paPlaybackOriginalBuffer = paAudioBuffer;
    }
    if (paPlaybackOriginalBuffer.empty()) {
        SDL_Log("Cannot play: PortAudio buffer is empty");
        return false;
    }

    SDL_Log("PortAudio buffer contains %zu bytes (%zu samples, %d channels)",
        paPlaybackOriginalBuffer.size(),
        paPlaybackOriginalBuffer.size() / sizeof(int16_t),
        paMicrophoneChannels);

    // Reopening for each take uses the current default output, matching Play Timbre.
    if (paPlaybackStream == nullptr) {
        PaStreamParameters outputParams;
        outputParams.device = Pa_GetDefaultOutputDevice();

        if (outputParams.device == paNoDevice) {
            SDL_Log("No output device found");
            return false;
        }

        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParams.device);
        if (deviceInfo == nullptr) return false;
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

        paPlaybackPosition = 0;
        paIsPlaying = true;
        err = Pa_StartStream(paPlaybackStream);
        if (err != paNoError) {
            paIsPlaying = false;
            SDL_Log("Error Pa_StartStream playback: %s", Pa_GetErrorText(err));
            Pa_CloseStream(paPlaybackStream);
            paPlaybackStream = nullptr;
            return false;
        }

        SDL_Log("Stream PortAudio playback created successfully (%d channels)", paMicrophoneChannels);
    }

    SDL_Log(">>> PortAudio playback STARTED (volume controlled by slider) <<<");
    return true;
}

void StopPortAudioPlayback()
{
    if (!IsPortAudioDeviceWorkerThread()) {
        RunPortAudioDeviceAction(StopPortAudioPlayback);
        return;
    }
    paIsPlaying = false;
    CloseRecordedPlayback(paPlaybackStream, "Original playback");
    paPlaybackPosition = 0;

    SDL_Log("PortAudio playback stopped");
}

// Open or start slider-controlled anonymized playback.
bool StartPortAudioPlaybackAnonymized()
{
    if (!IsPortAudioDeviceWorkerThread())
        return RunPortAudioDeviceAction(StartPortAudioPlaybackAnonymized);
    SDL_Log("=== StartPortAudioPlaybackAnonymized called ===");

    if (paIsPlayingAnonymized) {
        SDL_Log("Cannot play: anonymized playback already active");
        return false;
    }

    StopPortAudioPlaybackAnonymized();
    {
        lock_guard<mutex> recordingLock(paAudioBufferMutex);
        if (paIsRecording.load(memory_order_acquire)) {
            SDL_Log("Stop recording before playing pitch audio");
            return false;
        }
        paProcessedBuffer = paAudioBuffer;
    }
    if (paProcessedBuffer.empty()) {
        SDL_Log("Cannot play: PortAudio buffer is empty");
        return false;
    }

    // Pitch playback uses a byte copy; the slider-controlled processor changes samples during the
    // playback callback.
    SDL_Log("Buffer copied: %zu bytes (real-time DSP in callback)", paProcessedBuffer.size());

    if (paPlaybackAnonymizedStream == nullptr) {
        PaStreamParameters outputParams;
        outputParams.device = Pa_GetDefaultOutputDevice();

        if (outputParams.device == paNoDevice) {
            SDL_Log("No output device found");
            return false;
        }

        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParams.device);
        if (deviceInfo == nullptr) return false;
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

        paPlaybackAnonymizedPosition = 0;
        paIsPlayingAnonymized = true;
        err = Pa_StartStream(paPlaybackAnonymizedStream);
        if (err != paNoError) {
            paIsPlayingAnonymized = false;
            SDL_Log("Error Pa_StartStream anonymized playback: %s", Pa_GetErrorText(err));
            Pa_CloseStream(paPlaybackAnonymizedStream);
            paPlaybackAnonymizedStream = nullptr;
            return false;
        }

        SDL_Log("Stream PortAudio anonymized playback created successfully");
    }

    SDL_Log(">>> Anonymized PortAudio playback STARTED (real-time DSP with sliders) <<<");
    return true;
}

void StopPortAudioPlaybackAnonymized()
{
    if (!IsPortAudioDeviceWorkerThread()) {
        RunPortAudioDeviceAction(StopPortAudioPlaybackAnonymized);
        return;
    }
    paIsPlayingAnonymized = false;
    CloseRecordedPlayback(paPlaybackAnonymizedStream, "Pitch playback");
    paPlaybackAnonymizedPosition = 0;

    SDL_Log("Anonymized PortAudio playback stopped");
}

bool StartPortAudioPlaybackTimbre()
{
    if (!IsPortAudioDeviceWorkerThread())
        return RunPortAudioDeviceAction(StartPortAudioPlaybackTimbre);
    if (paIsRecording.load(memory_order_acquire)) {
        SDL_Log("Stop recording before starting the timbre preview");
        return false;
    }
    if (paIsPlayingTimbre.load(memory_order_acquire)) return false;

    // Close an already-completed preview before replacing its captured bytes and filter state.
    // Pa_CloseStream joins the playback callback before either object is destroyed.
    if (paPlaybackTimbreStream != nullptr) {
        CloseRecordedPlayback(paPlaybackTimbreStream, "Timbre playback");
    }
    timbrePlaybackProcessor.reset();
    {
        lock_guard<mutex> recordingLock(paAudioBufferMutex);
        paProcessedTimbreBuffer = paAudioBuffer;
    }
    if (paProcessedTimbreBuffer.empty()) {
        SDL_Log("Cannot preview timbre: record a voice sample first");
        return false;
    }

    const PaDeviceIndex outputDevice = Pa_GetDefaultOutputDevice();
    const PaDeviceInfo* outputInfo = outputDevice != paNoDevice
        ? Pa_GetDeviceInfo(outputDevice) : nullptr;
    if (outputInfo == nullptr) {
        SDL_Log("Cannot preview timbre: no output device is available");
        return false;
    }
    PaStreamParameters outputParams{};
    outputParams.device = outputDevice;
    outputParams.channelCount = paMicrophoneChannels;
    outputParams.sampleFormat = paFloat32;
    outputParams.suggestedLatency = outputInfo->defaultLowOutputLatency;

    // Construct the timbre filter before starting the PortAudio callback.
    timbrePlaybackProcessor = make_unique<TimbreShaper>();
    const PaError openResult = Pa_OpenStream(&paPlaybackTimbreStream, nullptr,
        &outputParams, PA_SAMPLE_RATE, paFramesPerBufferUnspecified, paClipOff,
        paPlaybackTimbreCallback, nullptr);
    if (openResult != paNoError) {
        SDL_Log("Timbre playback cannot open output: %s", Pa_GetErrorText(openResult));
        timbrePlaybackProcessor.reset();
        paPlaybackTimbreStream = nullptr;
        return false;
    }

    paPlaybackTimbrePosition = 0;
    paIsPlayingTimbre.store(true, memory_order_release);
    const PaError startResult = Pa_StartStream(paPlaybackTimbreStream);
    if (startResult != paNoError) {
        paIsPlayingTimbre.store(false, memory_order_release);
        SDL_Log("Timbre playback cannot start: %s", Pa_GetErrorText(startResult));
        Pa_CloseStream(paPlaybackTimbreStream);
        paPlaybackTimbreStream = nullptr;
        timbrePlaybackProcessor.reset();
        return false;
    }
    SDL_Log("Timbre playback started (%zu recorded frames; live slider)",
        paProcessedTimbreBuffer.size() / 2);
    return true;
}

void StopPortAudioPlaybackTimbre()
{
    if (!IsPortAudioDeviceWorkerThread()) {
        RunPortAudioDeviceAction(StopPortAudioPlaybackTimbre);
        return;
    }
    paIsPlayingTimbre.store(false, memory_order_release);
    if (paPlaybackTimbreStream != nullptr) {
        CloseRecordedPlayback(paPlaybackTimbreStream, "Timbre playback");
    }
    timbrePlaybackProcessor.reset();
    paPlaybackTimbrePosition = 0;
    SDL_Log("Timbre playback stopped");
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
    // The retained recorded-audio shifters are not deleted by this inherited
    // cleanup path; their allocations remain until process termination.
    // Stop and close the duplex stream before closing each optional playback stream and
    // terminating PortAudio.
    StopPortAudioPlaybackTimbre();

    CloseRecordedPlayback(paStream, "Live duplex audio");
    StopPortAudioPlayback();
    StopPortAudioPlaybackAnonymized();
    StopPortAudioPlaybackAnonymizedFormant();
    StopPortAudioPlaybackAnonymizedPitchFormant();

    Pa_Terminate();
}
