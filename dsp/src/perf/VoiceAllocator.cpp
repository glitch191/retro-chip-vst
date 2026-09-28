#include "chipdsp/perf/VoiceAllocator.h"

namespace chipdsp
{

namespace
{
    int clampNote(int note) noexcept { return note < 0 ? 0 : (note > 127 ? 127 : note); }
} // namespace

void VoiceAllocator::configure(int numHardwareChannels, uint32_t enabledMask) noexcept
{
    if (numHardwareChannels < 0)
        numHardwareChannels = 0;
    if (numHardwareChannels > kMaxHardwareChannels)
        numHardwareChannels = kMaxHardwareChannels;
    channelCount = numHardwareChannels;
    mask = enabledMask;
    for (Voice& v : voices)
        v = Voice {};
    rrCursor = 0;
    nextOrder = 1;
}

void VoiceAllocator::setEnabledMask(uint32_t enabledMask) noexcept
{
    mask = enabledMask;
    for (int ch = 0; ch < channelCount; ++ch)
        if (!isChannelEnabled(ch))
            voices[static_cast<size_t>(ch)].held = false;
    if (channelCount > 0 && rrCursor >= channelCount)
        rrCursor = 0;
}

bool VoiceAllocator::isChannelEnabled(int channel) const noexcept
{
    return validChannel(channel) && ((mask >> channel) & 1u) != 0u;
}

int VoiceAllocator::numEnabledChannels() const noexcept
{
    int n = 0;
    for (int ch = 0; ch < channelCount; ++ch)
        if (isChannelEnabled(ch))
            ++n;
    return n;
}

int VoiceAllocator::assign(int channel, int note, float velocity, int sampleOffset, int midiChannel) noexcept
{
    Voice& v = voices[static_cast<size_t>(channel)];
    v.legato = v.held;
    v.note = note;
    v.velocity = velocity;
    v.midiChannel = midiChannel;
    v.sampleOffset = sampleOffset;
    v.order = nextOrder++;
    v.held = true;
    return channel;
}

int VoiceAllocator::noteOn(int note, float velocity, int sampleOffset, int midiChannel) noexcept
{
    note = clampNote(note);
    if (channelCount <= 0)
        return kNoChannel;

    if (midiMode)
    {
        const int channel = midiChannel - 1;
        if (!isChannelEnabled(channel))
            return kNoChannel;
        return assign(channel, note, velocity, sampleOffset, midiChannel);
    }

    // Round-robin: first free enabled channel at or after the cursor.
    if (rrCursor < 0 || rrCursor >= channelCount)
        rrCursor = 0;
    for (int i = 0; i < channelCount; ++i)
    {
        const int channel = (rrCursor + i) % channelCount;
        if (!isChannelEnabled(channel) || voices[static_cast<size_t>(channel)].held)
            continue;
        rrCursor = (channel + 1) % channelCount;
        return assign(channel, note, velocity, sampleOffset, midiChannel);
    }

    // Every enabled channel is busy: steal the oldest note.
    int oldest = kNoChannel;
    for (int channel = 0; channel < channelCount; ++channel)
    {
        if (!isChannelEnabled(channel))
            continue;
        if (oldest == kNoChannel
            || voices[static_cast<size_t>(channel)].order < voices[static_cast<size_t>(oldest)].order)
            oldest = channel;
    }
    if (oldest == kNoChannel)
        return kNoChannel; // empty mask
    rrCursor = (oldest + 1) % channelCount;
    return assign(oldest, note, velocity, sampleOffset, midiChannel);
}

int VoiceAllocator::noteOff(int note, int midiChannel) noexcept
{
    note = clampNote(note);
    if (midiMode)
    {
        const int channel = midiChannel - 1;
        if (!validChannel(channel))
            return kNoChannel;
        Voice& v = voices[static_cast<size_t>(channel)];
        if (!v.held || v.note != note)
            return kNoChannel;
        v.held = false;
        return channel;
    }

    int oldest = kNoChannel;
    for (int channel = 0; channel < channelCount; ++channel)
    {
        const Voice& v = voices[static_cast<size_t>(channel)];
        if (!v.held || v.note != note || v.midiChannel != midiChannel)
            continue;
        if (oldest == kNoChannel || v.order < voices[static_cast<size_t>(oldest)].order)
            oldest = channel;
    }
    if (oldest != kNoChannel)
        voices[static_cast<size_t>(oldest)].held = false;
    return oldest;
}

void VoiceAllocator::allNotesOff() noexcept
{
    for (Voice& v : voices)
        v.held = false;
}

void VoiceAllocator::reset() noexcept
{
    for (Voice& v : voices)
        v = Voice {};
    rrCursor = 0;
    nextOrder = 1;
}

bool VoiceAllocator::isChannelHeld(int channel) const noexcept
{
    return validChannel(channel) && voices[static_cast<size_t>(channel)].held;
}

int VoiceAllocator::channelNote(int channel) const noexcept
{
    return validChannel(channel) ? voices[static_cast<size_t>(channel)].note : -1;
}

float VoiceAllocator::channelVelocity(int channel) const noexcept
{
    return validChannel(channel) ? voices[static_cast<size_t>(channel)].velocity : 0.0f;
}

int VoiceAllocator::channelSampleOffset(int channel) const noexcept
{
    return validChannel(channel) ? voices[static_cast<size_t>(channel)].sampleOffset : 0;
}

int VoiceAllocator::channelMidiChannel(int channel) const noexcept
{
    return validChannel(channel) ? voices[static_cast<size_t>(channel)].midiChannel : 0;
}

bool VoiceAllocator::lastNoteOnWasLegato(int channel) const noexcept
{
    return validChannel(channel) && voices[static_cast<size_t>(channel)].legato;
}

int VoiceAllocator::numHeldNotes() const noexcept
{
    int n = 0;
    for (int ch = 0; ch < channelCount; ++ch)
        if (voices[static_cast<size_t>(ch)].held)
            ++n;
    return n;
}

} // namespace chipdsp
