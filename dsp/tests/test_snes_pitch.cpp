// Pitch register (4.12 fixed point), pitch counter speed and pitch modulation.
// Reference values: docs/research/snes.md items 30..32.

#include "chipdsp/snes/SnesDriver.h"
#include "chipdsp/snes/SnesDsp.h"
#include "chipdsp/snes/SnesDspEngine.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace chipdsp::snes;

TEST_CASE("Pitch register from MIDI note, root at 32 kHz", "[snes][pitch]")
{
    struct Case { int semitones; int expected; };
    const Case cases[] = { { -24, 0x0400 }, { -12, 0x0800 }, { -7, 0x0AAE }, { -1, 0x0F1A }, { 0, 0x1000 },
                           { 1, 0x10F4 }, { 7, 0x17F9 }, { 12, 0x2000 }, { 19, 0x2FF2 }, { 24, 0x3FFF } };
    for (const auto& c : cases)
    {
        INFO("offset " << c.semitones);
        REQUIRE(SnesDriver::pitchRegister(60.0f + static_cast<float>(c.semitones), 60.0f, 32000.0) == c.expected);
    }
    // A 16 kHz sample needs half the register value for the same note.
    REQUIRE(SnesDriver::pitchRegister(60.0f, 60.0f, 16000.0) == 0x0800);
    REQUIRE(SnesDriver::pitchRegister(0.0f, 127.0f, 32000.0) == 3);   // far below: tiny but non-zero
}

TEST_CASE("Pitch modulation formula", "[snes][pitch]")
{
    REQUIRE(SnesDsp::modulatedPitch(0x1000, 0) == 0x1000);
    REQUIRE(SnesDsp::modulatedPitch(0x1000, 32766) == 0x1FFC);
    REQUIRE(SnesDsp::modulatedPitch(0x1000, -32768) == 0);
    REQUIRE(SnesDsp::modulatedPitch(0x1000, 16384) == 0x1800);
    REQUIRE(SnesDsp::modulatedPitch(0x1000, -16384) == 0x0800);
    REQUIRE(SnesDsp::modulatedPitch(0x3FFF, 32766) == 0x7FEE);
    REQUIRE(SnesDsp::modulatedPitch(0x1000, 31) == 0x1000);
}

namespace
{
    // Voices 0 and 1 play a one-block looping sample; the number of block headers loaded
    // (ENDX events, cleared after each) measures the BRR playback rate.
    struct PitchHarness
    {
        SnesDsp dsp;
        SnesDspOutput out;

        PitchHarness()
        {
            uint8_t* ram = dsp.ram();
            ram[0x0200] = 0x00; ram[0x0201] = 0x04;
            ram[0x0202] = 0x00; ram[0x0203] = 0x04;
            ram[0x0400] = 0x03;
            dsp.writeRegister(kRegDir, 0x02);
            dsp.writeRegister(kRegFlg, 0x00);
            for (int v = 0; v < 2; ++v)
            {
                dsp.writeRegister(v * 16 + kRegAdsr1, 0x00);
                dsp.writeRegister(v * 16 + kRegGain, 0x7F);
            }
        }
        void setPitch(int v, int p)
        {
            dsp.writeRegister(v * 16 + kRegPitchL, static_cast<uint8_t>(p & 0xFF));
            dsp.writeRegister(v * 16 + kRegPitchH, static_cast<uint8_t>(p >> 8));
        }
        // Block loads of voice 'v' over 'samples' samples, after the key-on start-up.
        int countBlocks(int v, int samples)
        {
            for (int i = 0; i < 8; ++i)
                dsp.step(out);
            dsp.writeRegister(kRegEndx, 0);
            int blocks = 0;
            for (int i = 0; i < samples; ++i)
            {
                dsp.step(out);
                if ((dsp.readRegister(kRegEndx) & (1 << v)) != 0)
                {
                    ++blocks;
                    dsp.writeRegister(kRegEndx, 0);
                }
            }
            return blocks;
        }
    };
} // namespace

