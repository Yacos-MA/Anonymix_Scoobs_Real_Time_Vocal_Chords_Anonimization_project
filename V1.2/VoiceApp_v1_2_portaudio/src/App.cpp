// SDL3 callback entry points: initialization, input events, frame update and shutdown. Widget actions are dispatched from SDL_AppEvent.

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>
#include "Widgets.h"
#include "Stream.h"
#include "Dsp.h"
#include "Wav.h"
#include "AppFrame.h"
#include "AudioDiagnostics.h"
#include <thread>
#include <system_error>
#include <condition_variable>
#include <functional>

// SDL owns the UI and every widget. The device worker owns PortAudio state. SDL disables
// audio buttons during reconnection and observes the result on a later frame.
static thread g_portAudioDeviceWorker;
static mutex g_audioWorkerMutex;
static condition_variable g_audioWorkerWake;
static bool g_audioWorkerInitialized = false;
static bool g_audioWorkerReady = false;
static bool g_audioWorkerHasRequest = false;
static bool g_audioWorkerRestoreLiveMode = false;
static bool g_audioWorkerShouldStop = false;
static function<bool()> g_audioWorkerAction;
static bool g_audioWorkerActionDone = true;
static bool g_audioWorkerActionResult = false;
static atomic<int> g_audioReopenState{0}; // 0: idle; 1: running; 2: success; 3: failure.

bool IsAudioDeviceReopening() {
    return g_audioReopenState.load(memory_order_acquire) == 1;
}

// Closing and enumerating a Windows WASAPI duplex stream can take seconds. This routine runs
// on the worker, with no widget or SDL renderer calls.
static bool PerformAudioDeviceReopen(bool restoreLiveMode, bool portAudioWasReady = true) {
    const auto reopenStarted = chrono::steady_clock::now();
    SDL_Log("=== AUDIO DEVICE REOPEN: PortAudio, previous live mode %s ===",
        restoreLiveMode ? "on" : "off");
    if (paStream != nullptr) {
        const int state = Pa_IsStreamActive(paStream);
        SDL_Log("PortAudio stream before reopen: %d (negative indicates a device error)", state);
    }
    if (portAudioWasReady) CleanupPortAudio();
    const auto portAudioClosed = chrono::steady_clock::now();
    SDL_Log("Reopen teardown: PortAudio %.1f ms",
        chrono::duration<double, milli>(portAudioClosed - reopenStarted).count());
    paIsPlaying = false;
    paIsPlayingAnonymized = false;
    paIsPlayingAnonymizedFormant = false;
    paIsPlayingAnonymizedPitchFormant = false;
    paIsPlayingTimbre.store(false, memory_order_release);

    const bool recovered = InitPortAudio();
    const auto devicesOpened = chrono::steady_clock::now();
    SDL_Log("Reopen setup: %.1f ms",
        chrono::duration<double, milli>(devicesOpened - portAudioClosed).count());
    if (recovered && restoreLiveMode)
        g_streamModeActive.store(true, memory_order_release);
    g_audioDiagnostics.takeSnapshot();
    const double reopenMilliseconds = chrono::duration<double, milli>(
        chrono::steady_clock::now() - reopenStarted).count();
    SDL_Log("=== AUDIO DEVICE REOPEN %s in %.1f ms; active backend: %s ===",
        recovered ? "SUCCEEDED" : "FAILED",
        reopenMilliseconds,
        recovered ? "PortAudio" : "none");
    return recovered;
}

// Keep initialization, stream operations and termination on one long-lived WASAPI thread.
static void PortAudioDeviceWorkerLoop() {
    bool ready = InitPortAudio();
    {
        lock_guard<mutex> lock(g_audioWorkerMutex);
        g_audioWorkerReady = ready;
        g_audioWorkerInitialized = true;
    }
    g_audioWorkerWake.notify_one();

    for (;;) {
        bool restoreLiveMode = false;
        function<bool()> action;
        {
            unique_lock<mutex> lock(g_audioWorkerMutex);
            g_audioWorkerWake.wait(lock, [] {
                return g_audioWorkerShouldStop || g_audioWorkerHasRequest || g_audioWorkerAction;
            });
            if (g_audioWorkerShouldStop) break;
            if (g_audioWorkerAction) {
                action = move(g_audioWorkerAction);
                g_audioWorkerAction = nullptr;
            } else {
                restoreLiveMode = g_audioWorkerRestoreLiveMode;
                g_audioWorkerHasRequest = false;
            }
        }
        if (action) {
            bool result = false;
            try { result = action(); }
            catch (...) { SDL_Log("Audio device action failed with an exception"); }
            {
                lock_guard<mutex> lock(g_audioWorkerMutex);
                g_audioWorkerActionResult = result;
                g_audioWorkerActionDone = true;
            }
            g_audioWorkerWake.notify_one();
            continue;
        }
        ready = PerformAudioDeviceReopen(restoreLiveMode, ready);
        {
            lock_guard<mutex> lock(g_audioWorkerMutex);
            g_audioWorkerReady = ready;
        }
        g_audioReopenState.store(ready ? 2 : 3, memory_order_release);
    }
    if (ready) CleanupPortAudio();
}

bool IsPortAudioDeviceWorkerThread() {
    return this_thread::get_id() == g_portAudioDeviceWorker.get_id();
}

bool RunPortAudioDeviceAction(bool (*action)()) {
    if (IsPortAudioDeviceWorkerThread()) return action();
    unique_lock<mutex> lock(g_audioWorkerMutex);
    if (!g_audioWorkerInitialized || !g_audioWorkerReady || g_audioWorkerShouldStop ||
        g_audioReopenState.load(memory_order_acquire) != 0) return false;
    g_audioWorkerWake.wait(lock, [] { return !g_audioWorkerAction && g_audioWorkerActionDone; });
    g_audioWorkerActionDone = false;
    g_audioWorkerAction = action;
    lock.unlock();
    g_audioWorkerWake.notify_one();
    lock.lock();
    g_audioWorkerWake.wait(lock, [] { return g_audioWorkerActionDone; });
    return g_audioWorkerActionResult;
}

void RunPortAudioDeviceAction(void (*action)()) {
    if (IsPortAudioDeviceWorkerThread()) { action(); return; }
    // A void action has no result; the bool dispatcher still waits for its completion.
    // Capture the function pointer in a function<bool()> through the same worker queue.
    unique_lock<mutex> lock(g_audioWorkerMutex);
    if (!g_audioWorkerInitialized || !g_audioWorkerReady || g_audioWorkerShouldStop ||
        g_audioReopenState.load(memory_order_acquire) != 0) return;
    g_audioWorkerWake.wait(lock, [] { return !g_audioWorkerAction && g_audioWorkerActionDone; });
    g_audioWorkerActionDone = false;
    g_audioWorkerAction = [action] { action(); return true; };
    lock.unlock();
    g_audioWorkerWake.notify_one();
    lock.lock();
    g_audioWorkerWake.wait(lock, [] { return g_audioWorkerActionDone; });
}

