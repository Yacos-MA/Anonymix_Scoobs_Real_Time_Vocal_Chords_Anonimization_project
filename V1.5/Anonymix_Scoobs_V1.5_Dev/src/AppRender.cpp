// Per-frame SDL rendering, pointer interaction and slider movement. Called on the SDL event thread.

#include "Widgets.h"
#include "Visualization.h"
#include "AppFrame.h"

struct SliderCaptionTexture {
    const char* name;
    float centerX;
    SDL_Texture* texture = nullptr;
    float width = 0.0f;
    float height = 0.0f;
};

static array<SliderCaptionTexture, 6> sliderCaptions{{
    {"Pitch", 715.0f}, {"Formant", 843.0f}, {"Timbre", 971.0f},
    {"Prosody", 1099.0f}, {"Jitter", 1227.0f}, {"Volume", 1355.0f}
}};
static bool sliderCaptionsPrepared = false;
// The 2x interface target is resized only when the window changes size.
// Mouse and widget coordinates remain in the original window coordinate system.
static SDL_Texture* interfaceTarget = nullptr;
static int interfaceWidth = 0;
static int interfaceHeight = 0;
static bool interfaceTargetUnavailable = false;

// Descriptions are intentionally short enough to fit the existing information
// panel. Both the effect button and the corresponding slider name use the same
// description. Textures are created once, not 60 times per second.
struct EffectHelp {
    const char* buttonName;
    array<const char*, 2> lines;
    array<SDL_Texture*, 2> textures{};
    array<float, 2> widths{};
    array<float, 2> heights{};
};

static array<EffectHelp, 5> effectHelp{{
    {"EffectPitch", {"Pitch changes how high or low your voice sounds.",
                     "Zero semitones leaves its pitch unchanged."}},
    {"EffectFormant", {"Formant changes the apparent vowel and voice colour.",
                       "Move its ratio to change that character."}},
    {"EffectTimbre", {"Timbre changes how bright or dark your voice sounds.",
                      "Positive is brighter; negative is darker."}},
    {"EffectProsody", {"Prosody adds a rising or falling gesture at voice onsets.",
                       "It does not recognize words or their meaning."}},
    {"EffectJitter", {"Jitter adds small, irregular changes to voice pitch.",
                      "Zero is neutral; try 0.020 on a held vowel."}}
}};

static void PrepareEffectHelp(EffectHelp& help) {
    if (!gFont) return;
    constexpr float scale = 0.68f;
    const SDL_Color darkText{ 14, 22, 28, 255 };
    for (size_t i = 0; i < help.lines.size(); ++i) {
        if (help.textures[i]) continue;
        SDL_Surface* surface = TTF_RenderText_Blended(gFont, help.lines[i],
            strlen(help.lines[i]), darkText);
        if (!surface) continue;
        help.widths[i] = surface->w * scale * 0.5f;
        help.heights[i] = surface->h * scale * 0.5f;
        help.textures[i] = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_DestroySurface(surface);
    }
}

void CleanupStaticSliderCaptions() {
    if (interfaceTarget != nullptr) {
        SDL_DestroyTexture(interfaceTarget);
        interfaceTarget = nullptr;
    }
    for (auto& caption : sliderCaptions) {
        if (caption.texture != nullptr) SDL_DestroyTexture(caption.texture);
        caption.texture = nullptr;
    }
    for (auto& help : effectHelp) {
        for (auto& tooltipTexture : help.textures) {
            if (tooltipTexture) SDL_DestroyTexture(tooltipTexture);
            tooltipTexture = nullptr;
        }
    }
    sliderCaptionsPrepared = false;
}

