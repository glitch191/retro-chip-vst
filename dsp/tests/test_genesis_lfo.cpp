// YM2612 LFO measured on the register-level core: frequencies from the emulated 7-bit
// counter, AM depths in dB and PM depths in cents. Reference values: docs/research/genesis.md
// references 30-32 and the OPNA manual tables quoted in section "LFO".

#include "chipdsp/genesis/Ym2612Core.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace chipdsp;
using namespace chipdsp::genesis;
using Catch::Approx;

namespace
{
    void writeOp(Ym2612Core& ym, int slot, int base, int value)
    {
        ym.write(0, static_cast<uint8_t>(base + kOpRegOffset[0][slot]), static_cast<uint8_t>(value));
    }
} // namespace

TEST_CASE("LFO frequencies measured from the counter", "[genesis][lfo]")
{
    const long expectedSamples[8] = { 13824, 9856, 9088, 8576, 7936, 5632, 1024, 640 };
    const double expectedHz[8] = { 3.853, 5.405, 5.861, 6.211, 6.712, 9.458, 52.019, 83.230 };
    for (int f = 0; f < 8; ++f)
    {
        Ym2612Core ym;
        ym.write(0, 0x22, static_cast<uint8_t>(0x08 | f));
        REQUIRE(ym.lfoCounter() == 0);
        long samples = 0;
        int steps = 0;
        int last = 0;
        while (steps < 128 && samples < 100000)
        {
            ym.clockSample();
            ++samples;
            if (ym.lfoCounter() != last)
            {
                CHECK(ym.lfoCounter() == ((last + 1) & 0x7F));
                last = ym.lfoCounter();
                ++steps;
            }
        }
        INFO("LFO setting " << f);
        CHECK(ym.lfoCounter() == 0);
        CHECK(samples == expectedSamples[f]);
        CHECK(fmSampleRate(ClockStandard::Ntsc) / static_cast<double>(samples) == Approx(expectedHz[f]).margin(1e-3));
    }
}

TEST_CASE("LFO disabled holds the counter at 0", "[genesis][lfo]")
{
    Ym2612Core ym;
    ym.write(0, 0x22, 0x0F);
    for (int i = 0; i < 1000; ++i)
        ym.clockSample();
    REQUIRE(ym.lfoCounter() != 0);
    ym.write(0, 0x22, 0x07);
    CHECK(ym.lfoCounter() == 0);
    for (int i = 0; i < 1000; ++i)
        ym.clockSample();
    CHECK(ym.lfoCounter() == 0);
}

TEST_CASE("AMS depths in dB", "[genesis][lfo]")
{
    // OPNA: AMS 0..3 = 0, 1.4, 5.9, 11.8 dB (ref 31: 0, 15, 63, 126 steps of 0.094 dB).
    const int expectedSteps[4] = { 0, 15, 63, 126 };
    const double opnaDb[4] = { 0.0, 1.4, 5.9, 11.8 };
    const double stepDb = 20.0 * std::log10(2.0) / 64.0;
    for (int ams = 0; ams < 4; ++ams)
    {
        Ym2612Core ym;
        ym.write(0, 0x22, 0x0F);                               // LFO on, fastest
        ym.write(0, 0xB0, 0x07);                               // algorithm 7
        ym.write(0, 0xB4, static_cast<uint8_t>(0xC0 | (ams << 4)));
        ym.write(0, 0xA4, 4 << 3);
        ym.write(0, 0xA0, 0x00);
        writeOp(ym, 3, 0x40, 0);                               // S4 TL 0
        writeOp(ym, 3, 0x50, 31);                              // AR 31
        writeOp(ym, 3, 0x60, 0x80);                            // AM on, DR 0
        writeOp(ym, 3, 0x80, 0x0F);                            // SL 0
        writeOp(ym, 2, 0x60, 0x00);                            // S3: AM off (control)
        writeOp(ym, 2, 0x50, 31);
        ym.write(0, 0x28, 0xF0);
        int mn = 1024, mx = -1, ctrlMn = 1024, ctrlMx = -1;
        for (int i = 0; i < 640 * 2; ++i)
        {
            ym.clockSample();
            mn = std::min(mn, ym.operatorEgOutput(0, 3));
            mx = std::max(mx, ym.operatorEgOutput(0, 3));
            ctrlMn = std::min(ctrlMn, ym.operatorEgOutput(0, 2));
            ctrlMx = std::max(ctrlMx, ym.operatorEgOutput(0, 2));
        }
        INFO("AMS " << ams);
        CHECK(mn == 0);
        CHECK(mx - mn == expectedSteps[ams]);
        CHECK((mx - mn) * stepDb == Approx(opnaDb[ams]).margin(0.06));
        CHECK(ctrlMx == ctrlMn);   // operators without the AM bit are not modulated
    }
}

