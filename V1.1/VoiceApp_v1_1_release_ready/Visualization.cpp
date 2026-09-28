// Waveform, spectrum and text drawing using the shared SDL renderer.

#include "Visualization.h"
#include "Dsp.h"

// Format a floating-point value using the requested number of decimal places.
string FormatDouble(double value, int precision) noexcept
{

    try {
        return format("{:.{}f}", value, precision);
    }
    // If std::format fails, the ostringstream fallback still returns a fixed-decimal label.
    catch (...) {

    }

    ostringstream ss;
    ss.setf(ios::fixed);
    ss << setprecision(precision) << value;
    return ss.str();
}

// Convert a numeric value to text for slider legends.
string ValueToText(double value, int precision) noexcept
{
    return FormatDouble(value, precision);
}

// Append the unit suffix selected by WidgetUnit to a formatted value.
string FormatValueWithUnit(double value, WidgetUnit unit, int precision) noexcept
{
    string s = ValueToText(value, precision);
    switch (unit) {
    case WidgetUnit::Pts:     return s + " pts";
    case WidgetUnit::Hz:      return s + " Hz";
    case WidgetUnit::dB:      return s + " dB";
    // Units here are presentation only; the volume slider converts displayed percent separately in
    // AppUpdate.cpp.
    case WidgetUnit::percent: return s + " %";
    case WidgetUnit::cents:   return s + " cents";
    case WidgetUnit::ms:      return s + " ms";
    case WidgetUnit::degrees: return s + "°";
    default:                return s;
    }
}

// Query the rendered text dimensions after applying the requested scale.
void getSurfaceSize(const string& text, float& surfW, float& surfH, double scale) {
    surfW = surfH = 0.0f;

    int w = 0, h = 0;
    // The caller must supply an initialized font; this sizing helper does not check gFont for
    // null.
    TTF_GetStringSize(gFont, text.c_str(), text.size(), &w, &h);
    surfW = static_cast<float>(w) * scale;
    surfH = static_cast<float>(h) * scale;
}

