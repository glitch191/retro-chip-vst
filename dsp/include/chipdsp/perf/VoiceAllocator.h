#pragma once

#include "chipdsp/ChipTypes.h"

#include <array>
#include <cstdint>

namespace chipdsp
{

// Maps MIDI notes to the hardware channels of one engine.
//
// Poly mode (default): round-robin over the enabled channels, so successive notes
// spread across channels and released notes keep ringing while others play. When
// every enabled channel holds a key-down note, the oldest note (by noteOn order) is
// stolen.
//
// MIDI channel mode: MIDI channel N drives hardware channel N-1. A second note on
// the same MIDI channel replaces the first (mono per channel); the enabled mask
// still applies.
//
// Never returns a channel outside the enabled mask or >= the configured count.
// Audio thread safe: fixed storage, no allocation, no exceptions.
class VoiceAllocator
{
public:
    static constexpr int kNoChannel = -1;

    // numHardwareChannels is clamped to [0, kMaxHardwareChannels]; bit c of enabledMask
    // enables hardware channel c. Forgets every held note.
    void configure(int numHardwareChannels, uint32_t enabledMask) noexcept;

    // Changes the mask without forgetting notes; notes on channels that become disabled
    // are released (the caller silences them).
    void setEnabledMask(uint32_t enabledMask) noexcept;
    void setMidiChannelMode(bool enabled) noexcept { midiMode = enabled; }

    int numChannels() const noexcept { return channelCount; }
    uint32_t enabledMask() const noexcept { return mask; }
    bool midiChannelMode() const noexcept { return midiMode; }
    bool isChannelEnabled(int channel) const noexcept;
    int numEnabledChannels() const noexcept;

    // Returns the hardware channel to trigger, or kNoChannel when nothing is available.
    // midiChannel is 1..16: it selects the hardware channel in MIDI channel mode and is
    // matched by noteOff in both modes.
    int noteOn(int note, float velocity, int sampleOffset, int midiChannel = 1) noexcept;

    // Releases the oldest key-down instance of 'note' and returns its channel, or kNoChannel
    // when no channel holds that note (already stolen, or never assigned).
    int noteOff(int note, int midiChannel = 1) noexcept;

    // Releases every note. No engine calls are made; the caller silences the channels.
    void allNotesOff() noexcept;
    void reset() noexcept;

    // Per-channel state. Invalid channels read as free / -1 / 0.
    bool isChannelHeld(int channel) const noexcept;       // key down on that channel
    int channelNote(int channel) const noexcept;          // last note assigned (held or released), -1 if none
    float channelVelocity(int channel) const noexcept;
    int channelSampleOffset(int channel) const noexcept;  // sampleOffset passed with the last noteOn
    int channelMidiChannel(int channel) const noexcept;

    // True when the last noteOn on 'channel' replaced a note that was still held (stolen
    // in Poly mode, retriggered in MIDI channel mode). This is the legato flag Glide expects.
    bool lastNoteOnWasLegato(int channel) const noexcept;

    int numHeldNotes() const noexcept;

private:
    struct Voice
    {
        int note = -1;
        float velocity = 0.0f;
        int midiChannel = 0;
        int sampleOffset = 0;
        uint64_t order = 0;     // allocation stamp, larger = more recent
        bool held = false;
        bool legato = false;
    };

    bool validChannel(int channel) const noexcept { return channel >= 0 && channel < channelCount; }
    int assign(int channel, int note, float velocity, int sampleOffset, int midiChannel) noexcept;

    std::array<Voice, kMaxHardwareChannels> voices {};
    int channelCount = 0;
    uint32_t mask = 0;
    bool midiMode = false;
    int rrCursor = 0;          // next channel to try in Poly mode
    uint64_t nextOrder = 1;
};

} // namespace chipdsp
