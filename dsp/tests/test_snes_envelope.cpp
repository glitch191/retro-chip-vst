// Global counter, rate table and ADSR/GAIN envelope steps.
// Reference values: docs/research/snes.md items 16..27, 44..46, 49.

#include "chipdsp/snes/SnesDsp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

using namespace chipdsp::snes;

TEST_CASE("Rate table: all 32 periods and offsets", "[snes][envelope]")
{
    const int periods[32] = { 0, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
                              64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1 };
    for (int r = 0; r < 32; ++r)
        REQUIRE(kRatePeriod[r] == periods[r]);

    // Offsets: rates 3n+1 -> 0, 3n+2 -> 1040, 3n (n >= 1) -> 536; rates 30 and 31 -> 0.
    for (int r = 1; r < 30; ++r)
    {
        const int expected = r % 3 == 1 ? 0 : (r % 3 == 2 ? 1040 : 536);
        REQUIRE(kRateOffset[r] == expected);
    }
    REQUIRE(kRateOffset[30] == 0);
    REQUIRE(kRateOffset[31] == 0);
}

TEST_CASE("Global counter: every rate fires at a constant spacing across wraps", "[snes][envelope]")
{
    // Counter = 0 at sample 0, then 0x77FF, 0x77FE, ... (decrement with wrap).
    std::vector<int> first(32, -1), last(32, -1);
    std::vector<bool> regular(32, true);
    int counter = 0;
    for (int s = 0; s < 61440; ++s)
    {
        for (int r = 1; r < 32; ++r)
        {
            if (!SnesDsp::rateFires(r, counter))
                continue;
            if (first[static_cast<size_t>(r)] < 0)
                first[static_cast<size_t>(r)] = s;
            else if (s - last[static_cast<size_t>(r)] != kRatePeriod[r])
                regular[static_cast<size_t>(r)] = false;
            last[static_cast<size_t>(r)] = s;
        }
        counter = counter == 0 ? 0x77FF : counter - 1;
    }
    for (int r = 1; r < 32; ++r)
    {
        INFO("rate " << r);
        REQUIRE(regular[static_cast<size_t>(r)]);
    }
    REQUIRE(!SnesDsp::rateFires(0, 0));
    // First events after reset.
    REQUIRE(first[2] == 1040);
    REQUIRE(first[3] == 536);
    REQUIRE(first[5] == 272);
    REQUIRE(first[6] == 536);
    REQUIRE(first[9] == 216);
    REQUIRE(first[12] == 56);
    REQUIRE(first[30] == 0);
    REQUIRE(first[1] == 0);
    REQUIRE(first[4] == 0);
    REQUIRE(first[7] == 0);
}

TEST_CASE("Global counter wraps from 0 to 0x77FF in the chip", "[snes][envelope]")
{
    SnesDsp dsp;
    SnesDspOutput out;
    REQUIRE(dsp.globalCounter() == 0);
    dsp.step(out);
    REQUIRE(dsp.globalCounter() == 0x77FF);
    for (int i = 0; i < 30719; ++i)
        dsp.step(out);
    REQUIRE(dsp.globalCounter() == 0);
    dsp.step(out);
    REQUIRE(dsp.globalCounter() == 0x77FF);
}

namespace
{
    // Voice 0 plays a silent looping block; the envelope runs regardless of the data.
    struct EnvHarness
    {
        SnesDsp dsp;
        SnesDspOutput out;
        std::vector<int> env;
        std::vector<EnvPhase> phase;

        EnvHarness(uint8_t adsr1, uint8_t adsr2, uint8_t gain)
        {
            uint8_t* ram = dsp.ram();
            ram[0x0200] = 0x00; ram[0x0201] = 0x04;
            ram[0x0202] = 0x00; ram[0x0203] = 0x04;
            ram[0x0400] = 0x03;                       // code 3 block of zeros
            dsp.writeRegister(kRegDir, 0x02);
            dsp.writeRegister(kRegFlg, 0x00);
            dsp.writeRegister(kRegPitchH, 0x10);
            dsp.writeRegister(kRegAdsr1, adsr1);
            dsp.writeRegister(kRegAdsr2, adsr2);
            dsp.writeRegister(kRegGain, gain);
            dsp.writeRegister(kRegKon, 0x01);
        }
        void run(int samples)
        {
            for (int i = 0; i < samples; ++i)
            {
                dsp.step(out);
                env.push_back(dsp.voice(0).env);
                phase.push_back(dsp.voice(0).phase);
            }
        }
        // Envelope values after each change, starting at sample 'from'.
        std::vector<int> changes(size_t from = 0) const
        {
            std::vector<int> c;
            for (size_t i = std::max<size_t>(from, 1); i < env.size(); ++i)
                if (env[i] != env[i - 1])
                    c.push_back(env[i]);
            return c;
        }
        size_t firstSampleIn(EnvPhase p) const
        {
            for (size_t i = 0; i < phase.size(); ++i)
                if (phase[i] == p)
                    return i;
            return phase.size();
        }
    };
} // namespace

