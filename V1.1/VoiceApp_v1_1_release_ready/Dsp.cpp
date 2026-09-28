// Offline PCM16 processing and FFT visualization. The visual FFT runs from the SDL frame loop.

#include "Dsp.h"

static double RandomUnitInterval();

// Find the largest display magnitude; an empty vector returns zero.
inline float FindMax(const vector<float>& data) noexcept {
    if (data.empty()) return 0.0f;

    float maxVal = data[0];
    for (size_t i = 1; i < data.size(); ++i) {
        if (data[i] > maxVal) {
            maxVal = data[i];
        }
    }
    return maxVal;
}

// Scale signed 16-bit audio samples in place toward the existing 70% target peak.
void NormalizeAudio(vector<Uint8>& audioData)
{
    if (audioData.empty()) return;

    // Interpret the byte vector as signed 16-bit samples. Callers must provide an even number of
    // bytes.
    int16_t* samples = reinterpret_cast<int16_t*>(audioData.data());
    size_t numSamples = audioData.size() / sizeof(int16_t);

    SDL_Log(">>> START NormalizeAudio <<<");
    SDL_Log("  Samples to normalize: %zu", numSamples);

    int16_t maxSample = 0;
    int zeroCount = 0;
    int invalidCount = 0;

    // The inherited int16_t temporary cannot represent abs(-32768); the
    // 'invalidCount' check below also cannot validate values already stored in int16_t.
    for (size_t i = 0; i < numSamples; ++i) {
        int16_t absSample = abs(samples[i]);
        if (absSample > maxSample) {
            maxSample = absSample;
        }
        if (samples[i] == 0) zeroCount++;

        if (absSample > 32767 || absSample < -32768) {
            invalidCount++;
        }
    }

    SDL_Log("  Peak before normalization: %d / 32767", maxSample);
    SDL_Log("  Zeros: %d (%.1f%%) | Invalid values: %d",
        zeroCount, (zeroCount * 100.0f) / numSamples, invalidCount);

    // A silent take has no peak from which to compute a useful normalization scale.
    if (maxSample == 0) {
        SDL_Log("  WARNING: Completely silent signal!");
        return;
    }

    if (invalidCount > 0) {
        SDL_Log("  CRITICAL ERROR: %d out-of-range values detected!", invalidCount);
    }

    // Use 70 percent of the signed positive PCM peak as the inherited output target.
    float scale = (32767.0f * 0.7f) / maxSample;
    SDL_Log("  Normalization factor: %.4f", scale);

    for (size_t i = 0; i < numSamples; ++i) {
        float sample = static_cast<float>(samples[i]) * scale;

        if (sample > 32767.0f) sample = 32767.0f;
        if (sample < -32768.0f) sample = -32768.0f;

        samples[i] = static_cast<int16_t>(sample);
    }

    SDL_Log(">>> END NormalizeAudio <<<");
}

// Apply the existing simple high-pass stage to packed PCM16 samples.
void ApplyHighPassFilter(vector<Uint8>& audioData, float cutoffFreq)
{
    if (audioData.empty()) return;

    int16_t* samples = reinterpret_cast<int16_t*>(audioData.data());
    size_t numSamples = audioData.size() / sizeof(int16_t);

    SDL_Log(">>> START ApplyHighPassFilter (%.0f Hz) <<<", cutoffFreq);

    vector<float> inputFloat(numSamples);
    for (size_t i = 0; i < numSamples; ++i) {
        inputFloat[i] = static_cast<float>(samples[i]);
    }

    const float sampleRate = PA_SAMPLE_RATE;
    // Compute a second-order high-pass biquad at the requested cutoff and normalize its
    // coefficients by a0.
    const float omega = 2.0f * 3.14159265f * cutoffFreq / sampleRate;
    const float cosOmega = cos(omega);
    const float sinOmega = sin(omega);
    const float alpha = sinOmega / (2.0f * 0.707f);

    const float b0 = (1.0f + cosOmega) / 2.0f;
    const float b1 = -(1.0f + cosOmega);
    const float b2 = (1.0f + cosOmega) / 2.0f;
    const float a0 = 1.0f + alpha;
    const float a1 = -2.0f * cosOmega;
    const float a2 = 1.0f - alpha;

    const float b0n = b0 / a0;
    const float b1n = b1 / a0;
    const float b2n = b2 / a0;
    const float a1n = a1 / a0;
    const float a2n = a2 / a0;

    float x1 = 0.0f, x2 = 0.0f;
    float y1 = 0.0f, y2 = 0.0f;

    for (size_t i = 0; i < numSamples; ++i) {
        float x0 = inputFloat[i];
        float y0 = b0n * x0 + b1n * x1 + b2n * x2 - a1n * y1 - a2n * y2;

        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = y0;

        if (y0 > 32767.0f) y0 = 32767.0f;
        if (y0 < -32768.0f) y0 = -32768.0f;

        samples[i] = static_cast<int16_t>(round(y0));
    }

    SDL_Log(">>> END ApplyHighPassFilter <<<");
}

