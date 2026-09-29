#include "chipdsp/perf/Arpeggiator.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using Catch::Approx;
using chipdsp::Arpeggiator;
using chipdsp::kMaxEventsPerBlock;
using chipdsp::NoteEvent;
using chipdsp::NoteEventBuffer;
using chipdsp::TransportInfo;

namespace
{
    // Reference numbers used throughout (derived by hand):
    //   120 BPM: one quarter note = 0.5 s = 24000 samples at 48 kHz.
    //   1/16 = 0.25 quarter = 125 ms = 6000 samples at 48 kHz (5512.5 at 44.1 kHz).
    //   Free 8 Hz = 48000 / 8 = 6000 samples.
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlock = 480;              // 10 ms blocks: 12.5 blocks per 1/16 step
    constexpr long long kStep = 6000;

    struct AbsEvent
    {
        long long time = 0; // absolute sample index
        NoteEvent event;
    };

    // Feeds an Arpeggiator block by block, keeping the transport's ppqPosition in sync with
    // the samples rendered, and records the output with absolute sample positions.
    struct Harness
    {
        Arpeggiator arp;
        TransportInfo transport;
        double sampleRate = kSampleRate;
        int blockSize = kBlock;
        long long blockStart = 0;
        std::vector<AbsEvent> queued;
        std::vector<AbsEvent> out;

        Harness()
        {
            transport.bpm = 120.0;
            transport.ppqPosition = 0.0;
            transport.isPlaying = true;
        }

        void queueOn(long long time, int note, float velocity = 1.0f, int midiChannel = 1)
        {
            queued.push_back({ time, NoteEvent::on(note, velocity, midiChannel, 0) });
        }

        void queueOff(long long time, int note, int midiChannel = 1)
        {
            queued.push_back({ time, NoteEvent::off(note, midiChannel, 0) });
        }

        void run(int numBlocks)
        {
            std::stable_sort(queued.begin(), queued.end(),
                             [](const AbsEvent& a, const AbsEvent& b) { return a.time < b.time; });
            for (int b = 0; b < numBlocks; ++b)
            {
                std::vector<NoteEvent> input;
                for (const AbsEvent& q : queued)
                {
                    if (q.time >= blockStart && q.time < blockStart + blockSize)
                    {
                        NoteEvent e = q.event;
                        e.sampleOffset = static_cast<int>(q.time - blockStart);
                        input.push_back(e);
                    }
                }
                NoteEventBuffer buffer;
                arp.process(transport, sampleRate, blockSize, input, buffer);
                for (const NoteEvent& e : buffer)
                    out.push_back({ blockStart + e.sampleOffset, e });
                if (transport.isPlaying)
                    transport.ppqPosition += static_cast<double>(blockSize) * transport.bpm / (60.0 * sampleRate);
                blockStart += blockSize;
            }
        }

        std::vector<AbsEvent> noteOns() const
        {
            std::vector<AbsEvent> r;
            for (const AbsEvent& e : out)
                if (e.event.isNoteOn())
                    r.push_back(e);
            return r;
        }

        std::vector<AbsEvent> noteOffs() const
        {
            std::vector<AbsEvent> r;
            for (const AbsEvent& e : out)
                if (e.event.isNoteOff())
                    r.push_back(e);
            return r;
        }

        std::vector<int> noteOnPitches(size_t count) const
        {
            std::vector<int> r;
            for (const AbsEvent& e : noteOns())
            {
                if (r.size() == count)
                    break;
                r.push_back(e.event.note);
            }
            return r;
        }
    };

    Arpeggiator::Params defaultParams()
    {
        Arpeggiator::Params p;
        p.enabled = true;
        p.pattern = Arpeggiator::Pattern::Up;
        p.octaves = 1;
        p.rateMode = Arpeggiator::RateMode::Sync;
        p.syncDivision = Arpeggiator::SyncDivision::Sixteenth;
        p.freeRateHz = 8.0f;
        p.gatePercent = 50.0f;
        p.hold = false;
        return p;
    }

    // C-E-G pressed together at sample 0.
    void holdCMajor(Harness& h)
    {
        h.queueOn(0, 60);
        h.queueOn(0, 64);
        h.queueOn(0, 67);
    }
} // namespace

// ---------------------------------------------------------------------------------------
// Step timing
// ---------------------------------------------------------------------------------------

TEST_CASE("Arpeggiator: 120 BPM 1/16 steps every 125 ms (6000 samples at 48 kHz)", "[perf][arp]")
{
    Harness h;
    h.arp.setParams(defaultParams());
    REQUIRE(h.arp.stepLengthSamples(h.transport, kSampleRate) == Approx(6000.0));

    h.queueOn(0, 60);
    h.run(60); // 28800 samples = 4.8 steps: note ons at 0, 6000, 12000, 18000, 24000

    const auto ons = h.noteOns();
    REQUIRE(ons.size() == 5);
    for (size_t i = 0; i < ons.size(); ++i)
    {
        REQUIRE(ons[i].time == kStep * static_cast<long long>(i));
        REQUIRE(ons[i].event.note == 60);
        REQUIRE(ons[i].event.velocity == 1.0f);
        REQUIRE(ons[i].event.midiChannel == 1);
    }
}

