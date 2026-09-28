#include "chipdsp/perf/Arpeggiator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace chipdsp
{

namespace
{
    // Absorbs floating-point error when a step lands exactly on a sample boundary.
    constexpr double kOffsetEpsilon = 1e-6;   // samples
    constexpr double kPpqEpsilon = 1e-6;      // quarter notes
    constexpr int kNoTime = std::numeric_limits<int>::max();

    int clampInt(int value, int lo, int hi) noexcept
    {
        return value < lo ? lo : (value > hi ? hi : value);
    }
} // namespace

Arpeggiator::Arpeggiator() noexcept
{
    seed(1u);
}

// ----- parameters -----------------------------------------------------------------------

void Arpeggiator::setParams(const Params& p) noexcept
{
    current = p;
    setOctaves(p.octaves);
    setFreeRateHz(p.freeRateHz);
    setGatePercent(p.gatePercent);
}

void Arpeggiator::setOctaves(int octaves) noexcept
{
    current.octaves = clampInt(octaves, 1, kMaxOctaves);
}

void Arpeggiator::setFreeRateHz(float hz) noexcept
{
    if (!(hz >= kMinFreeRateHz)) // also catches NaN
        hz = kMinFreeRateHz;
    if (hz > kMaxFreeRateHz)
        hz = kMaxFreeRateHz;
    current.freeRateHz = hz;
}

void Arpeggiator::setGatePercent(float percent) noexcept
{
    if (!(percent >= kMinGatePercent))
        percent = kMinGatePercent;
    if (percent > kMaxGatePercent)
        percent = kMaxGatePercent;
    current.gatePercent = percent;
}

void Arpeggiator::seed(uint32_t value) noexcept
{
    rngState = value != 0u ? value : 0x9E3779B9u;
}

uint32_t Arpeggiator::nextRandom() noexcept
{
    // xorshift32 (Marsaglia), period 2^32 - 1, state must never be 0.
    uint32_t x = rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rngState = x;
    return x;
}

void Arpeggiator::reset() noexcept
{
    heldCount = 0;
    sequenceLength = 0;
    stepIndex = 0;
    pendingCount = 0;
    immediatePending = false;
    gridValid = false;
    freeValid = false;
    hasFired = false;
    lastFireTime = 0.0;
    freeNext = 0.0;
    wasEnabled = current.enabled;
    wasHold = current.hold;
}

double Arpeggiator::divisionInQuarterNotes(SyncDivision division) noexcept
{
    switch (division)
    {
        case SyncDivision::Quarter:          return 1.0;
        case SyncDivision::Eighth:           return 0.5;
        case SyncDivision::EighthTriplet:    return 1.0 / 3.0;
        case SyncDivision::Sixteenth:        return 0.25;
        case SyncDivision::SixteenthTriplet: return 1.0 / 6.0;
        case SyncDivision::ThirtySecond:     return 0.125;
    }
    return 0.25;
}

double Arpeggiator::stepLengthSamples(const TransportInfo& transport, double sampleRate) const noexcept
{
    if (!(sampleRate > 0.0))
        sampleRate = 48000.0;
    double length;
    if (current.rateMode == RateMode::Free)
    {
        length = sampleRate / static_cast<double>(current.freeRateHz);
    }
    else
    {
        const double bpm = std::clamp(transport.bpm, 1.0, 999.0);
        length = divisionInQuarterNotes(current.syncDivision) * 60.0 * sampleRate / bpm;
    }
    return length < 1.0 ? 1.0 : length;
}

// ----- held notes -----------------------------------------------------------------------

bool Arpeggiator::anyKeyDown() const noexcept
{
    for (int i = 0; i < heldCount; ++i)
        if (held[static_cast<size_t>(i)].keyDown)
            return true;
    return false;
}

int Arpeggiator::findHeld(uint8_t note, uint8_t midiChannel) const noexcept
{
    for (int i = 0; i < heldCount; ++i)
    {
        const HeldNote& h = held[static_cast<size_t>(i)];
        if (h.note == note && h.midiChannel == midiChannel)
            return i;
    }
    return -1;
}

void Arpeggiator::removeHeld(int index) noexcept
{
    for (int i = index; i + 1 < heldCount; ++i)
        held[static_cast<size_t>(i)] = held[static_cast<size_t>(i + 1)];
    --heldCount;
}

void Arpeggiator::rebuildSequence() noexcept
{
    sequenceLength = 0;
    if (heldCount == 0)
        return;

    // Base order: note-on order for AsPlayed, ascending pitch otherwise (ties keep note-on order).
    std::array<int, kMaxHeldNotes> order {};
    for (int i = 0; i < heldCount; ++i)
        order[static_cast<size_t>(i)] = i;
    if (current.pattern != Pattern::AsPlayed)
    {
        std::sort(order.begin(), order.begin() + heldCount, [this](int a, int b) noexcept
        {
            const uint8_t na = held[static_cast<size_t>(a)].note;
            const uint8_t nb = held[static_cast<size_t>(b)].note;
            return na != nb ? na < nb : a < b;
        });
    }

    // Upward sequence: the base order transposed by 12 per octave. Notes above 127 are dropped.
    int length = 0;
    for (int octave = 0; octave < current.octaves; ++octave)
    {
        for (int i = 0; i < heldCount; ++i)
        {
            const HeldNote& h = held[static_cast<size_t>(order[static_cast<size_t>(i)])];
            const int note = static_cast<int>(h.note) + 12 * octave;
            if (note > 127)
                continue;
            sequence[static_cast<size_t>(length)] = { static_cast<uint8_t>(note), h.midiChannel, h.velocity };
            ++length;
        }
    }

    switch (current.pattern)
    {
        case Pattern::Up:
        case Pattern::AsPlayed:
        case Pattern::Random:
            break;
        case Pattern::Down:
            std::reverse(sequence.begin(), sequence.begin() + length);
            break;
        case Pattern::UpDown:
        {
            // Up, then back down without repeating the top and bottom notes.
            const int upLength = length;
            for (int i = upLength - 2; i >= 1; --i)
            {
                sequence[static_cast<size_t>(length)] = sequence[static_cast<size_t>(i)];
                ++length;
            }
            break;
        }
    }
    sequenceLength = length;
}

// ----- pending note offs ----------------------------------------------------------------

void Arpeggiator::emitPendingOffsUpTo(int time, NoteEventBuffer& out) noexcept
{
    int kept = 0;
    for (int i = 0; i < pendingCount; ++i)
    {
        const PendingOff& p = pending[static_cast<size_t>(i)];
        if (p.time <= time)
        {
            const int offset = p.time < 0 ? 0 : p.time;
            if (out.push(NoteEvent::off(p.note, p.midiChannel, offset)))
                continue; // emitted: drop from the list
        }
        pending[static_cast<size_t>(kept)] = p;
        ++kept;
    }
    pendingCount = kept;
}

void Arpeggiator::emitPendingOffFor(uint8_t note, uint8_t midiChannel, int time, NoteEventBuffer& out) noexcept
{
    int kept = 0;
    for (int i = 0; i < pendingCount; ++i)
    {
        const PendingOff& p = pending[static_cast<size_t>(i)];
        if (p.note == note && p.midiChannel == midiChannel)
        {
            if (out.push(NoteEvent::off(p.note, p.midiChannel, time)))
                continue;
        }
        pending[static_cast<size_t>(kept)] = p;
        ++kept;
    }
    pendingCount = kept;
}

void Arpeggiator::schedulePendingOff(uint8_t note, uint8_t midiChannel, int time, int now, NoteEventBuffer& out) noexcept
{
    if (pendingCount >= kMaxPendingOffs)
    {
        // Pathological overflow: turn the oldest pending note off right now.
        const PendingOff& oldest = pending[0];
        out.push(NoteEvent::off(oldest.note, oldest.midiChannel, now));
        for (int i = 0; i + 1 < pendingCount; ++i)
            pending[static_cast<size_t>(i)] = pending[static_cast<size_t>(i + 1)];
        --pendingCount;
    }
    pending[static_cast<size_t>(pendingCount)] = { note, midiChannel, time };
    ++pendingCount;
}

void Arpeggiator::flushPendingOffs(int time, NoteEventBuffer& out) noexcept
{
    for (int i = 0; i < pendingCount; ++i)
    {
        const PendingOff& p = pending[static_cast<size_t>(i)];
        out.push(NoteEvent::off(p.note, p.midiChannel, time));
    }
    pendingCount = 0;
}

// ----- processing -----------------------------------------------------------------------

void Arpeggiator::applyParamTransitions(NoteEventBuffer& out) noexcept
{
    if (wasEnabled && !current.enabled)
    {
        // Switched off: every sounding arp note stops now, the pattern is forgotten.
        flushPendingOffs(0, out);
        heldCount = 0;
        sequenceLength = 0;
        stepIndex = 0;
        immediatePending = false;
    }
    else if (!wasEnabled && current.enabled)
    {
        gridValid = false;
        freeValid = false;
        hasFired = false;
    }
    wasEnabled = current.enabled;

    if (wasHold && !current.hold)
    {
        // Hold released: latched keys that are physically up leave the pattern.
        for (int i = heldCount - 1; i >= 0; --i)
            if (!held[static_cast<size_t>(i)].keyDown)
                removeHeld(i);
        if (heldCount == 0)
            immediatePending = false;
    }
    wasHold = current.hold;
}

void Arpeggiator::handleInput(const NoteEvent& event, int time, NoteEventBuffer& out) noexcept
{
    const uint8_t note = NoteEvent::clampNote(event.note);
    const uint8_t midiChannel = NoteEvent::clampChannel(event.midiChannel);

    if (event.isNoteOn())
    {
        // A key pressed while nothing sounds fires at once; one that replaces a latched
        // chord (hold, every key up) takes over at the next step so the rhythm is kept.
        const bool wasIdle = heldCount == 0;
        if (current.hold && heldCount > 0 && !anyKeyDown())
        {
            heldCount = 0;
            stepIndex = 0;
        }

        const int existing = findHeld(note, midiChannel);
        if (existing >= 0)
        {
            HeldNote& h = held[static_cast<size_t>(existing)];
            h.velocity = event.velocity;
            h.keyDown = true;
        }
        else if (heldCount < kMaxHeldNotes)
        {
            held[static_cast<size_t>(heldCount)] = { note, midiChannel, true, event.velocity };
            ++heldCount;
        }
        else
        {
            return; // 128 notes already held: ignored
        }

        if (wasIdle)
        {
            immediatePending = true;
            stepIndex = 0;
        }
        return;
    }

    // Note off
    const int index = findHeld(note, midiChannel);
    if (index < 0)
    {
        // Not one of ours (pressed before the arp was enabled): pass it on so nothing sticks.
        out.push(NoteEvent::off(note, midiChannel, time));
        return;
    }
    if (current.hold)
    {
        held[static_cast<size_t>(index)].keyDown = false;
        return;
    }
    removeHeld(index);
    if (heldCount == 0)
        immediatePending = false;
}

int Arpeggiator::nextStepOffset(int cursor, int numSamples) const noexcept
{
    if (immediatePending)
        return cursor;

    double x;
    if (gridMode)
        x = (static_cast<double>(lastGridStep + 1) * gridDivision - ppqStart) / ppqPerSample;
    else if (freeValid)
        x = freeNext;
    else if (hasFired)
        x = lastFireTime + stepLen; // left the grid: keep the rhythm going from the last step
    else
        x = static_cast<double>(cursor);

    if (x >= static_cast<double>(numSamples))
        return numSamples;
    if (x <= static_cast<double>(cursor))
        return cursor;
    return static_cast<int>(std::ceil(x - kOffsetEpsilon));
}

void Arpeggiator::fireStep(int time, NoteEventBuffer& out) noexcept
{
    const bool immediate = immediatePending;
    immediatePending = false;

    // Consume the step even when nothing can be emitted, so the rhythm stays aligned.
    if (gridMode)
    {
        // Every grid line at or before this step is consumed. A grid-driven step always
        // consumes at least one line so the walk makes progress even if the division were
        // finer than a sample.
        const double ppq = ppqStart + static_cast<double>(time) * ppqPerSample;
        const auto consumed = static_cast<int64_t>(std::floor(ppq / gridDivision + kPpqEpsilon));
        const int64_t minimum = immediate ? lastGridStep : lastGridStep + 1;
        lastGridStep = consumed > minimum ? consumed : minimum;
    }
    else
    {
        double next = freeNext + stepLen;
        if (immediate || !freeValid || next <= static_cast<double>(time))
            next = static_cast<double>(time) + stepLen; // (re)start the counter from this step
        freeNext = next;
        freeValid = true;
    }
    lastFireTime = static_cast<double>(time);
    hasFired = true;

    rebuildSequence();
    if (sequenceLength == 0)
        return;

    int index;
    if (current.pattern == Pattern::Random)
    {
        index = static_cast<int>(nextRandom() % static_cast<uint32_t>(sequenceLength));
    }
    else
    {
        if (stepIndex >= sequenceLength)
            stepIndex = 0;
        index = stepIndex;
        ++stepIndex;
    }

    const SequenceNote& n = sequence[static_cast<size_t>(index)];

    // A previous instance of this note still sounding is turned off first (gate 100 %,
    // repeated single note), so its note off never follows the new note on.
    emitPendingOffFor(n.note, n.midiChannel, time, out);

    if (!out.push(NoteEvent::on(n.note, n.velocity, n.midiChannel, time)))
        return;

    const double gateSamples = stepLen * static_cast<double>(current.gatePercent) / 100.0;
    int gate = static_cast<int>(std::llround(gateSamples));
    if (gate < 1)
        gate = 1;
    schedulePendingOff(n.note, n.midiChannel, time + gate, time, out);
}

void Arpeggiator::process(const TransportInfo& transport, double sampleRate, int numSamples,
                          std::span<const NoteEvent> inputEvents, NoteEventBuffer& outputEvents) noexcept
{
    outputEvents.clear();
    if (numSamples < 0)
        numSamples = 0;
    if (!(sampleRate > 0.0))
        sampleRate = 48000.0;

    applyParamTransitions(outputEvents);

    if (!current.enabled)
    {
        for (const NoteEvent& e : inputEvents)
            outputEvents.push(e);
        return;
    }

    // Block timing.
    stepLen = stepLengthSamples(transport, sampleRate);
    gridMode = current.rateMode == RateMode::Sync && transport.isPlaying;
    if (gridMode)
    {
        gridDivision = divisionInQuarterNotes(current.syncDivision);
        ppqStart = transport.ppqPosition;
        ppqPerSample = std::clamp(transport.bpm, 1.0, 999.0) / (60.0 * sampleRate);
        // Index of the last grid line strictly before this block. Continuity leaves
        // lastGridStep there, or one behind when a line rounded onto the previous block's
        // end; anything else is a transport jump (loop, locate) and resynchronises.
        const auto expected = static_cast<int64_t>(std::ceil(ppqStart / gridDivision - kPpqEpsilon)) - 1;
        if (!gridValid || lastGridStep > expected || lastGridStep < expected - 1)
            lastGridStep = expected;
        gridValid = true;
        freeValid = false;
    }
    else
    {
        gridValid = false;
    }

    // Walk the block in time order: input events, step firings and pending note offs.
    const int lastOffset = numSamples > 0 ? numSamples - 1 : 0;
    int cursor = 0;
    size_t inputIndex = 0;
    while (true)
    {
        int nextInput = kNoTime;
        if (inputIndex < inputEvents.size())
            nextInput = clampInt(inputEvents[inputIndex].sampleOffset, cursor, lastOffset);

        int nextStep = kNoTime;
        if (heldCount > 0)
        {
            const int s = nextStepOffset(cursor, numSamples);
            if (s < numSamples)
                nextStep = s;
        }

        if (nextInput == kNoTime && nextStep == kNoTime)
            break;

        const int t = std::min(nextInput, nextStep);
        emitPendingOffsUpTo(t, outputEvents);
        if (nextInput <= nextStep)
        {
            // Input first at equal times: a key released on the step leaves the pattern now,
            // a key pressed on the step is part of it.
            handleInput(inputEvents[inputIndex], t, outputEvents);
            ++inputIndex;
        }
        else
        {
            fireStep(t, outputEvents);
        }
        cursor = t;
    }
    emitPendingOffsUpTo(lastOffset, outputEvents);

    // Make block-relative times relative to the next block.
    for (int i = 0; i < pendingCount; ++i)
        pending[static_cast<size_t>(i)].time -= numSamples;
    freeNext -= static_cast<double>(numSamples);
    lastFireTime -= static_cast<double>(numSamples);
}

} // namespace chipdsp
