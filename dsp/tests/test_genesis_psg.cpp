// SN76489 (Genesis VDP PSG) register-level core. docs/research/genesis.md "SN76489 (Genesis
// PSG)" and references 36-41.

#include "chipdsp/genesis/Sn76489Core.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using namespace chipdsp;
using namespace chipdsp::genesis;
using Catch::Approx;

namespace
{
    void setTone(Sn76489Core& psg, int ch, int period)
    {
        psg.write(static_cast<uint8_t>(0x80 | (ch << 5) | (period & 0x0F)));
        psg.write(static_cast<uint8_t>((period >> 4) & 0x3F));
    }
    void setAtt(Sn76489Core& psg, int ch, int att)
    {
        psg.write(static_cast<uint8_t>(0x90 | (ch << 5) | (att & 0x0F)));
    }
} // namespace

TEST_CASE("PSG power-on state", "[genesis][psg]")
{
    Sn76489Core psg;
    for (int ch = 0; ch < 4; ++ch)
        CHECK(psg.attenuation(ch) == 15);
    for (int ch = 0; ch < 3; ++ch)
        CHECK(psg.tonePeriod(ch) == 0);
    CHECK(psg.shiftRegister() == 0x8000);
    CHECK(psg.mix() == 0);
}

TEST_CASE("PSG write protocol: latch/data bytes", "[genesis][psg]")
{
    Sn76489Core psg;
    psg.write(0x8E);   // latch channel 0 tone, low 4 bits = 0xE
    CHECK(psg.tonePeriod(0) == 0x00E);   // low bits change the pitch immediately
    psg.write(0x0F);   // data: high 6 bits
    CHECK(psg.tonePeriod(0) == 0x0FE);
    psg.write(0x01);   // the latch is kept: another data byte replaces the high bits
    CHECK(psg.tonePeriod(0) == 0x01E);
    psg.write(0xBF);   // latch channel 1 volume = 15
    psg.write(0x05);   // data byte goes to the latched volume
    CHECK(psg.attenuation(1) == 5);
    CHECK(psg.tonePeriod(0) == 0x01E);
    psg.write(0xE5);   // noise: white, rate 1
    CHECK(psg.noiseControl() == 5);
    psg.write(0x02);   // data byte to the latched noise register
    CHECK(psg.noiseControl() == 2);
}

TEST_CASE("PSG attenuation table: 2 dB steps, 15 = silence", "[genesis][psg]")
{
    // ref 38 measured through the channel output (period 1 = constant +1)
    Sn76489Core psg;
    setTone(psg, 0, 1);
    const int expected[16] = { 32767, 26028, 20675, 16422, 13045, 10362, 8231, 6568,
                               5193, 4125, 3277, 2603, 2067, 1642, 1304, 0 };
    for (int a = 0; a < 16; ++a)
    {
        setAtt(psg, 0, a);
        psg.clock();
        CHECK(psg.channelLevel(0) == expected[a]);
        if (a > 0 && a < 15)
            CHECK(20.0 * std::log10(psg.channelLevel(0) / 32767.0) == Approx(-2.0 * a).margin(0.05));
    }
}

TEST_CASE("PSG tone frequency measured from the counter", "[genesis][psg]")
{
    // ref 36: period 0xFE -> 440.397 Hz; period 2 -> 55930.4 Hz
    for (int period : { 0xFE, 0x3FF, 2 })
    {
        Sn76489Core psg;
        setTone(psg, 1, period);
        setAtt(psg, 1, 0);
        int toggles = 0;
        int last = psg.outputBit(1);
        const int ticks = 223722;   // about one second
        for (int i = 0; i < ticks; ++i)
        {
            psg.clock();
            if (psg.outputBit(1) != last)
            {
                last = psg.outputBit(1);
                ++toggles;
            }
        }
        const double hz = toggles / 2.0 / (ticks / psgTickRate(ClockStandard::Ntsc));
        INFO("period " << period);
        CHECK(hz == Approx(psgToneFrequency(period, ClockStandard::Ntsc)).margin(1.0));
    }
}

TEST_CASE("PSG period 0 and 1 output a constant +1", "[genesis][psg]")
{
    // ref 40c, Ambiguities 20
    for (int period : { 0, 1 })
    {
        Sn76489Core psg;
        setTone(psg, 2, period);
        setAtt(psg, 2, 0);
        for (int i = 0; i < 1000; ++i)
        {
            psg.clock();
            CHECK(psg.channelLevel(2) == 32767);
        }
    }
}

