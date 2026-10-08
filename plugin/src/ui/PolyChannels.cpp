#include "ui/PolyChannels.h"

#include "ui/Theme.h"

#include "chipdsp/EngineFactory.h"

#include <cmath>

namespace rcv
{

namespace
{
    const juce::String kCaption ("Poly channels");

    struct ChannelLabel
    {
        const char* shortName;
        const char* fullName;
    };

    constexpr ChannelLabel kNesLabels[] = { { "P1", "Pulse 1" }, { "P2", "Pulse 2" }, { "Tri", "Triangle" },
                                            { "Noi", "Noise" }, { "DMC", "DMC" } };
    constexpr ChannelLabel kSnesLabels[] = { { "1", "Voice 1" }, { "2", "Voice 2" }, { "3", "Voice 3" }, { "4", "Voice 4" },
                                             { "5", "Voice 5" }, { "6", "Voice 6" }, { "7", "Voice 7" }, { "8", "Voice 8" } };
    constexpr ChannelLabel kGenesisLabels[] = { { "F1", "FM 1" }, { "F2", "FM 2" }, { "F3", "FM 3" }, { "F4", "FM 4" },
                                                { "F5", "FM 5" }, { "F6", "FM 6" }, { "T1", "PSG tone 1" },
                                                { "T2", "PSG tone 2" }, { "T3", "PSG tone 3" }, { "N", "PSG noise" } };

    const ChannelLabel& channelLabel (chipdsp::ChipId chip, int channel)
    {
        switch (chip)
        {
            case chipdsp::ChipId::Nes: return kNesLabels[channel];
            case chipdsp::ChipId::Snes: return kSnesLabels[channel];
            case chipdsp::ChipId::Genesis: break;
        }
        return kGenesisLabels[channel];
    }

    // Toggles on the first row; the rest go on the second.
    int firstRowCount (chipdsp::ChipId chip)
    {
        switch (chip)
        {
            case chipdsp::ChipId::Nes: return 3;   // pulses and triangle / noise and DMC
            case chipdsp::ChipId::Snes: return 4;
            case chipdsp::ChipId::Genesis: break;
        }
        return 6;   // FM / PSG
    }

