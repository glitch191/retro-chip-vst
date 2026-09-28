#pragma once

#include "chipdsp/ChipTypes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace rcv
{

// Single-producer / single-consumer ring buffers feeding the channel scopes: one stereo
// pair per hardware channel plus the main output (11 x 2), 8192 floats each.
//
// The audio thread writes with push() and publishes the new write index with a release
// store; the UI reads the newest samples with readLatest() after an acquire load. There
// are no locks. The reader may observe a torn window when the writer laps it during a
// copy; this is a scope feed, not a recording path, so that is acceptable.
//
// Slot layout: kMainSlot (0) is the main output, channelSlot(c) = c + 1 is hardware
// channel c of the active chip. Producers write only when isEnabled() is true (the
// editor enables the feed while it exists, so the audio thread does no work otherwise).
class VisualizerBuffers
{
public:
    static constexpr int kCapacity = 8192;
    static constexpr int kMainSlot = 0;
    static constexpr int kNumSlots = chipdsp::kMaxHardwareChannels + 1;

    static constexpr int channelSlot (int hardwareChannel) noexcept { return hardwareChannel + 1; }

    void setEnabled (bool shouldBeEnabled) noexcept { enabled.store (shouldBeEnabled, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled.load (std::memory_order_relaxed); }

    // Number of hardware channels currently fed (the active chip's count). Set by the host.
    void setActiveChannelCount (int count) noexcept { activeChannels.store (count, std::memory_order_relaxed); }
    int activeChannelCount() const noexcept { return activeChannels.load (std::memory_order_relaxed); }

    // Zeroes every ring. Message thread; harmless if the producer is running.
    void clear() noexcept
    {
        for (auto& ring : rings)
        {
            ring.left.fill (0.0f);
            ring.right.fill (0.0f);
        }
    }

    // Audio thread. right may be nullptr (mono feed: right copies left).
    void push (int slot, const float* left, const float* right, int numSamples) noexcept
    {
        if (slot < 0 || slot >= kNumSlots || left == nullptr || numSamples <= 0)
            return;
        auto& ring = rings[static_cast<size_t> (slot)];
        int w = ring.writeIndex.load (std::memory_order_relaxed);
        if (numSamples > kCapacity)
        {
            left += numSamples - kCapacity;
            if (right != nullptr)
                right += numSamples - kCapacity;
            numSamples = kCapacity;
        }
        for (int i = 0; i < numSamples; ++i)
        {
            ring.left[static_cast<size_t> (w)] = left[i];
            ring.right[static_cast<size_t> (w)] = right != nullptr ? right[i] : left[i];
            w = (w + 1) % kCapacity;
        }
        ring.writeIndex.store (w, std::memory_order_release);
        ring.generation.fetch_add (1u, std::memory_order_release);
    }

    // UI thread. Copies the newest numSamples (<= kCapacity) into left/right, oldest first,
    // and returns the generation counter (changes whenever the producer pushed).
    uint32_t readLatest (int slot, float* left, float* right, int numSamples) const noexcept
    {
        if (slot < 0 || slot >= kNumSlots || numSamples <= 0)
            return 0;
        numSamples = std::min (numSamples, kCapacity);
        const auto& ring = rings[static_cast<size_t> (slot)];
        const uint32_t gen = ring.generation.load (std::memory_order_acquire);
        const int w = ring.writeIndex.load (std::memory_order_acquire);
        int r = (w - numSamples + kCapacity) % kCapacity;
        for (int i = 0; i < numSamples; ++i)
        {
            if (left != nullptr)
                left[i] = ring.left[static_cast<size_t> (r)];
            if (right != nullptr)
                right[i] = ring.right[static_cast<size_t> (r)];
            r = (r + 1) % kCapacity;
        }
        return gen;
    }

    // Generation counter of a slot without copying; the UI compares it with the last one it
    // consumed to decide whether a repaint is needed.
    uint32_t generation (int slot) const noexcept
    {
        if (slot < 0 || slot >= kNumSlots)
            return 0;
        return rings[static_cast<size_t> (slot)].generation.load (std::memory_order_acquire);
    }

private:
    struct Ring
    {
        std::array<float, kCapacity> left {};
        std::array<float, kCapacity> right {};
        std::atomic<int> writeIndex { 0 };
        std::atomic<uint32_t> generation { 0 };
    };

    std::array<Ring, kNumSlots> rings;
    std::atomic<bool> enabled { false };
    std::atomic<int> activeChannels { 0 };
};

} // namespace rcv
