#include "PresetManager.h"

#include "chipdsp/EngineFactory.h"

#if RCV_HAS_ASSETS
    #include "RetroChipAssets.h"
#endif

#include <algorithm>
#include <cmath>

namespace rcv
{

namespace
{
    const juce::Identifier kSamplesTree ("UserSamples");
    const juce::Identifier kSampleNode ("Sample");
    const juce::Identifier kChipProp ("chip");
    const juce::Identifier kSlotProp ("slot");
    const juce::Identifier kWavProp ("wav");

    void logLine (const juce::String& text)
    {
        juce::Logger::writeToLog ("PresetManager: " + text);
    }

    juce::var numberVar (float v)
    {
        const float r = std::round (v);
        if (r == v && std::abs (v) < 1.0e9f)
            return juce::var (static_cast<int> (r));
        return juce::var (static_cast<double> (v));
    }

    bool readNumber (const juce::var& v, float& out)
    {
        if (v.isInt() || v.isInt64() || v.isDouble() || v.isBool())
        {
            out = static_cast<float> (static_cast<double> (v));
            return true;
        }
        if (v.isString())
        {
            const auto s = v.toString().trim();
            if (s.isNotEmpty() && s.containsOnly ("0123456789.-+eE"))
            {
                out = s.getFloatValue();
                return true;
            }
        }
        return false;
    }

    std::vector<std::pair<juce::String, float>> readNumberMap (const juce::var& v)
    {
        std::vector<std::pair<juce::String, float>> result;
        if (auto* obj = v.getDynamicObject())
            for (const auto& prop : obj->getProperties())
            {
                float value = 0.0f;
                if (readNumber (prop.value, value))
                    result.emplace_back (prop.name.toString(), value);
            }
        return result;
    }

    juce::var mapToVar (const std::vector<std::pair<juce::String, float>>& map)
    {
        auto* obj = new juce::DynamicObject();
        for (const auto& [key, value] : map)
            obj->setProperty (juce::Identifier (key), numberVar (value));
        return juce::var (obj);
    }
} // namespace

// ----- Preset ---------------------------------------------------------------------------------

const float* Preset::param (const juce::String& key) const noexcept
{
    for (const auto& p : params)
        if (p.first == key)
            return &p.second;
    return nullptr;
}

const float* Preset::globalValue (const juce::String& id) const noexcept
{
    for (const auto& g : global)
        if (g.first == id)
            return &g.second;
    return nullptr;
}

bool Preset::matches (const juce::String& text) const
{
    if (text.isEmpty())
        return true;
    if (name.containsIgnoreCase (text))
        return true;
    for (const auto& tag : tags)
        if (tag.containsIgnoreCase (text))
            return true;
    return false;
}

std::optional<chipdsp::ChipId> Preset::parseChip (const juce::var& value)
{
    if (value.isString())
    {
        const auto s = value.toString().trim();
        for (int c = 0; c < ParamRegistry::kNumChips; ++c)
        {
            const auto chip = static_cast<chipdsp::ChipId> (c);
            if (s.equalsIgnoreCase (chipdsp::chipKey (chip)) || s.equalsIgnoreCase (chipdsp::chipName (chip)))
                return chip;
        }
        return std::nullopt;
    }
    if (value.isInt() || value.isDouble())
    {
        const int c = static_cast<int> (value);
        if (c >= 0 && c < ParamRegistry::kNumChips)
            return static_cast<chipdsp::ChipId> (c);
    }
    return std::nullopt;
}

bool Preset::fromVar (const juce::var& document, Preset& out)
{
    auto* obj = document.getDynamicObject();
    if (obj == nullptr)
        return false;

    Preset p;
    p.name = obj->getProperty ("name").toString().trim();
    if (p.name.isEmpty())
        return false;
    const auto chip = parseChip (obj->getProperty ("chip"));
    if (! chip.has_value())
        return false;
    p.chip = *chip;
    p.category = obj->getProperty ("category").toString().trim();
    p.subcategory = obj->getProperty ("subcategory").toString().trim();

    if (auto* tags = obj->getProperty ("tags").getArray())
        for (const auto& t : *tags)
            if (t.isString())
                p.tags.add (t.toString());

    p.params = readNumberMap (obj->getProperty ("params"));
    p.global = readNumberMap (obj->getProperty ("global"));

    if (auto* samples = obj->getProperty ("samples").getDynamicObject())
        for (const auto& prop : samples->getProperties())
            if (prop.value.isString())
                p.samples.emplace_back (prop.name.toString(), prop.value.toString());

    out = std::move (p);
    return true;
}

juce::var Preset::toVar() const
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("name", name);
    obj->setProperty ("chip", juce::String (chipdsp::chipKey (chip)));
    obj->setProperty ("category", category);
    obj->setProperty ("subcategory", subcategory);

