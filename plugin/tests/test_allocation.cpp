// processBlock() must not allocate: every global operator new made by the audio thread
// (here the test thread, only while processBlock runs) is counted.

#include "AllocationCounter.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

namespace
{
    std::atomic<const void*> sink { nullptr };   // keeps test allocations observable
} // namespace

TEST_CASE ("The allocation counter counts operator new on the calling thread only", "[allocation]")
{
    {
        rcvtest::ScopedAllocationCounter counter;
        std::vector<int> v (100, 1);
        sink.store (v.data());
        auto p = std::make_unique<double[]> (16);
        sink.store (p.get());
        CHECK (counter.count() == 2);

        // Another thread's allocations are not counted by this thread's counter.
        std::thread other ([] { std::vector<int> w (50, 2); sink.store (w.data()); });
        other.join();
        const long afterThread = counter.count();
        CHECK (afterThread >= 2);   // std::thread itself may allocate its state on this thread
        long otherCount = -1;
        std::thread counted ([&otherCount]
        {
            rcvtest::ScopedAllocationCounter inner;
            std::vector<int> w (10, 3);
            sink.store (w.data());
            otherCount = inner.count();
        });
        counted.join();
        CHECK (otherCount == 1);
    }

    rcvtest::ScopedAllocationCounter idle;
    CHECK (idle.count() == 0);
}

TEST_CASE ("processBlock never allocates", "[allocation]")
{
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlock = 128;
    constexpr int kBlocks = 400;

    auto proc = rcvtest::makeProcessor();
    rcvtest::enableChannelBuses (*proc, rcv::RetroChipProcessor::kNumChannelBuses);
    proc->visualizer().setEnabled (true);
    proc->midiLearn().setMapping (74, rcv::ParamIds::masterGain);
    rcvtest::setRaw (*proc, rcv::ParamIds::glideTime, 80.0f);

    proc->setRateAndBufferSizeDetails (kSampleRate, kBlock);
    proc->prepareToPlay (kSampleRate, kBlock);

    // Every MIDI buffer is built before counting starts (MidiBuffer::addEvent allocates).
    std::vector<juce::MidiBuffer> midi (static_cast<size_t> (kBlocks));
    for (int b = 0; b < kBlocks; ++b)
    {
        auto& m = midi[static_cast<size_t> (b)];
        const int note = 48 + (b * 7) % 36;
        m.addEvent (juce::MidiMessage::noteOn (1 + b % 3, note, static_cast<juce::uint8> (60 + b % 60)), (b * 13) % kBlock);
        if (b >= 2)
            m.addEvent (juce::MidiMessage::noteOff (1 + (b - 2) % 3, 48 + ((b - 2) * 7) % 36), (b * 29) % kBlock);
        m.addEvent (juce::MidiMessage::controllerEvent (1, 74, (b * 5) % 128), 17);
        m.addEvent (juce::MidiMessage::pitchWheel (1, (b * 997) % 16384), 33);
        if (b % 50 == 10)
            m.addEvent (juce::MidiMessage::controllerEvent (1, 64, 127), 5);    // sustain on
        if (b % 50 == 30)
            m.addEvent (juce::MidiMessage::controllerEvent (1, 64, 0), 70);     // sustain off
        if (b == 250)
            m.addEvent (juce::MidiMessage::allNotesOff (1), 90);
        if (b == 260)
            m.addEvent (juce::MidiMessage::controllerEvent (1, 121, 0), 0);     // reset controllers
    }

    juce::AudioBuffer<float> buffer (proc->getTotalNumOutputChannels(), kBlock);
    juce::AudioBuffer<float> longBuffer (proc->getTotalNumOutputChannels(), 3 * kBlock + 5);   // sliced by the host
    juce::AudioBuffer<float> shortBuffer (proc->getTotalNumOutputChannels(), 17);
    juce::MidiBuffer emptyMidi;

    long allocations = 0;
    auto processCounted = [&] (juce::AudioBuffer<float>& audio, juce::MidiBuffer& events)
    {
        rcvtest::ScopedAllocationCounter counter;
        proc->processBlock (audio, events);
        allocations += counter.count();
    };

    for (int b = 0; b < kBlocks; ++b)
    {
        // Message-thread activity between blocks (not counted): parameter changes, chip
        // switches, mode changes, randomizer, MIDI learn drain.
        switch (b)
        {
            case 40:  rcvtest::setRaw (*proc, rcv::ParamIds::arpEnabled, 1.0f); break;
            case 80:  rcvtest::setRaw (*proc, rcv::ParamIds::chip, 1.0f); break;               // SNES
            case 85:  rcvtest::setRaw (*proc, rcv::ParamIds::chip, 2.0f); break;               // Genesis, during the fade
            case 120: proc->randomizer().randomize (chipdsp::ChipId::Genesis, 0.5f, 99u); break;
            case 140: rcvtest::setRaw (*proc, rcv::ParamIds::voiceMode, 0.0f); break;          // MIDI channel
            case 180: rcvtest::setRaw (*proc, rcv::ParamIds::voiceMode, 1.0f); break;          // Poly
            case 190: rcvtest::setRaw (*proc, rcv::ParamIds::polyChannels, 341.0f); break;    // 0x155
            case 200: rcvtest::setRaw (*proc, rcv::ParamIds::rawOutput, 1.0f); break;
            case 220: rcvtest::setRaw (*proc, rcv::ParamIds::arpRateMode, 1.0f); break;
            case 230: rcvtest::setRaw (*proc, rcv::ParamIds::arpEnabled, 0.0f); break;
            case 240: rcvtest::setRaw (*proc, rcv::ParamIds::chip, 0.0f); break;               // NES
            case 300: rcvtest::setRaw (*proc, rcv::ParamIds::masterGain, -12.0f); break;
            default: break;
        }
        if (b % 16 == 0)
            proc->midiLearn().drain();

        processCounted (buffer, midi[static_cast<size_t> (b)]);
        if (b % 97 == 0)
        {
            processCounted (longBuffer, emptyMidi);
            processCounted (shortBuffer, emptyMidi);
        }
    }

    proc->reset();
    {
        rcvtest::ScopedAllocationCounter counter;
        proc->reset();   // audio-thread API as well
        allocations += counter.count();
    }
    processCounted (buffer, emptyMidi);

    INFO ("operator new calls inside processBlock: " << allocations);
    CHECK (allocations == 0);
}
