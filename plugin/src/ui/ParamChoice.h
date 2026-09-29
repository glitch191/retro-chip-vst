#pragma once

#include "ui/ParamControl.h"

namespace rcv
{

// Enumerated parameter (choice labels from the ParamDesc) shown as a combo box, attached
// to the APVTS through a ComboBoxAttachment. Stacked: label above, the combo box on the
// knob line of the cell; Inline: label to the left; Bare: combo box only.
// Right-click opens the MIDI learn menu.
class ParamChoice final : public ParamControl
{
public:
    ParamChoice (UiContext& context, const ParamInfo& paramInfo, const juce::String& labelIn, ControlStyle styleIn);

    int preferredWidth() const override;

    // Width of the combo box alone (widest choice label, text inset and arrow).
    int comboWidth() const;

    juce::ComboBox& comboBox() noexcept { return combo; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    void enablementChanged() override { repaint(); }

protected:
    juce::String tooltipValue() const override { return combo.getText(); }

private:
    LearnableWidget<juce::ComboBox> combo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
};

} // namespace rcv