TEST_CASE("Arpeggiator: step length per division at 120 BPM and 48 kHz", "[perf][arp]")
{
    // Quarter = 24000 samples; 1/8 = 12000; 1/8T = 24000/3 = 8000; 1/16 = 6000;
    // 1/16T = 24000/6 = 4000; 1/32 = 3000.
    Harness h;
    Arpeggiator::Params p = defaultParams();
    struct Case { Arpeggiator::SyncDivision division; double samples; };
    const Case cases[] = {
        { Arpeggiator::SyncDivision::Quarter, 24000.0 },
        { Arpeggiator::SyncDivision::Eighth, 12000.0 },
        { Arpeggiator::SyncDivision::EighthTriplet, 8000.0 },
        { Arpeggiator::SyncDivision::Sixteenth, 6000.0 },
        { Arpeggiator::SyncDivision::SixteenthTriplet, 4000.0 },
        { Arpeggiator::SyncDivision::ThirtySecond, 3000.0 },
    };
    for (const Case& c : cases)
    {
        p.syncDivision = c.division;
        h.arp.setParams(p);
        REQUIRE(h.arp.stepLengthSamples(h.transport, kSampleRate) == Approx(c.samples));
    }

    // 1/8 at 120 BPM: note ons every 12000 samples.
    p.syncDivision = Arpeggiator::SyncDivision::Eighth;
    h.arp.setParams(p);
    h.queueOn(0, 60);
    h.run(60); // 28800 samples: 0, 12000, 24000
    const auto ons = h.noteOns();
    REQUIRE(ons.size() == 3);
    REQUIRE(ons[1].time == 12000);
    REQUIRE(ons[2].time == 24000);
}

TEST_CASE("Arpeggiator: grid steps at 44.1 kHz round to the sample after the grid line", "[perf][arp]")
{
    // 1/16 at 120 BPM = 5512.5 samples at 44.1 kHz. Grid lines fall at 5512.5, 11025,
    // 16537.5, 22050: a step starts on the first sample at or after the line.
    Harness h;
    h.sampleRate = 44100.0;
    h.blockSize = 441;
    h.arp.setParams(defaultParams());
    h.queueOn(0, 60);
    h.run(60); // 26460 samples

    const auto ons = h.noteOns();
    REQUIRE(ons.size() == 5);
    REQUIRE(ons[0].time == 0);
    REQUIRE(ons[1].time == 5513);
    REQUIRE(ons[2].time == 11025);
    REQUIRE(ons[3].time == 16538);
    REQUIRE(ons[4].time == 22050);
}

TEST_CASE("Arpeggiator: free rate 8 Hz steps every 6000 samples at 48 kHz", "[perf][arp]")
{
    Harness h;
    Arpeggiator::Params p = defaultParams();
    p.rateMode = Arpeggiator::RateMode::Free;
    p.freeRateHz = 8.0f;
    h.arp.setParams(p);
    h.transport.bpm = 77.0; // must be ignored in Free mode
    REQUIRE(h.arp.stepLengthSamples(h.transport, kSampleRate) == Approx(6000.0));

    h.queueOn(100, 60); // free-running counter starts at the key press
    h.run(60);
    const auto ons = h.noteOns();
    REQUIRE(ons.size() == 5); // 100, 6100, 12100, 18100, 24100 (< 28800)
    for (size_t i = 0; i < ons.size(); ++i)
        REQUIRE(ons[i].time == 100 + kStep * static_cast<long long>(i));

    // 7 Hz = 6857.142857 samples: the counter keeps its fraction, so onsets are
    // ceil(6857.14) = 6858, ceil(13714.29) = 13715, ceil(20571.43) = 20572, not 6858 * k.
    Harness h2;
    p.freeRateHz = 7.0f;
    h2.arp.setParams(p);
    h2.queueOn(0, 60);
    h2.run(60);
    const auto ons2 = h2.noteOns();
    REQUIRE(ons2.size() == 5);
    REQUIRE(ons2[1].time == 6858);
    REQUIRE(ons2[2].time == 13715);
    REQUIRE(ons2[3].time == 20572);
    REQUIRE(ons2[4].time == 27429); // ceil(27428.57)
}

TEST_CASE("Arpeggiator: Sync while the transport is stopped uses the tempo free-running", "[perf][arp]")
{
    Harness h;
    h.transport.isPlaying = false;
    h.transport.ppqPosition = 3.7; // irrelevant when stopped
    h.arp.setParams(defaultParams());
    h.queueOn(250, 60);
    h.run(60);
    const auto ons = h.noteOns();
    REQUIRE(ons.size() == 5);
    for (size_t i = 0; i < ons.size(); ++i)
        REQUIRE(ons[i].time == 250 + kStep * static_cast<long long>(i));
}

TEST_CASE("Arpeggiator: Sync steps align to the ppq grid, first step fires at the key press", "[perf][arp]")
{
    // Transport starts at ppq 0.1 with the key already pressed. The key press fires at
    // once (sample 0); the following steps sit on the 1/16 grid: ppq 0.25, 0.5, 0.75, 1.0,
    // 1.25 -> (0.25 - 0.1) * 24000 = 3600, (0.5 - 0.1) * 24000 = 9600, 15600, 21600, 27600.
    Harness h;
    h.transport.ppqPosition = 0.1;
    h.arp.setParams(defaultParams());
    h.queueOn(0, 60);
    h.run(60); // 28800 samples -> ppq 0.1 .. 1.3

    const auto ons = h.noteOns();
    REQUIRE(ons.size() == 6);
    REQUIRE(ons[0].time == 0);
    REQUIRE(ons[1].time == 3600);
    REQUIRE(ons[2].time == 9600);
    REQUIRE(ons[3].time == 15600);
    REQUIRE(ons[4].time == 21600);
    REQUIRE(ons[5].time == 27600);
}

