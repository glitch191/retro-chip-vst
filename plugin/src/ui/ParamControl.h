#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "MidiLearn.h"
#include "Parameters.h"

#include <functional>
#include <memory>

namespace rcv
{

// What every editor control needs from the processor. Kept narrow so the chip panels can be
// built (and rendered for layout checks) from any ParamRegistry + APVTS pair.
struct UiContext
{
    juce::AudioProcessorValueTreeState& apvts;
    const ParamRegistry& registry;
    MidiLearn& midiLearn;
};

// How a control places its caption.
enum class ControlStyle
{
    Stacked,   // label above, control, value below (knob cells in the chip panels)
    Inline,    // label to the left of the control (header and sidebar rows)
    Bare       // no label: the caption is drawn elsewhere (operator grid rows)
};

// A JUCE widget whose right-click opens the owner's MIDI learn menu instead of the widget's
// own behaviour, and whose tooltip text is computed on demand (it includes the current CC).
template <typename Base>
class LearnableWidget : public Base
{
public:
    using Base::Base;

    std::function<void()> onPopupMenu;
    std::function<juce::String()> tooltipText;

    juce::String getTooltip() override { return tooltipText ? tooltipText() : Base::getTooltip(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (onPopupMenu && e.mods.isPopupMenu())
        {
            onPopupMenu();
            return;
        }
        Base::mouseDown (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            Base::mouseDrag (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            Base::mouseUp (e);
    }
};

// Rotary slider covering a whole knob cell (the cell is the drag target) that draws its knob
// in knobArea only; RcvLookAndFeel::getSliderLayout() reads knobArea.
class CellSlider final : public LearnableWidget<juce::Slider>
{
public:
    CellSlider() : LearnableWidget<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox) {}

    juce::Rectangle<int> knobArea;
};

// Formats a native engine value for display: choice label, "On"/"Off", or number + unit.
juce::String formatNativeValue (const ParamInfo& info, float native);

// Base of Knob, ParamToggle and ParamChoice: one host parameter (or a free value), its
// display label, the MIDI learn menu and the learning outline.
class ParamControl : public juce::Component
{
public:
    // True for controls that take half a knob cell (toggles): two stack in one column.
    virtual bool isHalfHeight() const { return false; }

    // Width the control needs at scale 1.0 so that neither the label nor any value is cut.
    virtual int preferredWidth() const = 0;

    const ParamInfo* paramInfo() const noexcept { return info; }
    const juce::String& labelText() const noexcept { return label; }
    ControlStyle style() const noexcept { return controlStyle; }

    // Inline style: width of the label column (so rows of a form align); -1 = the label's own width.
    void setLabelWidth (int width) { fixedLabelWidth = width; resized(); }
    int labelWidth() const;

    void paintOverChildren (juce::Graphics& g) override;

    // Opens the right-click MIDI learn menu (used by the screenshot hook).
    void openMidiLearnMenu() { showMidiLearnMenu(); }

protected:
    ParamControl (UiContext* context, const ParamInfo* paramInfo, juce::String labelIn, ControlStyle styleIn);

    // Hooks the right-click menu and the tooltip of the inner widget.
    template <typename Widget>
    void connectWidget (Widget& widget)
    {
        widget.onPopupMenu = [this] { showMidiLearnMenu(); };
        widget.tooltipText = [this] { return tooltipText(); };
    }

    bool isLearning() const;
    juce::String tooltipText() const;

    // Value shown in the tooltip after the name, for controls whose value text can be cut
    // (combo boxes in narrow cells); empty for the others.
    virtual juce::String tooltipValue() const { return {}; }
    void showMidiLearnMenu();

    UiContext* ctx = nullptr;       // null for free (unattached) controls
    const ParamInfo* info = nullptr;
    juce::String label;
    ControlStyle controlStyle;
    int fixedLabelWidth = -1;
};

// Creates the right control for a parameter: a toggle for booleans (Bool, or 0..1 with no
// labels / "Off","On" labels), a combo box for other enumerations, a knob otherwise.
std::unique_ptr<ParamControl> createParamControl (UiContext& ctx, const ParamInfo& info,
                                                  const juce::String& label, ControlStyle style);

bool isToggleParam (const ParamInfo& info);

} // namespace rcv
