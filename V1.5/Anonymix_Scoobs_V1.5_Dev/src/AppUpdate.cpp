// Per-frame non-visual work: playback transitions, slider values, FFT updates and diagnostic reporting.

#include "Widgets.h"
#include "Stream.h"
#include "Dsp.h"
#include "AppFrame.h"
#include "AudioDiagnostics.h"

// Detect completed playback, copy slider settings to audio atomics and update FFT data.
void UpdatePlaybackAndAnalysis()
{
    const int exportResult = TakeSelectedSaveResult();
    if (exportResult != 0) {
        widgets("PA_SaveSelected").setText("Save WAV");
        widgets("PA_SaveSelected").setWidgetColorRGB(exportResult == 1
            ? 0.35f : 1.0f, 0.88f, 0.85f);
    }

    // The only recorded output stream clears its flag at the end of the take.
    // Join it on the device worker and restore the single Play button once.
    static bool wasPlaying = false;
    if (paIsPlayingSelected.load(memory_order_acquire)) {
        wasPlaying = true;
    } else if (wasPlaying || widgets("PA_PlaySelected").getButtonFunction() ==
        WidgetAction::PA_AudioPlaySelected_Stop) {
        wasPlaying = false;
        StopPortAudioPlaybackSelected();
        widgets("PA_PlaySelected").setButtonFunction(WidgetAction::PA_AudioPlaySelected_Start);
        widgets("PA_PlaySelected").setText("Play");
        widgets("PA_PlaySelected").setWidgetColorRGB(0.90f, 0.90f, 0.55f);
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

        g_prosodyPercent.store(static_cast<float>(
            widgets("Slider_Prosody").getLegend_yVal()), memory_order_release);

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
    // Refresh the spectrum once per rendered UI frame (up to the 60 FPS cap).
    // The audio callback only copies recent input to its fixed ring; FFT work stays on SDL.
    ComputeFFT_PortAudio();
    SmoothFFTDisplay_PortAudio(0.34f);

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
            SDL_Log("[V1.5 PortAudio] %llu live blocks, %llu frames (device block %llu..%llu frames); callback avg %.3f ms, max %.3f ms; over budget %llu; input overflows %llu; output underflows %llu",
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
                SDL_Log("[V1.5 PortAudio] Raw callback first-buffer timestamp gap: %.2f ms (%llu callbacks; NOT acoustic or algorithm latency)",
                    static_cast<double>(stats.totalEstimatedHostMicroseconds) /
                        (1000.0 * static_cast<double>(stats.hostTimingObservations)),
                    static_cast<unsigned long long>(stats.hostTimingObservations));
            }
            // The reporting window begins at app startup. A shorter span here may simply mean
            // that stream mode was enabled partway through the ten-second wall-clock window.
            SDL_Log("[V1.5] Audio frames in this report cover %.2f seconds at %.0f Hz",
                static_cast<double>(stats.frames) / PA_SAMPLE_RATE, PA_SAMPLE_RATE);
        }
    }
}
