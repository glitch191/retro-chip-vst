// Output stage (16-bit saturation, mute, post-amp inversion) and the noise generator.
// Reference values: docs/research/snes.md items 33, 34, 40, 42, 50.

#include "chipdsp/snes/SnesDsp.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace chipdsp::snes;

TEST_CASE("Noise LFSR: period 32767 and first outputs after reset", "[snes][noise]")
{
    const int16_t firstOutputs[10] = { -16384, 8192, 4096, 2048, 1024, 512, 256, 128, 64, 32 };
    uint16_t n = 0x4000;
    for (int i = 0; i < 10; ++i)
    {
        REQUIRE(clip15(n) == firstOutputs[i]);
        n = SnesDsp::noiseStep(n);
    }

    n = 0x4000;
    int period = 0;
    do
    {
        n = SnesDsp::noiseStep(n);
        ++period;
        REQUIRE(n != 0);
        REQUIRE(n <= 0x7FFF);
    } while (n != 0x4000 && period < 70000);
    REQUIRE(period == 32767);
}

TEST_CASE("Noise clock: LFSR updates per second follow the rate table", "[snes][noise]")
{
    struct Case { int rate; int perSecond; };
    const Case cases[] = { { 31, 32000 }, { 30, 16000 }, { 28, 8000 }, { 25, 4000 }, { 19, 1000 }, { 12, 200 }, { 0, 0 } };
    for (const auto& c : cases)
    {
        SnesDsp dsp;
        dsp.writeRegister(kRegFlg, static_cast<uint8_t>(c.rate));
        SnesDspOutput out;
        int updates = 0;
        uint16_t last = dsp.noiseState();
        for (int i = 0; i < 32000; ++i)
        {
            dsp.step(out);
            if (dsp.noiseState() != last)
                ++updates;
            last = dsp.noiseState();
        }
        INFO("rate " << c.rate);
        REQUIRE(updates == c.perSecond);
    }
}

TEST_CASE("Reset state: FLG 0xE0, ENDX 0, counter 0, noise 0x4000, muted output", "[snes][output]")
{
    SnesDsp dsp;
    REQUIRE(dsp.readRegister(kRegFlg) == 0xE0);
    REQUIRE(dsp.readRegister(kRegEndx) == 0);
    REQUIRE(dsp.globalCounter() == 0);
    REQUIRE(dsp.noiseState() == 0x4000);
    for (int v = 0; v < kNumVoices; ++v)
        REQUIRE(!dsp.isVoiceSounding(v));
    SnesDspOutput out;
    dsp.step(out);
    REQUIRE(out.mainL == -1);   // mute gives 0, inverted to 0xFFFF
}

TEST_CASE("Final inversion is a one's complement of the 16-bit output", "[snes][output]")
{
    // Silence 0 -> -1; +32767 -> -32768; -32768 -> +32767 (fullsnes "XOR FFFFh").
    REQUIRE(static_cast<int16_t>(~0) == -1);
    REQUIRE(static_cast<int16_t>(~32767) == -32768);
    REQUIRE(static_cast<int16_t>(~(-32768)) == 32767);
}

TEST_CASE("16-bit saturation arithmetic of the mixer", "[snes][output]")
{
    REQUIRE(clamp16(32510 + 32510) == 32767);
    REQUIRE(SnesDsp::volumeProduct(32767, 127) == 32511);
    REQUIRE(clamp16(32511 + SnesDsp::volumeProduct(32766, 127)) == 32767);
    int mix = 0;
    for (int v = 0; v < 8; ++v)
        mix = clamp16(mix - 32766);
    REQUIRE(mix == -32768);
}

