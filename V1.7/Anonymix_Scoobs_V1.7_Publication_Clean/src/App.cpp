// SDL3 callback entry points: initialization, input events, frame update and shutdown. Widget actions are dispatched from SDL_AppEvent.

#define SDL_MAIN_USE_CALLBACKS 1
#ifdef _WIN32
// Include the Windows COM declarations before Common.h exposes std names globally.
// Otherwise Windows SDK's byte and std::byte become ambiguous in the SDK headers.
// Windows' min/max macros would also break std::numeric_limits in Widgets.h.
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <objbase.h>
#if defined(_MSC_VER)
#pragma comment(lib, "Ole32.lib")
#endif
#endif
#include <SDL3/SDL_main.h>
#include "Widgets.h"
#include "Stream.h"
#include "Dsp.h"
#include "Wav.h"
#include "AppFrame.h"
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
// 0: idle; 1/2: starting/stopping; 3/4/5: started/failed/stopped awaiting UI delivery.
static atomic<int> g_recordedPlaybackTransition{0};

// Use the same unmistakable ON/OFF palette when selectors are created and toggled.
// Light neutral buttons stay legible over the animated green background.
static void SetEffectButtonAppearance(UiWidget& button, bool enabled) {
    if (enabled) {
        button.setWidgetColorRGB(0.05f, 0.38f, 0.16f);
        button.setTextColor(255, 255, 255);
    } else {
        button.setWidgetColorRGB(0.70f, 0.74f, 0.76f);
        button.setTextColor(20, 28, 34);
    }
}

bool IsAudioDeviceReopening() {
    return g_audioReopenState.load(memory_order_acquire) == 1;
}

// Closing and enumerating a Windows WASAPI duplex stream can take seconds. This routine runs
// on the worker, with no widget or SDL renderer calls.
static bool PerformAudioDeviceReopen(bool restoreLiveMode, bool portAudioWasReady = true) {
    if (portAudioWasReady) CleanupPortAudio();
    paIsPlayingSelected.store(false, memory_order_release);

    const bool recovered = InitPortAudio();
    if (recovered && restoreLiveMode)
        g_streamModeActive.store(true, memory_order_release);
    return recovered;
}

