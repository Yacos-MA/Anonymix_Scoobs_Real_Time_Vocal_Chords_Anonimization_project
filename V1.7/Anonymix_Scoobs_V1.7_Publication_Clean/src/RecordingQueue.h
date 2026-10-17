// Single-producer/single-consumer queue between PortAudio capture and the recording worker.
// Only the audio callback writes samples; only the worker reads them. Both indices are atomic,
// while slots are ordinary PCM16 values published by the release/acquire index handoff.
#pragma once

#include <array>
#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>

class RecordingQueue final {
public:
    // About 5.46 seconds at 48 kHz. A stalled consumer cannot grow this allocation or
    // delay the callback: a full queue drops the whole incoming block and counts its frames.
    static constexpr size_t Capacity = 1u << 18;
    static_assert((Capacity & (Capacity - 1)) == 0);
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
        "The x64 recording callback requires lock-free 64-bit counters");

    // Call only after the previous worker has joined and the producer has been disabled.
    void Reset() noexcept {
        read_.store(0, std::memory_order_relaxed);
        written_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
    }

    void Push(const float* input, size_t frames) noexcept {
        const uint64_t write = written_.load(std::memory_order_relaxed);
        const uint64_t read = read_.load(std::memory_order_acquire);
        if (frames > Capacity - static_cast<size_t>(write - read)) {
            dropped_.fetch_add(frames, std::memory_order_relaxed);
            return;
        }
        for (size_t i = 0; i < frames; ++i) {
            // Match V1.5's microphone conversion exactly; do not replace its 32767 scale
            // with the different 32768 scale used by processed WAV output.
            const float sample = std::clamp(input[i], -1.0f, 1.0f);
            data_[(write + i) & (Capacity - 1)] =
                static_cast<int16_t>(sample * 32767.0f);
        }
        written_.store(write + frames, std::memory_order_release);
    }

    size_t Pull(int16_t* destination, size_t limit) noexcept {
        const uint64_t read = read_.load(std::memory_order_relaxed);
        const uint64_t write = written_.load(std::memory_order_acquire);
        const size_t count = std::min(limit, static_cast<size_t>(write - read));
        for (size_t i = 0; i < count; ++i)
            destination[i] = data_[(read + i) & (Capacity - 1)];
        read_.store(read + count, std::memory_order_release);
        return count;
    }

    bool Empty() const noexcept {
        return read_.load(std::memory_order_acquire) ==
            written_.load(std::memory_order_acquire);
    }

    uint64_t DroppedFrames() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    std::array<int16_t, Capacity> data_{};
    std::atomic<uint64_t> read_{0};
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> dropped_{0};
};
