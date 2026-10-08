#include "ui/DiagnosticsOverlay.h"

#include "ui/Theme.h"

#include <algorithm>

namespace rcv
{

namespace
{
    const juce::StringArray kLabels { "Refresh rate", "Frame cost (mean)", "Frame cost (worst 1 %)", "Repaints per second" };
    constexpr int kLabelColumn = theme::kDiagLabelColumn;
    constexpr int kValueColumn = theme::kDiagValueColumn;
}

void DiagnosticsOverlay::Ring::push (double atMs, double value) noexcept
{
    if (count == kCapacity)
    {
        start = (start + 1) % kCapacity;
        --count;
    }
    items[static_cast<size_t> ((start + count) % kCapacity)] = { atMs, value };
    ++count;
}

void DiagnosticsOverlay::Ring::dropOlderThan (double limitMs) noexcept
{
    while (count > 0 && items[static_cast<size_t> (start)].atMs < limitMs)
    {
        start = (start + 1) % kCapacity;
        --count;
    }
}

DiagnosticsOverlay::DiagnosticsOverlay()
{
    setOpaque (true);
    setInterceptsMouseClicks (true, false);   // no invisible click targets under the overlay
    for (int i = 0; i < kLabels.size(); ++i)
        lines.add ("-");
}

int DiagnosticsOverlay::preferredWidth()
{
    return 2 * theme::kPad + kLabelColumn + kValueColumn;
}

int DiagnosticsOverlay::preferredHeight()
{
    return theme::kGroupTitleHeight + kLabels.size() * theme::kLabelHeight + (kLabels.size() - 1) * theme::kUnit + theme::kPad;
}

void DiagnosticsOverlay::setActive (bool shouldBeActive)
{
    active = shouldBeActive;
    lastTimestampSec = -1.0;
    pendingPaintMs = 0.0;
    timestampDeltas = {};
    frameCosts = {};
    repaints = {};
    for (auto& l : lines)
        l = "-";
    setVisible (active);
    if (active)
        repaint();
}

void DiagnosticsOverlay::countRepaint()
{
    if (active)
        repaints.push (juce::Time::getMillisecondCounterHiRes(), 1.0);
}

void DiagnosticsOverlay::addPaintTime (double ms)
{
    if (active)
        pendingPaintMs += ms;
}

void DiagnosticsOverlay::frame (double vblankTimestampSec, double vblankWorkMs)
{
    if (! active)
        return;

    const double now = juce::Time::getMillisecondCounterHiRes();
    if (lastTimestampSec >= 0.0 && vblankTimestampSec > lastTimestampSec)
        timestampDeltas.push (now, (vblankTimestampSec - lastTimestampSec) * 1000.0);
    // Frame cost: this vblank callback plus every editor paint since the previous one.
    frameCosts.push (now, vblankWorkMs + pendingPaintMs);
    pendingPaintMs = 0.0;
    lastTimestampSec = vblankTimestampSec;

    if (now - lastTextMs >= kTextIntervalMs)
    {
        lastTextMs = now;
        refreshText (now);
    }
}

void DiagnosticsOverlay::refreshText (double nowMs)
{
    timestampDeltas.dropOlderThan (nowMs - kWindowMs);
    frameCosts.dropOlderThan (nowMs - kWindowMs);
    repaints.dropOlderThan (nowMs - 1000.0);

    juce::StringArray next;

    // Refresh rate: median presentation interval.
    if (timestampDeltas.count > 0)
    {
        const int n = timestampDeltas.count;
        for (int i = 0; i < n; ++i)
            scratch[static_cast<size_t> (i)] = timestampDeltas.at (i).value;
        std::nth_element (scratch.begin(), scratch.begin() + n / 2, scratch.begin() + n);
        const double median = scratch[static_cast<size_t> (n / 2)];
        next.add (median > 0.0 ? juce::String (1000.0 / median, 1) + " Hz" : juce::String ("-"));
    }
    else
    {
        next.add ("-");
    }

    if (frameCosts.count > 0)
    {
        const int n = frameCosts.count;
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            scratch[static_cast<size_t> (i)] = frameCosts.at (i).value;
            sum += scratch[static_cast<size_t> (i)];
        }
        const int worstIndex = juce::jlimit (0, n - 1, static_cast<int> (0.99 * static_cast<double> (n)));
        std::nth_element (scratch.begin(), scratch.begin() + worstIndex, scratch.begin() + n);
        next.add (juce::String (sum / n, 2) + " ms");
        next.add (juce::String (scratch[static_cast<size_t> (worstIndex)], 2) + " ms");
    }
    else
    {
        next.add ("-");
        next.add ("-");
    }

    next.add (juce::String (repaints.count));

    if (next != lines)
    {
        lines = next;
        repaint();
    }
}

void DiagnosticsOverlay::paint (juce::Graphics& g)
{
    // Opaque: the corners outside the rounded window are filled with the window's edge.
    g.fillAll (theme::colours::controlEdge);
    theme::drawRecess (g, getLocalBounds().toFloat());

    g.setColour (theme::colours::silk);
    g.setFont (theme::silkFont (theme::kFontGroup));
    g.drawText ("DIAGNOSTICS", theme::kPad, theme::kOpticalOffset, getWidth() - 2 * theme::kPad, theme::kGroupTitleHeight, juce::Justification::centredLeft, false);

    g.setFont (theme::font());
    int y = theme::kGroupTitleHeight;
    for (int i = 0; i < kLabels.size(); ++i)
    {
        g.setColour (theme::colours::textDim);
        g.drawFittedText (kLabels[i], theme::kPad, y, kLabelColumn, theme::kLabelHeight, juce::Justification::centredLeft, 1, theme::kMinHorizontalScale);
        g.setColour (theme::colours::text);
        g.drawFittedText (lines[i], theme::kPad + kLabelColumn, y, kValueColumn, theme::kLabelHeight, juce::Justification::centredRight, 1, theme::kMinHorizontalScale);
        y += theme::kLabelHeight + theme::kUnit;
    }
}

} // namespace rcv
