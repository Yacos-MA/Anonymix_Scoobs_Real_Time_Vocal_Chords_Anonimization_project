// Per-frame update and rendering declarations, called in that order from SDL_AppIterate.

#pragma once
// Detect completed playback, copy slider settings to audio atomics and update FFT data.
void UpdatePlaybackAndAnalysis();
// Move and draw the interactive controls, waveform and spectrum, then present the frame.
void RenderFrame(double now);
