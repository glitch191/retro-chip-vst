// Echo unit: buffer length per EDL, FIR arithmetic and presets, feedback stability.
// Reference values: docs/research/snes.md items 35..39, 43, 51.

#include "chipdsp/snes/SnesDsp.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace chipdsp::snes;

namespace
{
    void writeWord(SnesDsp& dsp, int addr, int value)
    {
        dsp.ram()[addr & 0xFFFF] = static_cast<uint8_t>(value & 0xFF);
        dsp.ram()[(addr + 1) & 0xFFFF] = static_cast<uint8_t>((value >> 8) & 0xFF);
    }
    int readWord(const SnesDsp& dsp, int addr)
    {
        return static_cast<int16_t>(dsp.ram()[addr & 0xFFFF] | (dsp.ram()[(addr + 1) & 0xFFFF] << 8));
    }
    void setFir(SnesDsp& dsp, const int8_t taps[8])
    {
        for (int k = 0; k < 8; ++k)
            dsp.writeRegister(k * 16 + kRegFir, static_cast<uint8_t>(taps[k]));
    }

    // Frozen echo buffer (writes disabled) holding one non-zero entry; MVOL 0, EVOL 127.
    // Returns the sample indices at which the left output differs from silence (~0 = -1).
    std::vector<int> frozenPulses(int edl, const int8_t taps[8], int samples)
    {
        SnesDsp dsp;
        dsp.writeRegister(kRegFlg, kFlgEchoWriteDisable);
        dsp.writeRegister(kRegEsa, 0x80);
        dsp.writeRegister(kRegEdl, static_cast<uint8_t>(edl));
        dsp.writeRegister(kRegEvolL, 0x7F);
        setFir(dsp, taps);
        writeWord(dsp, 0x8000, 0x4000);     // entry 0, left: 15-bit 0x2000
        std::vector<int> pulses;
        SnesDspOutput out;
        for (int t = 0; t < samples; ++t)
        {
            dsp.step(out);
            if (out.mainL != -1)
                pulses.push_back(t);
        }
        return pulses;
    }
} // namespace

TEST_CASE("Echo buffer length in samples per EDL", "[snes][echo]")
{
    REQUIRE(SnesDsp::echoLengthForEdl(0) == 1);
    REQUIRE(SnesDsp::echoLengthForEdl(1) == 512);
    REQUIRE(SnesDsp::echoLengthForEdl(2) == 1024);
    REQUIRE(SnesDsp::echoLengthForEdl(8) == 4096);
    REQUIRE(SnesDsp::echoLengthForEdl(15) == 7680);

    // Measured on the chip: a frozen buffer repeats its single entry once per buffer length.
    const int8_t newest[8] = { 0, 0, 0, 0, 0, 0, 0, 127 };
    for (const int edl : { 0, 1, 2, 8, 15 })
    {
        INFO("EDL " << edl);
        const int length = SnesDsp::echoLengthForEdl(edl);
        const auto pulses = frozenPulses(edl, newest, 2 * length + 10);
        REQUIRE(pulses.size() >= 2);
        REQUIRE(pulses[0] == 0);
        REQUIRE(pulses[1] - pulses[0] == length);
    }
}

TEST_CASE("Echo: EDL takes effect only when the index wraps to 0", "[snes][echo]")
{
    SnesDsp dsp;
    dsp.writeRegister(kRegFlg, kFlgEchoWriteDisable);
    dsp.writeRegister(kRegEsa, 0x80);
    dsp.writeRegister(kRegEdl, 2);
    SnesDspOutput out;
    for (int i = 0; i < 100; ++i)
        dsp.step(out);
    dsp.writeRegister(kRegEdl, 1);
    for (int i = 100; i < 1024; ++i)
    {
        REQUIRE(dsp.echo().length == 1024);
        dsp.step(out);
    }
    REQUIRE(dsp.echo().index == 0);
    dsp.step(out);
    REQUIRE(dsp.echo().length == 512);
}

TEST_CASE("FIR: identity passes the input delayed by 7 taps", "[snes][echo]")
{
    const int8_t identity[8] = { 127, 0, 0, 0, 0, 0, 0, 0 };
    int16_t h[8] = {};
    std::vector<int> outputs;
    for (int t = 0; t < 12; ++t)
    {
        std::memmove(h, h + 1, 7 * sizeof(int16_t));
        h[7] = static_cast<int16_t>(t == 0 ? 0x0800 : 0);
        outputs.push_back(SnesDsp::firFilter(h, identity));
    }
    for (int t = 0; t < 12; ++t)
        REQUIRE(outputs[static_cast<size_t>(t)] == (t == 7 ? 4064 : 0));

    // Coefficient in FIR7 instead: no delay.
    const int8_t newest[8] = { 0, 0, 0, 0, 0, 0, 0, 127 };
    int16_t x[8] = { 0, 0, 0, 0, 0, 0, 0, 0x0800 };
    REQUIRE(SnesDsp::firFilter(x, newest) == 4064);
    x[7] = 0x3FFF;
    REQUIRE(SnesDsp::firFilter(x, newest) == 32510);
    x[7] = -0x4000;
    REQUIRE(SnesDsp::firFilter(x, newest) == -32512);
}