TEST_CASE("Arpeggiator: a transport jump resynchronises to the grid", "[perf][arp]")
{
    Harness h;
    h.arp.setParams(defaultParams());
    h.queueOn(0, 60);
    h.run(30); // 14400 samples: ons at 0, 6000, 12000; ppq now 0.6

    // Loop back to the start: ppq 0 is a grid line, so a step fires at the loop point
    // (sample 14400) and then every 6000 samples.
    h.transport.ppqPosition = 0.0;
    h.run(30);

    const auto ons = h.noteOns();
    REQUIRE(ons.size() == 6);
    REQUIRE(ons[3].time == 14400);
    REQUIRE(ons[4].time == 20400);
    REQUIRE(ons[5].time == 26400);
}

// ---------------------------------------------------------------------------------------
// Patterns
// ---------------------------------------------------------------------------------------

TEST_CASE("Arpeggiator: Up pattern for a held C-E-G over 1 and 2 octaves", "[perf][arp]")
{
    Arpeggiator::Params p = defaultParams();
    p.pattern = Arpeggiator::Pattern::Up;

    Harness h1;
    h1.arp.setParams(p);
    holdCMajor(h1);
    h1.run(100); // 48000 samples = 8 steps
    REQUIRE(h1.noteOnPitches(8) == std::vector<int> { 60, 64, 67, 60, 64, 67, 60, 64 });

    Harness h2;
    p.octaves = 2;
    h2.arp.setParams(p);
    holdCMajor(h2);
    h2.run(100);
    REQUIRE(h2.noteOnPitches(8) == std::vector<int> { 60, 64, 67, 72, 76, 79, 60, 64 });
}

TEST_CASE("Arpeggiator: Down pattern for a held C-E-G over 1 and 2 octaves", "[perf][arp]")
{
    Arpeggiator::Params p = defaultParams();
    p.pattern = Arpeggiator::Pattern::Down;

    Harness h1;
    h1.arp.setParams(p);
    holdCMajor(h1);
    h1.run(100);
    REQUIRE(h1.noteOnPitches(7) == std::vector<int> { 67, 64, 60, 67, 64, 60, 67 });

    Harness h2;
    p.octaves = 2;
    h2.arp.setParams(p);
    holdCMajor(h2);
    h2.run(100);
    REQUIRE(h2.noteOnPitches(8) == std::vector<int> { 79, 76, 72, 67, 64, 60, 79, 76 });
}

TEST_CASE("Arpeggiator: UpDown pattern does not repeat the top and bottom notes", "[perf][arp]")
{
    Arpeggiator::Params p = defaultParams();
    p.pattern = Arpeggiator::Pattern::UpDown;

    // 1 octave: C E G E | C E G E ... (period 4)
    Harness h1;
    h1.arp.setParams(p);
    holdCMajor(h1);
    h1.run(120); // 57600 samples = 9.6 steps
    REQUIRE(h1.noteOnPitches(9) == std::vector<int> { 60, 64, 67, 64, 60, 64, 67, 64, 60 });

    // 2 octaves: C E G C' E' G' E' C' G E | C ... (period 10)
    Harness h2;
    p.octaves = 2;
    h2.arp.setParams(p);
    holdCMajor(h2);
    h2.run(150); // 72000 samples = 12 steps
    REQUIRE(h2.noteOnPitches(12) == std::vector<int> { 60, 64, 67, 72, 76, 79, 76, 72, 67, 64, 60, 64 });

    // Single note: just repeats. Two notes: alternates.
    Harness h3;
    p.octaves = 1;
    h3.arp.setParams(p);
    h3.queueOn(0, 60);
    h3.run(40);
    REQUIRE(h3.noteOnPitches(3) == std::vector<int> { 60, 60, 60 });

    Harness h4;
    h4.arp.setParams(p);
    h4.queueOn(0, 60);
    h4.queueOn(0, 67);
    h4.run(40);
    REQUIRE(h4.noteOnPitches(3) == std::vector<int> { 60, 67, 60 });
}

TEST_CASE("Arpeggiator: AsPlayed follows the note-on order", "[perf][arp]")
{
    Arpeggiator::Params p = defaultParams();
    p.pattern = Arpeggiator::Pattern::AsPlayed;

    // Keys pressed in the order G, C, E (all before the first step fires).
    Harness h1;
    h1.arp.setParams(p);
    h1.queueOn(0, 67);
    h1.queueOn(0, 60);
    h1.queueOn(0, 64);
    h1.run(100);
    REQUIRE(h1.noteOnPitches(7) == std::vector<int> { 67, 60, 64, 67, 60, 64, 67 });

    Harness h2;
    p.octaves = 2;
    h2.arp.setParams(p);
    h2.queueOn(0, 67);
    h2.queueOn(0, 60);
    h2.queueOn(0, 64);
    h2.run(100);
    REQUIRE(h2.noteOnPitches(7) == std::vector<int> { 67, 60, 64, 79, 72, 76, 67 });
}

TEST_CASE("Arpeggiator: Random is deterministic for a seed", "[perf][arp]")
{
    Arpeggiator::Params p = defaultParams();
    p.pattern = Arpeggiator::Pattern::Random;
    p.octaves = 2;

    auto runSeed = [&](uint32_t seed)
    {
        Harness h;
        h.arp.setParams(p);
        h.arp.seed(seed);
        holdCMajor(h);
        h.run(400); // 192000 samples = 32 steps
        return h.noteOnPitches(32);
    };

    const std::vector<int> a = runSeed(42u);
    const std::vector<int> b = runSeed(42u);
    REQUIRE(a.size() == 32);
    REQUIRE(a == b);

    // Every pick comes from the 2-octave pattern.
    for (const int note : a)
        REQUIRE((note == 60 || note == 64 || note == 67 || note == 72 || note == 76 || note == 79));

    // Different seed, different order (32 draws over 6 notes cannot all coincide by chance
    // for a generator that visits more than one value).
    const std::vector<int> c = runSeed(7u);
    REQUIRE(c != a);

    // seed() on the same instance restarts the sequence.
    Harness h;
    h.arp.setParams(p);
    h.arp.seed(42u);
    holdCMajor(h);
    h.run(400);
    REQUIRE(h.noteOnPitches(32) == a);
}

