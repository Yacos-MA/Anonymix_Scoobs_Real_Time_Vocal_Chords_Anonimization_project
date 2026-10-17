// The same variable-delay voice transformation is used by the live stream, recorded playback
// and WAV export. Each path owns a separate processor instance. Despite the UI label, changing
// its read rate also shifts harmonics; this is not pitch-preserving spectral-envelope shifting.

#pragma once
#include "Dsp.h"

// Keep the live algorithm identical to the selected Borderless Aligned UI build.
class StreamFormantShifter {
private:
    // Preserve the inherited 16 ms delay, 6 ms fade and 1.5 ms safety margin.
    static constexpr size_t DELAY_SIZE = static_cast<size_t>(PA_SAMPLE_RATE * 0.016f);
    static constexpr size_t XFADE_SIZE = static_cast<size_t>(PA_SAMPLE_RATE * 0.006f);
    static constexpr float EDGE_MARGIN = PA_SAMPLE_RATE * 0.0015f;

    array<float, DELAY_SIZE> delayLine;
    array<float, XFADE_SIZE> xfadeBuffer;
    array<float, XFADE_SIZE> fadeOut;

    size_t writePos = 0;
    float readPos = 0.0f;
    size_t xfadePhase = 0;
    bool isCrossfading = false;
    float currentRatio = 1.0f;

public:
    // The read head starts halfway through the delay line. At a 1.0 ratio,
    // that is the fixed alignment offset for recording export.
    static constexpr size_t NOMINAL_DELAY = DELAY_SIZE / 2;
    static constexpr size_t MAX_DELAY = DELAY_SIZE;

    StreamFormantShifter() {
        delayLine.fill(0.0f);
        xfadeBuffer.fill(0.0f);
        // The fade phase is an integer, so its cosine never changes between resets.
        // Build weights when the processor is created, outside PortAudio callbacks.
        for (size_t i = 0; i < XFADE_SIZE; ++i) {
            const float t = static_cast<float>(i) / (XFADE_SIZE - 1);
            fadeOut[i] = 0.5f * (1.0f + cosf(3.14159265f * t));
        }
        readPos = DELAY_SIZE / 2.0f;

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

        static const float smoothing = RescaleLegacySmoothing(0.995f);
        currentRatio = smoothing * currentRatio + (1.0f - smoothing) * shiftRatio;

        float output;

        if (!isCrossfading) {

            output = ReadDelayInterpolated(readPos);

            readPos += currentRatio;
            if (readPos >= DELAY_SIZE) readPos -= DELAY_SIZE;

            float distance = static_cast<float>(writePos) - readPos;
            if (distance < 0.0f) distance += DELAY_SIZE;

            if ((distance < XFADE_SIZE + EDGE_MARGIN ||
                distance > DELAY_SIZE - XFADE_SIZE - EDGE_MARGIN)) {

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
                const float outWeight = fadeOut[xfadePhase];
                const float fadeIn = 1.0f - outWeight;

                float oldSample = xfadeBuffer[xfadePhase];
                float newSample = ReadDelayInterpolated(readPos);

                output = oldSample * outWeight + newSample * fadeIn;

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