namespace
{
    // All voices play a noise sample frozen at -16384 with ADSR attack 15 (E = 0x7FF):
    // env = -16376 per voice.
    struct LoudHarness
    {
        SnesDsp dsp;
        SnesDspOutput out;
        explicit LoudHarness(uint8_t keyMask, int8_t vol, uint8_t flg = 0x00)
        {
            uint8_t* ram = dsp.ram();
            ram[0x0200] = 0x00; ram[0x0201] = 0x04;
            ram[0x0202] = 0x00; ram[0x0203] = 0x04;
            ram[0x0400] = 0x03;
            dsp.writeRegister(kRegDir, 0x02);
            dsp.writeRegister(kRegFlg, flg);
            dsp.writeRegister(kRegNon, 0xFF);
            dsp.writeRegister(kRegMvolL, 0x7F);
            dsp.writeRegister(kRegMvolR, 0x7F);
            for (int v = 0; v < kNumVoices; ++v)
            {
                dsp.writeRegister(v * 16 + kRegAdsr1, 0x8F);
                dsp.writeRegister(v * 16 + kRegAdsr2, 0xE0);
                dsp.writeRegister(v * 16 + kRegVolL, static_cast<uint8_t>(vol));
                dsp.writeRegister(v * 16 + kRegVolR, static_cast<uint8_t>(vol));
            }
            dsp.writeRegister(kRegKon, keyMask);
            for (int i = 0; i < 10; ++i)
                dsp.step(out);
        }
    };
} // namespace

TEST_CASE("Output saturates at 16 bits instead of wrapping", "[snes][output]")
{
    // One voice: -16376 * -128 >> 6 = 32752; MVOL 127 -> 32752 * 127 >> 7 = 32496; inverted.
    LoudHarness one(0x01, -128);
    REQUIRE(one.out.voiceL[0] == static_cast<int16_t>(~32496));
    REQUIRE(one.out.mainL == static_cast<int16_t>(~32496));

    // Two voices: 32752 + 32752 saturates at 32767 before MVOL: 32767 * 127 >> 7 = 32511.
    LoudHarness two(0x03, -128);
    REQUIRE(two.out.mainL == static_cast<int16_t>(~32511));
    // Each per-voice bus still carries its own unsaturated value.
    REQUIRE(two.out.voiceL[1] == static_cast<int16_t>(~32496));

    // Eight voices at VOL 127: -16376 * 127 >> 6 = -32497 each, sum saturates at -32768;
    // MVOL 127: -32768 * 127 >> 7 = -32512.
    LoudHarness eight(0xFF, 127);
    REQUIRE(eight.out.mainL == static_cast<int16_t>(~(-32512)));

    // Adding a full-scale echo return on top of a saturated mix still saturates.
    LoudHarness echo(0x03, -128, kFlgEchoWriteDisable);
    echo.dsp.writeRegister(kRegEsa, 0x80);
    echo.dsp.writeRegister(kRegEdl, 0);
    echo.dsp.writeRegister(kRegEvolL, 0x7F);
    echo.dsp.writeRegister(7 * 16 + kRegFir, 0x7F);
    echo.dsp.ram()[0x8000] = 0xFE;
    echo.dsp.ram()[0x8001] = 0x7F;        // 15-bit 0x3FFF
    echo.dsp.step(echo.out);
    REQUIRE(echo.out.mainL == static_cast<int16_t>(~32767));
}

TEST_CASE("FLG mute silences every output", "[snes][output]")
{
    LoudHarness h(0x01, 127, kFlgMute);
    REQUIRE(h.out.mainL == -1);
    REQUIRE(h.out.voiceL[0] == -1);
    REQUIRE(h.dsp.voice(0).env == 0x7FF);   // the voice itself keeps running
}

TEST_CASE("FLG soft reset releases every voice with envelope 0", "[snes][output]")
{
    LoudHarness h(0xFF, 127);
    h.dsp.writeRegister(kRegFlg, kFlgReset);
    h.dsp.step(h.out);
    for (int v = 0; v < kNumVoices; ++v)
    {
        REQUIRE(h.dsp.voice(v).phase == EnvPhase::Release);
        REQUIRE(h.dsp.voice(v).env == 0);
        REQUIRE(!h.dsp.isVoiceSounding(v));
    }
}