TEST_CASE("Arpeggiator: octave transposition drops notes above 127", "[perf][arp]")
{
    Arpeggiator::Params p = defaultParams();
    p.octaves = 3;
    Harness h;
    h.arp.setParams(p);
    h.queueOn(0, 110); // 110, 122 fit; 134 does not
    h.run(60);
    REQUIRE(h.noteOnPitches(4) == std::vector<int> { 110, 122, 110, 122 });
}

// ---------------------------------------------------------------------------------------
// Gate
// ---------------------------------------------------------------------------------------

TEST_CASE("Arpeggiator: gate length is a percentage of the step", "[perf][arp]")
{
    // 1/16 at 120 BPM = 6000 samples: 50 % -> off 3000 samples after the on, 25 % -> 1500,
    // 5 % -> 300.
    struct Case { float gate; long long offset; };
    const Case cases[] = { { 50.0f, 3000 }, { 25.0f, 1500 }, { 5.0f, 300 } };
    for (const Case& c : cases)
    {
        Harness h;
        Arpeggiator::Params p = defaultParams();
        p.gatePercent = c.gate;
        h.arp.setParams(p);
        holdCMajor(h);
        h.run(60); // ons at 0, 6000, 12000, 18000, 24000

        const auto ons = h.noteOns();
        const auto offs = h.noteOffs();
        REQUIRE(ons.size() == 5);
        REQUIRE(offs.size() == 5);
        for (size_t i = 0; i < 5; ++i)
        {
            REQUIRE(offs[i].time == ons[i].time + c.offset);
            REQUIRE(offs[i].event.note == ons[i].event.note);
        }
    }

    // 1/8 (12000 samples) at 25 % -> 3000, so the gate follows the division too.
    Harness h;
    Arpeggiator::Params p = defaultParams();
    p.syncDivision = Arpeggiator::SyncDivision::Eighth;
    p.gatePercent = 25.0f;
    h.arp.setParams(p);
    h.queueOn(0, 60);
    h.run(60);
    REQUIRE(h.noteOffs().size() == 3);
    REQUIRE(h.noteOffs()[1].time == 12000 + 3000);
}

TEST_CASE("Arpeggiator: gate 100 % turns a note off exactly when the next one starts, off first", "[perf][arp]")
{
    Harness h;
    Arpeggiator::Params p = defaultParams();
    p.gatePercent = 100.0f;
    h.arp.setParams(p);
    holdCMajor(h);
    h.run(60);

    // Output order: on(0) ... off(6000) on(6000) off(12000) on(12000) ...
    REQUIRE(h.out.size() >= 9);
    REQUIRE(h.out[0].event.isNoteOn());
    REQUIRE(h.out[0].time == 0);
    for (size_t k = 1; k <= 4; ++k)
    {
        const AbsEvent& off = h.out[2 * k - 1];
        const AbsEvent& on = h.out[2 * k];
        REQUIRE(off.event.isNoteOff());
        REQUIRE(off.time == kStep * static_cast<long long>(k));
        REQUIRE(on.event.isNoteOn());
        REQUIRE(on.time == kStep * static_cast<long long>(k));
    }

    // Same with a single repeated note: the off of the previous instance precedes the new on.
    Harness h2;
    h2.arp.setParams(p);
    h2.queueOn(0, 60);
    h2.run(60);
    REQUIRE(h2.out.size() >= 3);
    REQUIRE(h2.out[1].event.isNoteOff());
    REQUIRE(h2.out[1].time == 6000);
    REQUIRE(h2.out[2].event.isNoteOn());
    REQUIRE(h2.out[2].time == 6000);
}

TEST_CASE("Arpeggiator: a pending note off is delivered in a later block", "[perf][arp]")
{
    // Block of 480 samples, gate 50 % of 6000 = 3000: the off of the note started at sample 0
    // lands in block 6 (samples 2880..3359) at offset 120.
    Harness h;
    h.arp.setParams(defaultParams());
    h.queueOn(0, 60);
    h.run(6);
    REQUIRE(h.noteOffs().empty());
    REQUIRE(h.arp.numPendingNoteOffs() == 1);
    h.run(1);
    REQUIRE(h.noteOffs().size() == 1);
    REQUIRE(h.noteOffs()[0].time == 3000);
    REQUIRE(h.noteOffs()[0].event.sampleOffset == 120);
    REQUIRE(h.arp.numPendingNoteOffs() == 0);
}

// ---------------------------------------------------------------------------------------
// Held notes, release, hold
// ---------------------------------------------------------------------------------------

TEST_CASE("Arpeggiator: a released key leaves the pattern at the next step", "[perf][arp]")
{
    // Up over C-E-G: 0:C, 6000:E. E is released at 6100 while it sounds: its gate still ends
    // at 9000 (50 % of 6000) and it never plays again. The pattern continues from E, the
    // last note played, over C and G: the note after E is G, then C, G, C, G.
    Harness h;
    h.arp.setParams(defaultParams());
    holdCMajor(h);
    h.queueOff(6100, 64);
    h.run(80); // 38400 samples: steps at 0, 6000, 12000, 18000, 24000, 30000, 36000

    REQUIRE(h.noteOns().size() == 7);
    REQUIRE(h.noteOnPitches(7) == std::vector<int> { 60, 64, 67, 60, 67, 60, 67 });

    bool foundEOff = false;
    for (const AbsEvent& e : h.noteOffs())
        if (e.event.note == 64)
        {
            REQUIRE(e.time == 9000);
            foundEOff = true;
        }
    REQUIRE(foundEOff);
    REQUIRE(h.arp.numHeldNotes() == 2);
}