// Move and draw the interactive controls, waveform and spectrum, then present the frame.
void RenderFrame(double now)
{
    // Animation phase uses elapsed seconds, not frame count. Faster movement does not
    // request more than the 60 visual frames per second scheduled by SDL_AppIterate.
    const double squarePhase = now * 9.0;

    // Render every control, its text and both visualizers at 2x. The final
    // linear reduction softens sloped pixels while retaining original hit boxes.
    int windowWidth = 0;
    int windowHeight = 0;
    const bool validWindowSize = SDL_GetWindowSize(window, &windowWidth, &windowHeight) &&
        windowWidth > 0 && windowHeight > 0;
    if (interfaceTarget != nullptr && validWindowSize &&
        (windowWidth != interfaceWidth || windowHeight != interfaceHeight)) {
        SDL_DestroyTexture(interfaceTarget);
        interfaceTarget = nullptr;
    }
    if (!interfaceTargetUnavailable && interfaceTarget == nullptr && validWindowSize) {
        interfaceTarget = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
            SDL_TEXTUREACCESS_TARGET, windowWidth * 2, windowHeight * 2);
        if (interfaceTarget != nullptr) {
            interfaceWidth = windowWidth;
            interfaceHeight = windowHeight;
            SDL_SetTextureScaleMode(interfaceTarget, SDL_SCALEMODE_LINEAR);
        } else {
            interfaceTargetUnavailable = true;
            SDL_Log("2x interface target unavailable: %s", SDL_GetError());
        }
    }
    bool interfaceSupersampled = false;
    if (interfaceTarget != nullptr) {
        interfaceSupersampled = SDL_SetRenderTarget(renderer, interfaceTarget);
        if (!interfaceSupersampled) {
            SDL_Log("2x interface target could not be selected: %s", SDL_GetError());
            SDL_SetRenderTarget(renderer, nullptr);
            SDL_DestroyTexture(interfaceTarget);
            interfaceTarget = nullptr;
            interfaceTargetUnavailable = true;
        }
    }
    if (interfaceSupersampled && !SDL_SetRenderScale(renderer, 2.0f, 2.0f)) {
        SDL_SetRenderTarget(renderer, nullptr);
        SDL_DestroyTexture(interfaceTarget);
        interfaceTarget = nullptr;
        interfaceTargetUnavailable = true;
        interfaceSupersampled = false;
        SDL_Log("2x interface scale unavailable: %s", SDL_GetError());
    }
    // SDL_RenderPresent at the end publishes the complete image.
    SDL_RenderClear(renderer);

    // These coordinates drive drag and hover tests and are also read in SDL_AppEvent.
    SDL_GetMouseState(&mx, &my);

    // Legacy toggle arithmetic controls the decorative animation. Repeated weights are inherited,
    // not an audio data representation.
    sevenBitsValue = int((toggle1 == true)) + 4 * int((toggle2 == true)) + 32 * int((toggle3 == true)) + 32 * int((toggle4 == true)) + 16 * int((toggle5 == true)) + 32 * int((toggle6 == true)) + 64 * int((toggle7 == true));

    tenBitsValue = int((toggle1 == true)) + 4 * int((toggle2 == true)) + 8 * int((toggle3 == true)) + 8 * int((toggle4 == true)) + 16 * int((toggle5 == true)) + 32 * int((toggle6 == true)) + 64 * int((toggle7 == true)) + 128 * int((toggle8 == true)) + 256 * int((toggle9 == true)) + 512 * int((toggle10 == true));

    toggle1 = not toggle1;

    toggle2 = not ((toggle2 == false) xor (toggle1 == false));

    toggle3 = not ((toggle3 == false) xor (((toggle2 == false) and (toggle1 == false))));

    toggle4 = not ((toggle4 == false) xor (((toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    toggle5 = not ((toggle5 == false) xor (((toggle4 == false) and (toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    toggle6 = not ((toggle6 == false) xor (((toggle5 == false) and (toggle4 == false) and (toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    toggle7 = not ((toggle7 == false) xor (((toggle6 == false) and (toggle5 == false) and (toggle4 == false) and (toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    toggle8 = not ((toggle8 == false) xor (((toggle7 == false) and (toggle6 == false) and (toggle5 == false) and (toggle4 == false) and (toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    toggle9 = not ((toggle9 == false) xor (((toggle8 == false) and (toggle7 == false) and (toggle6 == false) and (toggle5 == false) and (toggle4 == false) and (toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    toggle10 = not ((toggle10 == false) xor (((toggle9 == false) and (toggle8 == false) and (toggle7 == false) and (toggle6 == false) and (toggle5 == false) and (toggle4 == false) and (toggle3 == false) and (toggle2 == false) and (toggle1 == false))));

    // Phase-shifted sine values generate continuously changing RGB backgrounds from the frame time
    // in seconds.
    red = (float)(0.5 + 0.5 * SDL_sin(now));
    green = (float)(0.5 + 0.5 * SDL_sin(now + SDL_PI_D * 2 / 3));
    blue = (float)(0.5 + 0.5 * SDL_sin(now + SDL_PI_D * 4 / 3));
    SDL_SetRenderDrawColorFloat(renderer, red, green, blue, SDL_ALPHA_OPAQUE_FLOAT);

    SDL_RenderFillRect(renderer, &rect1);

    red = (float)(0.5 + 0.5 * SDL_sin(now + SDL_PI_D));
    green = (float)(0.5 + 0.5 * SDL_sin(now + SDL_PI_D + SDL_PI_D * 2 / 3));
    blue = (float)(0.5 + 0.5 * SDL_sin(now + SDL_PI_D + SDL_PI_D * 4 / 3));
    SDL_SetRenderDrawColorFloat(renderer, red, green, blue, SDL_ALPHA_OPAQUE_FLOAT);

    SDL_RenderFillRect(renderer, &rect2);

    // The decorative control and two orbiting rectangles are animated independently of recording
    // and playback.
    widgets("AnimatedWidget").setCenterX(280 + 90 * sin((SDL_PI_D * tenBitsValue) / 512));

    // A continuous shade avoids the rare white flash caused by converting
    // red == 1.0 to an integer before reducing its brightness.
    const float orbitShade = clamp(red * 0.5f, 0.0f, 0.5f);
    SDL_SetRenderDrawColorFloat(renderer, orbitShade, orbitShade, orbitShade,
        SDL_ALPHA_OPAQUE_FLOAT);

    // Restore the moving center and frame-dependent phase perturbation that produce the
    // original glitch animation. Wall-clock phase adds speed without changing UI FPS.
    const double glitchPhase = squarePhase + SDL_PI_D / 128 * sevenBitsValue;
    const int orbitX = static_cast<int>(widgets("AnimatedWidget").getCenterX());
    rect3.x = orbitX - int(rect3.w / 2) + int(160 * SDL_sin(glitchPhase));
    rect3.y = 240 - int(rect3.h / 2) + int(160 * SDL_sin(glitchPhase + SDL_PI_D / 2));

    SDL_RenderFillRect(renderer, &rect3);

    rect4.x = orbitX - int(rect4.w / 2) + int(160 * SDL_sin(glitchPhase + SDL_PI_D));
    rect4.y = 240 - int(rect4.h / 2) + int(160 * SDL_sin(glitchPhase + SDL_PI_D * 1.5));

    SDL_RenderFillRect(renderer, &rect4);

    // Rotate the center several full turns each second while preserving its
    // original bit-driven angle jumps and horizontal motion. Frame pacing stays
    // at 60 FPS; only the angular speed changes.
    widgets("AnimatedWidget").setFigureAngle(fmod(
        now * 24.0 - SDL_PI_D / 128 * sevenBitsValue, 2 * SDL_PI_D));

	bool test = false;

    // Rebuild hover state from current hit testing each frame. SDL_AppEvent uses the most recently
    // computed pointer.
    hoveredWidget = nullptr;

    for (auto& widget : widgets) {

        // Only the currently pressed widget follows the pointer. Its original click offset avoids
        // a jump at drag start.
        if (pressedWidget != nullptr && &widget == pressedWidget) {
            if (pressedWidget->get_isMouseLeftButtonDown()) {

                // Clamp horizontal drag to the track, then snap PosN variants to their configured
                // steps.
                if (mx - mouseClick_OFFSET.x < pressedWidget->getSlider_xMin()) {
                    pressedWidget->setHandleCenterX(pressedWidget->getSlider_xMin());
                }
                else if (mx - mouseClick_OFFSET.x > pressedWidget->getSlider_xMax()) {
                    pressedWidget->setHandleCenterX(pressedWidget->getSlider_xMax());
                }
                else {
                    if (pressedWidget->getType() != WidgetType::SliderH_PosN && pressedWidget->getType() != WidgetType::SliderH_FreeLimited && pressedWidget->getType() != WidgetType::Slider2D_FreeLimited && pressedWidget->getType() != WidgetType::Slider2D_PosN) {
                        pressedWidget->setHandleCenterX(pressedWidget->getSlider_xMin());
                    }
                    else {
                        if (pressedWidget->getType() != WidgetType::SliderH_FreeLimited && pressedWidget->getType() != WidgetType::Slider2D_FreeLimited) {
                            pressedWidget->setHandleCenterX(int(pressedWidget->getSlider_xMin() + int(((mx - mouseClick_OFFSET.x - pressedWidget->getSlider_xMin()) / pressedWidget->getRangeSizeX()) + 0.5f) * pressedWidget->getRangeSizeX()));
                        }
                        else {
                            pressedWidget->setHandleCenterX(mx - mouseClick_OFFSET.x);
                        }
                    }
                }

                // Apply the corresponding vertical clamp; screen Y increases downward.
                if (my - mouseClick_OFFSET.y < pressedWidget->getSlider_yMin()) {
                    pressedWidget->setHandleCenterY(pressedWidget->getSlider_yMin());
                }
                else if (my - mouseClick_OFFSET.y > pressedWidget->getSlider_yMax()) {
                    pressedWidget->setHandleCenterY(pressedWidget->getSlider_yMax());
                }
                else {
                    if (pressedWidget->getType() != WidgetType::SliderV_PosN && pressedWidget->getType() != WidgetType::SliderV_FreeLimited && pressedWidget->getType() != WidgetType::Slider2D_FreeLimited && pressedWidget->getType() != WidgetType::Slider2D_PosN) {
                        pressedWidget->setHandleCenterY(pressedWidget->getSlider_yMin());
                    }
                    else {
                        if (pressedWidget->getType() != WidgetType::SliderV_FreeLimited && pressedWidget->getType() != WidgetType::Slider2D_FreeLimited) {
                            pressedWidget->setHandleCenterY(pressedWidget->getSlider_yMin() + int(((my - mouseClick_OFFSET.y - pressedWidget->getSlider_yMin()) / pressedWidget->getRangeSizeY()) + 0.5f) * pressedWidget->getRangeSizeY());
                        }
                        else {
                            pressedWidget->setHandleCenterY(my - mouseClick_OFFSET.y);
                        }
                    }
                }

                // Translate handle pixels into displayed legend values after movement; AppUpdate
                // publishes those values.
                pressedWidget->setSlider_xVal(pressedWidget->getHandleCenterX());
                pressedWidget->setSlider_yVal(pressedWidget->getHandleCenterY());

                if (pressedWidget->getType() == WidgetType::SliderH_PosN || pressedWidget->getType() == WidgetType::SliderH_FreeLimited || pressedWidget->getType() == WidgetType::Slider2D_FreeLimited || pressedWidget->getType() == WidgetType::Slider2D_PosN) {
                    pressedWidget->setLegend_xText(FormatValueWithUnit(pressedWidget->getLegend_xVal(), pressedWidget->getLegendX_Unit()));
                }
                else{
                    pressedWidget->setLegend_xText("");
                }

                if (pressedWidget->getType() == WidgetType::SliderV_PosN || pressedWidget->getType() == WidgetType::SliderV_FreeLimited || pressedWidget->getType() == WidgetType::Slider2D_FreeLimited || pressedWidget->getType() == WidgetType::Slider2D_PosN) {
                    pressedWidget->setLegend_yText(FormatValueWithUnit(pressedWidget->getLegend_yVal(), pressedWidget->getLegendY_Unit()));
                }
                else {
					pressedWidget->setLegend_yText("");
                }

                // Show the active control values, separating two axes only for a two-dimensional
                // slider.
                widgets("WidgetInfo").setText(pressedWidget->getLegend_xText() + (pressedWidget->getType() == WidgetType::Slider2D_FreeLimited || pressedWidget->getType() == WidgetType::Slider2D_PosN ? " ; " : "") + pressedWidget->getLegend_yText());
            }
            else {
                if (hoveredWidget == nullptr) {
                    widgets("WidgetInfo").setText("");
                }
                else {
                    widgets("WidgetInfo").setText(hoveredWidget->getLegend_xText() + (hoveredWidget->getType() == WidgetType::Slider2D_FreeLimited || hoveredWidget->getType() == WidgetType::Slider2D_PosN ? " ; " : "") + hoveredWidget->getLegend_yText());
                }
            }
        }

        // updatePos also performs geometry/hit tests. Its return contract is incomplete in some
        // inherited branches.
        if (widget.updatePos()) {

            bool shouldIgnoreButton = false;

            try {

                string audioModeText = widgets("AudioMode").getText();

                // When streaming, the UI suppresses ordinary button hover/action paths. Names and
                // displayed mode text are compared exactly.
                if (IsAudioDeviceReopening() &&
                    widget.getType() == WidgetType::Button_StateMono) {
                    shouldIgnoreButton = true;
                }
                else if (audioModeText == "Active mode: Stream" &&
                    widget.getType() == WidgetType::Button_StateMono) {

                    string widgetName = widget.getName();

                    if (widgetName != "AudioMode" &&
                        widgetName != "AudioDevices" &&
                        widgetName != "EffectPitch" &&
                        widgetName != "EffectFormant" &&
                        widgetName != "EffectTimbre" &&
                        widgetName != "EffectProsody" &&
                        widgetName != "EffectJitter" &&
                        widgetName != "Audio mode" &&
                        widgetName != "Active mode: Stream" &&
                        widgetName != "Active mode: Recording") {

                        shouldIgnoreButton = true;
                        SDL_Log("Button '%s' ignored (stream mode active)", widgetName.c_str());
                    }
                }
            }
            catch (...) {
                SDL_Log("Error while checking audio mode");
            }

            if (!shouldIgnoreButton) {
                hoveredWidget = &widget;
                test = true;

                if (hoveredWidget->getType() == WidgetType::SliderH_PosN || hoveredWidget->getType() == WidgetType::SliderH_FreeLimited) {
                    widgets("WidgetInfo").setText(hoveredWidget->getLegend_xText());
                }
                else if (hoveredWidget->getType() == WidgetType::SliderV_PosN || hoveredWidget->getType() == WidgetType::SliderV_FreeLimited) {
                    widgets("WidgetInfo").setText(hoveredWidget->getLegend_yText());
                }
                else if (hoveredWidget->getType() == WidgetType::Slider2D_FreeLimited || hoveredWidget->getType() == WidgetType::Slider2D_PosN) {
                    widgets("WidgetInfo").setText(hoveredWidget->getLegend_xText() + " ; " + hoveredWidget->getLegend_yText());
                }
                else {
                    hoveredWidget->setLegend_yText("");
                }
            }
            else {
                if (widget.getFigureZoom() != 1.0f) {
                    widget.setFigureZoom(1.0f);
                }
            }

        }
        widget.renderFig();
    }

    // Clear the hover pointer and info text when no eligible control matched the mouse in this
    // frame.
    if (not test) {
		hoveredWidget = nullptr;
        widgets("WidgetInfo").setText("");
    }

    // Short scale legends make room for six sliders. Keep their names separately above
    // the tracks so the controls remain identifiable even when an effect is disabled.
    if (gFont) {
        if (!sliderCaptionsPrepared) {
            const SDL_Color captionColor{ 14, 22, 28, 255 };
            for (auto& caption : sliderCaptions) {
                SDL_Surface* textSurface = TTF_RenderText_Blended(gFont, caption.name,
                    strlen(caption.name), captionColor);
                if (textSurface == nullptr) continue;
                // Use nearly the font's original size to match the other UI
                // labels while retaining space between the six slider names.
                caption.width = textSurface->w * 0.95f * 0.5f;
                caption.height = textSurface->h * 0.95f * 0.5f;
                caption.texture = SDL_CreateTextureFromSurface(renderer, textSurface);
                SDL_DestroySurface(textSurface);
            }
            sliderCaptionsPrepared = true;
        }
        for (const auto& caption : sliderCaptions) {
            if (caption.texture == nullptr) continue;
            const SDL_FRect destination{ caption.centerX - caption.width * 0.5f,
                440.0f, caption.width, caption.height };
            SDL_RenderTexture(renderer, caption.texture, nullptr, &destination);
        }
    }

    // Only effect buttons and their printed slider names provide help. A new
    // target restarts the one-second timer; merely redrawing at 60 FPS does not.
    int helpTarget = -1;
    if (hoveredWidget != nullptr) {
        const string hoveredName = hoveredWidget->getName();
        for (size_t i = 0; i < effectHelp.size(); ++i) {
            if (hoveredName == effectHelp[i].buttonName) {
                helpTarget = static_cast<int>(i);
                break;
            }
        }
    }
    if (helpTarget < 0) {
        for (size_t i = 0; i < effectHelp.size(); ++i) {
            const auto& caption = sliderCaptions[i];
            if (caption.texture &&
                mx >= caption.centerX - caption.width * 0.5f &&
                mx <= caption.centerX + caption.width * 0.5f &&
                my >= 440.0f && my <= 440.0f + caption.height) {
                helpTarget = static_cast<int>(i);
                break;
            }
        }
    }

    static int previousHelpTarget = -1;
    static Uint64 hoverStartedAt = 0;
    const Uint64 currentTicks = SDL_GetTicks();
    if (helpTarget != previousHelpTarget) {
        previousHelpTarget = helpTarget;
        hoverStartedAt = currentTicks;
    }
    if (helpTarget >= 0 && currentTicks - hoverStartedAt >= 1000 && gFont) {
        auto& help = effectHelp[static_cast<size_t>(helpTarget)];
        PrepareEffectHelp(help);

        // Paint over the transient numeric value inside the lavender panel.
        // This stays in the visual thread and never affects the audio callback.
        const SDL_FRect infoArea{ 660.0f, 130.0f, 750.0f, 110.0f };
        SDL_SetRenderDrawColorFloat(renderer, 0.55f, 0.55f, 1.0f, 1.0f);
        SDL_RenderFillRect(renderer, &infoArea);
        for (size_t i = 0; i < help.textures.size(); ++i) {
            if (!help.textures[i]) continue;
            const SDL_FRect destination{ infoArea.x + 15.0f,
                infoArea.y + 20.0f + static_cast<float>(i) * 35.0f,
                help.widths[i], help.heights[i] };
            SDL_RenderTexture(renderer, help.textures[i], nullptr, &destination);
        }
    }

    // Draw the FFT and waveform over the same vertical band as the slider tracks. The spectrum
    // starts at their top edge, and the waveform ends at their bottom edge.
    RenderFFTSpectrum_PortAudio(40.0f, 510.0f, 560.0f, 210.0f);
    RenderWaveform(40.0f, 760.0f, 560.0f, 210.0f);

    if (interfaceSupersampled) {
        SDL_SetRenderTarget(renderer, nullptr);
        const SDL_FRect destination{ 0.0f, 0.0f,
            static_cast<float>(interfaceWidth), static_cast<float>(interfaceHeight) };
        SDL_RenderTexture(renderer, interfaceTarget, nullptr, &destination);
    }
    SDL_RenderPresent(renderer);

}
