#include "ui/ChannelScope.h"

#include "ui/Theme.h"

#include <cmath>

namespace rcv
{

ChannelScope::ChannelScope (VisualizerBuffers& b) : buffers (b)
{
    setInterceptsMouseClicks (true, false);   // for the tile tooltips; clicks do nothing
}

juce::String ChannelScope::getTooltip()
{
    const auto position = getMouseXYRelative();
    for (const auto& t : tiles)
        if (t.bounds.contains (position))
            return t.slot == VisualizerBuffers::kMainSlot ? juce::String ("Main output")
                                                          : t.name + " (output bus Out " + juce::String (t.slot) + ")";
    return {};
}

void ChannelScope::setChip (const chipdsp::IChipEngine& engine)
{
    tiles.clear();
    Tile main;
    main.name = "Main";
    main.slot = VisualizerBuffers::kMainSlot;
    tiles.push_back (main);
    for (int c = 0; c < engine.numChannels() && c < chipdsp::kMaxHardwareChannels; ++c)
    {
        Tile t;
        const auto info = engine.channelInfo (c);
        t.name = info.name != nullptr ? juce::String (info.name) : "Channel " + juce::String (c + 1);
        t.slot = VisualizerBuffers::channelSlot (c);
        tiles.push_back (t);
    }
    resized();
    for (auto& t : tiles)
    {
        t.lastGeneration = buffers.generation (t.slot) - 1u;   // force the first read
        readTile (t);
    }
    repaint();
}

void ChannelScope::resized()
{
    const int n = static_cast<int> (tiles.size());
    if (n == 0)
        return;
    const auto area = getLocalBounds();
    const int w = (area.getWidth() - (n - 1) * theme::kGap) / n;
    for (int i = 0; i < n; ++i)
    {
        auto& t = tiles[static_cast<size_t> (i)];
        const int x = area.getX() + i * (w + theme::kGap);
        t.bounds = { x, area.getY(), i == n - 1 ? area.getRight() - x : w, area.getHeight() };
        t.wave = t.bounds.reduced (theme::kPad, 0).withTrimmedTop (theme::kLabelHeight + theme::kUnit).withTrimmedBottom (theme::kUnit);
        t.columns = juce::jlimit (1, kMaxColumns, t.wave.getWidth());
    }
}

bool ChannelScope::readTile (Tile& tile)
{
    const auto generation = buffers.generation (tile.slot);
    if (generation == tile.lastGeneration)
        return false;
    if (tile.flat && buffers.trailingSilence (tile.slot) >= kRead)
    {
        tile.lastGeneration = generation;   // only silence arrived: nothing to read or draw
        return false;
    }
    tile.lastGeneration = buffers.readLatest (tile.slot, left.data(), right.data(), kRead);

    // Trigger on the first rising zero crossing, otherwise show the newest window.
    int start = kRead - kWindow;
    for (int i = 1; i < kRead - kWindow; ++i)
    {
        const float a = left[static_cast<size_t> (i - 1)] + right[static_cast<size_t> (i - 1)];
        const float b = left[static_cast<size_t> (i)] + right[static_cast<size_t> (i)];
        if (a <= 0.0f && b > 0.0f)
        {
            start = i;
            break;
        }
    }

    // Display gain: the window's peak fills the tile, up to kMaxDisplayGain (chip channels
    // often peak around 0.1, two pixels at unity). Only the drawing is scaled.
    float peak = 0.0f;
    for (int s = start; s < start + kWindow; ++s)
        peak = juce::jmax (peak, std::abs (0.5f * (left[static_cast<size_t> (s)] + right[static_cast<size_t> (s)])));
    const float gain = 1.0f / juce::jmax (peak, 1.0f / kMaxDisplayGain);

    const float halfPixel = 1.0f / juce::jmax (1.0f, static_cast<float> (tile.wave.getHeight()));   // in +/-1 units
    bool changed = false;
    bool flat = true;
    for (int c = 0; c < tile.columns; ++c)
    {
        const int s0 = start + (c * kWindow) / tile.columns;
        const int s1 = juce::jmax (s0 + 1, start + ((c + 1) * kWindow) / tile.columns);
        float lo = 1.0f, hi = -1.0f;
        for (int s = s0; s < s1; ++s)
        {
            const float v = juce::jlimit (-1.0f, 1.0f, gain * 0.5f * (left[static_cast<size_t> (s)] + right[static_cast<size_t> (s)]));
            lo = juce::jmin (lo, v);
            hi = juce::jmax (hi, v);
        }
        flat = flat && lo == 0.0f && hi == 0.0f;
        auto& oldLo = tile.low[static_cast<size_t> (c)];
        auto& oldHi = tile.high[static_cast<size_t> (c)];
        if (std::abs (oldLo - lo) >= halfPixel || std::abs (oldHi - hi) >= halfPixel)
        {
            changed = true;
            oldLo = lo;
            oldHi = hi;
        }
    }
    tile.flat = flat;
    return changed;
}

void ChannelScope::update()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastUpdateMs < theme::kScopeIntervalMs)
        return;
    lastUpdateMs = now;
    for (auto& t : tiles)
        if (readTile (t))
            repaint (t.wave);
}

void ChannelScope::paint (juce::Graphics& g)
{
    g.setFont (theme::font());
    for (const auto& t : tiles)
    {
        g.setColour (theme::colours::panel);
        g.fillRoundedRectangle (t.bounds.toFloat(), theme::kRadius);
        g.setColour (theme::colours::divider);
        g.drawRoundedRectangle (t.bounds.toFloat().reduced (0.5f * theme::kBorder), theme::kRadius, theme::kBorder);

        g.setColour (theme::colours::textDim);
        g.drawText (t.name, t.bounds.reduced (theme::kPad, 0).withHeight (theme::kLabelHeight + theme::kUnit),
                    juce::Justification::centredLeft, true);

        const float midY = static_cast<float> (t.wave.getCentreY());
        const float half = 0.5f * static_cast<float> (t.wave.getHeight());
        g.setColour (theme::colours::track);
        g.fillRect (static_cast<float> (t.wave.getX()), midY, static_cast<float> (t.wave.getWidth()), 1.0f);

        g.setColour (theme::colours::accent);
        for (int c = 0; c < t.columns; ++c)
        {
            const float top = midY - t.high[static_cast<size_t> (c)] * half;
            const float bottom = midY - t.low[static_cast<size_t> (c)] * half;
            g.fillRect (static_cast<float> (t.wave.getX() + c), top, 1.0f, juce::jmax (1.0f, bottom - top));
        }
    }
}

} // namespace rcv
