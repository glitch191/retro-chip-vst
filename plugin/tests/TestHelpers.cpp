#include "TestHelpers.h"

#include "chipdsp/factory/StubEngine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cmath>

namespace
{
    // JUCE needs a message manager for timers, change broadcasters and the processor's
    // "is this the message thread" checks. It lives for the whole test run, on the thread
    // that runs the tests.
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juceInit;

    class JuceListener final : public Catch::EventListenerBase
    {
    public:
        using Catch::EventListenerBase::EventListenerBase;

        void testRunStarting (const Catch::TestRunInfo&) override
        {
            juceInit = std::make_unique<juce::ScopedJuceInitialiser_GUI>();
        }

        void testRunEnded (const Catch::TestRunStats&) override
        {
            juceInit.reset();
        }
    };
} // namespace

CATCH_REGISTER_LISTENER (JuceListener)

namespace rcvtest
{

std::unique_ptr<rcv::RetroChipProcessor> makeProcessor()
{
    return std::make_unique<rcv::RetroChipProcessor>();
}

void setRaw (rcv::RetroChipProcessor& proc, const juce::String& id, float raw)
{
    auto* param = proc.parameters().getParameter (id);
    REQUIRE (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (raw));
}

float getRaw (rcv::RetroChipProcessor& proc, const juce::String& id)
{
    auto* raw = proc.parameters().getRawParameterValue (id);
    REQUIRE (raw != nullptr);
    return raw->load();
}

void enableChannelBuses (rcv::RetroChipProcessor& proc, int count)
{
    for (int b = 1; b <= count; ++b)
    {
        auto* bus = proc.getBus (false, b);
        REQUIRE (bus != nullptr);
        REQUIRE (bus->enable (true));
    }
}

bool usesStubEngines (rcv::RetroChipProcessor& proc)
{
    return dynamic_cast<const chipdsp::StubEngine*> (&proc.engineHost().engine (chipdsp::ChipId::Nes)) != nullptr;
}

void prepareAudibleDefaults (rcv::RetroChipProcessor& proc)
{
    if (usesStubEngines (proc))
        return;
    setRaw (proc, "nes_p1_sw_release", 60.0f);
    setRaw (proc, "nes_dmc_loop", 1.0f);
    auto& pm = proc.presetManager();
    REQUIRE (pm.loadFactorySampleIntoSlot (chipdsp::ChipId::Snes, 0, "square_loop"));
    REQUIRE (pm.loadFactorySampleIntoSlot (chipdsp::ChipId::Nes, 0, "kick_long"));
}

Runner::Runner (rcv::RetroChipProcessor& proc, double sampleRate, int blockSize)
    : processor (proc), size (blockSize)
{
    processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
    processor.prepareToPlay (sampleRate, blockSize);
    audio.setSize (processor.getTotalNumOutputChannels(), blockSize);
    events.ensureSize (4096);
}

void Runner::noteOn (int midiChannel, int note, int velocity, int sampleOffset)
{
    events.addEvent (juce::MidiMessage::noteOn (midiChannel, note, static_cast<juce::uint8> (velocity)), sampleOffset);
}

void Runner::noteOff (int midiChannel, int note, int sampleOffset)
{
    events.addEvent (juce::MidiMessage::noteOff (midiChannel, note), sampleOffset);
}

void Runner::controller (int midiChannel, int cc, int value, int sampleOffset)
{
    events.addEvent (juce::MidiMessage::controllerEvent (midiChannel, cc, value), sampleOffset);
}

void Runner::process()
{
    processor.processBlock (audio, events);
    events.clear();
}

void Runner::processInPieces (int pieceSize)
{
    juce::MidiBuffer none;
    for (int start = 0; start < size; start += pieceSize)
    {
        const int n = std::min (pieceSize, size - start);
        juce::AudioBuffer<float> piece (audio.getArrayOfWritePointers(), audio.getNumChannels(), start, n);
        processor.processBlock (piece, start == 0 ? events : none);
    }
    events.clear();
}

const float* Runner::mainChannel (int channel) const
{
    return audio.getReadPointer (channel);
}

const float* Runner::busChannel (int busIndex, int channel)
{
    auto bus = processor.getBusBuffer (audio, false, busIndex);
    if (channel >= bus.getNumChannels())
        return nullptr;
    return bus.getReadPointer (channel);
}

double rms (const float* data, int numSamples)
{
    if (data == nullptr || numSamples <= 0)
        return 0.0;
    double sum = 0.0;
    for (int i = 0; i < numSamples; ++i)
        sum += static_cast<double> (data[i]) * static_cast<double> (data[i]);
    return std::sqrt (sum / numSamples);
}

float peak (const float* data, int numSamples)
{
    float p = 0.0f;
    for (int i = 0; data != nullptr && i < numSamples; ++i)
        p = std::max (p, std::abs (data[i]));
    return p;
}

} // namespace rcvtest
