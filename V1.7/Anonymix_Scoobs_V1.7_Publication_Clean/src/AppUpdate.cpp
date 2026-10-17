// Per-frame non-visual work: playback transitions, slider values and FFT updates.

#include "Widgets.h"
#include "Stream.h"
#include "Dsp.h"
#include "AppFrame.h"

// Detect completed playback, copy slider settings to audio atomics and update FFT data.
void UpdatePlaybackAndAnalysis()
{
    const int exportResult = TakeSelectedSaveResult();
    if (exportResult != 0) {
        widgets("PA_SaveSelected").setText("Save WAV");
        widgets("PA_SaveSelected").setWidgetColorRGB(exportResult == 1
            ? 0.35f : 1.0f, 0.88f, 0.85f);
    }

    // The UI receives open/close results without waiting for WASAPI. A completed
    // preview is also closed on the device worker before Play becomes available.
    static bool wasPlaying = false;
    auto& playButton = widgets("PA_PlaySelected");
    const RecordedPlaybackResult result = TakeRecordedPlaybackResult();
    if (result == RecordedPlaybackResult::Started) {
        wasPlaying = true;
        playButton.setButtonFunction(WidgetAction::PA_AudioPlaySelected_Stop);
        playButton.setText("Stop");
        playButton.setWidgetColorRGB(1.0f, 0.55f, 0.45f);
    } else if (result == RecordedPlaybackResult::StartFailed ||
        result == RecordedPlaybackResult::Stopped) {
        wasPlaying = false;
        playButton.setButtonFunction(WidgetAction::PA_AudioPlaySelected_Start);
        playButton.setText("Play");
        playButton.setWidgetColorRGB(0.90f, 0.90f, 0.55f);
    }
    if (!IsRecordedPlaybackTransitionPending()) {
        if (paIsPlayingSelected.load(memory_order_acquire)) {
            wasPlaying = true;
        } else if (wasPlaying || playButton.getButtonFunction() ==
            WidgetAction::PA_AudioPlaySelected_Stop) {
            if (RequestRecordedPlaybackStop()) {
                playButton.setText("Stopping...");
                playButton.setWidgetColorRGB(1.0f, 0.85f, 0.45f);
            }
        }
    }

    try {

        // A missing widget leaves the previous audio setting unchanged.
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

        double jitterY = widgets("Slider_Jitter").getLegend_yVal();
        g_jitterAmount.store(static_cast<float>(jitterY), memory_order_release);

        double volumeY = widgets("Slider_Volume").getLegend_yVal();
        // The vertical percentage label is converted to a 0..1 multiplier for playback and
        // streaming.
        g_outputVolume.store(static_cast<float>(volumeY / 100.0), memory_order_release);

    }
    catch (...) { /* Keep the previously published slider settings. */ }

    // Refresh the spectrum once per rendered UI frame (up to the 60 FPS cap).
    // The audio callback only copies recent input to its fixed ring; FFT work stays on SDL.
    ComputeFFT_PortAudio();
    SmoothFFTDisplay_PortAudio(0.34f);

}
