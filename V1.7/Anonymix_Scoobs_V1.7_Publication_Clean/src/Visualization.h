// SDL drawing and text helpers. Call from the UI thread after renderer and font initialization.

#pragma once
#include "State.h"
// Format a floating-point value using the requested number of decimal places.
string FormatDouble(double, int precision = 2) noexcept;
// Convert a numeric value to text for slider legends.
string ValueToText(double, int precision = 0) noexcept;
// Append the unit suffix selected by WidgetUnit to a formatted value.
string FormatValueWithUnit(double, WidgetUnit, int precision = 2) noexcept;
// Query the rendered text dimensions after applying the requested scale.
void getSurfaceSize(const string&, float&, float&, double scale = 1.0);
// Draw text and report its dimensions. Cache labels whose contents are stable across frames.
bool RenderText(const string&, float, float, SDL_Color, float&, float&, double scale = 1.0,
    bool cacheStableLabel = false);
// Cache the changing value shown in the information panel separately from fixed captions.
// Its texture is replaced only when the actual text or color changes.
bool RenderDynamicInfoText(const string&, float, float, SDL_Color, float&, float&,
    double scale = 1.0);
// Release the label textures before closing the font or destroying the renderer.
void CleanupTextCache();
// Render a quad using its existing vertex colors; UI supersampling softens edges.
void RenderSolidQuad(const SDL_Vertex (&triangles)[2][3]);


// Snapshot the circular waveform under waveformMutex, then draw it without holding the lock.
// The callback uses try_to_lock so UI rendering cannot delay the audio callback.
void RenderWaveform(float, float, float, float);

// Draw the smoothed bars computed in Dsp.cpp; no FFT is performed here.
void RenderFFTSpectrum_PortAudio(float, float, float, float);
// Release the GPU texture while the SDL renderer is still alive.
void CleanupSpectrumRenderTarget();