// Return true when a request was accepted. Completion is asynchronous;
// CompleteAudioDeviceReopen publishes the actual success/failure on SDL's main thread.
bool ReopenAudioDevices() {
    if (paIsRecording.load(memory_order_acquire)) {
        SDL_Log("Stop the recording before reopening audio devices");
        return false;
    }
    if (g_audioReopenState.load(memory_order_acquire) != 0) return false;
    const bool restoreLiveMode = g_streamModeActive.exchange(false, memory_order_acq_rel);
    {
        lock_guard<mutex> lock(g_audioWorkerMutex);
        if (!g_audioWorkerInitialized || g_audioWorkerShouldStop) {
            g_streamModeActive.store(restoreLiveMode, memory_order_release);
            return false;
        }
        g_audioWorkerRestoreLiveMode = restoreLiveMode;
        g_audioWorkerHasRequest = true;
        g_audioReopenState.store(1, memory_order_release);
    }
    g_audioWorkerWake.notify_one();
    return true;
}

// Observe worker completion before another SDL frame reads audio state or accepts new buttons.
static void CompleteAudioDeviceReopen() {
    const int state = g_audioReopenState.load(memory_order_acquire);
    if (state != 2 && state != 3) return;
    widgets("AudioDevices").setText("Reopen Audio");
    widgets("AudioDevices").setWidgetColorRGB(state == 2 ? 0.55f : 1.0f, 0.85f, 1.0f);
    if (g_streamModeActive.load(memory_order_acquire)) {
        widgets("AudioMode").setText("Active mode: Stream");
        widgets("AudioMode").setWidgetColorRGB(0.3f, 1.0f, 0.3f);
    } else {
        widgets("AudioMode").setText("Active mode: Recording");
        widgets("AudioMode").setWidgetColorRGB(0.8f, 0.8f, 0.8f);
    }
    g_audioReopenState.store(0, memory_order_release);
}

