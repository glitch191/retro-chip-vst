#include "Parameters.h"

#include "chipdsp/EngineFactory.h"

#include <cmath>

namespace rcv
{

namespace
{
    const char* const kChipLabels[] = { "NES", "SNES", "Genesis" };
    const char* const kVoiceModeLabels[] = { "MIDI channel", "Poly" };
    const char* const kArpPatternLabels[] = { "Up", "Down", "Up-Down", "As played", "Random" };
    const char* const kArpRateModeLabels[] = { "Sync", "Free" };
    const char* const kArpDivisionLabels[] = { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
    const char* const kGlideModeLabels[] = { "Always", "Legato only" };

    struct GlobalDef
    {
        const char* id;
        const char* name;
        const char* group;
        ParamKind kind;
        float minValue;
        float maxValue;
        float defaultValue;
        const char* unit;
        const char* const* labels;
        bool automatable;
        bool showOnPanel;
    };

    // Table from docs/PLUGIN_SPECS.md ("Global parameters"). poly_channels defaults to 0,
    // which the engine host reads as "the chip's default mask" (ParamRegistry::defaultPolyMask),
    // so the default follows the chip on the audio thread without the plugin rewriting the
    // parameter.
    const GlobalDef kGlobals[] = {
        { ParamIds::chip,            "Chip",              "Global",      ParamKind::Choice, 0.0f,   2.0f,    0.0f,   "",   kChipLabels,        true,  true },
        { ParamIds::rawOutput,       "Raw output",        "Global",      ParamKind::Bool,   0.0f,   1.0f,    0.0f,   "",   nullptr,            true,  true },
        { ParamIds::voiceMode,       "Voice mode",        "Global",      ParamKind::Choice, 0.0f,   1.0f,    1.0f,   "",   kVoiceModeLabels,   true,  true },
        { ParamIds::polyChannels,    "Poly channels",     "Global",      ParamKind::Int,    0.0f,   1023.0f, 0.0f,   "",   nullptr,            true,  false },
        { ParamIds::masterGain,      "Master gain",       "Global",      ParamKind::Float,  -24.0f, 12.0f,   0.0f,   "dB", nullptr,            true,  true },
        { ParamIds::arpEnabled,      "Arp enabled",       "Arpeggiator", ParamKind::Bool,   0.0f,   1.0f,    0.0f,   "",   nullptr,            true,  true },
        { ParamIds::arpPattern,      "Arp pattern",       "Arpeggiator", ParamKind::Choice, 0.0f,   4.0f,    0.0f,   "",   kArpPatternLabels,  true,  true },
        { ParamIds::arpOctaves,      "Arp octaves",       "Arpeggiator", ParamKind::Int,    1.0f,   4.0f,    1.0f,   "",   nullptr,            true,  true },
        { ParamIds::arpRateMode,     "Arp rate mode",     "Arpeggiator", ParamKind::Choice, 0.0f,   1.0f,    0.0f,   "",   kArpRateModeLabels, true,  true },
        { ParamIds::arpSyncDivision, "Arp sync division", "Arpeggiator", ParamKind::Choice, 0.0f,   5.0f,    3.0f,   "",   kArpDivisionLabels, true,  true },
        { ParamIds::arpFreeRate,     "Arp free rate",     "Arpeggiator", ParamKind::Float,  0.5f,   50.0f,   8.0f,   "Hz", nullptr,            true,  true },
        { ParamIds::arpGate,         "Arp gate",          "Arpeggiator", ParamKind::Float,  5.0f,   100.0f,  50.0f,  "%",  nullptr,            true,  true },
        { ParamIds::arpHold,         "Arp hold",          "Arpeggiator", ParamKind::Bool,   0.0f,   1.0f,    0.0f,   "",   nullptr,            true,  true },
        { ParamIds::glideTime,       "Glide time",        "Glide",       ParamKind::Float,  0.0f,   2000.0f, 0.0f,   "ms", nullptr,            true,  true },
        { ParamIds::glideMode,       "Glide mode",        "Glide",       ParamKind::Choice, 0.0f,   1.0f,    0.0f,   "",   kGlideModeLabels,   true,  true },
        { ParamIds::uiScale,         "UI scale",          "UI",          ParamKind::Float,  1.0f,   2.0f,    1.0f,   "x",  nullptr,            false, false },
    };

    int choiceCount (const chipdsp::ParamDesc& d) noexcept
    {
        return static_cast<int> (std::lround (d.maxValue - d.minValue)) + 1;
    }

    juce::String makeGroupId (const juce::String& prefix, const juce::String& text)
    {
        juce::String id (prefix);
        id << "_";
        for (auto c : text)
            id << (juce::CharacterFunctions::isLetterOrDigit (c) ? juce::String::charToString (juce::CharacterFunctions::toLowerCase (c))
                                                                  : juce::String ("_"));
        return id;
    }
} // namespace

// ----- ParamInfo ----------------------------------------------------------------------------

float ParamInfo::nativeFromRaw (float raw) const noexcept
{
    return kind == ParamKind::Choice ? raw + desc.minValue : raw;
}

float ParamInfo::rawFromNative (float native) const noexcept
{
    return kind == ParamKind::Choice ? native - desc.minValue : native;
}

float ParamInfo::clampNative (float native) const noexcept
{
    if (std::isnan (native))
        return desc.defaultValue;
    const float v = juce::jlimit (desc.minValue, desc.maxValue, native);
    return desc.isInteger ? static_cast<float> (std::lround (v)) : v;
}

// ----- ParamRegistry ------------------------------------------------------------------------

ParamRegistry::ParamRegistry (const std::array<const chipdsp::IChipEngine*, 3>& engines)
{
    addGlobals();
    for (int c = 0; c < kNumChips; ++c)
        if (engines[static_cast<size_t> (c)] != nullptr)
            addEngine (static_cast<chipdsp::ChipId> (c), *engines[static_cast<size_t> (c)]);
}

void ParamRegistry::addGlobals()
{
    int id = 0;
    for (const auto& g : kGlobals)
    {
        ParamInfo info;
        info.id = g.id;
        info.name = g.name;
        info.group = g.group;
        info.chip = std::nullopt;
        info.engineParamId = -1;
        info.desc = { id++, g.id, g.name, g.group, g.minValue, g.maxValue, g.defaultValue,
                      g.kind != ParamKind::Float, g.unit, g.labels };
        info.kind = g.kind;
        info.showOnPanel = g.showOnPanel;
        info.index = static_cast<int> (infos.size());
        byId[info.id] = info.index;
        infos.push_back (info);
    }
}

void ParamRegistry::addEngine (chipdsp::ChipId chip, const chipdsp::IChipEngine& engine)
{
    const juce::String chipName (chipdsp::chipName (chip));
    for (const auto& d : engine.parameterDescriptors())
    {
        if (d.key == nullptr || ! (d.maxValue > d.minValue))
        {
            jassertfalse; // a descriptor without a key or with an empty range cannot become a host parameter
            continue;
        }

        ParamInfo info;
        info.id = engineParamId (chip, d.key);
        if (byId.count (info.id) != 0)
        {
            jassertfalse; // duplicate key in an engine's descriptor table
            continue;
        }
        info.name = chipName + " " + juce::String (d.name != nullptr ? d.name : d.key);
        info.group = d.group != nullptr ? juce::String (d.group) : juce::String ("General");
        info.chip = chip;
        info.engineParamId = d.id;
        info.desc = d;
        info.kind = ! d.isInteger ? ParamKind::Float : (d.choiceLabels != nullptr ? ParamKind::Choice : ParamKind::Int);
        info.showOnPanel = true;
        info.index = static_cast<int> (infos.size());
        byId[info.id] = info.index;
        byEngineId[static_cast<size_t> (chip)][d.id] = info.index;
        infos.push_back (info);
    }
}

std::unique_ptr<juce::RangedAudioParameter> ParamRegistry::makeParameter (const ParamInfo& info)
{
    const juce::ParameterID pid (info.id, kParameterVersionHint);
    const juce::String unit (info.desc.unit != nullptr ? info.desc.unit : "");
    const bool automatable = info.id != juce::String (ParamIds::uiScale);

    switch (info.kind)
    {
        case ParamKind::Choice:
        {
            juce::StringArray choices;
            const int count = choiceCount (info.desc);
            for (int i = 0; i < count; ++i)
                choices.add (info.desc.choiceLabels[i]);
            const int defaultIndex = juce::jlimit (0, count - 1, static_cast<int> (std::lround (info.desc.defaultValue - info.desc.minValue)));
            return std::make_unique<juce::AudioParameterChoice> (pid, info.name, choices, defaultIndex,
                                                                 juce::AudioParameterChoiceAttributes().withLabel (unit).withAutomatable (automatable));
        }
        case ParamKind::Int:
        {
            const int lo = static_cast<int> (std::lround (info.desc.minValue));
            const int hi = static_cast<int> (std::lround (info.desc.maxValue));
            const int def = juce::jlimit (lo, hi, static_cast<int> (std::lround (info.desc.defaultValue)));
            return std::make_unique<juce::AudioParameterInt> (pid, info.name, lo, hi, def,
                                                              juce::AudioParameterIntAttributes().withLabel (unit).withAutomatable (automatable));
        }
        case ParamKind::Bool:
            return std::make_unique<juce::AudioParameterBool> (pid, info.name, info.desc.defaultValue > 0.5f,
                                                               juce::AudioParameterBoolAttributes().withLabel (unit).withAutomatable (automatable));
        case ParamKind::Float:
        default:
            return std::make_unique<juce::AudioParameterFloat> (pid, info.name,
                                                                juce::NormalisableRange<float> (info.desc.minValue, info.desc.maxValue),
                                                                juce::jlimit (info.desc.minValue, info.desc.maxValue, info.desc.defaultValue),
                                                                juce::AudioParameterFloatAttributes().withLabel (unit).withAutomatable (automatable));
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout ParamRegistry::createParameterLayout() const
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // Globals: one group per UI group ("Global", "Arpeggiator", "Glide", "UI").
    {
        std::vector<std::unique_ptr<juce::AudioProcessorParameterGroup>> groups;
        for (const auto& info : infos)
        {
            if (! info.isGlobal())
                continue;
            juce::AudioProcessorParameterGroup* group = nullptr;
            for (auto& g : groups)
                if (g->getName() == info.group)
                    group = g.get();
            if (group == nullptr)
            {
                groups.push_back (std::make_unique<juce::AudioProcessorParameterGroup> (makeGroupId ("global", info.group), info.group, " | "));
                group = groups.back().get();
            }
            group->addChild (makeParameter (info));
        }
        for (auto& g : groups)
            layout.add (std::move (g));
    }

    // One group per chip, sub-groups per descriptor group in order of first appearance.
    for (int c = 0; c < kNumChips; ++c)
    {
        const auto chip = static_cast<chipdsp::ChipId> (c);
        auto chipGroup = std::make_unique<juce::AudioProcessorParameterGroup> (juce::String (chipdsp::chipKey (chip)), juce::String (chipdsp::chipName (chip)), " | ");
        std::vector<juce::AudioProcessorParameterGroup*> subGroups;

        for (const auto& info : infos)
        {
            if (! info.chip.has_value() || *info.chip != chip)
                continue;
            juce::AudioProcessorParameterGroup* sub = nullptr;
            for (auto* g : subGroups)
                if (g->getName() == info.group)
                    sub = g;
            if (sub == nullptr)
            {
                auto created = std::make_unique<juce::AudioProcessorParameterGroup> (makeGroupId (chipdsp::chipKey (chip), info.group), info.group, " | ");
                sub = created.get();
                subGroups.push_back (sub);
                chipGroup->addChild (std::move (created));
            }
            sub->addChild (makeParameter (info));
        }

        if (! subGroups.empty())
            layout.add (std::move (chipGroup));
    }

    return layout;
}

const ParamInfo* ParamRegistry::find (const juce::String& id) const noexcept
{
    const auto it = byId.find (id);
    return it == byId.end() ? nullptr : &infos[static_cast<size_t> (it->second)];
}

const ParamInfo* ParamRegistry::find (chipdsp::ChipId chip, int engineParamId) const noexcept
{
    const auto& table = byEngineId[static_cast<size_t> (chip)];
    const auto it = table.find (engineParamId);
    return it == table.end() ? nullptr : &infos[static_cast<size_t> (it->second)];
}

const ParamInfo* ParamRegistry::findByKey (chipdsp::ChipId chip, const juce::String& engineKey) const noexcept
{
    return find (juce::String (chipdsp::chipKey (chip)) + "_" + engineKey);
}

std::vector<const ParamInfo*> ParamRegistry::engineParams (chipdsp::ChipId chip) const
{
    std::vector<const ParamInfo*> result;
    for (const auto& info : infos)
        if (info.chip.has_value() && *info.chip == chip)
            result.push_back (&info);
    return result;
}

std::vector<const ParamInfo*> ParamRegistry::globalParams() const
{
    std::vector<const ParamInfo*> result;
    for (const auto& info : infos)
        if (info.isGlobal())
            result.push_back (&info);
    return result;
}

juce::String ParamRegistry::engineParamId (chipdsp::ChipId chip, const char* engineKey)
{
    return juce::String (chipdsp::chipKey (chip)) + "_" + juce::String (engineKey);
}

uint32_t ParamRegistry::chipChannelMask (chipdsp::ChipId chip) noexcept
{
    const int n = chipdsp::chipChannelCount (chip);
    return n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1u);
}

uint32_t ParamRegistry::defaultPolyMask (chipdsp::ChipId chip) noexcept
{
    switch (chip)
    {
        case chipdsp::ChipId::Nes:     return 0x07u;   // pulse 1, pulse 2, triangle
        case chipdsp::ChipId::Snes:    return 0xFFu;   // voices 1..8
        case chipdsp::ChipId::Genesis: return 0x3Fu;   // FM 1..6
    }
    return chipChannelMask (chip);
}

} // namespace rcv
