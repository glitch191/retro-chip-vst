// DMC channel and the 1-bit delta encoder (research "DMC channel", items 32-35).

#include "chipdsp/nes/Nes2A03Engine.h"
#include "chipdsp/nes/NesApu.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <vector>

using namespace chipdsp::nes;
using chipdsp::Nes2A03Engine;

TEST_CASE("NES DMC delta counter clamping", "[nes][dmc]")
{
    // Item 34: a step that would leave 0..127 leaves the level unchanged.
    REQUIRE(dmcOutputStep(125, true) == 127);
    REQUIRE(dmcOutputStep(126, true) == 126);
    REQUIRE(dmcOutputStep(127, true) == 127);
    REQUIRE(dmcOutputStep(1, false) == 1);
    REQUIRE(dmcOutputStep(2, false) == 0);
    REQUIRE(dmcOutputStep(0, false) == 0);
    REQUIRE(dmcOutputStep(64, true) == 66);
    REQUIRE(dmcOutputStep(64, false) == 62);
}

TEST_CASE("NES DMC address and length registers", "[nes][dmc]")
{
    // Item 35.
    DmcChannel d;
    d.write(2, 0xFF);
    d.write(3, 0xFF);
    REQUIRE(d.sampleAddress() == 0xFFC0);
    REQUIRE(d.sampleLength() == 4081);
    d.write(2, 0x00);
    d.write(3, 0x00);
    REQUIRE(d.sampleAddress() == 0xC000);
    REQUIRE(d.sampleLength() == 1);
}

TEST_CASE("NES DMC playback: LSB first, one bit per rate period, clamped, level held when silent", "[nes][dmc]")
{
    std::array<uint8_t, 64> mem {};
    mem[0] = 0xFF;  // eight +2 steps
    mem[1] = 0x00;  // eight -2 steps
    NesApu apu;
    apu.dmc.setMemory(mem.data(), static_cast<int>(mem.size()));
    apu.write(0x4010, 0x0F);   // rate 15 = 54 CPU cycles
    apu.write(0x4011, 121);    // direct load, immediate
    REQUIRE(apu.dmc.output() == 121);
    apu.write(0x4012, 0x00);   // $C000
    apu.write(0x4013, 0x01);   // 17 bytes
    apu.write(0x4015, 0x10);
    REQUIRE((apu.peekStatus() & 0x10) != 0);

    std::vector<int> changeCycles;
    std::vector<uint8_t> levels;
    uint8_t prev = apu.dmc.output();
    for (int c = 0; c < 54 * 8 * 20; ++c)
    {
        apu.clock();
        if (apu.dmc.output() != prev)
        {
            changeCycles.push_back(c);
            levels.push_back(apu.dmc.output());
            prev = apu.dmc.output();
        }
    }
    // The first output cycle starts silent (no byte in the shift register yet); byte 0 then plays
    // 121 -> 123 -> 125 -> 127, then two skipped +2 steps (clamp), then byte 1 goes down.
    REQUIRE(levels.size() >= 6);
    REQUIRE(levels[0] == 123);
    REQUIRE(levels[1] == 125);
    REQUIRE(levels[2] == 127);
    REQUIRE(levels[3] == 125); // first bit of byte 1
    for (size_t i = 1; i < 3; ++i)
        REQUIRE(changeCycles[i] - changeCycles[i - 1] == 54);
    // 127 is held for the five remaining '1' bits: 5 skipped steps + the first '0' bit.
    REQUIRE(changeCycles[3] - changeCycles[2] == 54 * 6);

    // After the 17 bytes the channel falls silent and holds its level.
    REQUIRE(apu.dmc.bytesRemainingCount() == 0);
    const uint8_t held = apu.dmc.output();
    for (int c = 0; c < 10000; ++c)
    {
        apu.clock();
        REQUIRE(apu.dmc.output() == held);
    }
    REQUIRE_FALSE(apu.dmc.isActive());
}

TEST_CASE("NES DMC loop, stop and IRQ", "[nes][dmc]")
{
    std::array<uint8_t, 32> mem {};
    mem.fill(0x55);
    NesApu apu;
    apu.dmc.setMemory(mem.data(), static_cast<int>(mem.size()));

    // Loop: bytes remaining is reloaded when it reaches 0.
    apu.write(0x4010, 0x4F);
    apu.write(0x4013, 0x00);   // 1 byte
    apu.write(0x4015, 0x10);
    for (int c = 0; c < 54 * 8 * 10; ++c)
        apu.clock();
    REQUIRE(apu.dmc.bytesRemainingCount() > 0);
    REQUIRE(apu.dmc.currentAddress() <= 0xC001);

    // Clearing the enable bit sets bytes remaining to 0.
    apu.write(0x4015, 0x00);
    REQUIRE(apu.dmc.bytesRemainingCount() == 0);
    REQUIRE((apu.peekStatus() & 0x10) == 0);

    // IRQ at the end of a non-looping sample when enabled.
    apu.write(0x4010, 0x8F);
    apu.write(0x4013, 0x00);
    for (int c = 0; c < 54 * 8 * 4; ++c)
        apu.clock();                    // let the previous buffer drain
    apu.write(0x4015, 0x10);
    for (int c = 0; c < 54 * 8 * 4; ++c)
        apu.clock();
    REQUIRE(apu.dmc.irqFlag());
    REQUIRE((apu.peekStatus() & 0x80) != 0);
    apu.write(0x4015, 0x00);            // writing $4015 clears the DMC interrupt flag
    REQUIRE_FALSE(apu.dmc.irqFlag());
}

