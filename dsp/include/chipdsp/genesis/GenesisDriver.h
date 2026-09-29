#pragma once

// Software sound driver of the Genesis engine: the small program a game runs on the
// 68000/Z80. It turns note events and engine settings into register writes, exactly the
// way a driver would: immediate writes for note on/off (key off, patch, $A4 then $A0,
// key on through $28), and once-per-video-frame updates for everything software games
// implemented in code (vibrato, PSG volume envelopes, unison, pitch slides, DAC streaming).
// See docs/research/genesis.md "Implementation decisions".
//
// Nothing here is chip behaviour; the quantisation and quirks all come from the cores.

#include "chipdsp/genesis/Sn76489Core.h"
#include "chipdsp/genesis/Ym2612Core.h"

#include <cstdint>

namespace chipdsp::genesis
{

struct OperatorPatch
{
    int tl = 0, ar = 31, dr = 0, sr = 0, rr = 15, sl = 0, mul = 1, dt = 0, rs = 0, am = 0, ssg = 0;
};

// Snapshot of the engine parameters, in native register units (see GenesisEngine.h).
struct DriverSettings
{
    ClockStandard clock = ClockStandard::Ntsc;
    int lfoEnable = 0, lfoFreq = 0;

    int algorithm = 4, feedback = 0, ams = 0, fms = 0;
    int transpose = 0, fineTune = 0;
    int vibratoRate = 0, vibratoDepth = 0, vibratoDelay = 0;
    int unisonDetune = 0;
    int pan[6] = { 1, 1, 1, 1, 1, 1 };
    int velocityDepth = 0;
    OperatorPatch op[4];

    int dacEnable = 0, dacSample = 0, dacRate = 16000, dacKeyed = 0, dacLoop = 0, dacVolume = 127;

    int psgAtt[4] = { 0, 0, 0, 0 };
    int noiseMode = 1, noiseRate = 0;
    int psgAttack = 0, psgDecay = 0, psgSustain = 15, psgRelease = 0;
    int psgVibratoRate = 0, psgVibratoDepth = 0;
    int psgUnisonDetune = 0, psgTranspose = 0;
};

// Read-only view of one DAC sample slot (8-bit unsigned PCM).
struct DacSampleView
{
    const uint8_t* data = nullptr;
    int length = 0;
};

constexpr int kDacSlots = 16;
constexpr int kDacMaxBytes = 65536;
constexpr int kDacChannel = 5;       // the DAC replaces FM channel 6

class GenesisDriver
{
public:
    void attach(Ym2612Core* ymCore, Sn76489Core* psgCore) noexcept { ym = ymCore; psg = psgCore; }

    // Power both chips on and run the driver's init sequence.
    void reset(const DriverSettings& s) noexcept;
    void setSettings(const DriverSettings& s) noexcept { settings = s; }
    const DriverSettings& currentSettings() const noexcept { return settings; }
    void setDacBank(const DacSampleView* slots) noexcept;

    // Channel 0..5 FM (5 = DAC when enabled), 6..8 PSG tone, 9 PSG noise.
    void noteOn(int channel, float midiNote, float velocity) noexcept;
    void noteOff(int channel) noexcept;
    void setPitch(int channel, float midiNote) noexcept;   // applied at the next frame
    bool isActive(int channel) const noexcept;

    // Once per video frame (research "Clocks and rates": 59.92 Hz NTSC / 49.70 Hz PAL).
    void frameTick() noexcept;

    // The engine calls this right after noteOn() when the next frame tick is less than half a
    // frame away: that tick then leaves the channel's PSG software envelope alone, so the
    // first envelope step lasts 0.5..1.5 frames (one frame on average), as in a game driver
    // that handles note commands inside its frame tick. Without it a note keyed just before
    // a tick loses its first step (a 2-frame hat would play one frame).
    void deferFirstTick(int channel) noexcept;

