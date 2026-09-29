#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <vector>

namespace rcv
{

// MIDI CC -> host parameter mapping with a learn mode.
//
// Threads
//   * Message thread: learn()/cancelLearn(), setMapping()/clearMapping()/clearAll(),
//     queries, serialization, drain().
//   * Audio thread: handleController(cc, value) only. It reads the map (atomics) and stores
//     the normalised value in the target parameter's latest-value slot (one atomic value
//     plus a dirty flag per parameter, allocated at construction); a learn completion is
//     published through one atomic. Nothing is queued, so nothing can overflow: a burst of
//     CC messages collapses to the newest value per parameter, and the final controller
//     position always arrives. A message-thread juce::Timer (kDrainIntervalMs) drains the
//     slots and calls setValueNotifyingHost() wrapped in beginChangeGesture()/
//     endChangeGesture(), so the host is notified from the message thread as JUCE
//     requires. drain() may also be called from the editor's vblank callback to lower
//     latency; it is idempotent.
//
// Mapping rules: one CC number maps to one parameter id; a CC already in use is reassigned;
// while learning, the first CC received on any channel binds the armed parameter.
// Listeners (juce::ChangeBroadcaster) are notified on the message thread when the map changes.
//
// State: toValueTree() -> <MidiLearn> <Map cc="" param=""/> ... </MidiLearn> (a child of
// <RetroChipState>, see PluginProcessor.h).
class MidiLearn final : public juce::ChangeBroadcaster,
                        private juce::Timer
{
public:
    static constexpr int kNumControllers = 128;
    static constexpr int kSustainPedal = 64;
    static constexpr int kDrainIntervalMs = 15;

    explicit MidiLearn (juce::AudioProcessorValueTreeState& apvts);
    ~MidiLearn() override;

    // ----- message thread ----------------------------------------------------------------
    void learn (const juce::String& paramId);     // arms: the next CC binds this parameter
    void cancelLearn();
    bool isLearning() const noexcept { return armedParam.load (std::memory_order_relaxed) >= 0; }
    juce::String learningParamId() const;

    void setMapping (int cc, const juce::String& paramId);
    void clearMapping (const juce::String& paramId);
    void clearController (int cc);
    void clearAll();

    int controllerForParam (const juce::String& paramId) const;   // -1 when unmapped
    juce::String paramForController (int cc) const;               // empty when unmapped
    bool isMapped (const juce::String& paramId) const { return controllerForParam (paramId) >= 0; }

    juce::ValueTree toValueTree() const;
    void restoreFromValueTree (const juce::ValueTree& tree);     // the "MidiLearn" node (or invalid: clears)

    // Applies queued CC values to the parameters. Message thread.
    void drain();

    // ----- audio thread -------------------------------------------------------------------
    void handleController (int cc, int value) noexcept;

private:
    void timerCallback() override { drain(); }
    int indexForId (const juce::String& paramId) const;
    juce::String idForIndex (int index) const;
    void bind (int cc, int paramIndex);   // message thread: one CC per parameter

    juce::AudioProcessorValueTreeState& apvts;
    std::vector<juce::AudioProcessorParameterWithID*> params;   // by processor parameter index

    std::array<std::atomic<int>, kNumControllers> ccToParam;   // parameter index or -1
    std::atomic<int> armedParam { -1 };

    // Audio thread -> message thread (sized once in the constructor)
    std::vector<std::atomic<float>> latestValue;   // normalised, per parameter index
    std::vector<std::atomic<bool>> dirty;          // latestValue not yet applied
    std::atomic<bool> anyDirty { false };
    std::atomic<int> pendingLearn { -1 };          // (paramIndex << 8) | cc, or -1

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiLearn)
};

} // namespace rcv
