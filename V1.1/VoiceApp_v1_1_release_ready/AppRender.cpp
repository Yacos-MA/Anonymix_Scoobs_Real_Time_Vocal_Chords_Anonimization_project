// Per-frame SDL rendering, pointer interaction and slider movement. Called on the SDL event thread.

#include "Widgets.h"
#include "Visualization.h"
#include "AppFrame.h"

// Move and draw the interactive controls, waveform and spectrum, then present the frame.
void RenderFrame(double now)
{

    // Start with a clean frame; SDL_RenderPresent at the end publishes all widget and audio
    // visualizations.
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

    SDL_SetRenderDrawColorFloat(renderer, (red - (red - (int(red) % 2))/2), (red - (red - (int(red) % 2)) / 2), (red - (red - (int(red) % 2)) / 2), SDL_ALPHA_OPAQUE_FLOAT);

    rect3.x = widgets("AnimatedWidget").getCenterX() - int(rect3.w/2) + int(160 * SDL_sin(now + SDL_PI_D/128 * sevenBitsValue));
    rect3.y = 240 - int(rect3.w / 2) + int(160 * SDL_sin(now + SDL_PI_D/128 * sevenBitsValue + SDL_PI_D/2));

    SDL_RenderFillRect(renderer, &rect3);

    rect4.x = widgets("AnimatedWidget").getCenterX() - int(rect4.w / 2) + int(160 * SDL_sin(now + SDL_PI_D + SDL_PI_D / 128 * sevenBitsValue));
    rect4.y = 240 - int(rect4.w / 2) + int(160 * SDL_sin(now + SDL_PI_D + SDL_PI_D / 128 * sevenBitsValue + SDL_PI_D / 2));

    SDL_RenderFillRect(renderer, &rect4);

    widgets("AnimatedWidget").setFigureAngle(fmod(-(SDL_PI_D / 128 * sevenBitsValue), 2 * SDL_PI_D));

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
                if (audioModeText == "Active mode: Stream" &&
                    widget.getType() == WidgetType::Button_StateMono) {

                    string widgetName = widget.getName();

                    if (widgetName != "AudioMode" &&
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

    // Draw the FFT and circular waveform last, then present the completed SDL frame.
    RenderFFTSpectrum_PortAudio(50.0f, 550.0f, 600.0f, 200.0f);
    RenderWaveform(50.0f, 800.0f, 600.0f, 200.0f);

    SDL_RenderPresent(renderer);

}