TEST_CASE("Arpeggiator: a chord change continues after the last note played", "[perf][arp]")
{
    // Up over C-E-G at 1/16: 0:C, 6000:E. C (below the position) is released at 6100:
    // the note after E among E-G is G at 12000, then E, G.
    {
        Harness h;
        h.arp.setParams(defaultParams());
        holdCMajor(h);
        h.queueOff(6100, 60);
        h.run(60); // steps at 0, 6000, 12000, 18000, 24000
        REQUIRE(h.noteOnPitches(5) == std::vector<int> { 60, 64, 67, 64, 67 });
        REQUIRE(h.noteOns()[2].time == 12000);
    }

    // A3 (57) joins below at sample 100, after C played at 0: the note after C among
    // A3-C-E-G is E at 6000, then G, then the wrap to the lowest note A3, then C.
    {
        Harness h;
        h.arp.setParams(defaultParams());
        holdCMajor(h);
        h.queueOn(100, 57);
        h.run(60);
        REQUIRE(h.noteOnPitches(5) == std::vector<int> { 60, 64, 67, 57, 60 });
    }

    // Down over C-E-G: 0:G, 6000:E. G (above the position) is released at 6100: the note
    // after E going down is C, then the wrap to the top, now E.
    {
        Harness h;
        Arpeggiator::Params p = defaultParams();
        p.pattern = Arpeggiator::Pattern::Down;
        h.arp.setParams(p);
        holdCMajor(h);
        h.queueOff(6100, 67);
        h.run(60);
        REQUIRE(h.noteOnPitches(5) == std::vector<int> { 67, 64, 60, 64, 60 });
    }

    // UpDown over C-E-G: 0:C, 6000:E, 12000:G (the top: the walk turns down). G is
    // released at 12100: going down from G the next note is E, then C (the bottom: turn
    // up), then E, C.
    {
        Harness h;
        Arpeggiator::Params p = defaultParams();
        p.pattern = Arpeggiator::Pattern::UpDown;
        h.arp.setParams(p);
        holdCMajor(h);
        h.queueOff(12100, 67);
        h.run(80); // steps at 0 .. 36000
        REQUIRE(h.noteOnPitches(7) == std::vector<int> { 60, 64, 67, 64, 60, 64, 60 });
    }
}

TEST_CASE("Arpeggiator: releasing every key stops the pattern after the sounding note's gate", "[perf][arp]")
{
    Harness h;
    h.arp.setParams(defaultParams());
    h.queueOn(0, 60);
    h.queueOff(6200, 60); // the note started at 6000 keeps sounding until 9000
    h.run(60);

    const auto ons = h.noteOns();
    REQUIRE(ons.size() == 2);
    REQUIRE(ons[1].time == 6000);
    const auto offs = h.noteOffs();
    REQUIRE(offs.size() == 2);
    REQUIRE(offs[1].time == 9000);
    REQUIRE(h.arp.numHeldNotes() == 0);
    REQUIRE(h.arp.numPendingNoteOffs() == 0);

    // A new key later restarts the pattern immediately at the key press (sample 40000, off
    // the grid); the following step falls on the next grid line, 42000 (ppq 1.75).
    h.queueOn(40000, 62);
    h.run(40); // up to 48000
    const auto ons2 = h.noteOns();
    REQUIRE(ons2.size() == 4);
    REQUIRE(ons2[2].time == 40000);
    REQUIRE(ons2[2].event.note == 62);
    REQUIRE(ons2[3].time == 42000);
}

TEST_CASE("Arpeggiator: hold keeps released keys until all keys are up and a new key arrives", "[perf][arp]")
{
    Harness h;
    Arpeggiator::Params p = defaultParams();
    p.hold = true;
    h.arp.setParams(p);
    holdCMajor(h);
    h.queueOff(100, 60);
    h.queueOff(101, 64);
    h.queueOff(102, 67);
    h.run(40); // 19200 samples: steps at 0, 6000, 12000, 18000 keep cycling C E G C

    REQUIRE(h.noteOnPitches(4) == std::vector<int> { 60, 64, 67, 60 });
    REQUIRE(h.arp.numHeldNotes() == 3);

    // Every key is up: a new key replaces the latched chord at the next step (24000) and
    // stays latched after its own release.
    h.queueOn(20000, 62);
    h.queueOff(20500, 62);
    h.run(60); // up to 48000: steps at 24000, 30000, 36000, 42000 play D
    const auto ons = h.noteOns();
    REQUIRE(ons.size() == 8);
    for (size_t i = 4; i < 8; ++i)
    {
        REQUIRE(ons[i].event.note == 62);
        REQUIRE(ons[i].time == kStep * static_cast<long long>(i));
    }
    REQUIRE(h.arp.numHeldNotes() == 1);

    // Turning hold off while every key is up empties the pattern: D's last gate ends, no more steps.
    p.hold = false;
    h.arp.setParams(p);
    h.run(50);
    REQUIRE(h.noteOns().size() == 8);
    REQUIRE(h.arp.numHeldNotes() == 0);
}

