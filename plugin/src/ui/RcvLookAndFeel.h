#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/Theme.h"

#include <vector>

namespace rcv
{

// Rack-module look and feel for every editor control (colours, fonts, sizes and drawing
// helpers from ui/Theme.h): rotary knob = grooved track + silkscreen value arc + pointer on
// a dark cap lit from above; buttons and combo boxes = dark rectangular hardware buttons
// with capitals, toggles with a red LED lit while on; text fields and tooltips in recessed
// black windows; popup menus and lists select in blue with amber text; alert windows,
// menus and tooltips in Bahnschrift (or the user typeface).
//
// Transitions: hover and toggle-state changes fade over theme::kTransitionMs. The value of
// a transition is computed from elapsed wall-clock time (Time::getMillisecondCounterHiRes),
// never from a frame count, so the duration is the same at 60 Hz and 144 Hz. A drawing
// method asks transition(); when the target state changed, a transition is recorded and
// the editor's vblank callback calls updateTransitions(), which repaints the components
// whose transition is running and forgets finished ones. With no transition running
// updateTransitions() does nothing, so the editor stays idle at rest.
class RcvLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    RcvLookAndFeel();

    // Current 0..1 value of a component's transition channel moving towards target.
    enum Channel { hoverChannel = 0, stateChannel = 1 };
    float transition (juce::Component& component, Channel channel, bool target);

    // Vblank: repaints components with a running transition. Returns true while any runs.
    bool updateTransitions();
    bool hasRunningTransitions() const noexcept { return ! running.empty(); }

    // ----- juce::LookAndFeel -------------------------------------------------------------------
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    juce::Slider::SliderLayout getSliderLayout (juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool isHighlighted, bool isDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool isHighlighted, bool isDown) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool isHighlighted, bool isDown) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::PopupMenu::Options getOptionsForComboBoxPopupMenu (juce::ComboBox&, juce::Label&) override;

    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon,
                            const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override;
    juce::Font getPopupMenuFont() override;
    int getPopupMenuBorderSize() override { return theme::kPopupBorder; }

    juce::Font getAlertWindowTitleFont() override;
    juce::Font getAlertWindowMessageFont() override;
    juce::Font getAlertWindowFont() override;

    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    // A caret that does not blink: a focused text field stays idle at rest (no timer repaints).
    juce::CaretComponent* createCaretComponent (juce::Component* keyFocusOwner) override;

    int getDefaultScrollbarWidth() override { return theme::kScrollbarWidth; }
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height, bool isVertical,
                        int thumbStart, int thumbSize, bool isMouseOver, bool isMouseDown) override;

    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;

private:
    void drawHardwareButton (juce::Graphics&, juce::Button&, const juce::String& text, bool isHighlighted, bool isDown);

    struct Transition
    {
        juce::Component::SafePointer<juce::Component> component;
        Channel channel = hoverChannel;
        float from = 0.0f;
        float to = 0.0f;
        double startMs = 0.0;
    };

    static float valueAt (const Transition& t, double nowMs) noexcept;

    std::vector<Transition> running;
};

} // namespace rcv
