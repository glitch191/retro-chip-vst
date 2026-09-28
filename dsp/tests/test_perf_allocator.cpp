#include "chipdsp/perf/VoiceAllocator.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using chipdsp::kMaxHardwareChannels;
using chipdsp::VoiceAllocator;

namespace
{
    constexpr int kNone = VoiceAllocator::kNoChannel;

    // Small deterministic generator for the stress test (xorshift32).
    struct Rng
    {
        uint32_t state = 0x1234567u;
        uint32_t next()
        {
            uint32_t x = state;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            state = x;
            return x;
        }
    };
} // namespace

TEST_CASE("VoiceAllocator: round-robin spreads notes over the enabled channels", "[perf][allocator]")
{
    VoiceAllocator alloc;
    alloc.configure(5, 0b11111u); // NES: 5 channels, all enabled
    REQUIRE(alloc.numChannels() == 5);

    // Fresh allocator: channels are handed out in order 0,1,2,3,4.
    REQUIRE(alloc.noteOn(60, 1.0f, 0) == 0);
    REQUIRE(alloc.noteOn(62, 1.0f, 1) == 1);
    REQUIRE(alloc.noteOn(64, 1.0f, 2) == 2);
    REQUIRE(alloc.noteOn(65, 1.0f, 3) == 3);
    REQUIRE(alloc.noteOn(67, 1.0f, 4) == 4);
    REQUIRE(alloc.numHeldNotes() == 5);

    for (int ch = 0; ch < 5; ++ch)
    {
        REQUIRE(alloc.isChannelHeld(ch));
        REQUIRE_FALSE(alloc.lastNoteOnWasLegato(ch)); // every channel was free when assigned
    }
    REQUIRE(alloc.channelNote(2) == 64);
    REQUIRE(alloc.channelSampleOffset(3) == 3);

    // Release channel 1 and 2: the cursor sits at 0 (5 assignments wrapped around), so the
    // next free channel scanning from 0 is 1, then 2.
    REQUIRE(alloc.noteOff(62) == 1);
    REQUIRE(alloc.noteOff(64) == 2);
    REQUIRE_FALSE(alloc.isChannelHeld(1));
    REQUIRE(alloc.channelNote(1) == 62); // last note is remembered for glide-from-release
    REQUIRE(alloc.noteOn(69, 1.0f, 5) == 1);
    REQUIRE(alloc.noteOn(71, 1.0f, 6) == 2);

    // A note that is not held anywhere returns kNoChannel.
    REQUIRE(alloc.noteOff(62) == kNone);
    REQUIRE(alloc.noteOff(100) == kNone);
}

TEST_CASE("VoiceAllocator: stealing takes the oldest note", "[perf][allocator]")
{
    VoiceAllocator alloc;
    alloc.configure(3, 0b111u);

    REQUIRE(alloc.noteOn(60, 1.0f, 0) == 0); // order 1
    REQUIRE(alloc.noteOn(62, 1.0f, 0) == 1); // order 2
    REQUIRE(alloc.noteOn(64, 1.0f, 0) == 2); // order 3

    // Full: the oldest (60 on channel 0) is stolen, and the steal is reported as legato.
    REQUIRE(alloc.noteOn(65, 1.0f, 0) == 0); // order 4
    REQUIRE(alloc.lastNoteOnWasLegato(0));
    REQUIRE(alloc.channelNote(0) == 65);
    REQUIRE(alloc.noteOff(60) == kNone); // 60 was stolen: nothing to release

    // Next oldest is 62 on channel 1, then 64 on channel 2, then 65 on channel 0.
    REQUIRE(alloc.noteOn(67, 1.0f, 0) == 1); // order 5
    REQUIRE(alloc.noteOn(69, 1.0f, 0) == 2); // order 6
    REQUIRE(alloc.noteOn(71, 1.0f, 0) == 0); // order 7

    // Releasing a note makes its channel the preferred target again (free before steal).
    REQUIRE(alloc.noteOff(69) == 2);
    REQUIRE(alloc.noteOn(72, 1.0f, 0) == 2);
    REQUIRE_FALSE(alloc.lastNoteOnWasLegato(2));

    // Same note held twice (two channels): noteOff releases the oldest instance first.
    alloc.configure(3, 0b111u);
    REQUIRE(alloc.noteOn(60, 1.0f, 0) == 0);
    REQUIRE(alloc.noteOn(60, 1.0f, 0) == 1);
    REQUIRE(alloc.noteOff(60) == 0);
    REQUIRE(alloc.noteOff(60) == 1);
    REQUIRE(alloc.noteOff(60) == kNone);
}

