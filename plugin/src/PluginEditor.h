#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

class RetroChipEditor final : public juce::AudioProcessorEditor
{
public:
    explicit RetroChipEditor(RetroChipProcessor&);
    void paint(juce::Graphics&) override;
    void resized() override {}

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RetroChipEditor)
};
