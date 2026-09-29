#include "ui/Knob.h"

#include "ui/Theme.h"

#include <cmath>

namespace rcv
{

Knob::Knob (UiContext& context, const ParamInfo& paramInfo, const juce::String& labelIn, ControlStyle styleIn)
    : ParamControl (&context, &paramInfo, labelIn, styleIn)
{
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (context.apvts, paramInfo.id, slider);
    setUp (paramInfo.rawFromNative (paramInfo.clampNative (paramInfo.desc.defaultValue)));
}

Knob::Knob (const juce::String& labelIn, const juce::String& fullName, float minValue, float maxValue,
            float defaultValue, float interval, const juce::String& unit, ControlStyle styleIn)
    : ParamControl (nullptr, &ownInfo, labelIn, styleIn), ownName (fullName), ownUnit (unit)
{
    ownInfo.id = {};
    ownInfo.name = fullName;
    ownInfo.kind = interval >= 1.0f ? ParamKind::Int : ParamKind::Float;
    ownInfo.desc = { -1, "", ownName.toRawUTF8(), "", minValue, maxValue, defaultValue,
                     interval >= 1.0f, ownUnit.toRawUTF8(), nullptr };

    slider.setRange (minValue, maxValue, interval);
    slider.setValue (defaultValue, juce::dontSendNotification);
    setUp (defaultValue);
}

void Knob::setUp (float defaultRaw)
{
    slider.setDoubleClickReturnValue (true, defaultRaw);
    slider.setRepaintsOnMouseActivity (true);
    slider.setPopupMenuEnabled (false);
    slider.onValueChange = [this]
    {
        repaint (valueArea());
        if (onChange)
            onChange();
    };
    connectWidget (slider);
    addAndMakeVisible (slider);
}

juce::String Knob::valueText() const
{
    return formatNativeValue (*info, info->nativeFromRaw (static_cast<float> (slider.getValue())));
}

int Knob::valueTextWidth() const
{
    const auto f = theme::font();
    const auto& d = info->desc;
    float w = 0.0f;
    if (d.choiceLabels != nullptr)
    {
        const int count = static_cast<int> (std::lround (d.maxValue - d.minValue)) + 1;
        for (int i = 0; i < count; ++i)
            w = juce::jmax (w, theme::textWidth (f, d.choiceLabels[i]));
    }
    else
    {
        for (float v : { d.minValue, d.maxValue, d.defaultValue, 0.5f * (d.minValue + d.maxValue) })
            w = juce::jmax (w, theme::textWidth (f, formatNativeValue (*info, v)));
    }
    return static_cast<int> (std::ceil (w));
}

int Knob::preferredWidth() const
{
    const int labelW = labelWidth();
    const int valueW = valueTextWidth();
    switch (controlStyle)
    {
        case ControlStyle::Stacked: return juce::jmax (theme::kKnobMinWidth, labelW + theme::kUnit, valueW + theme::kUnit);
        case ControlStyle::Inline:  return labelW + theme::kGap + theme::kKnobDiameter + theme::kGap + valueW;
        case ControlStyle::Bare:    return theme::kKnobDiameter + theme::kGap + valueW;
    }
    return theme::kKnobMinWidth;
}

juce::Rectangle<int> Knob::valueArea() const
{
    auto b = getLocalBounds();
    switch (controlStyle)
    {
        case ControlStyle::Stacked: return b.removeFromBottom (theme::kLabelHeight);
        case ControlStyle::Inline:
        case ControlStyle::Bare:
        {
            const int knobRight = slider.knobArea.getRight() + theme::kGap;
            return b.withLeft (knobRight);
        }
    }
    return b;
}

void Knob::resized()
{
    auto b = getLocalBounds();
    const int d = juce::jmin (theme::kKnobDiameter, b.getHeight());
    juce::Rectangle<int> knob;
    switch (controlStyle)
    {
        case ControlStyle::Stacked:
            knob = juce::Rectangle<int> (d, d).withCentre ({ b.getCentreX(), theme::kLabelHeight + theme::kOpticalOffset + d / 2 });
            break;
        case ControlStyle::Inline:
            knob = juce::Rectangle<int> (labelWidth() + theme::kGap, (b.getHeight() - d) / 2, d, d);
            break;
        case ControlStyle::Bare:
            knob = juce::Rectangle<int> (0, (b.getHeight() - d) / 2, d, d);
            break;
    }
    slider.knobArea = knob;
    slider.setBounds (b);
    slider.resized();   // the layout depends on knobArea even when the size did not change
}

void Knob::paint (juce::Graphics& g)
{
    const bool enabled = isEnabled();
    g.setFont (theme::font());

    if (controlStyle == ControlStyle::Stacked)
    {
        g.setColour (enabled ? theme::colours::text : theme::colours::textDisabled);
        g.drawText (label, getLocalBounds().removeFromTop (theme::kLabelHeight), juce::Justification::centred, false);
    }
    else if (controlStyle == ControlStyle::Inline)
    {
        g.setColour (enabled ? theme::colours::text : theme::colours::textDisabled);
        g.drawText (label, getLocalBounds().withRight (slider.knobArea.getX() - theme::kGap),
                    juce::Justification::centredLeft, false);
    }

    // While MIDI learn is armed for this knob the value turns accent (with the outline
    // drawn by ParamControl::paintOverChildren).
    g.setColour (isLearning() ? theme::colours::accent : (enabled ? theme::colours::textDim : theme::colours::textDisabled));
    const auto just = controlStyle == ControlStyle::Stacked ? juce::Justification::centred : juce::Justification::centredLeft;
    g.drawText (valueText(), valueArea(), just, false);
}

} // namespace rcv