TEST_CASE("VoiceAllocator: never leaves the enabled mask", "[perf][allocator]")
{
    VoiceAllocator alloc;
    alloc.configure(10, 0b0000001010u); // Genesis: only channels 1 and 3 enabled
    REQUIRE(alloc.numEnabledChannels() == 2);

    // Round-robin over {1, 3}, then stealing alternates the oldest: 1, 3, 1, 3 ...
    REQUIRE(alloc.noteOn(60, 1.0f, 0) == 1);
    REQUIRE(alloc.noteOn(61, 1.0f, 0) == 3);
    REQUIRE(alloc.noteOn(62, 1.0f, 0) == 1);
    REQUIRE(alloc.noteOn(63, 1.0f, 0) == 3);
    REQUIRE(alloc.noteOn(64, 1.0f, 0) == 1);

    // Empty mask: nothing can be assigned.
    alloc.configure(5, 0u);
    REQUIRE(alloc.noteOn(60, 1.0f, 0) == kNone);

    // Mask bits above the channel count do not enable anything.
    alloc.configure(5, 0b1100000u);
    REQUIRE(alloc.noteOn(60, 1.0f, 0) == kNone);

    // Shrinking the mask while notes play releases the notes on the disabled channels.
    alloc.configure(4, 0b1111u);
    REQUIRE(alloc.noteOn(60, 1.0f, 0) == 0);
    REQUIRE(alloc.noteOn(62, 1.0f, 0) == 1);
    alloc.setEnabledMask(0b0001u);
    REQUIRE_FALSE(alloc.isChannelHeld(1));
    REQUIRE(alloc.isChannelHeld(0));
    REQUIRE(alloc.noteOn(64, 1.0f, 0) == 0); // only channel 0 remains: stolen
}

TEST_CASE("VoiceAllocator: never exceeds the chip's channel count", "[perf][allocator]")
{
    VoiceAllocator alloc;
    alloc.configure(12, 0xFFFFFFFFu); // more than any chip has: clamped to kMaxHardwareChannels
    REQUIRE(alloc.numChannels() == kMaxHardwareChannels);
    for (int i = 0; i < 40; ++i)
    {
        const int ch = alloc.noteOn(40 + i, 0.5f, i);
        REQUIRE(ch >= 0);
        REQUIRE(ch < kMaxHardwareChannels);
    }

    alloc.configure(-3, 0xFFFFFFFFu); // nonsense count: clamped to 0, nothing assignable
    REQUIRE(alloc.numChannels() == 0);
    REQUIRE(alloc.noteOn(60, 1.0f, 0) == kNone);
    REQUIRE(alloc.noteOff(60) == kNone);
}

