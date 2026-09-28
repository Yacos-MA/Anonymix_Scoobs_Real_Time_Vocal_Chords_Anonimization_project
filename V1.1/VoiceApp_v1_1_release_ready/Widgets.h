// Project UI widgets. The header defines inline class methods for geometry, hit testing, sliders, buttons and text rendering.

#pragma once
#include "Visualization.h"
#include "Audio.h"

// One drawn and interactive control. center is its logical center; handleCenter tracks a draggable
// slider handle.
class UiWidget {
private:
    // The collection assigns positive IDs; name is the stable lookup key used by App.cpp and
    // AppUpdate.cpp.
    int id;
    string name;
    WidgetType type;
    // Two half diagonals and their angle determine the quad vertices. figureAngle rotates it
    // during animation.
    uint16_t halfDiag1, halfDiag2;
    double diagonalAngle = 0, figureAngle = 0, figureAngleOffset = 0;
    double figureZoom = 1;
    double zoomFactor = 1.15;
    bool diagonalAngleAbove90 = false;
    // Keep the logical center, draggable handle center and click offset separate to avoid a visual
    // jump while dragging.
    SDL_FPoint center = { 0, 0 };
    SDL_FPoint handleCenter = { 0, 0 };
    SDL_FPoint centerToHandleOffset = { 0, 0 };
    // Event-type codes are interpreted by the event and drawing routines.
    uint8_t eventType = 0;
    SDL_Vertex pointsTri[trianglesPerQuad][verticesPerTriangle];
    // These pointers refer into pointsTri and are reordered for hit testing; copying a widget needs special care.
    SDL_Vertex* verticesSortedByX[widgetVertexCount];
    SDL_Vertex* verticesSortedByY[widgetVertexCount];
    SDL_FPoint hitBoxMin, hitBoxMax;

    bool isMouseLeftButtonDown = false;

    // bgRect/bgRect2 store the slider track and handle decoration; geometry is recalculated when
    // type or size changes.
    SDL_FRect bgRect = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool hasBackground = false;

    SDL_FRect bgRect2 = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool hasBgRect2 = false;

    // SDL geometry colors use normalized 0..1 floating channels, while SDL_Color text uses 0..255
    // bytes.
    float bg_r = 0.12f;
    float bg_g = 0.12f;
    float bg_b = 0.12f;
    float bg_a = 1.0f;

    float slider_r = 0.0f;
    float slider_g = 0.0f;
    float slider_b = 0.0f;
    float slider_a = 1.0f;

    // Initialize pixel bounds before the constructor reads them; the UI later
    // supplies each control-specific range.
    double slider_xMin = 0.0;
    double slider_xMax = 1.0;

    double slider_yMin = 0.0;
    double slider_yMax = 1.0;

    // Pixel coordinates above are independent of the user-visible ranges
    // below. A vertical slider reverses the mapping to show high at the top.
    double legend_xMin_Val = 0;
    double legend_xMax_Val = 100;

    double legend_yMin_Val = 0;
    double legend_yMax_Val = 100;

    double slider_xVal = 0.0;
    double slider_yVal = 0.0;

    double legend_xVal = 0.0;
    double legend_yVal = 0.0;

    // Legend endpoint text is independent of live numeric labels; App.cpp configures both for each
    // slider.
    string legend_xMin_Text;
    string legend_xMax_Text;

    string legend_yMin_Text;
    string legend_yMax_Text;

    string legend_xText;
    string legend_yText;

    // Unit suffixes apply when FormatValueWithUnit renders live X and Y labels.
    WidgetUnit legendX_Unit = WidgetUnit::Pts;
    WidgetUnit legendY_Unit = WidgetUnit::Pts;

    double currentValue = 0.0f;

    double sliderMinX = 0.0f, sliderMaxX = 1.0f, sliderValueX = 0.0f;
    double sliderMinY = 0.0f, sliderMaxY = 1.0f, sliderValueY = 0.0f;

    int intervalCountX = 0;
    double rangeSizeX = 0;
    int intervalCountY = 0;
    double rangeSizeY = 0;

    // Button actions belong to the widget; App.cpp changes start/stop actions and their visible
    // captions together.
    WidgetAction buttonFunction = WidgetAction::None;

    string textboxText;
    SDL_Color textboxColor = { 0, 0, 0, 255 };

    // Only slider types use the specialized track, handle and drag behavior.
    static inline bool IsSliderType(WidgetType t) noexcept {
        switch (t) {
        case WidgetType::SliderH_PosN:
        case WidgetType::SliderH_FreeLimited:
        case WidgetType::SliderV_PosN:
        case WidgetType::SliderV_FreeLimited:
        case WidgetType::Slider2D_PosN:
        case WidgetType::Slider2D_FreeLimited:
            return true;
        default:
            return false;
        }
    }

    // Inclusive rectangle hit test in renderer pixel coordinates.
    static inline bool pointInRect(const SDL_FRect& r, float px, float py) noexcept {
        return (px >= r.x) && (px <= r.x + r.w) && (py >= r.y) && (py <= r.y + r.h);
    }

    // Recalculate the slider track and handle rectangles after geometry changes.
    void updateBackgroundRect() noexcept {
        // Non-slider types have no slider track or handle background.
        if (!IsSliderType(this->type)) {
            this->hasBackground = false;
            this->hasBgRect2 = false;
            this->bgRect = { 0.0f, 0.0f, 0.0f, 0.0f };
            this->bgRect2 = { 0.0f, 0.0f, 0.0f, 0.0f };
            return;
        }

        // For horizontal sliders, width follows the X axis; vertical sliders swap width and height
        // assumptions.
        if (this->type == WidgetType::SliderH_PosN || this->type == WidgetType::SliderH_FreeLimited) {
            float w = 440.0f;
            float h = max(8.0f, static_cast<float>(this->halfDiag2) * 1.5f);

            this->bgRect2.w = w;
            this->bgRect.h = h;
            this->bgRect2.x = this->center.x - w / 2.0f;
            this->bgRect.y = this->center.y - h / 2.0f;

            this->bgRect2.h = max(12.0f, this->bgRect.h / 4.0f);
            this->bgRect.w = max(52.0f, this->bgRect2.w + 20.0f);
            this->bgRect.x = this->center.x - this->bgRect.w / 2.0f;
            this->bgRect2.y = this->center.y - this->bgRect2.h / 2.0f;
        }
        else if (this->type == WidgetType::SliderV_PosN || this->type == WidgetType::SliderV_FreeLimited) {
            float h = 440.0f;
            float w = max(8.0f, static_cast<float>(this->halfDiag1) * 1.5f);

            this->bgRect.w = w;
            this->bgRect2.h = h;
            this->bgRect.x = this->center.x - w / 2.0f;
            this->bgRect2.y = this->center.y - h / 2.0f;

            this->bgRect2.w = max(12.0f, this->bgRect.w / 4.0f);
            this->bgRect.h = max(52.0f, this->bgRect2.h + 20.0f);
            this->bgRect2.x = this->center.x - this->bgRect2.w / 2.0f;
            this->bgRect.y = this->center.y - this->bgRect.h / 2.0f;
        }
        else {
            float w = 440.0f;
            float h = w;

            this->bgRect2.w = max(12.0f, w);
            this->bgRect2.h = max(12.0f, h);
            this->bgRect2.x = this->center.x - w / 2.0f;
            this->bgRect2.y = this->center.y - h / 2.0f;

            this->bgRect.w = max(52.0f, this->bgRect2.w + 20.0f);
            this->bgRect.h = max(52.0f, this->bgRect2.h + 20.0f);
            this->bgRect.x = this->center.x - this->bgRect.w / 2.0f;
            this->bgRect.y = this->center.y - this->bgRect.h / 2.0f;
        }

        this->hasBackground = true;
        this->hasBgRect2 = true;
    }

public:

