// Small speech-onset-driven pitch and loudness gestures. The control adds expressive
// colouring without claiming to recognize words, intonation or speaker identity.
#pragma once
#include "Common.h"

struct ProsodyStep {
    float semitones = 0.0f;
    float gain = 1.0f;
};

class ProsodyContour {
    // A gesture lasts approximately 240 ms. The first part rises gently and the
    // remainder decays; it is restarted only by a new speech-energy onset.
    static constexpr unsigned gestureLength = static_cast<unsigned>(PA_SAMPLE_RATE * 0.24f);
    static constexpr unsigned refractoryLength = static_cast<unsigned>(PA_SAMPLE_RATE * 0.11f);
    float fastEnvelope = 0.0f;
    float slowEnvelope = 0.0f;
    unsigned gestureSample = gestureLength;
    unsigned refractorySamples = 0;

public:
    ProsodyStep Advance(float input, float sliderPercent) noexcept {
        const float energy = fabsf(input);
        fastEnvelope += (energy - fastEnvelope) / (PA_SAMPLE_RATE * 0.005f);
        slowEnvelope += (energy - slowEnvelope) / (PA_SAMPLE_RATE * 0.10f);
        if (refractorySamples != 0) --refractorySamples;
        if (refractorySamples == 0 && fastEnvelope > 0.018f &&
            fastEnvelope > slowEnvelope * 1.45f) {
            gestureSample = 0;
            refractorySamples = refractoryLength;
        }
        float contour = 0.0f;
        if (gestureSample < gestureLength) {
            const float progress = static_cast<float>(gestureSample++) / gestureLength;
            contour = progress < 0.30f ? progress / 0.30f : (1.0f - progress) / 0.70f;
        }
        const float amount = clamp(sliderPercent * 0.01f, -1.0f, 1.0f);
        return { amount * 1.5f * contour, 1.0f + amount * 0.12f * contour };
    }
};