    // DAC streaming: the engine calls dacWriteNext() every dacIntervalMasterClocks() while
    // dacPlaying(); takeDacRestart() reports a new sample start (resets the write clock).
    bool dacPlaying() const noexcept { return dac.playing; }
    double dacIntervalMasterClocks() const noexcept { return dac.interval; }
    bool takeDacRestart() noexcept
    {
        const bool r = dac.restart;
        dac.restart = false;
        return r;
    }
    void dacWriteNext() noexcept;

    // Driver helpers exposed for tests.
    static int vibratoOffset(int frames, int rate, int depth, int delay) noexcept;
    static int velocityToTl(float velocity, int depth) noexcept;
    static int velocityToPsgAttenuation(float velocity) noexcept;
    FmPitch fmPitchFor(int channel) const noexcept;
    int psgPeriodFor(int channel) const noexcept;

private:
    enum class EnvStage : uint8_t { Off, Attack, Decay, Sustain, Release };

    struct Voice
    {
        bool gate = false;       // note held
        bool started = false;    // FM: keyed at least once since reset (release may still sound)
        float note = 60.0f;
        float velocity = 1.0f;
        int frames = 0;          // frames since note on
        int detune = 0;          // unison offset (fnum units for FM, period units for PSG)
        int partner = -1;        // unison channel driven by this voice
        int owner = -1;          // voice whose unison partner this channel is
        // PSG software envelope
        EnvStage stage = EnvStage::Off;
        int stageFrames = 0;
        int level = 0;           // 0..15, 15 = full
        int releaseFrom = 0;
        bool skipTick = false;   // see deferFirstTick()
    };

    struct DacState
    {
        bool playing = false;
        bool restart = false;
        bool gate = false;
        bool loop = false;
        int slot = 0;
        int pos = 0;
        int gain = 127;          // 0..127
        double interval = 3355.0;
    };

    // register writers (the driver keeps a shadow copy, like most sound drivers do)
    void writeYm(int bank, int reg, int value) noexcept;
    void writeYmForced(int bank, int reg, int value) noexcept;
    void writeFmPatch(int channel) noexcept;
    void writeFmFrequency(int channel, FmPitch pitch, bool force) noexcept;
    void writeGlobals() noexcept;
    void writePsgPeriod(int tone, int period) noexcept;
    void writePsgAttenuation(int ch, int att) noexcept;
    void writeNoiseControl(bool force) noexcept;

    void fmNoteOn(int channel) noexcept;
    void fmKeyOff(int channel) noexcept;
    void psgNoteOn(int channel) noexcept;
    void psgUpdate(int channel) noexcept;
    void takeOverTone3() noexcept;
    void dacNoteOn(float midiNote, float velocity) noexcept;
    void dacStop() noexcept;

    void releaseVoice(int channel) noexcept;
    void detachUnison(int channel) noexcept;
    int findFreeFm(int from) const noexcept;
    int findFreeTone(int from) const noexcept;
    bool dacMode() const noexcept { return settings.dacEnable != 0; }

    void envStart(Voice& v) noexcept;
    void envEnterDecay(Voice& v) noexcept;
    void envRelease(Voice& v) noexcept;
    void envTick(Voice& v) noexcept;
    int psgAttenuationFor(int channel) const noexcept;

    Ym2612Core* ym = nullptr;
    Sn76489Core* psg = nullptr;
    DriverSettings settings;
    const DacSampleView* dacBank = nullptr;

    Voice voices[10];
    DacState dac;
    bool noiseOwnsTone3 = false;   // noise in "tone 3" rate mode drives tone 3's period

    int16_t ymShadow[2][256] = {};
    int fmBlockFnum[6] = { -1, -1, -1, -1, -1, -1 };
    int psgPeriodShadow[3] = { -1, -1, -1 };
    int psgAttShadow[4] = { -1, -1, -1, -1 };
    int noiseShadow = -1;
};

} // namespace chipdsp::genesis
