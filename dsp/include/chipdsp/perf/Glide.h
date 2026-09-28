#pragma once

#include "chipdsp/ChipTypes.h"

#include <array>

namespace chipdsp
{

// Portamento per hardware channel: the pitch handed to IChipEngine::setChannelPitch()
// ramps linearly in semitones from the previous pitch to the new target over timeMs.
//
// The ramp is fixed-time (an octave takes as long as a semitone). timeMs 0, the first
// note on a channel, and a non-legato note in LegatoOnly mode jump immediately.
// Audio thread safe: fixed storage, no allocation, no exceptions.
class Glide
{
public:
    enum class Mode : int
    {
        Always = 0,      // every new target glides from the channel's current pitch
        LegatoOnly = 1   // glides only when the caller reports the note as legato
    };

    struct Params
    {
        float timeMs = 0.0f;       // 0..2000
        Mode mode = Mode::Always;
    };

    static constexpr float kMaxTimeMs = 2000.0f;

    void setParams(const Params& p) noexcept;
    void setTimeMs(float timeMs) noexcept;
    void setMode(Mode mode) noexcept { current.mode = mode; }
    const Params& params() const noexcept { return current; }

    // Every channel forgets its pitch: the next target jumps.
    void reset() noexcept;

    // New pitch for 'channel'. isLegato: another note was still held on that channel
    // when this one started (the voice allocator reports it as lastNoteOnWasLegato()).
    void setTarget(int channel, float midiNote, bool isLegato) noexcept;

    // Forces the current pitch without a ramp (also cancels a running ramp).
    void setCurrent(int channel, float midiNote) noexcept;

    // Advances every ramp by numSamples. currentNote() then reports the pitch reached at
    // the end of that span.
    void advance(int numSamples, double sampleRate) noexcept;

    float currentNote(int channel) const noexcept;
    float targetNote(int channel) const noexcept;
    bool isGliding(int channel) const noexcept;

private:
    struct State
    {
        float current = 0.0f;
        float start = 0.0f;
        float target = 0.0f;
        double elapsed = 0.0;   // samples since the ramp started
        bool active = false;
        bool hasNote = false;   // a target was set at least once since reset()
    };

    static bool validChannel(int channel) noexcept { return channel >= 0 && channel < kMaxHardwareChannels; }

    Params current {};
    std::array<State, kMaxHardwareChannels> states {};
};

} // namespace chipdsp
