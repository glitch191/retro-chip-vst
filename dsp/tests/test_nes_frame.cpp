// Frame sequencer, envelope and length counter timing (research "Frame counter",
// "Envelope generator", "Length counter", items 9-11, 26, 27, 44-48, ambiguities A12, A17).

#include "chipdsp/nes/NesApu.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace chipdsp::nes;
using Catch::Approx;

namespace
{
    struct Counts
    {
        int quarter = 0;
        int half = 0;
    };

    Counts countOneSecond(bool pal, uint8_t value4017)
    {
        FrameSequencer fs;
        fs.setRegion(pal);
        fs.reset(value4017);
        const int cycles = static_cast<int>(pal ? kCpuHzPal : kCpuHzNtsc);
        Counts c;
        for (int i = 0; i < cycles; ++i)
        {
            const FrameClock fc = fs.clock();
            c.quarter += fc.quarter ? 1 : 0;
            c.half += fc.half ? 1 : 0;
        }
        return c;
    }

    struct Event
    {
        uint32_t cycle;
        bool quarter;
        bool half;
    };

    std::vector<Event> firstSequenceEvents(bool pal, uint8_t value4017, uint32_t cycles)
    {
        FrameSequencer fs;
        fs.setRegion(pal);
        fs.reset(value4017);
        std::vector<Event> ev;
        for (uint32_t i = 0; i < cycles; ++i)
        {
            const FrameClock fc = fs.clock();
            if (fc.quarter || fc.half)
                ev.push_back({ i, fc.quarter, fc.half });
        }
        return ev;
    }
} // namespace

TEST_CASE("NES frame sequencer clock counts per second, 4- and 5-step, NTSC and PAL", "[nes][frame]")
{
    // Items 44-47: reset with bit 7 clear at cycle 0, one second of integer-clock CPU cycles.
    Counts c = countOneSecond(false, 0x40);
    REQUIRE(c.quarter == 239);
    REQUIRE(c.half == 119);
    c = countOneSecond(false, 0x00);
    REQUIRE(c.quarter == 239);
    REQUIRE(c.half == 119);
    c = countOneSecond(true, 0x40);
    REQUIRE(c.quarter == 199);
    REQUIRE(c.half == 99);

    // 5-step entered at cycle 0 with the immediate clock of a $4017 bit-7 write (item 48).
    c = countOneSecond(false, 0xC0);
    REQUIRE(c.quarter == 193);
    REQUIRE(c.half == 97);
    c = countOneSecond(true, 0xC0);
    REQUIRE(c.quarter == 160);
    REQUIRE(c.half == 80);
}

TEST_CASE("NES frame sequencer 5-step counts without the immediate clock", "[nes][frame]")
{
    // Items 45 and 47 count the 5-step table alone (sequencer reset without clocking).
    // Emulate that by discarding the immediate event delivered on the first cycle.
    for (bool pal : { false, true })
    {
        FrameSequencer fs;
        fs.setRegion(pal);
        fs.reset(0x80);
        const int cycles = static_cast<int>(pal ? kCpuHzPal : kCpuHzNtsc);
        int quarter = 0, half = 0;
        for (int i = 0; i < cycles; ++i)
        {
            const FrameClock fc = fs.clock();
            if (i == 0)
                continue;
            quarter += fc.quarter ? 1 : 0;
            half += fc.half ? 1 : 0;
        }
        REQUIRE(quarter == (pal ? 159 : 192));
        REQUIRE(half == (pal ? 79 : 96));
    }
}