TEST_CASE("AM stays applied while the LFO is disabled", "[genesis][lfo]")
{
    // Research "LFO" (counter held at 0 while disabled), "Amplitude modulation" (maximum at
    // counter 0, [unverified] phase) and Ambiguities 31/38: Nemesis (SpritesMind t=386 page 26)
    // reports that AM is still applied with the LFO off, with the waveform locked. AMS 3 at
    // counter 0 = 126 steps (ref 31, 11.85 dB); AMS 1 = 15; AMS 0 or AM bit clear = none.
    const int expected[4] = { 0, 15, 63, 126 };
    for (int ams = 0; ams < 4; ++ams)
    {
        Ym2612Core ym;
        ym.write(0, 0x22, 0x00);   // LFO disabled (power-on state)
        ym.write(0, 0xB0, 0x07);
        ym.write(0, 0xB4, static_cast<uint8_t>(0xC0 | (ams << 4)));
        ym.write(0, 0xA4, 4 << 3);
        ym.write(0, 0xA0, 0x00);
        writeOp(ym, 3, 0x40, 10);     // S4 TL 10
        writeOp(ym, 3, 0x50, 31);
        writeOp(ym, 3, 0x60, 0x80);   // AM on
        writeOp(ym, 3, 0x80, 0x0F);
        writeOp(ym, 2, 0x40, 10);     // S3: same, AM off
        writeOp(ym, 2, 0x50, 31);
        writeOp(ym, 2, 0x80, 0x0F);
        ym.write(0, 0x28, 0xF0);
        for (int i = 0; i < 3000; ++i)
        {
            ym.clockSample();
            REQUIRE(ym.lfoCounter() == 0);
        }
        INFO("AMS " << ams);
        CHECK(ym.operatorEgOutput(0, 3) == ym.operatorAttenuation(0, 3) + 80 + expected[ams]);
        CHECK(ym.operatorEgOutput(0, 2) == ym.operatorAttenuation(0, 2) + 80);
    }
}

TEST_CASE("FMS depths in cents", "[genesis][lfo]")
{
    // OPNA: PMS 0..7 = 0, 3.4, 6.7, 10, 14, 20, 40, 80 cents; ref 32 with fnum 0x400:
    // 0, 3.38, 6.75, 10.11, 13.47, 20.17, 40.11, 79.31 cents.
    const double cents[8] = { 0, 3.38, 6.75, 10.11, 13.47, 20.17, 40.11, 79.31 };
    const double opna[8] = { 0, 3.4, 6.7, 10, 14, 20, 40, 80 };
    for (int fms = 0; fms < 8; ++fms)
    {
        Ym2612Core ym;
        ym.write(0, 0x22, 0x0F);
        ym.write(0, 0xB4, static_cast<uint8_t>(0xC0 | fms));
        ym.write(0, 0xA4, (4 << 3) | 0x04);   // block 4, fnum 0x400
        ym.write(0, 0xA0, 0x00);
        writeOp(ym, 0, 0x30, 0x01);           // DT 0, MUL 1
        const double centre = 8192.0;         // (0x400 << 4) >> 1
        uint32_t mn = 0xFFFFFFFFu, mx = 0;
        for (int i = 0; i < 640 * 2; ++i)
        {
            ym.clockSample();
            mn = std::min(mn, ym.operatorPhaseIncrement(0, 0));
            mx = std::max(mx, ym.operatorPhaseIncrement(0, 0));
        }
        const double up = 1200.0 * std::log2(mx / centre);
        const double down = 1200.0 * std::log2(centre / mn);
        INFO("FMS " << fms);
        CHECK(up == Approx(cents[fms]).margin(0.02));
        CHECK(down == Approx(1200.0 * std::log2(2048.0 / (2048.0 - (mx - centre) / 4.0))).margin(0.02));
        CHECK(up == Approx(opna[fms]).margin(0.7));
    }
}
