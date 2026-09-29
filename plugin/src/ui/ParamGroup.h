#pragma once

#include "ui/ParamControl.h"

#include <memory>
#include <vector>

namespace rcv
{

// A titled box of the editor that knows its height for a given width. Chip panels stack
// sections in columns (ChipPanel::layoutRows).
//
// Tooltips: a title (or, in a ParamGroup, a cluster caption) that is cut with an ellipsis
// shows its full text as the tooltip of that area.
class PanelSection : public juce::Component,
                     public juce::TooltipClient
{
public:
    // A click on empty space focuses the section itself, never one of its controls.
    explicit PanelSection (juce::String titleIn) : title (std::move (titleIn)) { setWantsKeyboardFocus (true); }

    const juce::String& sectionTitle() const noexcept { return title; }

    virtual int heightForWidth (int width) const = 0;
    virtual int preferredWidth() const = 0;   // natural width (no wrapping)

    juce::String getTooltip() override;

protected:
    void paintBox (juce::Graphics& g) const;  // panel fill, 1 px outline, bold title
    juce::Rectangle<int> titleArea() const;

    // Full text of a truncated caption at 'position' (local coordinates), or empty.
    virtual juce::String truncatedCaptionAt (juce::Point<int>) const { return {}; }

    juce::String title;
};

// Group box of parameter controls laid out in knob cells.
//
// Controls are organised in clusters: related parameters under a small caption ("Sweep",
// "Vibrato", "Voice echo"), and untitled clusters of single parameters. Clusters flow left
// to right and wrap to a new row when they do not fit; a cluster wider than the whole group
// wraps inside itself. Toggles are half a cell high, so consecutive toggles of a cluster
// stack two per column (first half on the top line, second half below).
class ParamGroup final : public PanelSection
{
public:
    struct Item
    {
        const ParamInfo* info = nullptr;
        juce::String label;
    };

    struct Cluster
    {
        juce::String title;
        std::vector<Item> items;
    };

    ParamGroup (UiContext& ctx, juce::String titleIn, const std::vector<Cluster>& clusters,
                ControlStyle style = ControlStyle::Stacked);

    // Derives short labels and clusters from engine parameter names (see ParamGroup.cpp):
    // the group prefix is dropped ("Pulse 1 Duty" -> "Duty"), runs sharing a leading word
    // become a cluster ("Sweep": Enable, Period, Negate, Shift) and runs differing only in a
    // number become a numbered cluster ("Voice 1 Echo".."Voice 8 Echo" -> "Voice echo": 1..8).
    static std::vector<Cluster> clustersFor (const juce::String& groupName, const std::vector<const ParamInfo*>& params);

    // Adds an extra component (a plain button) as a full-height column after the controls.
    void addExtraColumn (juce::Component& component, int width);

    int heightForWidth (int width) const override;
    int preferredWidth() const override;

    ParamControl* controlFor (const juce::String& paramId) const;

    void paint (juce::Graphics& g) override;
    void resized() override;

protected:
    juce::String truncatedCaptionAt (juce::Point<int> position) const override;

private:
    struct Column
    {
        juce::Component* top = nullptr;
        juce::Component* bottom = nullptr;   // second toggle of a stack
        bool half = false;
        int width = 0;
    };

    struct ClusterColumns
    {
        juce::String title;
        std::vector<Column> columns;
    };

    struct Layout
    {
        struct Caption
        {
            juce::String text;
            juce::Rectangle<int> area;   // the caption line (text + rule)
        };
        std::vector<juce::Rectangle<int>> columnBounds;   // flattened over all clusters
        std::vector<Caption> captions;
        int height = 0;
    };

    Layout computeLayout (int width) const;

    std::vector<std::unique_ptr<ParamControl>> controls;
    std::vector<ClusterColumns> clusters;
    Layout current;
};

} // namespace rcv
