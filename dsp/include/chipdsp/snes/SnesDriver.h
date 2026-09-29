#pragma once

// Software driver for the S-DSP: the part a game's SPC700 sound program would do. It turns
// note events and the engine parameters into register writes and never touches the chip's
// internal state directly, so every hardware quantisation stays audible.
// Behaviour that is software (not chip) is marked "driver" in the comments and listed in
// docs/research/snes.md, "Implementation decisions".

#include "chipdsp/snes/SnesDsp.h"

#include <cstdint>

namespace chipdsp::snes
{

inline constexpr int kNumSampleSlots = 32;

// ----- APU RAM map used by the driver (research "Implementation decisions") -----------------
inline constexpr int kDirPage = 0x02;                 // DIR: directory at 0x0200 (64 entries)
inline constexpr int kDirAddress = kDirPage * 0x100;
inline constexpr int kEchoEdl0Page = 0x03;            // 4-byte echo buffer used when EDL = 0
inline constexpr int kSilentLoopBlock = 0x0304;       // code 3 block of zeros looping to itself
inline constexpr int kSilentEndBlock = 0x030D;        // code 1 block: releases the voice
inline constexpr int kSampleDataStart = 0x0400;       // BRR data packed from here upwards
// Directory entries: slot s uses entry 2s (one-shot: loop address = kSilentEndBlock) and
// entry 2s + 1 (looped: loop address = the slot's loop block).

// First byte of the echo buffer for an EDL value (EDL >= 1 sits at the top of RAM).
inline constexpr int echoBufferStart(int edl) noexcept { return edl == 0 ? kEchoEdl0Page * 0x100 : 0x10000 - edl * 2048; }
// Bytes available for BRR data when the echo buffer uses 'edl'.
inline constexpr int sampleCapacityBytes(int edl) noexcept { return (edl == 0 ? 0x10000 : echoBufferStart(edl)) - kSampleDataStart; }

// Sample slot as the driver sees it (a copy taken from the active sample bank).
struct SampleSlot
{
    bool loaded = false;
    bool hasLoop = false;
    float rootNote = 60.0f;        // MIDI note that plays at the stored rate
    double storedRate = 32000.0;   // sample rate of the BRR data
    int startAddress = kSilentLoopBlock;
    int numBlocks = 0;
    int loopStartBlock = -1;
};

// Engine parameters in native units, read once per block by the engine.
struct SnesDriverParams
{
    int sample = 0;
    int adsrEnable = 1, attack = 15, decay = 7, sustainLevel = 7, sustainRate = 0;
    int gainMode = 0, gainValue = 127;
    int releaseMode = 0, releaseRate = 31;
    int volume = 100, pan = 0, transpose = 0, fineTune = 0;
    int vibratoRate = 0, vibratoDepth = 0, vibratoDelay = 0;
    int noiseEnable = 0, noiseClock = 0, pmon = 0, loopOverride = 0;
    int echoEnable = 0, echoDelay = 0, echoFeedback = 0, echoVolume = 0, firPreset = 0;
    int eonMask = 0;
    int mainVolume = 127;
};

class SnesDriver
{
public:
    // Driver tick: 4 ms = 128 samples at 32 kHz, like a typical SPC700 driver timer.
    static constexpr int kSamplesPerTick = 128;
    // Driver echo re-initialisation wait (research "Echo": EDL applies only when the index
    // wraps, drivers wait 240 ms with echo writes disabled): 60 ticks.
    static constexpr int kEchoWaitTicks = 60;

    // 14-bit pitch register for a (fractional) MIDI note: P = round(4096 * rate / 32000 *
    // 2^((note - root) / 12)), clamped to 0..0x3FFF (research "Pitch counter").
    static int pitchRegister(float midiNote, float rootNote, double storedRate) noexcept;
    // Driver vibrato offset in pitch-register units for the tick 'tickPhase' (0 = first
    // vibrato tick): triangle of +/- depth, one half cycle every 'rate' ticks.
    static int vibratoOffset(int tickPhase, int rate, int depth) noexcept;
    // VxVOL left/right for a volume 0..127, a velocity 0..1 and a pan -64..64 (driver).
    static void voiceVolumes(int volume, float velocity, int pan, int& left, int& right) noexcept;
    // ADSR1, ADSR2 and GAIN register values for the instrument (driver).
    static uint8_t adsr1Value(const SnesDriverParams& p) noexcept;
    static uint8_t adsr2Value(const SnesDriverParams& p) noexcept;
    static uint8_t gainValue(const SnesDriverParams& p) noexcept;
    // Largest EDL whose buffer does not overlap BRR data ending at 'sampleDataEnd'.
    static int maxEchoDelay(int sampleDataEnd) noexcept;

    // Programs the freshly reset chip (DIR, volumes, echo, FLG) from the parameters.
    void reset(SnesDsp& dsp, const SnesDriverParams& p, int sampleDataEnd) noexcept;

    // The instrument part of 'p' (sample, loop override, envelope, release, volume, pan) is
    // latched for the note here; pitch, vibrato and the global registers follow the
    // parameters live at every tick.
    void noteOn(int channel, float midiNote, float velocity, const SnesDriverParams& p) noexcept;
    void noteOff(int channel) noexcept;
    void setPitch(int channel, float midiNote) noexcept;
    bool isActive(int channel, const SnesDsp& dsp) const noexcept;

    // Called before every DSP sample: flushes pending key events and runs the 4 ms tick.
    void beforeSample(SnesDsp& dsp, const SnesDriverParams& p, const SampleSlot* slots) noexcept;

    // New sample data was copied into APU RAM: disable echo writes at once if the current
    // echo buffer would overlap the new data (the next tick moves it).
    void onSampleMemoryChanged(SnesDsp& dsp, int sampleDataEnd) noexcept;

    int programmedEchoDelay() const noexcept { return edl; }
    bool echoWritesEnabled() const noexcept { return echoRunning; }

private:
    struct Channel
    {
        float note = 60.0f;
        float velocity = 1.0f;
        bool gainRelease = false;  // released by a GAIN write, waiting for envelope 0
        int ticks = 0;             // ticks since key-on (vibrato delay)
        int vibratoPhase = 0;
        int pitch = -1;            // last pitch register written
        float rootNote = 60.0f;    // of the sample keyed on
        double storedRate = 32000.0;
        SnesDriverParams instrument;   // latched at noteOn
    };

    void flushKeyOffs(SnesDsp& dsp) noexcept;
    void flushKeyOns(SnesDsp& dsp, const SnesDriverParams& p, const SampleSlot* slots) noexcept;
    void tick(SnesDsp& dsp, const SnesDriverParams& p) noexcept;
    void writeGlobals(SnesDsp& dsp, const SnesDriverParams& p) noexcept;
    void updateEcho(SnesDsp& dsp, const SnesDriverParams& p) noexcept;
    void programEchoBuffer(SnesDsp& dsp, int newEdl) noexcept;
    void clearEchoBuffer(SnesDsp& dsp) noexcept;
    int channelPitch(const Channel& c, const SnesDriverParams& p, int vibratoOffset) const noexcept;

    Channel channels[kNumVoices];
    uint8_t pendingKon = 0;
    uint8_t pendingKoff = 0;
    uint8_t koffMask = 0;          // value kept in the KOFF register
    int samplesSinceKon = 2;
    int tickCounter = 0;

    int dataEnd = kSampleDataStart;
    int edl = 0;                   // EDL currently programmed
    int echoWait = 0;              // ticks left before echo writes may be enabled
    bool echoRunning = false;      // echo writes enabled and EVOL applied
};

} // namespace chipdsp::snes