    juce::Array<juce::var> tagArray;
    for (const auto& t : tags)
        tagArray.add (t);
    obj->setProperty ("tags", tagArray);

    obj->setProperty ("params", mapToVar (params));
    obj->setProperty ("global", mapToVar (global));

    auto* sampleObj = new juce::DynamicObject();
    for (const auto& [key, value] : samples)
        sampleObj->setProperty (juce::Identifier (key), value);
    obj->setProperty ("samples", juce::var (sampleObj));
    return juce::var (obj);
}

// ----- PresetManager --------------------------------------------------------------------------

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& state, const ParamRegistry& reg, EngineHost& engineHost)
    : apvts (state), registry (reg), host (engineHost)
{
}

const char* PresetManager::findResource (const juce::String& originalFilename, int& numBytes)
{
    numBytes = 0;
#if RCV_HAS_ASSETS
    for (int i = 0; i < RetroChipAssets::namedResourceListSize; ++i)
    {
        const char* resourceName = RetroChipAssets::namedResourceList[i];
        const char* original = RetroChipAssets::getNamedResourceOriginalFilename (resourceName);
        if (original != nullptr && originalFilename.equalsIgnoreCase (original))
            return RetroChipAssets::getNamedResource (resourceName, numBytes);
    }
#else
    juce::ignoreUnused (originalFilename);
#endif
    return nullptr;
}

void PresetManager::loadBanks()
{
    factory.clear();
    filteredList.clear();
    loadSampleIndex();

    for (int c = 0; c < ParamRegistry::kNumChips; ++c)
    {
        const auto chip = static_cast<chipdsp::ChipId> (c);
        int size = 0;
        const char* data = findResource (juce::String (chipdsp::chipKey (chip)) + ".json", size);
        if (data == nullptr || size <= 0)
            continue;

        const auto parsed = juce::JSON::parse (juce::String::fromUTF8 (data, size));
        int added = 0;
        auto addDocument = [&] (const juce::var& doc)
        {
            Preset p;
            if (Preset::fromVar (doc, p))
            {
                if (p.chip != chip)
                    logLine ("bank " + juce::String (chipdsp::chipKey (chip)) + ": preset '" + p.name + "' declares another chip, kept as is");
                factory.push_back (std::move (p));
                ++added;
            }
        };
        if (auto* array = parsed.getArray())
        {
            for (const auto& doc : *array)
                addDocument (doc);
        }
        else
        {
            addDocument (parsed);
        }
        logLine ("bank " + juce::String (chipdsp::chipKey (chip)) + ": " + juce::String (added) + " presets");
    }

    sendChangeMessage();
}

void PresetManager::loadSampleIndex()
{
    for (auto& list : sampleIndex)
        list.clear();

    int size = 0;
    const char* data = findResource ("index.json", size);
    if (data == nullptr || size <= 0)
        return;

    const auto parsed = juce::JSON::parse (juce::String::fromUTF8 (data, size));
    auto* root = parsed.getDynamicObject();
    if (root == nullptr)
        return;

    for (int c = 0; c < ParamRegistry::kNumChips; ++c)
    {
        const auto chip = static_cast<chipdsp::ChipId> (c);
        auto* array = root->getProperty (juce::Identifier (chipdsp::chipKey (chip))).getArray();
        if (array == nullptr)
            continue;
        for (const auto& entry : *array)
        {
            SampleEntry e;
            if (entry.isString())
            {
                e.name = entry.toString();
            }
            else if (auto* obj = entry.getDynamicObject())
            {
                e.name = obj->getProperty ("name").toString();
                e.file = obj->getProperty ("file").toString();
            }
            if (e.name.isNotEmpty())
                sampleIndex[static_cast<size_t> (c)].push_back (e);
        }
    }
}

