#pragma once

#include "chipdsp/IChipEngine.h"
#include "chipdsp/util/BandLimitedStepSynth.h"
#include "chipdsp/util/Filters.h"

#include <array>
#include <atomic>
#include <vector>

namespace chipdsp
{

// Placeholder engine used while the real chip engines are being written, and kept as a
// test double for the plugin layer. It plays a naive square wave per channel through the
// shared resampler so that MIDI routing, buses, state and UI can be exercised end to end.
// It is never selected in release builds once the real engines exist (see EngineFactory.cpp).
class StubEngine final : public IChipEngine
{
public:
    enum Param : int
    {
        Volume = 0,      // 0..15
        Duty,            // 0..3
        AttackMs,        // 0..2000
        ReleaseMs,       // 0..2000
        Clock,           // 0..1
        NumParams
    };

    explicit StubEngine(ChipId chip);

    ChipId chipId() const noexcept override { return chip; }
    int numChannels() const noexcept override { return channelCount; }
    ChannelInfo channelInfo(int channel) const noexcept override;
    double nativeSampleRate() const noexcept override { return 1789773.0; }

    void prepare(double hostSampleRate, int maxBlockSize) override;
    std::span<const ParamDesc> parameterDescriptors() const noexcept override;
    bool loadSample(int, const float*, int, double) override { return false; }
    int numSampleSlots() const noexcept override { return 0; }

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

private:
    struct Channel
    {
        bool gate = false;
        float note = 60.0f;
        float velocity = 0.0f;
        float env = 0.0f;
        double phase = 0.0;      // cycles
        int level = 0;           // 0 or 1 (square)
        BandLimitedStepSynth synth;
        OnePoleHighPass dc;
    };

    ChipId chip;
    int channelCount;
    double sampleRate = 48000.0;
    std::array<std::atomic<float>, NumParams> params{};
    std::array<Channel, kMaxHardwareChannels> channels;
    std::vector<float> scratch;
    BandLimitedStepSynth mainSynth;
    OnePoleHighPass mainDc;
    bool raw = false;
};

} // namespace chipdsp
