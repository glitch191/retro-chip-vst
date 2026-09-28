#include "PluginEditor.h"

RetroChipEditor::RetroChipEditor(RetroChipProcessor& p) : AudioProcessorEditor(p)
{
    setSize(640, 360);
}

void RetroChipEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff202124));
    g.setColour(juce::Colour(0xffe8eaed));
    g.setFont(juce::FontOptions(15.0f));
    g.drawText("Retro Chip (skeleton)", getLocalBounds(), juce::Justification::centred);
}
