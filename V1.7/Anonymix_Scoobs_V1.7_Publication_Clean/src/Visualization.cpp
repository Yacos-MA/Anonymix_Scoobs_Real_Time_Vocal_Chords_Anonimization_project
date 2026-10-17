// Waveform, spectrum and text drawing using the shared SDL renderer.

#include "Visualization.h"
#include "Dsp.h"

namespace {
struct CachedLabel {
    string text;
    SDL_Color color;
    SDL_Texture* texture;
    int width;
    int height;
};

// Only short, stable UI labels enter this bounded cache. Changing a button caption may add
// one more entry, while per-frame waveform statistics continue through the transient path.
vector<CachedLabel> cachedLabels;
TTF_Font* cachedFont = nullptr;
SDL_Renderer* cachedRenderer = nullptr;
constexpr size_t maxCachedLabels = 128;
CachedLabel dynamicInfoLabel{ {}, { 0, 0, 0, 0 }, nullptr, 0, 0 };
// The continuously changing RMS/peak label has one replaceable texture rather than creating
// and destroying a GPU texture every visual frame. Its numbers refresh ten times per second;
// the waveform itself is still redrawn on every frame.
SDL_Texture* waveformStatsTexture = nullptr;
float waveformStatsWidth = 0.0f;
float waveformStatsHeight = 0.0f;
Uint64 waveformStatsUpdatedNs = 0;

void ClearWaveformStatsTexture() {
    if (waveformStatsTexture) SDL_DestroyTexture(waveformStatsTexture);
    waveformStatsTexture = nullptr;
    waveformStatsWidth = waveformStatsHeight = 0.0f;
    waveformStatsUpdatedNs = 0;
}

void EnsureTextCacheOwner() {
    if (cachedFont == gFont && cachedRenderer == renderer) return;
    for (auto& entry : cachedLabels) SDL_DestroyTexture(entry.texture);
    cachedLabels.clear();
    if (dynamicInfoLabel.texture) SDL_DestroyTexture(dynamicInfoLabel.texture);
    dynamicInfoLabel = { {}, { 0, 0, 0, 0 }, nullptr, 0, 0 };
    ClearWaveformStatsTexture();
    cachedFont = gFont;
    cachedRenderer = renderer;
}
} // namespace

void CleanupTextCache() {
    for (auto& entry : cachedLabels) SDL_DestroyTexture(entry.texture);
    cachedLabels.clear();
    if (dynamicInfoLabel.texture) SDL_DestroyTexture(dynamicInfoLabel.texture);
    dynamicInfoLabel = { {}, { 0, 0, 0, 0 }, nullptr, 0, 0 };
    ClearWaveformStatsTexture();
    cachedFont = nullptr;
    cachedRenderer = nullptr;
}

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

    EnsureTextCacheOwner();
    if (dynamicInfoLabel.texture && dynamicInfoLabel.text == text) {
        surfW = static_cast<float>(dynamicInfoLabel.width) * scale * 0.5f;
        surfH = static_cast<float>(dynamicInfoLabel.height) * scale * 0.5f;
        return;
    }
    for (const auto& entry : cachedLabels) {
        if (entry.text == text) {
            surfW = static_cast<float>(entry.width) * scale * 0.5f;
            surfH = static_cast<float>(entry.height) * scale * 0.5f;
            return;
        }
    }

    int w = 0, h = 0;
    // The caller must supply an initialized font; this sizing helper does not check gFont for
    // null.
    TTF_GetStringSize(gFont, text.c_str(), text.size(), &w, &h);
    surfW = static_cast<float>(w) * scale * 0.5f;
    surfH = static_cast<float>(h) * scale * 0.5f;
}

// Create and draw an SDL_ttf text surface, then report its rendered dimensions.
bool RenderText(const string& text, float x, float y, SDL_Color color, float& outW,
    float& outH, double scale, bool cacheStableLabel) {
    outW = outH = 0.0f;
    if (!gFont || !renderer) return false;
    EnsureTextCacheOwner();
    if (cacheStableLabel) {
        for (const auto& entry : cachedLabels) {
            if (entry.text == text && entry.color.r == color.r &&
                entry.color.g == color.g && entry.color.b == color.b &&
                entry.color.a == color.a) {
                outW = static_cast<float>(entry.width) * scale * 0.5f;
                outH = static_cast<float>(entry.height) * scale * 0.5f;
                const SDL_FRect dst{ x, y, outW, outH };
                return SDL_RenderTexture(renderer, entry.texture, nullptr, &dst);
            }
        }
    }
    // Dynamic text or a new stable label needs one SDL_ttf surface and one texture.
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
    const bool drawn = SDL_RenderTexture(renderer, tex, nullptr, &dst);
    if (cacheStableLabel && cachedLabels.size() < maxCachedLabels)
        cachedLabels.push_back({ text, color, tex, static_cast<int>(baseW),
            static_cast<int>(baseH) });
    else
        SDL_DestroyTexture(tex);
    return drawn;
}

