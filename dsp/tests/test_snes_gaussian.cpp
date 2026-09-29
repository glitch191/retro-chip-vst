// Gaussian interpolation table and formula, and the key-on start-up delay.
// Reference values: docs/research/snes.md items 3..9, 41, 47, 48.

#include "chipdsp/snes/SnesDsp.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace chipdsp::snes;

namespace
{
    int quad(int d)
    {
        return kGaussTable[255 - d] + kGaussTable[511 - d] + kGaussTable[256 + d] + kGaussTable[d];
    }
} // namespace

TEST_CASE("Gaussian table: spot values, sum and maximum", "[snes][gauss]")
{
    REQUIRE(kGaussTable[0] == 0x000);
    REQUIRE(kGaussTable[1] == 0x000);
    REQUIRE(kGaussTable[16] == 0x001);
    REQUIRE(kGaussTable[128] == 0x03A);
    REQUIRE(kGaussTable[255] == 0x172);
    REQUIRE(kGaussTable[256] == 0x176);
    REQUIRE(kGaussTable[300] == 0x233);
    REQUIRE(kGaussTable[400] == 0x410);
    REQUIRE(kGaussTable[510] == 0x519);
    REQUIRE(kGaussTable[511] == 0x519);

    int sum = 0;
    int maxValue = 0;
    for (int i = 0; i < 512; ++i)
    {
        sum += kGaussTable[i];
        maxValue = kGaussTable[i] > maxValue ? kGaussTable[i] : maxValue;
    }
    REQUIRE(sum == 262146);
    REQUIRE(maxValue == 0x519);
}

TEST_CASE("Gaussian table: monotonic, symmetric quad sums in 0x7FF..0x801", "[snes][gauss]")
{
    for (int i = 0; i < 511; ++i)
        REQUIRE(kGaussTable[i] <= kGaussTable[i + 1]);

    int count7ff = 0, count800 = 0, count801 = 0;
    for (int d = 0; d < 256; ++d)
    {
        const int q = quad(d);
        REQUIRE(q >= 0x7FF);
        REQUIRE(q <= 0x801);
        REQUIRE(q == quad(255 - d));
        count7ff += q == 0x7FF ? 1 : 0;
        count800 += q == 0x800 ? 1 : 0;
        count801 += q == 0x801 ? 1 : 0;
    }
    REQUIRE(quad(0) == 0x801);
    REQUIRE(quad(1) == 0x801);
    // Reference value 6 says d = 2 gives 0x800, but the documented table gives
    // 0x16A + 0x518 + 0x17D + 0 = 0x7FF (research "Ambiguities" 20); d = 5 is the first 0x800.
    REQUIRE(quad(2) == 0x7FF);
    REQUIRE(quad(5) == 0x800);
    REQUIRE(count800 == 168);
    REQUIRE(count801 == 46);
    REQUIRE(count7ff == 42);

    // Coefficient sets (g[255-d], g[511-d], g[256+d], g[d]).
    REQUIRE(kGaussTable[255] == 370);
    REQUIRE(kGaussTable[511] == 1305);
    REQUIRE(kGaussTable[256] == 374);
    REQUIRE(kGaussTable[255 - 0x40] == 168);
    REQUIRE(kGaussTable[511 - 0x40] == 1210);
    REQUIRE(kGaussTable[256 + 0x40] == 659);
    REQUIRE(kGaussTable[0x40] == 11);
    REQUIRE(kGaussTable[255 - 0x80] == 56);
    REQUIRE(kGaussTable[511 - 0x80] == 965);
    REQUIRE(kGaussTable[256 + 0x80] == 969);
    REQUIRE(kGaussTable[0x80] == 58);
}

TEST_CASE("Gaussian interpolation of a known sequence", "[snes][gauss]")
{
    REQUIRE(SnesDsp::gaussianInterpolate(1000, 2000, 3000, 4000, 0x80) == 2501);
    REQUIRE(SnesDsp::gaussianInterpolate(1000, 2000, 3000, 4000, 0x00) == 2001);
    REQUIRE(SnesDsp::gaussianInterpolate(1000, 2000, 3000, 4000, 0xFF) == 2998);

    // Impulses.
    REQUIRE(SnesDsp::gaussianInterpolate(0, 0, 16383, 0, 0x40) == 5271);
    REQUIRE(SnesDsp::gaussianInterpolate(0, 16383, 0, 0, 0x40) == 9679);
}

