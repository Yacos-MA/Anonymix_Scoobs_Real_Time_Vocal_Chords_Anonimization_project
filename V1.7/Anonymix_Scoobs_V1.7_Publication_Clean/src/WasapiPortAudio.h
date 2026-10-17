// PortAudio's optional Windows stream descriptor. Prefer the exact header supplied with the
// installed PortAudio development files; the small ABI fallback keeps source-only builds usable
// when that optional header is absent. The documented exclusive flag and stereo speaker
// channel mask select the endpoint's front-left and front-right outputs.
#pragma once

#include <portaudio.h>
#include <cstdint>

#ifdef _WIN32
#if __has_include(<pa_win_wasapi.h>)
#include <pa_win_wasapi.h>
using ApplicationWasapiStreamInfo = PaWasapiStreamInfo;
constexpr unsigned long APPLICATION_WASAPI_EXCLUSIVE = paWinWasapiExclusive;
constexpr unsigned long APPLICATION_WASAPI_USE_CHANNEL_MASK = paWinWasapiUseChannelMask;
#else
// Public PaWasapiStreamInfo version 1 layout for the Windows x64 PortAudio V19 API. If the
// runtime DLL rejects the descriptor, the caller falls back to the existing Shared path.
struct ApplicationWasapiStreamInfo {
    std::uint32_t size;
    std::int32_t hostApiType; // PaHostApiTypeId has an int representation in the public C API.
    std::uint32_t version;
    std::uint32_t flags;
    std::uint32_t channelMask;
    void (*hostProcessorOutput)(void*, long, void*, long, void*);
    void (*hostProcessorInput)(void*, long, void*, long, void*);
    int threadPriority;
    int streamCategory;
    int streamOption;
};
constexpr unsigned long APPLICATION_WASAPI_EXCLUSIVE = 1ul << 0;
constexpr unsigned long APPLICATION_WASAPI_USE_CHANNEL_MASK = 1ul << 2;
#if defined(_WIN64)
static_assert(sizeof(ApplicationWasapiStreamInfo) == 56,
    "Check the PortAudio WASAPI stream descriptor layout for Windows x64");
#endif
#endif

inline ApplicationWasapiStreamInfo MakeExclusiveWasapiStreamInfo(
    bool stereoOutput = false) noexcept {
    ApplicationWasapiStreamInfo info{};
    info.size = sizeof(info);
    info.hostApiType = paWASAPI;
    info.version = 1;
    info.flags = APPLICATION_WASAPI_EXCLUSIVE;
    if (stereoOutput) {
        // Route the same processed mono sample to the physical front-left and front-right
        // speakers. The public Windows/PortAudio speaker-mask bits are 0x1 and 0x2.
        info.flags |= APPLICATION_WASAPI_USE_CHANNEL_MASK;
        info.channelMask = 0x3;
    }
    return info;
}
#endif
