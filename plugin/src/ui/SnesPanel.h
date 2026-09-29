#pragma once

#include "ui/ChipPanel.h"
#include "ui/SampleListSection.h"

namespace rcv
{

// SNES (S-DSP): the instrument settings shared by the eight voices on the left, with the
// read-only list of loaded samples and free APU RAM below; the echo unit (enable, delay,
// feedback, volume, FIR preset and the eight per-voice echo toggles) and the main volume on
// the right.
class SnesPanel final : public ChipPanel
{
public:
    explicit SnesPanel (UiContext& context);

    // Message thread; repaints only when the content changes.
    void setSampleList (std::vector<SampleListSection::Entry> entries, int freeBytes);

protected:
    std::vector<RowSpec> layoutSpec() const override;

private:
    SampleListSection* sampleList = nullptr;   // owned by ChipPanel
};

} // namespace rcv
