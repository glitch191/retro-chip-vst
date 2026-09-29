// Noise LFSR properties (research "Noise channel", reference items 28-29a, ambiguity A4).

#include "chipdsp/nes/NesApu.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace chipdsp::nes;

namespace
{
    int cycleLength(uint16_t start, bool mode, int limit)
    {
        uint16_t s = start;
        for (int i = 1; i <= limit; ++i)
        {
            s = noiseStep(s, mode);
            if (s == start)
                return i;
        }
        return -1;
    }
} // namespace

TEST_CASE("NES noise LFSR long mode has period 32767 from state 1 and visits every non-zero state", "[nes][noise]")
{
    REQUIRE(cycleLength(1, false, 40000) == 32767);

    std::vector<bool> seen(32768, false);
    uint16_t s = 1;
    for (int i = 0; i < 32767; ++i)
    {
        REQUIRE(s != 0);
        REQUIRE_FALSE(seen[s]);
        seen[s] = true;
        s = noiseStep(s, false);
    }
    // State 0 is a fixed point in both modes.
    REQUIRE(noiseStep(0, false) == 0);
    REQUIRE(noiseStep(0, true) == 0);
}

TEST_CASE("NES noise LFSR short mode: 93-step cycles and the single 31-step cycle", "[nes][noise]")
{
    REQUIRE(cycleLength(0x0001, true, 200) == 93);
    REQUIRE(cycleLength(0x4000, true, 200) == 93);
    REQUIRE(cycleLength(0x7FFF, true, 200) == 93);
    REQUIRE(cycleLength(0x0737, true, 200) == 31);

    // Exactly 31 of the 32767 non-zero states lie on the 31-step cycle; the rest on 93-step cycles.
    int on31 = 0, on93 = 0;
    for (int st = 1; st < 32768; ++st)
    {
        const int len = cycleLength(static_cast<uint16_t>(st), true, 100);
        if (len == 31)
            ++on31;
        else if (len == 93)
            ++on93;
    }
    REQUIRE(on31 == 31);
    REQUIRE(on93 == 352 * 93);

    // Bit-0 outputs after each mode-1 clock from $0737.
    std::string bits;
    uint16_t s = 0x0737;
    for (int i = 0; i < 31; ++i)
    {
        s = noiseStep(s, true);
        bits += (s & 1u) ? '1' : '0';
    }
    REQUIRE(bits == "1101100111000011010100100010111");

    // $0737 is reached after 14739 mode-0 clocks from 1.
    s = 1;
    for (int i = 0; i < 14739; ++i)
        s = noiseStep(s, false);
    REQUIRE(s == 0x0737);
}

TEST_CASE("NES noise LFSR first 16 states from 1", "[nes][noise]")
{
    constexpr uint16_t mode0[16] = { 0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200, 0x0100, 0x0080,
                                     0x0040, 0x0020, 0x0010, 0x0008, 0x0004, 0x0002, 0x4001, 0x6000 };
    constexpr uint16_t mode1[16] = { 0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200, 0x0100, 0x0080,
                                     0x0040, 0x4020, 0x2010, 0x1008, 0x0804, 0x0402, 0x0201, 0x4100 };
    uint16_t a = 1, b = 1;
    for (int i = 0; i < 16; ++i)
    {
        a = noiseStep(a, false);
        b = noiseStep(b, true);
        REQUIRE(a == mode0[i]);
        REQUIRE(b == mode1[i]);
        REQUIRE((a & 1u) == (i == 14 ? 1u : 0u));
    }
}

TEST_CASE("NES noise channel clocks its LFSR every period CPU cycles and outputs volume when bit 0 is clear", "[nes][noise]")
{
    NesApu apu;
    apu.write(0x4015, 0x08);
    apu.write(0x400C, 0x3A);      // halt, constant volume 10
    apu.write(0x400E, 0x04);      // long mode, period index 4 = 64 CPU cycles
    apu.write(0x400F, 0x08);      // length load
    REQUIRE(apu.noise.shiftRegister() == 1);
    REQUIRE(apu.noise.output() == 0); // bit 0 of 1 is set -> 0

    // Count CPU cycles between shift-register changes.
    std::vector<int> changes;
    uint16_t prev = apu.noise.shiftRegister();
    for (int c = 0; c < 64 * 20; ++c)
    {
        apu.clock();
        if (apu.noise.shiftRegister() != prev)
        {
            changes.push_back(c);
            prev = apu.noise.shiftRegister();
        }
    }
    REQUIRE(changes.size() >= 10);
    for (size_t i = 1; i < changes.size(); ++i)
        REQUIRE(changes[i] - changes[i - 1] == 64);
    // After the first clock the state is $4000: bit 0 clear -> constant volume 10 to the mixer.
    REQUIRE(apu.noise.output() == ((apu.noise.shiftRegister() & 1u) ? 0 : 10));

    // Short mode, PAL period table.
    apu.setRegion(true);
    apu.write(0x400E, 0x82);     // mode 1, index 2 = 14 CPU cycles on PAL
    changes.clear();
    prev = apu.noise.shiftRegister();
    for (int c = 0; c < 14 * 40; ++c)
    {
        apu.clock();
        if (apu.noise.shiftRegister() != prev)
        {
            changes.push_back(c);
            prev = apu.noise.shiftRegister();
        }
    }
    for (size_t i = 2; i < changes.size(); ++i)
        REQUIRE(changes[i] - changes[i - 1] == 14);
}
