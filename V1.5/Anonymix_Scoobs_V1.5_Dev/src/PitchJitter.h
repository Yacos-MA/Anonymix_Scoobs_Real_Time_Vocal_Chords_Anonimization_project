// Bounded, allocation-free pitch instability for live output, previews and WAV export.
// This creative voice effect is not a measurement of vocal-fold cycle jitter.

#pragma once
#include <algorithm>
#include <cstdint>

class PitchJitter {
public:
    // Advance once for every 48 kHz sample. A slider value of 0.020 bounds
    // the instantaneous pitch ratio to roughly 0.98..1.02 (about +/-35 cents).
    // The random goal changes about 18 times a second, but the actual ratio
    // moves continuously so goal changes never add sample discontinuities.
    float NextRatio(float amount) noexcept {
        amount = std::clamp(amount, 0.0f, 0.020f);
        if (amount == 0.0f) {
            current_ = 0.0f;
            target_ = 0.0f;
            remaining_ = 0;
            return 1.0f;
        }

        if (remaining_ == 0) {
            // Xorshift32 is deterministic, cheap and independent per processor.
            // Randomness is generated entirely on the audio thread with no
            // allocations, locks or calls to random_device.
            state_ ^= state_ << 13;
            state_ ^= state_ >> 17;
            state_ ^= state_ << 5;
            const float unit = static_cast<float>(state_ & 0xffffu) / 65535.0f;
            target_ = (2.0f * unit - 1.0f) * amount;
            remaining_ = 48000 / 18;
        }
        --remaining_;

        // A short low-pass transition removes abrupt pitch jumps while
        // retaining a noticeable roughness at the maximum slider setting.
        current_ += (target_ - current_) * 0.0075f;
        return 1.0f + current_;
    }

private:
    uint32_t state_ = 0xa3618c7du;
    unsigned remaining_ = 0;
    float current_ = 0.0f;
    float target_ = 0.0f;
};
