#include "PluginProcessor.h"
#include "PluginEditor.h"

RetroChipProcessor::RetroChipProcessor()
    : AudioProcessor(BusesProperties().withOutput("Main", juce::AudioChannelSet::stereo(), true)),
      engine(chipdsp::createEngine(chipdsp::ChipId::Nes))
{
}

void RetroChipProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    engine->prepare(sampleRate, samplesPerBlock);
}

bool RetroChipProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void RetroChipProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    for (const auto metadata : midi)
    {
        const auto msg = metadata.getMessage();
        const int channel = juce::jlimit(0, engine->numChannels() - 1, msg.getChannel() - 1);
        if (msg.isNoteOn())
            engine->noteOn(channel, static_cast<float>(msg.getNoteNumber()), msg.getFloatVelocity());
        else if (msg.isNoteOff())
            engine->noteOff(channel);
    }

    engine->renderBlock(buffer.getWritePointer(0), buffer.getWritePointer(1), nullptr, nullptr, buffer.getNumSamples());
}

juce::AudioProcessorEditor* RetroChipProcessor::createEditor()
{
    return new RetroChipEditor(*this);
}

void RetroChipProcessor::getStateInformation(juce::MemoryBlock&) {}
void RetroChipProcessor::setStateInformation(const void*, int) {}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RetroChipProcessor();
}