TEST_CASE("ADSR attack 15: +1024 per sample, Decay after 2 steps at 0x7FF", "[snes][envelope]")
{
    EnvHarness h(0x8F, 0xE0, 0x00);   // A = 15, D = 0, SL = 7
    h.run(10);
    REQUIRE(h.env[4] == 0);
    REQUIRE(h.env[5] == 0x400);
    REQUIRE(h.env[6] == 0x7FF);
    REQUIRE(h.phase[5] == EnvPhase::Attack);
    REQUIRE(h.phase[6] == EnvPhase::Decay);
}

TEST_CASE("ADSR attack 0..14: 63 steps of +32, Decay entered at 0x7E0", "[snes][envelope]")
{
    for (const int a : { 14, 11, 7 })
    {
        EnvHarness h(static_cast<uint8_t>(0x80 | a), 0x00, 0x00);
        const int period = kRatePeriod[a * 2 + 1];
        h.run(70 * period);
        const size_t decayAt = h.firstSampleIn(EnvPhase::Decay);
        REQUIRE(decayAt < h.env.size());
        REQUIRE(h.env[decayAt] == 0x7E0);

        // Count the +32 steps taken before Decay and check their spacing.
        int count = 0;
        size_t lastStep = 0;
        for (size_t i = 1; i <= decayAt; ++i)
        {
            if (h.env[i] == h.env[i - 1])
                continue;
            REQUIRE(h.env[i] - h.env[i - 1] == 32);
            if (count > 0)
                REQUIRE(static_cast<int>(i - lastStep) == period);
            lastStep = i;
            ++count;
        }
        REQUIRE(count == 63);
        REQUIRE(decayAt == lastStep + 1);   // the test runs every sample on the computed value
    }
}

TEST_CASE("Exponential decrease from 0x7FF: step sizes and 695 steps to 0", "[snes][envelope]")
{
    // Attack 15 to 0x7FF, then switch to GAIN exponential decrease at rate 31 (every sample).
    EnvHarness h(0x8F, 0xE0, 0xBF);
    h.run(7);
    REQUIRE(h.env[6] == 0x7FF);
    h.dsp.writeRegister(kRegAdsr1, 0x0F);   // ADSR off -> GAIN 0xBF
    h.run(800);
    const auto steps = h.changes(7);
    const int expected[] = { 0x7F7, 0x7EF, 0x7E7, 0x7DF, 0x7D7, 0x7CF, 0x7C7, 0x7BF, 0x7B7, 0x7AF, 0x7A7, 0x79F };
    for (size_t i = 0; i < std::size(expected); ++i)
        REQUIRE(steps[i] == expected[i]);
    REQUIRE(steps.size() == 695);
    REQUIRE(steps.back() == 0);
    // Thresholds: 0x100 after 439 steps, 255 after 440, 119 after 576, 95 after 600, 15 after 680.
    REQUIRE(steps[438] == 0x100);
    REQUIRE(steps[439] == 255);
    REQUIRE(steps[575] == 119);
    REQUIRE(steps[599] == 95);
    REQUIRE(steps[679] == 15);
}

TEST_CASE("ADSR decay from 0x7FF until (E >> 8) == SL", "[snes][envelope]")
{
    const int steps[8] = { 440, 312, 227, 163, 112, 69, 32, 0 };
    const int after[8] = { 255, 510, 765, 1020, 1275, 1532, 1791, 2047 };
    for (int sl = 0; sl < 8; ++sl)
    {
        INFO("SL " << sl);
        // A = 15, D = 0 (rate 16, period 64), SR = 0 (sustain holds).
        EnvHarness h(0x8F, static_cast<uint8_t>(sl << 5), 0x00);
        h.run(7 + 64 * 450);
        const size_t decayAt = h.firstSampleIn(EnvPhase::Decay);
        const size_t sustainAt = h.firstSampleIn(EnvPhase::Sustain);
        REQUIRE(sustainAt < h.env.size());
        int count = 0;
        for (size_t i = decayAt + 1; i <= sustainAt; ++i)
            count += h.env[i] != h.env[i - 1] ? 1 : 0;
        REQUIRE(count == steps[sl]);
        REQUIRE(h.env[sustainAt] == after[sl]);
        REQUIRE(h.env.back() == after[sl]);   // SR = 0: never changes in Sustain
    }
}

