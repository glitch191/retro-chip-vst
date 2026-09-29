// Parameter layout and ParamInfo tests. Expected values for the globals are copied by hand
// from the table in docs/PLUGIN_SPECS.md ("Global parameters"); engine parameters are
// checked against each engine's own ParamDesc table.

#include "TestHelpers.h"

#include "chipdsp/EngineFactory.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>

using Catch::Matchers::WithinAbs;
using rcv::ParamKind;

namespace
{
    constexpr chipdsp::ChipId kChips[] = { chipdsp::ChipId::Nes, chipdsp::ChipId::Snes, chipdsp::ChipId::Genesis };

    struct ExpectedGlobal
    {
        const char* id;
        ParamKind kind;
        float minValue;
        float maxValue;
        float defaultValue;
        int numChoices;   // choice parameters only
    };

    // docs/PLUGIN_SPECS.md, "Global parameters".
    const ExpectedGlobal kExpectedGlobals[] = {
        { "chip",              ParamKind::Choice, 0.0f,   2.0f,    0.0f,  3 },
        { "raw_output",        ParamKind::Bool,   0.0f,   1.0f,    0.0f,  0 },
        { "voice_mode",        ParamKind::Choice, 0.0f,   1.0f,    1.0f,  2 },   // default Poly
        { "poly_channels",     ParamKind::Int,    0.0f,   1023.0f, 0.0f,  0 },   // 0 = the chip's default mask
        { "master_gain",       ParamKind::Float,  -24.0f, 12.0f,   0.0f,  0 },
        { "arp_enabled",       ParamKind::Bool,   0.0f,   1.0f,    0.0f,  0 },
        { "arp_pattern",       ParamKind::Choice, 0.0f,   4.0f,    0.0f,  5 },
        { "arp_octaves",       ParamKind::Int,    1.0f,   4.0f,    1.0f,  0 },
        { "arp_rate_mode",     ParamKind::Choice, 0.0f,   1.0f,    0.0f,  2 },
        { "arp_sync_division", ParamKind::Choice, 0.0f,   5.0f,    3.0f,  6 },   // default 1/16
        { "arp_free_rate",     ParamKind::Float,  0.5f,   50.0f,   8.0f,  0 },
        { "arp_gate",          ParamKind::Float,  5.0f,   100.0f,  50.0f, 0 },
        { "arp_hold",          ParamKind::Bool,   0.0f,   1.0f,    0.0f,  0 },
        { "glide_time",        ParamKind::Float,  0.0f,   2000.0f, 0.0f,  0 },
        { "glide_mode",        ParamKind::Choice, 0.0f,   1.0f,    0.0f,  2 },
        { "ui_scale",          ParamKind::Float,  1.0f,   2.0f,    1.0f,  0 },
    };

    // Checks that the host parameter object has the flavour, range and default of the descriptor.
    void checkHostParameter (juce::RangedAudioParameter* param, ParamKind kind, float minValue, float maxValue,
                             float defaultValue, const char* const* labels)
    {
        REQUIRE (param != nullptr);
        switch (kind)
        {
            case ParamKind::Float:
            {
                auto* p = dynamic_cast<juce::AudioParameterFloat*> (param);
                REQUIRE (p != nullptr);
                CHECK (p->range.start == minValue);
                CHECK (p->range.end == maxValue);
                CHECK_THAT (p->get(), WithinAbs (defaultValue, 1.0e-4));
                break;
            }
            case ParamKind::Int:
            {
                auto* p = dynamic_cast<juce::AudioParameterInt*> (param);
                REQUIRE (p != nullptr);
                CHECK (p->getRange().getStart() == static_cast<int> (minValue));
                CHECK (p->getRange().getEnd() == static_cast<int> (maxValue));
                CHECK (p->get() == static_cast<int> (defaultValue));
                break;
            }
            case ParamKind::Choice:
            {
                auto* p = dynamic_cast<juce::AudioParameterChoice*> (param);
                REQUIRE (p != nullptr);
                const int count = static_cast<int> (std::lround (maxValue - minValue)) + 1;
                REQUIRE (p->choices.size() == count);
                CHECK (p->getIndex() == static_cast<int> (std::lround (defaultValue - minValue)));
                if (labels != nullptr)
                    for (int i = 0; i < count; ++i)
                        CHECK (p->choices[i] == juce::String (labels[i]));
                break;
            }
            case ParamKind::Bool:
            {
                auto* p = dynamic_cast<juce::AudioParameterBool*> (param);
                REQUIRE (p != nullptr);
                CHECK (p->get() == (defaultValue > 0.5f));
                break;
            }
        }
    }
} // namespace