bool RenderDynamicInfoText(const string& text, float x, float y, SDL_Color color,
    float& outW, float& outH, double scale) {
    outW = outH = 0.0f;
    if (!gFont || !renderer || text.empty()) return false;
    EnsureTextCacheOwner();

    // Hover and slider values can produce hundreds of distinct strings. Keeping them in
    // the fixed-caption cache evicted useful labels and eventually made every frame
    // allocate and destroy a GPU texture. This single slot is independent of that cache.
    if (!dynamicInfoLabel.texture || dynamicInfoLabel.text != text ||
        dynamicInfoLabel.color.r != color.r ||
        dynamicInfoLabel.color.g != color.g ||
        dynamicInfoLabel.color.b != color.b ||
        dynamicInfoLabel.color.a != color.a) {
        SDL_Surface* surface = TTF_RenderText_Blended(gFont, text.c_str(), text.size(), color);
        if (!surface) return false;
        SDL_Texture* replacement = SDL_CreateTextureFromSurface(renderer, surface);
        const int width = surface->w;
        const int height = surface->h;
        SDL_DestroySurface(surface);
        if (!replacement) return false;
        if (dynamicInfoLabel.texture) SDL_DestroyTexture(dynamicInfoLabel.texture);
        dynamicInfoLabel = { text, color, replacement, width, height };
    }

    outW = static_cast<float>(dynamicInfoLabel.width) * scale * 0.5f;
    outH = static_cast<float>(dynamicInfoLabel.height) * scale * 0.5f;
    const SDL_FRect destination{ x, y, outW, outH };
    return SDL_RenderTexture(renderer, dynamicInfoLabel.texture, nullptr, &destination);
}

// Render both triangles as filled geometry. The 2x interface target smooths
// their boundaries without adding a darker outline or an alpha fringe.
void RenderSolidQuad(const SDL_Vertex (&triangles)[2][3]) {
    if (!renderer) return;
    SDL_RenderGeometry(renderer, nullptr, triangles[0], 3, nullptr, 0);
    SDL_RenderGeometry(renderer, nullptr, triangles[1], 3, nullptr, 0);
}

