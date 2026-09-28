#include "chipdsp/perf/Glide.h"

namespace chipdsp
{

void Glide::setParams(const Params& p) noexcept
{
    current = p;
    setTimeMs(p.timeMs);
}

void Glide::setTimeMs(float timeMs) noexcept
{
    if (!(timeMs > 0.0f)) // also catches NaN
        timeMs = 0.0f;
    if (timeMs > kMaxTimeMs)
        timeMs = kMaxTimeMs;
    current.timeMs = timeMs;
}

void Glide::reset() noexcept
{
    for (State& s : states)
        s = State {};
}

void Glide::setTarget(int channel, float midiNote, bool isLegato) noexcept
{
    if (!validChannel(channel))
        return;
    State& s = states[static_cast<size_t>(channel)];
    s.target = midiNote;

    const bool ramp = s.hasNote
                   && current.timeMs > 0.0f
                   && (current.mode == Mode::Always || isLegato);
    s.hasNote = true;

    if (!ramp || s.current == midiNote)
    {
        s.current = midiNote;
        s.start = midiNote;
        s.elapsed = 0.0;
        s.active = false;
        return;
    }

    s.start = s.current;
    s.elapsed = 0.0;
    s.active = true;
}

void Glide::setCurrent(int channel, float midiNote) noexcept
{
    if (!validChannel(channel))
        return;
    State& s = states[static_cast<size_t>(channel)];
    s.current = midiNote;
    s.start = midiNote;
    s.target = midiNote;
    s.elapsed = 0.0;
    s.active = false;
    s.hasNote = true;
}

void Glide::advance(int numSamples, double sampleRate) noexcept
{
    if (numSamples <= 0)
        return;
    const double totalSamples = static_cast<double>(current.timeMs) * sampleRate / 1000.0;
    for (State& s : states)
    {
        if (!s.active)
            continue;
        s.elapsed += static_cast<double>(numSamples);
        if (!(totalSamples > 0.0) || s.elapsed >= totalSamples)
        {
            s.current = s.target;
            s.start = s.target;
            s.active = false;
            continue;
        }
        const double progress = s.elapsed / totalSamples;
        s.current = static_cast<float>(static_cast<double>(s.start)
                                       + (static_cast<double>(s.target) - static_cast<double>(s.start)) * progress);
    }
}

float Glide::currentNote(int channel) const noexcept
{
    return validChannel(channel) ? states[static_cast<size_t>(channel)].current : 0.0f;
}

float Glide::targetNote(int channel) const noexcept
{
    return validChannel(channel) ? states[static_cast<size_t>(channel)].target : 0.0f;
}

bool Glide::isGliding(int channel) const noexcept
{
    return validChannel(channel) && states[static_cast<size_t>(channel)].active;
}

} // namespace chipdsp
