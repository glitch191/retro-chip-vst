#pragma once

#include "ui/ParamGroup.h"

#include <vector>

namespace rcv
{

// Read-only "Samples" box of the SNES panel: the loaded sample slots (slot number and
// factory sample name, or "User sample") flowing in columns, and the free APU RAM on the
// last line. The editor sets the content on the message thread when a preset, a sample or
// the echo delay changes; setContent() repaints only when the content differs, so nothing
// repaints at rest.
class SampleListSection final : public PanelSection
{
public:
    struct Entry
    {
        int slot = 0;
        juce::String name;   // factory sample name, or empty for a user sample

        bool operator== (const Entry& other) const { return slot == other.slot && name == other.name; }
    };

    SampleListSection();

    // freeBytes < 0: unknown (no line shown).
    void setContent (std::vector<Entry> entries, int freeBytes);

    int heightForWidth (int width) const override;
    int preferredWidth() const override;
    void paint (juce::Graphics& g) override;

private:
    std::vector<Entry> slots;
    int free = -1;
};

} // namespace rcv
