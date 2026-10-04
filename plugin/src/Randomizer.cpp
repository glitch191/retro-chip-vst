#include "Randomizer.h"

#include <cmath>
#include <random>

namespace rcv
{

Randomizer::Randomizer (juce::AudioProcessorValueTreeState& state, const ParamRegistry& reg)
    : apvts (state), registry (reg)
{
}

bool Randomizer::isExcluded (const ParamInfo& info) noexcept
{
    if (info.isGlobal())
        return true;
    const auto key = info.engineKey();
    if (key.endsWith ("_sample"))
        return true;
    for (const auto* excluded : kExcludedKeys)
        if (key == excluded)
            return true;
    return false;
}

void Randomizer::randomize (chipdsp::ChipId chip, float amount, juce::uint32 seed)
{
    amount = juce::jlimit (0.0f, 1.0f, amount);
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> unit (0.0f, 1.0f);

    for (const auto* info : registry.engineParams (chip))
    {
        if (! info->showOnPanel || isExcluded (*info))
            continue;
        auto* param = apvts.getParameter (info->id);
        auto* raw = apvts.getRawParameterValue (info->id);
        if (param == nullptr || raw == nullptr)
            continue;

        const float minValue = info->desc.minValue;
        const float maxValue = info->desc.maxValue;
        const float current = info->nativeFromRaw (raw->load (std::memory_order_relaxed));
        float next = current;

        // Integer 0..1 parameters without labels (loop, v1_echo, adsr_enable...) are switches:
        // like enumerations they flip with probability amount (the range formula would round
        // them back to their current value for every amount below 0.5).
        const bool isSwitch = info->desc.isInteger && minValue == 0.0f && maxValue == 1.0f;
        if (info->isChoice() || isSwitch)
        {
            if (unit (rng) >= amount)
                continue;
            const int lo = static_cast<int> (std::lround (minValue));
            int hi = static_cast<int> (std::lround (maxValue));
            // Genesis FM pan: Left / Center / Right only; Off (3) would leave notes silent.
            if (info->engineKey().endsWith ("_pan") && info->chip == chipdsp::ChipId::Genesis)
                hi = std::min (hi, 2);
            std::uniform_int_distribution<int> choice (lo, hi);
            next = static_cast<float> (choice (rng));
        }
        else
        {
            const float range = maxValue - minValue;
            const float lo = std::max (minValue, current - amount * range);
            const float hi = std::min (maxValue, current + amount * range);
            if (hi <= lo)
                continue;
            next = lo + (hi - lo) * unit (rng);
            if (info->desc.isInteger)
                next = static_cast<float> (std::lround (next));
        }

        next = info->clampNative (next);
        if (next == current)
            continue;
        param->setValueNotifyingHost (param->convertTo0to1 (info->rawFromNative (next)));
    }
}

} // namespace rcv