// Perturb samples while retaining the original behavior for silence and sample limits.
void ApplyJitter(vector<Uint8>& audioData, float amount)
{
    if (audioData.empty() || amount <= 0.0f) return;

    int16_t* samples = reinterpret_cast<int16_t*>(audioData.data());
    size_t numSamples = audioData.size() / sizeof(int16_t);

    SDL_Log(">>> START ApplyJitter (%.3f) <<<", amount);

    float globalRMS = 0.0f;
    for (size_t i = 0; i < numSamples; ++i) {
        float sample = static_cast<float>(samples[i]);
        globalRMS += sample * sample;
    }
    globalRMS = sqrt(globalRMS / numSamples);

    float silenceThreshold = globalRMS * 0.05f;

    SDL_Log("  RMS global: %.1f | Silence threshold: %.1f", globalRMS, silenceThreshold);

    // Keep a random generator across jitter calls; the amount parameter controls the random sample
    // modulation.
    static mt19937 rng(random_device{}());
    uniform_real_distribution<float> dist(-amount, amount);

    int samplesModified = 0;
    int samplesSilenced = 0;

    for (size_t i = 0; i < numSamples; ++i) {
        float sample = static_cast<float>(samples[i]);
        float absValue = fabs(sample);

        if (absValue > silenceThreshold) {

            float jitterAmount = dist(rng) * (absValue / 32767.0f);
            float jitteredSample = sample * (1.0f + jitterAmount);

            if (jitteredSample > 32767.0f) jitteredSample = 32767.0f;
            if (jitteredSample < -32768.0f) jitteredSample = -32768.0f;

            samples[i] = static_cast<int16_t>(round(jitteredSample));
            samplesModified++;
        }
        else {

            samplesSilenced++;
        }
    }

    SDL_Log("  Modified samples: %d | Preserved silent samples: %d (%.1f%%)",
        samplesModified, samplesSilenced, (samplesSilenced * 100.0f) / numSamples);

    SDL_Log(">>> END ApplyJitter <<<");
}

// Permute FFT input indices before the in-place radix-2 butterfly passes.
void BitReversalPermutation(vector<complex<float>>& data)
{
    // Bit reversal reorders input so iterative butterfly passes can operate in place.
    size_t n = data.size();
    size_t j = 0;

    for (size_t i = 0; i < n - 1; ++i) {
        if (i < j) {
            Swap(data[i], data[j]);
        }

        size_t k = n / 2;
        while (k <= j) {
            j -= k;
            k /= 2;
        }
        j += k;
    }
}

// Compute a radix-2 FFT; the input length must be a power of two.
void FFT_CooleyTukey(vector<complex<float>>& data)
{
    size_t n = data.size();

    // Radix-2 butterflies require at least two values and a power-of-two vector length.
    if (n < 2 || (n & (n - 1)) != 0) {
        SDL_Log("FFT ERROR: The size must be a power of two");
        return;
    }

    BitReversalPermutation(data);

    size_t log2n = 0;
    size_t temp = n;
    while (temp > 1) {
        temp >>= 1;
        log2n++;
    }

    // Combine progressively larger groups, starting with adjacent complex values.
    for (size_t s = 1; s <= log2n; ++s) {
        size_t m = 1 << s;
        size_t m2 = m / 2;

        complex<float> w(1.0f, 0.0f);
        complex<float> wm = polar(1.0f, static_cast<float>(-2.0 * numbers::pi / m));

        for (size_t j = 0; j < m2; ++j) {
            for (size_t k = j; k < n; k += m) {
                complex<float> t = w * data[k + m2];
                complex<float> u = data[k];

                data[k] = u + t;
                data[k + m2] = u - t;
            }
            w *= wm;
        }
    }
}