    // Reject invalid IDs and empty names, initialize vertex geometry and configure an optional control
    // type.
    // Initialize all geometry, numeric ranges and initial labels before callers set their final
    // per-widget values.
    UiWidget(int id, const string& name, uint16_t halfDiag1 = 50, uint16_t halfDiag2 = 50, double diagonalAngle = SDL_PI_D / 2, double figureAngleOffset = 0, double figureZoom = 1, float centerX = 0, float centerY = 0, WidgetType widgetType = WidgetType::Static_Full, uint8_t initialEventType = 0) {
        if (id <= 0) {
            throw invalid_argument("The ID must be positive.");
        }
        if (name.empty()) {
            throw invalid_argument("The name cannot be empty.");
        }
        this->id = id;
        this->name = name;
        this->type = widgetType;
        this->halfDiag1 = halfDiag1;
        this->halfDiag2 = halfDiag2;
        this->diagonalAngle = fmod(diagonalAngle, SDL_PI_D);
        this->figureAngleOffset = figureAngleOffset;
        this->figureAngle = figureAngleOffset;
        this->figureZoom = figureZoom;
        this->center.x = centerX;
        this->center.y = centerY;
        this->handleCenter.x = this->center.x + this->centerToHandleOffset.x;
        this->handleCenter.y = this->center.y + this->centerToHandleOffset.y;

        this->slider_xVal = this->handleCenter.x;
        this->legend_xVal = legend_xMin_Val + (slider_xVal - slider_xMin) * (legend_xMax_Val - legend_xMin_Val) / (slider_xMax - slider_xMin);
        this->legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);

