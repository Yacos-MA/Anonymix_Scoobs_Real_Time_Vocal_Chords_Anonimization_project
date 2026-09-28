// Declarations of shared state. Each extern is defined once in State.cpp. The audio callback and SDL frame code may run on different threads.

#pragma once
#include "Common.h"

// Captures software intervals inside the callback. T1..T4 do not include physical microphone or
// speaker latency; recordSample locks and appends to vectors.
struct LatencyMetrics {
    using TimePoint = chrono::high_resolution_clock::time_point;
    using Duration = chrono::duration<double, milli>;

    // T1 occurs after initial capture cleanup in the callback. Names T1..T4 describe software
    // checkpoints, not hardware events.
    TimePoint t1_microphoneCapture;
    TimePoint t2_readyForDSP;
    TimePoint t3_readyForOutput;
    TimePoint t4_sentToSpeaker;

    // Each latency field is a duration in milliseconds calculated from adjacent or combined
    // checkpoints.
    double latency_1_capture_to_dsp;
    double latency_2_dsp_processing;
    double latency_3_output_to_playback;
    double latency_4_non_processing;
    double latency_5_total;

    // The five vectors accumulate observations until printSecondReport clears them under
    // metricsMutex.
    vector<double> samples_latency1;
    vector<double> samples_latency2;
    vector<double> samples_latency3;
    vector<double> samples_latency4;
    vector<double> samples_latency5;

    // Audio and UI code both access these measurements; the mutex protects the vectors but adds
    // work to the audio callback.
    mutex metricsMutex;
    int secondCounter = 0;

    LatencyMetrics() {
        samples_latency1.reserve(100);
        samples_latency2.reserve(100);
        samples_latency3.reserve(100);
        samples_latency4.reserve(100);
        samples_latency5.reserve(100);
    }

    // Calculate the five intervals from callback timestamps and append them for the next report.
    void recordSample() {
        lock_guard<mutex> lock(metricsMutex);

        latency_1_capture_to_dsp = Duration(t2_readyForDSP - t1_microphoneCapture).count();
        latency_2_dsp_processing = Duration(t3_readyForOutput - t2_readyForDSP).count();
        latency_3_output_to_playback = Duration(t4_sentToSpeaker - t3_readyForOutput).count();
        latency_4_non_processing = latency_1_capture_to_dsp + latency_3_output_to_playback;
        latency_5_total = Duration(t4_sentToSpeaker - t1_microphoneCapture).count();

        samples_latency1.push_back(latency_1_capture_to_dsp);
        samples_latency2.push_back(latency_2_dsp_processing);
        samples_latency3.push_back(latency_3_output_to_playback);
        samples_latency4.push_back(latency_4_non_processing);
        samples_latency5.push_back(latency_5_total);
    }

    // Report averages and clear the samples; this describes callback processing time, not round-trip
    // latency.
    void printSecondReport() {
        lock_guard<mutex> lock(metricsMutex);

        // An empty reporting window has no meaningful average; retain the prior report counter in
        // this case.
        if (samples_latency1.empty()) return;

        auto calcAvg = [](const vector<double>& vec) {
            double sum = 0.0;
            for (double v : vec) sum += v;
            return sum / vec.size();
            };

        secondCounter++;

        SDL_Log("=== LATENCY REPORT (Second #%d) ===", secondCounter);
        SDL_Log("  1) Microphone --> DSP ready (with 80 Hz filter): %.3f ms (average of %zu samples)",
            calcAvg(samples_latency1), samples_latency1.size());
        SDL_Log("  2) Full DSP processing: %.3f ms (pitch + formant + jitter + norm + volume)",
            calcAvg(samples_latency2), samples_latency2.size());
        SDL_Log("  3) Output ready --> handed to speaker: %.3f ms",
            calcAvg(samples_latency3), samples_latency3.size());
        SDL_Log("  4) Time outside processing: %.3f ms (time 1 + time 3)",
            calcAvg(samples_latency4), samples_latency4.size());

        SDL_Log("  5) TOTAL MEASURED TIME: %.3f ms (inside callback)",
            calcAvg(samples_latency5), samples_latency5.size());
        SDL_Log("=====================================");

        // Once reported, discard accumulated samples so the next report covers a new window.
        samples_latency1.clear();
        samples_latency2.clear();
        samples_latency3.clear();
        samples_latency4.clear();
        samples_latency5.clear();
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
// This and the playback flags/positions are shared with callbacks without a complete
// synchronization contract.
extern bool paIsRecording;
// The main PortAudio stream invokes paUnifiedCallback; the following four streams handle recorded
// playback variants.
extern PaStream* paStream;
extern bool paIsPlaying;
extern size_t paPlaybackPosition;
extern PaStream* paPlaybackStream;
extern bool paIsPlayingAnonymized;
extern size_t paPlaybackAnonymizedPosition;
extern PaStream* paPlaybackAnonymizedStream;
// Playback copies the original byte buffer into variant-specific storage so the recorded source
// remains available.
extern vector<Uint8> paProcessedBuffer;
extern bool paIsPlayingAnonymizedFormant;
extern size_t paPlaybackAnonymizedFormantPosition;
extern PaStream* paPlaybackAnonymizedFormantStream;
extern vector<Uint8> paProcessedFormantBuffer;
extern bool paIsPlayingAnonymizedPitchFormant;
extern size_t paPlaybackAnonymizedPitchFormantPosition;
extern PaStream* paPlaybackAnonymizedPitchFormantStream;
extern vector<Uint8> paProcessedPitchFormantBuffer;

// Slider settings cross from SDL to PortAudio through atomics; other shared buffers and flags lack complete synchronization.
extern atomic<float> g_pitchShiftSemitones;
extern atomic<float> g_formantShiftRatio;
extern atomic<float> g_jitterAmount;
extern atomic<float> g_outputVolume;
// This device channel count is also used when saving WAV metadata. Recorded bytes are currently
// mono even in a stereo configuration.
extern int paMicrophoneChannels;
// Pointer lifetime depends on the named list of widgets in State.cpp. Removing or replacing a
// widget while pressed would invalidate pointers.
extern UiWidget* hoveredWidget;
extern UiWidget* pressedWidget;
extern UiWidgetCollection widgets;

// The unified callback uses this flag to select recording or duplex processing.
extern atomic<bool> g_streamModeActive;