TEST_CASE("VoiceAllocator: random traffic stays inside the mask and the channel count", "[perf][allocator]")
{
    const int counts[] = { 5, 8, 10 };
    const uint32_t masks[] = { 0b11111u, 0b10101010u, 0b1111111111u, 0b0100000000u };
    Rng rng;

    for (const int count : counts)
    {
        for (const uint32_t mask : masks)
        {
            VoiceAllocator alloc;
            alloc.configure(count, mask);
            std::vector<int> heldNotes;
            for (int step = 0; step < 2000; ++step)
            {
                const bool on = (rng.next() & 3u) != 0u || heldNotes.empty(); // 75 % note ons
                if (on)
                {
                    const int note = static_cast<int>(rng.next() % 128u);
                    const int ch = alloc.noteOn(note, 0.8f, step);
                    if (ch != kNone)
                    {
                        REQUIRE(ch >= 0);
                        REQUIRE(ch < count);
                        REQUIRE(((mask >> ch) & 1u) == 1u);
                        heldNotes.push_back(note);
                    }
                    else
                    {
                        // kNoChannel only when no channel of this count is enabled
                        REQUIRE(alloc.numEnabledChannels() == 0);
                    }
                }
                else
                {
                    const size_t pick = rng.next() % heldNotes.size();
                    const int ch = alloc.noteOff(heldNotes[pick]);
                    heldNotes.erase(heldNotes.begin() + static_cast<std::ptrdiff_t>(pick));
                    if (ch != kNone)
                    {
                        REQUIRE(ch < count);
                        REQUIRE(((mask >> ch) & 1u) == 1u);
                    }
                }
                REQUIRE(alloc.numHeldNotes() <= alloc.numEnabledChannels());
            }
        }
    }
}

TEST_CASE("VoiceAllocator: MIDI channel mode maps MIDI channel N to hardware channel N-1", "[perf][allocator]")
{
    VoiceAllocator alloc;
    alloc.configure(5, 0b11111u);
    alloc.setMidiChannelMode(true);
    REQUIRE(alloc.midiChannelMode());

    REQUIRE(alloc.noteOn(60, 1.0f, 0, 1) == 0);
    REQUIRE(alloc.noteOn(60, 1.0f, 0, 3) == 2);
    REQUIRE(alloc.noteOn(60, 1.0f, 0, 5) == 4);
    REQUIRE(alloc.noteOn(60, 1.0f, 0, 6) == kNone);  // channel 5 does not exist on a 5-channel chip
    REQUIRE(alloc.noteOn(60, 1.0f, 0, 16) == kNone);
    REQUIRE(alloc.noteOn(60, 1.0f, 0, 0) == kNone);  // invalid MIDI channel

    // Mono per channel: a second note replaces the first and is reported as legato.
    REQUIRE(alloc.noteOn(62, 1.0f, 0, 3) == 2);
    REQUIRE(alloc.lastNoteOnWasLegato(2));
    REQUIRE(alloc.channelNote(2) == 62);
    REQUIRE(alloc.noteOff(60, 3) == kNone); // replaced: nothing to release
    REQUIRE(alloc.noteOff(62, 3) == 2);
    REQUIRE_FALSE(alloc.isChannelHeld(2));

    // The note off must come from the same MIDI channel.
    REQUIRE(alloc.noteOff(60, 2) == kNone);
    REQUIRE(alloc.noteOff(60, 1) == 0);

    // The enabled mask still applies.
    alloc.configure(5, 0b11101u); // channel 1 disabled
    alloc.setMidiChannelMode(true);
    REQUIRE(alloc.noteOn(60, 1.0f, 0, 2) == kNone);
    REQUIRE(alloc.noteOn(60, 1.0f, 0, 1) == 0);

    // A new note on a free channel is not legato.
    REQUIRE(alloc.noteOff(60, 1) == 0);
    REQUIRE(alloc.noteOn(64, 1.0f, 0, 1) == 0);
    REQUIRE_FALSE(alloc.lastNoteOnWasLegato(0));
}

TEST_CASE("VoiceAllocator: allNotesOff and reset free every channel", "[perf][allocator]")
{
    VoiceAllocator alloc;
    alloc.configure(8, 0xFFu);
    for (int i = 0; i < 8; ++i)
        REQUIRE(alloc.noteOn(60 + i, 1.0f, 0) == i);
    REQUIRE(alloc.numHeldNotes() == 8);

    alloc.allNotesOff();
    REQUIRE(alloc.numHeldNotes() == 0);
    REQUIRE(alloc.channelNote(3) == 63); // remembered
    REQUIRE(alloc.noteOff(63) == kNone);

    alloc.reset();
    REQUIRE(alloc.channelNote(3) == -1);
    REQUIRE(alloc.noteOn(70, 1.0f, 0) == 0); // cursor back at 0
}
