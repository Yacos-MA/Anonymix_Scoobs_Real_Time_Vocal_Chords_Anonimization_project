// Shared V1.2 timbre processor for live audio, recorded playback and WAV export.
#pragma once
#include "Common.h"

// Construct normalized Butterworth low-pass coefficients. The low-pass output
// is subtracted from the input to isolate higher frequencies. At slider zero,
// the final mix is exactly the input, even though the filter keeps its state up to date.
struct TimbreLowPassCoefficients {
    float b0, b1, b2, a1, a2;
};

inline TimbreLowPassCoefficients MakeTimbreLowPassCoefficients() {
    constexpr double cutoffHz = 1600.0;
    constexpr double butterworthQ = 0.7071067811865476;
    const double omega = 2.0 * numbers::pi_v<double> * cutoffHz / PA_SAMPLE_RATE;
    const double cosine = cos(omega);
    const double alpha = sin(omega) / (2.0 * butterworthQ);
    const double inverseA0 = 1.0 / (1.0 + alpha);

    return {
        static_cast<float>((1.0 - cosine) * 0.5 * inverseA0),
        static_cast<float>((1.0 - cosine) * inverseA0),
        static_cast<float>((1.0 - cosine) * 0.5 * inverseA0),
        static_cast<float>(-2.0 * cosine * inverseA0),
        static_cast<float>((1.0 - alpha) * inverseA0)
    };
}

// This experimental voice effect has one parameter: -100% dark, 0% neutral, +100% bright.
// A fixed IIR cutoff avoids repeated coefficient creation in the time-critical audio callback.
// Transposed Direct Form II stores two filter states without allocating per audio block.
class TimbreShaper final {
    const TimbreLowPassCoefficients coefficients = MakeTimbreLowPassCoefficients();
    const float amountSmoothing = exp(-1.0f / (0.020f * PA_SAMPLE_RATE));
    float smoothedAmount = 0.0f;

    float state1 = 0.0f;
    float state2 = 0.0f;

public:
    void ProcessBuffer(float* samples, size_t count, float sliderPercent) noexcept {
        const float boundedPercent = clamp(sliderPercent, -100.0f, 100.0f);
        const float targetAmount = boundedPercent * 0.006f;

        for (size_t i = 0; i < count; ++i) {
            smoothedAmount = amountSmoothing * smoothedAmount +
                (1.0f - amountSmoothing) * targetAmount;
            const float input = samples[i];
            const float lowFrequency = coefficients.b0 * input + state1;
            state1 = coefficients.b1 * input - coefficients.a1 * lowFrequency + state2;
            state2 = coefficients.b2 * input - coefficients.a2 * lowFrequency;
            samples[i] = input + smoothedAmount * (input - lowFrequency);
        }

        // Prevent almost-silent filter history from becoming expensive denormal floats.
        if (abs(state1) < 1.0e-20f) state1 = 0.0f;
        if (abs(state2) < 1.0e-20f) state2 = 0.0f;
    }
};

