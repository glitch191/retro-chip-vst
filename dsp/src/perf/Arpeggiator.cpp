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

    // Transport sanitising. A ppqPosition beyond this (about 15 years at 120 BPM) is
    // treated like a non-finite one, which keeps the grid indices well inside int64.
    constexpr double kMaxPpq = 1.0e9;
    constexpr double kDefaultBpm = 120.0;
    constexpr double kDefaultSampleRate = 48000.0;
    constexpr double kMaxSampleRate = 1.0e7;

    int clampInt(int value, int lo, int hi) noexcept
    {
        return value < lo ? lo : (value > hi ? hi : value);
    }

    double sanitiseBpm(double bpm) noexcept
    {
        if (!std::isfinite(bpm) || !(bpm > 0.0))
            return kDefaultBpm;
        return std::clamp(bpm, 1.0, 999.0);
    }

    double sanitiseSampleRate(double sampleRate) noexcept
    {
        if (!std::isfinite(sampleRate) || !(sampleRate > 0.0))
            return kDefaultSampleRate;
        return std::clamp(sampleRate, 1.0, kMaxSampleRate);
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
    seedValue = value != 0u ? value : 0x9E3779B9u;
    rngState = seedValue;
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
    hasLast = false;
    ascending = true;
    pendingCount = 0;
    immediatePending = false;
    gridValid = false;
    freeValid = false;
    hasFired = false;
    lastFireTime = 0.0;
    freeNext = 0.0;
    expectedNextPpq = 0.0;
    rngState = seedValue;
    wasEnabled = current.enabled;
    wasHold = current.hold;
    wasPlaying = false;
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
    sampleRate = sanitiseSampleRate(sampleRate);
    double length;
    if (current.rateMode == RateMode::Free)
        length = sampleRate / static_cast<double>(current.freeRateHz);
    else
        length = divisionInQuarterNotes(current.syncDivision) * 60.0 * sampleRate / sanitiseBpm(transport.bpm);
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

// ----- pattern --------------------------------------------------------------------------

bool Arpeggiator::keyLess(const PatternKey& a, const PatternKey& b) const noexcept
{
    if (a.octave != b.octave)
        return a.octave < b.octave;
    if (current.pattern != Pattern::AsPlayed && a.pitch != b.pitch)
        return a.pitch < b.pitch;
    return a.serial < b.serial;
}

void Arpeggiator::rebuildSequence() noexcept
{
    sequenceLength = 0;
    if (heldCount == 0)
        return;

    // Base order: note-on order for AsPlayed, ascending pitch otherwise (ties keep
    // note-on order). The held array is in note-on order.
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

    // Upward sequence: the base order transposed by 12 per octave, so it is sorted by
    // PatternKey. Notes above 127 are dropped.
    int length = 0;
    for (int octave = 0; octave < current.octaves; ++octave)
    {
        for (int i = 0; i < heldCount; ++i)
        {
            const HeldNote& h = held[static_cast<size_t>(order[static_cast<size_t>(i)])];
            const int note = static_cast<int>(h.note) + 12 * octave;
            if (note > 127)
                continue;
            sequence[static_cast<size_t>(length)] = { static_cast<uint8_t>(note), h.midiChannel, h.velocity,
                                                      { octave, static_cast<int>(h.note), h.serial } };
            ++length;
        }
    }
    sequenceLength = length;
}

int Arpeggiator::selectStep() noexcept
{
    const int n = sequenceLength;
    if (current.pattern == Pattern::Random)
        return static_cast<int>(nextRandom() % static_cast<uint32_t>(n));

    // Neighbours of the last note played among the notes held now. The last note itself
    // may have left the pattern or notes may have joined on either side of it.
    int above = n;  // first entry after lastKey
    int below = -1; // last entry before lastKey
    if (hasLast)
    {
        for (int i = 0; i < n; ++i)
            if (keyLess(lastKey, sequence[static_cast<size_t>(i)].key))
            {
                above = i;
                break;
            }
        for (int i = n - 1; i >= 0; --i)
            if (keyLess(sequence[static_cast<size_t>(i)].key, lastKey))
            {
                below = i;
                break;
            }
    }

    int index;
    switch (current.pattern)
    {
        case Pattern::Down:
            index = !hasLast ? n - 1 : (below >= 0 ? below : n - 1);
            break;
        case Pattern::UpDown:
        {
            // Up to the top, back down to the bottom; the turning notes play once.
            bool up;
            if (!hasLast)
            {
                index = 0;
                up = true;
            }
            else if (ascending)
            {
                up = above < n;
                index = up ? above : (below >= 0 ? below : 0);
            }
            else
            {
                up = below < 0;
                index = !up ? below : (above < n ? above : 0);
            }
            if (index == 0)
                up = true;
            else if (index == n - 1)
                up = false;
            ascending = up;
            break;
        }
        default: // Up, AsPlayed
            index = !hasLast ? 0 : (above < n ? above : 0);
            break;
    }
    return index;
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

void Arpeggiator::deferOff(uint8_t note, uint8_t midiChannel, int time) noexcept
{
    // A note off that did not fit in the output buffer: sent as soon as there is room.
    // Only a full pending list (more than kMaxPendingOffs deferred offs) can lose one.
    if (pendingCount >= kMaxPendingOffs)
        return;
    pending[static_cast<size_t>(pendingCount)] = { note, midiChannel, time };
    ++pendingCount;
}

void Arpeggiator::flushPendingOffs(int time, NoteEventBuffer& out) noexcept
{
    // Every pending note off is sent now; one that does not fit stays pending, due now.
    int kept = 0;
    for (int i = 0; i < pendingCount; ++i)
    {
        PendingOff p = pending[static_cast<size_t>(i)];
        if (out.push(NoteEvent::off(p.note, p.midiChannel, time)))
            continue;
        p.time = std::min(p.time, time);
        pending[static_cast<size_t>(kept)] = p;
        ++kept;
    }
    pendingCount = kept;
}

void Arpeggiator::endBlock(int numSamples) noexcept
{
    // Make block-relative times relative to the next block. Overdue offs stay due at 0.
    for (int i = 0; i < pendingCount; ++i)
    {
        int& t = pending[static_cast<size_t>(i)].time;
        t = t > numSamples ? t - numSamples : 0;
    }
    freeNext -= static_cast<double>(numSamples);
    lastFireTime -= static_cast<double>(numSamples);
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
        hasLast = false;
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
            hasLast = false;
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
            held[static_cast<size_t>(heldCount)] = { note, midiChannel, true, event.velocity, nextSerial };
            ++nextSerial;
            ++heldCount;
        }
        else
        {
            return; // 128 notes already held: ignored
        }

        if (wasIdle)
        {
            immediatePending = true;
            hasLast = false;
        }
        return;
    }

    // Note off
    const int index = findHeld(note, midiChannel);
    if (index < 0)
    {
        // Not one of ours (pressed before the arp was enabled): pass it on so nothing sticks.
        if (!out.push(NoteEvent::off(note, midiChannel, time)))
            deferOff(note, midiChannel, time);
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

    if (!std::isfinite(x) || x >= static_cast<double>(numSamples))
        return numSamples; // no step in this block (the inputs are sanitised; this is a backstop)
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

    // Every arp note still sounding ends here, before the new note starts. With the gate
    // at most 100 % this only shortens a gate that rounded (or a tempo change pushed)
    // past the step, and it also covers a repeated single note.
    flushPendingOffs(time, out);

    rebuildSequence();
    if (sequenceLength == 0)
        return;

    const int index = selectStep();
    const SequenceNote& n = sequence[static_cast<size_t>(index)];
    lastKey = n.key;
    hasLast = true;

    // Never start a note whose note off could not be kept.
    if (pendingCount >= kMaxPendingOffs)
        return;
    if (!out.push(NoteEvent::on(n.note, n.velocity, n.midiChannel, time)))
        return;

    const double gateSamples = stepLen * static_cast<double>(current.gatePercent) / 100.0;
    int64_t gate = std::llround(gateSamples);
    gate = std::min<int64_t>(gate, static_cast<int64_t>(kNoTime - 1) - time); // no int overflow
    gate = std::max<int64_t>(gate, 1);
    pending[static_cast<size_t>(pendingCount)] = { n.note, n.midiChannel, time + static_cast<int>(gate) };
    ++pendingCount;
}

void Arpeggiator::process(const TransportInfo& transport, double sampleRate, int numSamples,
                          std::span<const NoteEvent> inputEvents, NoteEventBuffer& outputEvents) noexcept
{
    outputEvents.clear();
    if (numSamples < 0)
        numSamples = 0;
    sampleRate = sanitiseSampleRate(sampleRate);
    const int lastOffset = numSamples > 0 ? numSamples - 1 : 0;

    // A transport start replays the same Random sequence.
    if (transport.isPlaying && !wasPlaying)
        rngState = seedValue;
    wasPlaying = transport.isPlaying;

    applyParamTransitions(outputEvents);

    if (!current.enabled)
    {
        emitPendingOffsUpTo(0, outputEvents); // offs deferred from an earlier block
        for (const NoteEvent& e : inputEvents)
            if (!outputEvents.push(e) && e.isNoteOff())
                deferOff(e.note, e.midiChannel, clampInt(e.sampleOffset, 0, lastOffset));
        endBlock(numSamples);
        return;
    }

    // Block timing. A non-finite ppq cannot place a grid: the block runs free at the tempo.
    const double bpm = sanitiseBpm(transport.bpm);
    const bool ppqUsable = std::isfinite(transport.ppqPosition) && std::abs(transport.ppqPosition) <= kMaxPpq;
    stepLen = stepLengthSamples(transport, sampleRate);
    gridMode = current.rateMode == RateMode::Sync && transport.isPlaying && ppqUsable;
    if (gridMode)
    {
        gridDivision = divisionInQuarterNotes(current.syncDivision);
        ppqStart = transport.ppqPosition;
        ppqPerSample = bpm / (60.0 * sampleRate);
        // Index of the last grid line strictly before this block. When the transport runs
        // on, lastGridStep is already there, or one behind when the line fell after the
        // previous block's last sample (it rounds onto this block's first sample). Anything
        // else is a transport jump (loop, locate): resynchronise, skipping the lines passed over.
        const auto expected = static_cast<int64_t>(std::ceil(ppqStart / gridDivision - kPpqEpsilon)) - 1;
        bool continuous = gridValid && lastGridStep == expected;
        if (gridValid && lastGridStep == expected - 1)
        {
            // The unfired line belongs to this block only if the transport did not jump past it.
            const double line = static_cast<double>(expected) * gridDivision;
            continuous = line <= expectedNextPpq + ppqPerSample + kPpqEpsilon;
        }
        if (!continuous)
            lastGridStep = expected;
        gridValid = true;
        freeValid = false;
        expectedNextPpq = ppqStart + static_cast<double>(numSamples) * ppqPerSample;
    }
    else
    {
        gridValid = false;
    }

    // Walk the block in time order: input events, step firings and pending note offs.
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
    endBlock(numSamples);
}

} // namespace chipdsp
