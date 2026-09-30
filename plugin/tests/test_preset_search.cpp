// Cross-chip preset search (PresetManager::searchAll, Preset::matches) and the keyboard
// behaviour of the editor's search field (CommonStrip::SearchField).

#include "TestHelpers.h"

#include "ui/CommonStrip.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>
#include <vector>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace
{
    // Six user presets whose names all contain "zq" (absent from the factory banks), so the
    // results below do not depend on the banks. Import order is not the expected order.
    constexpr const char* kUserPresets = R"json([
      { "name": "Zq Alpha", "chip": "nes", "category": "Bass", "subcategory": "Pulse", "tags": ["warm"] },
      { "name": "Zq Beta", "chip": "snes", "category": "Pad", "subcategory": "Echo", "tags": ["zqtag"] },
      { "name": "Zq Gamma", "chip": "genesis", "category": "FM Bass", "subcategory": "Alg", "tags": [] },
      { "name": "Zq Delta", "chip": "genesis", "category": "FM Bass", "subcategory": "Alg", "tags": ["bright"] },
      { "name": "SNES Zq Epsilon", "chip": "snes", "category": "Pad", "subcategory": "Choir", "tags": [] },
      { "name": "Zq Zeta", "chip": "nes", "category": "Lead", "subcategory": "Pulse", "tags": [] }
    ])json";

    void importUserPresets (rcv::PresetManager& pm)
    {
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("rcv_search_test", ".json");
        REQUIRE (file.replaceWithText (kUserPresets));
        REQUIRE (pm.importFile (file));
        file.deleteFile();
    }

    std::vector<juce::String> names (const std::vector<const rcv::Preset*>& presets)
    {
        std::vector<juce::String> result;
        for (const auto* p : presets)
            result.push_back (p->name);
        return result;
    }

    constexpr chipdsp::ChipId kChips[] = { chipdsp::ChipId::Nes, chipdsp::ChipId::Snes, chipdsp::ChipId::Genesis };

    bool haveFactoryBanks (rcv::PresetManager& pm)
    {
        for (auto chip : kChips)
            if (pm.numPresets (chip) == 0)
                return false;
        return true;
    }

    // TextEditor reports Return and Escape through posted command messages: deliver the
    // pending ones (the tests have no running message loop).
    void deliverPendingMessages()
    {
       #if JUCE_WINDOWS
        MSG msg {};
        for (int i = 0; i < 1000 && PeekMessage (&msg, nullptr, 0, 0, PM_REMOVE) != 0; ++i)
        {
            TranslateMessage (&msg);
            DispatchMessage (&msg);
        }
       #endif
    }

    juce::Component* findById (juce::Component& root, const juce::String& id)
    {
        if (root.getComponentID() == id)
            return &root;
        for (auto* child : root.getChildren())
            if (auto* found = findById (*child, id))
                return found;
        return nullptr;
    }
} // namespace

