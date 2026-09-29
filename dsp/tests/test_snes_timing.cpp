// Per-sample voice timing: Anomie's S3c/S4 order (envelope applied before it is updated),
// KON against a same-sample block end, code-1 release, KON with KOFF held, the PMON index
// clamp, and the driver's GAIN-release and echo re-initialisation timing.
// Reference values: docs/research/snes.md "Key-on, key-off, ENDX and voice timing",
// "Exact per-sample update order", items 22, 25, 27, 32, 41, Ambiguities 3, 7, 8, 21, 24.

#include "chipdsp/snes/SnesDriver.h"
#include "chipdsp/snes/SnesDsp.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace chipdsp::snes;

namespace
{
    // Frozen noise (FLG noise rate 0): the voice sample is clip15(0x4000) = -16384 on every
    // sample, so out16 = 2 * ((-16384 * E) >> 11) shows exactly which E was applied.
    int32_t noiseOut16(int32_t e) { return 2 * ((-16384 * e) >> 11); }

    struct NoiseVoice
    {
        SnesDsp dsp;
        SnesDspOutput out;
        std::vector<int32_t> out16, env;

        NoiseVoice(uint8_t adsr1, uint8_t adsr2, uint8_t gain)
        {
            uint8_t* ram = dsp.ram();
            ram[0x0200] = 0x00; ram[0x0201] = 0x04;
            ram[0x0202] = 0x00; ram[0x0203] = 0x04;
            ram[0x0400] = 0x03;                       // code 3 block of zeros looping to itself
            dsp.writeRegister(kRegDir, 0x02);
            dsp.writeRegister(kRegFlg, 0x00);
            dsp.writeRegister(kRegNon, 0x01);
            dsp.writeRegister(kRegPitchH, 0x10);
            dsp.writeRegister(kRegAdsr1, adsr1);
            dsp.writeRegister(kRegAdsr2, adsr2);
            dsp.writeRegister(kRegGain, gain);
            dsp.writeRegister(kRegKon, 0x01);         // polled on sample 0
        }
        void run(int samples)
        {
            for (int i = 0; i < samples; ++i)
            {
                dsp.step(out);
                out16.push_back(dsp.voice(0).out16);
                env.push_back(dsp.voice(0).env);
            }
        }
    };
} // namespace

TEST_CASE("Envelope order: the output uses the envelope stored on the previous sample", "[snes][timing]")
{
    NoiseVoice h(0x8F, 0xE0, 0x00);   // A = 15 (E = 0x400 then 0x7FF), SL = 7, SR = 0 holds
    h.run(40);
    for (int i = 0; i <= 5; ++i)
        REQUIRE(h.out16[static_cast<size_t>(i)] == 0);        // reference value 41
    REQUIRE(h.env[5] == 0x400);                               // updating begins on #5
    REQUIRE(h.env[6] == 0x7FF);
    // Sample #6 applies E = 0x400 (stored on #5), not 0x7FF (Anomie S3c: "Apply the volume
    // envelope" comes before "Update the volume envelope").
    REQUIRE(h.out16[6] == -16384);
    REQUIRE(static_cast<int8_t>(h.dsp.readRegister(kRegOutx)) == -128);   // 0x7FF by now
    REQUIRE(h.out16[7] == noiseOut16(0x7FF));                 // -32752

    // Key-off on an even sample: that sample still applies E = 0x7FF, then -8 per sample.
    REQUIRE(h.env.size() % 2 == 0);
    h.dsp.writeRegister(kRegKoff, 0x01);
    h.run(300);
    const size_t koffAt = 40;
    REQUIRE(h.out16[koffAt] == noiseOut16(0x7FF));
    REQUIRE(h.env[koffAt] == 0x7F7);
    REQUIRE(h.out16[koffAt + 1] == noiseOut16(0x7F7));
    // Release reaches 0 after 256 samples (reference value 25); the last non-zero output is
    // the sample after E = 7 was stored (0x7FF - 255 * 8).
    REQUIRE(h.env[koffAt + 254] == 7);
    REQUIRE(h.env[koffAt + 255] == 0);
    REQUIRE(h.out16[koffAt + 255] == noiseOut16(7));
    REQUIRE(h.out16[koffAt + 256] == 0);

    // The invariant over the whole run.
    for (size_t i = 7; i < h.out16.size(); ++i)
        REQUIRE(h.out16[i] == noiseOut16(h.env[i - 1]));
}