TEST_CASE("Gaussian interpolation: documented overflow of full-scale input", "[snes][gauss]")
{
    // Four -0x4000 samples: the 15-bit wrap turns -16392 into +16376 (the "+3FF8h" pop).
    REQUIRE(SnesDsp::gaussianInterpolate(-16384, -16384, -16384, -16384, 0) == 16376);
    REQUIRE(SnesDsp::gaussianInterpolate(-16384, -16384, -16384, -16384, 1) == 16376);
    REQUIRE(SnesDsp::gaussianInterpolate(-16384, -16384, -16384, -16384, 2) == -16376);
    REQUIRE(SnesDsp::gaussianInterpolate(-16384, -16384, -16384, -16384, 0x80) == -16384);
    // Four +0x3FFF samples wrap the other way at d = 0.
    REQUIRE(SnesDsp::gaussianInterpolate(16383, 16383, 16383, 16383, 0) == -16379);
    REQUIRE(SnesDsp::gaussianInterpolate(16383, 16383, 16383, 16383, 0x80) == 16380);
}

namespace
{
    // APU RAM: directory at 0x0200, one looping block at 0x0400 holding 'nibbles'.
    void loadLoopingBlock(SnesDsp& dsp, uint8_t header, const uint8_t data[8])
    {
        uint8_t* ram = dsp.ram();
        ram[0x0200] = 0x00; ram[0x0201] = 0x04;   // start 0x0400
        ram[0x0202] = 0x00; ram[0x0203] = 0x04;   // loop  0x0400
        ram[0x0400] = static_cast<uint8_t>(header | 0x03);
        for (int i = 0; i < 8; ++i)
            ram[0x0401 + i] = data[i];
        dsp.writeRegister(kRegDir, 0x02);
        dsp.writeRegister(kRegFlg, 0x00);
    }
} // namespace

TEST_CASE("Key-on: five silent samples, then the output is centred on the second sample", "[snes][gauss][kon]")
{
    SnesDsp dsp;
    // Filter 0 shift 12 block: samples n * 2048 = 2048, 4096, 6144, 8192, 10240, ...
    const uint8_t data[8] = { 0x12, 0x34, 0x56, 0x70, 0xFE, 0xDC, 0xBA, 0x98 };
    loadLoopingBlock(dsp, 0xC0, data);
    dsp.writeRegister(kRegPitchL, 0x00);
    dsp.writeRegister(kRegPitchH, 0x10);        // P = 0x1000: integer steps, fraction 0
    dsp.writeRegister(kRegAdsr1, 0x00);         // GAIN mode
    dsp.writeRegister(kRegGain, 0x7F);          // direct gain: E = 0x7F0
    dsp.writeRegister(kRegVolL, 0x40);
    dsp.writeRegister(kRegKon, 0x01);           // polled on sample 0 (even)

    SnesDspOutput out;
    int outx[12];
    int env[12];
    for (int i = 0; i < 12; ++i)
    {
        dsp.step(out);
        outx[i] = static_cast<int8_t>(dsp.readRegister(kRegOutx));
        env[i] = dsp.voice(0).env;
    }
    // Sample 0 is the poll sample (envelope forced to 0); samples 1..5 are the silent start-up.
    for (int i = 0; i <= 5; ++i)
        REQUIRE(outx[i] == 0);
    REQUIRE(env[4] == 0);          // envelope updating begins on sample 5
    REQUIRE(env[5] == 0x7F0);

    // Sample 6: fraction 0 -> g[255]*s0 + g[511]*s1 + g[256]*s2 (+ g[0]*s3 = 0), then E.
    const int32_t g6 = SnesDsp::gaussianInterpolate(2048, 4096, 6144, 8192, 0);
    REQUIRE(outx[6] == ((g6 * 0x7F0) >> 11) >> 7);
    REQUIRE(outx[6] != 0);
    const int32_t g7 = SnesDsp::gaussianInterpolate(4096, 6144, 8192, 10240, 0);
    const int32_t env7 = (g7 * 0x7F0) >> 11;
    REQUIRE(outx[7] == env7 >> 7);
    REQUIRE(dsp.voice(0).out16 != 0);
}