TEST_CASE("ADSR decay from 0x7E0 (end of a +32 attack)", "[snes][envelope]")
{
    const int steps[8] = { 436, 308, 223, 159, 108, 65, 29, 0 };
    const int after[8] = { 255, 510, 765, 1020, 1275, 1533, 1785, 2016 };
    for (int sl = 0; sl < 8; ++sl)
    {
        INFO("SL " << sl);
        EnvHarness h(0x8E, static_cast<uint8_t>(sl << 5), 0x00);   // A = 14, D = 0
        h.run(200 + 64 * 450);
        const size_t decayAt = h.firstSampleIn(EnvPhase::Decay);
        const size_t sustainAt = h.firstSampleIn(EnvPhase::Sustain);
        REQUIRE(h.env[decayAt] == 0x7E0);
        REQUIRE(sustainAt < h.env.size());
        int count = 0;
        for (size_t i = decayAt + 1; i <= sustainAt; ++i)
            count += h.env[i] != h.env[i - 1] ? 1 : 0;
        REQUIRE(count == steps[sl]);
        REQUIRE(h.env[sustainAt] == after[sl]);
    }
}

TEST_CASE("GAIN bent-line increase: 48 steps of +32 then +8, 112 steps to 0x7FF", "[snes][envelope]")
{
    EnvHarness h(0x00, 0x00, 0xFF);   // GAIN mode 3, rate 31
    h.run(200);
    const auto steps = h.changes();
    REQUIRE(steps.size() == 112);
    REQUIRE(steps[47] == 0x600);
    REQUIRE(steps[48] == 0x608);
    REQUIRE(steps.back() == 0x7FF);
}

TEST_CASE("GAIN linear increase and decrease: +/-32 per event", "[snes][envelope]")
{
    EnvHarness up(0x00, 0x00, 0xDF);  // linear increase, rate 31
    up.run(80);
    const auto s = up.changes();
    REQUIRE(s.size() == 64);          // 0 -> 0x7E0 in 63 steps, 64th clamps to 0x7FF
    REQUIRE(s[0] == 32);
    REQUIRE(s[62] == 0x7E0);
    REQUIRE(s[63] == 0x7FF);

    up.dsp.writeRegister(kRegGain, 0x9F); // linear decrease, rate 31
    const size_t from = up.env.size();
    up.run(80);
    const auto d = up.changes(from);
    REQUIRE(d[0] == 0x7FF - 32);
    REQUIRE(d.back() == 0);
    REQUIRE(d.size() == 64);          // 2047 / 32 -> 63 steps to 31, then clamped to 0
}

TEST_CASE("GAIN direct sets E = value << 4", "[snes][envelope]")
{
    EnvHarness h(0x00, 0x00, 0x7F);
    h.run(8);
    REQUIRE(h.env[7] == 0x7F0);
    h.dsp.writeRegister(kRegGain, 0x40);
    h.run(1);
    REQUIRE(h.env.back() == 0x400);
}

TEST_CASE("Release: -8 per sample, 256 samples from 0x7FF to 0", "[snes][envelope]")
{
    EnvHarness h(0x8F, 0xE0, 0x00);
    h.run(10);
    REQUIRE(h.env.back() == 0x7FF);
    // Key off on an even sample (poll); 10 samples ran, so the next sample (10) polls.
    h.dsp.writeRegister(kRegKoff, 0x01);
    h.run(300);
    const size_t releaseAt = h.firstSampleIn(EnvPhase::Release);
    REQUIRE(releaseAt == 10);
    REQUIRE(h.env[releaseAt] == 0x7FF - 8);
    size_t zeroAt = releaseAt;
    while (h.env[zeroAt] != 0)
        ++zeroAt;
    REQUIRE(zeroAt - releaseAt + 1 == 256);
    REQUIRE(!h.dsp.isVoiceSounding(0));
}

TEST_CASE("ENVX and OUTX registers", "[snes][envelope]")
{
    // Noise voice with a frozen LFSR (rate 0): the sample is clip15(0x4000) = -16384.
    EnvHarness h(0x8F, 0xE0, 0x00);
    h.dsp.writeRegister(kRegNon, 0x01);
    h.run(10);
    REQUIRE(h.dsp.voice(0).env == 0x7FF);
    REQUIRE(h.dsp.readRegister(kRegEnvx) == 127);
    // env = -16384 * 0x7FF >> 11 = -16376 -> OUTX = -128 (0x80)
    REQUIRE(h.dsp.readRegister(kRegOutx) == 0x80);

    h.dsp.writeRegister(kRegAdsr1, 0x00);
    h.dsp.writeRegister(kRegGain, 0x40);   // E = 0x400 -> ENVX 64
    h.run(2);
    REQUIRE(h.dsp.readRegister(kRegEnvx) == 64);
}

TEST_CASE("Voice and master volume products", "[snes][envelope]")
{
    REQUIRE(SnesDsp::voiceVolume(16383, 127) == 32510);
    REQUIRE(SnesDsp::voiceVolume(16383, -128) == -32766);
    REQUIRE(SnesDsp::voiceVolume(16383, 64) == 16383);
    REQUIRE(SnesDsp::voiceVolume(16383, 1) == 255);
    REQUIRE(SnesDsp::volumeProduct(32767, 127) == 32511);
    REQUIRE(SnesDsp::volumeProduct(-32768, -128) == 32767);   // 32768 clamped (Ambiguity 16)
    REQUIRE(SnesDsp::volumeProduct(32767, -128) == -32767);
}
