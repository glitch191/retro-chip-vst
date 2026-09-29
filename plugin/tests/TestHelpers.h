#pragma once

#include "PluginProcessor.h"

#include <memory>
#include <vector>

// Shared helpers of the plugin-core tests. Every test runs on the thread that created the
// JUCE message manager (see TestHelpers.cpp), so message-thread APIs may be called directly.
namespace rcvtest
{

std::unique_ptr<rcv::RetroChipProcessor> makeProcessor();

// Sets a parameter to an APVTS raw value (choice index, integer, float) through the host API.
void setRaw (rcv::RetroChipProcessor& proc, const juce::String& id, float raw);
float getRaw (rcv::RetroChipProcessor& proc, const juce::String& id);

// Enables "Out 1".."Out <count>" (the main bus stays enabled). Call before prepare().
void enableChannelBuses (rcv::RetroChipProcessor& proc, int count);

// Drives processBlock() the way a host does: one buffer sized for every enabled output
// channel and one MIDI buffer, cleared after each block.
class Runner
{
public:
    Runner (rcv::RetroChipProcessor& proc, double sampleRate, int blockSize);

    void noteOn (int midiChannel, int note, int velocity, int sampleOffset = 0);
    void noteOff (int midiChannel, int note, int sampleOffset = 0);
    void controller (int midiChannel, int cc, int value, int sampleOffset = 0);

    // Processes one block with the queued MIDI, then clears the MIDI buffer.
    void process();

    // Same block, handed to processBlock() as consecutive calls of 'pieceSize' samples (the
    // way the engine host itself cuts sub-blocks). The queued MIDI goes with the first piece,
    // so its events should sit at offsets below pieceSize.
    void processInPieces (int pieceSize);

    int blockSize() const noexcept { return size; }
    const float* mainChannel (int channel) const;           // 0 = left, 1 = right
    const float* busChannel (int busIndex, int channel);    // busIndex 1..10 ("Out N")
    juce::AudioBuffer<float>& buffer() noexcept { return audio; }
    juce::MidiBuffer& midi() noexcept { return events; }

private:
    rcv::RetroChipProcessor& processor;
    int size;
    juce::AudioBuffer<float> audio;
    juce::MidiBuffer events;
};

double rms (const float* data, int numSamples);
float peak (const float* data, int numSamples);

} // namespace rcvtest
