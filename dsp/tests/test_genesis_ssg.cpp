// SSG-EG modes, measured on the register-level core.
// docs/research/genesis.md "SSG-EG" and reference 23c: with AR 31 and the fastest decay rate
// (inc 8 x 4 = 32 per EG cycle) the attenuation reaches 0x200 after 16 EG cycles; shapes
// 8 \\\\, 9 \___, A \/\/, B \-- , C ////, D /--, E /\/\, F /___ (OPNA 2-5-2).

#include "chipdsp/genesis/Ym2612Core.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <vector>

using namespace chipdsp;
using namespace chipdsp::genesis;

namespace
{
    void writeOp(Ym2612Core& ym, int slot, int base, int value)
    {
        ym.write(0, static_cast<uint8_t>(base + kOpRegOffset[0][slot]), static_cast<uint8_t>(value));
    }

    // Channel 1, S1: AR 31, DR 31, SL 15 (decay never reaches sustain), RR 15, TL 0, block 7 (rate 63).
    Ym2612Core setup(int ssgRegister)
    {
        Ym2612Core ym;
        ym.write(0, 0xA4, 7 << 3);
        ym.write(0, 0xA0, 0x01);
        writeOp(ym, 0, 0x40, 0);
        writeOp(ym, 0, 0x50, 31);
        writeOp(ym, 0, 0x60, 31);
        writeOp(ym, 0, 0x70, 0);
        writeOp(ym, 0, 0x80, 0xFF);
        writeOp(ym, 0, 0x90, ssgRegister);
        ym.write(0, 0x28, 0x10);
        return ym;
    }

    // EG output (inversion applied) after each of 'cycles' EG cycles.
    std::vector<int> trace(Ym2612Core& ym, int cycles)
    {
        std::vector<int> out;
        int last = ym.egCounter();
        while (static_cast<int>(out.size()) < cycles)
        {
            ym.clockSample();
            if (ym.egCounter() != last)
            {
                last = ym.egCounter();
                out.push_back(ym.operatorEgOutput(0, 0));
            }
        }
        return out;
    }

    int countJumps(const std::vector<int>& v, int minJump)
    {
        int n = 0;
        for (size_t i = 1; i < v.size(); ++i)
            if (std::abs(v[i] - v[i - 1]) >= minJump)
                ++n;
        return n;
    }

    int countTurns(const std::vector<int>& v)
    {
        int n = 0;
        int dir = 0;
        for (size_t i = 1; i < v.size(); ++i)
        {
            const int d = v[i] > v[i - 1] ? 1 : v[i] < v[i - 1] ? -1 : 0;
            if (d != 0 && dir != 0 && d != dir)
                ++n;
            if (d != 0)
                dir = d;
        }
        return n;
    }
} // namespace

TEST_CASE("SSG-EG first ramp reaches 0x200 after 16 EG cycles (x4 rule)", "[genesis][ssg]")
{
    // Non-inverted modes ramp the output 0 -> 0x200, inverted modes 0x200 -> 0.
    for (int reg = 8; reg <= 15; ++reg)
    {
        Ym2612Core ym = setup(reg);
        const bool inverted = (reg & 4) != 0;
        CHECK(ym.operatorEgOutput(0, 0) == (inverted ? 0x200 : 0));   // ref 23c: att 0 -> 0x200 when inverted
        const auto t = trace(ym, 16);
        INFO("SSG register " << reg);
        for (int k = 1; k <= 16; ++k)
        {
            const int att = 32 * k;
            const int expected = inverted ? (0x200 - att) & 0x3FF : att;
            CHECK(t[static_cast<size_t>(k - 1)] == expected);
        }
        if (inverted)
            CHECK(t[7] == 0x100);   // ref 23c: att 0x100 -> 0x100
    }
}

TEST_CASE("SSG-EG mode shapes", "[genesis][ssg]")
{
    constexpr int kCycles = 120;

    SECTION("8: repeating saw (quieter each ramp, restart)")
    {
        Ym2612Core ym = setup(0x8);
        const auto t = trace(ym, kCycles);
        CHECK(countJumps(t, 0x1C0) >= 5);
        for (size_t i = 1; i < t.size(); ++i)
            if (std::abs(t[i] - t[i - 1]) < 0x1C0)
                CHECK(t[i] >= t[i - 1]);   // between restarts the attenuation only rises
    }
    SECTION("9: one ramp then hold at minimum")
    {
        Ym2612Core ym = setup(0x9);
        const auto t = trace(ym, kCycles);
        for (size_t i = 20; i < t.size(); ++i)
            CHECK(t[i] == 0x3FF);
    }
    SECTION("A: triangle starting downwards in level")
    {
        Ym2612Core ym = setup(0xA);
        const auto t = trace(ym, kCycles);
        CHECK(countJumps(t, 0x100) == 0);
        CHECK(countTurns(t) >= 5);
        for (int v : t)
            CHECK(v <= 0x200);
    }
    SECTION("B: one ramp then hold at maximum level")
    {
        Ym2612Core ym = setup(0xB);
        const auto t = trace(ym, kCycles);
        for (size_t i = 20; i < t.size(); ++i)
            CHECK(t[i] == 0);
    }
    SECTION("C: repeating inverted saw (louder each ramp, restart)")
    {
        Ym2612Core ym = setup(0xC);
        const auto t = trace(ym, kCycles);
        CHECK(countJumps(t, 0x1C0) >= 5);
        for (size_t i = 1; i < t.size(); ++i)
            if (std::abs(t[i] - t[i - 1]) < 0x1C0)
                CHECK(t[i] <= t[i - 1]);
    }
    SECTION("D: one inverted ramp then hold at maximum level")
    {
        Ym2612Core ym = setup(0xD);
        const auto t = trace(ym, kCycles);
        for (size_t i = 20; i < t.size(); ++i)
            CHECK(t[i] == 0);
    }
    SECTION("E: triangle starting upwards in level")
    {
        Ym2612Core ym = setup(0xE);
        const auto t = trace(ym, kCycles);
        CHECK(countJumps(t, 0x100) == 0);
        CHECK(countTurns(t) >= 5);
        CHECK(t[0] > t[1] - 1);   // starts near 0x200 and falls
    }
    SECTION("F: one inverted ramp then hold at minimum")
    {
        Ym2612Core ym = setup(0xF);
        const auto t = trace(ym, kCycles);
        for (size_t i = 20; i < t.size(); ++i)
            CHECK(t[i] == 0x3FF);
    }
}

TEST_CASE("SSG-EG key-off applies the inversion to the stored attenuation", "[genesis][ssg]")
{
    // research "SSG-EG": key-off inverts the stored value in place and clears the flag, so the
    // output does not jump; the release phase is never inverted.
    Ym2612Core ym = setup(0xC);
    const auto t = trace(ym, 4);   // att 128, output 0x200 - 128 = 384
    REQUIRE(t.back() == 384);
    ym.write(0, 0x28, 0x00);
    CHECK(ym.operatorPhase(0, 0) == Ym2612Core::EgPhase::Release);
    CHECK(ym.operatorAttenuation(0, 0) == 384);
    CHECK(ym.operatorEgOutput(0, 0) == 384);
}

TEST_CASE("SSG-EG disabled: plain linear decay", "[genesis][ssg]")
{
    Ym2612Core ym = setup(0x0);
    const auto t = trace(ym, 20);
    for (size_t k = 0; k < t.size(); ++k)
        CHECK(t[k] == static_cast<int>(8 * (k + 1)));
}