int PresetManager::slotForSample (chipdsp::ChipId chip, const juce::String& name) const
{
    const auto& list = sampleIndex[static_cast<size_t> (chip)];
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].name.equalsIgnoreCase (name))
            return static_cast<int> (i);
    return -1;
}

juce::String PresetManager::fileForSample (chipdsp::ChipId chip, const juce::String& name) const
{
    const auto& list = sampleIndex[static_cast<size_t> (chip)];
    for (const auto& e : list)
        if (e.name.equalsIgnoreCase (name) && e.file.isNotEmpty())
            return e.file;
    return {};
}

int PresetManager::numPresets (chipdsp::ChipId chip) const noexcept
{
    int n = 0;
    for (const auto& p : factory)
        if (p.chip == chip)
            ++n;
    for (const auto& p : user)
        if (p.chip == chip)
            ++n;
    return n;
}

juce::StringArray PresetManager::categories (chipdsp::ChipId chip) const
{
    juce::StringArray result;
    auto collect = [&] (const std::deque<Preset>& list)
    {
        for (const auto& p : list)
            if (p.chip == chip && p.category.isNotEmpty())
                result.addIfNotAlreadyThere (p.category);
    };
    collect (factory);
    collect (user);
    return result;
}

juce::StringArray PresetManager::subcategories (chipdsp::ChipId chip, const juce::String& category) const
{
    juce::StringArray result;
    auto collect = [&] (const std::deque<Preset>& list)
    {
        for (const auto& p : list)
            if (p.chip == chip && p.subcategory.isNotEmpty() && (category.isEmpty() || p.category == category))
                result.addIfNotAlreadyThere (p.subcategory);
    };
    collect (factory);
    collect (user);
    return result;
}

std::vector<const Preset*> PresetManager::presets (chipdsp::ChipId chip, const juce::String& category, const juce::String& subcategory)
{
    std::vector<const Preset*> result;
    auto collect = [&] (const std::deque<Preset>& list)
    {
        for (const auto& p : list)
            if (p.chip == chip && (category.isEmpty() || p.category == category)
                && (subcategory.isEmpty() || p.subcategory == subcategory))
                result.push_back (&p);
    };
    collect (factory);
    collect (user);
    filteredList = result;
    return result;
}

std::vector<const Preset*> PresetManager::search (chipdsp::ChipId chip, const juce::String& text)
{
    std::vector<const Preset*> result;
    const auto needle = text.trim();
    auto collect = [&] (const std::deque<Preset>& list)
    {
        for (const auto& p : list)
            if (p.chip == chip && p.matches (needle))
                result.push_back (&p);
    };
    collect (factory);
    collect (user);
    filteredList = result;
    return result;
}

const Preset* PresetManager::findByName (chipdsp::ChipId chip, const juce::String& name) const noexcept
{
    for (const auto& p : user)
        if (p.chip == chip && p.name == name)
            return &p;
    for (const auto& p : factory)
        if (p.chip == chip && p.name == name)
            return &p;
    return nullptr;
}

// ----- applying -------------------------------------------------------------------------------

void PresetManager::setNative (const ParamInfo& info, float native)
{
    auto* param = apvts.getParameter (info.id);
    if (param == nullptr)
        return;
    const float raw = info.rawFromNative (info.clampNative (native));
    param->setValueNotifyingHost (param->convertTo0to1 (raw));
}

float PresetManager::nativeValue (const ParamInfo& info) const
{
    if (auto* raw = apvts.getRawParameterValue (info.id))
        return info.nativeFromRaw (raw->load (std::memory_order_relaxed));
    return info.desc.defaultValue;
}

bool PresetManager::isPresetManagedGlobal (const juce::String& id)
{
    return id.startsWith ("arp_") || id.startsWith ("glide_") || id == juce::String (ParamIds::polyChannels);
}

