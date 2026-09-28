#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace chipdsp
{

// One MIDI note message positioned inside an audio block. Plain data, shared by the
// arpeggiator (input and output), the voice allocator and the plugin layer.
struct NoteEvent
{
    enum class Type : uint8_t
    {
        NoteOn = 0,
        NoteOff = 1
    };

    Type type = Type::NoteOn;
    uint8_t note = 0;         // 0..127
    float velocity = 0.0f;    // 0..1 (ignored for NoteOff)
    uint8_t midiChannel = 1;  // 1..16
    int sampleOffset = 0;     // samples from the start of the current block

    bool isNoteOn() const noexcept { return type == Type::NoteOn; }
    bool isNoteOff() const noexcept { return type == Type::NoteOff; }

    static NoteEvent on(int note, float velocity, int midiChannel, int sampleOffset) noexcept
    {
        return { Type::NoteOn, clampNote(note), velocity, clampChannel(midiChannel), sampleOffset };
    }

    static NoteEvent off(int note, int midiChannel, int sampleOffset) noexcept
    {
        return { Type::NoteOff, clampNote(note), 0.0f, clampChannel(midiChannel), sampleOffset };
    }

    static uint8_t clampNote(int note) noexcept
    {
        return static_cast<uint8_t>(note < 0 ? 0 : (note > 127 ? 127 : note));
    }

    static uint8_t clampChannel(int channel) noexcept
    {
        return static_cast<uint8_t>(channel < 1 ? 1 : (channel > 16 ? 16 : channel));
    }
};

// Largest number of note events a module emits or accepts for one block.
constexpr int kMaxEventsPerBlock = 64;

// Fixed-capacity event list: no allocation, no exceptions. push() returns false and
// drops the event when the buffer is full.
class NoteEventBuffer
{
public:
    bool push(const NoteEvent& event) noexcept
    {
        if (count >= kMaxEventsPerBlock)
            return false;
        events[static_cast<size_t>(count)] = event;
        ++count;
        return true;
    }

    void clear() noexcept { count = 0; }
    int size() const noexcept { return count; }
    bool empty() const noexcept { return count == 0; }
    bool full() const noexcept { return count >= kMaxEventsPerBlock; }

    const NoteEvent& operator[](int index) const noexcept { return events[static_cast<size_t>(index)]; }

    std::span<const NoteEvent> view() const noexcept
    {
        return { events.data(), static_cast<size_t>(count) };
    }

    const NoteEvent* begin() const noexcept { return events.data(); }
    const NoteEvent* end() const noexcept { return events.data() + count; }

private:
    std::array<NoteEvent, kMaxEventsPerBlock> events {};
    int count = 0;
};

} // namespace chipdsp
