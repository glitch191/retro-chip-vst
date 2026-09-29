// Pulse channel and sweep unit (research "Pulse channels", "Sweep unit", items 21-25).

#include "chipdsp/nes/NesApu.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <vector>

using namespace chipdsp::nes;

namespace
{
    // Writes the 11-bit period through $4002/$4003 (reg 2/3) and the sweep register (reg 1).
    void setup(PulseChannel& p, int period, int shift, bool negate, bool enable = false, int dividerPeriod = 0)
    {
        p.write(1, static_cast<uint8_t>((enable ? 0x80 : 0) | (dividerPeriod << 4) | (negate ? 0x08 : 0) | shift));
        p.write(2, static_cast<uint8_t>(period & 0xFF));
        p.write(3, static_cast<uint8_t>((period >> 8) & 7));
    }
} // namespace

TEST_CASE("NES sweep target period: ones' complement on pulse 1, two's complement on pulse 2", "[nes][pulse][sweep]")
{
    PulseChannel p1(true), p2(false);

    setup(p1, 0x100, 1, false);
    setup(p2, 0x100, 1, false);
    REQUIRE(p1.targetPeriod() == 0x180);
    REQUIRE(p2.targetPeriod() == 0x180);

    setup(p1, 0x100, 1, true);
    setup(p2, 0x100, 1, true);
    REQUIRE(p1.targetPeriod() == 0x07F);
    REQUIRE(p2.targetPeriod() == 0x080);

    setup(p1, 0x0AB, 2, true);
    setup(p2, 0x0AB, 2, true);
    REQUIRE(p1.targetPeriod() == 0x080);
    REQUIRE(p2.targetPeriod() == 0x081);

    // period 1, shift 0, negate: clamped to 0 (pulse 1: 1 - 2 = -1 -> 0).
    setup(p1, 0x001, 0, true);
    setup(p2, 0x001, 0, true);
    REQUIRE(p1.targetPeriod() == 0);
    REQUIRE(p2.targetPeriod() == 0);
    REQUIRE(p1.isMuted()); // period < 8
}

TEST_CASE("NES sweep mute rules apply even with the sweep disabled", "[nes][pulse][sweep]")
{
    PulseChannel p(true);

    setup(p, 0x3FF, 0, false); // target $7FE
    REQUIRE(p.targetPeriod() == 0x7FE);
    REQUIRE_FALSE(p.isMuted());

    setup(p, 0x400, 0, false); // target $800 > $7FF
    REQUIRE(p.targetPeriod() == 0x800);
    REQUIRE(p.isMuted());

    setup(p, 0x7FF, 3, false); // target $8FE
    REQUIRE(p.targetPeriod() == 0x8FE);
    REQUIRE(p.isMuted());

    setup(p, 7, 1, false);
    REQUIRE(p.isMuted());      // period < 8
    setup(p, 8, 1, false);
    REQUIRE_FALSE(p.isMuted());

    // Muting gates the mixer input only: the sequencer keeps running.
    NesApu apu;
    apu.write(0x4015, 0x01);
    apu.write(0x4000, 0xBF);   // duty 2, halt, constant volume 15
    apu.write(0x4001, 0x00);   // sweep disabled, shift 0, negate 0
    apu.write(0x4002, 0x00);
    apu.write(0x4003, 0x0C);   // period $400 -> target $800 -> muted
    const uint8_t stepBefore = apu.pulse1.sequencerStep();
    for (int i = 0; i < 20000; ++i)
    {
        apu.clock();
        REQUIRE(apu.pulse1.output() == 0);
    }
    REQUIRE(apu.pulse1.sequencerStep() != stepBefore);
    apu.write(0x4003, 0x0B);   // period $300 -> target $600 -> audible
    int nonZero = 0;
    for (int i = 0; i < 20000; ++i)
    {
        apu.clock();
        nonZero += apu.pulse1.output() == 15 ? 1 : 0;
    }
    REQUIRE(nonZero > 0);
}