void PresetManager::apply (const Preset& preset)
{
    if (const auto* chipInfo = registry.find (ParamIds::chip))
        setNative (*chipInfo, static_cast<float> (static_cast<int> (preset.chip)));

    for (const auto* info : registry.engineParams (preset.chip))
    {
        const float* value = preset.param (info->engineKey());
        setNative (*info, value != nullptr ? *value : info->desc.defaultValue);
    }
    for (const auto& [key, value] : preset.params)
        if (registry.findByKey (preset.chip, key) == nullptr)
            logLine ("preset '" + preset.name + "': unknown parameter key '" + key + "' ignored");

    for (const auto* info : registry.globalParams())
    {
        if (! isPresetManagedGlobal (info->id))
            continue;
        const float* value = preset.globalValue (info->id);
        float def = info->desc.defaultValue;
        if (info->id == juce::String (ParamIds::polyChannels))
            def = static_cast<float> (ParamRegistry::defaultPolyMask (preset.chip));
        setNative (*info, value != nullptr ? *value : def);
    }
    for (const auto& [id, value] : preset.global)
        if (registry.find (id) == nullptr || ! isPresetManagedGlobal (id))
            logLine ("preset '" + preset.name + "': global '" + id + "' is not preset-managed, ignored");

    loadPresetSamples (preset);
    setCurrent (preset);
    sendChangeMessage();
}

void PresetManager::loadPresetSamples (const Preset& preset)
{
    for (const auto& [slotKey, sampleName] : preset.samples)
        loadFactorySample (preset.chip, slotKey, sampleName, preset);
}

bool PresetManager::loadFactorySample (chipdsp::ChipId chip, const juce::String& slotKey, const juce::String& name, const Preset& preset)
{
    const auto* info = registry.findByKey (chip, slotKey);
    if (info == nullptr)
    {
        logLine ("preset '" + preset.name + "': unknown sample slot parameter '" + slotKey + "'");
        return false;
    }

    auto& engine = host.engine (chip);
    const int numSlots = engine.numSampleSlots();
    if (numSlots <= 0)
        return false;

    int slot = -1;
    if (const float* v = preset.param (slotKey))
        slot = static_cast<int> (std::lround (*v));
    else if (const int indexed = slotForSample (chip, name); indexed >= 0)
        slot = indexed;
    else
        slot = static_cast<int> (std::lround (info->desc.defaultValue));
    slot = juce::jlimit (0, numSlots - 1, slot);

    juce::StringArray candidates;
    const auto indexedFile = fileForSample (chip, name);
    if (indexedFile.isNotEmpty())
        candidates.add (indexedFile);
    candidates.add (juce::String (chipdsp::chipKey (chip)) + "_" + name + ".wav");
    candidates.add (name + ".wav");

    const char* data = nullptr;
    int size = 0;
    for (const auto& file : candidates)
    {
        data = findResource (file, size);
        if (data != nullptr && size > 0)
            break;
    }
    if (data == nullptr || size <= 0)
    {
        logLine ("preset '" + preset.name + "': sample '" + name + "' not found in the embedded assets");
        return false;
    }

    juce::AudioBuffer<float> audio;
    double sampleRate = 0.0;
    if (! decodeWav (data, static_cast<size_t> (size), audio, sampleRate))
    {
        logLine ("sample '" + name + "': cannot decode WAV");
        return false;
    }

    if (! host.loadUserSample (chip, slot, audio, sampleRate))
    {
        logLine ("sample '" + name + "': engine refused slot " + juce::String (slot));
        return false;
    }

    setNative (*info, static_cast<float> (slot));
    return true;
}

void PresetManager::setCurrent (const Preset& preset)
{
    currentPreset = preset;
    hasCurrent = true;
}

int PresetManager::indexOfCurrentInFiltered() const
{
    if (! hasCurrent)
        return -1;
    for (size_t i = 0; i < filteredList.size(); ++i)
        if (filteredList[i]->chip == currentPreset.chip && filteredList[i]->name == currentPreset.name)
            return static_cast<int> (i);
    return -1;
}

void PresetManager::ensureFiltered (chipdsp::ChipId chip)
{
    if (filteredList.empty())
        presets (chip);
}

void PresetManager::next()
{
    const auto chip = hasCurrent ? currentPreset.chip : host.activeChip();
    ensureFiltered (chip);
    if (filteredList.empty())
        return;
    const int idx = indexOfCurrentInFiltered();
    const int nextIdx = idx < 0 ? 0 : (idx + 1) % static_cast<int> (filteredList.size());
    apply (*filteredList[static_cast<size_t> (nextIdx)]);
}

