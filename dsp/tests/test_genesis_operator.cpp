// YM2612 operators, algorithms, feedback, 9-bit DAC truncation, DAC and ladder output stage,
// measured on the register-level core. docs/research/genesis.md "Operator", "Algorithms",
// "DAC", "Output stage, ladder effect" and references 26-29, 33-34.

#include "chipdsp/genesis/Ym2612Core.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <vector>

using namespace chipdsp;
using namespace chipdsp::genesis;

namespace
{
    void writeOp(Ym2612Core& ym, int ch, int slot, int base, int value)
    {
        ym.write(ch / 3, static_cast<uint8_t>(base + kOpRegOffset[ch % 3][slot]), static_cast<uint8_t>(value));
    }

    // Channel 1: algorithm 'alg', every operator MUL 1, AR 31, SL 0, SR 0, TL from 'tl'.
    void patch(Ym2612Core& ym, int alg, int fb, const int (&tl)[4])
    {
        ym.write(0, 0xB0, static_cast<uint8_t>((fb << 3) | alg));
        ym.write(0, 0xA4, 4 << 3);
        ym.write(0, 0xA0, 0x80);
        for (int s = 0; s < 4; ++s)
        {
            writeOp(ym, 0, s, 0x30, 0x01);
            writeOp(ym, 0, s, 0x40, tl[s]);
            writeOp(ym, 0, s, 0x50, 31);
            writeOp(ym, 0, s, 0x60, 0);
            writeOp(ym, 0, s, 0x70, 0);
            writeOp(ym, 0, s, 0x80, 0x0F);
        }
        ym.write(0, 0x28, 0xF0);
    }

    std::vector<int> render(Ym2612Core& ym, int samples)
    {
        std::vector<int> v;
        for (int i = 0; i < samples; ++i)
        {
            ym.clockSample();
            v.push_back(ym.channelOutput(0));
        }
        return v;
    }

    int peak(const std::vector<int>& v)
    {
        int p = 0;
        for (int x : v)
            p = std::max(p, std::abs(x));
        return p;
    }
} // namespace

TEST_CASE("Algorithm routing: which operators are carriers", "[genesis][operator][algorithm]")
{
    // ref 29: 0-3 -> {S4}; 4 -> {S2, S4}; 5, 6 -> {S2, S3, S4}; 7 -> all
    const uint8_t carriers[8] = { 0b1000, 0b1000, 0b1000, 0b1000, 0b1010, 0b1110, 0b1110, 0b1111 };
    for (int alg = 0; alg < 8; ++alg)
        for (int s = 0; s < 4; ++s)
        {
            Ym2612Core ym;
            ym.setLadderEffect(false);
            int tl[4] = { 127, 127, 127, 127 };
            tl[s] = 0;
            patch(ym, alg, 0, tl);
            const int p = peak(render(ym, 1100));
            INFO("algorithm " << alg << " operator S" << s + 1);
            if ((carriers[alg] >> s) & 1)
                CHECK(p == 256);   // a lone full-scale carrier: -8168 >> 5 = -256
            else
                CHECK(p == 0);
        }
}

TEST_CASE("Algorithm routing: modulation paths", "[genesis][operator][algorithm]")
{
    // Algorithm 4: S1 -> S2 and S3 -> S4. With S4 silent, S3 must not affect the output,
    // while S1 must.
    auto run = [](int tl1, int tl3) {
        Ym2612Core ym;
        const int tl[4] = { tl1, 0, tl3, 127 };
        patch(ym, 4, 0, tl);
        return render(ym, 400);
    };
    const auto plain = run(127, 127);
    CHECK(run(127, 0) == plain);
    CHECK(run(0, 127) != plain);

    // Algorithm 0: S1 -> S2 -> S3 -> S4; muting S2 removes S1's influence entirely.
    auto chain = [](int tl1, int tl2) {
        Ym2612Core ym;
        const int tl[4] = { tl1, tl2, 0, 0 };
        patch(ym, 0, 0, tl);
        return render(ym, 400);
    };
    CHECK(chain(0, 127) == chain(127, 127));
    CHECK(chain(0, 0) != chain(127, 0));
}

TEST_CASE("Carrier sum is truncated per carrier and clamped to 9 bits", "[genesis][operator][dac]")
{
    // ref 28: single carrier peaks at +255 / -256; four in phase clamp to the same range.
    for (int alg : { 4, 7 })
    {
        Ym2612Core ym;
        const int tl[4] = { 0, 0, 0, 0 };
        patch(ym, alg, 0, tl);
        const auto v = render(ym, 2000);
        const auto [mn, mx] = std::minmax_element(v.begin(), v.end());
        INFO("algorithm " << alg);
        CHECK(*mx == 255);
        CHECK(*mn == -256);
    }
    Ym2612Core one;
    const int tl[4] = { 127, 127, 127, 0 };
    patch(one, 7, 0, tl);
    const auto v = render(one, 2000);
    const auto [mn, mx] = std::minmax_element(v.begin(), v.end());
    CHECK(*mx == 255);
    CHECK(*mn == -256);
}