TEST_CASE("Noise LFSR period: white 57337, periodic 16", "[genesis][psg][noise]")
{
    // ref 39
    struct Case { uint8_t reg; uint32_t period; };
    const Case cases[] = { { 0xE4, 57337u }, { 0xE0, 16u } };
    for (const auto& c : cases)
    {
        Sn76489Core psg;
        psg.write(c.reg);
        REQUIRE(psg.shiftRegister() == 0x8000);
        uint32_t period = 0;
        const uint32_t start = psg.noiseShiftCount();
        for (long i = 0; i < 64L * 70000; ++i)
        {
            psg.clock();
            if (psg.noiseShiftCount() != start && psg.shiftRegister() == 0x8000)
            {
                period = psg.noiseShiftCount() - start;
                break;
            }
        }
        INFO("noise register " << int(c.reg));
        CHECK(period == c.period);
    }
}

TEST_CASE("Noise LFSR: first 48 white-noise output bits after reset", "[genesis][psg][noise]")
{
    // ref 41
    const std::string expected = "000000000000000100000000000010010000000001000001";
    Sn76489Core psg;
    psg.write(0xE4);
    std::string bits;
    bits += static_cast<char>('0' + psg.outputBit(3));
    uint32_t count = psg.noiseShiftCount();
    while (bits.size() < expected.size())
    {
        psg.clock();
        if (psg.noiseShiftCount() != count)
        {
            count = psg.noiseShiftCount();
            bits += static_cast<char>('0' + psg.outputBit(3));
        }
    }
    CHECK(bits == expected);
}

TEST_CASE("Noise rate selection including tone 3 mode", "[genesis][psg][noise]")
{
    // ref 40, 40b: rr 0..2 -> clock/512, /1024, /2048 (reload 0x10, 0x20, 0x40); rr 3 -> tone 3
    const int ticks = 32768;
    const uint32_t expected[3] = { 1024, 512, 256 };
    for (int rr = 0; rr < 3; ++rr)
    {
        Sn76489Core psg;
        psg.write(static_cast<uint8_t>(0xE4 | rr));
        for (int i = 0; i < ticks; ++i)
            psg.clock();
        INFO("rate " << rr);
        CHECK(psg.noiseShiftCount() == expected[rr]);
        const double shiftHz = psg.noiseShiftCount() / (ticks / psgTickRate(ClockStandard::Ntsc));
        const double docHz[3] = { 6991.30, 3495.65, 1747.82 };
        CHECK(shiftHz == Approx(docHz[rr]).margin(0.01));
    }

    // Tone 3 mode: the LFSR shifts at the tone-3 frequency clock / (32 * period).
    for (int period : { 1, 100, 0x3FF })
    {
        Sn76489Core psg;
        setTone(psg, 2, period);
        psg.write(0xE7);
        const long n = 2L * period * 500;
        for (long i = 0; i < n; ++i)
            psg.clock();
        INFO("tone 3 period " << period);
        CHECK(psg.noiseShiftCount() == 500u);
    }

    // Periodic noise from tone 3 at period 1 is a 1-in-16 pulse train at 6991.3 Hz (ref 40).
    Sn76489Core psg;
    setTone(psg, 2, 1);
    psg.write(0xE3);
    setAtt(psg, 3, 0);
    int rising = 0;
    int last = psg.outputBit(3);
    const int ticks2 = 223722;
    for (int i = 0; i < ticks2; ++i)
    {
        psg.clock();
        const int b = psg.outputBit(3);
        if (b && !last)
            ++rising;
        last = b;
    }
    CHECK(rising / (ticks2 / psgTickRate(ClockStandard::Ntsc)) == Approx(6991.3).margin(1.0));
}

TEST_CASE("Writing the noise register resets the LFSR", "[genesis][psg][noise]")
{
    Sn76489Core psg;
    psg.write(0xE4);
    for (int i = 0; i < 5000; ++i)
        psg.clock();
    REQUIRE(psg.shiftRegister() != 0x8000);
    psg.write(0xE4);   // same value: still a reset
    CHECK(psg.shiftRegister() == 0x8000);
}
