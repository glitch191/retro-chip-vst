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
// missing banks simply yield no presets. Sample names resolve to the BinaryData WAV
// "<chip>_<file name>" (plugin/CMakeLists.txt stages assets/samples/<chip>/<name>.wav under
// that name because BinaryData drops folders). assets/samples/index.json, when present,
// gives the file of each name and the slot order per chip: the flat array written by
// tools/gen_samples.py ([{ "chip": "nes", "name": "kick_short", "file": "nes/kick_short.wav",
// ... }], order of appearance per chip = slot order); { "nes": ["kick", ...] } is also read.
// A missing index falls back to "<chip>_<name>.wav".
//
// Applying a preset sets every engine parameter of its chip (values from the preset,
// engine defaults for keys the preset omits), the preset-managed globals (arpeggiator,
// glide, poly_channels: preset values or defaults, poly_channels 0 = the chip's mask),
// loads the referenced samples into the slots and sets the chip last, each write in its
// own change gesture. Unknown keys are logged and ignored. Other globals (raw output,
// voice mode, master gain, UI scale) are left alone.
//
// Navigation: presets() and search() also become the "current filtered list" used by
// next()/previous(); filtered() returns it. Pointers returned by the queries stay valid
// until the next loadBanks()/importFile().
//
// State: stateSnapshot() may be called from any thread (the host's getState thread); it
// returns a copy of the current preset name/category and of the user samples tree, kept
// under a lock and refreshed whenever they change on the message thread.
class PresetManager final : public juce::ChangeBroadcaster
{
public:
    static constexpr double kMaxUserSampleSeconds = 10.0;

    struct StateSnapshot
    {
        juce::String presetName;
        juce::String presetCategory;
        juce::ValueTree samples;   // <UserSamples>; never modified once published (copy it before use)
    };

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

    // State restore, without touching parameters: remembers the preset by name, looked up on
    // the chip the `chip` parameter selects, and reloads its samples into the slots the
    // (restored) slot parameters reference. An unknown name only sets the displayed name.
    void restoreCurrent (const juce::String& name, const juce::String& category);

    // restoreCurrent() then restoreUserSamples(): the non-parameter part of setStateInformation().
    void restoreState (const juce::String& name, const juce::String& category, const juce::ValueTree& userSampleTree);

    // Any thread.
    StateSnapshot stateSnapshot() const;

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
    // Slots that held a user sample return to their default content (current preset's
    // sample, factory sample indexed at that slot, or silence), then the tree's samples load.
    void restoreUserSamples (const juce::ValueTree& tree);
    void clearUserSamples();

    // The sample-slot parameter of a chip (dmc_sample, sample, dac_sample) and the slot it
    // selects now; -1 when the chip's engine has no sample slots.
    const ParamInfo* sampleSlotParam (chipdsp::ChipId chip) const;
    int currentSampleSlot (chipdsp::ChipId chip) const;

    // ----- factory samples ---------------------------------------------------------------
    juce::StringArray sampleNames (chipdsp::ChipId chip) const;                  // index order
    int sampleIndexSlot (chipdsp::ChipId chip, const juce::String& name) const;  // -1 if unknown
    bool loadFactorySampleIntoSlot (chipdsp::ChipId chip, int slot, const juce::String& name);

    // Incremented by every apply(); lets the processor tell a preset's chip change apart
    // from a chip change made through the parameter alone.
    int applyCount() const noexcept { return applies; }

    // ----- helpers shared with the UI ----------------------------------------------------
    static bool decodeWav (const void* data, size_t numBytes, juce::AudioBuffer<float>& out, double& sampleRate);
    static juce::MemoryBlock encodeWavMono16 (const float* data, int numFrames, double sampleRate);
    static const char* findResource (const juce::String& originalFilename, int& numBytes);

private:
    struct SampleEntry
    {
        juce::String name;
        juce::String file;      // "<chip>/<name>.wav" relative to assets/samples, may be empty
        int rootNote = 60;      // informational: IChipEngine has no root-note API
    };

    void loadSampleIndex();
    const char* findSampleResource (chipdsp::ChipId chip, const juce::String& name, int& numBytes) const;
    int slotForSample (chipdsp::ChipId chip, const juce::String& name) const;
    juce::String fileForSample (chipdsp::ChipId chip, const juce::String& name) const;
    // writeSlotParam: applying a preset (slot from the preset document, then written to the
    // slot parameter); otherwise restoring (slot read from the parameter, nothing written).
    bool loadFactorySample (chipdsp::ChipId chip, const juce::String& slotKey, const juce::String& name,
                            const Preset& preset, bool writeSlotParam);
    void loadPresetSamples (const Preset& preset, bool writeSlotParams);
    int presetSampleSlot (chipdsp::ChipId chip, const juce::String& slotKey, const juce::String& name,
                          const Preset& preset, bool fromPresetDocument) const;
    void resetSlotToDefault (chipdsp::ChipId chip, int slot);
    void setNative (const ParamInfo& info, float native);
    float nativeValue (const ParamInfo& info) const;
    chipdsp::ChipId selectedChip() const;
    static bool isPresetManagedGlobal (const juce::String& id);
    void setCurrent (const Preset& preset);
    void publishCurrent();
    void publishSamples();

    static constexpr int kSilenceFrames = 16;        // one BRR block
    static constexpr double kSilenceRate = 32000.0;
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
    int applies = 0;
    std::vector<UserSample> samples;

    juce::CriticalSection snapshotLock;   // message thread vs. the host's state thread; never the audio thread
    StateSnapshot snapshot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};

} // namespace rcv
