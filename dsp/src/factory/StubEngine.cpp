#include "chipdsp/factory/StubEngine.h"

#include <algorithm>
#include <cmath>

namespace chipdsp
{

namespace
{
    const char* const kDutyLabels[] = { "12.5 %", "25 %", "50 %", "75 %" };
    const char* const kClockLabels[] = { "NTSC", "PAL" };

    constexpr ParamDesc kDescs[] = {
        { StubEngine::Volume,    "volume",     "Volume",  "Tone",   0.0f, 15.0f,   12.0f,  true,  "",   nullptr },
        { StubEngine::Duty,      "duty",       "Duty",    "Tone",   0.0f, 3.0f,    2.0f,   true,  "",   kDutyLabels },
        { StubEngine::AttackMs,  "attack_ms",  "Attack",  "Envelope", 0.0f, 2000.0f, 5.0f, false, "ms", nullptr },
        { StubEngine::ReleaseMs, "release_ms", "Release", "Envelope", 0.0f, 2000.0f, 50.0f, false, "ms", nullptr },
        { StubEngine::Clock,     "clock",      "Clock",   "Global", 0.0f, 1.0f,    0.0f,   true,  "",   kClockLabels },
    };

    const char* const kNames[kMaxHardwareChannels] = { "Channel 1", "Channel 2", "Channel 3", "Channel 4", "Channel 5",
                                                       "Channel 6", "Channel 7", "Channel 8", "Channel 9", "Channel 10" };
    const char* const kShort[kMaxHardwareChannels] = { "C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8", "C9", "C10" };
} // namespace

StubEngine::StubEngine(ChipId c)
    : chip(c),
      channelCount(c == ChipId::Nes ? kNesChannels : c == ChipId::Snes ? kSnesChannels : kGenesisChannels)
{
    for (const auto& d : kDescs)
        params[static_cast<size_t>(d.id)].store(d.defaultValue, std::memory_order_relaxed);
}

ChannelInfo StubEngine::channelInfo(int channel) const noexcept
{
    const int i = std::clamp(channel, 0, kMaxHardwareChannels - 1);
    return { kNames[i], kShort[i], true };
}

void StubEngine::prepare(double hostSampleRate, int maxBlockSize)
{
    sampleRate = hostSampleRate;
    scratch.assign(static_cast<size_t>(maxBlockSize), 0.0f);
    for (auto& ch : channels)
    {
        ch.synth.prepare(nativeSampleRate(), hostSampleRate, maxBlockSize);
        ch.dc.prepare(5.0, hostSampleRate);
    }
    mainSynth.prepare(nativeSampleRate(), hostSampleRate, maxBlockSize);
    mainDc.prepare(5.0, hostSampleRate);
    reset();
}

std::span<const ParamDesc> StubEngine::parameterDescriptors() const noexcept
{
    return { kDescs, std::size(kDescs) };
}

void StubEngine::reset() noexcept
{
    for (auto& ch : channels)
    {
        ch.gate = false;
        ch.env = 0.0f;
        ch.phase = 0.0;
        ch.level = 0;
        ch.synth.reset();
        ch.dc.reset();
    }
    mainSynth.reset();
    mainDc.reset();
}

void StubEngine::setParameter(int id, float value) noexcept
{
    if (id >= 0 && id < NumParams)
        params[static_cast<size_t>(id)].store(value, std::memory_order_relaxed);
}

float StubEngine::getParameter(int id) const noexcept
{
    return (id >= 0 && id < NumParams) ? params[static_cast<size_t>(id)].load(std::memory_order_relaxed) : 0.0f;
}

void StubEngine::noteOn(int channel, float midiNote, float velocity) noexcept
{
    if (channel < 0 || channel >= channelCount)
        return;
    auto& ch = channels[static_cast<size_t>(channel)];
    ch.gate = true;
    ch.note = midiNote;
    ch.velocity = velocity;
}

void StubEngine::noteOff(int channel) noexcept
{
    if (channel >= 0 && channel < channelCount)
        channels[static_cast<size_t>(channel)].gate = false;
}

void StubEngine::setChannelPitch(int channel, float midiNote) noexcept
{
    if (channel >= 0 && channel < channelCount)
        channels[static_cast<size_t>(channel)].note = midiNote;
}

bool StubEngine::isChannelActive(int channel) const noexcept
{
    if (channel < 0 || channel >= channelCount)
        return false;
    const auto& ch = channels[static_cast<size_t>(channel)];
    return ch.gate || ch.env > 1e-4f;
}

void StubEngine::setClockStandard(ClockStandard standard) noexcept
{
    params[Clock].store(standard == ClockStandard::Pal ? 1.0f : 0.0f, std::memory_order_relaxed);
}

void StubEngine::setRawOutput(bool r) noexcept
{
    raw = r;
    for (auto& ch : channels)
        ch.synth.setRaw(r);
    mainSynth.setRaw(r);
}

void StubEngine::renderBlock(float* mainL, float* mainR, float* const* channelOutsL, float* const* channelOutsR,
                             int numSamples) noexcept
{
    const float volume = getParameter(Volume) / 15.0f;
    const int duty = static_cast<int>(getParameter(Duty));
    const float dutyFrac = duty == 0 ? 0.125f : duty == 1 ? 0.25f : duty == 2 ? 0.5f : 0.75f;
    const float attackMs = getParameter(AttackMs);
    const float releaseMs = getParameter(ReleaseMs);
    const float attackStep = attackMs <= 0.0f ? 1.0f : static_cast<float>(1000.0 / (attackMs * sampleRate));
    const float releaseStep = releaseMs <= 0.0f ? 1.0f : static_cast<float>(1000.0 / (releaseMs * sampleRate));
    const bool pal = getParameter(Clock) > 0.5f;
    const double cpuClock = pal ? 1662607.0 : 1789773.0;

    std::fill(mainL, mainL + numSamples, 0.0f);

    for (int c = 0; c < channelCount; ++c)
    {
        auto& ch = channels[static_cast<size_t>(c)];
        const double freq = 440.0 * std::pow(2.0, (ch.note - 69.0) / 12.0);
        // 11-bit period quantisation like the 2A03 pulse channel: f = clock / (16 (p + 1)).
        const int period = std::clamp(static_cast<int>(std::lround(cpuClock / (16.0 * freq) - 1.0)), 0, 2047);
        const double quantisedFreq = period < 8 ? 0.0 : cpuClock / (16.0 * (period + 1));
        const double cyclesPerSample = quantisedFreq / sampleRate;

        // Envelope at block rate (control-rate is fine for a stub).
        const float target = ch.gate ? ch.velocity * volume : 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            if (ch.env < target)
                ch.env = std::min(target, ch.env + attackStep);
            else if (ch.env > target)
                ch.env = std::max(target, ch.env - releaseStep);

            const int newLevel = (ch.phase - std::floor(ch.phase)) < dutyFrac ? 1 : 0;
            if (newLevel != ch.level)
            {
                const float delta = static_cast<float>(newLevel - ch.level) * ch.env * 0.25f;
                ch.synth.addDelta(static_cast<double>(i), delta);
                mainSynth.addDelta(static_cast<double>(i), delta);
                ch.level = newLevel;
            }
            if (cyclesPerSample > 0.0)
                ch.phase += cyclesPerSample;
            if (ch.phase >= 1e9)
                ch.phase -= 1e9;
        }

        if (channelOutsL != nullptr && channelOutsL[c] != nullptr)
        {
            ch.synth.endBlockReplace(channelOutsL[c], numSamples);
            for (int i = 0; i < numSamples; ++i)
                channelOutsL[c][i] = ch.dc.process(channelOutsL[c][i]);
            if (channelOutsR != nullptr && channelOutsR[c] != nullptr)
                std::copy(channelOutsL[c], channelOutsL[c] + numSamples, channelOutsR[c]);
        }
        else
        {
            ch.synth.endBlockReplace(scratch.data(), numSamples);
        }
    }

    mainSynth.endBlock(mainL, numSamples);
    for (int i = 0; i < numSamples; ++i)
        mainL[i] = mainDc.process(mainL[i]);
    std::copy(mainL, mainL + numSamples, mainR);
}

} // namespace chipdsp
