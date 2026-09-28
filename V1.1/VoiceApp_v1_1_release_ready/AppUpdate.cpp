// Per-frame non-visual work: playback transitions, slider values, FFT updates and diagnostic reporting.

#include "Widgets.h"
#include "Stream.h"
#include "Dsp.h"
#include "AppFrame.h"

// Detect completed playback, copy slider settings to audio atomics and update FFT data.
void UpdatePlaybackAndAnalysis()
{

    // Remember whether each playback mode was active last frame. A true-to-false transition
    // restores its button once.
    static bool paWasPlaying = false;

    if (paIsPlaying) {
        paWasPlaying = true;
    }
    else if (paWasPlaying) {

        paWasPlaying = false;

        try {
            widgets("PA_Play").setButtonFunction(WidgetAction::PA_AudioPlay_Start);
            widgets("PA_Play").setText("Play OG");
            widgets("PA_Play").setWidgetColorRGB(1.0, 1.0, 0.55);
            SDL_Log(">>> PortAudio playback completed automatically <<<");
        }
        catch (...) {
            SDL_Log("Failed to reset button PA_Play");
        }
    }

    // The pitch playback button uses its own completion flag; the same pattern follows for formant
    // and combined modes.
    static bool paWasPlayingAnonymized = false;

    if (paIsPlayingAnonymized) {
        paWasPlayingAnonymized = true;
    }
    else if (paWasPlayingAnonymized) {
        paWasPlayingAnonymized = false;

        try {
            widgets("PA_PlayAnonPitch").setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitch_Start);
            widgets("PA_PlayAnonPitch").setText("Play Pitch");
            widgets("PA_PlayAnonPitch").setWidgetColorRGB(0.55, 1.0, 0.55);
            SDL_Log(">>> Anonymized PortAudio playback completed automatically <<<");
        }
        catch (...) {
            SDL_Log("Failed to reset button PA_PlayAnonPitch");
        }
    }

    // Reset this button only when its callback ends playback, leaving unrelated controls alone.
    static bool paWasPlayingAnonymizedFormant = false;

    if (paIsPlayingAnonymizedFormant) {
        paWasPlayingAnonymizedFormant = true;
    }
    else if (paWasPlayingAnonymizedFormant) {
        paWasPlayingAnonymizedFormant = false;

        try {
            widgets("PA_PlayAnonFormant").setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedFormant_Start);
            widgets("PA_PlayAnonFormant").setText("Play Formant");
            widgets("PA_PlayAnonFormant").setWidgetColorRGB(0.55, 0.85, 1.0);
            SDL_Log(">>> Anonymized formant PortAudio playback completed automatically <<<");
        }
        catch (...) {
            SDL_Log("Failed to reset button PA_PlayAnonFormant");
        }
    }

    // The combined callback clears its flag; this branch restores its action, label and color.
    static bool paWasPlayingAnonymizedPitchFormant = false;

    if (paIsPlayingAnonymizedPitchFormant) {
        paWasPlayingAnonymizedPitchFormant = true;
    }
    else if (paWasPlayingAnonymizedPitchFormant) {
        paWasPlayingAnonymizedPitchFormant = false;

        try {
            widgets("PA_PlayAnonPitchFormant").setButtonFunction(WidgetAction::PA_AudioPlayAnonymizedPitchFormant_Start);
            widgets("PA_PlayAnonPitchFormant").setText("Play P+F");
            widgets("PA_PlayAnonPitchFormant").setWidgetColorRGB(1.0, 0.55, 1.0);
            SDL_Log(">>> Pitch and formant PortAudio playback completed automatically <<<");
        }
        catch (...) {
            SDL_Log("Failed to reset button PA_PlayAnonPitchFormant");
        }
    }

    try {

        // Named widget lookup may throw if initialization omitted a key. The broad catch logs and
        // leaves the previous setting.
        double pitchY = widgets("Slider_PitchShift").getLegend_yVal();
        // The callback reads atomics with memory_order_acquire; UI text formatting is not
        // performed in the audio thread.
        g_pitchShiftSemitones.store(static_cast<float>(pitchY), memory_order_release);

        double formantY = widgets("Slider_FormantShift").getLegend_yVal();
        float formantValue = static_cast<float>(formantY);
        g_formantShiftRatio.store(formantValue, memory_order_release);

        static int logCounter = 0;
        if (++logCounter % 60 == 0) {
            SDL_Log(">>> Formant slider: legend_yVal=%.3f | atomic=%.3f <<<",
                formantY, formantValue);
        }

        double jitterY = widgets("Slider_Jitter").getLegend_yVal();
        g_jitterAmount.store(static_cast<float>(jitterY), memory_order_release);

        double volumeY = widgets("Slider_Volume").getLegend_yVal();
        // The vertical percentage label is converted to a 0..1 multiplier for playback and
        // streaming.
        g_outputVolume.store(static_cast<float>(volumeY / 100.0), memory_order_release);

    }
    catch (...) {
        SDL_Log("Error reading DSP slider values");
    }

    static size_t sampleCountPA = 0;

// This counter advances by 256 per SDL frame, not by the number of samples captured by PortAudio.
    sampleCountPA += 256;

    // FFT calculation happens on the SDL thread; the callback only supplies its input samples
    // under a mutex.
    if (sampleCountPA >= FFT_OVERLAP) {
        sampleCountPA = 0;

        ComputeFFT_PortAudio();
        SmoothFFTDisplay_PortAudio(0.25f);
    }

    static Uint64 lastLatencyReport = 0;
    Uint64 currentTicks = SDL_GetTicks();

    // One-second reporting is based on SDL ticks and only prints while stream processing is
    // active.
    if (currentTicks - lastLatencyReport >= 1000) {
        lastLatencyReport = currentTicks;

        if (g_latencyMetrics && g_streamModeActive.load(memory_order_acquire)) {
            g_latencyMetrics->printSecondReport();
        }
    }
}
