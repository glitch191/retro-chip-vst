#pragma once

#include <juce_graphics/juce_graphics.h>

#include "ui/Theme.h"

// Editor regions at scale 1.0 (1280 x 720 logical px). The whole content is scaled by
// ui_scale, so these rectangles never change at runtime.
//
//   +--------------------------------------------------------------+
//   | header: chip, presets, search, randomize, UI scale, diag.    |
//   +------------------------------------------+-------------------+
//   | chip panel (NES / SNES / Genesis)        | sidebar: arp,     |
//   |                                          | glide, output     |
//   +------------------------------------------+-------------------+
//   | channel scopes: main + one per hardware channel              |
//   +--------------------------------------------------------------+
namespace rcv::layout
{

inline constexpr int kMargin = theme::kGap;

inline juce::Rectangle<int> header()
{
    return { kMargin, kMargin, theme::kBaseWidth - 2 * kMargin, theme::kHeaderHeight };
}

inline juce::Rectangle<int> scopes()
{
    return { kMargin, theme::kBaseHeight - kMargin - theme::kScopeHeight, theme::kBaseWidth - 2 * kMargin, theme::kScopeHeight };
}

inline juce::Rectangle<int> sidebar()
{
    const int top = header().getBottom() + theme::kGap;
    return { theme::kBaseWidth - kMargin - theme::kSidebarWidth, top, theme::kSidebarWidth, scopes().getY() - theme::kGap - top };
}

inline juce::Rectangle<int> panel()
{
    const auto side = sidebar();
    return { kMargin, side.getY(), side.getX() - theme::kGap - kMargin, side.getHeight() };
}

} // namespace rcv::layout