TEST_CASE ("searchAll matches name, category, subcategory, tags and chip, every word, any case", "[presets][search]")
{
    auto proc = rcvtest::makeProcessor();
    auto& pm = proc->presetManager();
    importUserPresets (pm);

    using V = std::vector<juce::String>;

    // Order: chip (NES, SNES, Genesis), then category, subcategory, name.
    CHECK (names (pm.searchAll ("zq")) == V { "Zq Alpha", "Zq Zeta", "SNES Zq Epsilon", "Zq Beta", "Zq Delta", "Zq Gamma" });

    // A category word alone ("bass" is the category of Alpha, a substring of "FM Bass").
    CHECK (names (pm.searchAll ("zq bass")) == V { "Zq Alpha", "Zq Delta", "Zq Gamma" });
    // Case-insensitive, extra white space ignored.
    CHECK (names (pm.searchAll ("  ZQ   BaSs ")) == V { "Zq Alpha", "Zq Delta", "Zq Gamma" });
    // A name word.
    CHECK (names (pm.searchAll ("zq beta")) == V { "Zq Beta" });
    // A tag.
    CHECK (names (pm.searchAll ("zqtag")) == V { "Zq Beta" });
    // Several words across fields (category + subcategory), all required.
    CHECK (names (pm.searchAll ("zq pad echo")) == V { "Zq Beta" });
    CHECK (names (pm.searchAll ("zq pad lead")).empty());
    // A subcategory word on two chips.
    CHECK (names (pm.searchAll ("zq pulse")) == V { "Zq Alpha", "Zq Zeta" });
    CHECK (names (pm.searchAll ("zq alg")) == V { "Zq Delta", "Zq Gamma" });

    // Chip words match the chip only: "nes" is not a substring match of "SNES Zq Epsilon".
    CHECK (names (pm.searchAll ("zq nes")) == V { "Zq Alpha", "Zq Zeta" });
    CHECK (names (pm.searchAll ("snes zq")) == V { "SNES Zq Epsilon", "Zq Beta" });
    CHECK (names (pm.searchAll ("genesis zq bass")) == V { "Zq Delta", "Zq Gamma" });
    CHECK (names (pm.searchAll ("GEN zq")) == V { "Zq Delta", "Zq Gamma" });
    CHECK (names (pm.searchAll ("nes snes zq")).empty());

    CHECK (pm.searchAll ("zq nomatch").empty());

    // The results become the Previous/Next list.
    const auto found = pm.searchAll ("zq bass");
    CHECK (pm.filtered() == found);

    // The per-chip search keeps its API and the same rule.
    const auto snesOnly = pm.search (chipdsp::ChipId::Snes, "zq");
    CHECK (snesOnly.size() == 2);
    for (const auto* p : snesOnly)
        CHECK (p->chip == chipdsp::ChipId::Snes);
    CHECK (pm.search (chipdsp::ChipId::Snes, "zq nes").empty());
}

TEST_CASE ("searchAll over the factory banks returns every chip for a category word", "[presets][search]")
{
    auto proc = rcvtest::makeProcessor();
    auto& pm = proc->presetManager();
    if (! haveFactoryBanks (pm))
        SKIP ("factory banks not embedded in this build");

    const auto bass = pm.searchAll ("bass");
    std::set<int> chips;
    int previousChip = -1;
    for (const auto* p : bass)
    {
        chips.insert (static_cast<int> (p->chip));
        CHECK (static_cast<int> (p->chip) >= previousChip);   // grouped and ordered by chip
        previousChip = static_cast<int> (p->chip);
        bool inField = p->name.containsIgnoreCase ("bass") || p->category.containsIgnoreCase ("bass")
                    || p->subcategory.containsIgnoreCase ("bass");
        for (const auto& tag : p->tags)
            inField = inField || tag.containsIgnoreCase ("bass");
        CHECK (inField);
    }
    CHECK (chips.size() == 3);

    const auto genesisBass = pm.searchAll ("genesis bass");
    REQUIRE (! genesisBass.empty());
    for (const auto* p : genesisBass)
        CHECK (p->chip == chipdsp::ChipId::Genesis);
    CHECK (genesisBass.size() < bass.size());

    // "nes" names the NES chip: no SNES preset although every SNES name contains "nes".
    for (const auto* p : pm.searchAll ("nes"))
        CHECK (p->chip == chipdsp::ChipId::Nes);
}

