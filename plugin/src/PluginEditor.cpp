#include "PluginEditor.h"

#include "chipdsp/EngineFactory.h"

namespace rcv
{

RetroChipEditor::RetroChipEditor (RetroChipProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    if (auto* chipParam = dynamic_cast<juce::AudioParameterChoice*> (processor.parameters().getParameter (ParamIds::chip)))
        chipBox.addItemList (chipParam->choices, 1);
    addAndMakeVisible (chipBox);
    chipAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (processor.parameters(), ParamIds::chip, chipBox);

    chipLabel.setJustificationType (juce::Justification::centredLeft);
    chipLabel.setFont (juce::FontOptions (18.0f));
    addAndMakeVisible (chipLabel);

    processor.parameters().addParameterListener (ParamIds::chip, this);
    processor.visualizer().setEnabled (true);

    rebuildKnobs();
    setSize (640, 320);
}

RetroChipEditor::~RetroChipEditor()
{
    processor.visualizer().setEnabled (false);
    processor.parameters().removeParameterListener (ParamIds::chip, this);
    cancelPendingUpdate();
}

void RetroChipEditor::parameterChanged (const juce::String& parameterID, float)
{
    if (parameterID == juce::String (ParamIds::chip))
        triggerAsyncUpdate();
}

void RetroChipEditor::handleAsyncUpdate()
{
    rebuildKnobs();
}

void RetroChipEditor::rebuildKnobs()
{
    knobs.clear();
    const auto chip = processor.selectedChip();
    chipLabel.setText (juce::String (chipdsp::chipName (chip)) + " (stub engine)", juce::dontSendNotification);

    auto& apvts = processor.parameters();
    for (const auto* info : processor.paramRegistry().engineParams (chip))
    {
        if (static_cast<int> (knobs.size()) >= kMaxKnobs)
            break;
        Knob knob;
        knob.slider = std::make_unique<juce::Slider> (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow);
        knob.label = std::make_unique<juce::Label> (juce::String(), info->desc.name != nullptr ? juce::String (info->desc.name) : info->name);
        knob.label->setJustificationType (juce::Justification::centred);
        knob.label->setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (*knob.slider);
        addAndMakeVisible (*knob.label);
        knob.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, info->id, *knob.slider);
        knobs.push_back (std::move (knob));
    }
    resized();
}

void RetroChipEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff202124));
    g.setColour (juce::Colour (0xff9aa0a6));
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("Retro Chip placeholder editor", getLocalBounds().removeFromBottom (24).reduced (8, 0), juce::Justification::centredLeft);
}

void RetroChipEditor::resized()
{
    auto area = getLocalBounds().reduced (16);
    auto top = area.removeFromTop (28);
    chipBox.setBounds (top.removeFromLeft (160));
    top.removeFromLeft (12);
    chipLabel.setBounds (top);

    area.removeFromTop (16);
    const int count = static_cast<int> (knobs.size());
    if (count == 0)
        return;
    const int knobWidth = juce::jmax (64, area.getWidth() / juce::jmin (count, 4));
    auto row = area.removeFromTop (110);
    for (int i = 0; i < count; ++i)
    {
        if (i > 0 && i % 4 == 0)
        {
            area.removeFromTop (8);
            row = area.removeFromTop (110);
        }
        auto cell = row.removeFromLeft (knobWidth);
        knobs[static_cast<size_t> (i)].label->setBounds (cell.removeFromTop (18));
        knobs[static_cast<size_t> (i)].slider->setBounds (cell);
    }
}

} // namespace rcv
