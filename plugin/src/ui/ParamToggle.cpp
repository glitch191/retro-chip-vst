#include "ui/ParamToggle.h"

#include "ui/Theme.h"

#include <cmath>

namespace rcv
{

ParamToggle::ParamToggle (UiContext& context, const ParamInfo& paramInfo, const juce::String& labelIn)
    : ParamControl (&context, &paramInfo, labelIn, ControlStyle::Bare)
{
    button.setButtonText (labelIn);
    button.setClickingTogglesState (true);
    button.setWantsKeyboardFocus (false);  // see ParamControl: Space stays with the host
    connectWidget (button);
    addAndMakeVisible (button);
    attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (context.apvts, paramInfo.id, button);
}

int ParamToggle::preferredWidth() const
{
    const int textW = static_cast<int> (std::ceil (theme::textWidth (theme::font(), label)));
    return juce::jmax (theme::kControlHeight, textW + 2 * theme::kGap + theme::kUnit);
}

} // namespace rcv
