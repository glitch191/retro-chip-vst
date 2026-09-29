#include "ui/SnesPanel.h"

namespace rcv
{

SnesPanel::SnesPanel (UiContext& context)
    : ChipPanel (context, chipdsp::ChipId::Snes)
{
    // Group order of docs/ENGINE_SPECS.md.
    createGroups ({ "Instrument", "Echo", "Global" });
}

std::vector<ChipPanel::RowSpec> SnesPanel::layoutSpec() const
{
    return { RowSpec { { ColumnSpec { { "Instrument" }, 1.6f },
                         ColumnSpec { { "Echo", "Global" }, 1.0f } } } };
}

} // namespace rcv