TEST_CASE("Envelope order: FLG.7 cuts after the current sample", "[snes][timing]")
{
    NoiseVoice h(0x00, 0x00, 0x7F);   // direct GAIN: E = 0x7F0
    h.run(10);
    REQUIRE(h.env.back() == 0x7F0);
    h.dsp.writeRegister(kRegFlg, 0x80);
    h.run(1);
    REQUIRE(h.out16.back() == noiseOut16(0x7F0));   // FLG.7 is checked after the multiply
    REQUIRE(h.env.back() == 0);
    h.dsp.writeRegister(kRegFlg, 0x00);
    h.run(1);
    REQUIRE(h.out16.back() == 0);
    REQUIRE(h.dsp.voice(0).phase == EnvPhase::Release);
}

TEST_CASE("Key-on sample #0 outputs the old voice level (Ambiguity 21)", "[snes][timing][kon]")
{
    NoiseVoice h(0x00, 0x00, 0x7F);
    h.run(20);
    REQUIRE(h.env.back() == 0x7F0);
    h.dsp.writeRegister(kRegKon, 0x01);             // 20 samples ran: sample 20 polls
    h.run(8);
    REQUIRE(h.out16[20] == noiseOut16(0x7F0));      // "final pre-KON sample"
    REQUIRE(h.env[20] == 0);
    for (size_t i = 21; i <= 25; ++i)
        REQUIRE(h.out16[i] == 0);                   // #1..#5
    REQUIRE(h.env[25] == 0x7F0);                    // #5: envelope updating begins
    REQUIRE(h.out16[26] == noiseOut16(0x7F0));      // #6: first sample
}

TEST_CASE("KON while KOFF is held keys the voice off 2 samples later: nothing is output", "[snes][timing][kon]")
{
    NoiseVoice h(0x00, 0x00, 0x7F);
    h.dsp.writeRegister(kRegKoff, 0x01);            // KOFF set together with the KON
    h.run(40);
    for (const int32_t o : h.out16)
        REQUIRE(o == 0);
    REQUIRE(h.dsp.voice(0).phase == EnvPhase::Release);
    REQUIRE(h.dsp.voice(0).env == 0);
    REQUIRE(!h.dsp.isVoiceSounding(0));
}

namespace
{
    // Voice 0 plays a one-block one-shot sample (0x0400, header 0xC3 = shift 12, filter 0,
    // end + loop, sixteen samples of 14336) whose loop address is a code-1 terminator block
    // at 0x0310 that loops to itself, like the driver's one-shot directory entries.
    void loadOneShot(SnesDsp& dsp, int pitch)
    {
        uint8_t* ram = dsp.ram();
        ram[0x0200] = 0x00; ram[0x0201] = 0x04;     // start 0x0400
        ram[0x0202] = 0x10; ram[0x0203] = 0x03;     // loop  0x0310
        ram[0x0400] = 0xC3;
        for (int i = 1; i <= 8; ++i)
            ram[0x0400 + i] = 0x77;
        ram[0x0310] = 0x01;                          // end, no loop: releases the voice
        dsp.writeRegister(kRegDir, 0x02);
        dsp.writeRegister(kRegFlg, 0x00);
        dsp.writeRegister(kRegPitchL, static_cast<uint8_t>(pitch & 0xFF));
        dsp.writeRegister(kRegPitchH, static_cast<uint8_t>(pitch >> 8));
        dsp.writeRegister(kRegAdsr1, 0x00);
        dsp.writeRegister(kRegGain, 0x7F);           // direct: E = 0x7F0
    }
} // namespace

