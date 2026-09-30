// Plugin state round trip: getStateInformation() on one processor, setStateInformation() on
// a fresh one, then every parameter, the preset name, the MIDI learn map and the user
// samples must match.

#include "TestHelpers.h"

#include "chipdsp/nes/Nes2A03Engine.h"
#include "chipdsp/snes/SnesDspEngine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <thread>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
    constexpr chipdsp::ChipId kChips[] = { chipdsp::ChipId::Nes, chipdsp::ChipId::Snes, chipdsp::ChipId::Genesis };

    // Moves every parameter away from its default: normalised 0.8 when the default sits in
    // the lower half, 0.2 otherwise. Returns the resulting raw values by id.
    std::map<juce::String, float> setEveryParameterAwayFromDefault (rcv::RetroChipProcessor& proc)
    {
        std::map<juce::String, float> expected;
        for (auto* p : proc.getParameters())
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
            REQUIRE (ranged != nullptr);
            const float defaultNorm = ranged->getDefaultValue();
            ranged->setValueNotifyingHost (defaultNorm < 0.5f ? 0.8f : 0.2f);

            const float raw = rcvtest::getRaw (proc, ranged->paramID);
            const float defaultRaw = ranged->convertFrom0to1 (defaultNorm);
            INFO ("parameter " << ranged->paramID);
            CHECK (std::abs (raw - defaultRaw) > 1.0e-3f);   // the round trip would prove nothing otherwise
            expected[ranged->paramID] = raw;
        }
        return expected;
    }

    juce::MemoryBlock makeTestWav()
    {
        constexpr int numFrames = 2000;
        constexpr double sampleRate = 16000.0;
        std::vector<float> data (numFrames);
        for (int i = 0; i < numFrames; ++i)
            data[static_cast<size_t> (i)] = 0.5f * std::sin (2.0f * 3.14159265f * 440.0f * static_cast<float> (i) / static_cast<float> (sampleRate));
        return rcv::PresetManager::encodeWavMono16 (data.data(), numFrames, sampleRate);
    }

    std::optional<chipdsp::ChipId> chipWithSampleSlots (rcv::RetroChipProcessor& proc)
    {
        for (auto chip : kChips)
            if (proc.engineHost().engine (chip).numSampleSlots() > 0)
                return chip;
        return std::nullopt;
    }
} // namespace

