#pragma once

// Software sound driver for the 2A03 (ENGINE_SPECS.md "Common structure: chip core + driver").
//
// This is the part a game would run on the 6502: it turns note events and the engine
// parameters into APU register writes, once per video frame (60.0988 Hz NTSC, 50.0070 Hz
// PAL). Everything here is software behaviour (vibrato by period rewrites, pitch envelopes,
// software volume envelopes, triangle gating), labelled as such; the chip itself only sees
// register writes, so the hardware quantisation (11-bit periods, 4-bit volume, 16 noise
// periods) is always audible. See docs/research/nes.md "Implementation decisions".

#include "chipdsp/nes/NesApu.h"

#include <array>
#include <cstdint>

namespace chipdsp::nes
{

// Settings in native units, filled by Nes2A03Engine from its parameters.
struct PulseSettings
{
    int duty = 2, volume = 12, envEnable = 0, envLoop = 0;
    int sweepEnable = 0, sweepPeriod = 0, sweepNegate = 0, sweepShift = 0;
    int vibratoRate = 0, vibratoDepth = 0, vibratoDelay = 0;
    int swAttack = 0, swDecay = 0, swSustain = 15, swRelease = 0;
    int pitchEnvDepth = 0, pitchEnvSpeed = 0, transpose = 0;
};

struct TriangleSettings
{
    int linearLength = 127, gateFrames = 0, attackFrames = 0;
    int vibratoRate = 0, vibratoDepth = 0, vibratoDelay = 0;
    int pitchEnvDepth = 0, pitchEnvSpeed = 0, transpose = 0;
};

struct NoiseSettings
{
    int mode = 0, volume = 12, envEnable = 0, envLoop = 0, period = 8, keyed = 0;
    int swAttack = 0, swDecay = 0, swSustain = 15, swRelease = 0;
    int pitchEnvDepth = 0, pitchEnvSpeed = 0;
};

struct DmcSettings
{
    int rate = 15, sample = 0, loop = 0, keyed = 0, directLevel = 0;
};

struct DriverSettings
{
    std::array<PulseSettings, 2> pulse {};
    TriangleSettings triangle {};
    NoiseSettings noise {};
    DmcSettings dmc {};
    int frameMode = 0;   // $4017 bit 7: 0 = 4-step sequence, 1 = 5-step
};

// A DMC sample as mapped at $C000: 'capacity' readable bytes, 'length' = L * 16 + 1 bytes
// to play (0 = empty slot).
struct DmcSampleRef
{
    const uint8_t* bytes = nullptr;
    int capacity = 0;
    int length = 0;
};

// Software ADSR in frames on a 4-bit volume (driver behaviour).
struct SoftwareEnvelope
{
    enum class Stage : uint8_t { Off, Attack, Decay, Sustain, Release };

    Stage stage = Stage::Off;
    int frame = 0;          // frames since the current stage started
    int peak = 0;           // 0..15
    int releaseFrom = 0;
    int level = 0;          // last computed volume 0..15

    void start(int peakVolume) noexcept { stage = Stage::Attack; frame = 0; peak = peakVolume; level = 0; }
    void release() noexcept { releaseFrom = level; stage = Stage::Release; frame = 0; }
    // Volume for the current frame; attack/decay/release are lengths in frames, sustain 0..15
    // is a fraction of the peak (15 = peak).
    int compute(int attack, int decay, int sustain, int releaseFrames) noexcept;
};

class NesDriver
{
public:
    static constexpr int kLengthIndexLongest = 1; // kLengthTable[1] = 254 half frames

    // APU initialisation like NESdev "APU basics" (research "Registers", A15).
    void reset(NesApu& apu) noexcept;

    void noteOn(NesApu& apu, int channel, float midiNote, float velocity, const DriverSettings& s,
                const DmcSampleRef& dmcSample) noexcept;
    void noteOff(NesApu& apu, int channel, const DriverSettings& s) noexcept;
    void setPitch(int channel, float midiNote) noexcept;

    // One driver frame (called from the CPU-cycle loop once per video frame).
    void tick(NesApu& apu, const DriverSettings& s) noexcept;

    // Called by the engine right after noteOn() when the next frame tick is less than half a
    // frame away: that tick leaves the voice alone, so its first software step (envelope,
    // vibrato, pitch envelope, triangle delay) lasts 0.5..1.5 frames, one frame on average,
    // like a game driver that handles note commands inside its frame tick.
    void deferFirstTick(int channel) noexcept;

    bool isGateOn(int channel) const noexcept;
    // True while the driver itself will still raise a silent channel's level: software attack
    // in progress, triangle attack_frames delay, triangle gate_frames retrigger with the key
    // held. Used by isChannelActive() next to the hardware state (research "Note off").
    bool isAttackPending(int channel, const DriverSettings& s) const noexcept;

    // Helpers shared with the tests.
    static int pitchEnvelopeOffset(int depth, int speed, int frame) noexcept;
    static int vibratoOffset(int rate, int depth, int delay, int frame) noexcept;
    static int noiseIndexForNote(float midiNote) noexcept; // note 36 + n -> 15 - n, clamped
    static int dmcRateForNote(int baseRate, float midiNote) noexcept; // base + (note - 60), clamped

private:
    struct ToneVoice
    {
        bool gate = false;
        bool active = false;        // the driver still writes registers for this voice
        float note = 60.0f;
        float velocity = 1.0f;
        int frame = 0;              // frames since the note started sounding
        int delayFrames = 0;        // triangle attack_frames still to wait
        bool pendingStart = false;
        bool skipTick = false;      // see deferFirstTick()
        bool refused = false;       // base period above $7FF: the note is kept silent (A16)
        SoftwareEnvelope env;
        int lastHigh = -1;          // last timer high bits written ($4003 / $4007 / $400B)
        int lastLow = -1;
        int lastCtrl = -1;
        int lastSweep = -1;
    };

    void updatePulse(NesApu& apu, int index, const PulseSettings& s, bool noteStart) noexcept;
    void updateTriangle(NesApu& apu, const TriangleSettings& s, bool noteStart) noexcept;
    void updateNoise(NesApu& apu, const NoiseSettings& s, bool noteStart) noexcept;
    void silenceTriangle(NesApu& apu) noexcept;
    void writeStatus(NesApu& apu, uint8_t channelMask) noexcept;

    std::array<ToneVoice, 4> voices {}; // pulse 1, pulse 2, triangle, noise
    bool dmcGate = false;
    int frameModeWritten = 0;   // sequence mode last written to $4017
};

} // namespace chipdsp::nes
