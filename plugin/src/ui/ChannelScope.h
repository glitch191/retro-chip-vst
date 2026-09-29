#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "VisualizerBuffers.h"

#include "chipdsp/IChipEngine.h"

#include <array>
#include <vector>

namespace rcv
{

// One small waveform per hardware channel of the active chip plus the main output, read
// from VisualizerBuffers (lock-free rings written by the audio thread).
//
// update() runs from the editor's vblank callback but reads the rings at most every
// theme::kScopeIntervalMs (about 60 Hz, whatever the display rate). A tile is only re-read
// when its ring's generation counter moved and the ring did not only receive silence since
// the tile was drawn flat, and only repainted when the drawn columns actually changed by
// at least half a pixel: a silent or unchanged signal costs neither a copy nor a repaint,
// so the editor stays idle at rest even though the audio thread keeps pushing (silent)
// blocks.
//
// Tooltips: each tile names its channel and the output bus that carries it.
//
// Display: the newest kWindow samples (about 21 ms at 48 kHz), started on the first rising
// zero crossing of a longer read so periodic waveforms stand still; one min/max bar per
// pixel column. Each tile scales its window so the peak fills the tile, with at most
// kMaxDisplayGain (24 dB): the real chips' channels often peak near 0.1, which is two
// pixels at +/-1 full scale. A steady signal keeps a steady gain, so it stays at rest.
class ChannelScope final : public juce::Component,
                           public juce::TooltipClient
{
public:
    static constexpr int kWindow = 1024;
    static constexpr int kRead = 2048;
    static constexpr int kMaxColumns = 512;
    static constexpr float kMaxDisplayGain = 16.0f;

    explicit ChannelScope (VisualizerBuffers& buffers);

    // Rebuilds the tiles for a chip: main + one tile per hardware channel, named from the engine.
    void setChip (const chipdsp::IChipEngine& engine);

    void update();

    void paint (juce::Graphics& g) override;
    void resized() override;
    juce::String getTooltip() override;

private:
    struct Tile
    {
        juce::String name;
        int slot = 0;
        juce::uint32 lastGeneration = 0;
        juce::Rectangle<int> bounds;   // whole tile
        juce::Rectangle<int> wave;     // waveform area inside it
        int columns = 0;
        bool flat = false;             // every column drawn at zero
        std::array<float, kMaxColumns> low {}, high {};
    };

    bool readTile (Tile& tile);   // true when the drawn columns changed

    VisualizerBuffers& buffers;
    std::vector<Tile> tiles;
    double lastUpdateMs = 0.0;
    std::array<float, kRead> left {}, right {};
};

} // namespace rcv
