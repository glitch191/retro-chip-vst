#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters.h"
#include "VisualizerBuffers.h"

#include "chipdsp/ChipTypes.h"
#include "chipdsp/IChipEngine.h"
#include "chipdsp/perf/Arpeggiator.h"
#include "chipdsp/perf/Glide.h"
#include "chipdsp/perf/NoteEvent.h"
#include "chipdsp/perf/VoiceAllocator.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace rcv
{

class MidiLearn;

// Owns the three chip engines (all prepared, one active), the performance modules
// (Arpeggiator -> VoiceAllocator -> Glide) and the chip crossfade, and turns one host
// block into engine calls.
//
// How a block is processed (process(), audio thread, no allocation)
//
//   0. Blocks longer than the prepared maximum are cut into slices; every step below runs
//      per slice. MIDI is read straight from the MidiBuffer bytes, so no MidiMessage is
//      built.
//   1. Chip selection: the `chip` parameter is read (atomic). When it differs from the
//      running engine a 20 ms equal-power crossfade starts: the outgoing engine receives
//      noteOff on every channel and keeps rendering while its gain follows cos, the
//      incoming engine is reset (silent) and rises with sin. Once the fade has completed
//      the outgoing engine is reset. The allocator is reconfigured for the new chip and
//      the glide state is cleared; the arpeggiator keeps its held keys so it continues
//      on the new chip. A second switch during a fade finishes the running fade first.
//   2. Parameters: every engine parameter is compared with the value forwarded last time
//      (cached raw APVTS value) and only changed values reach IChipEngine::setParameter,
//      converted from the APVTS raw value to the native value (choice index + minValue).
//      All three engines are kept in sync so a switch never needs a bulk update. Global
//      parameters update the arpeggiator, the glide, the allocator (voice mode, poly mask
//      intersected with the chip's channels, chip default when empty), raw output and the
//      master gain target.
//   3. MIDI: note on/off go to the arpeggiator (pass-through when disabled), whose output
//      carries sample offsets. Pitch bend, sustain (CC 64), all notes/sound off, reset
//      controllers and other CCs are kept as control events with their offsets; other CCs
//      are handed to MidiLearn (lock-free).
//   4. Sub-blocks: the slice is split at every event offset. Before each sub-block the
//      events at its start are applied: notes through the VoiceAllocator (Poly round-robin
//      with oldest stealing, or MIDI channel N -> hardware channel N-1), then Glide
//      targets, then IChipEngine::noteOn/noteOff; sustain holds note-offs per MIDI channel
//      until the pedal is released. Every channel whose pitch (glide position + bend of
//      its MIDI channel, +/-2 semitones) changed gets setChannelPitch. While a glide or a
//      fade is running, sub-blocks are capped at kControlIntervalSamples so pitch and gain
//      keep moving between MIDI events. The active engine renders the main pair and, when
//      a channel bus is enabled or the editor's scopes are on, one pair per hardware
//      channel; during a fade the outgoing engine renders too and both are mixed.
//   5. Output: the master gain ramps linearly over the slice on the main and channel
//      signals, channel signals are copied to the enabled host buses (BusMap), and the
//      main and channel signals are pushed into the VisualizerBuffers.
//
// Threads: prepare(), attachParameters(), loadUserSample() are message-thread only.
// process() and reset() are audio-thread only. activeChip() may be read from any thread.
class EngineHost
{
public:
    static constexpr float kBendRangeSemitones = 2.0f;      // fixed pitch bend range
    static constexpr double kCrossfadeSeconds = 0.020;       // chip switch crossfade
    static constexpr int kControlIntervalSamples = 32;       // sub-block cap while gliding/fading
    static constexpr int kMaxControlEvents = 256;
    static constexpr int kNumChips = 3;

    // Host output pointers for each hardware channel bus of the current block, or nullptr
    // when that bus is disabled. Filled by the processor from getBusBuffer().
    struct BusMap
    {
        std::array<float*, chipdsp::kMaxHardwareChannels> left {};
        std::array<float*, chipdsp::kMaxHardwareChannels> right {};

        bool anyEnabled() const noexcept
        {
            for (auto* p : left)
                if (p != nullptr)
                    return true;
            return false;
        }
    };

    EngineHost();
    ~EngineHost();

    // Caches the atomic raw-value pointers of every parameter. Call once after the APVTS
    // exists and before prepare().
    void attachParameters (const ParamRegistry& registry, juce::AudioProcessorValueTreeState& apvts);
    void setMidiLearn (MidiLearn* learn) noexcept { midiLearn = learn; }

    void prepare (double sampleRate, int maxBlockSize);
    void reset() noexcept;

    void process (juce::AudioBuffer<float>& mainBus, juce::MidiBuffer& midi,
                  const chipdsp::TransportInfo& transport, const BusMap& buses) noexcept;

    chipdsp::IChipEngine& activeEngine() noexcept { return *engines[static_cast<size_t> (current)]; }
    const chipdsp::IChipEngine& activeEngine() const noexcept { return *engines[static_cast<size_t> (current)]; }
    chipdsp::IChipEngine& engine (chipdsp::ChipId chip) noexcept { return *engines[static_cast<size_t> (chip)]; }
    const chipdsp::IChipEngine& engine (chipdsp::ChipId chip) const noexcept { return *engines[static_cast<size_t> (chip)]; }

    // Chip currently rendering (the incoming one during a fade). Any thread. Before the
    // first process() call this is the chip the `chip` parameter held at attach time.
    chipdsp::ChipId activeChip() const noexcept { return static_cast<chipdsp::ChipId> (activeChipIndex.load (std::memory_order_relaxed)); }
    bool isCrossfading() const noexcept { return crossfading.load (std::memory_order_relaxed); }

    // Mono-mixes 'audio' and forwards it to IChipEngine::loadSample. Message thread only.
    bool loadUserSample (chipdsp::ChipId chip, int slot, const juce::AudioBuffer<float>& audio, double sampleRate);

    VisualizerBuffers& visualizer() noexcept { return scopes; }
    const VisualizerBuffers& visualizer() const noexcept { return scopes; }

    double currentSampleRate() const noexcept { return sampleRate; }
    int maxBlockSize() const noexcept { return maxBlock; }
    bool isPrepared() const noexcept { return prepared; }

    const chipdsp::VoiceAllocator& voiceAllocator() const noexcept { return allocator; }
    const chipdsp::Arpeggiator& arpeggiator() const noexcept { return arp; }
    const chipdsp::Glide& glide() const noexcept { return glider; }

private:
    struct ParamLink
    {
        std::atomic<float>* raw = nullptr;
        int engineId = -1;
        float offset = 0.0f;   // added to the raw value (choice parameters: minValue)
        float last = 0.0f;
        bool pushed = false;
    };

    struct ControlEvent
    {
        enum class Kind : uint8_t { PitchBend, SustainOn, SustainOff, Controller, AllNotesOff, ResetControllers };
        Kind kind = Kind::Controller;
        uint8_t midiChannel = 1;   // 1..16
        int offset = 0;
        int a = 0;                 // CC number / bend value
        int b = 0;                 // CC value
    };

    static constexpr int kMaxChannels = chipdsp::kMaxHardwareChannels;

    void processSlice (float* outL, float* outR, int numSamples, const juce::MidiBuffer& midi, int sliceStart,
                       const chipdsp::TransportInfo& transport, const BusMap& buses) noexcept;
    void updateChipSelection() noexcept;
    void beginSwitch (chipdsp::ChipId target) noexcept;
    void pushParameters() noexcept;
    void configureAllocator (bool force) noexcept;
    void silenceAll() noexcept;
    void collectMidi (const juce::MidiBuffer& midi, int sliceStart, int numSamples) noexcept;
    void applyControl (const ControlEvent& e) noexcept;
    void applyNote (const chipdsp::NoteEvent& e) noexcept;
    void releaseSustained (int midiChannel) noexcept;
    void updatePitches() noexcept;
    bool anyGlideActive() const noexcept;
    void renderSubBlock (int offset, int len, float* outL, float* outR, bool wantChannels) noexcept;
    void applyMasterGain (float* outL, float* outR, int numSamples, bool wantChannels) noexcept;
    void deliverBuses (const BusMap& buses, int numSamples, bool wantChannels) noexcept;
    void feedVisualizer (const float* outL, const float* outR, int numSamples, bool wantChannels) noexcept;
    float readRaw (const std::atomic<float>* p, float fallback) const noexcept;

    std::array<std::unique_ptr<chipdsp::IChipEngine>, kNumChips> engines;
    std::array<std::vector<ParamLink>, kNumChips> links;

    // Global parameter atomics (owned by the APVTS).
    std::atomic<float>* chipParam = nullptr;
    std::atomic<float>* rawOutputParam = nullptr;
    std::atomic<float>* voiceModeParam = nullptr;
    std::atomic<float>* polyChannelsParam = nullptr;
    std::atomic<float>* masterGainParam = nullptr;
    std::atomic<float>* arpEnabledParam = nullptr;
    std::atomic<float>* arpPatternParam = nullptr;
    std::atomic<float>* arpOctavesParam = nullptr;
    std::atomic<float>* arpRateModeParam = nullptr;
    std::atomic<float>* arpSyncDivisionParam = nullptr;
    std::atomic<float>* arpFreeRateParam = nullptr;
    std::atomic<float>* arpGateParam = nullptr;
    std::atomic<float>* arpHoldParam = nullptr;
    std::atomic<float>* glideTimeParam = nullptr;
    std::atomic<float>* glideModeParam = nullptr;

    MidiLearn* midiLearn = nullptr;
    VisualizerBuffers scopes;

    chipdsp::VoiceAllocator allocator;
    chipdsp::Arpeggiator arp;
    chipdsp::Glide glider;
    chipdsp::Arpeggiator::Params lastArpParams {};
    chipdsp::Glide::Params lastGlideParams {};
    bool arpParamsValid = false;
    bool glideParamsValid = false;
    bool lastRawOutput = false;
    bool rawOutputValid = false;
    bool lastMidiMode = false;
    uint32_t lastMask = 0;

    chipdsp::ChipId current = chipdsp::ChipId::Nes;
    chipdsp::ChipId outgoing = chipdsp::ChipId::Nes;
    std::atomic<int> activeChipIndex { 0 };
    std::atomic<bool> crossfading { false };
    bool fading = false;
    int fadePos = 0;
    int fadeLength = 1;
    std::vector<float> fadeIn, fadeOut;

    double sampleRate = 48000.0;
    int maxBlock = 0;
    bool prepared = false;

    // Per MIDI channel (index 0..15)
    std::array<float, 16> bend {};          // semitones
    std::array<bool, 16> sustain {};
    // Per hardware channel
    std::array<bool, kMaxChannels> sustained {};
    std::array<float, kMaxChannels> lastPitch {};
    std::array<bool, kMaxChannels> pitchValid {};

    chipdsp::NoteEventBuffer noteIn, noteOut;
    std::array<ControlEvent, kMaxControlEvents> controls {};
    int numControls = 0;

    float gain = 1.0f;
    float targetGain = 1.0f;
    int channelsThisBlock = 0;

    // Scratch (sized in prepare)
    std::vector<float> spareR;                                   // right channel when the main bus is mono
    std::vector<float> fadeMainL, fadeMainR;                     // outgoing engine main output
    std::array<std::vector<float>, kMaxChannels> chanBlockL, chanBlockR;   // per-channel block signals
    std::array<std::vector<float>, kMaxChannels> fadeChanL, fadeChanR;     // outgoing per-channel
    std::array<float*, kMaxChannels> chanPtrL {}, chanPtrR {}, fadePtrL {}, fadePtrR {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EngineHost)
};

} // namespace rcv