// Allocate visualization state, initialize SDL and PortAudio, create the window, and register all
// named controls.
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{

    // Set up inherited pixel-format state before drawing. Visualization buffers must be sized
    // before callbacks use them.
    pxFormatRGBA64 = SDL_GetPixelFormatDetails(SDL_PIXELFORMAT_RGBA64);
    color_grey = SDL_MapRGBA(pxFormatRGBA64, NULL, grey_r, grey_g, grey_b, grey_a);

    waveformBuffer.resize(WAVEFORM_SAMPLES, 0.0f);
    paFftMagnitudes.resize(PA_NUM_BARS, 0.0f);
    paFftSmoothed.resize(PA_NUM_BARS, 0.0f);
    paAdaptiveNoiseFloor.resize(PA_NUM_BARS, 0.0f);

    SDL_Log("V1.2 display FFT: project Cooley-Tukey (SDL3 rendering)");
    SDL_Log("V1.2 live and recorded timbre filter: standalone C++ IIR (one slider)");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // Initialize the live duplex stream on the worker that later owns all device operations.
    try {
        g_portAudioDeviceWorker = thread(PortAudioDeviceWorkerLoop);
        unique_lock<mutex> lock(g_audioWorkerMutex);
        g_audioWorkerWake.wait(lock, [] { return g_audioWorkerInitialized; });
        if (!g_audioWorkerReady)
            SDL_Log("WARNING: PortAudio initialization failed");
    }
    catch (const system_error& error) {
        SDL_Log("WARNING: Cannot start the PortAudio device worker: %s", error.what());
    }

    g_latencyMetrics = new LatencyMetrics();
    SDL_Log("=== Latency measurement system initialized ===");

    // The renderer is shared by all widgets and visualization routines. Initialization failures
    // return SDL_APP_FAILURE.
    if (!SDL_CreateWindowAndRenderer("Anonymix Scoobs", 1595, 1040, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // The icon surface can be discarded after SDL copies it. Keep the missing asset optional:
    // a portable build can still start when logo.bmp is absent.
    SDL_Surface* logoSurface = SDL_LoadBMP("logo.bmp");
    if (!logoSurface) {
        SDL_Log("Cannot load logo.bmp: %s", SDL_GetError());
    }
    else {
        SDL_SetWindowIcon(window, logoSurface);
        SDL_DestroySurface(logoSurface);
        SDL_Log("Window icon set from logo.bmp");
    }

    gLogoTexture = nullptr;
    gLogoRect = { 0.f, 0.f, 0.f, 0.f };

    // SDL3_ttf returns true on successful initialization. Open one font at the size used by
    // the labels, so gFont owns a single handle that SDL_AppQuit can close.
    if (!TTF_Init()) {
        SDL_Log("TTF_Init failed: %s", SDL_GetError());
        gFont = nullptr;
    }
    else {
        gFont = TTF_OpenFont("C:/Windows/Fonts/Arial.ttf", 27);
        if (!gFont) gFont = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 27);
        if (!gFont) SDL_Log("Warning: could not open a font. Text labels will be skipped.");
    }

    SDL_GetMouseState(&mx, &my);

    hoveredWidget = nullptr;

// Widget names are lookup keys used elsewhere; changes must be applied to every lookup.
    widgets.add("AnimatedWidget");
    widgets("AnimatedWidget").setType(WidgetType::Static_DynaZoom);
    widgets("AnimatedWidget").setHalfDiag1(50);
    widgets("AnimatedWidget").setHalfDiag2(50);
    widgets("AnimatedWidget").setCenterX(200);
    widgets("AnimatedWidget").setCenterY(240);
    widgets("AnimatedWidget").setWidgetColorRGB(0.0, 0.0, 0.0);

    // These layout values come from the earlier UI. The buttons below are registered by stable
    // names, not their screen positions.
    constexpr float MARGIN_H = 30.0f;
    constexpr float MARGIN_V = 25.0f;
    constexpr float ZOOM_SAFE = 1.20f;

    // The caption is also used as a state comparison in the action case; updating displayed
    // wording requires changing both strings.
    widgets.add("AudioMode");
    widgets("AudioMode").setType(WidgetType::Button_StateMono);
    widgets("AudioMode").setButtonFunction(WidgetAction::AudioMode_Toggle);
    widgets("AudioMode").setWidgetColorRGB(0.8f, 0.8f, 0.8f);
    widgets("AudioMode").setRectangleShape(190, 40);
    widgets("AudioMode").setCenterX(1120);
    widgets("AudioMode").setCenterY(65);
    widgets("AudioMode").setText("Active mode: Recording");

    // Reopen the default devices on demand after changing or reconnecting a microphone or
    // speaker. The same control is present in both backend builds for a direct A/B trial.
    widgets.add("AudioDevices");
    widgets("AudioDevices").setType(WidgetType::Button_StateMono);
    widgets("AudioDevices").setButtonFunction(WidgetAction::AudioDevices_Reopen);
    widgets("AudioDevices").setWidgetColorRGB(0.55f, 0.85f, 1.0f);
    widgets("AudioDevices").setRectangleShape(95, 40);
    widgets("AudioDevices").setCenterX(1435);
    widgets("AudioDevices").setCenterY(65);
    widgets("AudioDevices").setText("Reopen Audio");

    // Record, play and save buttons initially store a Start/Save WidgetAction; start/stop actions
    // change the same widget later.
    widgets.add("PA_Record");
    widgets("PA_Record").setType(WidgetType::Button_StateMono);
    widgets("PA_Record").setButtonFunction(WidgetAction::PA_AudioRecord_Start);
    widgets("PA_Record").setWidgetColorRGB(1.0, 0.55, 0.55);
    widgets("PA_Record").setRectangleShape(100, 48);
    widgets("PA_Record").setCenterX(760);
    widgets("PA_Record").setCenterY(190);
    widgets("PA_Record").setText("Start Rec");

    widgets.add("PA_Play");
    widgets("PA_Play").setType(WidgetType::Button_StateMono);
    widgets("PA_Play").setButtonFunction(WidgetAction::PA_AudioPlay_Start);
    widgets("PA_Play").setWidgetColorRGB(1.0, 1.0, 0.55);
    widgets("PA_Play").setRectangleShape(85, 35);
    widgets("PA_Play").setCenterX(925);
    widgets("PA_Play").setCenterY(310);
    widgets("PA_Play").setText("Play OG");

    widgets.add("PA_PlayTimbre");
    widgets("PA_PlayTimbre").setType(WidgetType::Button_StateMono);
    widgets("PA_PlayTimbre").setButtonFunction(WidgetAction::PA_AudioPlayTimbre_Start);
    widgets("PA_PlayTimbre").setWidgetColorRGB(0.7, 0.9, 0.7);
    widgets("PA_PlayTimbre").setRectangleShape(85, 35);
    widgets("PA_PlayTimbre").setCenterX(735);
    widgets("PA_PlayTimbre").setCenterY(310);
    widgets("PA_PlayTimbre").setText("Play Timbre");

    // Playback choices share paAudioBuffer but have independent callback streams and positions in
    // State.cpp.
    widgets.add("PA_PlayAnonPitchFormant");
    widgets("PA_PlayAnonPitchFormant").setType(WidgetType::Button_StateMono);
    widgets("PA_PlayAnonPitchFormant").setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitchFormant_Start);
    widgets("PA_PlayAnonPitchFormant").setWidgetColorRGB(1.0, 0.55, 1.0);
    widgets("PA_PlayAnonPitchFormant").setRectangleShape(85, 35);
    widgets("PA_PlayAnonPitchFormant").setCenterX(1115);
    widgets("PA_PlayAnonPitchFormant").setCenterY(310);
    widgets("PA_PlayAnonPitchFormant").setText("Play P+F");

    widgets.add("PA_PlayAnonPitch");
    widgets("PA_PlayAnonPitch").setType(WidgetType::Button_StateMono);
    widgets("PA_PlayAnonPitch").setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitch_Start);
    widgets("PA_PlayAnonPitch").setWidgetColorRGB(0.55, 1.0, 0.55);
    widgets("PA_PlayAnonPitch").setRectangleShape(85, 35);
    widgets("PA_PlayAnonPitch").setCenterX(1305);
    widgets("PA_PlayAnonPitch").setCenterY(310);
    widgets("PA_PlayAnonPitch").setText("Play Pitch");

    widgets.add("PA_PlayAnonFormant");
    widgets("PA_PlayAnonFormant").setType(WidgetType::Button_StateMono);
    widgets("PA_PlayAnonFormant").setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedFormant_Start);
    widgets("PA_PlayAnonFormant").setWidgetColorRGB(0.55, 0.85, 1.0);
    widgets("PA_PlayAnonFormant").setRectangleShape(90, 35);
    widgets("PA_PlayAnonFormant").setCenterX(1495);
    widgets("PA_PlayAnonFormant").setCenterY(310);
    widgets("PA_PlayAnonFormant").setText("Play Formant");

    // Saving runs synchronously from SDL_AppEvent on a copy or on the original bytes, depending on
    // the action.
    widgets.add("PA_SaveOriginal");
    widgets("PA_SaveOriginal").setType(WidgetType::Button_StateMono);
    widgets("PA_SaveOriginal").setButtonFunction(WidgetAction::PA_SaveOriginal);
    widgets("PA_SaveOriginal").setWidgetColorRGB(0.55, 0.85, 1.0);
    widgets("PA_SaveOriginal").setRectangleShape(85, 35);
    widgets("PA_SaveOriginal").setCenterX(925);
    widgets("PA_SaveOriginal").setCenterY(410);
    widgets("PA_SaveOriginal").setText("Save OG");

    widgets.add("PA_SaveTimbre");
    widgets("PA_SaveTimbre").setType(WidgetType::Button_StateMono);
    widgets("PA_SaveTimbre").setButtonFunction(WidgetAction::PA_SaveTimbre);
    widgets("PA_SaveTimbre").setWidgetColorRGB(0.8, 0.9, 0.6);
    widgets("PA_SaveTimbre").setRectangleShape(85, 35);
    widgets("PA_SaveTimbre").setCenterX(735);
    widgets("PA_SaveTimbre").setCenterY(410);
    widgets("PA_SaveTimbre").setText("Save Timbre");

    widgets.add("PA_SaveAnonPitchFormant");
    widgets("PA_SaveAnonPitchFormant").setType(WidgetType::Button_StateMono);
    widgets("PA_SaveAnonPitchFormant").setButtonFunction(WidgetAction::PA_SaveAnonymizedPitchFormant);
    widgets("PA_SaveAnonPitchFormant").setWidgetColorRGB(1.0, 0.85, 0.55);
    widgets("PA_SaveAnonPitchFormant").setRectangleShape(85, 35);
    widgets("PA_SaveAnonPitchFormant").setCenterX(1115);
    widgets("PA_SaveAnonPitchFormant").setCenterY(410);
    widgets("PA_SaveAnonPitchFormant").setText("Save P+F");

    widgets.add("PA_SaveAnonPitch");
    widgets("PA_SaveAnonPitch").setType(WidgetType::Button_StateMono);
    widgets("PA_SaveAnonPitch").setButtonFunction(WidgetAction::PA_SaveAnonymizedPitch);
    widgets("PA_SaveAnonPitch").setWidgetColorRGB(0.85, 0.55, 1.0);
    widgets("PA_SaveAnonPitch").setRectangleShape(85, 35);
    widgets("PA_SaveAnonPitch").setCenterX(1305);
    widgets("PA_SaveAnonPitch").setCenterY(410);
    widgets("PA_SaveAnonPitch").setText("Save Pitch");

    widgets.add("PA_SaveAnonFormant");
    widgets("PA_SaveAnonFormant").setType(WidgetType::Button_StateMono);
    widgets("PA_SaveAnonFormant").setButtonFunction(WidgetAction::PA_SaveAnonymizedFormant);
    widgets("PA_SaveAnonFormant").setWidgetColorRGB(1.0, 0.85, 0.55);
    widgets("PA_SaveAnonFormant").setRectangleShape(90, 35);
    widgets("PA_SaveAnonFormant").setCenterX(1495);
    widgets("PA_SaveAnonFormant").setCenterY(410);
    widgets("PA_SaveAnonFormant").setText("Save Formant");

    // Vertical pitch control: pixels increase downward, but the displayed semitone value increases
    // upward.
    widgets.add("Slider_PitchShift");
    widgets("Slider_PitchShift").setType(WidgetType::SliderV_FreeLimited);
    widgets("Slider_PitchShift").setBgColor(0.3f, 0.3f, 0.55f);
    widgets("Slider_PitchShift").setHalfDiag1(55);
    widgets("Slider_PitchShift").setHalfDiag2(40);
    widgets("Slider_PitchShift").setCenterX(925);
    widgets("Slider_PitchShift").setCenterY(740);
    widgets("Slider_PitchShift").setTriangleColorRGB(0, 0.0, 0.65, 0.0);
    widgets("Slider_PitchShift").setTriangleColorRGB(1, 0.0, 1.0, 0.0);
    widgets("Slider_PitchShift").setPointColorRGB(0, 1, 0.0, 0.15, 0.0);
    widgets("Slider_PitchShift").setPointColorRGB(1, 1, 0.75, 1.0, 0.75);

    widgets("Slider_PitchShift").setLegendY_Unit(WidgetUnit::Pts);
    widgets("Slider_PitchShift").setLegend_yMin_Val(-12.0);
    widgets("Slider_PitchShift").setLegend_yMax_Val(12.0);
    widgets("Slider_PitchShift").setLegend_yMin_Text("-12 ST");
    widgets("Slider_PitchShift").setLegend_yMax_Text("+12 ST");

    // Convert the +3 semitone default from [-12, +12] into the corresponding downward pixel
    // offset.
    double initialPosY = widgets("Slider_PitchShift").getSlider_yMin() +
        (12.0 - 3.0) / 24.0 * (widgets("Slider_PitchShift").getSlider_yMax() -
            widgets("Slider_PitchShift").getSlider_yMin());
    widgets("Slider_PitchShift").setHandleCenterY(initialPosY);

    // Formant slider uses a display range of 0.5..2.0 and starts near 1.18.
    widgets.add("Slider_FormantShift");
    widgets("Slider_FormantShift").setType(WidgetType::SliderV_FreeLimited);
    widgets("Slider_FormantShift").setBgColor(0.55f, 0.3f, 0.3f);
    widgets("Slider_FormantShift").setHalfDiag1(55);
    widgets("Slider_FormantShift").setHalfDiag2(40);
    widgets("Slider_FormantShift").setCenterX(1115);
    widgets("Slider_FormantShift").setCenterY(740);
    widgets("Slider_FormantShift").setTriangleColorRGB(0, 0.65, 0.0, 0.0);
    widgets("Slider_FormantShift").setTriangleColorRGB(1, 1.0, 0.0, 0.0);
    widgets("Slider_FormantShift").setPointColorRGB(0, 1, 0.15, 0.0, 0.0);
    widgets("Slider_FormantShift").setPointColorRGB(1, 1, 1.0, 0.75, 0.75);

    widgets("Slider_FormantShift").setLegendY_Unit(WidgetUnit::Pts);
    widgets("Slider_FormantShift").setLegend_yMin_Val(0.5);
    widgets("Slider_FormantShift").setLegend_yMax_Val(2.0);
    widgets("Slider_FormantShift").setLegend_yMin_Text("0.5x");
    widgets("Slider_FormantShift").setLegend_yMax_Text("2.0x");

    double initialPosY_Formant = widgets("Slider_FormantShift").getSlider_yMin() +
        (2.0 - 1.18) / 1.5 * (widgets("Slider_FormantShift").getSlider_yMax() -
            widgets("Slider_FormantShift").getSlider_yMin());
    widgets("Slider_FormantShift").setHandleCenterY(initialPosY_Formant);

    // Synchronize the initial stored legend value with the visual handle position; the UI
    // publishes this value each frame.
    widgets("Slider_FormantShift").setSlider_yVal(initialPosY_Formant);

    SDL_Log(">>> Formant slider initialized: position=%.1f | value=%.3f <<<",
        initialPosY_Formant,
        widgets("Slider_FormantShift").getLegend_yVal());

    // One independent live timbre slider: the centre is transparent (0%), lower values
    // attenuate higher frequencies, and upper values raise them. Its IIR filter keeps no
    // additional audio block, so changing this value adds no algorithmic buffering.
    widgets.add("Slider_Timbre");
    widgets("Slider_Timbre").setType(WidgetType::SliderV_FreeLimited);
    widgets("Slider_Timbre").setBgColor(0.25f, 0.50f, 0.55f);
    widgets("Slider_Timbre").setHalfDiag1(55);
    widgets("Slider_Timbre").setHalfDiag2(40);
    widgets("Slider_Timbre").setCenterX(735);
    widgets("Slider_Timbre").setCenterY(740);
    widgets("Slider_Timbre").setTriangleColorRGB(0, 0.0, 0.70, 0.75);
    widgets("Slider_Timbre").setTriangleColorRGB(1, 0.25, 1.0, 1.0);
    widgets("Slider_Timbre").setPointColorRGB(0, 1, 0.0, 0.35, 0.40);
    widgets("Slider_Timbre").setPointColorRGB(1, 1, 0.75, 1.0, 1.0);
    widgets("Slider_Timbre").setLegendY_Unit(WidgetUnit::percent);
    widgets("Slider_Timbre").setLegend_yMin_Val(-100.0);
    widgets("Slider_Timbre").setLegend_yMax_Val(100.0);
    widgets("Slider_Timbre").setLegend_yMin_Text("Dark -100%");
    widgets("Slider_Timbre").setLegend_yMax_Text("Bright +100%");
    const double initialPosY_Timbre =
        (widgets("Slider_Timbre").getSlider_yMin() +
         widgets("Slider_Timbre").getSlider_yMax()) * 0.5;
    widgets("Slider_Timbre").setHandleCenterY(initialPosY_Timbre);
    widgets("Slider_Timbre").setSlider_yVal(initialPosY_Timbre);

    // Jitter uses the small 0..0.02 range. AppUpdate publishes its displayed value to the
    // callback.
    widgets.add("Slider_Jitter");
    widgets("Slider_Jitter").setType(WidgetType::SliderV_FreeLimited);
    widgets("Slider_Jitter").setBgColor(0.3f, 0.55f, 0.3f);
    widgets("Slider_Jitter").setHalfDiag1(55);
    widgets("Slider_Jitter").setHalfDiag2(40);
    widgets("Slider_Jitter").setCenterX(1305);
    widgets("Slider_Jitter").setCenterY(740);
    widgets("Slider_Jitter").setTriangleColorRGB(0, 0.0, 0.65, 0.0);
    widgets("Slider_Jitter").setTriangleColorRGB(1, 0.0, 1.0, 0.0);

    widgets("Slider_Jitter").setLegendY_Unit(WidgetUnit::Pts);
    widgets("Slider_Jitter").setLegend_yMin_Val(0.0);
    widgets("Slider_Jitter").setLegend_yMax_Val(0.02);
    widgets("Slider_Jitter").setLegend_yMin_Text("0.000");
    widgets("Slider_Jitter").setLegend_yMax_Text("0.020");

    initialPosY = widgets("Slider_Jitter").getSlider_yMin() +
        (0.02 - 0.005) / 0.02 * (widgets("Slider_Jitter").getSlider_yMax() -
            widgets("Slider_Jitter").getSlider_yMin());
    widgets("Slider_Jitter").setHandleCenterY(initialPosY);

    // The UI displays 0..100 percent; AppUpdate divides by 100 before storing the audio volume
    // multiplier.
    widgets.add("Slider_Volume");
    widgets("Slider_Volume").setType(WidgetType::SliderV_FreeLimited);
    widgets("Slider_Volume").setBgColor(0.55f, 0.55f, 0.3f);
    widgets("Slider_Volume").setHalfDiag1(55);
    widgets("Slider_Volume").setHalfDiag2(40);
    widgets("Slider_Volume").setCenterX(1495);
    widgets("Slider_Volume").setCenterY(740);
    widgets("Slider_Volume").setTriangleColorRGB(0, 0.65, 0.65, 0.0);
    widgets("Slider_Volume").setTriangleColorRGB(1, 1.0, 1.0, 0.0);

    widgets("Slider_Volume").setLegendY_Unit(WidgetUnit::percent);
    widgets("Slider_Volume").setLegend_yMin_Val(0.0);
    widgets("Slider_Volume").setLegend_yMax_Val(100.0);
    widgets("Slider_Volume").setLegend_yMin_Text("0%");
    widgets("Slider_Volume").setLegend_yMax_Text("100%");

    initialPosY = widgets("Slider_Volume").getSlider_yMin() +
        (100.0 - 50.0) / 100.0 * (widgets("Slider_Volume").getSlider_yMax() -
            widgets("Slider_Volume").getSlider_yMin());
    widgets("Slider_Volume").setHandleCenterY(initialPosY);

    // This text box shows the current hovered or dragged slider values; AppRender updates its
    // content.
    widgets.add("WidgetInfo");
    widgets("WidgetInfo").setType(WidgetType::TextBox);
    widgets("WidgetInfo").setWidgetColorRGB(0.55, 0.55, 1.0);
    widgets("WidgetInfo").setRectangleShape(300, 55);
    widgets("WidgetInfo").setCenterX(1230);
    widgets("WidgetInfo").setCenterY(190);

    // Seed the FFT history so the first display calculation has a full window, then apply initial
    // smoothing.
    paFftInputBuffer.resize(PA_FFT_SIZE, 0.0f);

    ComputeFFT_PortAudio();
    SmoothFFTDisplay_PortAudio(0.25f);

    paFftDataReady = true;

    return SDL_APP_CONTINUE;
}

