#pragma once

#include "ui/ParamControl.h"

#include <array>
#include <memory>

namespace rcv
{

// Editor control of `poly_channels` (docs/PLUGIN_SPECS.md "Global parameters"): a caption and
// one toggle per hardware channel of the active chip, on two rows (NES P1 P2 Tri / Noi DMC,
// SNES 1..4 / 5..8, Genesis F1..F6 / T1 T2 T3 N). A lit toggle is a channel that Poly mode
// and the arpeggiator may use.
//
// The toggles show the mask the engine host resolves: the stored bits of the chip, or the
// chip's default mask when none is set (poly_channels 0). A click writes the whole mask as
// one host gesture, keeping the bits that belong to other chips. The last lit channel cannot
// be switched off (the host would fall back to the chip default anyway), and a mask equal to
// the chip default is written as 0, so the default keeps following the chip. Disabled in
// MIDI channel voice mode, where the mask is not used.
class PolyChannelsControl final : public juce::Component
{
public:
    static constexpr int kMaxChannels = 10;

    explicit PolyChannelsControl (UiContext& ctx);

    static int preferredHeight() noexcept;

    // The click handler of channel `channel` (0-based), public for the tests.
    void toggleChannel (int channel);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    chipdsp::ChipId currentChip() const;
    uint32_t storedMask() const;
    uint32_t shownMask() const;   // the mask the engine host uses for the current chip
    void refresh();

    UiContext& ctx;
    std::array<juce::ToggleButton, kMaxChannels> buttons;
    float maskValue = 0.0f, chipValue = 0.0f, voiceModeValue = static_cast<float> (VoiceMode::Poly);
    std::unique_ptr<juce::ParameterAttachment> maskAttachment, chipAttachment, voiceModeAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PolyChannelsControl)
};

} // namespace rcv
