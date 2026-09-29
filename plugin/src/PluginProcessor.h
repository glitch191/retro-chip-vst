#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "EngineHost.h"
#include "MidiLearn.h"
#include "Parameters.h"
#include "PresetManager.h"
#include "Randomizer.h"
#include "VisualizerBuffers.h"

#include "chipdsp/ChipTypes.h"

#include <atomic>
#include <optional>

namespace rcv
{

// The VST3 processor (docs/PLUGIN_SPECS.md). It owns the parameter registry, the APVTS,
// the engine host, MIDI learn, the preset manager and the randomizer, and exposes them to
// the editor through the accessors below.
//
// Buses: "Main" stereo (enabled) plus "Out 1".."Out 10" stereo, disabled by default; the
// per-chip meaning of Out N is documented in PLUGIN_SPECS.md (NES: 1 Pulse 1 ... 5 DMC;
// SNES: voices 1..8; Genesis: 1..6 FM, 7..9 PSG tone, 10 PSG noise).
//
// State: <RetroChipState version="1" presetName="" presetCategory="">
//          <Parameters .../> (APVTS)  <MidiLearn> <Map cc param/> </MidiLearn>
//          <UserSamples> <Sample chip slot wav="base64 WAV"/> </UserSamples>
//        </RetroChipState>, stored as XML through copyXmlToBinary(). Parameters and the
//        MIDI learn map are restored synchronously; the preset name and the samples go
//        through the engines' message-thread API, so when the host restores from another
//        thread they are queued and applied by the processor's message-thread timer.
//        getStateInformation() may run on any thread: while a restore is queued it writes
//        the queued preset name and samples, otherwise PresetManager::stateSnapshot().
//        Restoring never writes a parameter.
//
// poly_channels: 0 (the default) means the chip's default mask (NES pulses + triangle, SNES
// all voices, Genesis FM 1..6), resolved by the engine host on the audio thread at every
// chip switch; any other mask is kept across chips (intersected with the chip's channels).
class RetroChipProcessor final : public juce::AudioProcessor,
                                 private juce::Timer
{
public:
    static constexpr int kNumChannelBuses = chipdsp::kMaxHardwareChannels;
    static constexpr int kStateVersion = 1;
    static constexpr int kServiceIntervalMs = 50;

    RetroChipProcessor();
    ~RetroChipProcessor() override;

    // ----- juce::AudioProcessor ----------------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ----- plugin services for the editor ------------------------------------------------
    juce::AudioProcessorValueTreeState& parameters() noexcept { return apvts; }
    const ParamRegistry& paramRegistry() const noexcept { return registry; }
    EngineHost& engineHost() noexcept { return host; }
    const EngineHost& engineHost() const noexcept { return host; }
    PresetManager& presetManager() noexcept { return presets; }
    MidiLearn& midiLearn() noexcept { return learn; }
    Randomizer& randomizer() noexcept { return randomizerImpl; }
    VisualizerBuffers& visualizer() noexcept { return host.visualizer(); }

    // The chip the `chip` parameter selects (what the UI should display); engineHost()
    // .activeChip() tells which engine is rendering.
    chipdsp::ChipId selectedChip() const noexcept;

    static juce::String channelBusName (int busIndex);   // 1..10 -> "Out 1".."Out 10"

    // Development hook (editor screenshots, RCV_SCREENSHOT_NOTES): note-ons on MIDI channel 1
    // queued on the message thread and added at the start of the next block. Ignored while a
    // previous queue has not been consumed. Nothing is queued in normal use.
    static constexpr int kMaxTestNotes = 8;
    void queueTestNotes (std::initializer_list<int> notes, int velocity);

private:
    struct PendingRestore
    {
        juce::String presetName;
        juce::String presetCategory;
        juce::ValueTree samples;   // never modified after setStateInformation() created it
        int serial = 0;
    };

    static BusesProperties makeBuses();
    void timerCallback() override;
    void applyPendingRestore();

    EngineHost host;                             // creates the engines first
    ParamRegistry registry;                      // reads their descriptors
    juce::AudioProcessorValueTreeState apvts;    // built from the registry's layout
    MidiLearn learn;
    PresetManager presets;
    Randomizer randomizerImpl;

    // Message-thread services (timer)
    juce::CriticalSection pendingLock;           // message thread vs. the host's state thread; never the audio thread
    std::optional<PendingRestore> pendingRestore;
    int restoreSerial = 0;                       // guarded by pendingLock

    // queueTestNotes(): the message thread writes the notes while numTestNotes is 0, then
    // publishes the count; the audio thread copies them into testMidi (capacity reserved in
    // prepareToPlay) and resets the count.
    std::array<std::atomic<int>, kMaxTestNotes> testNotes {};
    std::atomic<int> testVelocity { 100 };
    std::atomic<int> numTestNotes { 0 };
    juce::MidiBuffer testMidi;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RetroChipProcessor)
};

} // namespace rcv
