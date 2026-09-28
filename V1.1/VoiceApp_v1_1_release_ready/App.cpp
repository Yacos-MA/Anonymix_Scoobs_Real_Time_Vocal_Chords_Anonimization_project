// SDL3 callback entry points: initialization, input events, frame update and shutdown. Widget actions are dispatched from SDL_AppEvent.

#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>
#include "Widgets.h"
#include "Stream.h"
#include "Dsp.h"
#include "Wav.h"
#include "AppFrame.h"

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

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return SDL_APP_FAILURE;
    }

    // The UI currently continues after an audio initialization failure; controls may then be
    // visible without a usable device.
    if (!InitPortAudio()) {
        SDL_Log("WARNING: PortAudio initialization failed");
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
        gFont = TTF_OpenFont("C:/Windows/Fonts/Arial.ttf", 32);
        if (!gFont) gFont = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 32);
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
    widgets("AudioMode").setRectangleShape(210, 40);
    widgets("AudioMode").setCenterX(1140);
    widgets("AudioMode").setCenterY(65);
    widgets("AudioMode").setText("Active mode: Recording");

    // Record, play and save buttons initially store a Start/Save WidgetAction; start/stop actions
    // change the same widget later.
    widgets.add("PA_Record");
    widgets("PA_Record").setType(WidgetType::Button_StateMono);
    widgets("PA_Record").setButtonFunction(WidgetAction::PA_AudioRecord_Start);
    widgets("PA_Record").setWidgetColorRGB(1.0, 0.55, 0.55);
    widgets("PA_Record").setRectangleShape(100, 48);
    widgets("PA_Record").setCenterX(790);
    widgets("PA_Record").setCenterY(190);
    widgets("PA_Record").setText("Start Rec");

    widgets.add("PA_Play");
    widgets("PA_Play").setType(WidgetType::Button_StateMono);
    widgets("PA_Play").setButtonFunction(WidgetAction::PA_AudioPlay_Start);
    widgets("PA_Play").setWidgetColorRGB(1.0, 1.0, 0.55);
    widgets("PA_Play").setRectangleShape(90, 35);
    widgets("PA_Play").setCenterX(765);
    widgets("PA_Play").setCenterY(310);
    widgets("PA_Play").setText("Play OG");

    // Playback choices share paAudioBuffer but have independent callback streams and positions in
    // State.cpp.
    widgets.add("PA_PlayAnonPitchFormant");
    widgets("PA_PlayAnonPitchFormant").setType(WidgetType::Button_StateMono);
    widgets("PA_PlayAnonPitchFormant").setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitchFormant_Start);
    widgets("PA_PlayAnonPitchFormant").setWidgetColorRGB(1.0, 0.55, 1.0);
    widgets("PA_PlayAnonPitchFormant").setRectangleShape(90, 35);
    widgets("PA_PlayAnonPitchFormant").setCenterX(985);
    widgets("PA_PlayAnonPitchFormant").setCenterY(310);
    widgets("PA_PlayAnonPitchFormant").setText("Play P+F");

    widgets.add("PA_PlayAnonPitch");
    widgets("PA_PlayAnonPitch").setType(WidgetType::Button_StateMono);
    widgets("PA_PlayAnonPitch").setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitch_Start);
    widgets("PA_PlayAnonPitch").setWidgetColorRGB(0.55, 1.0, 0.55);
    widgets("PA_PlayAnonPitch").setRectangleShape(90, 35);
    widgets("PA_PlayAnonPitch").setCenterX(1205);
    widgets("PA_PlayAnonPitch").setCenterY(310);
    widgets("PA_PlayAnonPitch").setText("Play Pitch");

    widgets.add("PA_PlayAnonFormant");
    widgets("PA_PlayAnonFormant").setType(WidgetType::Button_StateMono);
    widgets("PA_PlayAnonFormant").setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedFormant_Start);
    widgets("PA_PlayAnonFormant").setWidgetColorRGB(0.55, 0.85, 1.0);
    widgets("PA_PlayAnonFormant").setRectangleShape(105, 35);
    widgets("PA_PlayAnonFormant").setCenterX(1440);
    widgets("PA_PlayAnonFormant").setCenterY(310);
    widgets("PA_PlayAnonFormant").setText("Play Formant");

    // Saving runs synchronously from SDL_AppEvent on a copy or on the original bytes, depending on
    // the action.
    widgets.add("PA_SaveOriginal");
    widgets("PA_SaveOriginal").setType(WidgetType::Button_StateMono);
    widgets("PA_SaveOriginal").setButtonFunction(WidgetAction::PA_SaveOriginal);
    widgets("PA_SaveOriginal").setWidgetColorRGB(0.55, 0.85, 1.0);
    widgets("PA_SaveOriginal").setRectangleShape(90, 35);
    widgets("PA_SaveOriginal").setCenterX(765);
    widgets("PA_SaveOriginal").setCenterY(410);
    widgets("PA_SaveOriginal").setText("Save OG");

    widgets.add("PA_SaveAnonPitchFormant");
    widgets("PA_SaveAnonPitchFormant").setType(WidgetType::Button_StateMono);
    widgets("PA_SaveAnonPitchFormant").setButtonFunction(WidgetAction::PA_SaveAnonymizedPitchFormant);
    widgets("PA_SaveAnonPitchFormant").setWidgetColorRGB(1.0, 0.85, 0.55);
    widgets("PA_SaveAnonPitchFormant").setRectangleShape(90, 35);
    widgets("PA_SaveAnonPitchFormant").setCenterX(985);
    widgets("PA_SaveAnonPitchFormant").setCenterY(410);
    widgets("PA_SaveAnonPitchFormant").setText("Save P+F");

    widgets.add("PA_SaveAnonPitch");
    widgets("PA_SaveAnonPitch").setType(WidgetType::Button_StateMono);
    widgets("PA_SaveAnonPitch").setButtonFunction(WidgetAction::PA_SaveAnonymizedPitch);
    widgets("PA_SaveAnonPitch").setWidgetColorRGB(0.85, 0.55, 1.0);
    widgets("PA_SaveAnonPitch").setRectangleShape(90, 35);
    widgets("PA_SaveAnonPitch").setCenterX(1205);
    widgets("PA_SaveAnonPitch").setCenterY(410);
    widgets("PA_SaveAnonPitch").setText("Save Pitch");

    widgets.add("PA_SaveAnonFormant");
    widgets("PA_SaveAnonFormant").setType(WidgetType::Button_StateMono);
    widgets("PA_SaveAnonFormant").setButtonFunction(WidgetAction::PA_SaveAnonymizedFormant);
    widgets("PA_SaveAnonFormant").setWidgetColorRGB(1.0, 0.85, 0.55);
    widgets("PA_SaveAnonFormant").setRectangleShape(105, 35);
    widgets("PA_SaveAnonFormant").setCenterX(1440);
    widgets("PA_SaveAnonFormant").setCenterY(410);
    widgets("PA_SaveAnonFormant").setText("Save Formant");

    // Vertical pitch control: pixels increase downward, but the displayed semitone value increases
    // upward.
    widgets.add("Slider_PitchShift");
    widgets("Slider_PitchShift").setType(WidgetType::SliderV_FreeLimited);
    widgets("Slider_PitchShift").setBgColor(0.3f, 0.3f, 0.55f);
    widgets("Slider_PitchShift").setHalfDiag1(55);
    widgets("Slider_PitchShift").setHalfDiag2(40);
    widgets("Slider_PitchShift").setCenterX(800);
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
    widgets("Slider_FormantShift").setCenterX(1000);
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

    // Jitter uses the small 0..0.02 range. AppUpdate publishes its displayed value to the
    // callback.
    widgets.add("Slider_Jitter");
    widgets("Slider_Jitter").setType(WidgetType::SliderV_FreeLimited);
    widgets("Slider_Jitter").setBgColor(0.3f, 0.55f, 0.3f);
    widgets("Slider_Jitter").setHalfDiag1(55);
    widgets("Slider_Jitter").setHalfDiag2(40);
    widgets("Slider_Jitter").setCenterX(1200);
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
    widgets("Slider_Volume").setCenterX(1400);
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

                if (isStillOverButton && pressedWidget->getType() == WidgetType::Button_StateMono) {
                    SDL_Log(">>> Button action: %d <<<",
                        static_cast<int>(pressedWidget->getButtonFunction()));

                    switch (pressedWidget->getButtonFunction()) {

                    // The UI flips its action, text and color, then requests capture. The returned
                    // Start status is not currently checked.
                    case WidgetAction::PA_AudioRecord_Start:
                        SDL_Log(">>> Click on PA_AudioRecord_Start <<<");
                        pressedWidget->setButtonFunction(WidgetAction::PA_AudioRecord_Stop);
                        pressedWidget->setText("Stop Rec");
                        pressedWidget->setWidgetColorRGB(1.0, 0.3, 0.3);
                        StartPortAudioRecording();
                        break;

                    case WidgetAction::PA_AudioRecord_Stop:
                        SDL_Log(">>> Click on PA_AudioRecord_Stop <<<");
                        pressedWidget->setButtonFunction(WidgetAction::PA_AudioRecord_Start);
                        pressedWidget->setText("Start Rec");
                        pressedWidget->setWidgetColorRGB(1.0, 0.55, 0.55);
                        StopPortAudioRecording();
                        break;

                    // Playback controls similarly change their visible state before
                    // StartPortAudioPlayback reports success or failure.
                    case WidgetAction::PA_AudioPlay_Start:
                        SDL_Log(">>> Click on PA_AudioPlay_Start <<<");
                        pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlay_Stop);
                        pressedWidget->setText("Stop Audio");
                        pressedWidget->setWidgetColorRGB(1.0, 0.8, 0.0);
                        StartPortAudioPlayback();
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
                        pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitch_Stop);
                        pressedWidget->setText("Stop Pitch");
                        pressedWidget->setWidgetColorRGB(1.0, 0.5, 0.0);
                        StartPortAudioPlaybackAnonymized();
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
                        pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedFormant_Stop);
                        pressedWidget->setText("Stop Formant");
                        pressedWidget->setWidgetColorRGB(0.0, 0.5, 1.0);
                        StartPortAudioPlaybackAnonymizedFormant();
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

                            string formatSuffix = (paMicrophoneChannels == 2) ? "stereo" : "mono";
                            string filename = GenerateWAVFilename("audio_anonymized_formant_lpc_" + formatSuffix);

                            if (SaveWAV_Adaptive(filename, tempBuffer, paMicrophoneChannels, PA_SAMPLE_RATE)) {
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

                            string formatSuffix = (paMicrophoneChannels == 2) ? "stereo" : "mono";
                            string filename = GenerateWAVFilename("audio_original_" + formatSuffix);

                            if (SaveWAV_Adaptive(filename, tempBuffer, paMicrophoneChannels, PA_SAMPLE_RATE)) {
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

                            string formatSuffix = (paMicrophoneChannels == 2) ? "stereo" : "mono";
                            string filename = GenerateWAVFilename("audio_anonymized_pitch_" + formatSuffix);

                            if (SaveWAV_Adaptive(filename, tempBuffer, paMicrophoneChannels, PA_SAMPLE_RATE)) {
                                pressedWidget->setWidgetColorRGB(0.3, 1.0, 0.3);
                            }
                            else {
                                pressedWidget->setWidgetColorRGB(1.0, 0.3, 0.3);
                            }
                        }
                        break;

                    case WidgetAction::PA_AudioPlayAnonymizedPitchFormant_Start:
                        SDL_Log(">>> Click on PA_AudioPlayAnonymizedPitchFormant_Start <<<");
                        pressedWidget->setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitchFormant_Stop);
                        pressedWidget->setText("Stop P+F");
                        pressedWidget->setWidgetColorRGB(1.0, 0.0, 0.5);
                        StartPortAudioPlaybackAnonymizedPitchFormant();
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

                            string formatSuffix = (paMicrophoneChannels == 2) ? "stereo" : "mono";
                            string filename = GenerateWAVFilename("audio_anonymized_pitch_formant_" + formatSuffix);

                            if (SaveWAV_Adaptive(filename, tempBuffer, paMicrophoneChannels, PA_SAMPLE_RATE)) {
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
    const double now = ((double)SDL_GetTicks()) / 1000.0;
    UpdatePlaybackAndAnalysis();
    RenderFrame(now);
    return SDL_APP_CONTINUE;
}

// Release application-owned resources during SDL shutdown. The inherited
// order frees callback-visible metrics/processors before stopping PortAudio;
// it can race a callback and requires a separate lifecycle fix.
void SDL_AppQuit(void* appstate, SDL_AppResult result)
{

    if (gLogoTexture) {
        SDL_DestroyTexture(gLogoTexture);
        gLogoTexture = nullptr;
    }

    if (g_latencyMetrics) {
        delete g_latencyMetrics;
        g_latencyMetrics = nullptr;
    }

    // Destroy live processors and temporary buffers before stopping PortAudio streams during
    // normal shutdown.
    CleanupStreamMode();

    CleanupPortAudio();

    if (gFont) {
        TTF_CloseFont(gFont);
        gFont = nullptr;
    }
    TTF_Quit();

}