TEST_CASE("Pitch counter: playback rate = 32000 * P / 4096", "[snes][pitch]")
{
    struct Case { int pitch; int blocksPerSecond; };
    // 16 samples per block: 32000 * P / 4096 / 16 blocks per second.
    const Case cases[] = { { 0x1000, 2000 }, { 0x0800, 1000 }, { 0x2000, 4000 }, { 0x3FFF, 7999 }, { 0, 0 } };
    for (const auto& c : cases)
    {
        PitchHarness h;
        h.setPitch(0, c.pitch);
        h.dsp.writeRegister(kRegKon, 0x01);
        const int blocks = h.countBlocks(0, 32000);
        INFO("P = " << c.pitch);
        REQUIRE(std::abs(blocks - c.blocksPerSecond) <= 1);
    }
}

TEST_CASE("PMON: voice 1 follows the previous voice's output, not on noise voices", "[snes][pitch]")
{
    // Voice 0 is a noise voice with a frozen LFSR (-16384) at full envelope, VOL 0 (a silent
    // modulator): its out16 = -32752, so (out16 >> 5) * P >> 10 = -P and voice 1 stops.
    auto run = [](uint8_t pmon, uint8_t non) {
        PitchHarness h;
        h.setPitch(0, 0x1000);
        h.setPitch(1, 0x1000);
        h.dsp.writeRegister(kRegAdsr1, 0x8F);    // voice 0: attack 15 -> E = 0x7FF, SL 7, SR 0 holds
        h.dsp.writeRegister(kRegAdsr2, 0xE0);
        h.dsp.writeRegister(kRegNon, static_cast<uint8_t>(0x01 | non));
        h.dsp.writeRegister(kRegPmon, pmon);
        h.dsp.writeRegister(kRegKon, 0x03);
        return h.countBlocks(1, 3200);
    };
    auto near = [](int value, int expected) { return std::abs(value - expected) <= 1; };
    REQUIRE(near(run(0x00, 0x00), 200));   // unmodulated: 2000 blocks per second
    REQUIRE(run(0x02, 0x00) == 0);         // modulated down to pitch 0
    REQUIRE(near(run(0x02, 0x02), 200));   // voice 1 is a noise voice: PMON ignored (Ambiguity 4)
    REQUIRE(near(run(0x01, 0x00), 200));   // bit 0 has no function
}

TEST_CASE("Driver vibrato: triangle of +/- depth register units, rate ticks per half cycle", "[snes][pitch]")
{
    // 192 host samples at 48 kHz = 128 chip samples = exactly one 4 ms driver tick per block.
    auto engine = std::make_unique<chipdsp::SnesDspEngine>();
    engine->prepare(48000.0, 192);
    std::vector<float> l(192), r(192);
    engine->setParameter(chipdsp::SnesDspEngine::VibratoRate, 4.0f);
    engine->setParameter(chipdsp::SnesDspEngine::VibratoDepth, 20.0f);
    engine->setParameter(chipdsp::SnesDspEngine::VibratoDelay, 3.0f);
    engine->noteOn(0, 60.0f, 1.0f);

    std::vector<int> offsets;
    for (int t = 0; t < 19; ++t)
    {
        engine->renderBlock(l.data(), r.data(), nullptr, nullptr, 192);
        const auto& chip = engine->chip();
        offsets.push_back((chip.readRegister(kRegPitchL) | (chip.readRegister(kRegPitchH) << 8)) - 0x1000);
    }
    // 3 ticks of delay, then a period of 2 * 4 = 8 ticks starting at 0 and rising.
    const int expected[19] = { 0, 0, 0, 0, 10, 20, 10, 0, -10, -20, -10, 0, 10, 20, 10, 0, -10, -20, -10 };
    for (int t = 0; t < 19; ++t)
    {
        INFO("tick " << t);
        REQUIRE(offsets[static_cast<size_t>(t)] == expected[t]);
    }
}