    std::unique_ptr<juce::ParameterAttachment> attach (juce::AudioProcessorValueTreeState& apvts, const char* id,
                                                       std::function<void (float)> callback)
    {
        auto* param = apvts.getParameter (id);
        return param != nullptr ? std::make_unique<juce::ParameterAttachment> (*param, std::move (callback)) : nullptr;
    }
} // namespace

PolyChannelsControl::PolyChannelsControl (UiContext& context)
    : ctx (context)
{
    setComponentID (ParamIds::polyChannels);
    for (auto& b : buttons)
    {
        b.setWantsKeyboardFocus (false);   // see ParamControl: Space stays with the host
        addChildComponent (b);
    }
    for (int i = 0; i < kMaxChannels; ++i)
        buttons[static_cast<size_t> (i)].onClick = [this, i] { toggleChannel (i); };

    // The attachments pass the new value before the APVTS raw value is updated: keep it.
    maskAttachment = attach (ctx.apvts, ParamIds::polyChannels, [this] (float v) { maskValue = v; refresh(); });
    chipAttachment = attach (ctx.apvts, ParamIds::chip, [this] (float v) { chipValue = v; refresh(); });
    voiceModeAttachment = attach (ctx.apvts, ParamIds::voiceMode, [this] (float v) { voiceModeValue = v; refresh(); });
    for (auto* a : { maskAttachment.get(), chipAttachment.get(), voiceModeAttachment.get() })
        if (a != nullptr)
            a->sendInitialUpdate();
}

int PolyChannelsControl::preferredHeight() noexcept
{
    return theme::kClusterTitleHeight + 2 * theme::kMinTarget + theme::kUnit;
}

chipdsp::ChipId PolyChannelsControl::currentChip() const
{
    return static_cast<chipdsp::ChipId> (juce::jlimit (0, ParamRegistry::kNumChips - 1, juce::roundToInt (chipValue)));
}

uint32_t PolyChannelsControl::storedMask() const
{
    return static_cast<uint32_t> (juce::jlimit (0, 1023, juce::roundToInt (maskValue)));
}

uint32_t PolyChannelsControl::shownMask() const
{
    // Same resolution as EngineHost::configureAllocator().
    const auto chip = currentChip();
    const uint32_t chipMask = ParamRegistry::chipChannelMask (chip);
    const uint32_t mask = storedMask() & chipMask;
    return mask != 0 ? mask : ParamRegistry::defaultPolyMask (chip) & chipMask;
}

void PolyChannelsControl::toggleChannel (int channel)
{
    const auto chip = currentChip();
    if (channel < 0 || channel >= chipdsp::chipChannelCount (chip))
        return;

    const uint32_t chipMask = ParamRegistry::chipChannelMask (chip);
    const uint32_t mask = ((storedMask() & ~chipMask) | shownMask()) ^ (1u << channel);
    if ((mask & chipMask) != 0 && maskAttachment != nullptr)
        maskAttachment->setValueAsCompleteGesture (static_cast<float> (mask == ParamRegistry::defaultPolyMask (chip) ? 0u : mask));
    refresh();   // a refused click (last lit channel) puts the toggle back
}

void PolyChannelsControl::refresh()
{
    const auto chip = currentChip();
    const int count = chipdsp::chipChannelCount (chip);
    const uint32_t mask = shownMask();
    for (int i = 0; i < kMaxChannels; ++i)
    {
        auto& b = buttons[static_cast<size_t> (i)];
        const bool used = i < count;
        b.setVisible (used);
        if (! used)
            continue;
        const auto& label = channelLabel (chip, i);
        b.setButtonText (label.shortName);
        b.setTooltip (juce::String (label.fullName) + ": " + ((mask >> i) & 1u ? "used by Poly mode and the arpeggiator"
                                                                                  : "not used by Poly mode and the arpeggiator"));
        b.setToggleState (((mask >> i) & 1u) != 0, juce::dontSendNotification);
    }

    const bool poly = juce::roundToInt (voiceModeValue) == static_cast<int> (VoiceMode::Poly);
    if (poly != isEnabled())
    {
        setEnabled (poly);
        repaint();   // the caption colour
    }
    resized();
}

void PolyChannelsControl::paint (juce::Graphics& g)
{
    // Cluster caption: orange silkscreen and a rule to the right edge, as in the chip panels.
    g.setFont (theme::silkFont());
    g.setColour (isEnabled() ? theme::colours::silkOrange : theme::colours::textDisabled);
    const juce::Rectangle<int> area (0, 0, getWidth(), theme::kClusterTitleHeight);
    g.drawFittedText (kCaption.toUpperCase(), area, juce::Justification::centredLeft, 1, theme::kMinHorizontalScale);
    const int textRight = static_cast<int> (std::ceil (theme::drawnSilkWidth (kCaption, static_cast<float> (area.getWidth())))) + theme::kGap;
    if (textRight < area.getRight())
    {
        g.setColour (theme::colours::silkOrange.withAlpha (0.35f));
        g.fillRect (textRight, area.getCentreY(), area.getRight() - textRight, 1);
    }
}

void PolyChannelsControl::resized()
{
    const auto chip = currentChip();
    const int count = chipdsp::chipChannelCount (chip);
    const int perRow[2] = { firstRowCount (chip), count - firstRowCount (chip) };
    int index = 0;
    int y = theme::kClusterTitleHeight;
    for (int n : perRow)
    {
        const int w = (getWidth() - (n - 1) * theme::kUnit) / juce::jmax (1, n);
        for (int i = 0; i < n; ++i, ++index)
            buttons[static_cast<size_t> (index)].setBounds (i * (w + theme::kUnit), y, i == n - 1 ? getWidth() - i * (w + theme::kUnit) : w,
                                                            theme::kMinTarget);
        y += theme::kMinTarget + theme::kUnit;
    }
}

} // namespace rcv