// Reduce FFT edge discontinuity by weighting each sample with a Hann window.
void ApplyHannWindow(vector<float>& data)
{
    size_t n = data.size();
    for (size_t i = 0; i < n; ++i) {
        // Taper the endpoints; this reduces leakage from discontinuities at FFT window boundaries.
        float window = 0.5f * (1.0f - cos(2.0f * numbers::pi * i / (n - 1)));
        data[i] *= window;
    }
}

// Copy recent captured samples under paFftMutex, calculate bins and update the visible magnitudes.
void ComputeFFT_PortAudio()
{

    vector<float> localBuffer;
    {
        // Hold the lock only for the snapshot; the FFT work uses local data.
        lock_guard<mutex> lock(paFftMutex);

        if (paFftInputBuffer.size() < PA_FFT_SIZE) {
            return;
        }

        localBuffer.assign(paFftInputBuffer.end() - PA_FFT_SIZE, paFftInputBuffer.end());
    }

    {

        float meanEnergy = 0.0f;
        for (size_t i = 0; i < localBuffer.size(); ++i) {
            meanEnergy += localBuffer[i] * localBuffer[i];
        }
        meanEnergy /= localBuffer.size();

        float stdDev = 0.0f;
        for (size_t i = 0; i < localBuffer.size(); ++i) {
            float diff = localBuffer[i] * localBuffer[i] - meanEnergy;
            stdDev += diff * diff;
        }
        stdDev = sqrt(stdDev / localBuffer.size());

        // Isolated high-energy samples are replaced with their neighbors
        // before windowing so a click does not dominate the visible spectrum.
        float impulseThreshold = meanEnergy + 4.0f * stdDev;

        // This retained copy is assigned below but never consulted by the
        // current impulse filter. Do not infer temporal comparison from its name.
        static vector<float> previousBuffer(PA_FFT_SIZE, 0.0f);

        for (size_t i = 1; i < localBuffer.size() - 1; ++i) {
            float sampleEnergy = localBuffer[i] * localBuffer[i];

            if (sampleEnergy > impulseThreshold) {

                float leftEnergy = localBuffer[i - 1] * localBuffer[i - 1];
                float rightEnergy = localBuffer[i + 1] * localBuffer[i + 1];
                float avgNeighborEnergy = (leftEnergy + rightEnergy) / 2.0f;

                if (avgNeighborEnergy < impulseThreshold * 0.25f) {

                    localBuffer[i] = (localBuffer[i - 1] + localBuffer[i + 1]) / 2.0f;

                    static int impulseCounter = 0;
                    if (impulseCounter++ % 100 == 0) {
                        SDL_Log(">>> TIME-DOMAIN IMPULSE removed at i=%zu <<<", i);
                    }
                }
            }
        }

        previousBuffer = localBuffer;
    }

    // Window after optional impulse removal, then convert real samples to complex input for
    // FFT_CooleyTukey.
    ApplyHannWindow(localBuffer);

    vector<complex<float>> complexData(PA_FFT_SIZE);
    for (size_t i = 0; i < PA_FFT_SIZE; ++i) {
        complexData[i] = complex<float>(localBuffer[i], 0.0f);
    }

    FFT_CooleyTukey(complexData);

    size_t usableBins = PA_FFT_SIZE / 2;
    float binWidth = PA_SAMPLE_RATE / PA_FFT_SIZE;

    vector<float> rawMagnitudes(PA_NUM_BARS, 0.0f);

    for (size_t bar = 0; bar < PA_NUM_BARS; ++bar) {
        // Use logarithmically spaced frequency ranges for the vocal display;
        // average each range's FFT magnitudes into one visible bar.
        float freqStart = 80.0f * pow(4000.0f / 80.0f, static_cast<float>(bar) / PA_NUM_BARS);
        float freqEnd = 80.0f * pow(4000.0f / 80.0f, static_cast<float>(bar + 1) / PA_NUM_BARS);

        size_t binStart = static_cast<size_t>(freqStart / binWidth);
        size_t binEnd = static_cast<size_t>(freqEnd / binWidth);
        binStart = Min(binStart, usableBins - 1);
        binEnd = Min(binEnd, usableBins);

        float sum = 0.0f;
        size_t count = 0;

        for (size_t bin = binStart; bin < binEnd; ++bin) {
            sum += abs(complexData[bin]);
            count++;
        }

        rawMagnitudes[bar] = (count > 0) ? (sum / count) : 0.0f;
    }

    // Accumulate per-bar baseline magnitudes during the startup calibration window.
    if (paIsCalibrating) {
        for (size_t bar = 0; bar < PA_NUM_BARS; ++bar) {
            paAdaptiveNoiseFloor[bar] = Max(paAdaptiveNoiseFloor[bar], rawMagnitudes[bar]);
        }

        paCalibrationFrames++;

        if (paCalibrationFrames % 10 == 0) {
            int progress = (paCalibrationFrames * 100) / PA_CALIBRATION_FRAMES;
            SDL_Log("PortAudio noise calibration: %d%% (%d/%d frames)",
                progress, paCalibrationFrames, PA_CALIBRATION_FRAMES);
        }

        if (paCalibrationFrames >= PA_CALIBRATION_FRAMES) {
            paIsCalibrating = false;

            for (size_t bar = 0; bar < PA_NUM_BARS; ++bar) {
                float freqStart = 80.0f * pow(4000.0f / 80.0f, static_cast<float>(bar) / PA_NUM_BARS);
                float freqEnd = 80.0f * pow(4000.0f / 80.0f, static_cast<float>(bar + 1) / PA_NUM_BARS);
                float barCenterFreq = (freqStart + freqEnd) / 2.0f;

                if (barCenterFreq < 200.0f) {
                    paAdaptiveNoiseFloor[bar] *= 1.6f;
                }
                else if (barCenterFreq < 1000.0f) {
                    paAdaptiveNoiseFloor[bar] *= 1.4f;
                }
                else {
                    paAdaptiveNoiseFloor[bar] *= 1.3f;
                }
            }

            float avgNoise = 0.0f;
            float maxNoise = 0.0f;
            for (size_t bar = 0; bar < PA_NUM_BARS; ++bar) {
                avgNoise += paAdaptiveNoiseFloor[bar];
                maxNoise = Max(maxNoise, paAdaptiveNoiseFloor[bar]);
            }
            avgNoise /= PA_NUM_BARS;

            SDL_Log("=== PortAudio CALIBRATION COMPLETE (WITH BOOST) ===");
            SDL_Log("Adjusted average noise: %.4f | Maximum noise: %.4f", avgNoise, maxNoise);
        }

        for (size_t i = 0; i < PA_NUM_BARS; ++i) {
            paFftMagnitudes[i] = rawMagnitudes[i];
        }

        paFftDataReady = true;
        return;
    }

    // Retain prior spectrum energy for the adaptive display noise subtraction.
    static vector<float> spectralHistory(PA_NUM_BARS, 0.0f);

    for (size_t bar = 0; bar < PA_NUM_BARS; ++bar) {

        spectralHistory[bar] = spectralHistory[bar] * 0.85f + rawMagnitudes[bar] * 0.15f;
    }

    for (size_t bar = 0; bar < PA_NUM_BARS; ++bar) {
        // Subtract each bar noise floor without allowing negative visible magnitudes.
        float cleanMag = Max(0.0f, spectralHistory[bar] - paAdaptiveNoiseFloor[bar]);
        paFftMagnitudes[bar] = cleanMag;
    }

    // Scale the display with its strongest current bar after noise filtering.
    float maxMagFinal = FindMax(paFftMagnitudes);

    if (maxMagFinal > 0.00001f) {
        for (size_t i = 0; i < PA_NUM_BARS; ++i) {
            paFftMagnitudes[i] = Min(1.0f, paFftMagnitudes[i] / maxMagFinal);
        }
    }

    paFftDataReady = true;

    static int paLogCounter = 0;
    if (paLogCounter % 120 == 0) {
        float totalEnergy = 0.0f;
        for (size_t i = 0; i < PA_NUM_BARS; ++i) {
            totalEnergy += paFftMagnitudes[i];
        }
        SDL_Log("PA FFT | Energy: %.2f | Max: %.2f", totalEnergy, maxMagFinal);
    }
    paLogCounter++;
}

// Blend current FFT bars with earlier values to reduce visual flicker.
// Blend each computed magnitude into the persistent display bars at the supplied smoothing factor.
void SmoothFFTDisplay_PortAudio(float smoothingFactor)
{
    for (size_t i = 0; i < PA_NUM_BARS; ++i) {
        paFftSmoothed[i] = paFftSmoothed[i] * (1.0f - smoothingFactor) + paFftMagnitudes[i] * smoothingFactor;
    }
}

// Legacy random helper retained in the original module; inspect call sites before removing it.
static double RandomUnitInterval()
{
    static mt19937_64 rng(random_device{}());
    static uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng);
}
