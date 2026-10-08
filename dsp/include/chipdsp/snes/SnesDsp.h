#pragma once

// S-DSP chip core: 8 BRR voices with Gaussian interpolation, ADSR/GAIN envelopes, the
// shared noise generator, pitch modulation, the echo unit and the output mixer, clocked
// one 32 kHz sample at a time. It knows nothing about MIDI: it is driven purely through
// its 128-byte register file and its 64 KiB APU RAM image, like the real chip.
//
// Every formula follows docs/research/snes.md; section names are quoted in the comments.
// All state is fixed-size; step() never allocates.

#include "chipdsp/snes/BrrCodec.h"
#include "chipdsp/snes/SnesTables.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace chipdsp::snes
{

// Envelope phase tracked for every voice, in ADSR and GAIN mode alike
// (research "Envelope (ADSR/GAIN) and the global counter", "State machine").
enum class EnvPhase : uint8_t
{
    Attack,
    Decay,
    Sustain,
    Release,
};

// One voice: decoder ring, pitch counter and envelope.
struct Voice
{
    // 12-sample ring of decoded 15-bit samples, three groups of four (Anomie).
    int16_t ring[12] = {};
    int ringHead = 0;            // index of the oldest sample in the ring (0, 4 or 8)
    BrrHistory history;          // BRR filter history (last two decoded samples)
    uint16_t brrAddr = 0;        // address of the current 9-byte block in APU RAM
    int brrOffset = 1;           // next data byte pair within the block (1, 3, 5, 7)
    int32_t index = 0;           // interpolation index, 4.12 fixed point, 0..0x7FFF
    int konDelay = 0;            // remaining samples of the 5-sample key-on start-up

    int32_t env = 0;             // 11-bit envelope level 0..0x7FF
    int32_t hiddenEnv = 0;       // previous computed value & 0x7FF (bent-increase test)
    EnvPhase phase = EnvPhase::Release;

    int32_t out16 = 0;           // envelope-scaled 16-bit sample (PMON source for voice + 1)
};

// Echo unit state (research "Echo (ESA/EDL, buffer, FIR, EFB, EVOL, EON)").
struct Echo
{
    int index = 0;               // current entry within the buffer
    int length = 1;              // entries, latched from EDL when the index wraps to 0
    int16_t historyL[8] = {};    // last eight 15-bit buffer reads, [0] oldest .. [7] newest
    int16_t historyR[8] = {};
};

// One 32 kHz output sample: the main stereo pair and each voice alone through the same
// output stage (VxVOL, MVOL, mute, final inversion; no echo).
// Samples at which the hardware arithmetic saturated or wrapped since reset(), per stage
// (left and right counted separately). Measurement only: counting never changes a sample.
// Used by the preset QA (chiptool features, "chord" pass) to reject presets that drive the
// mixers or the echo buffer to full scale; `peak` / 32768 is how close a render came (all
// these stages scale linearly with the voice volumes while nothing saturates).
struct SaturationCounts
{
    uint32_t mix = 0;          // main voice mix clamped (clamp16 after an addition)
    uint32_t echoMix = 0;      // echo voice mix (EON voices) clamped
    uint32_t echoInput = 0;    // echo buffer write (echo mix + feedback) clamped
    uint32_t firWrap = 0;      // FIR taps 0-6 wrapped at 16 bits (the audible clicks)
    uint32_t firClamp = 0;     // FIR tap 7 addition clamped
    uint32_t output = 0;       // main output (main + echo) clamped
    int32_t peak = 0;          // largest |value| any of these stages computed before its clamp or wrap

    uint32_t total() const noexcept { return mix + echoMix + echoInput + firWrap + firClamp + output; }
};

struct SnesDspOutput
{
    int16_t mainL = 0;
    int16_t mainR = 0;
    int16_t voiceL[kNumVoices] = {};
    int16_t voiceR[kNumVoices] = {};
};

class SnesDsp
{
public:
    SnesDsp() { reset(); }

    // Power-on / reset state: every register 0 except FLG = 0xE0 (soft reset, mute, echo
    // writes disabled), ENDX 0, global counter 0, noise 0x4000, APU RAM cleared
    // (research "Registers", Ambiguity 15).
    void reset() noexcept;

    // Register file access through DSPADDR/DSPDATA semantics (7-bit address).
    void writeRegister(int address, uint8_t value) noexcept;
    uint8_t readRegister(int address) const noexcept { return regs[static_cast<size_t>(address & 0x7F)]; }

    // The 64 KiB APU RAM the DSP reads samples from and keeps its echo buffer in.
    uint8_t* ram() noexcept { return aram.data(); }
    const uint8_t* ram() const noexcept { return aram.data(); }

    // Runs one output sample (research "Order of operations per sample").
    void step(SnesDspOutput& out) noexcept;

    // ----- inspection (tests, driver) --------------------------------------------------------
    const Voice& voice(int v) const noexcept { return voices[static_cast<size_t>(v)]; }
    // True from a KON write until the envelope has released to 0 (or the voice never started).
    bool isVoiceSounding(int v) const noexcept;
    int globalCounter() const noexcept { return counter; }
    uint16_t noiseState() const noexcept { return noise; }
    const Echo& echo() const noexcept { return echoState; }
    uint64_t samplesElapsed() const noexcept { return sampleIndex; }
    const SaturationCounts& saturation() const noexcept { return saturationCounts; }
    void clearSaturation() noexcept { saturationCounts = {}; }

    // ----- pure hardware arithmetic, exposed for unit tests -------------------------------
    // Envelope/noise event test for a rate against the global counter.
    static bool rateFires(int rate, int counterValue) noexcept
    {
        return rate != 0 && ((counterValue + kRateOffset[rate]) % kRatePeriod[rate]) == 0;
    }
    // Gaussian interpolation of four 15-bit samples h0 (oldest) .. h3 with fraction d 0..255.
    static int32_t gaussianInterpolate(int32_t h0, int32_t h1, int32_t h2, int32_t h3, int d) noexcept;
    // One noise LFSR step: feedback bit0 ^ bit1 into bit 14.
    static uint16_t noiseStep(uint16_t n) noexcept
    {
        return static_cast<uint16_t>((n >> 1) | (((n << 14) ^ (n << 13)) & 0x4000));
    }
    // Pitch after modulation by the previous voice's 16-bit output (research "Pitch modulation").
    static int32_t modulatedPitch(int32_t pitch14, int32_t prevOut16) noexcept
    {
        return pitch14 + (((prevOut16 >> 5) * pitch14) >> 10);
    }
    // 8-tap echo FIR over 15-bit samples [0] oldest .. [7] newest, taps FIR0..FIR7. With
    // `counts`, a wrap of taps 0-6 and a clamp of tap 7 are counted.
    static int32_t firFilter(const int16_t history[8], const int8_t taps[8], SaturationCounts* counts = nullptr) noexcept;
    // Voice volume: (env15 * VxVOL) >> 6 (fullsnes; Ambiguity 18).
    static int32_t voiceVolume(int32_t env15, int8_t vol) noexcept { return (env15 * vol) >> 6; }
    // MVOL / EVOL / EFB products, clamped to 16 bits (Ambiguity 16 decision).
    static int32_t volumeProduct(int32_t sample16, int8_t vol) noexcept { return clamp16((sample16 * vol) >> 7); }
    // Echo buffer length in stereo entries for an EDL value (EDL 0 = one 4-byte entry).
    static int echoLengthForEdl(int edl) noexcept { return (edl & 15) == 0 ? 1 : (edl & 15) * 512; }

private:
    // clamp16 that counts the samples it changed.
    int32_t countedClamp16(int32_t v, uint32_t& counter) noexcept
    {
        const int32_t c = clamp16(v);
        counter += c != v ? 1u : 0u;
        saturationCounts.peak = std::max(saturationCounts.peak, v < 0 ? -v : v);
        return c;
    }
    int voiceReg(int v, int offset) const noexcept { return regs[static_cast<size_t>(v * 16 + offset)]; }
    uint16_t readDirectory(int v, bool loopEntry) const noexcept;
    void checkBlockEnd(int v) noexcept;
    void decodeGroup(int v, int slot) noexcept;
    void finishGroup(int v) noexcept;
    void updateEnvelope(int v) noexcept;

    std::array<uint8_t, 128> regs{};
    std::array<uint8_t, kAramSize> aram{};
    std::array<Voice, kNumVoices> voices{};
    Echo echoState;

    uint8_t konInternal = 0;     // KON bits waiting for the next poll
    int counter = 0;             // global rate counter, 0x77FF..0
    uint16_t noise = 0x4000;     // 15-bit LFSR
    uint64_t sampleIndex = 0;    // even samples poll KON/KOFF
    SaturationCounts saturationCounts;
};

} // namespace chipdsp::snes
