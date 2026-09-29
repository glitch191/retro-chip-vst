#pragma once

#include "ui/ChipPanel.h"

namespace rcv
{

// SNES (S-DSP): the instrument settings shared by the eight voices on the left; the echo
// unit (enable, delay, feedback, volume, FIR preset and the eight per-voice echo toggles)
// and the main volume on the right.
class SnesPanel final : public ChipPanel
{
public:
    explicit SnesPanel (UiContext& context);

protected:
    std::vector<RowSpec> layoutSpec() const override;
};

} // namespace rcv
