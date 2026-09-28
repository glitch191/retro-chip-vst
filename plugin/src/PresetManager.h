#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "EngineHost.h"
#include "Parameters.h"

#include "chipdsp/ChipTypes.h"

#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace rcv
{

// One preset document (docs/PLUGIN_SPECS.md "Presets").
struct Preset
{
    juce::String name;
    juce::String category;
    juce::String subcategory;
    chipdsp::ChipId chip = chipdsp::ChipId::Nes;
    juce::StringArray tags;
    std::vector<std::pair<juce::String, float>> params;          // engine key (no chip prefix) -> native value
    std::vector<std::pair<juce::String, float>> global;          // global parameter id -> value
    std::vector<std::pair<juce::String, juce::String>> samples;  // slot parameter key -> sample name
    bool isUser = false;                                          // imported at runtime

    const float* param (const juce::String& key) const noexcept;
    const float* globalValue (const juce::String& id) const noexcept;
    bool matches (const juce::String& text) const;               // case-insensitive substring over name and tags

    juce::var toVar() const;
    static bool fromVar (const juce::var& document, Preset& out);
    static std::optional<chipdsp::ChipId> parseChip (const juce::var& value);
};

// A WAV the user imported into an engine sample slot; kept for the plugin state.
struct UserSample
{
    chipdsp::ChipId chip = chipdsp::ChipId::Nes;
    int slot = 0;
    juce::MemoryBlock wav;   // 16-bit mono PCM WAV
};

// Factory banks, user presets, sample loading and navigation. Message thread only.
//
// Banks come from BinaryData (assets/presets/<chip>.json, embedded when RCV_HAS_ASSETS);
// missing banks simply yield no presets. Sample names resolve to BinaryData WAVs named
// "<chip>_<name>.wav" or "<name>.wav" (assets/samples/<chip>/<name>.wav loses its folder
// when embedded, so unique names per chip are expected); assets/samples/index.json, when
// present, gives the slot order per chip: { "nes": ["kick", ...] } or
// { "nes": [{ "name": "kick", "file": "kick.wav" }, ...] }.
//
// Applying a preset sets the chip, every engine parameter of that chip (values from the
// preset, engine defaults for keys the preset omits), the preset-managed globals
// (arpeggiator, glide, poly_channels: preset values or defaults, poly_channels defaulting
// to the chip's mask) and then loads the referenced samples into the slots. Unknown keys
// are logged and ignored. Other globals (raw output, voice mode, master gain, UI scale)
// are left alone.
//
// Navigation: presets() and search() also become the "current filtered list" used by
// next()/previous(); filtered() returns it. Pointers returned by the queries stay valid
// until the next loadBanks()/importFile().
class PresetManager final : public juce::ChangeBroadcaster
{
public:
    static constexpr double kMaxUserSampleSeconds = 10.0;

    PresetManager (juce::AudioProcessorValueTreeState& apvts, const ParamRegistry& registry, EngineHost& host);

    // ----- banks and queries -------------------------------------------------------------
    void loadBanks();
    int numPresets() const noexcept { return static_cast<int> (factory.size() + user.size()); }
    int numPresets (chipdsp::ChipId chip) const noexcept;
    juce::StringArray categories (chipdsp::ChipId chip) const;
    juce::StringArray subcategories (chipdsp::ChipId chip, const juce::String& category) const;
    std::vector<const Preset*> presets (chipdsp::ChipId chip, const juce::String& category = {}, const juce::String& subcategory = {});
    std::vector<const Preset*> search (chipdsp::ChipId chip, const juce::String& text);
    const std::vector<const Preset*>& filtered() const noexcept { return filteredList; }
    const Preset* findByName (chipdsp::ChipId chip, const juce::String& name) const noexcept;

    // ----- current preset ----------------------------------------------------------------
    void apply (const Preset& preset);
    const Preset* current() const noexcept { return hasCurrent ? &currentPreset : nullptr; }
    juce::String currentName() const { return hasCurrent ? currentPreset.name : juce::String(); }
    juce::String currentCategory() const { return hasCurrent ? currentPreset.category : juce::String(); }
    void next();
    void previous();

    // State restore: remembers the preset by name (and reloads its samples) without touching
    // parameters. An unknown name only sets the displayed name.
    void restoreCurrent (const juce::String& name, const juce::String& category);

    // ----- files -------------------------------------------------------------------------
    Preset captureCurrent() const;                 // the current parameter values as a preset document
    bool exportCurrent (const juce::File& file) const;
    bool importFile (const juce::File& file);      // JSON document or array; applies the first one

    // ----- user samples ------------------------------------------------------------------
    bool importUserSample (chipdsp::ChipId chip, int slot, const juce::File& wavFile);
    bool setUserSample (chipdsp::ChipId chip, int slot, const juce::MemoryBlock& wavData);
    void removeUserSample (chipdsp::ChipId chip, int slot);
    const std::vector<UserSample>& userSamples() const noexcept { return samples; }
    juce::ValueTree userSamplesToValueTree() const;     // <UserSamples> <Sample chip slot wav/> </UserSamples>
    void restoreUserSamples (const juce::ValueTree& tree);
    void clearUserSamples();

    // ----- helpers shared with the UI ----------------------------------------------------
    static bool decodeWav (const void* data, size_t numBytes, juce::AudioBuffer<float>& out, double& sampleRate);
    static juce::MemoryBlock encodeWavMono16 (const float* data, int numFrames, double sampleRate);
    static const char* findResource (const juce::String& originalFilename, int& numBytes);

private:
    struct SampleEntry
    {
        juce::String name;
        juce::String file;
    };

    void loadSampleIndex();
    int slotForSample (chipdsp::ChipId chip, const juce::String& name) const;
    juce::String fileForSample (chipdsp::ChipId chip, const juce::String& name) const;
    bool loadFactorySample (chipdsp::ChipId chip, const juce::String& slotKey, const juce::String& name, const Preset& preset);
    void loadPresetSamples (const Preset& preset);
    void setNative (const ParamInfo& info, float native);
    float nativeValue (const ParamInfo& info) const;
    static bool isPresetManagedGlobal (const juce::String& id);
    void setCurrent (const Preset& preset);
    int indexOfCurrentInFiltered() const;
    void ensureFiltered (chipdsp::ChipId chip);

    juce::AudioProcessorValueTreeState& apvts;
    const ParamRegistry& registry;
    EngineHost& host;

    std::deque<Preset> factory;
    std::deque<Preset> user;
    std::vector<const Preset*> filteredList;
    std::array<std::vector<SampleEntry>, ParamRegistry::kNumChips> sampleIndex;

    Preset currentPreset;
    bool hasCurrent = false;
    std::vector<UserSample> samples;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};

} // namespace rcv