void PresetManager::previous()
{
    const auto chip = hasCurrent ? currentPreset.chip : host.activeChip();
    ensureFiltered (chip);
    if (filteredList.empty())
        return;
    const int idx = indexOfCurrentInFiltered();
    const int count = static_cast<int> (filteredList.size());
    const int prevIdx = idx < 0 ? count - 1 : (idx - 1 + count) % count;
    apply (*filteredList[static_cast<size_t> (prevIdx)]);
}

void PresetManager::restoreCurrent (const juce::String& name, const juce::String& category)
{
    if (name.isEmpty())
    {
        hasCurrent = false;
        sendChangeMessage();
        return;
    }

    const Preset* found = nullptr;
    for (int c = 0; c < ParamRegistry::kNumChips && found == nullptr; ++c)
        found = findByName (static_cast<chipdsp::ChipId> (c), name);

    if (found != nullptr)
    {
        setCurrent (*found);
        loadPresetSamples (*found);
    }
    else
    {
        Preset p;
        p.name = name;
        p.category = category;
        p.chip = host.activeChip();
        setCurrent (p);
    }
    sendChangeMessage();
}

// ----- files ----------------------------------------------------------------------------------

Preset PresetManager::captureCurrent() const
{
    Preset p;
    const auto* chipInfo = registry.find (ParamIds::chip);
    p.chip = chipInfo != nullptr ? static_cast<chipdsp::ChipId> (juce::jlimit (0, ParamRegistry::kNumChips - 1, static_cast<int> (nativeValue (*chipInfo))))
                                 : host.activeChip();
    if (hasCurrent)
    {
        p.name = currentPreset.name;
        p.category = currentPreset.category;
        p.subcategory = currentPreset.subcategory;
        p.tags = currentPreset.tags;
        if (currentPreset.chip == p.chip)
            p.samples = currentPreset.samples;
    }
    if (p.name.isEmpty())
        p.name = "Untitled";

    for (const auto* info : registry.engineParams (p.chip))
        p.params.emplace_back (info->engineKey(), nativeValue (*info));
    for (const auto* info : registry.globalParams())
        if (isPresetManagedGlobal (info->id))
            p.global.emplace_back (info->id, nativeValue (*info));
    p.isUser = true;
    return p;
}

bool PresetManager::exportCurrent (const juce::File& file) const
{
    const auto preset = captureCurrent();
    const auto text = juce::JSON::toString (preset.toVar());
    return file.replaceWithText (text);
}

bool PresetManager::importFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return false;

    const auto parsed = juce::JSON::parse (file);
    std::vector<Preset> imported;
    auto addDocument = [&] (const juce::var& doc)
    {
        Preset p;
        if (Preset::fromVar (doc, p))
        {
            p.isUser = true;
            imported.push_back (std::move (p));
        }
    };
    if (auto* array = parsed.getArray())
    {
        for (const auto& doc : *array)
            addDocument (doc);
    }
    else
    {
        addDocument (parsed);
    }
    if (imported.empty())
        return false;

    const Preset* first = nullptr;
    for (auto& p : imported)
    {
        // Replace a user preset with the same chip and name.
        auto it = std::find_if (user.begin(), user.end(), [&] (const Preset& u) { return u.chip == p.chip && u.name == p.name; });
        if (it != user.end())
        {
            *it = std::move (p);
            if (first == nullptr)
                first = &(*it);
        }
        else
        {
            user.push_back (std::move (p));
            if (first == nullptr)
                first = &user.back();
        }
    }
    filteredList.clear();
    if (first != nullptr)
        apply (*first);
    return true;
}

// ----- user samples ---------------------------------------------------------------------------

bool PresetManager::decodeWav (const void* data, size_t numBytes, juce::AudioBuffer<float>& out, double& sampleRate)
{
    if (data == nullptr || numBytes == 0)
        return false;
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader (format.createReaderFor (new juce::MemoryInputStream (data, numBytes, false), true));
    if (reader == nullptr || reader->numChannels == 0 || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0)
        return false;

    const auto maxFrames = static_cast<juce::int64> (kMaxUserSampleSeconds * reader->sampleRate);
    const int numFrames = static_cast<int> (std::min<juce::int64> (reader->lengthInSamples, std::max<juce::int64> (1, maxFrames)));
    out.setSize (static_cast<int> (reader->numChannels), numFrames);
    if (! reader->read (&out, 0, numFrames, 0, true, true))
        return false;
    sampleRate = reader->sampleRate;
    return true;
}

