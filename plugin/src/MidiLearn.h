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
//   * Audio thread: handleController(cc, value) only. It reads the map (atomics), pushes
//     (parameter, normalised value) pairs into a lock-free FIFO and triggers an
//     AsyncUpdater; the message thread drains the FIFO and calls setValueNotifyingHost()
//     wrapped in beginChangeGesture()/endChangeGesture(), so the host is notified from the
//     message thread as JUCE requires. drain() may also be called from the editor's vblank
//     callback to lower latency; it is idempotent.
//
// Mapping rules: one CC number maps to one parameter id; a CC already in use is reassigned;
// while learning, the first CC received on any channel binds the armed parameter.
// Listeners (juce::ChangeBroadcaster) are notified on the message thread when the map changes.
//
// State: toValueTree() -> <MidiLearn> <Map cc="" param=""/> ... </MidiLearn>.
class MidiLearn final : public juce::ChangeBroadcaster,
                        private juce::AsyncUpdater
{
public:
    static constexpr int kNumControllers = 128;
    static constexpr int kFifoSize = 512;
    static constexpr int kSustainPedal = 64;

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
    struct Event
    {
        enum class Type : int { Value = 0, Learned = 1 };
        Type type = Type::Value;
        int paramIndex = -1;
        int cc = -1;
        float normalised = 0.0f;
    };

    void handleAsyncUpdate() override { drain(); }
    int indexForId (const juce::String& paramId) const;
    juce::String idForIndex (int index) const;

    juce::AudioProcessorValueTreeState& apvts;
    std::vector<juce::AudioProcessorParameterWithID*> params;   // by processor parameter index

    std::array<std::atomic<int>, kNumControllers> ccToParam;   // parameter index or -1
    std::atomic<int> armedParam { -1 };

    juce::AbstractFifo fifo { kFifoSize };
    std::array<Event, kFifoSize> events {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiLearn)
};

} // namespace rcv