TEST_CASE("Arpeggiator: with hold, a key pressed while another is down joins the chord", "[perf][arp]")
{
    Harness h;
    Arpeggiator::Params p = defaultParams();
    p.hold = true;
    h.arp.setParams(p);
    h.queueOn(0, 60);
    h.queueOn(50, 64);  // C still down: joins
    h.queueOff(100, 60);
    h.queueOff(110, 64);
    h.run(50); // steps at 0 (C alone, E not yet down), 6000, 12000, 18000
    REQUIRE(h.arp.numHeldNotes() == 2);
    REQUIRE(h.noteOnPitches(4) == std::vector<int> { 60, 64, 60, 64 });
}

// ---------------------------------------------------------------------------------------
// Disabled: pass-through
// ---------------------------------------------------------------------------------------

TEST_CASE("Arpeggiator: disabled, input events pass through unchanged", "[perf][arp]")
{
    Arpeggiator arp;
    Arpeggiator::Params p = defaultParams();
    p.enabled = false;
    arp.setParams(p);

    const std::vector<NoteEvent> input = {
        NoteEvent::on(60, 0.8f, 1, 0),
        NoteEvent::on(64, 0.5f, 2, 17),
        NoteEvent::off(60, 1, 200),
        NoteEvent::on(71, 1.0f, 16, 479),
        NoteEvent::off(64, 2, 479),
    };
    TransportInfo transport;
    transport.isPlaying = true;
    NoteEventBuffer out;
    arp.process(transport, kSampleRate, 480, input, out);

    REQUIRE(out.size() == static_cast<int>(input.size()));
    for (size_t i = 0; i < input.size(); ++i)
    {
        const NoteEvent& a = input[i];
        const NoteEvent& b = out[static_cast<int>(i)];
        REQUIRE(a.type == b.type);
        REQUIRE(a.note == b.note);
        REQUIRE(a.velocity == b.velocity);
        REQUIRE(a.midiChannel == b.midiChannel);
        REQUIRE(a.sampleOffset == b.sampleOffset);
    }

    // Nothing is generated later either.
    arp.process(transport, kSampleRate, 480, {}, out);
    REQUIRE(out.empty());
    REQUIRE(arp.numHeldNotes() == 0);
}

TEST_CASE("Arpeggiator: switching off turns pending arp notes off and passes input through", "[perf][arp]")
{
    Harness h;
    Arpeggiator::Params p = defaultParams();
    p.gatePercent = 100.0f;
    h.arp.setParams(p);
    holdCMajor(h);
    h.run(2); // C sounding (gate ends at 6000), keys still down
    REQUIRE(h.arp.numPendingNoteOffs() == 1);

    p.enabled = false;
    h.arp.setParams(p);
    h.queueOff(960, 60); // the physical key release arrives in the same block
    h.run(1);

    // Block 2 output: off(60) at offset 0 from the flush, then the pass-through off(60).
    REQUIRE(h.out.size() == 3);
    REQUIRE(h.out[1].event.isNoteOff());
    REQUIRE(h.out[1].event.note == 60);
    REQUIRE(h.out[1].time == 960);
    REQUIRE(h.out[2].event.isNoteOff());
    REQUIRE(h.out[2].time == 960);
    REQUIRE(h.arp.numPendingNoteOffs() == 0);
    REQUIRE(h.arp.numHeldNotes() == 0);

    // Re-enabling starts from scratch: the notes still physically down were forgotten and
    // their later releases pass through untouched.
    p.enabled = true;
    h.arp.setParams(p);
    h.queueOff(1500, 64);
    h.run(2);
    REQUIRE(h.out.size() == 4);
    REQUIRE(h.out[3].event.isNoteOff());
    REQUIRE(h.out[3].event.note == 64);
    REQUIRE(h.out[3].time == 1500);
}

TEST_CASE("Arpeggiator: note offs for keys it does not hold pass through", "[perf][arp]")
{
    Harness h;
    h.arp.setParams(defaultParams());
    h.queueOff(10, 60);
    h.run(1);
    REQUIRE(h.out.size() == 1);
    REQUIRE(h.out[0].event.isNoteOff());
    REQUIRE(h.out[0].event.note == 60);
    REQUIRE(h.out[0].time == 10);
}

TEST_CASE("Arpeggiator: output events carry the velocity and MIDI channel of the held key", "[perf][arp]")
{
    Harness h;
    Arpeggiator::Params p = defaultParams();
    p.pattern = Arpeggiator::Pattern::AsPlayed;
    h.arp.setParams(p);
    h.queueOn(0, 60, 0.25f, 3);
    h.queueOn(0, 64, 0.75f, 5);
    h.run(30);

    const auto ons = h.noteOns();
    REQUIRE(ons.size() == 3);
    REQUIRE(ons[0].event.velocity == 0.25f);
    REQUIRE(ons[0].event.midiChannel == 3);
    REQUIRE(ons[1].event.velocity == 0.75f);
    REQUIRE(ons[1].event.midiChannel == 5);
    const auto offs = h.noteOffs();
    REQUIRE(offs.size() == 2);
    REQUIRE(offs[0].event.midiChannel == 3);
    REQUIRE(offs[1].event.midiChannel == 5);
}

TEST_CASE("Arpeggiator: parameters are clamped to their documented ranges", "[perf][arp]")
{
    Arpeggiator arp;
    Arpeggiator::Params p = defaultParams();
    p.octaves = 9;
    p.freeRateHz = 0.01f;
    p.gatePercent = 250.0f;
    arp.setParams(p);
    REQUIRE(arp.params().octaves == 4);
    REQUIRE(arp.params().freeRateHz == 0.5f);
    REQUIRE(arp.params().gatePercent == 100.0f);
    arp.setOctaves(0);
    arp.setFreeRateHz(500.0f);
    arp.setGatePercent(-3.0f);
    REQUIRE(arp.params().octaves == 1);
    REQUIRE(arp.params().freeRateHz == 50.0f);
    REQUIRE(arp.params().gatePercent == 5.0f);
}

