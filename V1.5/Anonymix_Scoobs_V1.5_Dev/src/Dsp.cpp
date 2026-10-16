// Offline PCM16 processing and FFT visualization. The visual FFT runs from the SDL frame loop.

#include "Dsp.h"

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

        if (paFftSamplesAvailable < PA_FFT_SIZE || paFftInputBuffer.empty()) {
            return;
        }

        localBuffer.resize(PA_FFT_SIZE);
        const size_t capacity = paFftInputBuffer.size();
        const size_t oldest = (paFftWritePos + capacity - PA_FFT_SIZE) % capacity;
        for (size_t i = 0; i < PA_FFT_SIZE; ++i)
            localBuffer[i] = paFftInputBuffer[(oldest + i) % capacity];
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

    }

    // The display FFT receives cleaned and windowed input. The live
    // microphone-to-speaker DSP does not use this transform.
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
        // Logarithmic spacing covers the broad audible range. Low bands can be
        // narrower than one FFT bin, so include at least one physical bin.
        float freqStart = PA_FFT_MIN_HZ * pow(PA_FFT_MAX_HZ / PA_FFT_MIN_HZ,
            static_cast<float>(bar) / PA_NUM_BARS);
        float freqEnd = PA_FFT_MIN_HZ * pow(PA_FFT_MAX_HZ / PA_FFT_MIN_HZ,
            static_cast<float>(bar + 1) / PA_NUM_BARS);

        size_t binStart = clamp(static_cast<size_t>(lround(freqStart / binWidth)),
            size_t{1}, usableBins - 1);
        size_t binEnd = min(usableBins, max(binStart + 1,
            static_cast<size_t>(ceil(freqEnd / binWidth))));

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
                float freqStart = PA_FFT_MIN_HZ * pow(PA_FFT_MAX_HZ / PA_FFT_MIN_HZ,
                    static_cast<float>(bar) / PA_NUM_BARS);
                float freqEnd = PA_FFT_MIN_HZ * pow(PA_FFT_MAX_HZ / PA_FFT_MIN_HZ,
                    static_cast<float>(bar + 1) / PA_NUM_BARS);
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

        // Follow new speech energy promptly. Decay more gently to keep the visual
        // contour legible between syllables without a half-second reaction lag.
        const float historyFactor = rawMagnitudes[bar] > spectralHistory[bar]
            ? 0.72f : 0.30f;
        spectralHistory[bar] += (rawMagnitudes[bar] - spectralHistory[bar]) * historyFactor;
    }

    for (size_t bar = 0; bar < PA_NUM_BARS; ++bar) {
        // Subtract each bar noise floor without allowing negative visible magnitudes.
        float cleanMag = Max(0.0f, spectralHistory[bar] - paAdaptiveNoiseFloor[bar]);
        paFftMagnitudes[bar] = cleanMag;
    }

    // Scale the display with its strongest current bar after noise filtering.
    float maxMagFinal = paFftMagnitudes.empty() ? 0.0f :
        *max_element(paFftMagnitudes.begin(), paFftMagnitudes.end());

    if (maxMagFinal > 0.00001f) {
        for (size_t i = 0; i < PA_NUM_BARS; ++i) {
            paFftMagnitudes[i] = powf(Min(1.0f,
                paFftMagnitudes[i] / maxMagFinal), 0.78f);
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
        const float factor = paFftMagnitudes[i] > paFftSmoothed[i]
            ? 0.82f : smoothingFactor;
        paFftSmoothed[i] += (paFftMagnitudes[i] - paFftSmoothed[i]) * factor;
    }
}