TEST_CASE("NES frame sequencer event positions in CPU cycles", "[nes][frame]")
{
    // Items 9 and 11: 4-step.
    auto ev = firstSequenceEvents(false, 0x40, 29830);
    REQUIRE(ev.size() == 4);
    const uint32_t ntsc4[4] = { 7457, 14913, 22371, 29829 };
    const bool half4[4] = { false, true, false, true };
    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(ev[static_cast<size_t>(i)].cycle == ntsc4[i]);
        REQUIRE(ev[static_cast<size_t>(i)].quarter);
        REQUIRE(ev[static_cast<size_t>(i)].half == half4[i]);
    }
    ev = firstSequenceEvents(true, 0x40, 33254);
    const uint32_t pal4[4] = { 8313, 16627, 24939, 33253 };
    REQUIRE(ev.size() == 4);
    for (int i = 0; i < 4; ++i)
        REQUIRE(ev[static_cast<size_t>(i)].cycle == pal4[i]);

    // Item 10: 5-step (no clock at 29829); the first event is the immediate clock at cycle 0.
    ev = firstSequenceEvents(false, 0x80, 37282);
    const uint32_t ntsc5[5] = { 0, 7457, 14913, 22371, 37281 };
    const bool half5[5] = { true, false, true, false, true };
    REQUIRE(ev.size() == 5);
    for (int i = 0; i < 5; ++i)
    {
        REQUIRE(ev[static_cast<size_t>(i)].cycle == ntsc5[i]);
        REQUIRE(ev[static_cast<size_t>(i)].half == half5[i]);
    }
    ev = firstSequenceEvents(true, 0x80, 41566);
    REQUIRE(ev.size() == 5);
    REQUIRE(ev[4].cycle == 41565);

    // The second sequence repeats with the documented length.
    ev = firstSequenceEvents(false, 0x40, 2 * 29830);
    REQUIRE(ev.size() == 8);
    REQUIRE(ev[4].cycle == 29830 + 7457);
}

TEST_CASE("NES frame interrupt flag: 4-step only, IRQ inhibit clears it", "[nes][frame]")
{
    FrameSequencer fs;
    fs.reset(0x00);
    for (uint32_t i = 0; i < 29828; ++i)
        fs.clock();
    REQUIRE_FALSE(fs.irqFlag());
    fs.clock(); // cycle 29828
    REQUIRE(fs.irqFlag());

    fs.reset(0x40);
    REQUIRE_FALSE(fs.irqFlag());
    for (int i = 0; i < 100000; ++i)
        fs.clock();
    REQUIRE_FALSE(fs.irqFlag());

    fs.reset(0x80);
    for (int i = 0; i < 100000; ++i)
        fs.clock();
    REQUIRE_FALSE(fs.irqFlag());
}

TEST_CASE("NES frame interrupt flag is set at cycles 29828, 29829 and 29830 (= 0) (NTSC and PAL)", "[nes][frame]")
{
    // Research "Frame counter", mode 0 table: "set" on the last two rows and on the wrap row.
    for (bool pal : { false, true })
    {
        const uint32_t length = pal ? 33254u : 29830u;
        FrameSequencer fs;
        fs.setRegion(pal);
        fs.reset(0x00);
        for (uint32_t i = 0; i < length - 2; ++i) // cycles 0 .. length - 3
            fs.clock();
        REQUIRE_FALSE(fs.irqFlag());
        // A $4015 read (clearIrq) after each cycle: the flag is set again on each of the three.
        for (int k = 0; k < 3; ++k)
        {
            fs.clock(); // cycles length - 2, length - 1, then 0 of the next sequence
            REQUIRE(fs.irqFlag());
            fs.clearIrq();
        }
        REQUIRE(fs.cycle() == 1);
        fs.clock(); // cycle 1: no longer set
        REQUIRE_FALSE(fs.irqFlag());
    }
}

TEST_CASE("NES $4017 IRQ inhibit applies at the write, the timer reset 3 cycles later (A26)", "[nes][frame]")
{
    FrameSequencer fs;
    fs.reset(0x00);
    for (uint32_t i = 0; i < 29829; ++i)
        fs.clock();
    REQUIRE(fs.irqFlag());
    fs.write(0x40); // inhibit set: "the frame interrupt flag is cleared"
    REQUIRE_FALSE(fs.irqFlag());

    // A write with inhibit set just before cycle 29828: the two cycles clocked before the delayed
    // reset do not set the flag any more.
    fs.reset(0x00);
    for (uint32_t i = 0; i < 29827; ++i)
        fs.clock();
    fs.write(0x40);
    fs.clock(); // 29827
    fs.clock(); // 29828: would set the flag without the inhibit
    REQUIRE_FALSE(fs.irqFlag());
    fs.clock(); // reset applied
    REQUIRE(fs.cycle() == 1);
    REQUIRE_FALSE(fs.irqFlag());

    // Clearing the inhibit bit does not clear the flag ("otherwise it is unaffected").
    fs.reset(0x00);
    for (uint32_t i = 0; i < 29829; ++i)
        fs.clock();
    fs.write(0x00);
    REQUIRE(fs.irqFlag());
}