TEST_CASE("NES sweep half-frame procedure updates the period in the documented order", "[nes][pulse][sweep]")
{
    PulseChannel p(false);
    setup(p, 0x100, 1, false, true, 1); // enabled, divider period P = 1 (2 half frames), shift 1

    // Clock 1: divider counter 0 and enabled -> period = target ($180); reload flag -> counter = P.
    p.clockHalf();
    REQUIRE(p.timerPeriod() == 0x180);
    // Clock 2: counter 1 -> decremented, no update.
    p.clockHalf();
    REQUIRE(p.timerPeriod() == 0x180);
    // Clock 3: counter 0 -> update to $240.
    p.clockHalf();
    REQUIRE(p.timerPeriod() == 0x240);

    // A muting target blocks the update but the divider keeps running.
    setup(p, 0x600, 1, false, true, 0); // target $900 -> muted
    p.clockHalf();
    p.clockHalf();
    REQUIRE(p.timerPeriod() == 0x600);

    // Shift 0 behaves like disabled for updates.
    setup(p, 0x100, 0, true, true, 0);
    p.clockHalf();
    p.clockHalf();
    REQUIRE(p.timerPeriod() == 0x100);

    // Negate on pulse 2 sweeps upwards in pitch: $100 -> $080 -> $040.
    setup(p, 0x100, 1, true, true, 0);
    p.clockHalf();
    REQUIRE(p.timerPeriod() == 0x080);
    p.clockHalf();
    REQUIRE(p.timerPeriod() == 0x040);
}

TEST_CASE("NES pulse timer: 8-step sequence of 2*(t+1) CPU cycles per step", "[nes][pulse]")
{
    NesApu apu;
    apu.write(0x4015, 0x01);
    apu.write(0x4000, 0x3F | (1 << 6)); // duty 1 (25 %), halt, constant volume 15
    apu.write(0x4001, 0x08);            // negate set so a low period is not muted by the target rule
    const int t = 100;
    apu.write(0x4002, static_cast<uint8_t>(t & 0xFF));
    apu.write(0x4003, static_cast<uint8_t>(0x08 | (t >> 8)));
    REQUIRE(apu.pulse1.sequencerStep() == 0); // restarted at the first value

    std::vector<int> stepChanges;
    uint8_t prev = apu.pulse1.sequencerStep();
    int high = 0;
    const int total = 16 * (t + 1) * 20;
    for (int c = 0; c < total; ++c)
    {
        apu.clock();
        if (apu.pulse1.sequencerStep() != prev)
        {
            stepChanges.push_back(c);
            prev = apu.pulse1.sequencerStep();
        }
        high += apu.pulse1.output() == 15 ? 1 : 0;
    }
    for (size_t i = 1; i < stepChanges.size(); ++i)
        REQUIRE(stepChanges[i] - stepChanges[i - 1] == 2 * (t + 1));
    // 25 % duty: two of eight steps high (within one step at the edges of the window).
    REQUIRE(std::abs(high - total / 4) <= 2 * (t + 1));

    // $4002 writes do not restart the sequencer; $4003 writes do.
    apu.write(0x4002, 0x50);
    REQUIRE(apu.pulse1.sequencerStep() == prev);
    apu.write(0x4003, 0x08);
    REQUIRE(apu.pulse1.sequencerStep() == 0);
}

TEST_CASE("NES pulse output requires a non-zero length counter", "[nes][pulse]")
{
    NesApu apu;
    apu.write(0x4000, 0xBF);
    apu.write(0x4002, 0x00);
    apu.write(0x4003, 0x09);   // channel disabled in $4015: the length counter does not load
    REQUIRE(apu.pulse1.lengthValue() == 0);
    apu.write(0x4015, 0x01);
    apu.write(0x4003, 0x09);   // index 1 -> 254
    REQUIRE(apu.pulse1.lengthValue() == 254);
    REQUIRE((apu.peekStatus() & 0x01) != 0);
    apu.write(0x4015, 0x00);   // disabling forces the length counter to 0
    REQUIRE(apu.pulse1.lengthValue() == 0);
    REQUIRE(apu.pulse1.output() == 0);
}