TEST_CASE ("State round trip restores parameters, preset name, MIDI learn and user samples", "[state]")
{
    auto source = rcvtest::makeProcessor();
    const auto expected = setEveryParameterAwayFromDefault (*source);
    REQUIRE (expected.count (rcv::ParamIds::presetGain) == 1);   // preset_gain goes through the round trip too

    const juce::String presetName ("Plugin Test Preset");
    const juce::String presetCategory ("Test Category");
    source->presetManager().restoreCurrent (presetName, presetCategory);
    REQUIRE (source->presetManager().currentName() == presetName);

    const auto learnTarget = source->paramRegistry().engineParams (chipdsp::ChipId::Snes).front()->id;
    source->midiLearn().setMapping (74, learnTarget);
    REQUIRE (source->midiLearn().controllerForParam (learnTarget) == 74);

    // Real engines: one user sample in NES DMC slot 0 and one in SNES slot 1, encoded for the
    // parameter values set above (dmc_rate, echo_delay budget).
    const auto sampleChip = chipWithSampleSlots (*source);
    const bool realSlots = sampleChip.has_value() && ! rcvtest::usesStubEngines (*source);
    if (realSlots)
    {
        REQUIRE (source->presetManager().setUserSample (chipdsp::ChipId::Nes, 0, makeTestWav()));
        REQUIRE (source->presetManager().setUserSample (chipdsp::ChipId::Snes, 1, makeTestWav()));
    }
    else
    {
        WARN ("No engine exposes sample slots (stub engines): only the empty user-sample case is checked");
    }
    const int numUserSamples = realSlots ? 2 : 0;

    juce::MemoryBlock state;
    source->getStateInformation (state);
    REQUIRE (state.getSize() > 0);

    // The stored document has the documented shape.
    const auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), static_cast<int> (state.getSize()));
    REQUIRE (xml != nullptr);
    CHECK (xml->hasTagName ("RetroChipState"));
    CHECK (xml->getIntAttribute ("version") == rcv::RetroChipProcessor::kStateVersion);
    CHECK (xml->getStringAttribute ("presetName") == presetName);
    CHECK (xml->getStringAttribute ("presetCategory") == presetCategory);
    CHECK (xml->getChildByName ("Parameters") != nullptr);
    REQUIRE (xml->getChildByName ("MidiLearn") != nullptr);
    CHECK (xml->getChildByName ("MidiLearn")->getNumChildElements() == 1);
    REQUIRE (xml->getChildByName ("UserSamples") != nullptr);
    CHECK (xml->getChildByName ("UserSamples")->getNumChildElements() == numUserSamples);

    auto restored = rcvtest::makeProcessor();
    const auto* sourceNes = dynamic_cast<const chipdsp::Nes2A03Engine*> (&source->engineHost().engine (chipdsp::ChipId::Nes));
    const auto* sourceSnes = dynamic_cast<const chipdsp::SnesDspEngine*> (&source->engineHost().engine (chipdsp::ChipId::Snes));
    const auto* restoredNes = dynamic_cast<const chipdsp::Nes2A03Engine*> (&restored->engineHost().engine (chipdsp::ChipId::Nes));
    const auto* restoredSnes = dynamic_cast<const chipdsp::SnesDspEngine*> (&restored->engineHost().engine (chipdsp::ChipId::Snes));
    if (realSlots)
    {
        REQUIRE (sourceNes != nullptr);
        REQUIRE (sourceSnes != nullptr);
        REQUIRE (restoredNes != nullptr);
        REQUIRE (restoredSnes != nullptr);
        // Before the restore: the start-up sample in NES slot 0, nothing in SNES slot 1.
        REQUIRE (restored->presetManager().factorySampleInSlot (chipdsp::ChipId::Nes, 0).isNotEmpty());
        REQUIRE_FALSE (restoredSnes->sampleInfo (1).loaded);
    }
    restored->setStateInformation (state.getData(), static_cast<int> (state.getSize()));

    for (const auto& [id, value] : expected)
    {
        INFO ("parameter " << id);
        CHECK_THAT (rcvtest::getRaw (*restored, id), WithinAbs (value, 1.0e-5));
    }
    CHECK (restored->selectedChip() == source->selectedChip());

    CHECK (restored->presetManager().currentName() == presetName);
    CHECK (restored->presetManager().currentCategory() == presetCategory);

    CHECK (restored->midiLearn().controllerForParam (learnTarget) == 74);
    CHECK (restored->midiLearn().paramForController (74) == learnTarget);

    if (realSlots)
    {
        const auto& userSamples = restored->presetManager().userSamples();
        REQUIRE (userSamples.size() == 2);
        CHECK (userSamples[0].chip == chipdsp::ChipId::Nes);
        CHECK (userSamples[0].slot == 0);
        CHECK (userSamples[1].chip == chipdsp::ChipId::Snes);
        CHECK (userSamples[1].slot == 1);
        CHECK (userSamples[0].wav == source->presetManager().userSamples()[0].wav);
        CHECK (userSamples[1].wav == source->presetManager().userSamples()[1].wav);
        CHECK (restored->presetManager().sampleStatus().isEmpty());

        // The engines hold the same encoded data as the source's: same DMC length (encoded at
        // the restored dmc_rate), same BRR size, stored rate and root note / one-shot loop.
        CHECK (restoredNes->sampleLength (0) > 0);
        CHECK (restoredNes->sampleLength (0) == sourceNes->sampleLength (0));
        CHECK (restored->presetManager().factorySampleInSlot (chipdsp::ChipId::Nes, 0).isEmpty());   // the user sample replaced it
        const auto a = sourceSnes->sampleInfo (1);
        const auto b = restoredSnes->sampleInfo (1);
        CHECK (b.loaded);
        CHECK (b.brrBytes == a.brrBytes);
        CHECK (b.brrBytes > 0);
        CHECK (b.storedRate == a.storedRate);
        CHECK (b.rootNote == a.rootNote);
        CHECK (b.loopStartBlock == a.loopStartBlock);
    }
    else
    {
        CHECK (restored->presetManager().userSamples().empty());
    }
}