TEST_CASE("Key-on always wins over a block end decoded on the same sample", "[snes][timing][kon]")
{
    // research "Key-on, key-off, ENDX": "The bit set by the decoder in the same sample as a
    // key-on does not override the clear (Anomie step S4)"; Anomie S3c checks the BRR end
    // bits before handling KON, so a code-1 header cannot cancel the key-on either.
    const int32_t g = SnesDsp::gaussianInterpolate(14336, 14336, 14336, 14336, 0);
    const int expectedOutx = static_cast<int8_t>(((g * 0x7F0) >> 11) >> 7);
    for (const int pitch : { 0x1000, 0x1100, 0x1234, 0x0F00, 0x2000, 0x3FFF })
    {
        for (int t = 0; t < 64; ++t)
        {
            INFO("pitch " << pitch << " delay " << t);
            SnesDsp dsp;
            SnesDspOutput out;
            loadOneShot(dsp, pitch);
            dsp.writeRegister(kRegKon, 0x01);
            for (int i = 0; i < 300 + t; ++i)
                dsp.step(out);
            // The first note has ended on the terminator, which keeps cycling (ENDX set).
            REQUIRE(dsp.voice(0).phase == EnvPhase::Release);
            REQUIRE(dsp.voice(0).env == 0);

            dsp.writeRegister(kRegKon, 0x01);
            int guard = 0;
            do
            {
                dsp.step(out);
            } while (dsp.voice(0).konDelay != 5 && ++guard < 4);
            REQUIRE(dsp.voice(0).konDelay == 5);    // this step was key-on sample #0
            REQUIRE((dsp.readRegister(kRegEndx) & 1) == 0);
            REQUIRE(dsp.voice(0).phase == EnvPhase::Attack);
            for (int i = 1; i <= 5; ++i)
            {
                dsp.step(out);
                REQUIRE((dsp.readRegister(kRegEndx) & 1) == 0);   // fresh key-on reads 0
            }
            REQUIRE(dsp.voice(0).env == 0x7F0);
            dsp.step(out);                                          // #6
            REQUIRE(static_cast<int8_t>(dsp.readRegister(kRegOutx)) == expectedOutx);
        }
    }
}

TEST_CASE("Code-1 block: ENDX when its header becomes current, E = 0 at the next sample's check", "[snes][timing]")
{
    // Block 1 at 0x0400 code 0, block 2 at 0x0409 code 1 (both sixteen samples of 14336).
    SnesDsp dsp;
    SnesDspOutput out;
    uint8_t* ram = dsp.ram();
    ram[0x0200] = 0x00; ram[0x0201] = 0x04;
    ram[0x0202] = 0x00; ram[0x0203] = 0x04;
    ram[0x0400] = 0xC0;
    ram[0x0409] = 0xC1;
    for (int i = 1; i <= 8; ++i)
    {
        ram[0x0400 + i] = 0x77;
        ram[0x0409 + i] = 0x77;
    }
    dsp.writeRegister(kRegDir, 0x02);
    dsp.writeRegister(kRegFlg, 0x00);
    dsp.writeRegister(kRegPitchH, 0x10);
    dsp.writeRegister(kRegAdsr1, 0x00);
    dsp.writeRegister(kRegGain, 0x7F);
    dsp.writeRegister(kRegKon, 0x01);

    int endxAt = -1;
    std::vector<int32_t> out16;
    std::vector<EnvPhase> phase;
    for (int i = 0; i < 60; ++i)
    {
        dsp.step(out);
        out16.push_back(dsp.voice(0).out16);
        phase.push_back(dsp.voice(0).phase);
        if (endxAt < 0 && (dsp.readRegister(kRegEndx) & 1) != 0)
            endxAt = i;
    }
    // Ambiguity 8 (recommended): ENDX rises when block 2's header becomes current, i.e. on
    // the sample that finishes block 1. Groups 1..3 are decoded on #2..#4; at P = 0x1000 the
    // index reaches 0x4000 on the 4th advance (#6..#9), so group 4 is decoded on sample 9.
    REQUIRE(endxAt == 9);
    REQUIRE(phase[9] != EnvPhase::Release);
    REQUIRE(out16[9] != 0);
    // Anomie S3b/S3c: the header is read at the next sample, after that sample's multiply.
    REQUIRE(phase[10] == EnvPhase::Release);
    REQUIRE(out16[10] != 0);
    REQUIRE(out16[11] == 0);
    REQUIRE(!dsp.isVoiceSounding(0));
}

TEST_CASE("PMON: a modulated step above 0x3FFF is not clamped, the index is clamped to 0x7FFF", "[snes][timing][pitch]")
{
    // Ambiguity 3 (Anomie): voice 1 at P = 0x3FFF modulated by a large positive voice 0
    // gets a step near 0x77EE; the index saturates at 0x7FFF, so after the decode it is
    // exactly 0x7FFF - 0x4000 = 0x3FFF on every sample (one group of 4 per sample).
    SnesDsp dsp;
    SnesDspOutput out;
    uint8_t* ram = dsp.ram();
    ram[0x0200] = 0x00; ram[0x0201] = 0x04;
    ram[0x0202] = 0x00; ram[0x0203] = 0x04;
    ram[0x0400] = 0xC3;                              // looping block of 14336
    for (int i = 1; i <= 8; ++i)
        ram[0x0400 + i] = 0x77;
    dsp.writeRegister(kRegDir, 0x02);
    dsp.writeRegister(kRegFlg, 0x00);
    for (int v = 0; v < 2; ++v)
    {
        dsp.writeRegister(v * 16 + kRegAdsr1, 0x00);
        dsp.writeRegister(v * 16 + kRegGain, 0x7F);
    }
    dsp.writeRegister(kRegPitchH, 0x10);                        // voice 0: P = 0x1000
    dsp.writeRegister(0x10 + kRegPitchL, 0xFF);
    dsp.writeRegister(0x10 + kRegPitchH, 0x3F);                 // voice 1: P = 0x3FFF
    dsp.writeRegister(kRegPmon, 0x02);
    dsp.writeRegister(kRegKon, 0x03);
    for (int i = 0; i < 20; ++i)
        dsp.step(out);
    const int32_t mod = dsp.voice(0).out16;
    REQUIRE(mod > 28000);
    REQUIRE(SnesDsp::modulatedPitch(0x3FFF, mod) > 0x3FFF);   // the step itself is not clamped
    for (int i = 0; i < 16; ++i)
    {
        dsp.step(out);
        REQUIRE(dsp.voice(1).index == 0x3FFF);
    }
}

