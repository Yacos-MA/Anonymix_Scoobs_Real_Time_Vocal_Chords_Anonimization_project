// Declarations of shared state. Each extern is defined once in State.cpp. The audio callback and SDL frame code may run on different threads.

#pragma once
#include "Common.h"

// Captures software intervals inside the callback. T1..T4 do not include physical microphone or
// speaker latency. Atomic counters avoid acquiring a mutex or growing vectors on the audio thread.
struct LatencyMetrics {
    using TimePoint = chrono::steady_clock::time_point;

    atomic<uint64_t> sampleCount{0};
    atomic<uint64_t> copyToDspNanoseconds{0};
    atomic<uint64_t> dspNanoseconds{0};
    atomic<uint64_t> copyToOutputNanoseconds{0};
    int secondCounter = 0;

    // Record the three adjacent software stages in nanoseconds without callback-side allocation.
    void recordSample(TimePoint t1, TimePoint t2, TimePoint t3, TimePoint t4) noexcept {
        const auto toNanoseconds = [](TimePoint start, TimePoint end) {
            return static_cast<uint64_t>(chrono::duration_cast<chrono::nanoseconds>(
                end - start).count());
        };
        copyToDspNanoseconds.fetch_add(toNanoseconds(t1, t2), memory_order_relaxed);
        dspNanoseconds.fetch_add(toNanoseconds(t2, t3), memory_order_relaxed);
        copyToOutputNanoseconds.fetch_add(toNanoseconds(t3, t4), memory_order_relaxed);
        sampleCount.fetch_add(1, memory_order_relaxed);
    }

    // Report and reset one approximate observation window on the SDL thread. Independent atomic
    // exchanges can split a concurrent sample across windows; these are diagnostic averages.
    void printSecondReport() {
        const auto count = sampleCount.exchange(0, memory_order_relaxed);
        const auto copyToDsp = copyToDspNanoseconds.exchange(0, memory_order_relaxed);
        const auto dsp = dspNanoseconds.exchange(0, memory_order_relaxed);
        const auto copyToOutput = copyToOutputNanoseconds.exchange(0, memory_order_relaxed);
        if (count == 0) return;

        const auto averageMs = [count](uint64_t nanoseconds) {
            return static_cast<double>(nanoseconds) / (1000000.0 * static_cast<double>(count));
        };

        secondCounter++;

        SDL_Log("=== SOFTWARE CALLBACK STAGES (report #%d; no device latency) ===", secondCounter);
        SDL_Log("  1) Copy cleaned input to DSP buffer: %.3f ms (average of %llu DSP chunks)",
            averageMs(copyToDsp), static_cast<unsigned long long>(count));
        SDL_Log("  2) Pitch, formant, timbre, jitter and volume: %.3f ms",
            averageMs(dsp));
        SDL_Log("  3) Copy processed samples to output buffer: %.3f ms",
            averageMs(copyToOutput));
        SDL_Log("  4) Time outside processing: %.3f ms (time 1 + time 3)",
            averageMs(copyToDsp + copyToOutput));

        SDL_Log("  5) TIMED SOFTWARE STAGES: %.3f ms (excludes device buffering and DSP delay)",
            averageMs(copyToDsp + dsp + copyToOutput));
        SDL_Log("=====================================");
    }
};

// Allocated in SDL_AppInit, used by live processing and reporting, and deleted during SDL_AppQuit.
extern LatencyMetrics* g_latencyMetrics;
class UiWidget;
class UiWidgetCollection;

// SDL handles and drawing state belong to the UI. Widget and visualization functions use renderer
// and gFont.
extern SDL_Window* window;
extern SDL_Renderer* renderer;
extern SDL_Surface* surface;
extern SDL_Texture* texture;
extern TTF_Font* gFont;
extern const SDL_FRect rect1;
extern const SDL_FRect rect2;
extern SDL_FRect rect3;
extern SDL_FRect rect4;
extern const SDL_PixelFormatDetails* pxFormatRGBA64;
extern SDL_Texture* gLogoTexture;
extern SDL_FRect    gLogoRect;
// Legacy toggles drive the decorative animation in AppRender.cpp; they are not audio-mode flags.
extern bool toggle1;
extern bool toggle2;
extern bool toggle3;
extern bool toggle4;
extern bool toggle5;
extern bool toggle6;
extern bool toggle7;
extern bool toggle8;
extern bool toggle9;
extern bool toggle10;
extern int sevenBitsValue;
extern int tenBitsValue;
// Mouse coordinates and the pressed/hovered widget pointers connect SDL_AppEvent with per-frame
// hit testing.
extern float mx, my;
extern float x, y;
extern Uint32 mousePos;
extern bool boolVar;
extern double angleGeo;
extern float red, green, blue;
extern Uint8 grey_r;
extern Uint8 grey_g;
extern Uint8 grey_b;
extern Uint8 grey_a;
extern Uint32 color_grey;
extern SDL_FPoint mouseClick_OFFSET;