TEST_CASE("FIR: taps 0-6 wrap, tap 7 saturates, bit 0 cleared", "[snes][echo]")
{
    const int16_t full[8] = { 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF };
    const int8_t oldestPair[8] = { 127, 127, 0, 0, 0, 0, 0, 0 };
    REQUIRE(SnesDsp::firFilter(full, oldestPair) == -516);      // 65020 wraps
    const int8_t newestPair[8] = { 0, 0, 0, 0, 0, 0, 127, 127 };
    REQUIRE(SnesDsp::firFilter(full, newestPair) == 32766);     // clamps to 32767, bit 0 cleared

    const int16_t mid[8] = { 0x1000, 0x1000, 0x1000, 0x1000, 0x1000, 0x1000, 0x1000, 0x1000 };
    const int8_t sixteen[8] = { 16, 16, 16, 16, 16, 16, 16, 16 };
    REQUIRE(SnesDsp::firFilter(mid, sixteen) == 8192);
}

TEST_CASE("Echo volume and feedback products", "[snes][echo]")
{
    REQUIRE(SnesDsp::volumeProduct(0x7FFE, 0x7F) == 32510);
    REQUIRE(SnesDsp::volumeProduct(0x7FFE, -128) == -32766);
    REQUIRE(SnesDsp::volumeProduct(0x7FFE, 0x40) == 16383);
}

TEST_CASE("FIR presets: names, pass-through preset delays the echo by 7 samples", "[snes][echo]")
{
    const char* const names[8] = { "Pass-through", "Low-pass soft", "Low-pass strong", "High-pass",
                                   "Band-pass", "Comb", "Bright", "Dark" };
    REQUIRE(kNumFirPresets == 8);
    for (int i = 0; i < 8; ++i)
        REQUIRE(std::string(kFirPresets[i].name) == names[i]);

    // The documented N-SPC / manual byte sets (research "Echo").
    const uint8_t nspcHigh[8] = { 0x58, 0xBF, 0xDB, 0xF0, 0xFE, 0x07, 0x0C, 0x0C };
    const uint8_t nspcLow[8] = { 0x0C, 0x21, 0x2B, 0x2B, 0x13, 0xFE, 0xF3, 0xF9 };
    const uint8_t nspcBand[8] = { 0x34, 0x33, 0x00, 0xD9, 0xE5, 0x01, 0xFC, 0xEB };
    const uint8_t manualLow[8] = { 0xFF, 0x08, 0x17, 0x24, 0x24, 0x17, 0x08, 0xFF };
    for (int k = 0; k < 8; ++k)
    {
        REQUIRE(static_cast<uint8_t>(kFirPresets[0].taps[k]) == (k == 0 ? 0x7F : 0x00));
        REQUIRE(static_cast<uint8_t>(kFirPresets[1].taps[k]) == nspcLow[k]);
        REQUIRE(static_cast<uint8_t>(kFirPresets[2].taps[k]) == manualLow[k]);
        REQUIRE(static_cast<uint8_t>(kFirPresets[3].taps[k]) == nspcHigh[k]);
        REQUIRE(static_cast<uint8_t>(kFirPresets[4].taps[k]) == nspcBand[k]);
    }

    // On the chip: the frozen entry comes out 7 samples late, at 0x2000 * 127 >> 6 -> 16256,
    // then EVOL 127: 16256 * 127 >> 7 = 16129, inverted by the post-amp.
    SnesDsp dsp;
    dsp.writeRegister(kRegFlg, kFlgEchoWriteDisable);
    dsp.writeRegister(kRegEsa, 0x80);
    dsp.writeRegister(kRegEdl, 1);
    dsp.writeRegister(kRegEvolL, 0x7F);
    setFir(dsp, kFirPresets[0].taps);
    writeWord(dsp, 0x8000, 0x4000);
    SnesDspOutput out;
    for (int t = 0; t < 520; ++t)
    {
        dsp.step(out);
        const int expected = (t == 7 || t == 7 + 512) ? ~16129 : ~0;
        REQUIRE(out.mainL == expected);
    }
}

TEST_CASE("Echo feedback at maximum decays and never grows", "[snes][echo]")
{
    auto run = [](int8_t efb, int passes) {
        SnesDsp dsp;
        dsp.writeRegister(kRegFlg, 0x00);          // echo writes enabled, unmuted
        dsp.writeRegister(kRegEsa, 0xF8);          // 0xF800..0xFFFF
        dsp.writeRegister(kRegEdl, 1);             // 512 entries
        dsp.writeRegister(kRegEfb, static_cast<uint8_t>(efb));
        const int8_t identity[8] = { 127, 0, 0, 0, 0, 0, 0, 0 };
        setFir(dsp, identity);
        writeWord(dsp, 0xF800, 0x7FFE);            // one entry holding 15-bit 0x3FFF
        std::vector<int> written;
        SnesDspOutput out;
        for (int t = 0; t < 512 * passes + 64; ++t)
        {
            const int entry = dsp.echo().index;
            dsp.step(out);
            const int v = readWord(dsp, 0xF800 + entry * 4);
            if (v != 0)
                written.push_back(v);
        }
        return written;
    };

    const auto maxFeedback = run(0x7F, 200);
    const int expected[6] = { 32256, 31752, 31254, 30764, 30282, 29808 };
    REQUIRE(maxFeedback.size() >= 6);
    for (int i = 0; i < 6; ++i)
        REQUIRE(maxFeedback[static_cast<size_t>(i)] == expected[i]);
    for (size_t i = 1; i < maxFeedback.size(); ++i)
        REQUIRE(maxFeedback[i] < maxFeedback[i - 1]);

    const auto half = run(0x40, 6);
    const int expectedHalf[4] = { 16254, 8062, 3998, 1982 };
    REQUIRE(half.size() >= 4);
    for (int i = 0; i < 4; ++i)
        REQUIRE(half[static_cast<size_t>(i)] == expectedHalf[i]);
}
