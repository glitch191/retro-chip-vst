#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "chipdsp/EngineFactory.h"

#include <array>
#include <memory>

// Minimal skeleton used to validate the JUCE toolchain. Replaced by the full
// implementation described in docs/PLUGIN_SPECS.md.
class RetroChipProcessor final : public juce::AudioProcessor
{
public:
    RetroChipProcessor();
    ~RetroChipProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

private:
    std::unique_ptr<chipdsp::IChipEngine> engine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RetroChipProcessor)
};
