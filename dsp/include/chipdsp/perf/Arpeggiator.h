#pragma once

#include "chipdsp/ChipTypes.h"
#include "chipdsp/perf/NoteEvent.h"

#include <array>
#include <cstdint>
#include <span>

namespace chipdsp
{

// Note arpeggiator placed in front of the voice allocator.
//
// Held keys form a pattern (Up, Down, UpDown, AsPlayed, Random) repeated over 1..4
// octaves. Each step emits a note on; the matching note off is scheduled gatePercent %
// of the step length later and may land in a later block. Step timing:
//   * Sync while the host plays: steps start exactly when ppqPosition crosses a
//     multiple of the division. The first step after silence fires immediately at the
//     key press and the following ones fall on the grid.
//   * Sync while stopped, and Free: a free-running counter (division at the host BPM,
//     or freeRateHz) started by the first key press.
// A released key leaves the pattern at the next step (its sounding note finishes its
// gate). hold keeps released keys in the pattern until every key is up and a new key
// arrives, which starts a new pattern.
// Disabled: input events pass through unchanged; pending arp notes are turned off in
// the first block after the switch. Note offs for notes the arpeggiator does not hold
// (e.g. keys pressed before it was enabled) pass through so nothing gets stuck.
// Random uses a xorshift32 generator; seed() makes the sequence reproducible.
// Audio thread safe: fixed storage, no allocation, no exceptions.
class Arpeggiator
{
public:
    enum class Pattern : int
    {
        Up = 0,
        Down = 1,
        UpDown = 2,     // top and bottom notes are not repeated at the turn
        AsPlayed = 3,   // note-on order
        Random = 4
    };

    enum class RateMode : int
    {
        Sync = 0,
        Free = 1
    };

    enum class SyncDivision : int
    {
        Quarter = 0,          // 1/4
        Eighth = 1,           // 1/8
        EighthTriplet = 2,    // 1/8T
        Sixteenth = 3,        // 1/16
        SixteenthTriplet = 4, // 1/16T
        ThirtySecond = 5      // 1/32
    };

    struct Params
    {
        bool enabled = false;
        Pattern pattern = Pattern::Up;
        int octaves = 1;                                   // 1..4
        RateMode rateMode = RateMode::Sync;
        SyncDivision syncDivision = SyncDivision::Sixteenth;
        float freeRateHz = 8.0f;                           // 0.5..50
        float gatePercent = 50.0f;                         // 5..100
        bool hold = false;
    };

    static constexpr int kMaxHeldNotes = 128;
    static constexpr int kMaxOctaves = 4;
    static constexpr float kMinFreeRateHz = 0.5f;
    static constexpr float kMaxFreeRateHz = 50.0f;
    static constexpr float kMinGatePercent = 5.0f;
    static constexpr float kMaxGatePercent = 100.0f;

    Arpeggiator() noexcept;

    // Parameters may be changed at any time; transitions (disable, hold off) are applied
    // at the start of the next process() call.
    void setParams(const Params& p) noexcept;
    const Params& params() const noexcept { return current; }
    void setEnabled(bool enabled) noexcept { current.enabled = enabled; }
    void setPattern(Pattern pattern) noexcept { current.pattern = pattern; }
    void setOctaves(int octaves) noexcept;
    void setRateMode(RateMode mode) noexcept { current.rateMode = mode; }
    void setSyncDivision(SyncDivision division) noexcept { current.syncDivision = division; }
    void setFreeRateHz(float hz) noexcept;
    void setGatePercent(float percent) noexcept;
    void setHold(bool hold) noexcept { current.hold = hold; }

    // Reseeds the Random pattern generator (0 is replaced by a fixed non-zero seed).
    void seed(uint32_t value) noexcept;

    // Forgets held notes, pending note offs and timing. Emits nothing.
    void reset() noexcept;

    // Consumes the block's input events (sorted by sampleOffset) and fills outputEvents
    // with the events to hand to the voice allocator, sample offsets included.
    void process(const TransportInfo& transport, double sampleRate, int numSamples,
                 std::span<const NoteEvent> inputEvents, NoteEventBuffer& outputEvents) noexcept;

    int numHeldNotes() const noexcept { return heldCount; }
    int numPendingNoteOffs() const noexcept { return pendingCount; }

    static double divisionInQuarterNotes(SyncDivision division) noexcept;

    // Step length in samples for the current parameters (Sync uses transport.bpm even when stopped).
    double stepLengthSamples(const TransportInfo& transport, double sampleRate) const noexcept;

private:
    struct HeldNote
    {
        uint8_t note = 0;
        uint8_t midiChannel = 1;
        bool keyDown = false;
        float velocity = 0.0f;
    };

    struct SequenceNote
    {
        uint8_t note = 0;
        uint8_t midiChannel = 1;
        float velocity = 0.0f;
    };

    struct PendingOff
    {
        uint8_t note = 0;
        uint8_t midiChannel = 1;
        int time = 0;           // block-relative sample offset, may exceed the block
    };

    static constexpr int kMaxSequence = kMaxHeldNotes * kMaxOctaves * 2; // UpDown worst case
    static constexpr int kMaxPendingOffs = 64;

    void applyParamTransitions(NoteEventBuffer& out) noexcept;
    void handleInput(const NoteEvent& event, int time, NoteEventBuffer& out) noexcept;
    void fireStep(int time, NoteEventBuffer& out) noexcept;
    int nextStepOffset(int cursor, int numSamples) const noexcept;
    void rebuildSequence() noexcept;
    void emitPendingOffsUpTo(int time, NoteEventBuffer& out) noexcept;
    void emitPendingOffFor(uint8_t note, uint8_t midiChannel, int time, NoteEventBuffer& out) noexcept;
    void schedulePendingOff(uint8_t note, uint8_t midiChannel, int time, int now, NoteEventBuffer& out) noexcept;
    void flushPendingOffs(int time, NoteEventBuffer& out) noexcept;
    bool anyKeyDown() const noexcept;
    int findHeld(uint8_t note, uint8_t midiChannel) const noexcept;
    void removeHeld(int index) noexcept;
    uint32_t nextRandom() noexcept;

    Params current {};
    bool wasEnabled = false;
    bool wasHold = false;

    std::array<HeldNote, kMaxHeldNotes> held {};
    int heldCount = 0;

    std::array<SequenceNote, kMaxSequence> sequence {};
    int sequenceLength = 0;
    int stepIndex = 0;

    std::array<PendingOff, kMaxPendingOffs> pending {};
    int pendingCount = 0;

    // Block timing, valid inside process()
    bool gridMode = false;      // Sync and the host is playing
    double ppqStart = 0.0;
    double ppqPerSample = 0.0;
    double gridDivision = 0.25;    // quarter notes per step in grid mode
    double stepLen = 1.0;

    bool immediatePending = false; // a key press while idle: fire at the current position
    bool gridValid = false;
    int64_t lastGridStep = 0;      // index of the last grid line consumed
    bool freeValid = false;
    double freeNext = 0.0;         // block-relative time of the next free-running step
    bool hasFired = false;
    double lastFireTime = 0.0;     // block-relative time of the last step

    uint32_t rngState = 1u;
};

} // namespace chipdsp