TEST_CASE("Arpeggiator: the output buffer has a fixed capacity", "[perf][arp]")
{
    Arpeggiator arp;
    Arpeggiator::Params p = defaultParams();
    p.enabled = false;
    arp.setParams(p);
    std::vector<NoteEvent> input;
    for (int i = 0; i < kMaxEventsPerBlock + 10; ++i)
        input.push_back(NoteEvent::on(i, 1.0f, 1, i));
    TransportInfo transport;
    NoteEventBuffer out;
    arp.process(transport, kSampleRate, 512, input, out);
    REQUIRE(out.size() == kMaxEventsPerBlock);
    REQUIRE(out.full());
}

TEST_CASE("Arpeggiator: a note off that does not fit in the output buffer is sent in the next block", "[perf][arp]")
{
    // C sounds (gate 100 %, off pending). The arp is switched off and 64 note offs
    // (notes 0..63) arrive in the same block at 960..1023: the flushed off(60) takes one
    // of the 64 slots, so the pass-through off of note 63 waits and is sent at the start
    // of the next block (sample 1440).
    Harness h;
    Arpeggiator::Params p = defaultParams();
    p.gatePercent = 100.0f;
    h.arp.setParams(p);
    holdCMajor(h);
    h.run(2);
    REQUIRE(h.arp.numPendingNoteOffs() == 1);

    p.enabled = false;
    h.arp.setParams(p);
    for (int i = 0; i < kMaxEventsPerBlock; ++i)
        h.queueOff(960 + i, i);
    h.run(1);
    REQUIRE(h.out.size() == 1 + static_cast<size_t>(kMaxEventsPerBlock));
    REQUIRE(h.out[1].event.note == 60); // the flush comes first
    REQUIRE(h.out[1].time == 960);
    REQUIRE(h.out.back().event.note == 62);
    REQUIRE(h.arp.numPendingNoteOffs() == 1);

    h.run(1);
    REQUIRE(h.out.size() == 2 + static_cast<size_t>(kMaxEventsPerBlock));
    REQUIRE(h.out.back().event.isNoteOff());
    REQUIRE(h.out.back().event.note == 63);
    REQUIRE(h.out.back().time == 1440);
    REQUIRE(h.out.back().event.sampleOffset == 0);
    REQUIRE(h.arp.numPendingNoteOffs() == 0);
}

// ---------------------------------------------------------------------------------------
// Timing edge cases
// ---------------------------------------------------------------------------------------

TEST_CASE("Arpeggiator: gate 100 % with a fractional step never ends a note after the next one starts", "[perf][arp]")
{
    // 1/16 at 120 BPM and 44.1 kHz = 5512.5 samples: onsets 0, 5513, 11025, 16538, 22050
    // (5513 and 5512 samples apart). Gate 100 % = llround(5512.5) = 5513 samples, which
    // would end the 5513 note at 11026, after the 11025 onset: every note ends exactly at
    // the next onset instead, off first.
    Harness h;
    h.sampleRate = 44100.0;
    h.blockSize = 441;
    Arpeggiator::Params p = defaultParams();
    p.gatePercent = 100.0f;
    h.arp.setParams(p);
    holdCMajor(h);
    h.run(60); // 26460 samples

    const long long onsets[] = { 0, 5513, 11025, 16538, 22050 };
    const int pitches[] = { 60, 64, 67, 60, 64 };
    REQUIRE(h.out.size() == 9);
    REQUIRE(h.out[0].event.isNoteOn());
    REQUIRE(h.out[0].time == 0);
    for (size_t k = 1; k < 5; ++k)
    {
        const AbsEvent& off = h.out[2 * k - 1];
        const AbsEvent& on = h.out[2 * k];
        REQUIRE(off.event.isNoteOff());
        REQUIRE(off.event.note == pitches[k - 1]);
        REQUIRE(off.time == onsets[k]);
        REQUIRE(on.event.isNoteOn());
        REQUIRE(on.event.note == pitches[k]);
        REQUIRE(on.time == onsets[k]);
    }
}

TEST_CASE("Arpeggiator: a grid line after a block's last sample fires on the next block's first sample", "[perf][arp]")
{
    // 44.1 kHz, blocks of 5513 samples. The 1/16 line at 5512.5 lies after sample 5512,
    // the last of block 0: it fires at 5513, sample 0 of block 1. The next lines fall at
    // 11025, 16538 (16537.5), 22050 and 27563 (27562.5).
    Harness h;
    h.sampleRate = 44100.0;
    h.blockSize = 5513;
    h.arp.setParams(defaultParams());
    h.queueOn(0, 60);
    h.run(5); // 27565 samples

    const auto ons = h.noteOns();
    REQUIRE(ons.size() == 6);
    const long long expected[] = { 0, 5513, 11025, 16538, 22050, 27563 };
    for (size_t i = 0; i < ons.size(); ++i)
        REQUIRE(ons[i].time == expected[i]);
}

