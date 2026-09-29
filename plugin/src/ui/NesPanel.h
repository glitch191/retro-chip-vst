#pragma once

#include "ui/ChipPanel.h"

namespace rcv
{

// NES (2A03): four columns: Pulse 1, Pulse 2, Triangle above DMC, Noise above the chip
// globals (clock, console filter). The 2A03 has no effects, so there is no effects group.
class NesPanel final : public ChipPanel
{
public:
    explicit NesPanel (UiContext& context);

protected:
    std::vector<RowSpec> layoutSpec() const override;
};

} // namespace rcv