// Run button actions on mouse release only when the pointer remains over the same pressed widget.
SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
    // SDL asks to terminate through this event; returning SDL_APP_SUCCESS triggers shutdown.
    if (event->type == SDL_EVENT_QUIT) {
        return SDL_APP_SUCCESS;
    }

    // A press records the widget selected by RenderFrame hit testing and the pointer-to-handle
    // offset.
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (event->button.button == SDL_BUTTON_LEFT) {

            SDL_Log(">>> MOUSE DOWN at (%.0f, %.0f) - hoveredWidget = %s <<<",
                mx, my,
                hoveredWidget ? hoveredWidget->getName().c_str() : "nullptr");

            if (hoveredWidget == nullptr) {
                pressedWidget = nullptr;
            }
            else {
                pressedWidget = hoveredWidget;
                pressedWidget->set_isMouseLeftButtonDown(true);

                // Store the click offset; RenderFrame subtracts it while dragging to retain the
                // original grab point.
                mouseClick_OFFSET.x = mx - pressedWidget->getHandleCenterX();
                mouseClick_OFFSET.y = my - pressedWidget->getHandleCenterY();

                SDL_Log(">>> Calculated offset: (%.2f, %.2f) <<<",
                    mouseClick_OFFSET.x, mouseClick_OFFSET.y);

                pressedWidget->updatePressedColorRGB();
            }
        }
    }
    // A release acts only on a left-button press that still points at the same hovered button.
    else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event->button.button == SDL_BUTTON_LEFT) {
            SDL_Log(">>> MOUSE CLICK at position (%.0f, %.0f) <<<", mx, my);
            SDL_Log(">>> pressedWidget = %s <<<",
                pressedWidget ? pressedWidget->getName().c_str() : "nullptr");

            if (pressedWidget != nullptr) {
                SDL_Log(">>> Clicked widget: %s (type: %d) <<<",
                    pressedWidget->getName().c_str(),
                    static_cast<int>(pressedWidget->getType()));

                // Compare widget addresses rather than captions so dragging off a button cancels
                // its action.
                bool isStillOverButton = (hoveredWidget == pressedWidget);

                if (isStillOverButton && pressedWidget->getType() == WidgetType::Button_StateMono &&
                    IsAudioDeviceReopening()) {
                    SDL_Log("Audio controls are unavailable while devices are reopening");
                }
                else if (isStillOverButton && pressedWidget->getType() == WidgetType::Button_StateMono) {
                    SDL_Log(">>> Button action: %d <<<",
                        static_cast<int>(pressedWidget->getButtonFunction()));

                    // Device opens can also pause a playback button. Report every start time
                    // separately so its pause can be compared with Reopen Audio's stages.
                    const auto startPlayback = [](bool (*start)(), const char* name) {
                        const auto startTime = chrono::steady_clock::now();
                        const bool started = start();
                        SDL_Log("%s start %s in %.1f ms", name,
                            started ? "succeeded" : "failed",
                            chrono::duration<double, milli>(chrono::steady_clock::now() - startTime).count());
                        return started;
                    };

                    switch (pressedWidget->getButtonFunction()) {

                    case WidgetAction::PA_AudioPlayTimbre_Start:
                        if (startPlayback(StartPortAudioPlaybackTimbre, "Timbre playback")) {
                            pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlayTimbre_Stop);
                            pressedWidget->setText("Stop Timbre");
                            pressedWidget->setWidgetColorRGB(0.3, 0.75, 0.3);
                        }
                        break;

                    case WidgetAction::PA_AudioPlayTimbre_Stop:
                        StopPortAudioPlaybackTimbre();
                        pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlayTimbre_Start);
                        pressedWidget->setText("Play Timbre");
                        pressedWidget->setWidgetColorRGB(0.7, 0.9, 0.7);
                        break;

                    case WidgetAction::PA_SaveTimbre:
                        pressedWidget->setWidgetColorRGB(SavePortAudioTimbreRecording()
                            ? 0.8 : 1.0, 0.9, 0.6);
                        break;

                    case WidgetAction::AudioDevices_Reopen:
                        pressedWidget->setWidgetColorRGB(ReopenAudioDevices()
                            ? 0.55 : 1.0, 0.85, 1.0);
                        if (IsAudioDeviceReopening()) {
                            pressedWidget->setText("Reopening...");
                            pressedWidget->setWidgetColorRGB(1.0f, 0.85f, 0.45f);
                        }
                        // Any old playback stream was joined and closed by the reopen action.
                        widgets("PA_Play").setButtonFunction(WidgetAction::PA_AudioPlay_Start);
                        widgets("PA_Play").setText("Play OG");
                        widgets("PA_Play").setWidgetColorRGB(1.0, 1.0, 0.55);
                        widgets("PA_PlayAnonPitch").setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitch_Start);
                        widgets("PA_PlayAnonPitch").setText("Play Pitch");
                        widgets("PA_PlayAnonPitch").setWidgetColorRGB(0.55, 1.0, 0.55);
                        widgets("PA_PlayAnonFormant").setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedFormant_Start);
                        widgets("PA_PlayAnonFormant").setText("Play Formant");
                        widgets("PA_PlayAnonFormant").setWidgetColorRGB(0.55, 0.85, 1.0);
                        widgets("PA_PlayAnonPitchFormant").setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitchFormant_Start);
                        widgets("PA_PlayAnonPitchFormant").setText("Play P+F");
                        widgets("PA_PlayAnonPitchFormant").setWidgetColorRGB(1.0, 0.55, 1.0);
                        widgets("PA_PlayTimbre").setButtonFunction(WidgetAction::PA_AudioPlayTimbre_Start);
                        widgets("PA_PlayTimbre").setText("Play Timbre");
                        widgets("PA_PlayTimbre").setWidgetColorRGB(0.7, 0.9, 0.7);
                        if (!g_streamModeActive.load(memory_order_acquire)) {
                            widgets("AudioMode").setText("Active mode: Recording");
                            widgets("AudioMode").setWidgetColorRGB(0.8f, 0.8f, 0.8f);
                        }
                        break;

                    // A failed device reopen must not leave the recording button showing Stop.
                    case WidgetAction::PA_AudioRecord_Start:
                        SDL_Log(">>> Click on PA_AudioRecord_Start <<<");
                        if (paStream != nullptr && StartPortAudioRecording()) {
                            pressedWidget->setButtonFunction(WidgetAction::PA_AudioRecord_Stop);
                            pressedWidget->setText("Stop Rec");
                            pressedWidget->setWidgetColorRGB(1.0, 0.3, 0.3);
                        } else {
                            SDL_Log("Cannot record without an active audio input; press Reopen Audio after reconnecting");
                        }
                        break;

                    case WidgetAction::PA_AudioRecord_Stop:
                        SDL_Log(">>> Click on PA_AudioRecord_Stop <<<");
                        pressedWidget->setButtonFunction(WidgetAction::PA_AudioRecord_Start);
                        pressedWidget->setText("Start Rec");
                        pressedWidget->setWidgetColorRGB(1.0, 0.55, 0.55);
                        StopPortAudioRecording();
                        break;

                    // Show Stop only after the selected output has opened successfully.
                    case WidgetAction::PA_AudioPlay_Start:
                        SDL_Log(">>> Click on PA_AudioPlay_Start <<<");
                        if (startPlayback(StartPortAudioPlayback, "Original playback")) {
                            pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlay_Stop);
                            pressedWidget->setText("Stop Audio");
                            pressedWidget->setWidgetColorRGB(1.0, 0.8, 0.0);
                        }
                        break;

                    case WidgetAction::PA_AudioPlay_Stop:
                        SDL_Log(">>> Click on PA_AudioPlay_Stop <<<");
                        pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlay_Start);
                        pressedWidget->setText("Play OG");
                        pressedWidget->setWidgetColorRGB(1.0, 1.0, 0.55);
                        StopPortAudioPlayback();
                        break;

                    case WidgetAction::PA_AudioPlayAnonymizedPitch_Start:
                        SDL_Log(">>> Click on PA_AudioPlayAnonymized_Start <<<");
                        if (startPlayback(StartPortAudioPlaybackAnonymized, "Pitch playback")) {
                            pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitch_Stop);
                            pressedWidget->setText("Stop Pitch");
                            pressedWidget->setWidgetColorRGB(1.0, 0.5, 0.0);
                        }
                        break;

                    case WidgetAction::PA_AudioPlayAnonymizedPitch_Stop:
                        SDL_Log(">>> Click on PA_AudioPlayAnonymized_Stop <<<");
                        pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitch_Start);
                        pressedWidget->setText("Play Pitch");
                        pressedWidget->setWidgetColorRGB(0.55, 1.0, 0.55);
                        StopPortAudioPlaybackAnonymized();
                        break;

                    case WidgetAction::PA_AudioPlayAnonymizedFormant_Start:
                        SDL_Log(">>> Click on PA_AudioPlayAnonymizedFormant_Start <<<");
                        if (startPlayback(StartPortAudioPlaybackAnonymizedFormant, "Formant playback")) {
                            pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedFormant_Stop);
                            pressedWidget->setText("Stop Formant");
                            pressedWidget->setWidgetColorRGB(0.0, 0.5, 1.0);
                        }
                        break;

                    case WidgetAction::PA_AudioPlayAnonymizedFormant_Stop:
                        SDL_Log(">>> Click on PA_AudioPlayAnonymizedFormant_Stop <<<");
                        pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedFormant_Start);
                        pressedWidget->setText("Play Formant");
                        pressedWidget->setWidgetColorRGB(0.55, 0.85, 1.0);
                        StopPortAudioPlaybackAnonymizedFormant();
                        break;

                    // WAV export starts with a copy of PCM16 bytes. DSP and volume modify the copy
                    // before SaveWAV_Adaptive writes it.
                    case WidgetAction::PA_SaveAnonymizedFormant:
                        SDL_Log(">>> Click on PA_SaveAnonymizedFormant <<<");
                        if (paAudioBuffer.empty()) {
                            SDL_Log("ERROR: No audio to save (empty buffer)");
                            pressedWidget->setWidgetColorRGB(1.0, 0.3, 0.3);
                        }
                        else {
                            vector<Uint8> tempBuffer = paAudioBuffer;

                            float formantShift = g_formantShiftRatio.load(memory_order_acquire);
                            float jitter = g_jitterAmount.load(memory_order_acquire);
                            float volume = g_outputVolume.load(memory_order_acquire);

                            SDL_Log("=== DSP PROCESSING FORMANT (SLIDER SETTINGS) ===");
                            SDL_Log("  Formant: %.2fx | Jitter: %.4f | Volume: %.0f%%",
                                formantShift, jitter, volume * 100.0f);

                            ApplyHighPassFilter(tempBuffer, 80.0f);
                            ApplyRealtimeFormantShift(tempBuffer, formantShift);
                            ApplyJitter(tempBuffer, jitter);
                            NormalizeAudio(tempBuffer);

                            int16_t* samples = reinterpret_cast<int16_t*>(tempBuffer.data());
                            size_t numSamples = tempBuffer.size() / sizeof(int16_t);

                            for (size_t i = 0; i < numSamples; ++i) {
                                float reduced = static_cast<float>(samples[i]) * volume;
                                reduced = Max(-32768.0f, Min(32767.0f, reduced));
                                samples[i] = static_cast<int16_t>(round(reduced));
                            }

                            string formatSuffix = "mono";
                            string filename = GenerateWAVFilename("audio_anonymized_formant_lpc_" + formatSuffix);

                            if (SaveWAV_Adaptive(filename, tempBuffer, 1, PA_SAMPLE_RATE)) {
                                pressedWidget->setWidgetColorRGB(0.3, 1.0, 0.3);
                            }
                            else {
                                pressedWidget->setWidgetColorRGB(1.0, 0.3, 0.3);
                            }
                        }
                        break;

                    // The original-save path scales a temporary PCM16 copy by the volume setting
                    // without applying pitch or formant DSP.
                    case WidgetAction::PA_SaveOriginal:
                        SDL_Log(">>> Click on PA_SaveOriginal <<<");
                        if (paAudioBuffer.empty()) {
                            SDL_Log("ERROR: No audio to save (empty buffer)");
                            pressedWidget->setWidgetColorRGB(1.0, 0.3, 0.3);
                        }
                        else {

                            vector<Uint8> tempBuffer = paAudioBuffer;

                            float volume = g_outputVolume.load(memory_order_acquire);

                            SDL_Log("=== SAVE ORIGINAL AUDIO (VOLUME: %.0f%%) ===", volume * 100.0f);

                            int16_t* samples = reinterpret_cast<int16_t*>(tempBuffer.data());
                            size_t numSamples = tempBuffer.size() / sizeof(int16_t);

                            for (size_t i = 0; i < numSamples; ++i) {
                                float reduced = static_cast<float>(samples[i]) * volume;
                                reduced = Max(-32768.0f, Min(32767.0f, reduced));
                                samples[i] = static_cast<int16_t>(reduced);
                            }

                            string formatSuffix = "mono";
                            string filename = GenerateWAVFilename("audio_original_" + formatSuffix);

                            if (SaveWAV_Adaptive(filename, tempBuffer, 1, PA_SAMPLE_RATE)) {
                                pressedWidget->setWidgetColorRGB(0.3, 1.0, 0.3);
                            }
                            else {
                                pressedWidget->setWidgetColorRGB(1.0, 0.3, 0.3);
                            }
                        }
                        break;

                    // The pitch-save path uses the recorded-data processor in Audio.cpp, then
                    // scales PCM16 samples.
                    case WidgetAction::PA_SaveAnonymizedPitch:
                        SDL_Log(">>> Click on PA_SaveAnonymized <<<");
                        if (paAudioBuffer.empty()) {
                            SDL_Log("ERROR: No audio to save (empty buffer)");
                            pressedWidget->setWidgetColorRGB(1.0, 0.3, 0.3);
                        }
                        else {
                            vector<Uint8> tempBuffer = paAudioBuffer;

                            float pitchShift = g_pitchShiftSemitones.load(memory_order_acquire);
                            float jitter = g_jitterAmount.load(memory_order_acquire);
                            float volume = g_outputVolume.load(memory_order_acquire);

                            SDL_Log("=== DSP PROCESSING Pitch (SLIDER SETTINGS) ===");
                            SDL_Log("  Pitch: %.2f ST | Jitter: %.4f | Volume: %.0f%%",
                                pitchShift, jitter, volume * 100.0f);

                            ApplyHighPassFilter(tempBuffer, 80.0f);
                            ApplyRealtimePitchShift(tempBuffer, pitchShift);
                            ApplyJitter(tempBuffer, jitter);
                            NormalizeAudio(tempBuffer);

                            int16_t* samples = reinterpret_cast<int16_t*>(tempBuffer.data());
                            size_t numSamples = tempBuffer.size() / sizeof(int16_t);

                            for (size_t i = 0; i < numSamples; ++i) {
                                float reduced = static_cast<float>(samples[i]) * volume;
                                reduced = Max(-32768.0f, Min(32767.0f, reduced));
                                samples[i] = static_cast<int16_t>(round(reduced));
                            }

                            string formatSuffix = "mono";
                            string filename = GenerateWAVFilename("audio_anonymized_pitch_" + formatSuffix);

                            if (SaveWAV_Adaptive(filename, tempBuffer, 1, PA_SAMPLE_RATE)) {
                                pressedWidget->setWidgetColorRGB(0.3, 1.0, 0.3);
                            }
                            else {
                                pressedWidget->setWidgetColorRGB(1.0, 0.3, 0.3);
                            }
                        }
                        break;

                    case WidgetAction::PA_AudioPlayAnonymizedPitchFormant_Start:
                        SDL_Log(">>> Click on PA_AudioPlayAnonymizedPitchFormant_Start <<<");
                        if (startPlayback(StartPortAudioPlaybackAnonymizedPitchFormant, "Pitch+formant playback")) {
                            pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitchFormant_Stop);
                            pressedWidget->setText("Stop P+F");
                            pressedWidget->setWidgetColorRGB(1.0, 0.0, 0.5);
                        }
                        break;

                    case WidgetAction::PA_AudioPlayAnonymizedPitchFormant_Stop:
                        SDL_Log(">>> Click on PA_AudioPlayAnonymizedPitchFormant_Stop <<<");
                        pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitchFormant_Start);
                        pressedWidget->setText("Play P+F");
                        pressedWidget->setWidgetColorRGB(1.0, 0.55, 1.0);
                        StopPortAudioPlaybackAnonymizedPitchFormant();
                        break;

                    // The combined-save path runs both recorded-data transformations. It is
                    // separate from live duplex processing.
                    case WidgetAction::PA_SaveAnonymizedPitchFormant:
                        SDL_Log(">>> Click on PA_SaveAnonymizedPitchFormant <<<");
                        if (paAudioBuffer.empty()) {
                            SDL_Log("ERROR: No audio to save (empty buffer)");
                            pressedWidget->setWidgetColorRGB(1.0, 0.3, 0.3);
                        }
                        else {
                            vector<Uint8> tempBuffer = paAudioBuffer;

                            float pitchShift = g_pitchShiftSemitones.load(memory_order_acquire);
                            float formantShift = g_formantShiftRatio.load(memory_order_acquire);
                            float jitter = g_jitterAmount.load(memory_order_acquire);
                            float volume = g_outputVolume.load(memory_order_acquire);

                            SDL_Log("=== DSP PROCESSING PITCH+FORMANT (SLIDER SETTINGS) ===");
                            SDL_Log("  Pitch: %.2f ST | Formant: %.2fx | Jitter: %.4f | Volume: %.0f%%",
                                pitchShift, formantShift, jitter, volume * 100.0f);

                            ApplyHighPassFilter(tempBuffer, 80.0f);
                            ApplyRealtimePitchShift(tempBuffer, pitchShift);
                            ApplyRealtimeFormantShift(tempBuffer, formantShift);
                            ApplyJitter(tempBuffer, jitter);
                            NormalizeAudio(tempBuffer);

                            int16_t* samples = reinterpret_cast<int16_t*>(tempBuffer.data());
                            size_t numSamples = tempBuffer.size() / sizeof(int16_t);

                            for (size_t i = 0; i < numSamples; ++i) {
                                float reduced = static_cast<float>(samples[i]) * volume;
                                samples[i] = static_cast<int16_t>(Max(-32768.0f, Min(32767.0f, reduced)));
                            }

                            string formatSuffix = "mono";
                            string filename = GenerateWAVFilename("audio_anonymized_pitch_formant_" + formatSuffix);

                            if (SaveWAV_Adaptive(filename, tempBuffer, 1, PA_SAMPLE_RATE)) {
                                pressedWidget->setWidgetColorRGB(0.3, 1.0, 0.3);
                            }
                            else {
                                pressedWidget->setWidgetColorRGB(1.0, 0.3, 0.3);
                            }
                        }
                        break;