TEST_CASE("NES DMC rate table sets the bit period (NTSC and PAL)", "[nes][dmc]")
{
    for (bool pal : { false, true })
    {
        // Research "DMC channel" rate tables (CPU cycles per output bit), transcribed here.
        const int rates[4] = { 0, 8, 13, 15 };
        const int refNtsc[4] = { 428, 190, 84, 54 };
        const int refPal[4] = { 398, 176, 78, 50 };
        for (int k = 0; k < 4; ++k)
        {
            const int rate = rates[k];
            std::array<uint8_t, 32> mem {};
            mem.fill(0xFF);
            NesApu apu;
            apu.setRegion(pal);
            apu.dmc.setMemory(mem.data(), static_cast<int>(mem.size()));
            apu.write(0x4010, static_cast<uint8_t>(rate));
            apu.write(0x4011, 0);
            apu.write(0x4013, 0x01);
            apu.write(0x4015, 0x10);
            const int period = pal ? refPal[k] : refNtsc[k];
            std::vector<int> changes;
            uint8_t prev = 0;
            for (int c = 0; c < period * 40; ++c)
            {
                apu.clock();
                if (apu.dmc.output() != prev)
                {
                    changes.push_back(c);
                    prev = apu.dmc.output();
                }
            }
            REQUIRE(changes.size() >= 20);
            for (size_t i = 1; i < changes.size(); ++i)
                REQUIRE(changes[i] - changes[i - 1] == period);
        }
    }
}

TEST_CASE("NES DMC encoder tracks the target and decodes exactly through the hardware model", "[nes][dmc][encoder]")
{
    const double bitRate = kCpuHzNtsc / kDmcPeriodNtsc[15];
    const double srcRate = 44100.0;
    std::vector<float> pcm(4410);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = 0.8f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 100.0 * static_cast<double>(i) / srcRate));

    std::vector<uint8_t> bytes(4096, 0);
    const int n = Nes2A03Engine::encodeDmc(pcm.data(), static_cast<int>(pcm.size()), srcRate, bitRate, 64, bytes.data(), 4081);
    REQUIRE(n == static_cast<int>(std::ceil(std::ceil(4410.0 * bitRate / srcRate) / 8.0)));

    // Software decode with the documented counter rule: tracking error of a slow sine stays
    // within one step plus the per-bit slope.
    uint8_t level = 64;
    std::vector<uint8_t> decoded;
    for (int i = 0; i < n * 8; ++i)
    {
        level = dmcOutputStep(level, ((bytes[static_cast<size_t>(i / 8)] >> (i % 8)) & 1) != 0);
        decoded.push_back(level);
        const double t = static_cast<double>(i) / bitRate;
        if (t * srcRate < 4400.0)
        {
            const double target = 64.0 + 63.0 * 0.8 * std::sin(2.0 * std::numbers::pi * 100.0 * t);
            REQUIRE(std::abs(static_cast<double>(level) - target) <= 3.0);
        }
    }

    // Hardware playback of the same bytes reproduces the software decode bit for bit.
    NesApu apu;
    apu.dmc.setMemory(bytes.data(), static_cast<int>(bytes.size()));
    apu.write(0x4010, 0x0F);
    apu.write(0x4011, 64);
    apu.write(0x4013, static_cast<uint8_t>((Nes2A03Engine::paddedDmcLength(n) - 1) / 16));
    apu.write(0x4015, 0x10);
    std::vector<uint8_t> played;
    // One silent output cycle (8 bits) passes before the first byte reaches the shift register.
    int timerClocks = 0;
    for (int c = 0; c < 54 * (n * 8 + 16); ++c)
    {
        apu.clock();
        if (c % 54 == 53)
        {
            ++timerClocks;
            if (timerClocks > 8)
                played.push_back(apu.dmc.output());
        }
    }
    REQUIRE(played.size() >= decoded.size());
    for (size_t i = 0; i < decoded.size(); ++i)
    {
        INFO("bit " << i);
        REQUIRE(played[i] == decoded[i]);
    }

    // Full-scale square: the counter saturates by skipping steps, never wraps.
    std::vector<float> square(2000);
    for (size_t i = 0; i < square.size(); ++i)
        square[i] = (i / 200) % 2 == 0 ? 1.0f : -1.0f;
    const int m = Nes2A03Engine::encodeDmc(square.data(), 2000, srcRate, bitRate, 64, bytes.data(), 4081);
    REQUIRE(m > 0);
    level = 64;
    uint8_t lo = 127, hi = 0;
    for (int i = 0; i < m * 8; ++i)
    {
        level = dmcOutputStep(level, ((bytes[static_cast<size_t>(i / 8)] >> (i % 8)) & 1) != 0);
        lo = std::min(lo, level);
        hi = std::max(hi, level);
    }
    REQUIRE(lo <= 2);
    REQUIRE(hi >= 126);
}

