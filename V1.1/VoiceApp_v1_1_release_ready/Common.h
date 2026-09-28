// Shared C++ types and constants. Audio sample counts are distinct from byte counts; the PortAudio sample rate is measured in hertz.

#pragma once
// SDL owns windowing and rendering; PortAudio owns audio devices and callbacks. These shared
// includes preserve the dependencies of the earlier monolithic source.
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>
#include <list>
#include <type_traits>
#include <format>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <utility>
#include <stdexcept>
#include <random>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>
#include <mutex>
#include <ctime>
#include <atomic>
#include <array>

#ifdef _WIN32
#include <direct.h>
#define mkdir(dir, mode) _mkdir(dir)
#else

#endif

#include <portaudio.h>
#include <chrono>
#include <algorithm>
#include <iterator>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
// Inherited global using directive: all project headers currently depend on unqualified standard
// types. Removing it requires updating every header and implementation.
using namespace std;

// A quadrilateral has four logical corners. SDL_RenderGeometry draws it with two triangles of
// three vertices each.
#define widgetVertexCount 4
#define verticesPerTriangle 3
#define rgbChannelCount 3
#define trianglesPerQuad 2

// The waveform ring stores display samples, not the full recording. Its write position wraps at
// this capacity.
constexpr size_t WAVEFORM_SAMPLES = 2048;

// At 16 kHz, 1024 samples span 64 ms; PA_NUM_BARS compresses the selected 80..4000 Hz bins.
constexpr size_t PA_FFT_SIZE = 1024;
constexpr size_t PA_NUM_BARS = 69;
// Changing the 16 kHz rate affects callback timing, filter coefficients, FFT bin spacing, delay
// lengths and WAV metadata.
constexpr float PA_SAMPLE_RATE = 16000.0f;
// This threshold schedules visualization work from the SDL frame loop. The current frame counter
// is not an exact audio sample counter.
constexpr size_t FFT_OVERLAP = PA_FFT_SIZE / 2;
constexpr int PA_CALIBRATION_FRAMES = 60;
constexpr float mouseLeftButtonClick_Color_OFFSET = 0.15f;
constexpr float JITTER_AMOUNT = 0.005f;

// WidgetType determines rendering and hit testing. PosN variants snap to intervals; FreeLimited
// variants retain continuous positions.
enum class WidgetType : uint8_t {
    Static_Full = 0,
    Static_DynaZoom,
    Button_StateMono,
    Button_StateDuo,
    Knob_PosN,
    Knob_FreeLimited,
    Knob_Free360,
    SliderH_PosN,
    SliderH_FreeLimited,
    SliderV_PosN,
    SliderV_FreeLimited,
    Slider2D_PosN,
    Slider2D_FreeLimited,
    TextBox,
    List
};

// SDL_AppEvent dispatches button actions by these values. A new button needs an enum value, UI
// configuration and an event case.
enum class WidgetAction : uint8_t {
    None = 0,
    PA_AudioRecord_Start,
    PA_AudioRecord_Stop,
    PA_AudioPlay_Start,
    PA_AudioPlay_Stop,
    PA_AudioPlayAnonymizedPitch_Start,
    PA_AudioPlayAnonymizedPitch_Stop,
    PA_AudioPlayAnonymizedFormant_Start,
    PA_AudioPlayAnonymizedFormant_Stop,
    PA_AudioPlayAnonymizedPitchFormant_Start,
    PA_AudioPlayAnonymizedPitchFormant_Stop,
    PA_SaveOriginal,
    PA_SaveAnonymizedPitch,
    PA_SaveAnonymizedFormant,
    PA_SaveAnonymizedPitchFormant,
    AudioMode_Toggle
};

enum class ColorChannel : Uint8 {
    r = 0,
    g,
    b
};

// FormatValueWithUnit converts these display units to suffixes; unit choices do not change the
// numerical DSP calculation.
enum class WidgetUnit : uint8_t {
    Pts = 0,
    Hz,
    dB,
    percent,
    cents,
    ms,
    degrees,
};

// Temporary duplex arrays hold 320 float samples. The callback assumes framesPerBuffer does not exceed this capacity.
constexpr size_t STREAM_BUFFER_SIZE = 320;

// The packed conceptual RIFF/WAVE PCM header written by Wav.cpp; the file writer fills each field
// explicitly.
struct WAVHeader {

    char riffID[4];
    // RIFF chunk size equals total file bytes minus eight, while dataSize below contains only PCM
    // data bytes.
    uint32_t fileSize;
    char riffType[4];

    char fmtID[4];
    uint32_t fmtSize;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    // byteRate = sampleRate * channelCount * bitsPerSample / 8; blockAlign gives bytes per
    // interleaved frame.
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;

    char dataID[4];
    uint32_t dataSize;
};

template<typename T>
// Generic helpers preserve the behavior of the earlier source; Min and Max compare values without
// clamping them to audio limits.
inline void Swap(T& a, T& b) noexcept;

template<typename T>
inline T Min(T a, T b) noexcept;

template<typename T>
inline T Max(T a, T b) noexcept;

template<typename T>
inline void Swap(T& a, T& b) noexcept {
    T temp = a;
    a = b;
    b = temp;
}

template<typename T>
inline T Min(T a, T b) noexcept {
    return (a < b) ? a : b;
}

template<typename T>
inline T Max(T a, T b) noexcept {
    return (a > b) ? a : b;
}

