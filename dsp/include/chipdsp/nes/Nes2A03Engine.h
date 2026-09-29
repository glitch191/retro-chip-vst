#pragma once

// NES engine: 2A03 / 2A07 APU core (NesApu) + software driver (NesDriver), resampled with
// BandLimitedStepSynth. Contract: chipdsp/IChipEngine.h. Parameters: docs/ENGINE_SPECS.md
// "NES". Hardware reference: docs/research/nes.md.

#include "chipdsp/IChipEngine.h"
#include "chipdsp/nes/NesApu.h"
#include "chipdsp/nes/NesDriver.h"
#include "chipdsp/util/BandLimitedStepSynth.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace chipdsp
{

class Nes2A03Engine final : public IChipEngine
{
public:
    // Parameter ids (stable; ParamDesc::key in parentheses).
    enum Param : int
    {
        Clock = 0,            // clock 0..1 (NTSC / PAL)
        ConsoleFilter,        // console_filter 0..1

        // Pulse 1 (p1_*); Pulse 2 uses the same layout at P2Duty = P1Duty + kPulseParamStride.
        P1Duty, P1Volume, P1EnvEnable, P1EnvLoop,
        P1SweepEnable, P1SweepPeriod, P1SweepNegate, P1SweepShift,
        P1VibratoRate, P1VibratoDepth, P1VibratoDelay,
        P1SwAttack, P1SwDecay, P1SwSustain, P1SwRelease,
        P1PitchEnvDepth, P1PitchEnvSpeed, P1Transpose,

        P2Duty, P2Volume, P2EnvEnable, P2EnvLoop,
        P2SweepEnable, P2SweepPeriod, P2SweepNegate, P2SweepShift,
        P2VibratoRate, P2VibratoDepth, P2VibratoDelay,
        P2SwAttack, P2SwDecay, P2SwSustain, P2SwRelease,
        P2PitchEnvDepth, P2PitchEnvSpeed, P2Transpose,

        // Triangle (tri_*)
        TriLinearLength, TriGateFrames, TriAttackFrames,
        TriVibratoRate, TriVibratoDepth, TriVibratoDelay,
        TriPitchEnvDepth, TriPitchEnvSpeed, TriTranspose,

        // Noise (nz_*)
        NzMode, NzVolume, NzEnvEnable, NzEnvLoop, NzPeriod, NzKeyed,
        NzSwAttack, NzSwDecay, NzSwSustain, NzSwRelease,
        NzPitchEnvDepth, NzPitchEnvSpeed,

        // DMC (dmc_*)
        DmcRate, DmcSample, DmcLoop, DmcKeyed, DmcDirectLevel,

        NumParams
    };
    static constexpr int kPulseParamStride = P2Duty - P1Duty;

    static constexpr int kNumSampleSlots = 16;
    static constexpr int kSlotCapacity = 4096;           // >= 4081 bytes (hardware maximum)
    static constexpr uint8_t kDmcEncoderStartLevel = 64; // = dmc_direct_level default (see research decisions)

    Nes2A03Engine();

    ChipId chipId() const noexcept override { return ChipId::Nes; }
    int numChannels() const noexcept override { return kNesChannels; }
    ChannelInfo channelInfo(int channel) const noexcept override;
    double nativeSampleRate() const noexcept override;

    void prepare(double hostSampleRate, int maxBlockSize) override;
    std::span<const ParamDesc> parameterDescriptors() const noexcept override;
    bool loadSample(int slot, const float* mono, int numFrames, double sourceSampleRate) override;
    int numSampleSlots() const noexcept override { return kNumSampleSlots; }

    void reset() noexcept override;
    void setParameter(int id, float value) noexcept override;
    float getParameter(int id) const noexcept override;
    void noteOn(int channel, float midiNote, float velocity) noexcept override;
    void noteOff(int channel) noexcept override;
    void setChannelPitch(int channel, float midiNote) noexcept override;
    bool isChannelActive(int channel) const noexcept override;
    void setClockStandard(ClockStandard standard) noexcept override;
    void setRawOutput(bool raw) noexcept override;
    void renderBlock(float* mainL, float* mainR, float* const* channelOutsL, float* const* channelOutsR,
                     int numSamples) noexcept override;

    // 1-bit delta encoder (PCM -> DMC bytes, LSB first). The encoder tracks the hardware
    // counter from 'startLevel': bit = 1 when the target level is above the counter, and the
    // counter follows the hardware rule (+/-2, a step that would leave 0..127 is skipped).
    // Returns the number of bytes written, or -1 if more than maxBytes would be needed.
    static int encodeDmc(const float* mono, int numFrames, double sourceSampleRate, double bitRateHz,
                         uint8_t startLevel, uint8_t* out, int maxBytes);
    // Playable length for n encoded bytes: the smallest L * 16 + 1 >= n.
    static int paddedDmcLength(int numBytes) noexcept;

    // Read-only access for tests and visualisation.
    const nes::NesApu& apu() const noexcept { return chip; }
    int sampleLength(int slot) const noexcept;

private:
    struct SampleBank
    {
        std::array<uint8_t, kNumSampleSlots * kSlotCapacity> data {};
        std::array<int, kNumSampleSlots> length {};    // playable bytes (L * 16 + 1), 0 = empty
    };

    struct Levels
    {
        uint8_t p1 = 0, p2 = 0, tri = 0, noise = 0, dmc = 0;
        bool triUltrasonic = false;
        bool operator==(const Levels&) const = default;
    };

    void applyParam(int id, int value) noexcept;
    void applyRegion(bool pal) noexcept;
    Levels readLevels() const noexcept;
    float soloLevel(int channel, const Levels& lv) const noexcept;
    int claimActiveBank() noexcept;
    nes::DmcSampleRef sampleRef(int bank, int slot) const noexcept;
    void renderChunk(float* mainL, float* mainR, float* const* channelOutsL, float* const* channelOutsR,
                     int offset, int numSamples) noexcept;

    std::array<std::atomic<float>, NumParams> params;
    nes::DriverSettings settings;

    nes::NesApu chip;
    nes::NesDriver driver;
    nes::NesMixer mixer;

    double hostRate = 48000.0;
    int maxBlock = 0;
    double hostSamplesPerClock = 48000.0 / nes::kCpuHzNtsc;
    double clockTime = 0.0;          // host-sample time of the next CPU cycle (block-relative)
    uint32_t driverPhase = 0;        // half CPU cycles since the last driver tick
    uint32_t driverPeriod = nes::kFrameHalfCyclesNtsc;
    bool raw = false;

    BandLimitedStepSynth mainSynth;
    nes::NesOutputStage mainStage;
    std::array<BandLimitedStepSynth, kNesChannels> channelSynths;
    std::array<nes::NesOutputStage, kNesChannels> channelStages;
    std::vector<float> scratch;
    bool channelsRendered = false;

    Levels lastLevels;
    float lastMix = 0.0f;
    std::array<float, kNesChannels> lastSolo {};

    // Three sample banks. The DMC reads only the bank it was mapped to at its last note-on
    // ('mappedBank', published by the audio thread and kept until the next DMC note-on, so a
    // playing sample never changes under it). loadSample() (one message thread) writes a bank
    // that is neither active nor mapped (with three banks one always exists, no waiting), then
    // flips 'activeBank'. Research "DMC samples".
    static constexpr int kNumBanks = 3;
    std::unique_ptr<SampleBank[]> banks;
    std::atomic<int> activeBank { 0 };
    std::atomic<int> mappedBank { -1 };
};

} // namespace chipdsp
