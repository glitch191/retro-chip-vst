#pragma once

#include "ui/ChipPanel.h"

namespace rcv
{

// Genesis (YM2612 + SN76489):
//   top:    chip globals (clock, revision, Model 1 filter, LFO) and the DAC channel
//   bottom: FM globals on the left, the four operators as a grid (one column per operator,
//           one row per operator parameter) in the middle, the PSG on the right.
class GenesisPanel final : public ChipPanel
{
public:
    explicit GenesisPanel (UiContext& context);

protected:
    std::vector<RowSpec> layoutSpec() const override;
};

// The operator grid: column n shows the "Operator n" group, rows follow the parameters of
// Operator 1 (TL, AR, DR, SR, RR, SL, MUL, DT, RS, AM, SSG-EG). Cells hold bare controls
// (knob + value, combo box or toggle) so a row reads like a patch table.
class OperatorGrid final : public PanelSection
{
public:
    OperatorGrid (UiContext& ctx, chipdsp::ChipId chip, int numOperators);

    bool isEmpty() const noexcept { return rows.empty(); }

    int heightForWidth (int width) const override;
    int preferredWidth() const override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Row
    {
        juce::String label;
        std::vector<ParamControl*> cells;   // one per operator, nullptr when missing
    };

    static int gridTop() noexcept;          // y of the first row (below the column headers)
    int labelColumnWidth() const;
    int cellWidth() const;

    std::vector<std::unique_ptr<ParamControl>> controls;
    std::vector<Row> rows;
    int numOps;
};

} // namespace rcv