TEST_CASE("Arpeggiator: a short forward locate skips the grid lines it passes over", "[perf][arp]")
{
    // 1/16 at 120 BPM, 480-sample blocks. Steps at 0 (C) and 6000 (E). At sample 6240
    // (ppq 0.26) the transport jumps to ppq 0.635, over the 0.5 line. The next step is on
    // the 0.75 line: 6240 + (0.75 - 0.635) * 24000 = 9000, then 15000 and 21000. The E
    // started at 6000 keeps its full 3000-sample gate (off at 9000, before the new on).
    Harness h;
    h.arp.setParams(defaultParams());
    holdCMajor(h);
    h.run(13); // 6240 samples, ppq 0.26
    h.transport.ppqPosition = 0.635;
    h.run(40); // up to 25440

    const auto ons = h.noteOns();
    REQUIRE(ons.size() == 5);
    const long long expected[] = { 0, 6000, 9000, 15000, 21000 };
    for (size_t i = 0; i < ons.size(); ++i)
        REQUIRE(ons[i].time == expected[i]);
    REQUIRE(h.noteOnPitches(5) == std::vector<int> { 60, 64, 67, 60, 64 });

    bool foundEOff = false;
    for (size_t i = 0; i < h.out.size(); ++i)
        if (h.out[i].event.isNoteOff() && h.out[i].event.note == 64 && h.out[i].time < 12000)
        {
            REQUIRE(h.out[i].time == 9000);
            REQUIRE(h.out[i + 1].event.isNoteOn());
            REQUIRE(h.out[i + 1].time == 9000);
            foundEOff = true;
        }
    REQUIRE(foundEOff);
}

TEST_CASE("Arpeggiator: a non-finite transport falls back to 120 BPM free-running", "[perf][arp]")
{
    // Sync, stopped, bpm NaN: the step length is that of 120 BPM, 6000 samples.
    {
        Harness h;
        h.transport.isPlaying = false;
        h.transport.bpm = std::numeric_limits<double>::quiet_NaN();
        h.arp.setParams(defaultParams());
        REQUIRE(h.arp.stepLengthSamples(h.transport, kSampleRate) == Approx(6000.0));
        h.queueOn(250, 60);
        h.run(60);
        const auto ons = h.noteOns();
        REQUIRE(ons.size() == 5);
        for (size_t i = 0; i < ons.size(); ++i)
            REQUIRE(ons[i].time == 250 + kStep * static_cast<long long>(i));
    }

    // Sync, playing, ppq NaN (or infinite, with a negative infinite bpm): no grid can be
    // placed, so the steps run free at the tempo from the key press.
    const double badPpq[] = { std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() };
    for (const double ppq : badPpq)
    {
        Harness h;
        h.transport.ppqPosition = ppq;
        if (std::isinf(ppq))
            h.transport.bpm = -std::numeric_limits<double>::infinity();
        h.arp.setParams(defaultParams());
        h.queueOn(100, 60);
        h.run(60);
        const auto ons = h.noteOns();
        REQUIRE(ons.size() == 5);
        for (size_t i = 0; i < ons.size(); ++i)
            REQUIRE(ons[i].time == 100 + kStep * static_cast<long long>(i));
    }

    // A NaN sample rate counts as 48 kHz.
    {
        Harness h;
        h.transport.isPlaying = false;
        h.sampleRate = std::numeric_limits<double>::quiet_NaN();
        h.arp.setParams(defaultParams());
        h.queueOn(0, 60);
        h.run(30); // 14400 samples: 0, 6000, 12000
        REQUIRE(h.noteOns().size() == 3);
        REQUIRE(h.noteOns()[2].time == 12000);
    }

    // The grid comes back when ppq is finite again: NaN for blocks 13..24 (samples 6240 to
    // 12000; the free-running rhythm from 6000 would put the next step at 12000), then the
    // transport reports ppq 0.5, a grid line, at sample 12000: steps at 12000 and 18000.
    {
        Harness h;
        h.arp.setParams(defaultParams());
        h.queueOn(0, 60);
        h.run(13); // 0, 6000
        h.transport.ppqPosition = std::numeric_limits<double>::quiet_NaN();
        h.run(12); // up to 12000: free-running from 6000 -> no further step before 12000
        h.transport.ppqPosition = 0.5;
        h.run(20); // up to 21600
        const auto ons = h.noteOns();
        REQUIRE(ons.size() == 4);
        REQUIRE(ons[1].time == 6000);
        REQUIRE(ons[2].time == 12000); // ppq 0.5 is a grid line: fires on the jump point
        REQUIRE(ons[3].time == 18000);
    }
}

TEST_CASE("Arpeggiator: reset and a transport start replay the same Random sequence", "[perf][arp]")
{
    Arpeggiator::Params p = defaultParams();
    p.pattern = Arpeggiator::Pattern::Random;
    p.octaves = 2;

    // 16 steps from sample 0 (steps at 0 .. 90000), keys released at 95000.
    auto playPass = [](Harness& h)
    {
        const long long start = h.blockStart;
        h.queueOn(start, 60);
        h.queueOn(start, 64);
        h.queueOn(start, 67);
        h.queueOff(start + 95000, 60);
        h.queueOff(start + 95000, 64);
        h.queueOff(start + 95000, 67);
        const size_t before = h.noteOns().size();
        h.run(200); // 96000 samples
        std::vector<int> pitches;
        const auto ons = h.noteOns();
        for (size_t i = before; i < ons.size(); ++i)
            pitches.push_back(ons[i].event.note);
        return pitches;
    };

    Harness h;
    h.arp.setParams(p);
    h.arp.seed(42u);
    const std::vector<int> first = playPass(h);
    REQUIRE(first.size() == 16);

    // reset() (e.g. before an offline render) returns to the seed.
    h.arp.reset();
    h.transport.ppqPosition = 0.0;
    REQUIRE(playPass(h) == first);

    // Stop, then start again from the top: same sequence.
    h.transport.isPlaying = false;
    h.run(10);
    h.transport.isPlaying = true;
    h.transport.ppqPosition = 0.0;
    REQUIRE(playPass(h) == first);
}
