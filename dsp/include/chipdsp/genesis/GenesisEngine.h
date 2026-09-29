#pragma once

// Sega Genesis engine: YM2612 (6 FM channels, DAC on channel 6) + SN76489 PSG (3 tones +
// noise), driven by GenesisDriver once per video frame. Both cores run at their native rate
// and feed BandLimitedStepSynth instances; the output stage models the 9-bit DAC with the
// optional ladder effect, the PSG/FM mix ratio, the coupling capacitor and the Model 1
// low-pass. See docs/ENGINE_SPECS.md and docs/research/genesis.md.
//
// Channels: 0..5 FM1..FM6, 6..8 PSG1..PSG3, 9 PSG noise.

#include "chipdsp/IChipEngine.h"
#include "chipdsp/genesis/GenesisDriver.h"
#include "chipdsp/genesis/Sn76489Core.h"
#include "chipdsp/genesis/Ym2612Core.h"
#include "chipdsp/util/BandLimitedStepSynth.h"
#include "chipdsp/util/Filters.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace chipdsp
{

class GenesisEngine final : public IChipEngine
{
public:
    // Parameter ids (native units, see parameterDescriptors()).
    enum ParamId : int
    {
        // Global
        Clock = 0,
        ChipRevision,
        Model1Lowpass,
        LfoEnable,
        LfoFreq,
        // FM patch (all FM channels)
        Algorithm,
        Feedback,
        Ams,
        Fms,
        Transpose,
        FineTune,
        VibratoRate,
        VibratoDepth,
        VibratoDelay,
        UnisonDetune,
        Fm1Pan,
        Fm2Pan,
        Fm3Pan,
        Fm4Pan,
        Fm5Pan,
        Fm6Pan,
        VelocityDepth,
        // Operators S1..S4: NumOpFields consecutive ids each, see opParam()
        Op1Tl,
        OpParamsEnd = Op1Tl + 4 * 11,
        // DAC (channel 6)
        DacEnable = OpParamsEnd,
        DacSample,
        DacRate,
        DacKeyed,
        DacLoop,
        DacVolume,
        // PSG
        Psg1Att,
        Psg2Att,
        Psg3Att,
        PsgnAtt,
        PsgNoiseMode,
        PsgNoiseRate,
        PsgSwAttack,
        PsgSwDecay,
        PsgSwSustain,
        PsgSwRelease,
        PsgVibratoRate,
        PsgVibratoDepth,
        PsgUnisonDetune,
        PsgTranspose,
        NumParams
    };

    enum OpField : int { OpTl = 0, OpAr, OpDr, OpSr, OpRr, OpSl, OpMul, OpDt, OpRs, OpAm, OpSsg, NumOpFields };
    static_assert(NumOpFields == 11);

    // op = 0..3 (S1..S4)
    static constexpr int opParam(int op, int field) noexcept { return Op1Tl + op * NumOpFields + field; }

    GenesisEngine();

    ChipId chipId() const noexcept override { return ChipId::Genesis; }
    int numChannels() const noexcept override { return kGenesisChannels; }
    ChannelInfo channelInfo(int channel) const noexcept override;
    double nativeSampleRate() const noexcept override;

    void prepare(double hostSampleRate, int maxBlockSize) override;
    std::span<const ParamDesc> parameterDescriptors() const noexcept override;

    bool loadSample(int slot, const float* mono, int numFrames, double sourceSampleRate) override;
    int numSampleSlots() const noexcept override { return genesis::kDacSlots; }
    bool clearSample(int slot) override;
    // dac_rate drives the encoder; setParameter() only stores atomics, so staging is the same.
    void stageParameter(int id, float value) noexcept override { setParameter(id, value); }

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

    // Inspection (tests, visualisation).
    const genesis::Ym2612Core& ym2612() const noexcept { return ym; }
    const genesis::Sn76489Core& sn76489() const noexcept { return psg; }
    const genesis::GenesisDriver& driver() const noexcept { return drv; }

private:
    // Coupling capacitor (DC block) then the optional Model 1 RC low-pass (genesis::rcLowPass).
    // The low-pass always runs so that switching it on does not start from a stale state.
    struct OutputFilters
    {
        OnePoleHighPass dc;
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f;
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f;
        void prepare(double sampleRate) noexcept;
        void reset() noexcept
        {
            dc.reset();
            x1 = x2 = y1 = 0.0f;
        }
        float process(float x, bool lowPassOn) noexcept
        {
            const float y = dc.process(x);
            const float z = b0 * y + b1 * x1 + b2 * x2 - a1 * y1;
            x2 = x1;
            x1 = y;
            y1 = z;
            return lowPassOn ? z : y;
        }
    };

    struct DacBank
    {
        std::vector<uint8_t> data[genesis::kDacSlots];
        genesis::DacSampleView view[genesis::kDacSlots];
    };

    int paramInt(int id) const noexcept;
    genesis::DriverSettings readSettings() const noexcept;
    void refreshDriver() noexcept;
    void applyClock() noexcept;
    void renderChunk(float* mainL, float* mainR, float* const* outsL, float* const* outsR, int numSamples) noexcept;
    void emitFm(double hostTime, bool perChannel) noexcept;
    void emitPsg(double hostTime, bool perChannel) noexcept;
    // Start the level trackers at the chip's current output (idle DC from the ladder offsets
    // and the PSG's unipolar levels) instead of 0, so that DC never enters the resamplers as a
    // step: that step would reach the output as a click at every reset or bus activation.
    void syncLevelTrackers(bool mainToo) noexcept;

    std::array<std::atomic<float>, NumParams> params;

    genesis::Ym2612Core ym;
    genesis::Sn76489Core psg;
    genesis::GenesisDriver drv;

    // DAC sample banks (ARCHITECTURE "Sample slots"): the loader writes the inactive bank and
    // flips activeBank. bankInUse is the bank the audio thread may be reading (-1 = none, between
    // blocks); the loader waits while it equals the bank it is about to overwrite.
    void acquireDacBank() noexcept;
    void releaseDacBank() noexcept { bankInUse.store(-1, std::memory_order_seq_cst); }

    DacBank banks[2];
    std::atomic<int> activeBank { 0 };
    std::atomic<int> bankInUse { -1 };

    double hostRate = 48000.0;
    int maxBlock = 0;
    ClockStandard clockStd = ClockStandard::Ntsc;
    double masterPerHost = 1.0;
    double hostPerMaster = 1.0;
    bool lowPassOn = true;
    bool raw = false;

    // Event times in master clocks (absolute, rebased periodically).
    double blockStart = 0.0;
    double nextFm = 0.0;
    double nextPsg = 0.0;
    double nextFrame = 0.0;
    double nextDac = 0.0;

    // Main output
    BandLimitedStepSynth fmSynthL, fmSynthR, psgSynth;
    int fmLevelL = 0, fmLevelR = 0, psgLevel = 0;
    OutputFilters mainFilterL, mainFilterR;

    // Per-channel outputs (only fed while the host asks for them)
    bool perChannelLive = false;
    BandLimitedStepSynth fmChSynthL[6], fmChSynthR[6], psgChSynth[4];
    int fmChLevelL[6] = {}, fmChLevelR[6] = {}, psgChLevel[4] = {};
    OutputFilters chFilterL[kGenesisChannels], chFilterR[kGenesisChannels];

    std::vector<float> scratch;
};

} // namespace chipdsp
