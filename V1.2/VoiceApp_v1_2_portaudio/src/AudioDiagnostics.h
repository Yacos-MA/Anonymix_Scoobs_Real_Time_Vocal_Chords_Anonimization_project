// Lightweight callback statistics for live PortAudio input and output on one machine.
// This header depends only on the C++ standard library.
#pragma once

#include <atomic>
#include <cstdint>

class AudioDiagnostics final {
public:
    struct Snapshot {
        std::uint64_t blocks = 0;
        std::uint64_t frames = 0;
        std::uint64_t minimumFramesPerBlock = 0;
        std::uint64_t maximumFramesPerBlock = 0;
        std::uint64_t totalProcessingMicroseconds = 0;
        std::uint64_t maximumProcessingMicroseconds = 0;
        std::uint64_t blocksOverBudget = 0;
        std::uint64_t inputOverflows = 0;
        std::uint64_t outputUnderflows = 0;
        std::uint64_t hostTimingObservations = 0;
        std::uint64_t totalEstimatedHostMicroseconds = 0;
    };

    // Called once per *live* block by the audio thread. No vector growth, logging, or mutex is
    // allowed here. The caller measures the complete callback duration around its existing DSP.
    void recordLiveBlock(std::uint64_t frames,
        std::uint64_t processingMicroseconds,
        double sampleRate,
        bool inputOverflow,
        bool outputUnderflow,
        double estimatedAdcToDacSeconds = 0.0) noexcept
    {
        blocks_.fetch_add(1, std::memory_order_relaxed);
        frames_.fetch_add(frames, std::memory_order_relaxed);
        totalProcessingMicroseconds_.fetch_add(processingMicroseconds, std::memory_order_relaxed);

        auto minimumFrames = minimumFramesPerBlock_.load(std::memory_order_relaxed);
        while (minimumFrames > frames &&
            !minimumFramesPerBlock_.compare_exchange_weak(minimumFrames, frames,
                std::memory_order_relaxed)) {}
        auto maximumFrames = maximumFramesPerBlock_.load(std::memory_order_relaxed);
        while (maximumFrames < frames &&
            !maximumFramesPerBlock_.compare_exchange_weak(maximumFrames, frames,
                std::memory_order_relaxed)) {}

        // One block has frames/sampleRate seconds available. Exceeding this budget means the
        // callback alone took longer than its nominal audio period; it is not a round-trip test.
        if (sampleRate > 0.0 &&
            static_cast<double>(processingMicroseconds) * sampleRate >
                static_cast<double>(frames) * 1000000.0) {
            blocksOverBudget_.fetch_add(1, std::memory_order_relaxed);
        }

        if (inputOverflow) inputOverflows_.fetch_add(1, std::memory_order_relaxed);
        if (outputUnderflow) outputUnderflows_.fetch_add(1, std::memory_order_relaxed);

        // PortAudio timestamps estimate when the first input sample was captured and when the
        // first output position will play. Invalid or missing timestamps are ignored.
        // This excludes any additional delay introduced by the pitch/formant read heads.
        if (estimatedAdcToDacSeconds > 0.0 && estimatedAdcToDacSeconds < 5.0) {
            hostTimingObservations_.fetch_add(1, std::memory_order_relaxed);
            totalEstimatedHostMicroseconds_.fetch_add(
                static_cast<std::uint64_t>(estimatedAdcToDacSeconds * 1000000.0),
                std::memory_order_relaxed);
        }

        // C++20 does not provide atomic::fetch_max. This compare/exchange loop only updates the
        // observed maximum when the new block is slower than the previous maximum.
        auto maximum = maximumProcessingMicroseconds_.load(std::memory_order_relaxed);
        while (maximum < processingMicroseconds &&
            !maximumProcessingMicroseconds_.compare_exchange_weak(maximum,
                processingMicroseconds, std::memory_order_relaxed)) {}
    }

    // The SDL thread consumes approximately one reporting window at a time. Individual relaxed
    // exchanges may straddle a callback; the totals are diagnostic, not an exact transaction.
    Snapshot takeSnapshot() noexcept
    {
        Snapshot result;
        result.blocks = blocks_.exchange(0, std::memory_order_relaxed);
        result.frames = frames_.exchange(0, std::memory_order_relaxed);
        const auto minimum = minimumFramesPerBlock_.exchange(~std::uint64_t{0},
            std::memory_order_relaxed);
        result.minimumFramesPerBlock = result.blocks == 0 ? 0 : minimum;
        result.maximumFramesPerBlock = maximumFramesPerBlock_.exchange(0,
            std::memory_order_relaxed);
        result.totalProcessingMicroseconds = totalProcessingMicroseconds_.exchange(0,
            std::memory_order_relaxed);
        result.maximumProcessingMicroseconds = maximumProcessingMicroseconds_.exchange(0,
            std::memory_order_relaxed);
        result.blocksOverBudget = blocksOverBudget_.exchange(0, std::memory_order_relaxed);
        result.inputOverflows = inputOverflows_.exchange(0, std::memory_order_relaxed);
        result.outputUnderflows = outputUnderflows_.exchange(0, std::memory_order_relaxed);
        result.hostTimingObservations = hostTimingObservations_.exchange(0,
            std::memory_order_relaxed);
        result.totalEstimatedHostMicroseconds = totalEstimatedHostMicroseconds_.exchange(0,
            std::memory_order_relaxed);
        return result;
    }

private:
    std::atomic<std::uint64_t> blocks_{0};
    std::atomic<std::uint64_t> frames_{0};
    std::atomic<std::uint64_t> minimumFramesPerBlock_{~std::uint64_t{0}};
    std::atomic<std::uint64_t> maximumFramesPerBlock_{0};
    std::atomic<std::uint64_t> totalProcessingMicroseconds_{0};
    std::atomic<std::uint64_t> maximumProcessingMicroseconds_{0};
    std::atomic<std::uint64_t> blocksOverBudget_{0};
    std::atomic<std::uint64_t> inputOverflows_{0};
    std::atomic<std::uint64_t> outputUnderflows_{0};
    std::atomic<std::uint64_t> hostTimingObservations_{0};
    std::atomic<std::uint64_t> totalEstimatedHostMicroseconds_{0};
};

// Defined exactly once in Audio.cpp; Stream.cpp records and AppUpdate.cpp reports.
extern AudioDiagnostics g_audioDiagnostics;