// The callback appends FFT input while the UI copies it for analysis. The waveform ring below has
// no matching complete synchronization.
extern mutex paFftMutex;
// Circular float waveform used only for display; waveformWritePos indexes the next value to be
// written.
extern vector<float> waveformBuffer;
extern size_t waveformWritePos;
extern bool waveformDataReady;
// Recent cleaned float samples feed the FFT. The resulting magnitudes, smoothing and calibration
// state drive the spectrum view.
extern vector<float> paFftInputBuffer;
extern vector<float> paFftMagnitudes;
extern vector<float> paFftSmoothed;
extern bool paFftDataReady;
extern vector<float> paAdaptiveNoiseFloor;
extern bool paIsCalibrating;
extern int paCalibrationFrames;

// Capture storage is raw little-endian PCM16 bytes without a WAV header. The callback writes one
// mono sample per frame.
extern vector<Uint8> paAudioBuffer;
// Recording and playback flags are atomics; playback positions are changed only after their
// streams are stopped and joined, or inside the corresponding callback.
extern atomic<bool> paIsRecording;
// The main PortAudio stream invokes paUnifiedCallback; the following five streams handle recorded
// playback variants.
extern PaStream* paStream;
extern atomic<bool> paIsPlaying;
extern size_t paPlaybackPosition;
extern PaStream* paPlaybackStream;
extern vector<Uint8> paPlaybackOriginalBuffer;
extern atomic<bool> paIsPlayingAnonymized;
extern size_t paPlaybackAnonymizedPosition;
extern PaStream* paPlaybackAnonymizedStream;
// Playback copies the original byte buffer into variant-specific storage so the recorded source
// remains available.
extern vector<Uint8> paProcessedBuffer;
extern atomic<bool> paIsPlayingAnonymizedFormant;
extern size_t paPlaybackAnonymizedFormantPosition;
extern PaStream* paPlaybackAnonymizedFormantStream;
extern vector<Uint8> paProcessedFormantBuffer;
extern atomic<bool> paIsPlayingAnonymizedPitchFormant;
extern size_t paPlaybackAnonymizedPitchFormantPosition;
extern PaStream* paPlaybackAnonymizedPitchFormantStream;
extern vector<Uint8> paProcessedPitchFormantBuffer;
// The timbre playback processor owns separate filter history; its PCM snapshot is immutable
// while its playback stream is running.
extern atomic<bool> paIsPlayingTimbre;
extern size_t paPlaybackTimbrePosition;
extern PaStream* paPlaybackTimbreStream;
extern vector<Uint8> paProcessedTimbreBuffer;

// Slider settings cross from SDL to PortAudio through atomics; other shared buffers and flags lack complete synchronization.
extern atomic<float> g_pitchShiftSemitones;
extern atomic<float> g_formantShiftRatio;
// -100 is darker, zero is neutral and +100 is brighter. The audio thread smooths changes.
extern atomic<float> g_timbrePercent;
// The live callback uses this mutex only when appending recorded PCM bytes. The SDL thread
// takes it when starting a new capture or copying the finished take for playback/export.
extern mutex paAudioBufferMutex;
extern atomic<float> g_jitterAmount;
extern atomic<float> g_outputVolume;
// The V1.2 capture, live processing and recorded-playback path uses one mono channel.
extern int paMicrophoneChannels;
// Pointer lifetime depends on the named list of widgets in State.cpp. Removing or replacing a
// widget while pressed would invalidate pointers.
extern UiWidget* hoveredWidget;
extern UiWidget* pressedWidget;
extern UiWidgetCollection widgets;

// The unified callback uses this flag to select recording or duplex processing.
extern atomic<bool> g_streamModeActive;
