#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

#include <memory>
#include <vector>

namespace rcv
{

// Placeholder editor: chip selector, chip name and a few auto-generated knobs for the
// selected chip. The real editor (plugin/src/ui) replaces it.
class RetroChipEditor final : public juce::AudioProcessorEditor,
                              private juce::AudioProcessorValueTreeState::Listener,
                              private juce::AsyncUpdater
{
public:
    explicit RetroChipEditor (RetroChipProcessor& processor);
    ~RetroChipEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    static constexpr int kMaxKnobs = 8;

    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override;
    void rebuildKnobs();

    RetroChipProcessor& processor;

    juce::ComboBox chipBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> chipAttachment;
    juce::Label chipLabel;

    struct Knob
    {
        std::unique_ptr<juce::Slider> slider;
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    std::vector<Knob> knobs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RetroChipEditor)
};

} // namespace rcv