namespace
{
    struct DriverRig
    {
        SnesDsp dsp;
        SnesDriver driver;
        SnesDriverParams params;
        SampleSlot slots[kNumSampleSlots];
        SnesDspOutput out;

        void start() { driver.reset(dsp, params, kSampleDataStart); }
        void sample()
        {
            driver.beforeSample(dsp, params, slots);
            dsp.step(out);
        }
    };
} // namespace

TEST_CASE("Driver GAIN release: inactive as soon as E reaches 0; KOFF from ENVX at the next tick", "[snes][timing][driver]")
{
    DriverRig rig;
    rig.params.releaseMode = 1;
    rig.params.releaseRate = 31;          // exponential decrease every sample: 695 steps (item 22)
    rig.start();
    rig.driver.noteOn(0, 60.0f, 1.0f, rig.params);
    for (int i = 0; i < 300; ++i)
        rig.sample();
    REQUIRE(rig.dsp.voice(0).env == 0x7FF);   // A = 15, SL = 7, SR = 0 (defaults)

    rig.driver.noteOff(0);
    int steps = 0;
    bool koffSeen = false;
    int32_t envAtKoff = -1;
    for (int i = 0; i < 2000; ++i)
    {
        const int32_t before = rig.dsp.voice(0).env;
        rig.sample();
        const int32_t e = rig.dsp.voice(0).env;
        steps += e != before ? 1 : 0;
        // isChannelActive follows the envelope exactly: active while E > 0.
        REQUIRE(rig.driver.isActive(0, rig.dsp) == (e != 0));
        if (!koffSeen && (rig.dsp.readRegister(kRegKoff) & 1) != 0)
        {
            koffSeen = true;
            envAtKoff = before;               // the tick read ENVX = before >> 4
        }
        if (e == 0 && koffSeen)
            break;
    }
    REQUIRE(koffSeen);
    REQUIRE((envAtKoff >> 4) == 0);           // KOFF only once ENVX ($08) reads 0
    if (envAtKoff == 0)
        REQUIRE(steps == 695);                // the whole exponential release (item 22)
    else
        REQUIRE(steps >= 681);                // E = 15 after 680 steps (item 45), then -8 per sample
    REQUIRE(!rig.driver.isActive(0, rig.dsp));
}

TEST_CASE("Driver echo re-initialisation waits exactly 60 ticks = 7680 samples", "[snes][timing][driver]")
{
    DriverRig rig;
    rig.params.echoEnable = 1;
    rig.params.echoDelay = 4;
    rig.start();
    for (int i = 0; i < 1000; ++i)
        rig.sample();
    REQUIRE(rig.driver.echoWritesEnabled());

    rig.params.echoDelay = 8;
    int changedAt = -1, enabledAt = -1;
    for (int s = 0; s < 10000 && enabledAt < 0; ++s)
    {
        rig.sample();
        if (changedAt < 0 && rig.driver.programmedEchoDelay() == 8)
            changedAt = s;
        if (changedAt >= 0 && rig.driver.echoWritesEnabled())
            enabledAt = s;
        if (changedAt >= 0 && enabledAt < 0)
            REQUIRE((rig.dsp.readRegister(kRegFlg) & kFlgEchoWriteDisable) != 0);
    }
    REQUIRE(changedAt >= 0);
    REQUIRE(enabledAt - changedAt == 60 * SnesDriver::kSamplesPerTick);   // 240 ms
    REQUIRE((rig.dsp.readRegister(kRegFlg) & kFlgEchoWriteDisable) == 0);
}
