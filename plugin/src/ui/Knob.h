#pragma once

#include "ui/ParamControl.h"

namespace rcv
{

// Rotary control with a visible label and value readout.
//
// Parameter knobs are attached to the APVTS (SliderAttachment); the value text comes from
// the ParamDesc (choice labels or number + unit), a double click returns to the default,
// the right-click menu offers MIDI learn and the tooltip is the full parameter name.
// Free knobs (the randomize amount) hold a plain value and report changes via onChange.
//
// Layout by style: Stacked = label / knob / value in a 64 px cell; Inline = label, knob,
// value side by side; Bare = knob and value (the row header names it).
class Knob final : public ParamControl
{
public:
    Knob (UiContext& context, const ParamInfo& paramInfo, const juce::String& labelIn, ControlStyle styleIn);

    // Free knob: fullName is the tooltip, unit is appended to the value text.
    Knob (const juce::String& labelIn, const juce::String& fullName, float minValue, float maxValue,
          float defaultValue, float interval, const juce::String& unit, ControlStyle styleIn);

    float getValue() const { return static_cast<float> (slider.getValue()); }
    std::function<void()> onChange;

    int preferredWidth() const override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void enablementChanged() override { repaint(); }

private:
    void setUp (float defaultRaw);
    juce::String valueText() const;
    int valueTextWidth() const;
    juce::Rectangle<int> valueArea() const;

    CellSlider slider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

    // Free knobs format through their own descriptor.
    ParamInfo ownInfo;
    juce::String ownName, ownUnit;
};

} // namespace rcv