TEST_CASE("Driver vibrato: every rate 1..15 reaches +/- depth, one half cycle per rate ticks", "[snes][pitch]")
{
    // research "Driver tick model": triangle of +/- vibrato_depth, period 2 * rate ticks.
    for (int rate = 1; rate <= 15; ++rate)
    {
        INFO("rate " << rate);
        int lo = 0, hi = 0, firstMax = -1, firstMin = -1;
        for (int t = 0; t < 4 * rate; ++t)
        {
            const int o = SnesDriver::vibratoOffset(t, rate, 20);
            REQUIRE(o >= -20);
            REQUIRE(o <= 20);
            REQUIRE(o == SnesDriver::vibratoOffset(t + 2 * rate, rate, 20));   // period 2 * rate
            if (o > hi) { hi = o; firstMax = t; }
            if (o < lo) { lo = o; firstMin = t; }
        }
        REQUIRE(hi == 20);
        REQUIRE(lo == -20);
        REQUIRE(firstMin - firstMax == rate);   // one half cycle from +depth to -depth
        // Starts at (or just above) 0 and rises.
        REQUIRE(SnesDriver::vibratoOffset(0, rate, 20) >= 0);
        REQUIRE(SnesDriver::vibratoOffset(0, rate, 20) <= 20 / rate + 1);
    }
    // Rate 1: alternate +depth / -depth every tick.
    REQUIRE(SnesDriver::vibratoOffset(0, 1, 7) == 7);
    REQUIRE(SnesDriver::vibratoOffset(1, 1, 7) == -7);
    // Rate 3, depth 20 (was 13 at most before the fix): 7, 20, 7, -7, -20, -7.
    const int r3[6] = { 7, 20, 7, -7, -20, -7 };
    for (int t = 0; t < 6; ++t)
        REQUIRE(SnesDriver::vibratoOffset(t, 3, 20) == r3[t]);
}

TEST_CASE("Engine: MIDI note -> pitch register, transpose and fine tune, PMON bits", "[snes][pitch]")
{
    auto engine = std::make_unique<chipdsp::SnesDspEngine>();
    engine->prepare(48000.0, 256);
    std::vector<float> l(256), r(256);

    engine->noteOn(0, 72.0f, 1.0f);   // root 60 at 32 kHz (empty slot) -> +12 -> 0x2000
    engine->renderBlock(l.data(), r.data(), nullptr, nullptr, 256);
    const auto& chip = engine->chip();
    REQUIRE((chip.readRegister(kRegPitchL) | (chip.readRegister(kRegPitchH) << 8)) == 0x2000);

    engine->setParameter(chipdsp::SnesDspEngine::Transpose, -12.0f);
    engine->setParameter(chipdsp::SnesDspEngine::FineTune, 100.0f);    // +1 semitone
    engine->setParameter(chipdsp::SnesDspEngine::Pmon, 1.0f);
    engine->noteOn(1, 64.0f, 1.0f);   // 64 - 12 + 1 = 53 -> -7 semitones from 60
    engine->renderBlock(l.data(), r.data(), nullptr, nullptr, 256);
    REQUIRE((chip.readRegister(0x12) | (chip.readRegister(0x13) << 8)) == 0x0AAE);   // reference value 30
    REQUIRE(chip.readRegister(kRegPmon) == 0xFE);

    // setChannelPitch is picked up at the next 4 ms driver tick.
    engine->setChannelPitch(0, 78.0f);    // 78 - 12 + 1 = 67 -> +7
    engine->renderBlock(l.data(), r.data(), nullptr, nullptr, 256);
    REQUIRE((chip.readRegister(kRegPitchL) | (chip.readRegister(kRegPitchH) << 8)) == 0x17F9);   // reference value 30
}