        this->slider_yVal = this->handleCenter.y;
        this->legend_yVal = legend_yMax_Val - (slider_yVal - slider_yMin) * (legend_yMax_Val - legend_yMin_Val) / (slider_yMax - slider_yMin);
        this->legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);

        this->eventType = initialEventType;

        this->diagonalAngleAbove90 = (diagonalAngle > SDL_PI_D / 2);

        if (this->halfDiag1 > this->halfDiag2) {
            this->hitBoxMin.x = this->handleCenter.x - (this->halfDiag1 * this->figureZoom);
            this->hitBoxMin.y = this->handleCenter.y - (this->halfDiag1 * this->figureZoom);
            this->hitBoxMax.x = this->handleCenter.x + (this->halfDiag1 * this->figureZoom);
            this->hitBoxMax.y = this->handleCenter.y + (this->halfDiag1 * this->figureZoom);
        }
        else {
            this->hitBoxMin.x = this->handleCenter.x - (this->halfDiag2 * this->figureZoom);
            this->hitBoxMin.y = this->handleCenter.y - (this->halfDiag2 * this->figureZoom);
            this->hitBoxMax.x = this->handleCenter.x + (this->halfDiag2 * this->figureZoom);
            this->hitBoxMax.y = this->handleCenter.y + (this->halfDiag2 * this->figureZoom);
        }

        // The generic geometry path computes vertices from polar distances and angles, then sorts
        // them for the hit test.
        double pointAngle, pointDist;

        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 3; j++) {
                pointAngle = (j == 1) * this->diagonalAngle + int(0.5 * ((2 * i + j) % 4)) * SDL_PI_D;
                pointDist = (j != 1) * this->halfDiag1 + (j == 1) * this->halfDiag2;
                pointDist = pointDist * this->figureZoom;
                this->pointsTri[i][j].position.x = this->handleCenter.x + pointDist * cos(pointAngle + this->figureAngle);
                this->pointsTri[i][j].position.y = this->handleCenter.y + pointDist * sin(pointAngle + this->figureAngle);
                this->pointsTri[i][j].color.r = 0.0;
                this->pointsTri[i][j].color.g = 0.0;
                this->pointsTri[i][j].color.b = 0.0;
                this->pointsTri[i][j].color.a = 1.0;

                if (j < 2) {
                    verticesSortedByX[i * 2 + j] = &pointsTri[i][j];
                    verticesSortedByY[i * 2 + j] = &pointsTri[i][j];
                }
            }
        }

        // Pointer arrays sort the same underlying six triangle vertices on X and Y for bounding
        // and hit testing.
        SDL_Vertex* temporaryVertex;

        for (int i = 0; i < 3; i++) {
            for (int j = i + 1; j < 4; j++) {
                if (this->verticesSortedByX[j]->position.x < this->verticesSortedByX[i]->position.x) {
                    temporaryVertex = verticesSortedByX[i];
                    verticesSortedByX[i] = verticesSortedByX[j];
                    verticesSortedByX[j] = temporaryVertex;
                }
                if (this->verticesSortedByY[j]->position.y < this->verticesSortedByY[i]->position.y) {
                    temporaryVertex = verticesSortedByY[i];
                    verticesSortedByY[i] = verticesSortedByY[j];
                    verticesSortedByY[j] = temporaryVertex;
                }
            }
        }

        hitBoxMin.x = this->verticesSortedByX[0]->position.x;
        hitBoxMin.y = this->verticesSortedByY[0]->position.y;
        hitBoxMax.x = this->verticesSortedByX[3]->position.x;
        hitBoxMax.y = this->verticesSortedByY[3]->position.y;

        updateBackgroundRect();

    }

    // Read-only accessors expose geometry and labels to AppRender; setters below keep derived
    // slider values in sync.
    int getId() const { return id; }
    string getName() const { return name; }

    WidgetType getType() const { return type; }

    uint16_t getHalfDiag1() { return halfDiag1; }
    uint16_t getHalfDiag2() { return halfDiag2; }
    double getDiagonalAngle() { return diagonalAngle; }
    double getFigureAngle() { return figureAngle; }
    double getFigureAngleOffset() { return figureAngleOffset; }
    double getFigureZoom() { return figureZoom; }

    double getZoomFactor() { return zoomFactor; }

    float getCenterX() { return center.x; }
    float getCenterY() { return center.y; }

    float getHandleCenterX() { return handleCenter.x; }
    float getHandleCenterY() { return handleCenter.y; }

    float getCenterToHandleOffsetX() { return centerToHandleOffset.x; }
    float getCenterToHandleOffsetY() { return centerToHandleOffset.y; }

    float getBgRect2w() { return bgRect2.w; }
    float getBgRect2h() { return bgRect2.h; }

    float getEventType() { return eventType; }

    float getPointPosX(uint8_t triangle, uint8_t point) { return pointsTri[triangle][point].position.x; }
    float getPointPosY(uint8_t triangle, uint8_t point) { return pointsTri[triangle][point].position.y; }
    float getPointColorR(uint8_t triangle, uint8_t point) { return pointsTri[triangle][point].color.r; }
    float getPointColorG(uint8_t triangle, uint8_t point) { return pointsTri[triangle][point].color.g; }
    float getPointColorB(uint8_t triangle, uint8_t point) { return pointsTri[triangle][point].color.b; }

    bool get_isMouseLeftButtonDown() { return isMouseLeftButtonDown; }

    double getLegend_xMin_Val() { return legend_xMin_Val; }
    double getLegend_xMax_Val() { return legend_xMax_Val; }

    double getLegend_yMin_Val() { return legend_yMin_Val; }
    double getLegend_yMax_Val() { return legend_yMax_Val; }

    double getLegend_xVal() { return legend_xVal; }
    double getLegend_yVal() { return legend_yVal; }

    string getLegend_xMin_Text() { return legend_xMin_Text; }
    string getLegend_xMax_Text() { return legend_xMax_Text; }

    string getLegend_yMin_Text() { return legend_yMin_Text; }
    string getLegend_yMax_Text() { return legend_yMax_Text; }

    string getLegend_xText() { return legend_xText; }
    string getLegend_yText() { return legend_yText; }

    WidgetUnit getLegendX_Unit() { return legendX_Unit; }
    WidgetUnit getLegendY_Unit() { return legendY_Unit; }

    int getStepCountX() { return intervalCountX; }
    int getStepCountY() { return intervalCountY; }

    double getRangeSizeX() { return rangeSizeX; }
    double getRangeSizeY() { return rangeSizeY; }

    WidgetAction getButtonFunction() { return buttonFunction; }

    // Keep collection IDs positive; add() assigns them sequentially.
    void setId(int newId) {
        if (newId <= 0) throw invalid_argument("The ID must be positive.");
        id = newId;
    }
    // Changing a name breaks any lookup by its old string in other modules.
    void setName(const string& newName) {
        if (newName.empty()) throw invalid_argument("The name cannot be empty.");
        name = newName;
    }

    // Switch control behavior and refresh slider geometry; configure the associated bounds afterward.
    void setType(WidgetType newType) {
        using UT = underlying_type_t<WidgetType>;
        UT v = static_cast<UT>(newType);
        if (v < static_cast<UT>(WidgetType::Static_Full) || v > static_cast<UT>(WidgetType::List)) {
            throw invalid_argument("Invalid value.");
        }
        type = newType;

        updateBackgroundRect();

        if (type != WidgetType::SliderH_PosN && type != WidgetType::SliderH_FreeLimited && type != WidgetType::Slider2D_FreeLimited && type != WidgetType::Slider2D_PosN) {
            slider_xMin = center.x;
            slider_xMax = center.x;
        }
        else {
            slider_xMin = center.x - bgRect2.w / 2 + 20;
            slider_xMax = center.x + bgRect2.w / 2 - 20;
        }

        if (type != WidgetType::SliderV_PosN && type != WidgetType::SliderV_FreeLimited && type != WidgetType::Slider2D_FreeLimited && type != WidgetType::Slider2D_PosN) {
            slider_yMin = center.y;
            slider_yMax = center.y;
        }
        else {
            slider_yMin = center.y - bgRect2.h / 2 + 20;
            slider_yMax = center.y + bgRect2.h / 2 - 20;
        }
    }

    // Geometry changes also refresh slider track dimensions when the widget is a slider.
    void setHalfDiag1(uint16_t length) { halfDiag1 = length; adaptZoomToLongestDiagonal(); if (IsSliderType(type)) updateBackgroundRect(); }
    void setHalfDiag2(uint16_t length) { halfDiag2 = length; adaptZoomToLongestDiagonal(); if (IsSliderType(type)) updateBackgroundRect(); }

    void setDiagonalAngle(double angle = 0.0) { diagonalAngle = angle; }
    void setFigureAngle(double angle = 0.0) { figureAngle = angle; }
    void setFigureAngleOffset(double angle = 0.0) { figureAngleOffset = angle; }
    void setFigureZoom(double zoom = 1.0) { figureZoom = zoom; }

    void setZoomFactor(double zoom = 1.15) { zoomFactor = zoom; }

    // Scale the figure relative to its largest half diagonal so hit testing tracks the visible shape.
    void adaptZoomToLongestDiagonal() {
        double longestDiag = static_cast<double>(max(halfDiag1, halfDiag2));
        if (longestDiag > 110) {
            zoomFactor = 1 + 0.15 * pow(110.0f / longestDiag, 1.4);
        }
        else {
            zoomFactor = 1.15;
        }
    }

    // Move the widget horizontally and keep its slider handle offset consistent with its center.
    // A center change adjusts geometry and preserves the relative handle position.
    void setCenterX(uint16_t posX = 0) {
        center.x = posX;
        handleCenter.x = center.x + centerToHandleOffset.x;
        if (IsSliderType(type)) updateBackgroundRect();

        if (type != WidgetType::SliderH_PosN && type != WidgetType::SliderH_FreeLimited && type != WidgetType::Slider2D_FreeLimited && type != WidgetType::Slider2D_PosN) {
            slider_xMin = center.x;
            slider_xMax = center.x;
        }
        else {
            slider_xMin = center.x - bgRect2.w / 2 + 20;
            slider_xMax = center.x + bgRect2.w / 2 - 20;
        }

    }
    // Move the widget vertically and keep its slider handle offset consistent with its center.
    void setCenterY(uint16_t posY = 0) {
        center.y = posY;
        handleCenter.y = center.y + centerToHandleOffset.y;
        if (IsSliderType(type)) updateBackgroundRect();

        if (type != WidgetType::SliderV_PosN && type != WidgetType::SliderV_FreeLimited && type != WidgetType::Slider2D_FreeLimited && type != WidgetType::Slider2D_PosN) {
            slider_yMin = center.y;
            slider_yMax = center.y;
        }
        else {
            slider_yMin = center.y - bgRect2.h / 2 + 20;
            slider_yMax = center.y + bgRect2.h / 2 - 20;
        }

    }

    // Handle movement updates the stored offset from center; the Y setter mirrors this behavior.
    void setHandleCenterX(uint16_t posX = 0) { handleCenter.x = posX; centerToHandleOffset.x = handleCenter.x - center.x; }
    void setHandleCenterY(uint16_t posY = 0) { handleCenter.y = posY; centerToHandleOffset.y = handleCenter.y - center.y; }

    void setCenterToHandleOffsetX(uint16_t posX = 0) {
        centerToHandleOffset.x = posX; handleCenter.x = center.x + centerToHandleOffset.x;
    }
    void setCenterToHandleOffsetY(uint16_t posY = 0) {
        centerToHandleOffset.y = posY; handleCenter.y = center.y + centerToHandleOffset.y;
    }

    void setEventType(uint8_t newEventType) { eventType = newEventType; }

    // Change one rendered vertex; SDL geometry interpolates these color channels across triangles.
    // Individual vertex colors create the gradient rendered across each SDL triangle.
    void setPointColorRGB(uint8_t triangle, uint8_t point, float color_R = 0.0, float color_G = 0.0, float color_B = 0.0) {
        pointsTri[triangle][point].color.r = color_R;
        pointsTri[triangle][point].color.g = color_G;
        pointsTri[triangle][point].color.b = color_B;
    }

    // Apply a uniform color to the three vertices of one triangle.
    void setTriangleColorRGB(uint8_t triangle, float color_R = 0.0, float color_G = 0.0, float color_B = 0.0) {
        for (int j = 0; j < 3; j++) {
            pointsTri[triangle][j].color.r = color_R;
            pointsTri[triangle][j].color.g = color_G;
            pointsTri[triangle][j].color.b = color_B;
        }
    }

    // Apply a uniform color to every triangle of this widget.
    void setWidgetColorRGB(float color_R = 0.0, float color_G = 0.0, float color_B = 0.0) {
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 3; j++) {
                pointsTri[i][j].color.r = color_R;
                pointsTri[i][j].color.g = color_G;
                pointsTri[i][j].color.b = color_B;
            }
        }
    }

    // Update the pressed appearance of a button using its existing color values.
    void updatePressedColorRGB() {
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 3; j++) {
                pointsTri[i][j].color.r = pointsTri[i][j].color.r + (0.5 - (isMouseLeftButtonDown == true)) * 2 * mouseLeftButtonClick_Color_OFFSET;
                pointsTri[i][j].color.g = pointsTri[i][j].color.g + (0.5 - (isMouseLeftButtonDown == true)) * 2 * mouseLeftButtonClick_Color_OFFSET;
                pointsTri[i][j].color.b = pointsTri[i][j].color.b + (0.5 - (isMouseLeftButtonDown == true)) * 2 * mouseLeftButtonClick_Color_OFFSET;
            }
        }
    }

    // Track and handle colors have separate setters; values are normalized SDL float colors.
    void setBgColor(float r, float g, float b, float a = 1.0f) noexcept {
        auto clamp01 = [](float v) -> float { if (v < 0.0f) return 0.0f; if (v > 1.0f) return 1.0f; return v; };
        bg_r = clamp01(r);
        bg_g = clamp01(g);
        bg_b = clamp01(b);
        bg_a = clamp01(a);
    }

    void getBgColor(float& r, float& g, float& b, float& a) const noexcept {
        r = bg_r; g = bg_g; b = bg_b; a = bg_a;
    }

    void setBgKnobColor(float r, float g, float b, float a = 1.0f) noexcept {
        auto clamp01 = [](float v) -> float { if (v < 0.0f) return 0.0f; if (v > 1.0f) return 1.0f; return v; };
        slider_r = clamp01(r);
        slider_g = clamp01(g);
        slider_b = clamp01(b);
        slider_a = clamp01(a);
    }

    void getBgKnobColor(float& r, float& g, float& b, float& a) const noexcept {
        r = slider_r; g = slider_g; b = slider_b; a = slider_a;
    }

    void set_isMouseLeftButtonDown(bool MouseLeftButtonDown_state) { isMouseLeftButtonDown = MouseLeftButtonDown_state; }

    // Pixel track bounds are set before legend bounds; setters using the current handle value
    // divide by the track span.
    void setSlider_xMin(double val) noexcept { slider_xMin = val; }
    void setSlider_xMax(double val) noexcept { slider_xMax = val; }

    void setSlider_yMin(double val) noexcept { slider_yMin = val; }
    void setSlider_yMax(double val) noexcept { slider_yMax = val; }

    // Convert a horizontal handle coordinate to its displayed value within configured bounds.
    void setSlider_xVal(double val) noexcept {
        slider_xVal = val;
        legend_xVal = legend_xMin_Val + (slider_xVal - slider_xMin) * (legend_xMax_Val - legend_xMin_Val) / (slider_xMax - slider_xMin);
        legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);
    }

    // Convert a vertical handle coordinate to its displayed value within configured bounds.
    void setSlider_yVal(double val) noexcept {
        slider_yVal = val;
        legend_yVal = legend_yMax_Val - (slider_yVal - slider_yMin) * (legend_yMax_Val - legend_yMin_Val) / (slider_yMax - slider_yMin);
        legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);
    }

    double getSlider_xMin() const noexcept { return slider_xMin; }
    double getSlider_xMax() const noexcept { return slider_xMax; }

    double getSlider_yMin() const noexcept { return slider_yMin; }
    double getSlider_yMax() const noexcept { return slider_yMax; }

    double getSlider_xVal() const noexcept { return slider_xVal; }
    double getSlider_yVal() const noexcept { return slider_yVal; }

    // Update the lower X-axis display bound and recompute the current label value.
    void setLegend_xMin_Val(double minimumXLegend) {
        legend_xMin_Val = minimumXLegend;
        slider_xVal = handleCenter.x;
        legend_xVal = legend_xMin_Val + (slider_xVal - slider_xMin) * (legend_xMax_Val - legend_xMin_Val) / (slider_xMax - slider_xMin);
        legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);
    }
    // Update the upper X-axis display bound and recompute the current label value.
    void setLegend_xMax_Val(double maximumXLegend) {
        legend_xMax_Val = maximumXLegend;
        slider_xVal = handleCenter.x;
        legend_xVal = legend_xMin_Val + (slider_xVal - slider_xMin) * (legend_xMax_Val - legend_xMin_Val) / (slider_xMax - slider_xMin);
        legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);
    }

    // Update the lower Y-axis display bound and recompute the current label value.
    void setLegend_yMin_Val(double minimumYLegend) {
        legend_yMin_Val = minimumYLegend;
        slider_yVal = handleCenter.y;
        legend_yVal = legend_yMax_Val - (slider_yVal - slider_yMin) * (legend_yMax_Val - legend_yMin_Val) / (slider_yMax - slider_yMin);
        legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);
    }
    // Update the upper Y-axis display bound and recompute the current label value.
    void setLegend_yMax_Val(float maximumYLegend) {
        legend_yMax_Val = maximumYLegend;
        slider_yVal = handleCenter.y;
        legend_yVal = legend_yMax_Val - (slider_yVal - slider_yMin) * (legend_yMax_Val - legend_yMin_Val) / (slider_yMax - slider_yMin);
        legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);
    }

    // Changing the value directly also rebuilds the formatted label text.
    void setLegend_xVal(double xLegend) {
        legend_xVal = xLegend;
        legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);
    }
    void setLegend_yVal(double yLegend) {
        legend_yVal = yLegend;
        legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);
    }

    void setLegend_xMin_Text(string xMinLegendStr) { legend_xMin_Text = xMinLegendStr; }
    void setLegend_xMax_Text(string xMaxLegendStr) { legend_xMax_Text = xMaxLegendStr; }

    void setLegend_yMin_Text(string yMinLegendStr) { legend_yMin_Text = yMinLegendStr; }
    void setLegend_yMax_Text(string yMaxLegendStr) { legend_yMax_Text = yMaxLegendStr; }

    void setLegend_xText(string xLegendText) { legend_xText = xLegendText; }
    void setLegend_yText(string yLegendText) { legend_yText = yLegendText; }

    // Store the unit used when formatting the horizontal slider legend.
    void setLegendX_Unit(WidgetUnit unitX) {
        legendX_Unit = unitX;
        slider_xVal = handleCenter.x;
        legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);
    }

    // Store the unit used when formatting the vertical slider legend.
    void setLegendY_Unit(WidgetUnit unitY) {
        legendY_Unit = unitY;
        slider_yVal = handleCenter.y;
        legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);
    }

    // Discrete sliders use step counts and corresponding pixel range sizes to snap drag positions.
    void setIntervalCountX(int intervals) {
        intervalCountX = intervals;
        rangeSizeX = (slider_xMax - slider_xMin) / static_cast<double>(intervalCountX);
    }
    void setIntervalCountY(int intervals) {
        intervalCountY = intervals;
        rangeSizeY = (slider_yMax - slider_yMin) / static_cast<double>(intervalCountY);
    }

    void setRangeSizeX(double rangeSize) { rangeSizeX = rangeSize; }
    void setRangeSizeY(double rangeSize) { rangeSizeY = rangeSize; }

    // Buttons and text boxes share a label field; it is not automatically linked to WidgetAction.
    void setText(const string& t) noexcept { textboxText = t; }
    void setTextColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) noexcept { textboxColor = { r, g, b, a }; }
    string getText() const noexcept { return textboxText; }
    SDL_Color getTextColor() const noexcept { return textboxColor; }

    // Event code changes do not alter text or color; App.cpp updates all three to express the
    // visible state.
    void setButtonFunction(WidgetAction newEvent) { buttonFunction = newEvent; }

    // Rebuild a rectangular widget from the supplied half-width and half-height.
    // Rebuild both triangles and the hit box around a rectangular button/text widget.
    void setRectangleShape(uint16_t halfWidth, uint16_t halfHeight) noexcept {

        const double w = static_cast<double>(halfWidth);
        const double h = static_cast<double>(halfHeight);

        double cornerDist = hypot(w, h);

        auto clampToUint16 = [](double v) -> uint16_t {
            if (v <= 0.0) return 0;
            if (v >= static_cast<double>(numeric_limits<uint16_t>::max()))
                return numeric_limits<uint16_t>::max();
            return static_cast<uint16_t>(lround(v));
            };

        uint16_t diag = clampToUint16(cornerDist);

        this->halfDiag1 = diag;
        this->halfDiag2 = diag;

        double cornerAngle = 0.0;
        if (w != 0.0 || h != 0.0) cornerAngle = atan2(h, w);

        double angleDiag = SDL_PI_D - 2.0 * cornerAngle;

        angleDiag = fmod(angleDiag + SDL_PI_D, SDL_PI_D);

        this->diagonalAngle = angleDiag;
        this->figureAngleOffset = cornerAngle;
        this->diagonalAngleAbove90 = (this->diagonalAngle > (SDL_PI_D / 2.0));

        adaptZoomToLongestDiagonal();

        if (IsSliderType(this->type)) updateBackgroundRect();
        (void)this->updatePos();

        // For buttons/text boxes, the rectangular hit box follows the handle center.
        if (this->type == WidgetType::Button_StateMono || this->type == WidgetType::TextBox) {

            float rectWidth = 2.0f * halfWidth;
            float rectHeight = 2.0f * halfHeight;

            hitBoxMin.x = this->handleCenter.x - rectWidth / 2.0f;
            hitBoxMin.y = this->handleCenter.y - rectHeight / 2.0f;
            hitBoxMax.x = this->handleCenter.x + rectWidth / 2.0f;
            hitBoxMax.y = this->handleCenter.y + rectHeight / 2.0f;

            SDL_Log("Button '%s' - hit box recalculated: (%.0f, %.0f) -> (%.0f, %.0f)",
                this->name.c_str(),
                hitBoxMin.x, hitBoxMin.y,
                hitBoxMax.x, hitBoxMax.y);
        }
        else {

            legend_xVal = legend_xMin_Val + (slider_xVal - slider_xMin) * (legend_xMax_Val - legend_xMin_Val) / (slider_xMax - slider_xMin);
            legend_xText = FormatValueWithUnit(legend_xVal, legendX_Unit);

            legend_yVal = legend_yMax_Val - (slider_yVal - slider_yMin) * (legend_yMax_Val - legend_yMin_Val) / (slider_yMax - slider_yMin);
            legend_yText = FormatValueWithUnit(legend_yVal, legendY_Unit);
        }
    }

    // Recompute vertices and the hit box after movement or rotation. Some legacy branches lack an
    // explicit return.
    bool updatePos() {
        // Two triangles share the four logical corners of the figure. Both
        // their visible positions and their sorted hit-test pointers change.
        double pointAngle, pointDist;

        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 3; j++) {
                pointAngle = (j == 1) * diagonalAngle + int(0.5 * ((2 * i + j) % 4)) * SDL_PI_D;
                pointDist = (j != 1) * halfDiag1 + (j == 1) * halfDiag2;
                pointDist = pointDist * figureZoom;
                pointsTri[i][j].position.x = handleCenter.x + pointDist * cos(pointAngle + figureAngle + figureAngleOffset);
                pointsTri[i][j].position.y = handleCenter.y + pointDist * sin(pointAngle + figureAngle + figureAngleOffset);
            }
        }

        // Sort pointers, not vertex storage, so both SDL triangles keep their
        // original order while the bounding box tracks rotation and zoom.
        if (this->type != WidgetType::Static_Full) {
            SDL_Vertex* temporaryVertex;

            if ((this->verticesSortedByX[0]->position.x > this->verticesSortedByX[1]->position.x) or (this->verticesSortedByX[0]->position.x > this->verticesSortedByX[2]->position.x) or (this->verticesSortedByX[1]->position.x > this->verticesSortedByX[2]->position.x)) {
                for (int i = 0; i < 3; i++) {
                    for (int j = i + 1; j < 4; j++) {
                        if (this->verticesSortedByX[j]->position.x < this->verticesSortedByX[i]->position.x) {
                            temporaryVertex = verticesSortedByX[i];
                            verticesSortedByX[i] = verticesSortedByX[j];
                            verticesSortedByX[j] = temporaryVertex;
                        }
                    }
                }
            }

            if ((this->verticesSortedByY[0]->position.y > this->verticesSortedByY[1]->position.y) or (this->verticesSortedByY[0]->position.y > this->verticesSortedByY[2]->position.y) or (this->verticesSortedByY[1]->position.y > this->verticesSortedByY[2]->position.y)) {
                for (int i = 0; i < 3; i++) {
                    for (int j = i + 1; j < 4; j++) {
                        if (this->verticesSortedByY[j]->position.y < this->verticesSortedByY[i]->position.y) {
                            temporaryVertex = verticesSortedByY[i];
                            verticesSortedByY[i] = verticesSortedByY[j];
                            verticesSortedByY[j] = temporaryVertex;
                        }
                    }
                }
            }

            hitBoxMin.x = this->verticesSortedByX[0]->position.x;
            hitBoxMin.y = this->verticesSortedByY[0]->position.y;
            hitBoxMax.x = this->verticesSortedByX[3]->position.x;
            hitBoxMax.y = this->verticesSortedByY[3]->position.y;

            // The axis-aligned box is a coarse rejection test. The branches
            // below compare the mouse Y coordinate with edge slopes for the
            // rotated quadrilateral; they are inherited and not a general
            // robust point-in-polygon implementation.
            if (mx >= hitBoxMin.x && mx <= hitBoxMax.x && my >= hitBoxMin.y && my <= hitBoxMax.y) {

                if ((this->verticesSortedByX[0]->position.x == this->verticesSortedByX[1]->position.x and this->verticesSortedByX[0]->position.y == this->verticesSortedByX[2]->position.y) or (this->verticesSortedByX[0]->position.x == this->verticesSortedByX[2]->position.x and this->verticesSortedByX[0]->position.y == this->verticesSortedByX[1]->position.y)) {

                    if (this->figureZoom != this->zoomFactor) { this->figureZoom = this->zoomFactor; return true; }
                }
                // Equal X coordinates require a separate geometry branch;
                // the subsequent slopes can still have degenerate denominators.
                else if (this->verticesSortedByX[0]->position.x == this->verticesSortedByX[3]->position.x or this->verticesSortedByX[1]->position.x == this->verticesSortedByX[2]->position.x) {

                    int lowBoundY_at_mx, upBoundY_at_mx;
                    double lowSlope, upSlope;

                    if (mx >= this->verticesSortedByX[0]->position.x and mx <= this->verticesSortedByX[1]->position.x) {

                        if (this->verticesSortedByX[1]->position.y <= this->handleCenter.y) {
                            lowSlope = (this->verticesSortedByX[1]->position.y - this->verticesSortedByX[0]->position.y) / (this->verticesSortedByX[1]->position.x - this->verticesSortedByX[0]->position.x);
                            lowBoundY_at_mx = this->verticesSortedByX[0]->position.y + lowSlope * (mx - this->verticesSortedByX[0]->position.x);
                            upSlope = (this->verticesSortedByX[2]->position.y - this->verticesSortedByX[0]->position.y) / (this->verticesSortedByX[2]->position.x - this->verticesSortedByX[0]->position.x);
                            upBoundY_at_mx = this->verticesSortedByX[0]->position.y + upSlope * (mx - this->verticesSortedByX[0]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->figureZoom != this->zoomFactor) { this->figureZoom = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->figureZoom != 1.0) { this->figureZoom = 1.0; return false; }
                                return false;
                            }
                        }
                        else {
                            lowSlope = (this->verticesSortedByX[2]->position.y - this->verticesSortedByX[0]->position.y) / (this->verticesSortedByX[2]->position.x - this->verticesSortedByX[0]->position.x);
                            lowBoundY_at_mx = this->verticesSortedByX[0]->position.y + lowSlope * (mx - this->verticesSortedByX[0]->position.x);
                            upSlope = (this->verticesSortedByX[1]->position.y - this->verticesSortedByX[0]->position.y) / (this->verticesSortedByX[1]->position.x - this->verticesSortedByX[0]->position.x);
                            upBoundY_at_mx = this->verticesSortedByX[0]->position.y + upSlope * (mx - this->verticesSortedByX[0]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->figureZoom != this->zoomFactor) { this->figureZoom = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->figureZoom != 1.0) { this->figureZoom = 1.0; return false; }
                                return false;
                            }
                        }
                    }
                    else {
                        if (this->verticesSortedByX[1]->position.y <= this->handleCenter.y) {
                            lowSlope = (this->verticesSortedByX[3]->position.y - this->verticesSortedByX[1]->position.y) / (this->verticesSortedByX[3]->position.x - this->verticesSortedByX[1]->position.x);
                            lowBoundY_at_mx = this->verticesSortedByX[1]->position.y + lowSlope * (mx - this->verticesSortedByX[1]->position.x);
                            upSlope = (this->verticesSortedByX[3]->position.y - this->verticesSortedByX[2]->position.y) / (this->verticesSortedByX[3]->position.x - this->verticesSortedByX[2]->position.x);
                            upBoundY_at_mx = this->verticesSortedByX[2]->position.y + upSlope * (mx - this->verticesSortedByX[2]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->figureZoom != this->zoomFactor) { this->figureZoom = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->figureZoom != 1.0) { this->figureZoom = 1.0; return false; }
                                return false;
                            }
                        }
                        else {
                            lowSlope = (this->verticesSortedByX[3]->position.y - this->verticesSortedByX[2]->position.y) / (this->verticesSortedByX[3]->position.x - this->verticesSortedByX[2]->position.x);
                            lowBoundY_at_mx = this->verticesSortedByX[2]->position.y + lowSlope * (mx - this->verticesSortedByX[2]->position.x);
                            upSlope = (this->verticesSortedByX[3]->position.y - this->verticesSortedByX[1]->position.y) / (this->verticesSortedByX[3]->position.x - this->verticesSortedByX[1]->position.x);
                            upBoundY_at_mx = this->verticesSortedByX[1]->position.y + upSlope * (mx - this->verticesSortedByX[1]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->figureZoom != this->zoomFactor) { this->figureZoom = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->figureZoom != 1.0) { this->figureZoom = 1.0; return false; }
                                return false;
                            }
                        }
                    }
                }
                else {

                    int lowBoundY_at_mx, upBoundY_at_mx;
                    double lowSlope, upSlope;

                    if (mx >= this->verticesSortedByX[0]->position.x and mx <= this->verticesSortedByX[1]->position.x) {
                        if (this->verticesSortedByX[1]->position.y <= this->handleCenter.y) {
                            lowSlope = (this->verticesSortedByX[1]->position.y - this->verticesSortedByX[0]->position.y) / (this->verticesSortedByX[1]->position.x - this->verticesSortedByX[0]->position.x);
                            lowBoundY_at_mx = this->verticesSortedByX[0]->position.y + lowSlope * (mx - this->verticesSortedByX[0]->position.x);
                            upSlope = (this->verticesSortedByX[2]->position.y - this->verticesSortedByX[0]->position.y) / (this->verticesSortedByX[2]->position.x - this->verticesSortedByX[0]->position.x);
                            upBoundY_at_mx = this->verticesSortedByX[0]->position.y + upSlope * (mx - this->verticesSortedByX[0]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->figureZoom != this->zoomFactor) { this->figureZoom = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->figureZoom != 1.0) { this->figureZoom = 1.0; return false; }
                                return false;
                            }
                        }
                        else {
                            lowSlope = (this->verticesSortedByX[2]->position.y - this->verticesSortedByX[0]->position.y) / (this->verticesSortedByX[2]->position.x - this->verticesSortedByX[0]->position.x);
                            lowBoundY_at_mx = this->verticesSortedByX[0]->position.y + lowSlope * (mx - this->verticesSortedByX[0]->position.x);
                            upSlope = (this->verticesSortedByX[1]->position.y - this->verticesSortedByX[0]->position.y) / (this->verticesSortedByX[1]->position.x - this->verticesSortedByX[0]->position.x);
                            upBoundY_at_mx = this->verticesSortedByX[0]->position.y + upSlope * (mx - this->verticesSortedByX[0]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->figureZoom != this->zoomFactor) { this->figureZoom = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->figureZoom != 1.0) { this->figureZoom = 1.0; return false; }
                                return false;
                            }
                        }
                    }
                    else if (mx >= this->verticesSortedByX[1]->position.x and mx <= this->verticesSortedByX[2]->position.x) {
                        if (this->verticesSortedByX[1]->position.y <= this->handleCenter.y) {
                            lowSlope = (this->verticesSortedByX[3]->position.y - this->verticesSortedByX[1]->position.y) / (this->verticesSortedByX[3]->position.x - this->verticesSortedByX[1]->position.x);
                            lowBoundY_at_mx = this->verticesSortedByX[1]->position.y + lowSlope * (mx - this->verticesSortedByX[1]->position.x);
                            upSlope = (this->verticesSortedByX[2]->position.y - this->verticesSortedByX[0]->position.y) / (this->verticesSortedByX[2]->position.x - this->verticesSortedByX[0]->position.x);
                            upBoundY_at_mx = this->verticesSortedByX[0]->position.y + upSlope * (mx - this->verticesSortedByX[0]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->figureZoom != this->zoomFactor) { this->figureZoom = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->figureZoom != 1.0) { this->figureZoom = 1.0; return false; }
                                return false;
                            }
                        }
                        else {
                            lowSlope = (this->verticesSortedByX[2]->position.y - this->verticesSortedByX[0]->position.y) / (this->verticesSortedByX[2]->position.x - this->verticesSortedByX[0]->position.x);
                            lowBoundY_at_mx = this->verticesSortedByX[0]->position.y + lowSlope * (mx - this->verticesSortedByX[0]->position.x);
                            upSlope = (this->verticesSortedByX[3]->position.y - this->verticesSortedByX[1]->position.y) / (this->verticesSortedByX[3]->position.x - this->verticesSortedByX[1]->position.x);
                            upBoundY_at_mx = this->verticesSortedByX[1]->position.y + upSlope * (mx - this->verticesSortedByX[1]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->figureZoom != this->zoomFactor) { this->figureZoom = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->figureZoom != 1.0) { this->figureZoom = 1.0; return false; }
                                return false;
                            }
                        }
                    }
                    else {
                        if (this->verticesSortedByX[1]->position.y <= this->handleCenter.y) {
                            lowSlope = (this->verticesSortedByX[3]->position.y - this->verticesSortedByX[1]->position.y) / (this->verticesSortedByX[3]->position.x - this->verticesSortedByX[1]->position.x);
                            lowBoundY_at_mx = this->verticesSortedByX[1]->position.y + lowSlope * (mx - this->verticesSortedByX[1]->position.x);
                            upSlope = (this->verticesSortedByX[3]->position.y - this->verticesSortedByX[2]->position.y) / (this->verticesSortedByX[3]->position.x - this->verticesSortedByX[2]->position.x);
                            upBoundY_at_mx = this->verticesSortedByX[2]->position.y + upSlope * (mx - this->verticesSortedByX[2]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->figureZoom != this->zoomFactor) { this->figureZoom = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->figureZoom != 1.0) { this->figureZoom = 1.0; return false; }
                                return false;
                            }
                        }
                        else {
                            lowSlope = (this->verticesSortedByX[3]->position.y - this->verticesSortedByX[2]->position.y) / (this->verticesSortedByX[3]->position.x - this->verticesSortedByX[2]->position.x);
                            lowBoundY_at_mx = this->verticesSortedByX[2]->position.y + lowSlope * (mx - this->verticesSortedByX[2]->position.x);
                            upSlope = (this->verticesSortedByX[3]->position.y - this->verticesSortedByX[1]->position.y) / (this->verticesSortedByX[3]->position.x - this->verticesSortedByX[1]->position.x);
                            upBoundY_at_mx = this->verticesSortedByX[1]->position.y + upSlope * (mx - this->verticesSortedByX[1]->position.x);
                            if (my >= lowBoundY_at_mx and my <= upBoundY_at_mx) {
                                if (this->figureZoom != this->zoomFactor) { this->figureZoom = this->zoomFactor; return true; }
                            }
                            else {
                                if (this->figureZoom != 1.0) { this->figureZoom = 1.0; return false; }
                                return false;
                            }
                        }
                    }
                }
            }
            else {

                if (this->figureZoom != 1.0) { this->figureZoom = 1.0; return false; }
                return false;
            }
        }
        else {
            return false;
        }

    }

    // Render the current widget type using the shared SDL renderer and font.
    void renderFig() {

        // Draw slider track, handle, and endpoint labels before the quad itself.
        if (hasBackground && renderer) {

            SDL_SetRenderDrawColorFloat(renderer, bg_r, bg_g, bg_b, bg_a);
            SDL_RenderFillRect(renderer, &bgRect);

            SDL_SetRenderDrawColorFloat(renderer, 0.6f, 0.6f, 0.6f, 1.0f);
            SDL_RenderRect(renderer, &bgRect);

            if (hasBgRect2) {
                SDL_SetRenderDrawColorFloat(renderer, slider_r, slider_g, slider_b, slider_a);
                SDL_RenderFillRect(renderer, &bgRect2);

                SDL_SetRenderDrawColorFloat(renderer, 0.85f, 0.85f, 0.85f, 0.6f);
                SDL_RenderRect(renderer, &bgRect2);
            }

            if (IsSliderType(type) && gFont) {
                SDL_Color textColor = { 0, 0, 0, 255 };
                float tw = 0.0f, th = 0.0f;
                if (type == WidgetType::SliderH_PosN || type == WidgetType::SliderH_FreeLimited) {
                    getSurfaceSize(getLegend_xMin_Text(), tw, th);
                    RenderText(getLegend_xMin_Text(), bgRect.x - tw / 2.0f, bgRect.y + bgRect.h + 8.0f, textColor, tw, th);
                    getSurfaceSize(getLegend_xMax_Text(), tw, th);
                    RenderText(getLegend_xMax_Text(), bgRect.x + bgRect.w - tw / 2.0f, bgRect.y + bgRect.h + 8.0f, textColor, tw, th);
                }
                else if (type == WidgetType::SliderV_PosN || type == WidgetType::SliderV_FreeLimited) {
                    getSurfaceSize(getLegend_yMax_Text(), tw, th);
                    RenderText(getLegend_yMax_Text(), center.x - tw / 2.0f, bgRect.y - th - 8.0f, textColor, tw, th);
                    getSurfaceSize(getLegend_yMin_Text(), tw, th);
                    RenderText(getLegend_yMin_Text(), center.x - tw / 2.0f, bgRect.y + bgRect.h + 8.0f, textColor, tw, th);
                }
                else {
                    getSurfaceSize(getLegend_xMin_Text(), tw, th);
                    RenderText(getLegend_xMin_Text(), bgRect.x - tw / 2.0f, bgRect.y + bgRect.h + 8.0f, textColor, tw, th);
                    getSurfaceSize(getLegend_xMax_Text(), tw, th);
                    RenderText(getLegend_xMax_Text(), bgRect.x + bgRect.w - tw / 2.0f, bgRect.y + bgRect.h + 8.0f, textColor, tw, th);

                    getSurfaceSize(getLegend_yMax_Text(), tw, th);
                    RenderText(getLegend_yMax_Text(), center.x - tw / 2.0f, bgRect.y - th - 8.0f, textColor, tw, th);
                    getSurfaceSize(getLegend_yMin_Text(), tw, th);
                    RenderText(getLegend_yMin_Text(), center.x - tw / 2.0f, bgRect.y + bgRect.h + 8.0f, textColor, tw, th);
                }
            }
        }

        for (int i = 0; i < 2; i++) {
            // Two SDL triangles render the colored quadrilateral.
            SDL_RenderGeometry(renderer, nullptr, pointsTri[i], verticesPerTriangle, nullptr, 0);
        }

        // Render centered button/box text only when both a font and label are available.
        if ((type == WidgetType::TextBox || type == WidgetType::Button_StateMono) && gFont && !textboxText.empty()) {
            float centerX = (hitBoxMin.x + hitBoxMax.x) * 0.5f;
            float centerY = (hitBoxMin.y + hitBoxMax.y) * 0.5f;

            double scale = static_cast<double>(this->figureZoom);
            float tw = 0.0f, th = 0.0f;
            getSurfaceSize(textboxText, tw, th, scale);

            if (tw <= 0.0f || th <= 0.0f) {
                RenderText(textboxText, centerX, centerY, textboxColor, tw, th, scale);
            }
            float px = centerX - tw / 2.0f;
            float py = centerY - th / 2.0f;
            RenderText(textboxText, px, py, textboxColor, tw, th, scale);
        }
    }

    // Console-only diagnostic output for a widget ID and name.
    void display() const {
        cout << "MyObject[ID=" << id << ", Name=" << name << "]\n";
    }
};

