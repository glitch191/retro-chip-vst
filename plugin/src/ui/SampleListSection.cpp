#include "ui/SampleListSection.h"

#include "ui/Theme.h"

namespace rcv
{

namespace
{
    constexpr int kRowHeight = theme::kLabelHeight + theme::kUnit;   // one entry
    constexpr int kSlotWidth = 24;                                    // right-aligned slot number
    constexpr int kColumnWidth = 176;                                 // slot number + name
    constexpr int kMinRows = 2;
} // namespace

SampleListSection::SampleListSection() : PanelSection ("Samples") {}

void SampleListSection::setContent (std::vector<Entry> entries, int freeBytes)
{
    if (entries == slots && freeBytes == free)
        return;
    slots = std::move (entries);
    free = freeBytes;
    repaint();
}

int SampleListSection::heightForWidth (int) const
{
    return theme::kGroupTitleHeight + kMinRows * kRowHeight + theme::kGap + theme::kLabelHeight + theme::kPad;
}

int SampleListSection::preferredWidth() const
{
    return kColumnWidth + 2 * theme::kPad;
}

void SampleListSection::paint (juce::Graphics& g)
{
    paintBox (g);

    auto area = getLocalBounds().withTrimmedTop (theme::kGroupTitleHeight).reduced (theme::kPad, 0).withTrimmedBottom (theme::kPad);
    const auto font = theme::font();
    g.setFont (font);

    if (free >= 0)
    {
        g.setColour (theme::colours::textDim);
        g.drawText ("Free APU RAM: " + juce::String (free) + " bytes", area.removeFromBottom (theme::kLabelHeight),
                    juce::Justification::centredLeft, true);
        area.removeFromBottom (theme::kGap);
    }

    if (slots.empty())
    {
        g.setColour (theme::colours::textDim);
        g.drawText ("No samples loaded", area.removeFromTop (kRowHeight), juce::Justification::centredLeft, true);
        return;
    }

    // Column-major flow; when the box is full the last cell tells how many are not shown.
    const int rows = juce::jmax (1, area.getHeight() / kRowHeight);
    const int columns = juce::jmax (1, (area.getWidth() + theme::kGap) / (kColumnWidth + theme::kGap));
    const int capacity = rows * columns;
    const int count = static_cast<int> (slots.size());
    const int shown = count <= capacity ? count : capacity - 1;

    auto cellAt = [&] (int i)
    {
        return juce::Rectangle<int> (area.getX() + (i / rows) * (kColumnWidth + theme::kGap),
                                     area.getY() + (i % rows) * kRowHeight, kColumnWidth, kRowHeight);
    };
    for (int i = 0; i < shown; ++i)
    {
        const auto& entry = slots[static_cast<size_t> (i)];
        auto cell = cellAt (i);
        g.setColour (theme::colours::textDim);
        g.drawText (juce::String (entry.slot), cell.removeFromLeft (kSlotWidth), juce::Justification::centredRight, false);
        cell.removeFromLeft (theme::kGap);
        g.setColour (theme::colours::text);
        g.drawText (entry.name.isNotEmpty() ? entry.name : juce::String ("User sample"), cell, juce::Justification::centredLeft, true);
    }
    if (shown < count)
    {
        g.setColour (theme::colours::textDim);
        g.drawText ("+" + juce::String (count - shown) + " more", cellAt (shown), juce::Justification::centredLeft, true);
    }
}

} // namespace rcv
