#pragma once

#include "ui/ParamGroup.h"

#include "chipdsp/ChipTypes.h"

#include <memory>
#include <vector>

namespace rcv
{

// Base of the three chip panels: one section per ParamInfo group of the chip, in the group
// order of docs/ENGINE_SPECS.md, arranged by the subclass in rows of columns.
//
// Groups the subclass does not name (an engine that adds a group, or the placeholder engines
// used during development) are never dropped: they form a last row, one flexible column each.
class ChipPanel : public juce::Component
{
public:
    ChipPanel (UiContext& ctx, chipdsp::ChipId chip);

    chipdsp::ChipId chip() const noexcept { return chipId; }

    // Height the current layout needs beyond the panel's bounds (0 when everything fits).
    // Used by the layout checks; the editor never scrolls.
    int overflow() const noexcept { return overflowPx; }

    void resized() override;

protected:
    struct ColumnSpec
    {
        std::vector<juce::String> sections;   // section titles, stacked top to bottom
        float weight = 1.0f;                  // share of the width left after natural columns
        bool natural = false;                 // width = widest section's preferredWidth()
    };

    struct RowSpec
    {
        std::vector<ColumnSpec> columns;      // the last row takes the remaining height
    };

    // Creates one ParamGroup per engine group, in groupOrder first, then the others in
    // descriptor order. Groups listed in `skip` are left to the subclass (the operator grid).
    void createGroups (const std::vector<juce::String>& groupOrder, const std::vector<juce::String>& skip = {});

    // Adds a section built by the subclass.
    void addSection (std::unique_ptr<PanelSection> section);

    // Engine parameters of one group, in descriptor order.
    std::vector<const ParamInfo*> paramsOfGroup (const juce::String& group) const;

    virtual std::vector<RowSpec> layoutSpec() const = 0;

    UiContext& ctx;

private:
    PanelSection* find (const juce::String& title) const;
    int layoutColumn (const std::vector<PanelSection*>& column, juce::Rectangle<int> area, bool place);

    chipdsp::ChipId chipId;
    std::vector<std::unique_ptr<PanelSection>> sections;
    int overflowPx = 0;
};

} // namespace rcv
