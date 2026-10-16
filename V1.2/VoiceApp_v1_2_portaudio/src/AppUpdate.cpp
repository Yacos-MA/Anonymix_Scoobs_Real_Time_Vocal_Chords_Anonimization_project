// Per-frame non-visual work: playback transitions, slider values, FFT updates and diagnostic reporting.

#include "Widgets.h"
#include "Stream.h"
#include "Dsp.h"
#include "AppFrame.h"
#include "AudioDiagnostics.h"

// Detect completed playback, copy slider settings to audio atomics and update FFT data.
void UpdatePlaybackAndAnalysis()
{

    // Remember whether each playback mode was active last frame. A true-to-false transition
    // restores its button once.
    static bool paWasPlaying = false;

    if (paIsPlaying) {
        paWasPlaying = true;
    }
    else if (paWasPlaying || widgets("PA_Play").getButtonFunction() == WidgetAction::PA_AudioPlay_Stop) {

        paWasPlaying = false;
        StopPortAudioPlayback();

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
    else if (paWasPlayingAnonymized ||
        widgets("PA_PlayAnonPitch").getButtonFunction() == WidgetAction::PA_AudioPlayAnonymizedPitch_Stop) {
        paWasPlayingAnonymized = false;
        StopPortAudioPlaybackAnonymized();

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
    else if (paWasPlayingAnonymizedFormant ||
        widgets("PA_PlayAnonFormant").getButtonFunction() == WidgetAction::PA_AudioPlayAnonymizedFormant_Stop) {
        paWasPlayingAnonymizedFormant = false;
        StopPortAudioPlaybackAnonymizedFormant();

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
    else if (paWasPlayingAnonymizedPitchFormant ||
        widgets("PA_PlayAnonPitchFormant").getButtonFunction() == WidgetAction::PA_AudioPlayAnonymizedPitchFormant_Stop) {
        paWasPlayingAnonymizedPitchFormant = false;
        StopPortAudioPlaybackAnonymizedPitchFormant();

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

    // The timbre preview has its own callback and completion flag. Restore its button when
    // the recorded sample ends, including after the playback stream is reopened.
    static bool timbreWasPlaying = false;
    if (paIsPlayingTimbre.load(memory_order_acquire)) {
        timbreWasPlaying = true;
    }
    else if (timbreWasPlaying ||
        widgets("PA_PlayTimbre").getButtonFunction() == WidgetAction::PA_AudioPlayTimbre_Stop) {
        timbreWasPlaying = false;
        StopPortAudioPlaybackTimbre();
        widgets("PA_PlayTimbre").setButtonFunction(WidgetAction::PA_AudioPlayTimbre_Start);
        widgets("PA_PlayTimbre").setText("Play Timbre");
        widgets("PA_PlayTimbre").setWidgetColorRGB(0.7, 0.9, 0.7);
        SDL_Log("Timbre recording playback completed");
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

        // The timbre effect reads this atomic in both live and recorded playback callbacks.
        double timbreY = widgets("Slider_Timbre").getLegend_yVal();
        g_timbrePercent.store(static_cast<float>(timbreY), memory_order_release);

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

    const Uint64 currentTicks = SDL_GetTicks();
    static Uint64 lastFftUpdate = 0;

    // Schedule display analysis in wall-clock time. Counting samples per SDL frame would be
    // incorrect because the render loop and the 48 kHz audio device run independently.
    // FFT calculation stays on the SDL thread; the audio callback only appends input samples.
    if (currentTicks - lastFftUpdate >= 40) {
        lastFftUpdate = currentTicks;

        ComputeFFT_PortAudio();
        SmoothFFTDisplay_PortAudio(0.25f);
    }

    static Uint64 lastLatencyReport = 0;

    // One-second reporting is based on SDL ticks and only prints while stream processing is
    // active.
    if (currentTicks - lastLatencyReport >= 1000) {
        lastLatencyReport = currentTicks;

        if (g_latencyMetrics && g_streamModeActive.load(memory_order_acquire)) {
            g_latencyMetrics->printSecondReport();
        }
    }

    // Report a ten-second live window with callback size, processing time and device errors.
    static Uint64 lastAudioBenchmarkReport = SDL_GetTicks();
    if (currentTicks - lastAudioBenchmarkReport >= 10000) {
        lastAudioBenchmarkReport = currentTicks;
        const auto stats = g_audioDiagnostics.takeSnapshot();
        if (stats.blocks != 0) {
            const double averageMs = static_cast<double>(stats.totalProcessingMicroseconds) /
                (1000.0 * static_cast<double>(stats.blocks));
            SDL_Log("[V1.2 PortAudio] %llu live blocks, %llu frames (device block %llu..%llu frames); callback avg %.3f ms, max %.3f ms; over budget %llu; input overflows %llu; output underflows %llu",
                static_cast<unsigned long long>(stats.blocks),
                static_cast<unsigned long long>(stats.frames),
                static_cast<unsigned long long>(stats.minimumFramesPerBlock),
                static_cast<unsigned long long>(stats.maximumFramesPerBlock), averageMs,
                static_cast<double>(stats.maximumProcessingMicroseconds) / 1000.0,
                static_cast<unsigned long long>(stats.blocksOverBudget),
                static_cast<unsigned long long>(stats.inputOverflows),
                static_cast<unsigned long long>(stats.outputUnderflows));
            if (stats.hostTimingObservations != 0) {
                // Callback timestamps do not measure total acoustic round-trip latency.
                SDL_Log("[V1.2 PortAudio] Raw callback first-buffer timestamp gap: %.2f ms (%llu callbacks; NOT acoustic or algorithm latency)",
                    static_cast<double>(stats.totalEstimatedHostMicroseconds) /
                        (1000.0 * static_cast<double>(stats.hostTimingObservations)),
                    static_cast<unsigned long long>(stats.hostTimingObservations));
            }
            // The reporting window begins at app startup. A shorter span here may simply mean
            // that stream mode was enabled partway through the ten-second wall-clock window.
            SDL_Log("[V1.2] Audio frames in this report cover %.2f seconds at %.0f Hz",
                static_cast<double>(stats.frames) / PA_SAMPLE_RATE, PA_SAMPLE_RATE);
        }
    }
}
