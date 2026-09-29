#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace rcv
{

// Rendering diagnostics, toggled by the "Diagnostics" button (off by default).
//
//   Refresh rate    1 / median interval between VBlankAttachment timestamps (the display's
//                   presentation times), over the last 2 s
//   Frame cost      mean time the editor spends per displayed frame, over the last 2 s:
//                   the vblank callback (MIDI learn drain, transitions, scopes) plus every
//                   editor paint since the previous vblank, measured with the wall clock
//                   (Time::getMillisecondCounterHiRes). It is the work, not the interval
//                   between frames (that is 1 / refresh rate).
//   Worst 1 %       99th percentile of the frame cost over the last 2 s
//   Repaints        editor paints per second, counted by the editor content for every paint
//                   whose clip is not inside this overlay; must stay at 0 at rest
//
// The overlay is opaque, takes the clicks on its area (the controls it covers are not
// reachable while it is shown), refreshes its text at most every kTextIntervalMs, and only
// repaints when the text changed; its own repaints are not counted. When hidden it records
// nothing (frame(), addPaintTime() and countRepaint() return immediately).
class DiagnosticsOverlay final : public juce::Component
{
public:
    static constexpr double kWindowMs = 2000.0;
    static constexpr double kTextIntervalMs = 250.0;
    static constexpr int kCapacity = 1024;   // > 2 s of frames at 500 Hz

    DiagnosticsOverlay();

    void setActive (bool shouldBeActive);
    bool isActive() const noexcept { return active; }

    void frame (double vblankTimestampSec, double vblankWorkMs);
    void addPaintTime (double ms);
    void countRepaint();

    static int preferredWidth();
    static int preferredHeight();

    void paint (juce::Graphics& g) override;

private:
    struct Sample
    {
        double atMs = 0.0;
        double value = 0.0;
    };

    struct Ring
    {
        std::array<Sample, kCapacity> items {};
        int start = 0, count = 0;
        void push (double atMs, double value) noexcept;
        void dropOlderThan (double limitMs) noexcept;
        const Sample& at (int i) const noexcept { return items[static_cast<size_t> ((start + i) % kCapacity)]; }
    };

    void refreshText (double nowMs);

    bool active = false;
    double lastTimestampSec = -1.0;
    double pendingPaintMs = 0.0;
    double lastTextMs = 0.0;
    Ring timestampDeltas, frameCosts, repaints;
    std::array<double, kCapacity> scratch {};
    juce::StringArray lines;
};

} // namespace rcv