TEST_CASE("S1 feedback modulates S1 only through its last two outputs", "[genesis][operator]")
{
    // With FB 0 the lone S1 carrier is a plain sine; with FB 7 it deviates once outputs exist.
    Ym2612Core a;
    Ym2612Core b;
    const int tl[4] = { 0, 127, 127, 127 };
    patch(a, 7, 0, tl);
    patch(b, 7, 7, tl);
    const auto va = render(a, 300);
    const auto vb = render(b, 300);
    // First sample: history is zero in both.
    CHECK(va[0] == vb[0]);
    CHECK(va != vb);
    CHECK(peak(vb) <= 256);

    // FB 0: S1 output equals the operator formula at the phase counter value.
    Ym2612Core c;
    patch(c, 7, 0, tl);
    for (int i = 0; i < 200; ++i)
    {
        const int phase10 = static_cast<int>(c.operatorPhaseCounter(0, 0) >> 10);
        c.clockSample();
        CHECK(c.operatorLastOutput(0, 0) == operatorOutput(phase10, c.operatorEgOutput(0, 0)));
    }
}

TEST_CASE("S1 feedback on the core follows the documented formula", "[genesis][operator]")
{
    // Research "Phase modulation input and feedback" and ref 27: S1's modulation input is
    // (out[n-1] + out[n-2]) >> (10 - FB), added to the 10-bit phase. The outputs are traced on the
    // core and each new output is recomputed from the documented formula and the log-sin/exp
    // tables (refs 24-26), not from the core's own modulation code.
    for (int fb : { 1, 4, 7 })
    {
        Ym2612Core ym;
        const int tl[4] = { 0, 127, 127, 127 };
        patch(ym, 7, fb, tl);
        int o1 = 0, o2 = 0;
        int mismatches = 0;
        bool sawLargeModulation = false;
        for (int i = 0; i < 2000; ++i)
        {
            const int phase10 = static_cast<int>(ym.operatorPhaseCounter(0, 0) >> 10);
            ym.clockSample();
            const int mod = (o1 + o2) >> (10 - fb);
            sawLargeModulation = sawLargeModulation || std::abs(mod) > 256;
            if (ym.operatorLastOutput(0, 0) != operatorOutput(phase10 + mod, ym.operatorEgOutput(0, 0)))
                ++mismatches;
            o2 = o1;
            o1 = ym.operatorLastOutput(0, 0);
        }
        INFO("FB " << fb);
        CHECK(mismatches == 0);
        if (fb == 7)
            CHECK(sawLargeModulation);   // FB 7 reaches the ref 27 range (0x3FA at 8168 + 8168)
    }
    // ref 27 itself: two full-scale outputs at FB 7.
    CHECK((((8168 + 8168) >> (10 - 7)) & 0x3FF) == 0x3FA);
}

TEST_CASE("Pipeline delay: in algorithm 0, S3 sees S2's previous-sample output", "[genesis][operator][algorithm]")
{
    // Research "Evaluation order quirk" (Nemesis page 13, jsgroth part 4): operators run in the
    // order S1, S3, S2, S4, so S3 reads S2 from the previous sample while S2 reads S1 and S4 reads
    // S3 from the current one. A modulator's contribution is its output >> 1.
    Ym2612Core ym;
    const int tl[4] = { 0, 0, 0, 0 };
    patch(ym, 0, 0, tl);
    int prevS2 = 0;
    int bad3 = 0, bad2 = 0, bad4 = 0;
    bool s2Moves = false;
    for (int i = 0; i < 1500; ++i)
    {
        uint32_t ph[4];
        for (int s = 0; s < 4; ++s)
            ph[s] = ym.operatorPhaseCounter(0, s) >> 10;
        ym.clockSample();
        const int s1 = ym.operatorLastOutput(0, 0);
        const int s2 = ym.operatorLastOutput(0, 1);
        const int s3 = ym.operatorLastOutput(0, 2);
        const int s4 = ym.operatorLastOutput(0, 3);
        if (s2 != operatorOutput(static_cast<int>(ph[1]) + (s1 >> 1), ym.operatorEgOutput(0, 1)))
            ++bad2;
        if (s3 != operatorOutput(static_cast<int>(ph[2]) + (prevS2 >> 1), ym.operatorEgOutput(0, 2)))
            ++bad3;
        if (s4 != operatorOutput(static_cast<int>(ph[3]) + (s3 >> 1), ym.operatorEgOutput(0, 3)))
            ++bad4;
        s2Moves = s2Moves || s2 != prevS2;
        prevS2 = s2;
    }
    CHECK(s2Moves);
    CHECK(bad2 == 0);
    CHECK(bad3 == 0);
    CHECK(bad4 == 0);
}