// Use the public ActivateStreamMode/DeactivateStreamMode API. Private processor pointers are not visible here.
                    case WidgetAction::AudioMode_Toggle:
                        SDL_Log(">>> Click on AudioMode_Toggle <<<");
                        // The visible caption is changed before activation.
                        // ActivateStreamMode returns void, so an allocation
                        // failure can leave the caption out of sync with the
                        // actual atomic mode flag.
                        if (pressedWidget->getText() == "Active mode: Recording") {

                            pressedWidget->setText("Active mode: Stream");
                            pressedWidget->setWidgetColorRGB(0.3f, 1.0f, 0.3f);
                            ActivateStreamMode();
                        }
                        else {

                            pressedWidget->setText("Active mode: Recording");
                            pressedWidget->setWidgetColorRGB(0.8f, 0.8f, 0.8f);
                            DeactivateStreamMode();
                        }
                        break;

                    default:
                        SDL_Log(">>> WARNING: Unhandled button action: %d <<<",
                            static_cast<int>(pressedWidget->getButtonFunction()));
                        break;
                    }
                }
                else if (!isStillOverButton) {
                    SDL_Log(">>> Click canceled: mouse released outside the button <<<");
                }

                pressedWidget->set_isMouseLeftButtonDown(false);
                pressedWidget->updatePressedColorRGB();
            }
            else {
                SDL_Log(">>> NO WIDGET UNDER POINTER <<<");
            }

            // Release the UI press state after the action; callbacks continue independently on
            // PortAudio streams.
            pressedWidget = nullptr;
        }
    }

    return SDL_APP_CONTINUE;
}