TEST_CASE ("Parameter layout contains every engine key with the chip prefix and every global", "[parameters]")
{
    auto proc = rcvtest::makeProcessor();
    auto& apvts = proc->parameters();
    const auto& registry = proc->paramRegistry();

    size_t expectedCount = rcv::ParamIds::all.size();

    for (const auto* id : rcv::ParamIds::all)
    {
        INFO ("global " << id);
        CHECK (apvts.getParameter (id) != nullptr);
        const auto* info = registry.find (id);
        REQUIRE (info != nullptr);
        CHECK (info->isGlobal());
        CHECK (info->engineParamId == -1);
    }

    for (auto chip : kChips)
    {
        const auto& engine = proc->engineHost().engine (chip);
        const auto descs = engine.parameterDescriptors();
        expectedCount += descs.size();
        CHECK (registry.engineParams (chip).size() == descs.size());

        for (const auto& d : descs)
        {
            const juce::String expectedId = juce::String (chipdsp::chipKey (chip)) + "_" + d.key;
            INFO ("engine parameter " << expectedId);
            CHECK (rcv::ParamRegistry::engineParamId (chip, d.key) == expectedId);

            auto* param = apvts.getParameter (expectedId);
            REQUIRE (param != nullptr);
            CHECK (param->getName (100) == juce::String (chipdsp::chipName (chip)) + " " + d.name);

            const auto* info = registry.find (expectedId);
            REQUIRE (info != nullptr);
            CHECK (info->chip == chip);
            CHECK (info->engineParamId == d.id);
            CHECK (info->engineKey() == juce::String (d.key));
            CHECK (registry.find (chip, d.id) == info);
            CHECK (registry.findByKey (chip, d.key) == info);
            REQUIRE (info->index >= 0);
            REQUIRE (static_cast<size_t> (info->index) < registry.all().size());
            CHECK (&registry.all()[static_cast<size_t> (info->index)] == info);
        }
    }

    CHECK (registry.all().size() == expectedCount);
    CHECK (static_cast<size_t> (proc->getParameters().size()) == expectedCount);
    CHECK (registry.globalParams().size() == rcv::ParamIds::all.size());
    CHECK (apvts.getParameter ("nes_does_not_exist") == nullptr);
    CHECK (registry.find (juce::String ("nes_does_not_exist")) == nullptr);
}

TEST_CASE ("ParamInfo ranges equal the engine ParamDesc and the host parameters match", "[parameters]")
{
    auto proc = rcvtest::makeProcessor();
    auto& apvts = proc->parameters();
    const auto& registry = proc->paramRegistry();

    for (auto chip : kChips)
    {
        for (const auto& d : proc->engineHost().engine (chip).parameterDescriptors())
        {
            const auto* info = registry.findByKey (chip, d.key);
            INFO ("engine parameter " << chipdsp::chipKey (chip) << "_" << d.key);
            REQUIRE (info != nullptr);
            CHECK (info->desc.id == d.id);
            CHECK (info->desc.minValue == d.minValue);
            CHECK (info->desc.maxValue == d.maxValue);
            CHECK (info->desc.defaultValue == d.defaultValue);
            CHECK (info->desc.isInteger == d.isInteger);
            CHECK (info->desc.choiceLabels == d.choiceLabels);
            CHECK (info->group == juce::String (d.group != nullptr ? d.group : "General"));

            const ParamKind expectedKind = ! d.isInteger ? ParamKind::Float
                                         : (d.choiceLabels != nullptr ? ParamKind::Choice : ParamKind::Int);
            CHECK (info->kind == expectedKind);
            checkHostParameter (apvts.getParameter (info->id), expectedKind, d.minValue, d.maxValue, d.defaultValue, d.choiceLabels);

            // The APVTS raw value starts at the default, expressed in raw units.
            CHECK_THAT (rcvtest::getRaw (*proc, info->id), WithinAbs (info->rawFromNative (d.defaultValue), 1.0e-4));
        }
    }

    for (const auto& g : kExpectedGlobals)
    {
        INFO ("global " << g.id);
        const auto* info = registry.find (g.id);
        REQUIRE (info != nullptr);
        CHECK (info->kind == g.kind);
        CHECK (info->desc.minValue == g.minValue);
        CHECK (info->desc.maxValue == g.maxValue);
        CHECK (info->desc.defaultValue == g.defaultValue);
        checkHostParameter (apvts.getParameter (g.id), g.kind, g.minValue, g.maxValue, g.defaultValue, nullptr);
        if (g.kind == ParamKind::Choice)
            CHECK (dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (g.id))->choices.size() == g.numChoices);
    }

    // ui_scale is saved with the state but is not automatable; everything else is.
    CHECK_FALSE (apvts.getParameter (rcv::ParamIds::uiScale)->isAutomatable());
    CHECK (apvts.getParameter (rcv::ParamIds::masterGain)->isAutomatable());

    // Labels of the chip selector, in ChipId order.
    auto* chipParam = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (rcv::ParamIds::chip));
    REQUIRE (chipParam != nullptr);
    CHECK (chipParam->choices == juce::StringArray ("NES", "SNES", "Genesis"));
}

