#include "ui/ParamChoice.h"

#include "ui/Theme.h"

#include <cmath>

namespace rcv
{

ParamChoice::ParamChoice (UiContext& context, const ParamInfo& paramInfo, const juce::String& labelIn, ControlStyle styleIn)
    : ParamControl (&context, &paramInfo, labelIn, styleIn)
{
    const auto& d = paramInfo.desc;
    const int count = static_cast<int> (std::lround (d.maxValue - d.minValue)) + 1;
    for (int i = 0; i < count; ++i)
        combo.addItem (d.choiceLabels != nullptr ? juce::String (d.choiceLabels[i]) : juce::String (d.minValue + static_cast<float> (i)), i + 1);

    combo.setRepaintsOnMouseActivity (true);
    combo.setWantsKeyboardFocus (false);   // see ParamControl: arrow keys stay with the host
    connectWidget (combo);
    addAndMakeVisible (combo);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (context.apvts, paramInfo.id, combo);
}

int ParamChoice::comboWidth() const
{
    const auto f = theme::font();
    float w = 0.0f;
    for (int i = 0; i < combo.getNumItems(); ++i)
        w = juce::jmax (w, theme::textWidth (f, combo.getItemText (i)));
    return juce::jmax (theme::kChoiceMinWidth, theme::kGap + static_cast<int> (std::ceil (w)) + theme::kUnit + theme::kComboArrowWidth);
}

int ParamChoice::preferredWidth() const
{
    switch (controlStyle)
    {
        case ControlStyle::Stacked: return juce::jmax (comboWidth(), labelWidth() + theme::kUnit);
        case ControlStyle::Inline:  return labelWidth() + theme::kGap + comboWidth();
        case ControlStyle::Bare:    return comboWidth();
    }
    return comboWidth();
}

void ParamChoice::resized()
{
    auto b = getLocalBounds();
    const int h = juce::jmin (theme::kControlHeight, b.getHeight());
    switch (controlStyle)
    {
        case ControlStyle::Stacked:
        {
            // Centred on the knob line of the cell so combo boxes and knobs align in a row.
            const int centreY = theme::kKnobCentreY;
            combo.setBounds (b.getX(), centreY - h / 2, b.getWidth(), h);
            break;
        }
        case ControlStyle::Inline:
            combo.setBounds (b.withLeft (labelWidth() + theme::kGap).withSizeKeepingCentre (b.getWidth() - labelWidth() - theme::kGap, h));
            break;
        case ControlStyle::Bare:
            combo.setBounds (b.withSizeKeepingCentre (b.getWidth(), h));
            break;
    }
}

void ParamChoice::paint (juce::Graphics& g)
{
    if (controlStyle == ControlStyle::Bare)
        return;
    g.setFont (theme::font());
    g.setColour (isEnabled() ? theme::colours::text : theme::colours::textDisabled);
    if (controlStyle == ControlStyle::Stacked)
        g.drawText (label, getLocalBounds().removeFromTop (theme::kLabelHeight), juce::Justification::centred, false);
    else
        g.drawText (label, getLocalBounds().withWidth (labelWidth()), juce::Justification::centredLeft, false);
}

} // namespace rcv
