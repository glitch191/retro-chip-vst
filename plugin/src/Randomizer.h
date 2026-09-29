#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "Parameters.h"

#include "chipdsp/ChipTypes.h"

#include <array>

namespace rcv
{

// Draws new values for the engine parameters of one chip (docs/PLUGIN_SPECS.md
// "Randomizer"). Message thread only: values go through the APVTS parameters.
//
// For every engine parameter of the chip that is shown on the panel and not excluded:
//   * enumerated parameters (choiceLabels) and integer 0..1 switches are re-drawn uniformly
//     with probability amount;
//   * others draw uniformly inside [max(min, v - amount * range), min(max, v + amount * range)],
//     rounded for integers, so no value ever leaves [minValue, maxValue].
// Excluded: sample slot parameters (`sample`, `*_sample`), `clock`, `chip_revision`,
// `console_filter`, `model1_lowpass` (the Genesis counterpart of console_filter: the
// console's output filter), `main_volume`. Global parameters are untouched.
class Randomizer
{
public:
    static constexpr float kDefaultAmount = 0.3f;
    static constexpr std::array<const char*, 6> kExcludedKeys = {
        "sample", "clock", "chip_revision", "console_filter", "model1_lowpass", "main_volume"
    };

    Randomizer (juce::AudioProcessorValueTreeState& apvts, const ParamRegistry& registry);

    void randomize (chipdsp::ChipId chip, float amount, juce::uint32 seed);

    static bool isExcluded (const ParamInfo& info) noexcept;

private:
    juce::AudioProcessorValueTreeState& apvts;
    const ParamRegistry& registry;
};

} // namespace rcv
