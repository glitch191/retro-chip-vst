#include "ui/GenesisPanel.h"

#include "ui/Theme.h"

#include <cmath>

namespace rcv
{

namespace
{
    constexpr int kNumOperators = 4;

    juce::String operatorGroup (int op)
    {
        return "Operator " + juce::String (op);
    }

    // "Op 1 TL" / "Operator 1 TL" -> "TL"
    juce::String rowLabel (const ParamInfo& info)
    {
        juce::StringArray words;
        words.addTokens (info.desc.name != nullptr ? juce::String (info.desc.name) : info.engineKey(), " ", "");
        words.removeEmptyStrings();
        if (words.size() > 2 && (words[0] == "Op" || words[0] == "Operator") && words[1].containsOnly ("0123456789"))
            words.removeRange (0, 2);
        return words.joinIntoString (" ");
    }
} // namespace

// ----- GenesisPanel ---------------------------------------------------------------------------

GenesisPanel::GenesisPanel (UiContext& context)
    : ChipPanel (context, chipdsp::ChipId::Genesis)
{
    std::vector<juce::String> operators;
    for (int op = 1; op <= kNumOperators; ++op)
        operators.push_back (operatorGroup (op));

    // Group order of docs/ENGINE_SPECS.md; the operator groups form the grid instead.
    // The engine names the FM patch group "FM Patch" (ENGINE_SPECS.md "FM patch"); "FM" is
    // accepted as well.
    createGroups ({ "Global", "FM Patch", "FM", operators[0], operators[1], operators[2], operators[3], "DAC", "PSG" }, operators);

    auto grid = std::make_unique<OperatorGrid> (context, chipdsp::ChipId::Genesis, kNumOperators);
    if (! grid->isEmpty())
        addSection (std::move (grid));
}

std::vector<ChipPanel::RowSpec> GenesisPanel::layoutSpec() const
{
    return { RowSpec { { ColumnSpec { { "Global" }, 1.0f, true },
                         ColumnSpec { { "DAC" }, 1.0f } } },
             RowSpec { { ColumnSpec { { "FM Patch", "FM" }, 1.1f },
                         ColumnSpec { { "Operators" }, 1.0f, true },
                         ColumnSpec { { "PSG" }, 1.0f } } } };
}

// ----- OperatorGrid ---------------------------------------------------------------------------

OperatorGrid::OperatorGrid (UiContext& ctx, chipdsp::ChipId chip, int numOperators)
    : PanelSection ("Operators"), numOps (numOperators)
{
    const juce::String firstPrefix ("op1_");
    for (const auto* info : ctx.registry.engineParams (chip))
    {
        if (info->group != operatorGroup (1) || ! info->engineKey().startsWith (firstPrefix))
            continue;
        const auto suffix = info->engineKey().substring (firstPrefix.length());

        Row row;
        row.label = rowLabel (*info);
        for (int op = 1; op <= numOps; ++op)
        {
            const auto* cellInfo = ctx.registry.findByKey (chip, "op" + juce::String (op) + "_" + suffix);
            if (cellInfo == nullptr)
            {
                row.cells.push_back (nullptr);
                continue;
            }
            // The row header names the parameter; a toggle cell only says "On".
            auto control = createParamControl (ctx, *cellInfo, isToggleParam (*cellInfo) ? juce::String ("On") : row.label, ControlStyle::Bare);
            addAndMakeVisible (*control);
            row.cells.push_back (control.get());
            controls.push_back (std::move (control));
        }
        rows.push_back (std::move (row));
    }
}

int OperatorGrid::gridTop() noexcept
{
    return theme::kGroupTitleHeight + theme::kLabelHeight + theme::kUnit;
}

int OperatorGrid::labelColumnWidth() const
{
    const auto f = theme::layoutNameFont();
    float w = 0.0f;
    for (const auto& row : rows)
        w = juce::jmax (w, theme::textWidth (f, row.label.toUpperCase()));
    return static_cast<int> (std::ceil (w));
}

int OperatorGrid::cellWidth() const
{
    // Capped: a long enumeration (SSG-EG "B: down, hold high") would otherwise widen all
    // sixteen columns. A capped combo box shows its value with an ellipsis; its tooltip
    // carries the full value.
    int w = theme::kKnobMinWidth;
    for (const auto& c : controls)
        w = juce::jmax (w, juce::jmin (theme::kGridCellMaxWidth, c->preferredWidth()));
    return w;
}

int OperatorGrid::heightForWidth (int) const
{
    return gridTop() + static_cast<int> (rows.size()) * theme::kGridRowHeight + theme::kPad;
}

int OperatorGrid::preferredWidth() const
{
    return 2 * theme::kPad + labelColumnWidth() + numOps * (theme::kGap + cellWidth());
}

void OperatorGrid::resized()
{
    const int labelW = labelColumnWidth();
    const int cellW = cellWidth();
    for (size_t r = 0; r < rows.size(); ++r)
    {
        const int y = gridTop() + static_cast<int> (r) * theme::kGridRowHeight + (theme::kGridRowHeight - theme::kControlHeight) / 2;
        for (size_t op = 0; op < rows[r].cells.size(); ++op)
            if (auto* cell = rows[r].cells[op])
                cell->setBounds (theme::kPad + labelW + theme::kGap + static_cast<int> (op) * (cellW + theme::kGap), y,
                                 cellW, theme::kControlHeight);
    }
}

void OperatorGrid::paint (juce::Graphics& g)
{
    paintBox (g);

    const int labelW = labelColumnWidth();
    const int cellW = cellWidth();
    // Column and row names in orange silkscreen, rows separated by engraved rules.
    g.setColour (theme::colours::silkOrange);
    g.setFont (theme::silkFont());
    for (int op = 0; op < numOps; ++op)
        g.drawFittedText ("OP " + juce::String (op + 1),
                          theme::kPad + labelW + theme::kGap + op * (cellW + theme::kGap), theme::kGroupTitleHeight,
                          cellW, theme::kLabelHeight, juce::Justification::centredLeft, 1, theme::kMinHorizontalScale);

    g.setFont (theme::nameFont());
    for (size_t r = 0; r < rows.size(); ++r)
    {
        const int y = gridTop() + static_cast<int> (r) * theme::kGridRowHeight;
        if (r > 0)
        {
            g.setColour (theme::colours::rule);
            g.fillRect (theme::kPad, y, getWidth() - 2 * theme::kPad, 1);
        }
        g.setColour (theme::colours::silkOrange);
        g.drawFittedText (rows[r].label.toUpperCase(), theme::kPad, y, labelW, theme::kGridRowHeight, juce::Justification::centredLeft, 1,
                          theme::kMinHorizontalScale);
    }
}

} // namespace rcv