TEST_CASE("NES $4017 write resets the sequencer 3 CPU cycles later", "[nes][frame]")
{
    FrameSequencer fs;
    fs.reset(0x40);
    for (int i = 0; i < 1000; ++i)
        fs.clock();
    fs.write(0xC0);
    FrameClock a = fs.clock();
    FrameClock b = fs.clock();
    REQUIRE_FALSE(a.quarter);
    REQUIRE_FALSE(b.quarter);
    FrameClock c = fs.clock(); // third cycle: reset + immediate quarter/half clock
    REQUIRE(c.quarter);
    REQUIRE(c.half);
    REQUIRE(fs.isFiveStep());
    REQUIRE(fs.cycle() == 1);
}

TEST_CASE("NES envelope decay timing: V = 15 takes 1.000015 s from 15 to 0 (NTSC 4-step)", "[nes][frame][envelope]")
{
    for (int v : { 15, 0 })
    {
        NesApu apu;
        apu.frame.reset(0x40);
        apu.write(0x4015, 0x01);
        apu.write(0x4000, static_cast<uint8_t>(0x20 | v)); // envelope mode, halt so the length never cuts
        apu.write(0x4002, 0x00);
        apu.write(0x4003, 0x09);   // restarts the envelope

        long long start = -1, end = -1;
        for (long long c = 0; c < 3000000 && end < 0; ++c)
        {
            apu.clock();
            if (start < 0 && apu.pulse1.volume() == 15 && !apu.pulse1.env().start)
                start = c;
            if (start >= 0 && apu.pulse1.volume() == 0)
                end = c;
        }
        REQUIRE(start >= 0);
        REQUIRE(end > start);
        const double seconds = static_cast<double>(end - start) / kCpuHzNtsc;
        if (v == 15)
            REQUIRE(seconds == Approx(1.000015).margin(1e-6)); // 240 quarter frames = 60 sequences exactly
        else
            REQUIRE(seconds == Approx(0.062501).margin(5e-6)); // 15 quarter frames, +-1 cycle of phase
    }

    // Loop flag: the decay wraps to 15.
    NesApu apu;
    apu.frame.reset(0x40);
    apu.write(0x4015, 0x01);
    apu.write(0x4000, 0x20);  // V = 0, loop
    apu.write(0x4003, 0x09);
    bool wrapped = false;
    uint8_t prev = 0;
    for (int c = 0; c < 300000; ++c)
    {
        apu.clock();
        const uint8_t now = apu.pulse1.volume();
        if (prev == 0 && now == 15 && c > 10000)
            wrapped = true;
        prev = now;
    }
    REQUIRE(wrapped);
}

TEST_CASE("NES length counter: index 1 lasts 254 half frames = 2.116699 s (NTSC 4-step)", "[nes][frame][length]")
{
    NesApu apu;
    apu.frame.reset(0x40);
    apu.write(0x4015, 0x01);
    apu.write(0x4000, 0x1F);   // constant volume, halt clear
    apu.write(0x4003, 0x08);   // index 1 -> 254
    REQUIRE(apu.pulse1.lengthValue() == 254);
    long long cycles = 0;
    while (apu.pulse1.lengthValue() > 0 && cycles < 5000000)
    {
        apu.clock();
        ++cycles;
    }
    REQUIRE(static_cast<double>(cycles) / kCpuHzNtsc == Approx(2.116699).margin(1e-6));

    // Halt flag stops the count.
    apu.write(0x4000, 0x3F);
    apu.write(0x4003, 0x18);   // index 3 -> 2
    for (int c = 0; c < 200000; ++c)
        apu.clock();
    REQUIRE(apu.pulse1.lengthValue() == 2);
}
