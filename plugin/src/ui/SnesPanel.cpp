#include "ui/SnesPanel.h"

namespace rcv
{

SnesPanel::SnesPanel (UiContext& context)
    : ChipPanel (context, chipdsp::ChipId::Snes)
{
    // Group order of docs/ENGINE_SPECS.md.
    createGroups ({ "Instrument", "Echo", "Global" });

    auto list = std::make_unique<SampleListSection>();
    sampleList = list.get();
    addSection (std::move (list));
}

void SnesPanel::setSampleList (std::vector<SampleListSection::Entry> entries, int freeBytes)
{
    sampleList->setContent (std::move (entries), freeBytes);
}

std::vector<ChipPanel::RowSpec> SnesPanel::layoutSpec() const
{
    return { RowSpec { { ColumnSpec { { "Instrument", "Samples" }, 1.6f },
                         ColumnSpec { { "Echo", "Global" }, 1.0f } } } };
}

} // namespace rcv
