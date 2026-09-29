#include "ui/NesPanel.h"

namespace rcv
{

NesPanel::NesPanel (UiContext& context)
    : ChipPanel (context, chipdsp::ChipId::Nes)
{
    // Group order of docs/ENGINE_SPECS.md.
    createGroups ({ "Global", "Pulse 1", "Pulse 2", "Triangle", "Noise", "DMC" });
}

std::vector<ChipPanel::RowSpec> NesPanel::layoutSpec() const
{
    return { RowSpec { { ColumnSpec { { "Pulse 1" } },
                         ColumnSpec { { "Pulse 2" } },
                         ColumnSpec { { "Triangle", "DMC" } },
                         ColumnSpec { { "Noise", "Global" } } } } };
}

} // namespace rcv
