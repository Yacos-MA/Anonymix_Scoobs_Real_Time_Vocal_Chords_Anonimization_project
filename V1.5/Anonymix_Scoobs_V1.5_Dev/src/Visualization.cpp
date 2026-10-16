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
    surfW = static_cast<float>(w) * scale * 0.5f;
    surfH = static_cast<float>(h) * scale * 0.5f;
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
    outW = baseW * scale * 0.5f;
    outH = baseH * scale * 0.5f;
    SDL_FRect dst = { x, y, outW, outH };
    SDL_RenderTexture(renderer, tex, nullptr, &dst);
    SDL_DestroyTexture(tex);
    return true;
}

// Render both triangles as filled geometry. The 2x interface target smooths
// their boundaries without adding a darker outline or an alpha fringe.
void RenderSolidQuad(const SDL_Vertex (&triangles)[2][3]) {
    if (!renderer) return;
    SDL_RenderGeometry(renderer, nullptr, triangles[0], 3, nullptr, 0);
    SDL_RenderGeometry(renderer, nullptr, triangles[1], 3, nullptr, 0);
}

// Draw samples from the circular visualization buffer; this read is not fully synchronized with
// the callback.
void RenderWaveform(float x, float y, float width, float height)
{
    SDL_FRect background = { x, y, width, height };
    SDL_SetRenderDrawColorFloat(renderer, 0.15f, 0.15f, 0.2f, 0.9f);
    SDL_RenderFillRect(renderer, &background);

    // Until capture has filled the ring, show a status label rather than reading uninitialized
    // display values.
    if (!waveformDataReady.load(memory_order_acquire)) {

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

    // Copy the microphone ring quickly; drawing 2048 line segments never holds the
    // callback's waveform lock.
    array<float, WAVEFORM_SAMPLES> recent{};
    {
        lock_guard<mutex> lock(waveformMutex);
        for (size_t i = 0; i < WAVEFORM_SAMPLES; ++i)
            recent[i] = waveformBuffer[(waveformWritePos + i) % WAVEFORM_SAMPLES];
    }
    float xStep = width / static_cast<float>(WAVEFORM_SAMPLES);

    float currentRMS = 0.0f;

    for (size_t i = 0; i < WAVEFORM_SAMPLES; ++i) {
        float sample = recent[i];

        sample = Max(-1.0f, Min(1.0f, sample));

        float px = x + i * xStep;
        // Map signed amplitude to vertical pixels around the midline; positive samples point
        // upward.
        float py = y + height / 2.0f - (sample * height * 0.45f);

        wavePoints.push_back({ px, py });

        currentRMS += sample * sample;

    }

    // RMS summarizes the displayed ring, not the entire recording.
    currentRMS = sqrt(currentRMS / WAVEFORM_SAMPLES);

    // One batched geometry call replaces 2047 separate SDL draw submissions per frame.
    if (wavePoints.size() >= 2) {
        SDL_SetRenderDrawColorFloat(renderer, 0.3f, 0.92f, 0.35f, 1.0f);
        SDL_RenderLines(renderer, wavePoints.data(), static_cast<int>(wavePoints.size()));
    }

    float peak = 0.0f;
    for (size_t i = 0; i < WAVEFORM_SAMPLES; ++i) {
        peak = Max(peak, fabs(recent[i]));
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

// Draw the FFT at 8x and resolve through 4x and 2x before the 2x interface
// target reaches the window. Reuse the render targets between visual frames.
static SDL_Texture* spectrumTarget = nullptr;
static SDL_Texture* spectrumIntermediateTarget = nullptr;
static SDL_Texture* spectrumResolveTarget = nullptr;
static bool spectrumTargetUnavailable = false;
static int spectrumTargetWidth = 0;
static int spectrumTargetHeight = 0;
static int spectrumTargetScale = 0;

static void DestroySpectrumTargets() {
    if (spectrumTarget != nullptr) SDL_DestroyTexture(spectrumTarget);
    if (spectrumIntermediateTarget != nullptr)
        SDL_DestroyTexture(spectrumIntermediateTarget);
    if (spectrumResolveTarget != nullptr) SDL_DestroyTexture(spectrumResolveTarget);
    spectrumTarget = nullptr;
    spectrumIntermediateTarget = nullptr;
    spectrumResolveTarget = nullptr;
}

// Draw spectrum geometry without text; the title is rendered on the current interface target
// after the spectrum image is copied back into it.
static void DrawSpectrumBody(float x, float y, float width, float height,
    int samplingScale)
{
    SDL_FRect background = { x, y, width, height };
    SDL_SetRenderDrawColorFloat(renderer, 0.1f, 0.1f, 0.15f, 0.8f);
    SDL_RenderFillRect(renderer, &background);

    // The FFT still has 96 measured bands. Evaluate the curve once per target
    // pixel. Monotone cubic tangents keep each measured band height and round
    // the peak tips without inventing an overshoot or clipping one into a flat top.
    vector<SDL_FPoint> curvePoints;
    // The existing quarter-pixel curve tessellation is sufficient even when
    // the raster target is 8x; extra vertices would burden the 60 FPS UI.
    const int displaySegments = max(1,
        static_cast<int>(ceil(width * min(samplingScale, 4))));
    curvePoints.reserve(static_cast<size_t>(displaySegments) + 1);
    const int lastBand = static_cast<int>(PA_NUM_BARS) - 1;
    const auto magnitudeAt = [&](int band) {
        return paFftSmoothed[clamp(band, 0, lastBand)];
    };
    for (int pixelStep = 0; pixelStep <= displaySegments; ++pixelStep) {
        const float fractionX = static_cast<float>(pixelStep) / displaySegments;
        const float bandPosition = fractionX * PA_NUM_BARS - 0.5f;
        const int firstBand = static_cast<int>(floor(bandPosition));
        const float t = bandPosition - firstBand;
        const float p0 = magnitudeAt(firstBand - 1);
        const float p1 = magnitudeAt(firstBand);
        const float p2 = magnitudeAt(firstBand + 1);
        const float p3 = magnitudeAt(firstBand + 2);
        const float before = p1 - p0;
        const float between = p2 - p1;
        const float after = p3 - p2;
        const float startSlope = before * between > 0.0f
            ? 2.0f * before * between / (before + between) : 0.0f;
        const float endSlope = between * after > 0.0f
            ? 2.0f * between * after / (between + after) : 0.0f;
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float cubic = (2.0f * t3 - 3.0f * t2 + 1.0f) * p1 +
            (t3 - 2.0f * t2 + t) * startSlope +
            (-2.0f * t3 + 3.0f * t2) * p2 +
            (t3 - t2) * endSlope;
        const float magnitude = clamp(cubic, min(p1, p2), max(p1, p2));
        curvePoints.push_back({ x + fractionX * width,
            y + height - clamp(magnitude, 0.0f, 1.0f) * height * 0.9f });
    }

    if (curvePoints.size() >= 3) {
        // Keep the approved top-to-bottom fill. Shifting its interior vertices
        // sideways across narrow peaks created overlapping strips and streaks.
        vector<SDL_Vertex> fillVertices;
        fillVertices.reserve(curvePoints.size() * 2);

        for (size_t i = 0; i < curvePoints.size(); ++i) {
            float hue = static_cast<float>(i) / (curvePoints.size() - 1);
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

        vector<SDL_Vertex> triangles;
        triangles.reserve((curvePoints.size() - 1) * 6);
        for (size_t i = 0; i + 3 < fillVertices.size(); i += 2) {
            triangles.push_back(fillVertices[i]);
            triangles.push_back(fillVertices[i + 1]);
            triangles.push_back(fillVertices[i + 2]);
            triangles.push_back(fillVertices[i + 1]);
            triangles.push_back(fillVertices[i + 2]);
            triangles.push_back(fillVertices[i + 3]);
        }
        SDL_RenderGeometry(renderer, nullptr, triangles.data(),
            static_cast<int>(triangles.size()), nullptr, 0);
    }

    if (curvePoints.size() >= 2) {
        // Keep the reference's vertical halo, which is broad on flat valleys
        // and peak tips and naturally thin on steep sides. More raster samples
        // smooth the steep edges without widening the contour sideways.
        vector<SDL_Vertex> edgeVertices;
        constexpr array<float, 6> offsets{ -4.0f, -2.0f, -0.55f,
            0.55f, 2.0f, 4.0f };
        constexpr array<float, 6> opacities{ 0.0f, 0.28f, 1.0f,
            1.0f, 0.28f, 0.0f };
        edgeVertices.reserve(curvePoints.size() * offsets.size());
        for (const auto& point : curvePoints) {
            for (size_t edge = 0; edge < offsets.size(); ++edge) {
                SDL_Vertex vertex{};
                vertex.position = { point.x,
                    clamp(point.y + offsets[edge], y + 1.0f, y + height - 1.0f) };
                vertex.color = { 0.42f, 0.92f, 1.0f,
                    opacities[edge] };
                edgeVertices.push_back(vertex);
            }
        }
        vector<SDL_Vertex> edgeTriangles;
        edgeTriangles.reserve((curvePoints.size() - 1) *
            (offsets.size() - 1) * 6);
        for (size_t i = 0; i + 1 < curvePoints.size(); ++i) {
            for (size_t band = 0; band + 1 < offsets.size(); ++band) {
                const auto& topLeft = edgeVertices[i * offsets.size() + band];
                const auto& bottomLeft = edgeVertices[i * offsets.size() + band + 1];
                const auto& topRight = edgeVertices[(i + 1) * offsets.size() + band];
                const auto& bottomRight = edgeVertices[(i + 1) * offsets.size() + band + 1];
                edgeTriangles.push_back(topLeft);
                edgeTriangles.push_back(bottomLeft);
                edgeTriangles.push_back(topRight);
                edgeTriangles.push_back(bottomLeft);
                edgeTriangles.push_back(topRight);
                edgeTriangles.push_back(bottomRight);
            }
        }
        SDL_RenderGeometry(renderer, nullptr, edgeTriangles.data(),
            static_cast<int>(edgeTriangles.size()), nullptr, 0);
    }

}

// Reduce 8x -> 4x -> 2x -> the window; fall back to 4x, 2x or direct drawing
// when a renderer cannot allocate the larger texture chain.
void RenderFFTSpectrum_PortAudio(float x, float y, float width, float height)
{
    // The caller may itself be drawing into the 2x interface target. Restore
    // that target after the FFT passes so the remaining UI stays supersampled.
    SDL_Texture* previousTarget = SDL_GetRenderTarget(renderer);
    if (spectrumTarget != nullptr &&
        (static_cast<int>(ceil(width * spectrumTargetScale)) != spectrumTargetWidth ||
            static_cast<int>(ceil(height * spectrumTargetScale)) != spectrumTargetHeight)) {
        DestroySpectrumTargets();
    }
    if (!spectrumTargetUnavailable && spectrumTarget == nullptr) {
        for (int candidateScale : { 8, 4, 2 }) {
            const int targetWidth = static_cast<int>(ceil(width * candidateScale));
            const int targetHeight = static_cast<int>(ceil(height * candidateScale));
            spectrumTarget = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                SDL_TEXTUREACCESS_TARGET, targetWidth, targetHeight);
            if (spectrumTarget != nullptr && candidateScale == 8)
                spectrumIntermediateTarget = SDL_CreateTexture(renderer,
                    SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                    static_cast<int>(ceil(width * 4.0f)),
                    static_cast<int>(ceil(height * 4.0f)));
            if (spectrumTarget != nullptr &&
                (candidateScale != 8 || spectrumIntermediateTarget != nullptr) &&
                candidateScale >= 4)
                spectrumResolveTarget = SDL_CreateTexture(renderer,
                    SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                    static_cast<int>(ceil(width * 2.0f)),
                    static_cast<int>(ceil(height * 2.0f)));
            if (spectrumTarget != nullptr &&
                (candidateScale != 8 || spectrumIntermediateTarget != nullptr) &&
                (candidateScale < 4 || spectrumResolveTarget != nullptr)) {
                spectrumTargetScale = candidateScale;
                spectrumTargetWidth = targetWidth;
                spectrumTargetHeight = targetHeight;
                SDL_SetTextureScaleMode(spectrumTarget, SDL_SCALEMODE_LINEAR);
                if (spectrumIntermediateTarget != nullptr)
                    SDL_SetTextureScaleMode(spectrumIntermediateTarget, SDL_SCALEMODE_LINEAR);
                if (spectrumResolveTarget != nullptr)
                    SDL_SetTextureScaleMode(spectrumResolveTarget, SDL_SCALEMODE_LINEAR);
                break;
            }
            SDL_Log("FFT %dx render targets unavailable: %s", candidateScale,
                SDL_GetError());
            DestroySpectrumTargets();
        }
        spectrumTargetUnavailable = spectrumTarget == nullptr;
    }

    bool spectrumReady = false;
    if (spectrumTarget != nullptr) {
        const bool targetActive = SDL_SetRenderTarget(renderer, spectrumTarget);
        if (targetActive && SDL_SetRenderScale(renderer,
            static_cast<float>(spectrumTargetScale),
            static_cast<float>(spectrumTargetScale))) {
            SDL_SetRenderDrawColorFloat(renderer, 0.1f, 0.1f, 0.15f, 1.0f);
            SDL_RenderClear(renderer);
            DrawSpectrumBody(0.0f, 0.0f, width, height, spectrumTargetScale);
            spectrumReady = true;
        }
        if (spectrumReady && spectrumIntermediateTarget != nullptr) {
            spectrumReady = SDL_SetRenderTarget(renderer, spectrumIntermediateTarget);
            if (spectrumReady) {
                const SDL_FRect intermediateArea{ 0.0f, 0.0f,
                    width * 4.0f, height * 4.0f };
                spectrumReady = SDL_RenderTexture(renderer, spectrumTarget,
                    nullptr, &intermediateArea);
            }
        }
        if (spectrumReady && spectrumResolveTarget != nullptr) {
            spectrumReady = SDL_SetRenderTarget(renderer, spectrumResolveTarget);
            if (spectrumReady) {
                const SDL_FRect resolveArea{ 0.0f, 0.0f, width * 2.0f, height * 2.0f };
                SDL_Texture* input = spectrumIntermediateTarget != nullptr
                    ? spectrumIntermediateTarget : spectrumTarget;
                spectrumReady = SDL_RenderTexture(renderer, input,
                    nullptr, &resolveArea);
            }
        }
        SDL_SetRenderTarget(renderer, previousTarget);
        if (!spectrumReady) {
            SDL_Log("FFT render target setup failed: %s", SDL_GetError());
            DestroySpectrumTargets();
            spectrumTargetUnavailable = true;
        }
    }
    if (spectrumReady) {
        const SDL_FRect destination{ x, y, width, height };
        SDL_Texture* result = spectrumResolveTarget != nullptr
            ? spectrumResolveTarget : spectrumTarget;
        if (!SDL_RenderTexture(renderer, result, nullptr, &destination)) {
            SDL_Log("FFT texture copy failed: %s", SDL_GetError());
            DestroySpectrumTargets();
            spectrumTargetUnavailable = true;
            DrawSpectrumBody(x, y, width, height, 2);
        }
    } else {
        DrawSpectrumBody(x, y, width, height, 2);
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
            label = "Audio Spectrum (20 Hz - 20 kHz @ 48 kHz) - Calibrated noise";
        }

        RenderText(label, x + 10, y + 5, textColor, tw, th, 0.6);
    }
}

void CleanupSpectrumRenderTarget() {
    DestroySpectrumTargets();
}