TEST_CASE ("ParamInfo raw/native conversion and clamping", "[parameters]")
{
    static const char* const labels[] = { "A", "B", "C", "D" };

    rcv::ParamInfo choice;
    choice.kind = ParamKind::Choice;
    choice.desc = { 7, "mode", "Mode", "Test", 2.0f, 5.0f, 3.0f, true, "", labels };
    CHECK (choice.nativeFromRaw (0.0f) == 2.0f);   // index 0 -> minValue
    CHECK (choice.nativeFromRaw (3.0f) == 5.0f);
    CHECK (choice.rawFromNative (4.0f) == 2.0f);
    CHECK (choice.clampNative (9.0f) == 5.0f);
    CHECK (choice.clampNative (-1.0f) == 2.0f);
    CHECK (choice.clampNative (3.4f) == 3.0f);
    CHECK (choice.isChoice());

    rcv::ParamInfo integer;
    integer.kind = ParamKind::Int;
    integer.desc = { 1, "volume", "Volume", "Test", 0.0f, 15.0f, 12.0f, true, "", nullptr };
    CHECK (integer.nativeFromRaw (7.0f) == 7.0f);  // no offset outside choices
    CHECK (integer.rawFromNative (7.0f) == 7.0f);
    CHECK (integer.clampNative (3.6f) == 4.0f);
    CHECK (integer.clampNative (16.0f) == 15.0f);
    CHECK (integer.clampNative (std::numeric_limits<float>::quiet_NaN()) == 12.0f);

    rcv::ParamInfo continuous;
    continuous.kind = ParamKind::Float;
    continuous.desc = { 2, "time", "Time", "Test", -1.0f, 1.0f, 0.25f, false, "ms", nullptr };
    CHECK (continuous.clampNative (0.3f) == 0.3f);
    CHECK (continuous.clampNative (5.0f) == 1.0f);
    CHECK (continuous.clampNative (-5.0f) == -1.0f);
    CHECK (continuous.clampNative (std::numeric_limits<float>::quiet_NaN()) == 0.25f);
    CHECK (continuous.isGlobal());   // no chip set
}

TEST_CASE ("Channel masks follow the chip channel counts", "[parameters]")
{
    using R = rcv::ParamRegistry;
    CHECK (R::chipChannelMask (chipdsp::ChipId::Nes) == 0x1Fu);
    CHECK (R::chipChannelMask (chipdsp::ChipId::Snes) == 0xFFu);
    CHECK (R::chipChannelMask (chipdsp::ChipId::Genesis) == 0x3FFu);
    CHECK (R::defaultPolyMask (chipdsp::ChipId::Nes) == 0x07u);
    CHECK (R::defaultPolyMask (chipdsp::ChipId::Snes) == 0xFFu);
    CHECK (R::defaultPolyMask (chipdsp::ChipId::Genesis) == 0x3Fu);
}

TEST_CASE ("Processor basics: buses, programs, tail and MIDI flags", "[processor]")
{
    auto proc = rcvtest::makeProcessor();

    CHECK (proc->getNumPrograms() == 1);
    CHECK (proc->getTailLengthSeconds() == 0.0);
    CHECK (proc->acceptsMidi());
    CHECK_FALSE (proc->producesMidi());
    CHECK_FALSE (proc->isMidiEffect());

    CHECK (proc->getBusCount (true) == 0);
    REQUIRE (proc->getBusCount (false) == 1 + rcv::RetroChipProcessor::kNumChannelBuses);
    CHECK (proc->getBus (false, 0)->getName() == "Main");
    CHECK (proc->getBus (false, 0)->isEnabled());
    CHECK (proc->getBus (false, 0)->getCurrentLayout() == juce::AudioChannelSet::stereo());
    for (int b = 1; b <= rcv::RetroChipProcessor::kNumChannelBuses; ++b)
    {
        INFO ("bus " << b);
        CHECK (rcv::RetroChipProcessor::channelBusName (b) == "Out " + juce::String (b));
        CHECK (proc->getBus (false, b)->getName() == rcv::RetroChipProcessor::channelBusName (b));
        CHECK_FALSE (proc->getBus (false, b)->isEnabled());
    }
    CHECK (proc->getTotalNumOutputChannels() == 2);

    // Main must be stereo; channel buses are stereo or disabled.
    auto layout = proc->getBusesLayout();
    CHECK (proc->checkBusesLayoutSupported (layout));
    layout.outputBuses.getReference (3) = juce::AudioChannelSet::stereo();
    CHECK (proc->checkBusesLayoutSupported (layout));
    layout.outputBuses.getReference (3) = juce::AudioChannelSet::mono();
    CHECK_FALSE (proc->checkBusesLayoutSupported (layout));
    layout.outputBuses.getReference (3) = juce::AudioChannelSet::disabled();
    layout.outputBuses.getReference (0) = juce::AudioChannelSet::mono();
    CHECK_FALSE (proc->checkBusesLayoutSupported (layout));

    rcvtest::enableChannelBuses (*proc, 5);
    CHECK (proc->getTotalNumOutputChannels() == 12);
}
