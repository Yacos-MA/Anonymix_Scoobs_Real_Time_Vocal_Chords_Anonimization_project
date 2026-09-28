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
// Create and draw an SDL_ttf text surface, then report its rendered dimensions.
bool RenderText(const string&, float, float, SDL_Color, float&, float&, double scale = 1.0);

// Draw samples from the circular visualization buffer; this read is not fully synchronized with
// the callback.
void RenderWaveform(float, float, float, float);

// Draw the smoothed bars computed in Dsp.cpp; no FFT is performed here.
void RenderFFTSpectrum_PortAudio(float, float, float, float);
