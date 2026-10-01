// The editor's poly_channels control (ui/PolyChannels.h): which toggles it shows per chip,
// the mask a click writes, and its place in the sidebar.

#include "TestHelpers.h"

#include "ui/EditorLayout.h"
#include "ui/PolyChannels.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace
{
    juce::Component* findById (juce::Component& root, const juce::String& id)
    {
        if (root.getComponentID() == id)
            return &root;
        for (auto* child : root.getChildren())
            if (auto* found = findById (*child, id))
                return found;
        return nullptr;
    }

    std::vector<juce::ToggleButton*> shownToggles (juce::Component& control)
    {
        std::vector<juce::ToggleButton*> result;
        for (auto* child : control.getChildren())
            if (auto* b = dynamic_cast<juce::ToggleButton*> (child); b != nullptr && b->isVisible())
                result.push_back (b);
        return result;
    }

    juce::String litLabels (juce::Component& control)
    {
        juce::StringArray lit;
        for (auto* b : shownToggles (control))
            if (b->getToggleState())
                lit.add (b->getButtonText());
        return lit.joinIntoString (" ");
    }

    juce::Rectangle<int> boundsIn (juce::Component& root, juce::Component& c)
    {
        return root.getLocalArea (&c, c.getLocalBounds());
    }

    int polyMask (rcv::RetroChipProcessor& proc)
    {
        return juce::roundToInt (rcvtest::getRaw (proc, rcv::ParamIds::polyChannels));
    }
} // namespace

TEST_CASE ("Poly channels: one toggle per hardware channel, lit as the engine host resolves the mask", "[editor][poly]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::setRaw (*proc, rcv::ParamIds::chip, 0.0f);   // NES
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());
    REQUIRE (editor != nullptr);
    auto* control = dynamic_cast<rcv::PolyChannelsControl*> (findById (*editor, rcv::ParamIds::polyChannels));
    REQUIRE (control != nullptr);

    // Default (poly_channels 0): the chip's default mask.
    CHECK (shownToggles (*control).size() == 5);
    CHECK (litLabels (*control) == "P1 P2 Tri");

    rcvtest::setRaw (*proc, rcv::ParamIds::chip, 1.0f);   // SNES
    CHECK (shownToggles (*control).size() == 8);
    CHECK (litLabels (*control) == "1 2 3 4 5 6 7 8");

    rcvtest::setRaw (*proc, rcv::ParamIds::chip, 2.0f);   // Genesis
    CHECK (shownToggles (*control).size() == 10);
    CHECK (litLabels (*control) == "F1 F2 F3 F4 F5 F6");

    // A stored mask is kept across chips: bits 0..3 and 9.
    rcvtest::setRaw (*proc, rcv::ParamIds::polyChannels, static_cast<float> (0x20F));
    CHECK (litLabels (*control) == "F1 F2 F3 F4 N");
    rcvtest::setRaw (*proc, rcv::ParamIds::chip, 0.0f);
    CHECK (litLabels (*control) == "P1 P2 Tri Noi");

    // A mask with no channel of the current chip falls back to the chip default.
    rcvtest::setRaw (*proc, rcv::ParamIds::polyChannels, static_cast<float> (0x200));
    CHECK (litLabels (*control) == "P1 P2 Tri");

    // MIDI channel voice mode does not use the mask.
    CHECK (control->isEnabled());
    rcvtest::setRaw (*proc, rcv::ParamIds::voiceMode, static_cast<float> (rcv::VoiceMode::MidiChannel));
    CHECK_FALSE (control->isEnabled());
    rcvtest::setRaw (*proc, rcv::ParamIds::voiceMode, static_cast<float> (rcv::VoiceMode::Poly));
    CHECK (control->isEnabled());
}

TEST_CASE ("Poly channels: a click writes the mask, keeps the other chips' bits and never empties it", "[editor][poly]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::setRaw (*proc, rcv::ParamIds::chip, 0.0f);   // NES
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());
    REQUIRE (editor != nullptr);
    auto* control = dynamic_cast<rcv::PolyChannelsControl*> (findById (*editor, rcv::ParamIds::polyChannels));
    REQUIRE (control != nullptr);

    control->toggleChannel (3);   // noise on top of the default pulses + triangle
    CHECK (polyMask (*proc) == 0x0F);
    CHECK (litLabels (*control) == "P1 P2 Tri Noi");

    control->toggleChannel (3);   // back to the chip default: stored as 0 so it follows the chip
    CHECK (polyMask (*proc) == 0);

    control->toggleChannel (0);
    control->toggleChannel (1);
    CHECK (polyMask (*proc) == 0x04);   // triangle only
    control->toggleChannel (2);         // the last lit channel stays on
    CHECK (polyMask (*proc) == 0x04);
    CHECK (litLabels (*control) == "Tri");

    // Bits of channels the chip does not have are kept.
    rcvtest::setRaw (*proc, rcv::ParamIds::polyChannels, static_cast<float> (0x204));
    control->toggleChannel (4);   // DMC
    CHECK (polyMask (*proc) == 0x214);

    // Genesis: switching off FM 6 of the default mask.
    rcvtest::setRaw (*proc, rcv::ParamIds::polyChannels, 0.0f);
    rcvtest::setRaw (*proc, rcv::ParamIds::chip, 2.0f);
    control->toggleChannel (5);
    CHECK (polyMask (*proc) == 0x1F);
    control->toggleChannel (9);   // PSG noise added
    CHECK (polyMask (*proc) == 0x21F);
}

TEST_CASE ("Poly channels: the sidebar still fits and the toggles are at least 24 px", "[editor][poly]")
{
    auto proc = rcvtest::makeProcessor();
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());
    REQUIRE (editor != nullptr);
    auto* control = dynamic_cast<rcv::PolyChannelsControl*> (findById (*editor, rcv::ParamIds::polyChannels));
    REQUIRE (control != nullptr);
    REQUIRE (control->getParentComponent() != nullptr);

    for (float chip : { 0.0f, 1.0f, 2.0f })
    {
        rcvtest::setRaw (*proc, rcv::ParamIds::chip, chip);
        INFO ("chip " << chip);
        const auto outputBox = boundsIn (*editor, *control->getParentComponent());
        // Editor content is laid out in 1280 x 720 logical px and scaled as a whole.
        const float scale = static_cast<float> (editor->getWidth()) / static_cast<float> (rcv::theme::kBaseWidth);
        CHECK (static_cast<float> (outputBox.getBottom()) <= static_cast<float> (rcv::layout::sidebar().getBottom()) * scale + 0.5f);
        for (auto* b : shownToggles (*control))
        {
            CHECK (b->getWidth() >= rcv::theme::kMinTarget);
            CHECK (b->getHeight() >= rcv::theme::kMinTarget);
        }
    }
}