// Snapshot the circular waveform under waveformMutex and release the lock before drawing it.
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
            RenderText("No audio input detected", x + 10, y + height / 2.0f, textColor, tw, th, 0.6, true);
        }
        return;
    }

    SDL_SetRenderDrawColorFloat(renderer, 0.4f, 0.4f, 0.5f, 0.6f);
    SDL_RenderLine(renderer, x, y + height / 2.0f, x + width, y + height / 2.0f);

    static vector<SDL_FPoint> wavePoints;
    wavePoints.clear();
    wavePoints.reserve(WAVEFORM_SAMPLES);

    // Copy the microphone ring quickly; drawing 2048 line segments never holds the
    // callback's waveform lock.
    array<float, WAVEFORM_SAMPLES> recent{};
    {
        lock_guard<mutex> lock(waveformMutex);
        // Copy the circular buffer in two contiguous spans. The old per-sample
        // modulo loop kept the same mutex that the audio callback needs for longer.
        const size_t start = waveformWritePos % WAVEFORM_SAMPLES;
        const size_t firstSpan = WAVEFORM_SAMPLES - start;
        copy_n(waveformBuffer.data() + start, firstSpan, recent.data());
        if (start != 0)
            copy_n(waveformBuffer.data(), start, recent.data() + firstSpan);
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
        RenderText(label, x + 10, y + 5, textColor, tw, th, 0.6, true);

        const Uint64 nowNs = SDL_GetTicksNS();
        constexpr Uint64 statsIntervalNs = 100000000ULL;
        if (!waveformStatsTexture || nowNs - waveformStatsUpdatedNs >= statsIntervalNs) {
            SDL_Color statsColor = (currentRMS > 0.01f)
                ? SDL_Color{ 100, 255, 100, 255 } : SDL_Color{ 200, 200, 200, 255 };
            string stats = "RMS: " + FormatDouble(currentRMS * 100.0, 2) +
                "% | Peak: " + FormatDouble(peak * 100.0, 1) + "%";
            SDL_Surface* statsSurface = TTF_RenderText_Blended(gFont, stats.c_str(),
                stats.size(), statsColor);
            if (statsSurface) {
                SDL_Texture* newTexture = SDL_CreateTextureFromSurface(renderer, statsSurface);
                if (newTexture) {
                    ClearWaveformStatsTexture();
                    waveformStatsTexture = newTexture;
                    waveformStatsWidth = statsSurface->w * 0.25f;
                    waveformStatsHeight = statsSurface->h * 0.25f;
                    waveformStatsUpdatedNs = nowNs;
                }
                SDL_DestroySurface(statsSurface);
            }
        }
        if (waveformStatsTexture) {
            const SDL_FRect dst{ x + 10, y + height - 25,
                waveformStatsWidth, waveformStatsHeight };
            SDL_RenderTexture(renderer, waveformStatsTexture, nullptr, &dst);
        }
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
    int samplingScale, bool targetAlreadyCleared = false)
{
    // The offscreen target was just cleared to the same opaque color. Repainting its
    // entire 8x surface with that color would add millions of redundant pixel writes.
    if (!targetAlreadyCleared) {
        SDL_FRect background = { x, y, width, height };
        SDL_SetRenderDrawColorFloat(renderer, 0.1f, 0.1f, 0.15f, 0.8f);
        SDL_RenderFillRect(renderer, &background);
    }

    // The FFT still has 96 measured bands. Evaluate the curve once per target
    // pixel. Monotone cubic tangents keep each measured band height and round
    // the peak tips without inventing an overshoot or clipping one into a flat top.
    static vector<SDL_FPoint> curvePoints;
    curvePoints.clear();
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
        static vector<SDL_Vertex> fillVertices;
        fillVertices.clear();
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

        // Share each colored vertex between neighboring triangles. The index order is
        // identical to the old expanded triangle list, so the pixels do not change.
        static vector<int> fillIndices;
        const size_t expectedFillIndices = (curvePoints.size() - 1) * 6;
        if (fillIndices.size() != expectedFillIndices) {
            fillIndices.clear();
            fillIndices.reserve(expectedFillIndices);
            for (size_t i = 0; i + 3 < fillVertices.size(); i += 2) {
                fillIndices.push_back(static_cast<int>(i));
                fillIndices.push_back(static_cast<int>(i + 1));
                fillIndices.push_back(static_cast<int>(i + 2));
                fillIndices.push_back(static_cast<int>(i + 1));
                fillIndices.push_back(static_cast<int>(i + 2));
                fillIndices.push_back(static_cast<int>(i + 3));
            }
        }
        SDL_RenderGeometry(renderer, nullptr, fillVertices.data(),
            static_cast<int>(fillVertices.size()), fillIndices.data(),
            static_cast<int>(fillIndices.size()));
    }

    if (curvePoints.size() >= 2) {
        // Keep the reference's vertical halo, which is broad on flat valleys
        // and peak tips and naturally thin on steep sides. More raster samples
        // smooth the steep edges without widening the contour sideways.
        static vector<SDL_Vertex> edgeVertices;
        edgeVertices.clear();
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
        static vector<int> edgeIndices;
        const size_t expectedEdgeIndices = (curvePoints.size() - 1) *
            (offsets.size() - 1) * 6;
        if (edgeIndices.size() != expectedEdgeIndices) {
            edgeIndices.clear();
            edgeIndices.reserve(expectedEdgeIndices);
            for (size_t i = 0; i + 1 < curvePoints.size(); ++i) {
                for (size_t band = 0; band + 1 < offsets.size(); ++band) {
                    const int topLeft = static_cast<int>(i * offsets.size() + band);
                    const int bottomLeft = topLeft + 1;
                    const int topRight = static_cast<int>((i + 1) * offsets.size() + band);
                    const int bottomRight = topRight + 1;
                    edgeIndices.push_back(topLeft);
                    edgeIndices.push_back(bottomLeft);
                    edgeIndices.push_back(topRight);
                    edgeIndices.push_back(bottomLeft);
                    edgeIndices.push_back(topRight);
                    edgeIndices.push_back(bottomRight);
                }
            }
        }
        SDL_RenderGeometry(renderer, nullptr, edgeVertices.data(),
            static_cast<int>(edgeVertices.size()), edgeIndices.data(),
            static_cast<int>(edgeIndices.size()));
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
            DrawSpectrumBody(0.0f, 0.0f, width, height, spectrumTargetScale, true);
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
            DestroySpectrumTargets();
            spectrumTargetUnavailable = true;
        }
    }
    if (spectrumReady) {
        const SDL_FRect destination{ x, y, width, height };
        SDL_Texture* result = spectrumResolveTarget != nullptr
            ? spectrumResolveTarget : spectrumTarget;
        if (!SDL_RenderTexture(renderer, result, nullptr, &destination)) {
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

        RenderText(label, x + 10, y + 5, textColor, tw, th, 0.6, true);
    }
}

void CleanupSpectrumRenderTarget() {
    DestroySpectrumTargets();
}