// Named widget registry. Lookup by name or ID throws if an entry is absent; keep App.cpp names in
// sync with its users.
class UiWidgetCollection {
    // A std::list keeps addresses stable under append; remove() still copies a widget containing
    // internal vertex pointers.
    list<UiWidget> items;
public:
    using iterator = list<UiWidget>::iterator;
    using const_iterator = list<UiWidget>::const_iterator;

    // Append a named control with the next ID; lookup later uses this exact name.
    void add(const string& name) {
        items.emplace_back(items.size() + 1, name);
    }

    // Erase a named widget; the legacy copy of pointer-owning geometry needs review before relying on
    // removal.
    void remove(const string& name) {
        if (items.empty()) {
            cout << "\nNo item to remove.\n";
            return;
        }
        auto it = items.begin();
        while (it != items.end()) {
            if (it->getName() == name) {
                auto lastIt = prev(items.end());
                if (it != lastIt) {
                    int oldId = it->getId();
                    *it = *lastIt;
                    it->setId(oldId);
                }
                items.erase(lastIt);
                return;
            }
            ++it;
        }
        cout << "\nWidget '" << name << "' was not found.\n";
    }

    // Return the named control or throw out_of_range when it is missing.
    UiWidget& operator()(const string& name) {
        for (auto& item : items) {
            if (item.getName() == name) return item;
        }
        throw out_of_range("No item found with name \"" + name + "\"");
    }

    // Return the control with the given ID or throw out_of_range.
    UiWidget& operator()(int id) {
        for (auto& item : items) {
            if (item.getId() == id) return item;
        }
        throw out_of_range("No item found with ID " + to_string(id));
    }

    size_t size() const noexcept {
        return items.size();
    }

    // Iterators let AppRender traverse widgets without exposing the collection container directly.
    iterator begin() noexcept { return items.begin(); }
    iterator end() noexcept { return items.end(); }
    const_iterator begin() const noexcept { return items.begin(); }
    const_iterator end() const noexcept { return items.end(); }
    const_iterator cbegin() const noexcept { return items.cbegin(); }
    const_iterator cend() const noexcept { return items.cend(); }

    void displayAll() const {
        if (items.empty()) {
            cout << "No items in the collection.\n";
            return;
        }
        for (const auto& item : items) item.display();
    }
};