juce::MemoryBlock PresetManager::encodeWavMono16 (const float* data, int numFrames, double sampleRate)
{
    juce::MemoryBlock block;
    if (data == nullptr || numFrames <= 0 || sampleRate <= 0.0)
        return block;

    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (block, false);
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (1).withBitsPerSample (16));
    if (writer == nullptr)
        return {};
    const float* channels[1] = { data };
    if (! writer->writeFromFloatArrays (channels, 1, numFrames))
        return {};
    writer.reset();   // finalises the header and trims the block
    return block;
}

bool PresetManager::importUserSample (chipdsp::ChipId chip, int slot, const juce::File& wavFile)
{
    juce::MemoryBlock fileData;
    if (! wavFile.existsAsFile() || ! wavFile.loadFileAsData (fileData))
        return false;
    return setUserSample (chip, slot, fileData);
}

bool PresetManager::setUserSample (chipdsp::ChipId chip, int slot, const juce::MemoryBlock& wavData)
{
    auto& engine = host.engine (chip);
    if (slot < 0 || slot >= engine.numSampleSlots())
        return false;

    juce::AudioBuffer<float> audio;
    double sampleRate = 0.0;
    if (! decodeWav (wavData.getData(), wavData.getSize(), audio, sampleRate))
        return false;

    if (! host.loadUserSample (chip, slot, audio, sampleRate))
        return false;

    // Keep a compact 16-bit mono copy for the plugin state.
    std::vector<float> mono (static_cast<size_t> (audio.getNumSamples()), 0.0f);
    const float scale = 1.0f / static_cast<float> (std::max (1, audio.getNumChannels()));
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const float* src = audio.getReadPointer (ch);
        for (int i = 0; i < audio.getNumSamples(); ++i)
            mono[static_cast<size_t> (i)] += src[i] * scale;
    }
    auto encoded = encodeWavMono16 (mono.data(), audio.getNumSamples(), sampleRate);
    if (encoded.getSize() == 0)
        return false;

    removeUserSample (chip, slot);
    samples.push_back ({ chip, slot, std::move (encoded) });
    sendChangeMessage();
    return true;
}

void PresetManager::removeUserSample (chipdsp::ChipId chip, int slot)
{
    samples.erase (std::remove_if (samples.begin(), samples.end(),
                                   [&] (const UserSample& s) { return s.chip == chip && s.slot == slot; }),
                   samples.end());
}

void PresetManager::clearUserSamples()
{
    samples.clear();
    sendChangeMessage();
}

juce::ValueTree PresetManager::userSamplesToValueTree() const
{
    juce::ValueTree tree (kSamplesTree);
    for (const auto& s : samples)
    {
        juce::ValueTree node (kSampleNode);
        node.setProperty (kChipProp, juce::String (chipdsp::chipKey (s.chip)), nullptr);
        node.setProperty (kSlotProp, s.slot, nullptr);
        node.setProperty (kWavProp, juce::Base64::toBase64 (s.wav.getData(), s.wav.getSize()), nullptr);
        tree.addChild (node, -1, nullptr);
    }
    return tree;
}

void PresetManager::restoreUserSamples (const juce::ValueTree& tree)
{
    samples.clear();
    if (! tree.isValid() || ! tree.hasType (kSamplesTree))
    {
        sendChangeMessage();
        return;
    }

    for (const auto& node : tree)
    {
        if (! node.hasType (kSampleNode))
            continue;
        const auto chip = Preset::parseChip (node.getProperty (kChipProp));
        const int slot = static_cast<int> (node.getProperty (kSlotProp, -1));
        const auto base64 = node.getProperty (kWavProp).toString();
        if (! chip.has_value() || slot < 0 || base64.isEmpty())
            continue;

        juce::MemoryOutputStream decoded;
        if (! juce::Base64::convertFromBase64 (decoded, base64))
            continue;
        juce::MemoryBlock block (decoded.getData(), decoded.getDataSize());
        setUserSample (*chip, slot, block);
    }
    sendChangeMessage();
}

} // namespace rcv