// Keep initialization, stream operations and termination on one long-lived WASAPI thread.
static void PortAudioDeviceWorkerLoop() {
#ifdef _WIN32
    // COM apartments belong to threads. SDL's COM setup on the UI thread does not
    // initialize the PortAudio device worker that enumerates Windows audio endpoints.
    // A successful S_FALSE also requires a matching CoUninitialize on this thread.
    struct WorkerComApartment final {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~WorkerComApartment() {
            if (SUCCEEDED(result)) CoUninitialize();
        }
    } comApartment;
#endif
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
            catch (...) { /* The UI receives a failed device action. */ }
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

static bool RequestRecordedPlaybackAction(bool start) {
    {
        lock_guard<mutex> lock(g_audioWorkerMutex);
        if (!g_audioWorkerInitialized || !g_audioWorkerReady || g_audioWorkerShouldStop ||
            g_audioReopenState.load(memory_order_acquire) != 0 ||
            g_recordedPlaybackTransition.load(memory_order_acquire) != 0 ||
            g_audioWorkerAction || !g_audioWorkerActionDone) return false;
        g_recordedPlaybackTransition.store(start ? 1 : 2, memory_order_release);
        g_audioWorkerActionDone = false;
        // The worker owns every PortAudio open/close. Publish only the result here;
        // SDL changes button text after observing that result on its next frame.
        g_audioWorkerAction = [start] {
            bool succeeded = false;
            try {
                if (start) succeeded = StartPortAudioPlaybackSelected();
                else { StopPortAudioPlaybackSelected(); succeeded = true; }
            } catch (...) {
            }
            g_recordedPlaybackTransition.store(start ? (succeeded ? 3 : 4) : 5,
                memory_order_release);
            return succeeded;
        };
    }
    g_audioWorkerWake.notify_one();
    return true;
}

bool RequestRecordedPlaybackStart() { return RequestRecordedPlaybackAction(true); }
bool RequestRecordedPlaybackStop() { return RequestRecordedPlaybackAction(false); }

bool IsRecordedPlaybackTransitionPending() {
    return g_recordedPlaybackTransition.load(memory_order_acquire) != 0;
}

RecordedPlaybackResult TakeRecordedPlaybackResult() {
    lock_guard<mutex> lock(g_audioWorkerMutex);
    if (!g_audioWorkerActionDone) return RecordedPlaybackResult::None;
    int state = g_recordedPlaybackTransition.load(memory_order_acquire);
    if (state < 3 || state > 5 ||
        !g_recordedPlaybackTransition.compare_exchange_strong(state, 0,
            memory_order_acq_rel)) return RecordedPlaybackResult::None;
    return state == 3 ? RecordedPlaybackResult::Started :
        state == 4 ? RecordedPlaybackResult::StartFailed :
        RecordedPlaybackResult::Stopped;
}

// Return true when a request was accepted. Completion is asynchronous;
// CompleteAudioDeviceReopen publishes the actual success/failure on SDL's main thread.
bool ReopenAudioDevices() {
    if (IsRecordedPlaybackTransitionPending()) return false;
    if (paIsRecording.load(memory_order_acquire)) {
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
        widgets("AudioMode").setText("Mode: Stream");
        widgets("AudioMode").setWidgetColorRGB(0.3f, 1.0f, 0.3f);
    } else {
        widgets("AudioMode").setText("Mode: Recording");
        widgets("AudioMode").setWidgetColorRGB(0.8f, 0.8f, 0.8f);
    }
    const int openedWasapiMode = g_activeWasapiMode.load(memory_order_acquire);
    widgets("WasapiMode").setText(openedWasapiMode == 1 ? "WASAPI: Excl." :
        openedWasapiMode == 0 ? "WASAPI: Shared" :
        openedWasapiMode == -1 ? "Other Host" : "No Audio");
    widgets("WasapiMode").setWidgetColorRGB(openedWasapiMode == 1 ? 0.95f : 0.60f,
        openedWasapiMode == 1 ? 0.72f : 0.85f, 0.95f);
    g_audioReopenState.store(0, memory_order_release);
}

// Allocate visualization state, initialize SDL and PortAudio, create the window, and register all
// named controls.
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{

    // Set up inherited pixel-format state before drawing. Visualization buffers must be sized
    // before callbacks use them.

    waveformBuffer.resize(WAVEFORM_SAMPLES, 0.0f);
    paFftInputBuffer.resize(PA_FFT_SIZE * 2, 0.0f);
    paFftWritePos = PA_FFT_SIZE;
    paFftSamplesAvailable = PA_FFT_SIZE;
    paFftMagnitudes.resize(PA_NUM_BARS, 0.0f);
    paFftSmoothed.resize(PA_NUM_BARS, 0.0f);
    paAdaptiveNoiseFloor.resize(PA_NUM_BARS, 0.0f);


    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return SDL_APP_FAILURE;
    }

    // Initialize the live duplex stream on the worker that later owns all device operations.
    try {
        g_portAudioDeviceWorker = thread(PortAudioDeviceWorkerLoop);
        unique_lock<mutex> lock(g_audioWorkerMutex);
        g_audioWorkerWake.wait(lock, [] { return g_audioWorkerInitialized; });
    }
    catch (const system_error&) {
    }


    // The renderer is shared by all widgets and visualization routines. Initialization failures
    // return SDL_APP_FAILURE.
    if (!SDL_CreateWindowAndRenderer("Anonymix Scoobs", 1430, 1040, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        return SDL_APP_FAILURE;
    }
    // Geometry uses per-vertex alpha for the antialiased spectrum outline.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // The icon surface can be discarded after SDL copies it. Keep the missing asset optional:
    // a portable build can still start when logo.bmp is absent.
    SDL_Surface* logoSurface = SDL_LoadBMP("logo.bmp");
    if (logoSurface) {
        SDL_SetWindowIcon(window, logoSurface);
        SDL_DestroySurface(logoSurface);
    }


    // Rasterize letters at twice their logical UI size. The whole interface is
    // drawn to a 2x target and reduced once, so diagonal letter strokes gain
    // the same pixel coverage as diagonal widget edges.
    if (!TTF_Init()) {
        gFont = nullptr;
    }
    else {
        gFont = TTF_OpenFont("C:/Windows/Fonts/Arial.ttf", 54);
        if (!gFont) gFont = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 54);
    }

    SDL_GetMouseState(&mx, &my);

    hoveredWidget = nullptr;

// Widget names are lookup keys used elsewhere; changes must be applied to every lookup.
    widgets.add("AnimatedWidget");
    widgets("AnimatedWidget").setType(WidgetType::Static_DynaZoom);
    widgets("AnimatedWidget").setHalfDiag1(50);
    widgets("AnimatedWidget").setHalfDiag2(50);
    widgets("AnimatedWidget").setCenterX(320);
    widgets("AnimatedWidget").setCenterY(240);
    widgets("AnimatedWidget").setWidgetColorRGB(0.0, 0.0, 0.0);

    // Button names stay stable across the event, update and render modules.

    // The three top buttons share the same y axis and span the lavender panel's x range.
    // AudioMode is narrower to make room for a WASAPI share-mode selector.
    widgets.add("AudioMode");
    widgets("AudioMode").setType(WidgetType::Button_StateMono);
    widgets("AudioMode").setButtonFunction(WidgetAction::AudioMode_Toggle);
    widgets("AudioMode").setWidgetColorRGB(0.8f, 0.8f, 0.8f);
    widgets("AudioMode").setRectangleShape(140, 40);
    widgets("AudioMode").setCenterX(800);
    widgets("AudioMode").setCenterY(65);
    widgets("AudioMode").setText("Mode: Recording");

    widgets.add("WasapiMode");
    widgets("WasapiMode").setType(WidgetType::Button_StateMono);
    widgets("WasapiMode").setButtonFunction(WidgetAction::WasapiMode_Toggle);
    widgets("WasapiMode").setRectangleShape(100, 40);
    widgets("WasapiMode").setCenterX(1060);
    widgets("WasapiMode").setCenterY(65);
    const int startupWasapiMode = g_activeWasapiMode.load(memory_order_acquire);
    widgets("WasapiMode").setText(startupWasapiMode == 1 ? "WASAPI: Excl." :
        startupWasapiMode == 0 ? "WASAPI: Shared" :
        startupWasapiMode == -1 ? "Other Host" : "No Audio");
    widgets("WasapiMode").setWidgetColorRGB(0.60f, 0.85f, 0.95f);

    // Reopen the default devices on demand after changing or reconnecting a microphone or
    // speaker. This operation stays on the PortAudio device worker.
    widgets.add("AudioDevices");
    widgets("AudioDevices").setType(WidgetType::Button_StateMono);
    widgets("AudioDevices").setButtonFunction(WidgetAction::AudioDevices_Reopen);
    widgets("AudioDevices").setWidgetColorRGB(0.55f, 0.85f, 1.0f);
    widgets("AudioDevices").setRectangleShape(115, 40);
    widgets("AudioDevices").setCenterX(1295);
    widgets("AudioDevices").setCenterY(65);
    widgets("AudioDevices").setText("Reopen Audio");

    // Record, play and save buttons initially store a Start/Save WidgetAction; start/stop actions
    // change the same widget later.
    widgets.add("PA_Record");
    widgets("PA_Record").setType(WidgetType::Button_StateMono);
    widgets("PA_Record").setButtonFunction(WidgetAction::PA_AudioRecord_Start);
    widgets("PA_Record").setWidgetColorRGB(1.0, 0.55, 0.55);
    widgets("PA_Record").setRectangleShape(90, 35);
    widgets("PA_Record").setCenterX(1130);
    widgets("PA_Record").setCenterY(300);
    widgets("PA_Record").setText("Start Rec");

    // Effect buttons are independent toggles. Any combination can be previewed or saved.
    // All recorded previews share the one Play control and a single PortAudio stream.
    const auto addEffectButton = [&](const string& name, WidgetAction action,
        float centerX, float centerY, const string& label, bool enabled) {
        widgets.add(name);
        auto& button = widgets(name);
        button.setType(WidgetType::Button_StateMono);
        button.setButtonFunction(action);
        SetEffectButtonAppearance(button, enabled);
        button.setRectangleShape(90, 35);
        button.setCenterX(centerX);
        button.setCenterY(centerY);
        button.setText(label + (enabled ? ": ON" : ": OFF"));
    };
    addEffectButton("EffectPitch", WidgetAction::EffectPitch_Toggle,
        750, 300, "Pitch", true);
    addEffectButton("EffectFormant", WidgetAction::EffectFormant_Toggle,
        940, 300, "Formant", true);
    addEffectButton("EffectTimbre", WidgetAction::EffectTimbre_Toggle,
        750, 385, "Timbre", false);
    addEffectButton("EffectProsody", WidgetAction::EffectProsody_Toggle,
        940, 385, "Prosody", false);
    addEffectButton("EffectJitter", WidgetAction::EffectJitter_Toggle,
        1130, 385, "Jitter", false);

    widgets.add("PA_PlaySelected");
    widgets("PA_PlaySelected").setType(WidgetType::Button_StateMono);
    widgets("PA_PlaySelected").setButtonFunction(WidgetAction::PA_AudioPlaySelected_Start);
    widgets("PA_PlaySelected").setWidgetColorRGB(0.90f, 0.90f, 0.55f);
    widgets("PA_PlaySelected").setRectangleShape(90, 35);
    widgets("PA_PlaySelected").setCenterX(1320);
    widgets("PA_PlaySelected").setCenterY(300);
    widgets("PA_PlaySelected").setText("Play");

    widgets.add("PA_SaveSelected");
    widgets("PA_SaveSelected").setType(WidgetType::Button_StateMono);
    widgets("PA_SaveSelected").setButtonFunction(WidgetAction::PA_SaveSelected);
    widgets("PA_SaveSelected").setWidgetColorRGB(0.70f, 0.88f, 0.85f);
    widgets("PA_SaveSelected").setRectangleShape(90, 35);
    widgets("PA_SaveSelected").setCenterX(1320);
    widgets("PA_SaveSelected").setCenterY(385);
    widgets("PA_SaveSelected").setText("Save WAV");

    // Vertical pitch control: pixels increase downward, but the displayed semitone value increases
    // upward.
    widgets.add("Slider_PitchShift");
    widgets("Slider_PitchShift").setType(WidgetType::SliderV_FreeLimited);
    widgets("Slider_PitchShift").setBgColor(0.3f, 0.3f, 0.55f);
    widgets("Slider_PitchShift").setHalfDiag1(55);
    widgets("Slider_PitchShift").setHalfDiag2(40);
    widgets("Slider_PitchShift").setCenterX(715);
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

    // Position the neutral 0 semitone default at the center of [-12, +12].
    double initialPosY = widgets("Slider_PitchShift").getSlider_yMin() +
        (12.0 - 0.0) / 24.0 * (widgets("Slider_PitchShift").getSlider_yMax() -
            widgets("Slider_PitchShift").getSlider_yMin());
    widgets("Slider_PitchShift").setHandleCenterY(initialPosY);
    widgets("Slider_PitchShift").setSlider_yVal(initialPosY);

    // Formant slider uses a display range of 0.5..2.0 and starts at its neutral 1.00x ratio.
    widgets.add("Slider_FormantShift");
    widgets("Slider_FormantShift").setType(WidgetType::SliderV_FreeLimited);
    widgets("Slider_FormantShift").setBgColor(0.55f, 0.3f, 0.3f);
    widgets("Slider_FormantShift").setHalfDiag1(55);
    widgets("Slider_FormantShift").setHalfDiag2(40);
    widgets("Slider_FormantShift").setCenterX(843);
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
        (2.0 - 1.0) / 1.5 * (widgets("Slider_FormantShift").getSlider_yMax() -
            widgets("Slider_FormantShift").getSlider_yMin());
    widgets("Slider_FormantShift").setHandleCenterY(initialPosY_Formant);

    // Synchronize the initial stored legend value with the visual handle position; the UI
    // publishes this value each frame.
    widgets("Slider_FormantShift").setSlider_yVal(initialPosY_Formant);


    // One independent live timbre slider: the centre is transparent (0%), lower values
    // attenuate higher frequencies, and upper values raise them. Its IIR filter keeps no
    // additional audio block, so changing this value adds no algorithmic buffering.
    widgets.add("Slider_Timbre");
    widgets("Slider_Timbre").setType(WidgetType::SliderV_FreeLimited);
    widgets("Slider_Timbre").setBgColor(0.25f, 0.50f, 0.55f);
    widgets("Slider_Timbre").setHalfDiag1(55);
    widgets("Slider_Timbre").setHalfDiag2(40);
    widgets("Slider_Timbre").setCenterX(971);
    widgets("Slider_Timbre").setCenterY(740);
    widgets("Slider_Timbre").setTriangleColorRGB(0, 0.0, 0.70, 0.75);
    widgets("Slider_Timbre").setTriangleColorRGB(1, 0.25, 1.0, 1.0);
    widgets("Slider_Timbre").setPointColorRGB(0, 1, 0.0, 0.35, 0.40);
    widgets("Slider_Timbre").setPointColorRGB(1, 1, 0.75, 1.0, 1.0);
    widgets("Slider_Timbre").setLegendY_Unit(WidgetUnit::percent);
    widgets("Slider_Timbre").setLegend_yMin_Val(-100.0);
    widgets("Slider_Timbre").setLegend_yMax_Val(100.0);
    widgets("Slider_Timbre").setLegend_yMin_Text("-100%");
    widgets("Slider_Timbre").setLegend_yMax_Text("+100%");
    const double initialPosY_Timbre =
        (widgets("Slider_Timbre").getSlider_yMin() +
         widgets("Slider_Timbre").getSlider_yMax()) * 0.5;
    widgets("Slider_Timbre").setHandleCenterY(initialPosY_Timbre);
    widgets("Slider_Timbre").setSlider_yVal(initialPosY_Timbre);

    // The neutral centre preserves the recorded voice. One slider controls both an
    // onset-based pitch gesture and its small loudness accent in recording and stream modes.
    widgets.add("Slider_Prosody");
    widgets("Slider_Prosody").setType(WidgetType::SliderV_FreeLimited);
    widgets("Slider_Prosody").setBgColor(0.50f, 0.38f, 0.68f);
    widgets("Slider_Prosody").setHalfDiag1(55);
    widgets("Slider_Prosody").setHalfDiag2(40);
    widgets("Slider_Prosody").setCenterX(1099);
    widgets("Slider_Prosody").setCenterY(740);
    widgets("Slider_Prosody").setTriangleColorRGB(0, 0.45, 0.23, 0.75);
    widgets("Slider_Prosody").setTriangleColorRGB(1, 0.78, 0.62, 1.0);
    widgets("Slider_Prosody").setPointColorRGB(0, 1, 0.46, 0.22, 0.72);
    widgets("Slider_Prosody").setPointColorRGB(1, 1, 0.83, 0.71, 1.0);
    widgets("Slider_Prosody").setLegendY_Unit(WidgetUnit::percent);
    widgets("Slider_Prosody").setLegend_yMin_Val(-100.0);
    widgets("Slider_Prosody").setLegend_yMax_Val(100.0);
    widgets("Slider_Prosody").setLegend_yMin_Text("-100%");
    widgets("Slider_Prosody").setLegend_yMax_Text("+100%");
    const double initialProsodyY = (widgets("Slider_Prosody").getSlider_yMin() +
        widgets("Slider_Prosody").getSlider_yMax()) * 0.5;
    widgets("Slider_Prosody").setHandleCenterY(initialProsodyY);
    widgets("Slider_Prosody").setSlider_yVal(initialProsodyY);

    // Jitter varies the pitch ratio by up to +/-2%. The effect starts OFF,
    // and its slider also starts at zero so the user must enable and raise it.
    widgets.add("Slider_Jitter");
    widgets("Slider_Jitter").setType(WidgetType::SliderV_FreeLimited);
    widgets("Slider_Jitter").setBgColor(0.3f, 0.55f, 0.3f);
    widgets("Slider_Jitter").setHalfDiag1(55);
    widgets("Slider_Jitter").setHalfDiag2(40);
    widgets("Slider_Jitter").setCenterX(1227);
    widgets("Slider_Jitter").setCenterY(740);
    widgets("Slider_Jitter").setTriangleColorRGB(0, 0.0, 0.65, 0.0);
    widgets("Slider_Jitter").setTriangleColorRGB(1, 0.0, 1.0, 0.0);

    widgets("Slider_Jitter").setLegendY_Unit(WidgetUnit::Pts);
    widgets("Slider_Jitter").setLegend_yMin_Val(0.0);
    widgets("Slider_Jitter").setLegend_yMax_Val(0.02);
    widgets("Slider_Jitter").setLegend_yMin_Text("0.000");
    widgets("Slider_Jitter").setLegend_yMax_Text("0.020");

    initialPosY = widgets("Slider_Jitter").getSlider_yMax();
    widgets("Slider_Jitter").setHandleCenterY(initialPosY);
    widgets("Slider_Jitter").setSlider_yVal(initialPosY);

    // The UI displays 0..100 percent; AppUpdate divides by 100 before storing the audio volume
    // multiplier.
    widgets.add("Slider_Volume");
    widgets("Slider_Volume").setType(WidgetType::SliderV_FreeLimited);
    widgets("Slider_Volume").setBgColor(0.55f, 0.55f, 0.3f);
    widgets("Slider_Volume").setHalfDiag1(55);
    widgets("Slider_Volume").setHalfDiag2(40);
    widgets("Slider_Volume").setCenterX(1355);
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
    widgets("WidgetInfo").setRectangleShape(375, 55);
    widgets("WidgetInfo").setCenterX(1035);
    widgets("WidgetInfo").setCenterY(185);

    // Attempt the first display calculation, then initialize smoothing. The microphone ring may
    // still be empty; ComputeFFT_PortAudio returns until a full window is available.
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


                pressedWidget->updatePressedColorRGB();
            }
        }
    }
    // A release acts only on a left-button press that still points at the same hovered button.
    else if (event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event->button.button == SDL_BUTTON_LEFT) {

            if (pressedWidget != nullptr) {

                // Compare widget addresses rather than captions so dragging off a button cancels
                // its action.
                bool isStillOverButton = (hoveredWidget == pressedWidget);

                const bool playbackDeviceBusy = IsRecordedPlaybackTransitionPending() &&
                    (pressedWidget == &widgets("PA_PlaySelected") ||
                        pressedWidget == &widgets("PA_Record") ||
                        pressedWidget == &widgets("AudioMode") ||
                        pressedWidget == &widgets("AudioDevices") ||
                        pressedWidget == &widgets("WasapiMode"));
                if (isStillOverButton &&
                    pressedWidget->getType() == WidgetType::Button_StateMono &&
                    !IsAudioDeviceReopening() && !playbackDeviceBusy) {

                    // Selectors are available in both modes. Their atomic mask also drives
                    // the live stream and the next block of recorded playback.
                    const auto toggleEffect = [&](uint8_t bit, const string& name) {
                        const uint8_t before = g_selectedEffects.fetch_xor(bit, memory_order_acq_rel);
                        const bool enabled = (before & bit) == 0;
                        pressedWidget->setText(name + (enabled ? ": ON" : ": OFF"));
                        SetEffectButtonAppearance(*pressedWidget, enabled);
                    };

                    switch (pressedWidget->getButtonFunction()) {
                    case WidgetAction::EffectPitch_Toggle:
                        toggleEffect(EFFECT_PITCH, "Pitch");
                        break;
                    case WidgetAction::EffectFormant_Toggle:
                        toggleEffect(EFFECT_FORMANT, "Formant");
                        break;
                    case WidgetAction::EffectTimbre_Toggle:
                        toggleEffect(EFFECT_TIMBRE, "Timbre");
                        break;
                    case WidgetAction::EffectProsody_Toggle:
                        toggleEffect(EFFECT_PROSODY, "Prosody");
                        break;
                    case WidgetAction::EffectJitter_Toggle:
                        toggleEffect(EFFECT_JITTER, "Jitter");
                        break;
                    case WidgetAction::PA_AudioPlaySelected_Start:
                        if (!g_streamModeActive.load(memory_order_acquire) &&
                            RequestRecordedPlaybackStart()) {
                            pressedWidget->setText("Starting...");
                            pressedWidget->setWidgetColorRGB(1.0f, 0.85f, 0.45f);
                        }
                        break;
                    case WidgetAction::PA_AudioPlaySelected_Stop:
                        if (RequestRecordedPlaybackStop()) {
                            pressedWidget->setText("Stopping...");
                            pressedWidget->setWidgetColorRGB(1.0f, 0.85f, 0.45f);
                        }
                        break;
                    case WidgetAction::PA_SaveSelected:
                        if (!g_streamModeActive.load(memory_order_acquire) &&
                            SavePortAudioSelectedRecording()) {
                            pressedWidget->setText("Saving...");
                            pressedWidget->setWidgetColorRGB(1.0f, 0.85f, 0.45f);
                        }
                        break;
                    case WidgetAction::AudioDevices_Reopen:
                        if (ReopenAudioDevices()) {
                            pressedWidget->setText("Reopening...");
                            pressedWidget->setWidgetColorRGB(1.0f, 0.85f, 0.45f);
                        }
                        widgets("PA_PlaySelected").setButtonFunction(
                            WidgetAction::PA_AudioPlaySelected_Start);
                        widgets("PA_PlaySelected").setText("Play");
                        widgets("PA_PlaySelected").setWidgetColorRGB(0.90f, 0.90f, 0.55f);
                        break;
                    case WidgetAction::WasapiMode_Toggle: {
                        // Recording cannot survive a device close; the existing reopen path
                        // rejects it. The UI stays responsive while the worker changes modes.
                        const int current = g_activeWasapiMode.load(memory_order_acquire);
                        const int previousRequest = g_requestedWasapiMode.load(memory_order_acquire);
                        g_requestedWasapiMode.store(current == 1 ? 0 : 1, memory_order_release);
                        if (ReopenAudioDevices()) {
                            pressedWidget->setText("Switching...");
                            pressedWidget->setWidgetColorRGB(1.0f, 0.85f, 0.45f);
                            widgets("AudioDevices").setText("Reopening...");
                            widgets("PA_PlaySelected").setButtonFunction(
                                WidgetAction::PA_AudioPlaySelected_Start);
                            widgets("PA_PlaySelected").setText("Play");
                            widgets("PA_PlaySelected").setWidgetColorRGB(0.90f, 0.90f, 0.55f);
                        } else {
                            g_requestedWasapiMode.store(previousRequest, memory_order_release);
                        }
                        break;
                    }
                    case WidgetAction::PA_AudioRecord_Start:
                        if (!g_streamModeActive.load(memory_order_acquire) &&
                            StartPortAudioRecording()) {
                            pressedWidget->setButtonFunction(WidgetAction::PA_AudioRecord_Stop);
                            pressedWidget->setText("Stop Rec");
                            pressedWidget->setWidgetColorRGB(1.0f, 0.3f, 0.3f);
                        }
                        break;
                    case WidgetAction::PA_AudioRecord_Stop:
                        StopPortAudioRecording();
                        pressedWidget->setButtonFunction(WidgetAction::PA_AudioRecord_Start);
                        pressedWidget->setText("Start Rec");
                        pressedWidget->setWidgetColorRGB(1.0f, 0.55f, 0.55f);
                        break;
                    case WidgetAction::AudioMode_Toggle:
                        if (g_streamModeActive.load(memory_order_acquire))
                            DeactivateStreamMode();
                        else {
                            // A live output and a recorded preview must never mix at the speaker.
                            if (paIsRecording.load(memory_order_acquire)) {
                                StopPortAudioRecording();
                                widgets("PA_Record").setButtonFunction(WidgetAction::PA_AudioRecord_Start);
                                widgets("PA_Record").setText("Start Rec");
                                widgets("PA_Record").setWidgetColorRGB(1.0f, 0.55f, 0.55f);
                            }
                            StopPortAudioPlaybackSelected();
                            widgets("PA_PlaySelected").setButtonFunction(
                                WidgetAction::PA_AudioPlaySelected_Start);
                            widgets("PA_PlaySelected").setText("Play");
                            widgets("PA_PlaySelected").setWidgetColorRGB(0.90f, 0.90f, 0.55f);
                            ActivateStreamMode();
                        }
                        pressedWidget->setText(g_streamModeActive.load(memory_order_acquire)
                            ? "Mode: Stream" : "Mode: Recording");
                        pressedWidget->setWidgetColorRGB(g_streamModeActive.load(memory_order_acquire)
                            ? 0.3f : 0.8f, g_streamModeActive.load(memory_order_acquire)
                            ? 1.0f : 0.8f, g_streamModeActive.load(memory_order_acquire)
                            ? 0.3f : 0.8f);
                        break;
                    default:
                        break;
                    }
                }
                pressedWidget->set_isMouseLeftButtonDown(false);
                pressedWidget->updatePressedColorRGB();
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
    const Uint64 frameStartedNs = SDL_GetTicksNS();
    constexpr Uint64 targetFrameNs = 1000000000ULL / 60;
    // Keep one 60 Hz timeline instead of basing each new deadline on the actual
    // start of the preceding frame. Small scheduler delays then do not accumulate.
    static Uint64 nextVisualDeadlineNs = 0;
    if (nextVisualDeadlineNs == 0 ||
        frameStartedNs > nextVisualDeadlineNs + targetFrameNs * 3)
        nextVisualDeadlineNs = frameStartedNs + targetFrameNs;

    const double now = ((double)SDL_GetTicks()) / 1000.0;
    CompleteAudioDeviceReopen();
    const bool reopening = IsAudioDeviceReopening();
    // Playback completion can close PortAudio streams, so skip that work while the worker owns
    // the devices. SDL continues rendering the existing visualization during the transition.
    if (!reopening) UpdatePlaybackAndAnalysis();
    RenderFrame(now);
    const Uint64 afterRenderNs = SDL_GetTicksNS();
    // Wait only until the next deadline. A slow GPU/driver frame may miss one
    // or more deadlines; skip those slots instead of trying to render old frames.
    if (afterRenderNs < nextVisualDeadlineNs)
        SDL_DelayPrecise(nextVisualDeadlineNs - afterRenderNs);
    const Uint64 frameFinishedNs = SDL_GetTicksNS();
    if (frameFinishedNs < nextVisualDeadlineNs) {
        nextVisualDeadlineNs += targetFrameNs;
    } else {
        const Uint64 missedSlots = (frameFinishedNs - nextVisualDeadlineNs) /
            targetFrameNs + 1;
        nextVisualDeadlineNs += missedSlots * targetFrameNs;
    }
    return SDL_APP_CONTINUE;
}

// Release application-owned resources during SDL shutdown. Stop and join the audio callbacks
// before deleting any state they read; this prevents the inherited teardown race.
void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
    JoinSelectedSaveWorker();
    CleanupSpectrumRenderTarget();
    CleanupStaticSliderCaptions();
    CleanupTextCache();
    // Close PortAudio on the worker thread that initialized WASAPI/COM, then join it before
    // freeing callback-visible application state. A pending reopen finishes before shutdown.
    {
        lock_guard<mutex> lock(g_audioWorkerMutex);
        g_audioWorkerShouldStop = true;
    }
    g_audioWorkerWake.notify_one();
    if (g_portAudioDeviceWorker.joinable()) g_portAudioDeviceWorker.join();

    CleanupStreamMode();

    if (gFont) {
        TTF_CloseFont(gFont);
        gFont = nullptr;
    }
    TTF_Quit();

}