TEST_CASE ("Applying a search result of another chip switches the chip; Next walks across chips", "[presets][search]")
{
    auto proc = rcvtest::makeProcessor();
    auto& pm = proc->presetManager();
    importUserPresets (pm);

    rcvtest::setRaw (*proc, rcv::ParamIds::chip, 0.0f);   // NES
    REQUIRE (proc->selectedChip() == chipdsp::ChipId::Nes);

    const auto found = pm.searchAll ("zq bass");
    REQUIRE (found.size() == 3);
    REQUIRE (found[1]->chip == chipdsp::ChipId::Genesis);
    pm.apply (*found[1]);
    CHECK (proc->selectedChip() == chipdsp::ChipId::Genesis);
    CHECK (rcvtest::getRaw (*proc, rcv::ParamIds::chip) == 2.0f);
    CHECK_FALSE (pm.isApplying());

    // The cross-chip results are the Previous/Next list.
    pm.apply (*found[0]);   // Zq Alpha, NES
    CHECK (proc->selectedChip() == chipdsp::ChipId::Nes);
    pm.next();
    CHECK (pm.currentName() == "Zq Delta");
    CHECK (proc->selectedChip() == chipdsp::ChipId::Genesis);
    pm.previous();
    CHECK (pm.currentName() == "Zq Alpha");
    CHECK (proc->selectedChip() == chipdsp::ChipId::Nes);
}

TEST_CASE ("The search field consumes every key; Return and Escape release the keyboard", "[editor][search][keyboard]")
{
    auto proc = rcvtest::makeProcessor();
    importUserPresets (proc->presetManager());
    rcvtest::setRaw (*proc, rcv::ParamIds::chip, 0.0f);   // NES

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());
    REQUIRE (editor != nullptr);
    auto* field = dynamic_cast<rcv::CommonStrip::SearchField*> (findById (*editor, "presetSearch"));
    REQUIRE (field != nullptr);
    auto* strip = dynamic_cast<rcv::CommonStrip*> (field->getParentComponent());
    REQUIRE (strip != nullptr);

    SECTION ("keys are reported as handled, host shortcuts pass")
    {
        using K = juce::KeyPress;
        const juce::ModifierKeys none;
        for (const auto& key : { K ('a', none, 'a'), K ('z', juce::ModifierKeys::shiftModifier, 'Z'), K ('4', none, '4'),
                                 K (K::spaceKey, none, ' '), K (K::backspaceKey), K (K::deleteKey), K (K::leftKey), K (K::rightKey),
                                 K (K::upKey), K (K::downKey), K (K::homeKey), K (K::endKey), K (K::tabKey), K (K::F5Key),
                                 K (K::returnKey), K (K::escapeKey) })
        {
            INFO ("key " << key.getTextDescription());
            CHECK (field->keyPressed (key));
        }
        CHECK (field->keyStateChanged (true));
        CHECK (field->keyStateChanged (false));
        CHECK_FALSE (field->keyPressed (K ('s', juce::ModifierKeys::ctrlModifier, 0)));   // e.g. the host's Save
    }

    SECTION ("Down + Return applies a result of another chip, then nothing holds the focus")
    {
        editor->addToDesktop (0);
        editor->setVisible (true);
        field->grabKeyboardFocus();
        const bool focused = field->hasKeyboardFocus (false);
        if (! focused)
            WARN ("no keyboard focus available in this session: focus checks skipped");

        strip->setSearchText ("zq genesis");
        CHECK (field->keyPressed (juce::KeyPress (juce::KeyPress::downKey)));   // first result: Zq Delta
        CHECK (field->keyPressed (juce::KeyPress (juce::KeyPress::returnKey)));
        deliverPendingMessages();
        CHECK (proc->presetManager().currentName() == "Zq Delta");
        CHECK (proc->selectedChip() == chipdsp::ChipId::Genesis);
        CHECK (field->getText() == "zq genesis");   // the search stays after the chip switch
        CHECK (juce::Component::getCurrentlyFocusedComponent() == nullptr);

        if (focused)
        {
            field->grabKeyboardFocus();
            CHECK (field->hasKeyboardFocus (false));
        }
        CHECK (field->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
        deliverPendingMessages();
        CHECK (field->getText().isEmpty());
        CHECK (juce::Component::getCurrentlyFocusedComponent() == nullptr);
    }
}