TEST_CASE ("A stored user sample the engine cannot take is dropped on restore", "[state]")
{
    auto proc = rcvtest::makeProcessor();
    // Stub engines refuse every sample. Real engines refuse one larger than the hardware
    // memory: 5 s at 32 kHz is 90000 BRR bytes on the SNES (64 KiB of APU RAM) and more
    // than the 4081-byte DMC limit on the NES.
    const bool real = ! rcvtest::usesStubEngines (*proc);
    const char* chipKeyText = real ? "snes" : "nes";

    // Hand-made state: the default parameters plus one sample in slot 0.
    juce::MemoryBlock state;
    proc->getStateInformation (state);
    auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), static_cast<int> (state.getSize()));
    REQUIRE (xml != nullptr);
    auto* samples = xml->getChildByName ("UserSamples");
    REQUIRE (samples != nullptr);
    juce::MemoryBlock wav = makeTestWav();
    if (real)
    {
        std::vector<float> longData (160000, 0.25f);
        wav = rcv::PresetManager::encodeWavMono16 (longData.data(), static_cast<int> (longData.size()), 32000.0);
    }
    auto* node = samples->createNewChildElement ("Sample");
    node->setAttribute ("chip", chipKeyText);
    node->setAttribute ("slot", 0);
    node->setAttribute ("wav", juce::Base64::toBase64 (wav.getData(), wav.getSize()));

    juce::MemoryBlock edited;
    juce::AudioProcessor::copyXmlToBinary (*xml, edited);
    auto restored = rcvtest::makeProcessor();
    restored->setStateInformation (edited.getData(), static_cast<int> (edited.getSize()));
    CHECK (restored->presetManager().userSamples().empty());
    CHECK (restored->presetManager().sampleStatus().isNotEmpty());   // reported, not silent
}

TEST_CASE ("getState right after an off-thread setState writes the restored preset", "[state]")
{
    auto source = rcvtest::makeProcessor();
    source->presetManager().restoreCurrent ("Queued Preset", "Queued Category");
    juce::MemoryBlock state;
    source->getStateInformation (state);

    auto target = rcvtest::makeProcessor();
    target->presetManager().restoreCurrent ("Old Preset", "Old Category");

    // The host restores from a worker thread: the preset part is queued for the message
    // thread, which has not run yet when the host asks for the state again.
    std::thread worker ([&] { target->setStateInformation (state.getData(), static_cast<int> (state.getSize())); });
    worker.join();
    CHECK (target->presetManager().currentName() == "Old Preset");   // not applied yet

    juce::MemoryBlock again;
    std::thread reader ([&] { target->getStateInformation (again); });   // also from a worker thread
    reader.join();
    const auto xml = juce::AudioProcessor::getXmlFromBinary (again.getData(), static_cast<int> (again.getSize()));
    REQUIRE (xml != nullptr);
    CHECK (xml->getStringAttribute ("presetName") == "Queued Preset");
    CHECK (xml->getStringAttribute ("presetCategory") == "Queued Category");
    REQUIRE (xml->getChildByName ("UserSamples") != nullptr);
}

TEST_CASE ("Restoring a state never writes a parameter", "[state]")
{
    auto source = rcvtest::makeProcessor();
    auto& pm = source->presetManager();

    // A factory preset when the banks are embedded, otherwise a hand-made one; either way
    // on SNES, then one parameter edited after loading it.
    rcv::Preset preset;
    const auto factory = pm.presets (chipdsp::ChipId::Snes);
    if (! factory.empty())
    {
        preset = *factory.front();
    }
    else
    {
        preset.name = "Restore Test";
        preset.chip = chipdsp::ChipId::Snes;
    }
    pm.apply (preset);
    REQUIRE (source->selectedChip() == chipdsp::ChipId::Snes);
    const auto* edited = source->paramRegistry().engineParams (chipdsp::ChipId::Snes).front();
    auto* editedParam = source->parameters().getParameter (edited->id);
    editedParam->setValueNotifyingHost (editedParam->getValue() < 0.5f ? 0.9f : 0.1f);
    if (! factory.empty())
        pm.restoreCurrent (preset.name, preset.category);   // what setState does with the name
    else
        pm.restoreCurrent ("Restore Test", {});

    juce::MemoryBlock state;
    source->getStateInformation (state);

    auto restored = rcvtest::makeProcessor();
    struct Counter final : juce::AudioProcessorParameter::Listener
    {
        int changes = 0;
        void parameterValueChanged (int, float) override { ++changes; }
        void parameterGestureChanged (int, bool) override {}
    };
    restored->setStateInformation (state.getData(), static_cast<int> (state.getSize()));

    // Every parameter matches the source, including the edited one and the sample slots.
    for (auto* p : source->getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
        REQUIRE (ranged != nullptr);
        INFO ("parameter " << ranged->paramID);
        CHECK_THAT (rcvtest::getRaw (*restored, ranged->paramID), WithinAbs (rcvtest::getRaw (*source, ranged->paramID), 1.0e-5));
    }
    CHECK (restored->presetManager().currentName() == source->presetManager().currentName());
    REQUIRE (restored->presetManager().current() != nullptr);
    CHECK (restored->presetManager().current()->chip == chipdsp::ChipId::Snes);

    // A second restore of the same state on the restored processor changes no parameter.
    Counter counter;
    for (auto* p : restored->getParameters())
        p->addListener (&counter);
    restored->presetManager().restoreState (restored->presetManager().currentName(), {}, juce::ValueTree ("UserSamples"));
    for (auto* p : restored->getParameters())
        p->removeListener (&counter);
    CHECK (counter.changes == 0);
}

