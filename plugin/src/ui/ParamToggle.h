#pragma once

#include "ui/ParamControl.h"

namespace rcv
{

// On/off parameter shown as a labelled toggle button (accent fill when on), attached to
// the APVTS through a ButtonAttachment. Half a knob cell high: two toggles stack in one
// column of a group. Right-click opens the MIDI learn menu.
class ParamToggle final : public ParamControl
{
public:
    ParamToggle (UiContext& context, const ParamInfo& paramInfo, const juce::String& labelIn);

    bool isHalfHeight() const override { return true; }
    int preferredWidth() const override;

    void resized() override { button.setBounds (getLocalBounds()); }

private:
    LearnableWidget<juce::ToggleButton> button;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
};

} // namespace rcv