// Publish and analyze the current state, then render exactly one SDL frame.
SDL_AppResult SDL_AppIterate(void* appstate)
{
    // Pace the SDL UI at 60 frames per second in every mode. Without a frame limit the default
    // renderer can redraw as fast as the CPU allows, competing with WASAPI during a reopen.
    // Record reopen frame intervals separately so slow driver calls remain visible in logs.
    const auto frameStarted = chrono::steady_clock::now();
    const Uint64 frameStartedNs = SDL_GetTicksNS();
    static chrono::steady_clock::time_point previousReopenFrame{};
    static unsigned reopenFrames = 0;
    static unsigned slowReopenFrames = 0;
    static double longestReopenFrameMs = 0.0;

    const double now = ((double)SDL_GetTicks()) / 1000.0;
    CompleteAudioDeviceReopen();
    const bool reopening = IsAudioDeviceReopening();
    if (reopening) {
        if (reopenFrames != 0) {
            const double intervalMs = chrono::duration<double, milli>(
                frameStarted - previousReopenFrame).count();
            longestReopenFrameMs = max(longestReopenFrameMs, intervalMs);
            if (intervalMs > 33.0) ++slowReopenFrames;
        }
        previousReopenFrame = frameStarted;
        ++reopenFrames;
    } else if (reopenFrames != 0) {
        SDL_Log("Reopen UI: %u frames, %u intervals over 33 ms, longest %.1f ms",
            reopenFrames, slowReopenFrames, longestReopenFrameMs);
        reopenFrames = 0;
        slowReopenFrames = 0;
        longestReopenFrameMs = 0.0;
    }
    // Playback completion can close PortAudio streams, so skip that work while the worker owns
    // the devices. SDL continues rendering the existing visualization during the transition.
    if (!reopening) UpdatePlaybackAndAnalysis();
    RenderFrame(now);
    // Wait for the unspent portion of this visual frame. If drawing took too long, do not
    // delay further: the next frame starts immediately instead of accumulating lateness.
    constexpr Uint64 targetFrameNs = 1000000000ULL / 60;
    const Uint64 deadlineNs = frameStartedNs + targetFrameNs;
    const Uint64 renderedNs = SDL_GetTicksNS();
    if (renderedNs < deadlineNs)
        SDL_DelayPrecise(deadlineNs - renderedNs);
    return SDL_APP_CONTINUE;
}

// Release application-owned resources during SDL shutdown. Stop and join the audio callbacks
// before deleting any state they read; this prevents the inherited teardown race.
void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
    // Close PortAudio on the worker thread that initialized WASAPI/COM, then join it before
    // freeing callback-visible application state. A pending reopen finishes before shutdown.
    {
        lock_guard<mutex> lock(g_audioWorkerMutex);
        g_audioWorkerShouldStop = true;
    }
    g_audioWorkerWake.notify_one();
    if (g_portAudioDeviceWorker.joinable()) g_portAudioDeviceWorker.join();

    if (gLogoTexture) {
        SDL_DestroyTexture(gLogoTexture);
        gLogoTexture = nullptr;
    }

    CleanupStreamMode();

    if (g_latencyMetrics) {
        delete g_latencyMetrics;
        g_latencyMetrics = nullptr;
    }

    if (gFont) {
        TTF_CloseFont(gFont);
        gFont = nullptr;
    }
    TTF_Quit();

}