// Create and draw an SDL_ttf text surface, then report its rendered dimensions.
bool RenderText(const string& text, float x, float y, SDL_Color color, float& outW, float& outH, double scale) {
    outW = outH = 0.0f;
    if (!gFont || !renderer) return false;
    // SDL_ttf creates a surface; create a texture from it, draw it, and release both temporary
    // resources.
    SDL_Surface* surf = TTF_RenderText_Blended(gFont, text.c_str(), text.size(), color);
    if (!surf) return false;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (!tex) {
        SDL_DestroySurface(surf);
        return false;
    }

    float baseW = static_cast<float>(surf->w);
    float baseH = static_cast<float>(surf->h);
    SDL_DestroySurface(surf);
    outW = baseW * scale;
    outH = baseH * scale;
    SDL_FRect dst = { x, y, outW, outH };
    SDL_RenderTexture(renderer, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    return true;
}

// Draw samples from the circular visualization buffer; this read is not fully synchronized with
// the callback.
void RenderWaveform(float x, float y, float width, float height)
{
    SDL_FRect background = { x, y, width, height };
    SDL_SetRenderDrawColorFloat(renderer, 0.15f, 0.15f, 0.2f, 0.9f);
    SDL_RenderFillRect(renderer, &background);

    SDL_SetRenderDrawColorFloat(renderer, 0.3f, 0.3f, 0.4f, 1.0f);
    SDL_RenderRect(renderer, &background);

    // Until capture has filled the ring, show a status label rather than reading uninitialized
    // display values.
    if (!waveformDataReady) {

        if (gFont) {
            SDL_Color textColor = { 200, 100, 100, 255 };
            float tw = 0.0f, th = 0.0f;
            RenderText("No audio input detected", x + 10, y + height / 2.0f, textColor, tw, th, 0.6);
        }
        return;
    }

    SDL_SetRenderDrawColorFloat(renderer, 0.4f, 0.4f, 0.5f, 0.6f);
    SDL_RenderLine(renderer, x, y + height / 2.0f, x + width, y + height / 2.0f);

    vector<SDL_FPoint> wavePoints;
    wavePoints.reserve(WAVEFORM_SAMPLES);

    // Begin at the next write position, which is the oldest value in a full circular ring.
    size_t readPos = waveformWritePos;
    float xStep = width / static_cast<float>(WAVEFORM_SAMPLES);

    float currentRMS = 0.0f;

    for (size_t i = 0; i < WAVEFORM_SAMPLES; ++i) {
        float sample = waveformBuffer[readPos];

        sample = Max(-1.0f, Min(1.0f, sample));

        float px = x + i * xStep;
        // Map signed amplitude to vertical pixels around the midline; positive samples point
        // upward.
        float py = y + height / 2.0f - (sample * height * 0.45f);

        wavePoints.push_back({ px, py });

        currentRMS += sample * sample;

        readPos = (readPos + 1) % WAVEFORM_SAMPLES;
    }

    // RMS summarizes the displayed ring, not the entire recording.
    currentRMS = sqrt(currentRMS / WAVEFORM_SAMPLES);

    if (wavePoints.size() >= 2) {
        for (size_t i = 1; i < wavePoints.size(); ++i) {
            float intensity = 0.5f + 0.5f * fabs(waveformBuffer[(waveformWritePos + i) % WAVEFORM_SAMPLES]);

            SDL_SetRenderDrawColorFloat(renderer,
                0.2f * intensity,
                0.8f + 0.2f * intensity,
                0.3f * intensity,
                1.0f);

            SDL_RenderLine(renderer,
                wavePoints[i - 1].x, wavePoints[i - 1].y,
                wavePoints[i].x, wavePoints[i].y);
        }
    }

    float peak = 0.0f;
    for (size_t i = 0; i < WAVEFORM_SAMPLES; ++i) {
        peak = Max(peak, fabs(waveformBuffer[i]));
    }

    if (gFont) {
        SDL_Color textColor = { 200, 255, 200, 255 };
        float tw = 0.0f, th = 0.0f;

        string label = "Audio Waveform (Oscilloscope)";
        RenderText(label, x + 10, y + 5, textColor, tw, th, 0.6);

        SDL_Color statsColor = (currentRMS > 0.01f) ? SDL_Color{ 100, 255, 100, 255 } : SDL_Color{ 200, 200, 200, 255 };
        string stats = "RMS: " + FormatDouble(currentRMS * 100.0, 2) + "% | Peak: " + FormatDouble(peak * 100.0, 1) + "%";
        RenderText(stats, x + 10, y + height - 25, statsColor, tw, th, 0.5);
    }
}

// Draw the smoothed bars computed in Dsp.cpp; no FFT is performed here.
void RenderFFTSpectrum_PortAudio(float x, float y, float width, float height)
{
    SDL_FRect background = { x, y, width, height };
    SDL_SetRenderDrawColorFloat(renderer, 0.1f, 0.1f, 0.15f, 0.8f);
    SDL_RenderFillRect(renderer, &background);

    SDL_SetRenderDrawColorFloat(renderer, 0.3f, 0.3f, 0.4f, 1.0f);
    SDL_RenderRect(renderer, &background);

    vector<SDL_FPoint> curvePoints;
    curvePoints.reserve(PA_NUM_BARS + 2);

    // Display bars use the smoothed analysis values; their frequency bins were chosen in Dsp.cpp.
    float barWidth = width / PA_NUM_BARS;

    curvePoints.push_back({ x, y + height });

    for (size_t i = 0; i < PA_NUM_BARS; ++i) {
        float magnitude = paFftSmoothed[i];
        float barHeight = magnitude * height * 0.9f;

        float pointX = x + (i + 0.5f) * barWidth;
        float pointY = y + height - barHeight;

        curvePoints.push_back({ pointX, pointY });
    }

    curvePoints.push_back({ x + width, y + height });

    if (curvePoints.size() >= 3) {
        // Build top/bottom vertex pairs and join each adjacent pair with two colored triangles.
        vector<SDL_Vertex> fillVertices;
        fillVertices.reserve(curvePoints.size() * 2);

        for (size_t i = 0; i < curvePoints.size(); ++i) {
            float hue = static_cast<float>(i) / curvePoints.size();
            float intensity = (curvePoints[i].y < y + height) ?
                1.0f - (curvePoints[i].y - y) / height : 0.0f;

            float r = Max(0.0f, Min(1.0f, (hue - 0.5f) * 2.0f + intensity * 0.3f));
            float g = Max(0.0f, Min(1.0f, 1.0f - fabs(hue - 0.5f) * 2.0f + intensity * 0.2f));
            float b = Max(0.0f, Min(1.0f, (0.5f - hue) * 2.0f + intensity * 0.1f));

            SDL_Vertex topVertex;
            topVertex.position = curvePoints[i];
            topVertex.color = { r, g, b, 0.6f };

            SDL_Vertex bottomVertex;
            bottomVertex.position = { curvePoints[i].x, y + height };
            bottomVertex.color = { r * 0.3f, g * 0.3f, b * 0.3f, 0.2f };

            fillVertices.push_back(topVertex);
            fillVertices.push_back(bottomVertex);
        }

        for (size_t i = 0; i < fillVertices.size() - 2; i += 2) {
            SDL_Vertex triangle[3] = {
                fillVertices[i],
                fillVertices[i + 1],
                fillVertices[i + 2]
            };
            SDL_RenderGeometry(renderer, nullptr, triangle, 3, nullptr, 0);

            if (i + 3 < fillVertices.size()) {
                SDL_Vertex triangle2[3] = {
                    fillVertices[i + 1],
                    fillVertices[i + 2],
                    fillVertices[i + 3]
                };
                SDL_RenderGeometry(renderer, nullptr, triangle2, 3, nullptr, 0);
            }
        }
    }

    if (curvePoints.size() >= 2) {
        for (size_t i = 1; i < curvePoints.size(); ++i) {
            float hue = static_cast<float>(i) / curvePoints.size();

            float r = Max(0.0f, Min(1.0f, (hue - 0.5f) * 2.0f + 0.5f));
            float g = Max(0.0f, Min(1.0f, 1.0f - fabs(hue - 0.5f) * 2.0f + 0.3f));
            float b = Max(0.0f, Min(1.0f, (0.5f - hue) * 2.0f + 0.2f));

            SDL_SetRenderDrawColorFloat(renderer, r, g, b, 1.0f);

            for (int offset = -1; offset <= 1; ++offset) {
                SDL_RenderLine(renderer,
                    curvePoints[i - 1].x, curvePoints[i - 1].y + offset,
                    curvePoints[i].x, curvePoints[i].y + offset);
            }
        }
    }

    if (gFont) {
        SDL_Color textColor = { 200, 200, 255, 255 };
        float tw = 0.0f, th = 0.0f;

        string label;
        // The spectrum title switches from progress to the calibrated display label when startup
        // calibration ends.
        if (paIsCalibrating) {
            int progress = (paCalibrationFrames * 100) / PA_CALIBRATION_FRAMES;
            label = "PortAudio - Noise calibration: " + to_string(progress) + "%";
        }
        else {
            label = "PortAudio Vocal Spectrum (80 Hz - 4 kHz @ 16 kHz) - Calibrated noise";
        }

        RenderText(label, x + 10, y + 5, textColor, tw, th, 0.6);
    }
}
