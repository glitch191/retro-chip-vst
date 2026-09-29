#pragma once

// SNES engine: the S-DSP chip core (SnesDsp) run at exactly 32000 Hz, a small software
// driver (SnesDriver) turning notes and parameters into register writes, the 64 KiB APU
// RAM budget shared by the BRR samples and the echo buffer, and band-limited resampling
// of every 32 kHz output sample to the host rate. See docs/ENGINE_SPECS.md ("SNES") and
// docs/research/snes.md.

#include "chipdsp/IChipEngine.h"
#include "chipdsp/snes/SnesDriver.h"
#include "chipdsp/snes/SnesDsp.h"
#include "chipdsp/util/BandLimitedStepSynth.h"
#include "chipdsp/util/Filters.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace chipdsp
{

class SnesDspEngine final : public IChipEngine
{
public:
    // Parameter ids (ParamDesc::id); keys in parentheses (docs/ENGINE_SPECS.md).
    enum Param : int
    {
        Sample = 0,       // sample 0..31
        AdsrEnable,       // adsr_enable 0..1
        Attack,           // attack 0..15
        Decay,            // decay 0..7
        SustainLevel,     // sustain_level 0..7
        SustainRate,      // sustain_rate 0..31
        GainMode,         // gain_mode 0..4
        GainValue,        // gain_value 0..127 (direct) / 0..31 (rates, clamped)
        ReleaseMode,      // release_mode 0..1
        ReleaseRate,      // release_rate 0..31
        Volume,           // volume 0..127
        Pan,              // pan -64..64
        Transpose,        // transpose -24..24
        FineTune,         // fine_tune -100..100 cents
        VibratoRate,      // vibrato_rate 0..15 ticks per half cycle
        VibratoDepth,     // vibrato_depth 0..64 pitch register units
        VibratoDelay,     // vibrato_delay 0..60 ticks
        NoiseEnable,      // noise_enable 0..1
        NoiseClock,       // noise_clock 0..31
        Pmon,             // pmon 0..1
        LoopOverride,     // loop_override 0..2
        EchoEnable,       // echo_enable 0..1
        EchoDelay,        // echo_delay 0..15
        EchoFeedback,     // echo_feedback -128..127
        EchoVolume,       // echo_volume -128..127
        FirPreset,        // fir_preset 0..7
        V1Echo,           // v1_echo .. v8_echo 0..1
        V2Echo,
        V3Echo,
        V4Echo,
        V5Echo,
        V6Echo,
        V7Echo,
        V8Echo,
        MainVolume,       // main_volume 0..127
        NumParams
    };

    static constexpr int kNumSampleSlots = snes::kNumSampleSlots;

    struct SampleInfo
    {
        bool loaded = false;
        int numBlocks = 0;          // 9-byte BRR blocks (16 samples each)
        int brrBytes = 0;
        int loopStartBlock = -1;    // -1 = one-shot
        float rootNote = 60.0f;
        double storedRate = 32000.0;
    };

    SnesDspEngine();
    ~SnesDspEngine() override;

    ChipId chipId() const noexcept override { return ChipId::Snes; }
    int numChannels() const noexcept override { return kSnesChannels; }
    ChannelInfo channelInfo(int channel) const noexcept override;
    double nativeSampleRate() const noexcept override { return snes::kSampleRate; }

    void prepare(double hostSampleRate, int maxBlockSize) override;
    std::span<const ParamDesc> parameterDescriptors() const noexcept override;

    // Encodes mono PCM to BRR (sources above 32 kHz are first resampled to 32 kHz). Returns
    // false when the slot is invalid or all loaded samples would exceed the APU RAM left by
    // the echo buffer of the current echo_delay. Resets the slot's loop point, keeps its root.
    bool loadSample(int slot, const float* mono, int numFrames, double sourceSampleRate) override;
    int numSampleSlots() const noexcept override { return kNumSampleSlots; }
    // Frees the slot's BRR bytes (the slot's directory entries point at the silent loop block).
    bool clearSample(int slot) override;
    // echo_delay sets the budget; setParameter() only stores atomics, so staging is the same.
    void stageParameter(int id, float value) noexcept override { setParameter(id, value); }

    // ----- SNES sample API (message thread) ---------------------------------------------------
    // Loop from block 'loopStartBlock' (0..numBlocks-1) to the end, or -1 for a one-shot. The
    // slot is re-encoded so that the loop block uses filter 0.
    bool setSampleLoop(int slot, int loopStartBlock);
    // MIDI note (fractional allowed) that plays the slot at its stored rate. Default 60.
    bool setSampleRootNote(int slot, float midiNote);
    // IChipEngine metadata hook: root note + loop start in source frames, converted to the
    // stored rate and rounded down to a 16-sample BRR block boundary.
    bool setSampleInfo(int slot, float rootNote, int loopStartFrame, double sourceSampleRate) override;
    SampleInfo sampleInfo(int slot) const;
    // BRR bytes still free for the current echo_delay.
    int freeSampleBytes() const;

    // ----- audio thread -----------------------------------------------------------------------
    void reset() noexcept override;
    void setParameter(int id, float value) noexcept override;
    float getParameter(int id) const noexcept override;
    void noteOn(int channel, float midiNote, float velocity) noexcept override;
    void noteOff(int channel) noexcept override;
    void setChannelPitch(int channel, float midiNote) noexcept override;
    bool isChannelActive(int channel) const noexcept override;
    void setClockStandard(ClockStandard) noexcept override {}   // the S-DSP has one clock
    void setRawOutput(bool raw) noexcept override;
    void renderBlock(float* mainL, float* mainR, float* const* channelOutsL, float* const* channelOutsR,
                     int numSamples) noexcept override;

    // Inspection for tests.
    const snes::SnesDsp& chip() const noexcept { return *dsp; }
    const snes::SnesDriver& driver() const noexcept { return drv; }

private:
    // APU RAM image of the sample area plus the slot table; two of them are flipped
    // between the message thread (writer) and the audio thread (reader).
    struct SampleBank
    {
        std::array<uint8_t, snes::kAramSize> image{};
        std::array<snes::SampleSlot, kNumSampleSlots> slots{};
        int dataEnd = snes::kSampleDataStart;
    };
    // Message-thread copy of each slot (source of every bank rebuild).
    struct MasterSlot
    {
        bool loaded = false;
        std::vector<int16_t> pcm15;
        std::vector<uint8_t> brr;
        int loopStartBlock = -1;
        float rootNote = 60.0f;
        double storedRate = 32000.0;
    };

    snes::SnesDriverParams readParams() const noexcept;
    int paramInt(int id) const noexcept;
    bool publish(const std::array<MasterSlot, kNumSampleSlots>& slots);
    static int totalBrrBytes(const std::array<MasterSlot, kNumSampleSlots>& slots);
    void syncSampleBank(bool force) noexcept;
    void renderChunk(float* mainL, float* mainR, float* const* chL, float* const* chR, int offset, int n) noexcept;

    std::unique_ptr<snes::SnesDsp> dsp;
    snes::SnesDriver drv;
    std::array<std::atomic<float>, NumParams> params{};

    // Sample banks: bankState = (generation << 1) | active index; bankReading = index the
    // audio thread is copying from, or -1.
    std::unique_ptr<SampleBank> banks[2];
    std::atomic<uint32_t> bankState{ 0 };
    std::atomic<int> bankReading{ -1 };
    uint32_t bankStateCopied = 0xFFFFFFFFu;
    std::array<snes::SampleSlot, kNumSampleSlots> liveSlots{};
    int liveDataEnd = snes::kSampleDataStart;
    std::array<MasterSlot, kNumSampleSlots> master;

    // Output stage: one step synth per output, then the coupling-capacitor DC blocker.
    bool prepared = false;
    int maxBlock = 0;
    double hostPerNative = 1.0;
    double nativeTime = 0.0;       // block-relative host time of the next 32 kHz sample
    BandLimitedStepSynth mainSynth[2];
    OnePoleHighPass mainDc[2];
    int16_t mainLevel[2] = {};
    BandLimitedStepSynth voiceSynth[kSnesChannels][2];
    OnePoleHighPass voiceDc[kSnesChannels][2];
    int16_t voiceLevel[kSnesChannels][2] = {};
};

} // namespace chipdsp