TEST_CASE("Key-on resets the phase counter, key-off does not", "[genesis][operator]")
{
    Ym2612Core ym;
    const int tl[4] = { 0, 127, 127, 127 };
    patch(ym, 7, 0, tl);
    render(ym, 100);
    REQUIRE(ym.operatorPhaseCounter(0, 0) != 0);
    ym.write(0, 0x28, 0x00);
    const uint32_t before = ym.operatorPhaseCounter(0, 0);
    CHECK(before != 0);
    ym.write(0, 0x28, 0x10);
    CHECK(ym.operatorPhaseCounter(0, 0) == 0);
}

TEST_CASE("DAC replaces channel 6 with the 9-bit DAC value", "[genesis][operator][dac]")
{
    // ref 33
    Ym2612Core ym;
    ym.setLadderEffect(false);
    ym.write(0, 0x2B, 0x80);
    struct Case { uint8_t v; int out; };
    const Case cases[] = { { 0x00, -256 }, { 0x80, 0 }, { 0xFF, 254 }, { 0x7F, -2 } };
    for (const auto& c : cases)
    {
        ym.write(0, 0x2A, c.v);
        ym.clockSample();   // the register is sampled once per FM sample
        CHECK(ym.channelOutput(5) == c.out);
        CHECK(ym.channelOutputLeft(5) == c.out);
    }
    // Only channel 6's L/R bits apply to the DAC.
    ym.write(1, 0xB6, 0x40);   // right only
    ym.write(0, 0x2A, 0xFF);
    ym.clockSample();
    CHECK(ym.channelOutputLeft(5) == 0);
    CHECK(ym.channelOutputRight(5) == 254);
    // Disabling the DAC returns channel 6 to FM (silent here).
    ym.write(0, 0x2B, 0x00);
    ym.clockSample();
    CHECK(ym.channelOutput(5) == 0);
}

TEST_CASE("Ladder effect: revision 0 vs revision 1", "[genesis][operator][dac]")
{
    // ref 34 on the core output stage
    Ym2612Core ym;
    ym.clockSample();
    // Power-on: six silent channels (sample 0), L and R enabled: +4 each with the ladder.
    CHECK(ym.outputLeft() == 24);
    CHECK(ym.outputRight() == 24);
    ym.setLadderEffect(false);
    CHECK(ym.outputLeft() == 0);

    ym.setLadderEffect(true);
    ym.write(0, 0x2B, 0x80);
    ym.write(0, 0x2A, 0x7F);   // -2
    ym.clockSample();
    CHECK(ym.channelOutputLeft(5) == -5);
    ym.write(1, 0xB6, 0x00);   // channel 6 muted on both sides
    ym.clockSample();
    CHECK(ym.channelOutputLeft(5) == -4);
    CHECK(ym.channelOutputRight(5) == -4);
    ym.write(0, 0x2A, 0xC0);   // +128, muted
    ym.clockSample();
    CHECK(ym.channelOutputLeft(5) == 4);
    ym.setLadderEffect(false);
    CHECK(ym.channelOutputLeft(5) == 0);
    ym.write(1, 0xB6, 0xC0);
    ym.clockSample();
    CHECK(ym.channelOutputLeft(5) == 128);
    ym.setLadderEffect(true);
    CHECK(ym.channelOutputLeft(5) == 132);
}

TEST_CASE("Register decode: bank 1 channels and invalid channel 3 slot", "[genesis][operator][registers]")
{
    Ym2612Core ym;
    // Channel 4 (bank 1, first channel), key code check through the frequency registers.
    ym.write(1, 0xA4, (6 << 3) | 0x07);
    ym.write(1, 0xA0, 0xFF);
    CHECK(ym.channelBlock(3) == 6);
    CHECK(ym.channelFnum(3) == 0x7FF);
    CHECK(ym.channelFnum(0) == 0);
    // Address offset 3 is not a channel: the write is ignored.
    ym.write(0, 0xA7, 0x3F);
    ym.write(0, 0xA3, 0xFF);
    for (int c = 0; c < 3; ++c)
        CHECK(ym.channelFnum(c) == 0);
    // $28 channel codes 3 and 7 are invalid.
    ym.write(0, 0x28, 0xF3);
    ym.write(0, 0x28, 0xF7);
    for (int c = 0; c < 6; ++c)
        CHECK(ym.operatorPhase(c, 0) == Ym2612Core::EgPhase::Release);
    // $28 addresses channels 4-6 through the channel field (code 4 = channel 4).
    ym.write(0, 0x28, 0xF4);
    CHECK(ym.operatorPhase(3, 0) == Ym2612Core::EgPhase::Attack);
    // Global registers do not exist in bank 1.
    ym.write(1, 0x2B, 0x80);
    CHECK_FALSE(ym.dacEnabled());
}
