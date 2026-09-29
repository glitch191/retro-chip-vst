// Triangle channel (research "Triangle channel", ambiguities A2, A3).

#include "chipdsp/nes/NesApu.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace chipdsp::nes;
using Catch::Approx;

namespace
{
    void runQuarterFrames(NesApu& apu, int count)
    {
        // Advance until 'count' quarter-frame events have happened (4-step mode, IRQ inhibited).
        int seen = 0;
        while (seen < count)
        {
            const uint32_t before = apu.frame.cycle();
            apu.clock();
            // Quarter events are at these CPU cycles of the sequence (research "Frame counter").
            for (const auto& s : kFrame4StepNtsc)
                if (s.cpuCycle == before && s.quarter)
                    ++seen;
        }
    }
} // namespace

TEST_CASE("NES triangle timer runs at the CPU rate: 32 * (t + 1) CPU cycles per period", "[nes][triangle]")
{
    NesApu apu;
    apu.frame.reset(0x40);
    apu.write(0x4015, 0x04);
    apu.write(0x4008, 0xFF);   // control set, reload 127: plays indefinitely
    const int t = 50;
    apu.write(0x400A, static_cast<uint8_t>(t));
    apu.write(0x400B, 0x08);
    runQuarterFrames(apu, 1); // loads the linear counter

    std::vector<int> wraps;    // cycles at which the sequence returns to step 0
    uint8_t prev = apu.triangle.sequencerStep();
    for (int c = 0; c < 32 * (t + 1) * 10; ++c)
    {
        apu.clock();
        const uint8_t s = apu.triangle.sequencerStep();
        if (s == 0 && prev == 31)
            wraps.push_back(c);
        prev = s;
    }
    REQUIRE(wraps.size() >= 8);
    for (size_t i = 1; i < wraps.size(); ++i)
        REQUIRE(wraps[i] - wraps[i - 1] == 32 * (t + 1));
}

TEST_CASE("NES triangle linear counter: control clear counts down and halts holding the step", "[nes][triangle]")
{
    NesApu apu;
    apu.frame.reset(0x40);
    apu.write(0x4015, 0x04);
    apu.write(0x4008, 0x0A);   // control clear, reload value 10
    apu.write(0x400A, 0x40);
    apu.write(0x400B, 0x08);   // sets the reload flag

    runQuarterFrames(apu, 1);
    REQUIRE(apu.triangle.linearValue() == 10); // reloaded, reload flag then cleared (control clear)
    runQuarterFrames(apu, 9);
    REQUIRE(apu.triangle.linearValue() == 1);
    runQuarterFrames(apu, 1);
    REQUIRE(apu.triangle.linearValue() == 0);
    REQUIRE_FALSE(apu.triangle.isRunning());

    // Halted: the output stays at the current step (A3), it is not forced to 0.
    const uint8_t held = apu.triangle.output();
    const uint8_t step = apu.triangle.sequencerStep();
    for (int c = 0; c < 20000; ++c)
    {
        apu.clock();
        REQUIRE(apu.triangle.sequencerStep() == step);
        REQUIRE(apu.triangle.output() == held);
    }
    REQUIRE(held == kTriangleSequence[step]);
}

TEST_CASE("NES triangle linear counter: control set reloads every quarter frame", "[nes][triangle]")
{
    NesApu apu;
    apu.frame.reset(0x40);
    apu.write(0x4015, 0x04);
    apu.write(0x4008, 0x85);   // control set, reload value 5
    apu.write(0x400A, 0x40);
    apu.write(0x400B, 0x08);
    for (int q = 0; q < 50; ++q)
    {
        runQuarterFrames(apu, 1);
        REQUIRE(apu.triangle.linearValue() == 5);
    }
    // Clearing control: one more reload, then the count-down.
    apu.write(0x4008, 0x05);
    runQuarterFrames(apu, 1);
    REQUIRE(apu.triangle.linearValue() == 5);
    runQuarterFrames(apu, 5);
    REQUIRE(apu.triangle.linearValue() == 0);
    // The length counter is halted while control is set, and counts once it is clear.
    REQUIRE(apu.triangle.lengthValue() < 254);
}

TEST_CASE("NES triangle at periods 0 and 1 reports the 7.5 average while running", "[nes][triangle]")
{
    NesApu apu;
    apu.frame.reset(0x40);
    apu.write(0x4015, 0x04);
    apu.write(0x4008, 0xFF);
    apu.write(0x400A, 0x00);
    apu.write(0x400B, 0x08);   // t = 0
    REQUIRE_FALSE(apu.triangle.isUltrasonic()); // linear counter not loaded yet
    runQuarterFrames(apu, 1);
    REQUIRE(apu.triangle.isUltrasonic());
    apu.write(0x400A, 0x01);   // t = 1
    REQUIRE(apu.triangle.isUltrasonic());
    apu.write(0x400A, 0x02);   // t = 2: normal stepping
    REQUIRE_FALSE(apu.triangle.isUltrasonic());

    NesMixer mixer;
    mixer.build();
    REQUIRE(mixer.mix(0, 0, 0, true, 0, 0) == Approx(0.133499).margin(1e-6));
    // Frequencies (item 14, 15): t = 0 -> 55930.41 Hz NTSC, 51956.47 Hz PAL.
    REQUIRE(kCpuHzNtsc / 32.0 == Approx(55930.40625));
    REQUIRE(kCpuHzPal / 32.0 == Approx(51956.46875));

    // Silencing via $4015 halts it: the held step value comes back (documented pop).
    apu.write(0x400A, 0x00);
    apu.write(0x4015, 0x00);
    REQUIRE_FALSE(apu.triangle.isUltrasonic());
    REQUIRE(apu.triangle.output() == kTriangleSequence[apu.triangle.sequencerStep()]);
}
