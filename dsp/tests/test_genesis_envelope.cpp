// YM2612 envelope generator timing, measured on the register-level core.
// Reference counts: docs/research/genesis.md "Documented durations for verification"
// (simulated table) and "Reference values" 19-21b. Tolerance for milliseconds: 0.05 ms or
// 0.1 %, whichever is larger (the doc rounds to 0.1 ms / 3 digits); EG-cycle counts are exact.

#include "chipdsp/genesis/Ym2612Core.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace chipdsp;
using namespace chipdsp::genesis;
using Catch::Approx;

namespace
{
    void setFrequency(Ym2612Core& ym, int ch, int block, int fnum)
    {
        const int bank = ch / 3;
        const int cc = ch % 3;
        ym.write(bank, static_cast<uint8_t>(0xA4 + cc), static_cast<uint8_t>((block << 3) | (fnum >> 8)));
        ym.write(bank, static_cast<uint8_t>(0xA0 + cc), static_cast<uint8_t>(fnum & 0xFF));
    }

    void writeOp(Ym2612Core& ym, int ch, int slot, int base, int value)
    {
        ym.write(ch / 3, static_cast<uint8_t>(base + kOpRegOffset[ch % 3][slot]), static_cast<uint8_t>(value));
    }

    struct EgSetup
    {
        int ar = 31, dr = 0, sr = 0, rr = 15, sl = 0, block = 0, fnum = 1;
    };

    // Channel 1, operator S1 only, RS 0.
    void configure(Ym2612Core& ym, const EgSetup& s)
    {
        setFrequency(ym, 0, s.block, s.fnum);
        writeOp(ym, 0, 0, 0x50, s.ar);
        writeOp(ym, 0, 0, 0x60, s.dr);
        writeOp(ym, 0, 0, 0x70, s.sr);
        writeOp(ym, 0, 0, 0x80, (s.sl << 4) | s.rr);
    }

    // Run until pred(att) holds after an EG cycle; returns the number of EG cycles (-1 on timeout).
    template <typename Pred>
    long egCyclesUntil(Ym2612Core& ym, Pred pred, long maxCycles)
    {
        long cycles = 0;
        int lastCounter = ym.egCounter();
        while (cycles < maxCycles)
        {
            ym.clockSample();
            if (ym.egCounter() != lastCounter)
            {
                lastCounter = ym.egCounter();
                ++cycles;
                if (pred(ym.operatorAttenuation(0, 0)))
                    return cycles;
            }
        }
        return -1;
    }

    double cyclesToMs(long cycles) { return cycles * 3.0 / fmSampleRate(ClockStandard::Ntsc) * 1000.0; }

    double msTolerance(double ms) { return std::max(0.05, ms * 1e-3); }
} // namespace

TEST_CASE("Attack timing 0x3FF -> 0 for several rates", "[genesis][envelope]")
{
    // Simulated table: rate -> EG cycles / ms (NTSC). Rate = 2 * AR with key code 0 and RS 0.
    struct Case { int rate; long cycles; double ms; };
    const Case cases[] = {
        { 2, 296888, 16720.7 }, { 8, 74222, 4180.2 }, { 32, 1160, 65.3 }, { 44, 145, 8.2 },
        { 48, 73, 4.1 }, { 52, 40, 2.3 }, { 56, 21, 1.2 }, { 60, 10, 0.56 },
    };
    for (const auto& c : cases)
    {
        Ym2612Core ym;
        EgSetup s;
        s.ar = c.rate / 2;
        configure(ym, s);
        REQUIRE(ym.operatorAttenuation(0, 0) == 0x3FF);
        ym.write(0, 0x28, 0x10);
        const long cycles = egCyclesUntil(ym, [](int att) { return att == 0; }, 400000);
        INFO("rate " << c.rate);
        CHECK(cycles == c.cycles);
        CHECK(cyclesToMs(cycles) == Approx(c.ms).margin(msTolerance(c.ms)));
    }
}

TEST_CASE("Attack rates 62/63 skip the attack", "[genesis][envelope]")
{
    // ref 19: rate 63 -> 0 cycles; research "Phase transitions"
    Ym2612Core ym;
    EgSetup s;
    s.ar = 31;
    s.block = 7;   // key code 28, Rks 3: rate 63
    configure(ym, s);
    ym.write(0, 0x28, 0x10);
    CHECK(ym.operatorAttenuation(0, 0) == 0);
    CHECK(ym.operatorPhase(0, 0) == Ym2612Core::EgPhase::Attack);
}

TEST_CASE("Decay/sustain timing 0 -> 0x3FF for several rates", "[genesis][envelope]")
{
    // SL 0 moves straight to the sustain phase, so the sustain rate alone drives 0 -> 0x3FF.
    struct Case { int rate; long cycles; double ms; };
    // ref 20 (rates 63, 48, 32, 2) and the simulated table (44, 16).
    const Case cases[] = {
        { 63, 128, 7.2 }, { 48, 1023, 57.6 }, { 44, 2045, 115.2 }, { 32, 16357, 921.2 }, { 16, 261697, 14738.8 },
        { 2, 4187138, 235.8 * 1000.0 },
    };
    for (const auto& c : cases)
    {
        Ym2612Core ym;
        EgSetup s;
        s.ar = 31;
        s.sl = 0;
        if (c.rate == 63)
        {
            s.sr = 31;
            s.block = 7;
        }
        else
        {
            s.sr = c.rate / 2;
        }
        s.dr = s.sr;
        configure(ym, s);
        ym.write(0, 0x28, 0x10);
        REQUIRE(ym.operatorAttenuation(0, 0) == 0);
        const long cycles = egCyclesUntil(ym, [](int att) { return att == 0x3FF; }, c.cycles + 1000);
        INFO("rate " << c.rate);
        CHECK(cycles == c.cycles);
        CHECK(cyclesToMs(cycles) == Approx(c.ms).margin(msTolerance(c.ms)));
    }
}