TEST_CASE ("Applying a preset sets the chip after every engine parameter, in gestures", "[state][presets]")
{
    auto proc = rcvtest::makeProcessor();
    const auto& registry = proc->paramRegistry();

    rcv::Preset preset;
    preset.name = "Order Test";
    preset.chip = chipdsp::ChipId::Genesis;
    for (const auto* info : registry.engineParams (chipdsp::ChipId::Genesis))
        if (! info->isChoice())
            preset.params.emplace_back (info->engineKey(), info->desc.minValue + 0.75f * (info->desc.maxValue - info->desc.minValue));

    struct Recorder final : juce::AudioProcessorParameter::Listener
    {
        std::vector<int> changed;
        std::vector<int> gestureStarts;
        void parameterValueChanged (int index, float) override { changed.push_back (index); }
        void parameterGestureChanged (int index, bool starting) override
        {
            if (starting)
                gestureStarts.push_back (index);
        }
    } recorder;
    for (auto* p : proc->getParameters())
        p->addListener (&recorder);
    proc->presetManager().apply (preset);
    for (auto* p : proc->getParameters())
        p->removeListener (&recorder);

    auto* chipParam = proc->parameters().getParameter (rcv::ParamIds::chip);
    REQUIRE (chipParam != nullptr);
    const int chipIndex = chipParam->getParameterIndex();
    REQUIRE (! recorder.changed.empty());
    CHECK (recorder.changed.back() == chipIndex);   // the chip is written last
    CHECK (recorder.changed.size() >= 2);
    for (int index : recorder.changed)
    {
        INFO ("parameter index " << index);
        CHECK (std::find (recorder.gestureStarts.begin(), recorder.gestureStarts.end(), index) != recorder.gestureStarts.end());
    }
    CHECK (proc->selectedChip() == chipdsp::ChipId::Genesis);
}

TEST_CASE ("preset_gain is preset-managed: applied, reset to 0 dB when missing, exported, kept by the state", "[state][presets]")
{
    auto proc = rcvtest::makeProcessor();
    auto& pm = proc->presetManager();

    rcv::Preset loud;
    loud.name = "Gain Test";
    loud.chip = chipdsp::ChipId::Nes;
    loud.global.emplace_back (rcv::ParamIds::presetGain, 14.5f);
    pm.apply (loud);
    CHECK (rcvtest::getRaw (*proc, rcv::ParamIds::presetGain) == 14.5f);

    // Exported presets carry it in "global".
    const auto captured = pm.captureCurrent();
    const float* exported = captured.globalValue (rcv::ParamIds::presetGain);
    REQUIRE (exported != nullptr);
    CHECK (*exported == 14.5f);

    // The plugin state keeps it.
    juce::MemoryBlock state;
    proc->getStateInformation (state);
    auto restored = rcvtest::makeProcessor();
    restored->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    CHECK (rcvtest::getRaw (*restored, rcv::ParamIds::presetGain) == 14.5f);

    // A preset without it returns to the default, and master_gain is never touched.
    rcvtest::setRaw (*proc, rcv::ParamIds::masterGain, -3.0f);
    rcv::Preset plain;
    plain.name = "Plain";
    plain.chip = chipdsp::ChipId::Snes;
    pm.apply (plain);
    CHECK (rcvtest::getRaw (*proc, rcv::ParamIds::presetGain) == 0.0f);
    CHECK (rcvtest::getRaw (*proc, rcv::ParamIds::masterGain) == -3.0f);
}

TEST_CASE ("Invalid state data leaves the processor untouched", "[state]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::setRaw (*proc, rcv::ParamIds::masterGain, -6.0f);
    proc->midiLearn().setMapping (20, rcv::ParamIds::glideTime);

    const char garbage[] = "not a plugin state";
    proc->setStateInformation (garbage, static_cast<int> (sizeof (garbage)));
    CHECK_THAT (rcvtest::getRaw (*proc, rcv::ParamIds::masterGain), WithinAbs (-6.0, 1.0e-4));
    CHECK (proc->midiLearn().controllerForParam (rcv::ParamIds::glideTime) == 20);

    // A valid XML document of another type is ignored as well.
    juce::XmlElement other ("SomethingElse");
    juce::MemoryBlock block;
    juce::AudioProcessor::copyXmlToBinary (other, block);
    proc->setStateInformation (block.getData(), static_cast<int> (block.getSize()));
    CHECK_THAT (rcvtest::getRaw (*proc, rcv::ParamIds::masterGain), WithinAbs (-6.0, 1.0e-4));
}
