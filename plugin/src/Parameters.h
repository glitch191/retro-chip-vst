#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "chipdsp/ChipTypes.h"
#include "chipdsp/IChipEngine.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace rcv
{

// Host parameter ids of the global (chip-independent) parameters, fixed by
// docs/PLUGIN_SPECS.md. Engine parameters use "<chipKey>_<engineKey>", built by
// ParamRegistry::engineParamId().
namespace ParamIds
{
    inline constexpr const char* chip = "chip";
    inline constexpr const char* rawOutput = "raw_output";
    inline constexpr const char* voiceMode = "voice_mode";
    inline constexpr const char* polyChannels = "poly_channels";
    inline constexpr const char* masterGain = "master_gain";
    inline constexpr const char* presetGain = "preset_gain";
    inline constexpr const char* arpEnabled = "arp_enabled";
    inline constexpr const char* arpPattern = "arp_pattern";
    inline constexpr const char* arpOctaves = "arp_octaves";
    inline constexpr const char* arpRateMode = "arp_rate_mode";
    inline constexpr const char* arpSyncDivision = "arp_sync_division";
    inline constexpr const char* arpFreeRate = "arp_free_rate";
    inline constexpr const char* arpGate = "arp_gate";
    inline constexpr const char* arpHold = "arp_hold";
    inline constexpr const char* glideTime = "glide_time";
    inline constexpr const char* glideMode = "glide_mode";
    inline constexpr const char* uiScale = "ui_scale";

    // Every global id, in the order they appear in the parameter layout.
    inline constexpr std::array<const char*, 17> all = {
        chip, rawOutput, voiceMode, polyChannels, masterGain, presetGain,
        arpEnabled, arpPattern, arpOctaves, arpRateMode, arpSyncDivision, arpFreeRate, arpGate, arpHold,
        glideTime, glideMode, uiScale
    };
} // namespace ParamIds

// Choice indices of the global choice parameters (they match the chipdsp enums).
enum class VoiceMode : int { MidiChannel = 0, Poly = 1 };

// Host parameter flavour created for a descriptor.
enum class ParamKind
{
    Float,   // juce::AudioParameterFloat
    Int,     // juce::AudioParameterInt
    Choice,  // juce::AudioParameterChoice (integer descriptor with choiceLabels)
    Bool     // juce::AudioParameterBool (globals only)
};

// One host parameter: an engine parameter of one chip, or a global.
struct ParamInfo
{
    juce::String id;                        // host parameter id ("nes_p1_duty", "arp_rate_mode")
    juce::String name;                      // display name ("NES Pulse 1 Duty")
    juce::String group;                     // UI group ("Pulse 1", "Echo", "Arpeggiator")
    std::optional<chipdsp::ChipId> chip;    // nullopt for globals
    int engineParamId = -1;                 // IChipEngine parameter id, -1 for globals
    chipdsp::ParamDesc desc {};             // native units and bounds (copy)
    ParamKind kind = ParamKind::Float;
    bool showOnPanel = true;
    int index = -1;                         // position in ParamRegistry::all()

    bool isGlobal() const noexcept { return ! chip.has_value(); }
    bool isChoice() const noexcept { return kind == ParamKind::Choice; }
    juce::String engineKey() const { return desc.key != nullptr ? juce::String (desc.key) : juce::String(); }

    // APVTS raw value (getRawParameterValue) <-> engine native value. They differ only for
    // choice parameters, whose raw value is the choice index while the native value is
    // minValue + index (a descriptor may start its enumeration above zero).
    float nativeFromRaw (float raw) const noexcept;
    float rawFromNative (float native) const noexcept;
    float clampNative (float native) const noexcept;
};

// Registry of every host parameter, built once at construction from the three engines'
// parameterDescriptors() plus the global table. Message thread only; EngineHost copies
// what it needs into audio-thread structures.
class ParamRegistry
{
public:
    // engines: NES, SNES, Genesis in chipdsp::ChipId order. Only parameterDescriptors() is read.
    explicit ParamRegistry (const std::array<const chipdsp::IChipEngine*, 3>& engines);

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout() const;

    const std::vector<ParamInfo>& all() const noexcept { return infos; }
    const ParamInfo* find (const juce::String& id) const noexcept;
    const ParamInfo* find (chipdsp::ChipId chip, int engineParamId) const noexcept;
    const ParamInfo* findByKey (chipdsp::ChipId chip, const juce::String& engineKey) const noexcept;
    std::vector<const ParamInfo*> engineParams (chipdsp::ChipId chip) const;
    std::vector<const ParamInfo*> globalParams() const;

    // "<chipKey>_<engineKey>"
    static juce::String engineParamId (chipdsp::ChipId chip, const char* engineKey);

    // Bit mask covering every hardware channel of the chip (NES 0x1F, SNES 0xFF, Genesis 0x3FF).
    static uint32_t chipChannelMask (chipdsp::ChipId chip) noexcept;

    // Default poly_channels mask per chip: NES pulses + triangle, SNES all voices, Genesis FM 1..6.
    static uint32_t defaultPolyMask (chipdsp::ChipId chip) noexcept;

    static constexpr int kNumChips = 3;
    static constexpr int kParameterVersionHint = 1;

private:
    void addGlobals();
    void addEngine (chipdsp::ChipId chip, const chipdsp::IChipEngine& engine);
    static std::unique_ptr<juce::RangedAudioParameter> makeParameter (const ParamInfo& info);

    std::vector<ParamInfo> infos;
    std::map<juce::String, int> byId;
    std::array<std::map<int, int>, kNumChips> byEngineId;
};

} // namespace rcv