TEST_CASE("NES engine sample slots: 16 slots, 4081-byte limit, padded L*16+1 lengths", "[nes][dmc][engine]")
{
    Nes2A03Engine engine;
    engine.prepare(48000.0, 512);
    REQUIRE(engine.numSampleSlots() == 16);

    std::vector<float> shortPcm(1000, 0.25f);
    REQUIRE_FALSE(engine.loadSample(-1, shortPcm.data(), 1000, 44100.0));
    REQUIRE_FALSE(engine.loadSample(16, shortPcm.data(), 1000, 44100.0));
    REQUIRE(engine.loadSample(3, shortPcm.data(), 1000, 44100.0));
    const int len = engine.sampleLength(3);
    REQUIRE(len > 0);
    REQUIRE((len - 1) % 16 == 0);
    REQUIRE(len <= 4081);

    // 2 s at rate 15 needs 66288 bits = 8286 bytes: more than the hardware maximum.
    std::vector<float> longPcm(88200, 0.0f);
    REQUIRE_FALSE(engine.loadSample(4, longPcm.data(), 88200, 44100.0));
    REQUIRE(engine.sampleLength(4) == 0);
    REQUIRE(engine.sampleLength(3) == len); // the other slot survives the bank flip

    // The same length fits at rate 0 (4181.7 bits/s -> 1046 bytes).
    engine.setParameter(Nes2A03Engine::DmcRate, 0.0f);
    REQUIRE(engine.loadSample(4, longPcm.data(), 88200, 44100.0));
    REQUIRE(engine.sampleLength(4) == Nes2A03Engine::paddedDmcLength(1046));

    REQUIRE(Nes2A03Engine::paddedDmcLength(1) == 1);
    REQUIRE(Nes2A03Engine::paddedDmcLength(2) == 17);
    REQUIRE(Nes2A03Engine::paddedDmcLength(17) == 17);
    REQUIRE(Nes2A03Engine::paddedDmcLength(18) == 33);
    REQUIRE(Nes2A03Engine::paddedDmcLength(4081) == 4081);
}

TEST_CASE("NES engine clearSample empties one slot; stageParameter sets the encoder rate", "[nes][dmc][engine]")
{
    Nes2A03Engine engine;
    engine.prepare(48000.0, 512);
    std::vector<float> pcm(2000, 0.25f);
    REQUIRE(engine.loadSample(0, pcm.data(), 2000, 44100.0));
    REQUIRE(engine.loadSample(1, pcm.data(), 2000, 44100.0));
    const int len = engine.sampleLength(1);

    REQUIRE(engine.clearSample(0));
    REQUIRE(engine.sampleLength(0) == 0);
    REQUIRE(engine.sampleLength(1) == len);
    REQUIRE_FALSE(engine.clearSample(16));

    // A DMC note on the empty slot maps nothing: the channel is not active.
    engine.noteOn(4, 60.0f, 1.0f);
    std::vector<float> l(512), r(512);
    engine.renderBlock(l.data(), r.data(), nullptr, nullptr, 512);
    REQUIRE_FALSE(engine.isChannelActive(4));

    // Staged dmc_rate 0 (not yet delivered by setParameter) already drives the encoder.
    std::vector<float> longPcm(88200, 0.0f);
    REQUIRE_FALSE(engine.loadSample(2, longPcm.data(), 88200, 44100.0));
    engine.stageParameter(Nes2A03Engine::DmcRate, 0.0f);
    REQUIRE(engine.getParameter(Nes2A03Engine::DmcRate) == 0.0f);
    REQUIRE(engine.loadSample(2, longPcm.data(), 88200, 44100.0));
    engine.stageParameter(Nes2A03Engine::DmcRate, 99.0f);   // clamped like setParameter()
    REQUIRE(engine.getParameter(Nes2A03Engine::DmcRate) == 15.0f);
}
