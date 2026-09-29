#include "ui/ChipPanel.h"

#include "ui/Theme.h"

#include <algorithm>

namespace rcv
{

ChipPanel::ChipPanel (UiContext& context, chipdsp::ChipId chip)
    : ctx (context), chipId (chip)
{
    setWantsKeyboardFocus (true);   // a click between groups focuses the panel, not a control
}

std::vector<const ParamInfo*> ChipPanel::paramsOfGroup (const juce::String& group) const
{
    std::vector<const ParamInfo*> result;
    for (const auto* info : ctx.registry.engineParams (chipId))
        if (info->showOnPanel && info->group == group)
            result.push_back (info);
    return result;
}

void ChipPanel::createGroups (const std::vector<juce::String>& groupOrder, const std::vector<juce::String>& skip)
{
    juce::StringArray order;
    for (const auto& g : groupOrder)
        order.add (g);
    for (const auto* info : ctx.registry.engineParams (chipId))
        if (info->showOnPanel)
            order.addIfNotAlreadyThere (info->group);

    for (const auto& group : order)
    {
        if (std::find (skip.begin(), skip.end(), group) != skip.end())
            continue;
        const auto params = paramsOfGroup (group);
        if (params.empty())
            continue;
        addSection (std::make_unique<ParamGroup> (ctx, group, ParamGroup::clustersFor (group, params)));
    }
}

void ChipPanel::addSection (std::unique_ptr<PanelSection> section)
{
    addAndMakeVisible (*section);
    sections.push_back (std::move (section));
}

PanelSection* ChipPanel::find (const juce::String& title) const
{
    for (const auto& s : sections)
        if (s->sectionTitle() == title)
            return s.get();
    return nullptr;
}

int ChipPanel::layoutColumn (const std::vector<PanelSection*>& column, juce::Rectangle<int> area, bool place)
{
    int y = area.getY();
    for (size_t i = 0; i < column.size(); ++i)
    {
        int h = column[i]->heightForWidth (area.getWidth());
        // The last section of a column reaches the bottom of its row, so columns end level.
        if (place && i + 1 == column.size())
            h = juce::jmax (h, area.getBottom() - y);
        if (place)
            column[i]->setBounds (area.getX(), y, area.getWidth(), h);
        y += h + theme::kGap;
    }
    return column.empty() ? 0 : y - theme::kGap - area.getY();
}

void ChipPanel::resized()
{
    // Resolve the spec against the sections that exist.
    struct Column
    {
        std::vector<PanelSection*> sections;
        float weight;
        bool natural;
    };
    std::vector<std::vector<Column>> rows;
    std::vector<PanelSection*> used;
    for (const auto& rowSpec : layoutSpec())
    {
        std::vector<Column> row;
        for (const auto& colSpec : rowSpec.columns)
        {
            Column col { {}, colSpec.weight, colSpec.natural };
            for (const auto& title : colSpec.sections)
                if (auto* s = find (title))
                {
                    col.sections.push_back (s);
                    used.push_back (s);
                }
            if (! col.sections.empty())
                row.push_back (std::move (col));
        }
        if (! row.empty())
            rows.push_back (std::move (row));
    }
    std::vector<PanelSection*> unplaced;
    for (const auto& s : sections)
        if (std::find (used.begin(), used.end(), s.get()) == used.end())
            unplaced.push_back (s.get());
    // Sections no spec column places get a last row of their own, one flexible column each,
    // so they use the panel width whatever the kind of the spec's last column.
    if (! unplaced.empty())
    {
        std::vector<Column> extra;
        for (auto* s : unplaced)
            extra.push_back (Column { { s }, 1.0f, false });
        rows.push_back (std::move (extra));
    }

    auto area = getLocalBounds();
    int bottom = 0;
    for (size_t r = 0; r < rows.size(); ++r)
    {
        auto& row = rows[r];
        const int available = area.getWidth() - theme::kGap * static_cast<int> (row.size() - 1);

        std::vector<int> widths (row.size(), 0);
        int naturalTotal = 0;
        float weightTotal = 0.0f;
        for (size_t c = 0; c < row.size(); ++c)
        {
            if (row[c].natural)
            {
                for (auto* s : row[c].sections)
                    widths[c] = juce::jmax (widths[c], s->preferredWidth());
                naturalTotal += widths[c];
            }
            else
            {
                weightTotal += row[c].weight;
            }
        }
        const int flexible = juce::jmax (0, available - naturalTotal);
        int assigned = naturalTotal;
        int lastFlexible = -1;
        for (size_t c = 0; c < row.size(); ++c)
        {
            if (row[c].natural)
                continue;
            widths[c] = weightTotal > 0.0f ? static_cast<int> (static_cast<float> (flexible) * row[c].weight / weightTotal) : 0;
            assigned += widths[c];
            lastFlexible = static_cast<int> (c);
        }
        if (lastFlexible >= 0)
            widths[static_cast<size_t> (lastFlexible)] += available - assigned;   // rounding remainder

        const bool lastRow = r + 1 == rows.size();
        int rowHeight = 0;
        {
            int x = area.getX();
            for (size_t c = 0; c < row.size(); ++c)
            {
                rowHeight = juce::jmax (rowHeight, layoutColumn (row[c].sections, { x, area.getY(), widths[c], 0 }, false));
                x += widths[c] + theme::kGap;
            }
        }
        const int placedHeight = lastRow ? juce::jmax (rowHeight, area.getHeight()) : rowHeight;
        auto rowArea = area.removeFromTop (placedHeight);
        area.removeFromTop (theme::kGap);

        int x = rowArea.getX();
        for (size_t c = 0; c < row.size(); ++c)
        {
            layoutColumn (row[c].sections, { x, rowArea.getY(), widths[c], placedHeight }, true);
            x += widths[c] + theme::kGap;
        }
        bottom = rowArea.getY() + rowHeight;
    }
    overflowPx = juce::jmax (0, bottom - getHeight());
}

} // namespace rcv
