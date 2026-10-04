// Randomizer: 1000 seeds per chip never leave the ParamDesc bounds and never touch the
// excluded parameters, the globals or the other chips' parameters.

#include "TestHelpers.h"

#include "chipdsp/EngineFactory.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

namespace
{
    constexpr chipdsp::ChipId kChips[] = { chipdsp::ChipId::Nes, chipdsp::ChipId::Snes, chipdsp::ChipId::Genesis };

    std::vector<float> snapshot (rcv::RetroChipProcessor& proc)
    {
        std::vector<float> values;
        for (const auto& info : proc.paramRegistry().all())
            values.push_back (rcvtest::getRaw (proc, info.id));
        return values;
    }
} // namespace

TEST_CASE ("Randomizer exclusion list", "[randomizer]")
{
    auto proc = rcvtest::makeProcessor();
    for (const auto& info : proc->paramRegistry().all())
    {
        INFO ("parameter " << info.id);
        const auto key = info.engineKey();
        const bool expectedExcluded = info.isGlobal() || key == "sample" || key.endsWith ("_sample") || key == "clock"
                                   || key == "chip_revision" || key == "console_filter" || key == "model1_lowpass"
                                   || key == "main_volume" || key == "frame_mode";
        CHECK (rcv::Randomizer::isExcluded (info) == expectedExcluded);
    }
    // The preset's level correction is a global: never randomized.
    const auto* presetGain = proc->paramRegistry().find (rcv::ParamIds::presetGain);
    REQUIRE (presetGain != nullptr);
    CHECK (rcv::Randomizer::isExcluded (*presetGain));
}

TEST_CASE ("Randomizer leaves preset_gain alone", "[randomizer]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::setRaw (*proc, rcv::ParamIds::presetGain, 12.5f);
    for (auto chip : kChips)
        for (juce::uint32 seed = 1; seed <= 20; ++seed)
            proc->randomizer().randomize (chip, 1.0f, seed);
    CHECK (rcvtest::getRaw (*proc, rcv::ParamIds::presetGain) == 12.5f);
}

TEST_CASE ("Randomizer stays inside ParamDesc bounds and never touches excluded parameters", "[randomizer]")
{
    auto proc = rcvtest::makeProcessor();
    const auto& all = proc->paramRegistry().all();

    for (auto chip : kChips)
    {
        INFO ("chip " << chipdsp::chipKey (chip));
        int changedDraws = 0;
        auto before = snapshot (*proc);

        for (juce::uint32 seed = 1; seed <= 1000; ++seed)
        {
            // Amounts 0.0, 0.1 ... 1.0 in turn.
            const float amount = static_cast<float> (seed % 11u) / 10.0f;
            proc->randomizer().randomize (chip, amount, seed);
            const auto after = snapshot (*proc);

            bool anyChange = false;
            for (size_t i = 0; i < all.size(); ++i)
            {
                const auto& info = all[i];
                const bool randomizable = info.chip == chip && info.showOnPanel && ! rcv::Randomizer::isExcluded (info);
                if (! randomizable)
                {
                    if (after[i] != before[i])
                    {
                        FAIL_CHECK ("seed " << seed << " changed " << info.id << " from " << before[i] << " to " << after[i]);
                    }
                    continue;
                }

                const float native = info.nativeFromRaw (after[i]);
                if (native < info.desc.minValue || native > info.desc.maxValue)
                    FAIL_CHECK ("seed " << seed << ": " << info.id << " = " << native << " outside ["
                                        << info.desc.minValue << ", " << info.desc.maxValue << "]");
                if (info.desc.isInteger && native != std::round (native))
                    FAIL_CHECK ("seed " << seed << ": integer " << info.id << " = " << native);
                if (amount == 0.0f && after[i] != before[i])
                    FAIL_CHECK ("seed " << seed << ": amount 0 changed " << info.id);
                if (chip == chipdsp::ChipId::Genesis && info.engineKey().endsWith ("_pan") && native == 3.0f)
                    FAIL_CHECK ("seed " << seed << ": " << info.id << " randomized to Off");
                anyChange = anyChange || after[i] != before[i];
            }
            if (anyChange)
                ++changedDraws;
            before = after;
        }

        // With amount > 0 (909 of the 1000 seeds) the randomizer must actually move something.
        CHECK (changedDraws > 800);
    }
}

TEST_CASE ("Randomizer is deterministic for a seed", "[randomizer]")
{
    auto a = rcvtest::makeProcessor();
    auto b = rcvtest::makeProcessor();
    a->randomizer().randomize (chipdsp::ChipId::Genesis, 0.7f, 1234u);
    b->randomizer().randomize (chipdsp::ChipId::Genesis, 0.7f, 1234u);
    CHECK (snapshot (*a) == snapshot (*b));
}