TEST_CASE("Decay never overshoots the sustain level", "[genesis][envelope]")
{
    // Research "Phase transitions": decay -> sustain when att >= SL10, checked immediately, with
    // the clamp att = SL10 (Nemesis page 12). SL10 = SL << 5 (ref 22). Increments of 4 and 8
    // (rates 54 and 58) do not land on 32, 64 or 160 exactly, so a late check would overshoot.
    struct Case { int dr, sl; };
    const Case cases[] = { { 27, 1 }, { 29, 2 }, { 29, 5 }, { 31, 3 }, { 26, 7 } };
    for (const auto& c : cases)
    {
        Ym2612Core ym;
        EgSetup s;
        s.ar = 31;   // key code 0: rate 62, attack skipped
        s.dr = c.dr;
        s.sr = 0;    // hold in sustain
        s.sl = c.sl;
        configure(ym, s);
        ym.write(0, 0x28, 0x10);
        REQUIRE(ym.operatorAttenuation(0, 0) == 0);
        const int sl10 = c.sl << 5;
        int last = 0, peak = 0;
        bool monotonic = true;
        for (int i = 0; i < 3 * 400; ++i)
        {
            ym.clockSample();
            const int att = ym.operatorAttenuation(0, 0);
            monotonic = monotonic && att >= last;
            last = att;
            peak = std::max(peak, att);
        }
        INFO("DR " << c.dr << " SL " << c.sl);
        CHECK(peak == sl10);
        CHECK(monotonic);
        CHECK(ym.operatorPhase(0, 0) == Ym2612Core::EgPhase::Sustain);
        CHECK(ym.operatorAttenuation(0, 0) == sl10);
    }
}

TEST_CASE("Release RR 15 silences within 128 EG cycles (7.2 ms)", "[genesis][envelope]")
{
    // Ambiguities 15: plutiedev's "128 samples" are EG updates; 384 FM samples at 3 per cycle.
    Ym2612Core ym;
    EgSetup s;
    s.ar = 31;
    s.rr = 15;
    s.block = 7;
    configure(ym, s);
    ym.write(0, 0x28, 0x10);
    ym.write(0, 0x28, 0x00);
    CHECK(ym.operatorPhase(0, 0) == Ym2612Core::EgPhase::Release);
    const long cycles = egCyclesUntil(ym, [](int att) { return att == 0x3FF; }, 1000);
    CHECK(cycles == 128);
    CHECK(cyclesToMs(cycles) == Approx(7.21).margin(0.01));
}

TEST_CASE("Decay stops at the sustain level, SL 15 = 0x3E0", "[genesis][envelope]")
{
    for (int sl : { 4, 8, 15 })
    {
        Ym2612Core ym;
        EgSetup s;
        s.ar = 31;
        s.dr = 31;
        s.sr = 0;   // hold in sustain
        s.sl = sl;
        s.block = 7;
        configure(ym, s);
        ym.write(0, 0x28, 0x10);
        for (int i = 0; i < 3 * 400; ++i)
            ym.clockSample();
        const int expected = sl == 15 ? 0x3E0 : sl << 5;
        INFO("SL " << sl);
        CHECK(ym.operatorPhase(0, 0) == Ym2612Core::EgPhase::Sustain);
        CHECK(ym.operatorAttenuation(0, 0) == expected);
    }
}

TEST_CASE("Key-on does not reset the attenuation", "[genesis][envelope]")
{
    // Ambiguities 4: the attack continues from the current value (rates below 62).
    Ym2612Core ym;
    EgSetup s;
    s.ar = 31;
    s.rr = 6;   // release rate 13 * 2 + 3 = 29
    s.block = 7;
    configure(ym, s);
    ym.write(0, 0x28, 0x10);
    ym.write(0, 0x28, 0x00);
    for (int i = 0; i < 3 * 2000; ++i)
        ym.clockSample();
    const int released = ym.operatorAttenuation(0, 0);
    REQUIRE(released > 0x40);
    REQUIRE(released < 0x3FF);

    writeOp(ym, 0, 0, 0x50, 10);   // slow attack, rate 20 + 3
    ym.write(0, 0x28, 0x10);
    CHECK(ym.operatorPhase(0, 0) == Ym2612Core::EgPhase::Attack);
    CHECK(ym.operatorAttenuation(0, 0) == released);
    // Re-writing key-on while keyed has no effect.
    ym.write(0, 0x28, 0x10);
    CHECK(ym.operatorAttenuation(0, 0) == released);
}

TEST_CASE("Global EG counter is 12-bit and skips 0", "[genesis][envelope]")
{
    Ym2612Core ym;
    CHECK(ym.egCounter() == 0);
    for (int i = 0; i < 3; ++i)
        ym.clockSample();
    CHECK(ym.egCounter() == 1);
    for (int i = 0; i < 3 * 4094; ++i)
        ym.clockSample();
    CHECK(ym.egCounter() == 4095);
    for (int i = 0; i < 3; ++i)
        ym.clockSample();
    CHECK(ym.egCounter() == 1);
}
