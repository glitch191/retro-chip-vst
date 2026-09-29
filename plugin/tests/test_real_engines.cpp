// Plugin core with the real chip engines: every factory preset applied in sequence (samples,
// parameter values, SNES APU RAM budget), sample-load failures reported, Poly + arpeggiator
// channel use on the NES, and MIDI learn surviving the plugin state. Skipped on the stubs.

#include "TestHelpers.h"

#include "chipdsp/EngineFactory.h"
#include "chipdsp/nes/Nes2A03Engine.h"
#include "chipdsp/snes/SnesDspEngine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <vector>

namespace
{
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlock = 64;
    constexpr chipdsp::ChipId kChips[] = { chipdsp::ChipId::Nes, chipdsp::ChipId::Snes, chipdsp::ChipId::Genesis };

    int presetSlot (const rcv::Preset& preset, const juce::String& slotKey)
    {
        const float* v = preset.param (slotKey);
        return v != nullptr ? static_cast<int> (std::lround (*v)) : -1;
    }

    // The value the engine must hold after the preset: the preset's (or the default) value,
    // clamped to the descriptor range and rounded for integer parameters.
    float expectedEngineValue (const rcv::Preset& preset, const rcv::ParamInfo& info)
    {
        const float* v = preset.param (info.engineKey());
        float x = std::clamp (v != nullptr ? *v : info.desc.defaultValue, info.desc.minValue, info.desc.maxValue);
        if (info.desc.isInteger || info.isChoice())
            x = std::round (x);
        return x;
    }

    int loadedSnesSlots (const chipdsp::SnesDspEngine& snes)
    {
        int n = 0;
        for (int s = 0; s < snes.numSampleSlots(); ++s)
            if (snes.sampleInfo (s).loaded)
                ++n;
        return n;
    }

    juce::MemoryBlock sineWav (int numFrames, double sampleRate)
    {
        std::vector<float> data (static_cast<size_t> (numFrames));
        for (int i = 0; i < numFrames; ++i)
            data[static_cast<size_t> (i)] = 0.5f * std::sin (2.0f * 3.14159265f * 220.0f * static_cast<float> (i) / static_cast<float> (sampleRate));
        return rcv::PresetManager::encodeWavMono16 (data.data(), numFrames, sampleRate);
    }
} // namespace

TEST_CASE ("Every factory preset applies with its samples and parameter values", "[presets][real]")
{
    auto proc = rcvtest::makeProcessor();
    if (rcvtest::usesStubEngines (*proc))
        SKIP ("Stub engines: no sample memory");
    auto& pm = proc->presetManager();
    auto& host = proc->engineHost();
    rcvtest::Runner runner (*proc, kSampleRate, kBlock);
    const auto* nes = dynamic_cast<const chipdsp::Nes2A03Engine*> (&host.engine (chipdsp::ChipId::Nes));
    const auto* snes = dynamic_cast<const chipdsp::SnesDspEngine*> (&host.engine (chipdsp::ChipId::Snes));
    REQUIRE (nes != nullptr);
    REQUIRE (snes != nullptr);

    // A user sample in NES slot 0, which no factory preset writes: it must survive the whole
    // NES bank. SNES user samples in a slot presets write are replaced (and leave the state).
    REQUIRE (pm.setUserSample (chipdsp::ChipId::Nes, 0, sineWav (2000, 16000.0)));
    const int userNesLength = nes->sampleLength (0);
    REQUIRE (userNesLength > 0);

    // Bank order for every chip, then the SNES bank again in a shuffled order.
    std::vector<rcv::Preset> sequence;
    for (auto chip : kChips)
    {
        const auto list = pm.presets (chip);
        INFO ("chip " << chipdsp::chipKey (chip));
        REQUIRE (! list.empty());
        for (const auto* p : list)
            sequence.push_back (*p);
    }
    {
        std::vector<rcv::Preset> snesBank;
        for (const auto* p : pm.presets (chipdsp::ChipId::Snes))
            snesBank.push_back (*p);
        juce::Random random (20260928);
        for (int i = static_cast<int> (snesBank.size()) - 1; i > 0; --i)
            std::swap (snesBank[static_cast<size_t> (i)], snesBank[static_cast<size_t> (random.nextInt (i + 1))]);
        sequence.insert (sequence.end(), snesBank.begin(), snesBank.end());
    }

    int applied = 0, withSamples = 0, parameterChecks = 0;
    int minSnesFree = std::numeric_limits<int>::max();
    for (const auto& preset : sequence)
    {
        INFO ("preset '" << preset.name << "' (" << chipdsp::chipKey (preset.chip) << ")");
        pm.apply (preset);
        runner.process();   // the audio thread forwards the new values to the engines
        ++applied;

        CHECK (pm.sampleStatus().isEmpty());
        CHECK (proc->selectedChip() == preset.chip);

        // Samples: every named sample sits in the slot its parameter value names.
        std::set<int> used;
        for (const auto& [slotKey, name] : preset.samples)
        {
            const int slot = presetSlot (preset, slotKey);
            INFO ("sample '" << name << "' in slot " << slot);
            REQUIRE (slot >= 0);
            used.insert (slot);
            CHECK (pm.factorySampleInSlot (preset.chip, slot) == name);
            if (preset.chip == chipdsp::ChipId::Nes)
                CHECK (nes->sampleLength (slot) > 0);
            if (preset.chip == chipdsp::ChipId::Snes)
                CHECK (snes->sampleInfo (slot).loaded);
        }
        if (! preset.samples.empty())
            ++withSamples;
        // Only the preset's own factory samples stay loaded on its chip.
        for (int slot = 0; slot < host.engine (preset.chip).numSampleSlots(); ++slot)
            if (used.count (slot) == 0)
                CHECK (pm.factorySampleInSlot (preset.chip, slot).isEmpty());

        // Parameters: the engine holds every preset value (clamped, rounded for integers).
        const auto& engine = host.engine (preset.chip);
        for (const auto* info : proc->paramRegistry().engineParams (preset.chip))
        {
            const float expected = expectedEngineValue (preset, *info);
            const float actual = engine.getParameter (info->engineParamId);
            const float tolerance = 1.0e-4f * std::max (1.0f, info->desc.maxValue - info->desc.minValue);
            if (std::abs (actual - expected) > tolerance)
                FAIL_CHECK ("parameter " << info->id << ": engine " << actual << ", preset " << expected);
            ++parameterChecks;
        }

        // SNES APU RAM: the loaded BRR data always fits beside the echo buffer.
        if (preset.chip == chipdsp::ChipId::Snes)
        {
            CHECK (loadedSnesSlots (*snes) == static_cast<int> (used.size()));
            minSnesFree = std::min (minSnesFree, snes->freeSampleBytes());
            CHECK (snes->freeSampleBytes() >= 0);
        }
    }

    // The NES user sample in slot 0 is still there and still in the state.
    CHECK (nes->sampleLength (0) == userNesLength);
    const auto& userSamples = pm.userSamples();
    CHECK (std::any_of (userSamples.begin(), userSamples.end(), [] (const rcv::UserSample& s)
                        { return s.chip == chipdsp::ChipId::Nes && s.slot == 0; }));

    WARN ("applied " << applied << " presets (" << withSamples << " with samples), " << parameterChecks
          << " engine parameter checks, smallest free SNES APU RAM " << minSnesFree << " bytes");
}

TEST_CASE ("A user sample in a slot the preset writes is replaced and leaves the state", "[presets][real]")
{
    auto proc = rcvtest::makeProcessor();
    if (rcvtest::usesStubEngines (*proc))
        SKIP ("Stub engines: no sample memory");
    auto& pm = proc->presetManager();
    const auto list = pm.presets (chipdsp::ChipId::Snes);
    REQUIRE (! list.empty());
    const rcv::Preset preset = *list.front();
    REQUIRE (! preset.samples.empty());
    const int slot = presetSlot (preset, preset.samples.front().first);

    REQUIRE (pm.setUserSample (chipdsp::ChipId::Snes, slot, sineWav (2000, 16000.0)));
    REQUIRE (pm.userSamples().size() == 1);
    pm.apply (preset);
    CHECK (pm.sampleStatus().isEmpty());
    CHECK (pm.userSamples().empty());
    CHECK (pm.factorySampleInSlot (chipdsp::ChipId::Snes, slot) == preset.samples.front().second);
}

TEST_CASE ("A sample that does not fit the hardware memory is reported", "[presets][real]")
{
    auto proc = rcvtest::makeProcessor();
    if (rcvtest::usesStubEngines (*proc))
        SKIP ("Stub engines: no sample memory");
    auto& pm = proc->presetManager();
    const auto* snes = dynamic_cast<const chipdsp::SnesDspEngine*> (&proc->engineHost().engine (chipdsp::ChipId::Snes));
    REQUIRE (snes != nullptr);

    // A preset whose sample is large enough that it cannot sit beside a 3.5 s user sample.
    const rcv::Preset* big = nullptr;
    for (const auto* p : pm.presets (chipdsp::ChipId::Snes))
        if (! p->samples.empty() && (big == nullptr || p->samples.front().second == "piano_bright"))
            big = p;
    REQUIRE (big != nullptr);
    const int presetSlotIndex = presetSlot (*big, big->samples.front().first);
    const int userSlot = (presetSlotIndex + 1) % snes->numSampleSlots();

    // 3.5 s at 32 kHz = 112000 frames = 63000 BRR bytes: fits alone (64512), not with more.
    REQUIRE (pm.setUserSample (chipdsp::ChipId::Snes, userSlot, sineWav (112000, 32000.0)));
    CHECK (pm.sampleStatus().isEmpty());

    pm.apply (*big);
    INFO ("status: " << pm.sampleStatus());
    CHECK (pm.sampleStatus().isNotEmpty());
    CHECK (pm.sampleStatus().contains (big->samples.front().second));
    CHECK (pm.factorySampleInSlot (chipdsp::ChipId::Snes, presetSlotIndex).isEmpty());
    CHECK (snes->sampleInfo (userSlot).loaded);   // the user sample stays

    // A second user sample that cannot fit either is refused and reported.
    CHECK_FALSE (pm.setUserSample (chipdsp::ChipId::Snes, (userSlot + 1) % snes->numSampleSlots(), sineWav (32000, 32000.0)));
    CHECK (pm.sampleStatus().isNotEmpty());

    // Once the user sample is gone the preset loads again and the status clears.
    pm.clearUserSamples();
    CHECK_FALSE (snes->sampleInfo (userSlot).loaded);   // the slot's memory is released
    pm.apply (*big);
    CHECK (pm.sampleStatus().isEmpty());
}

TEST_CASE ("Poly mode with the arpeggiator on NES stays inside poly_channels", "[enginehost][poly][arp][real]")
{
    const uint32_t mask = GENERATE (0u, 0x0Bu, 0x1Fu);   // chip default (pulses + triangle), P1 + P2 + noise, all five
    INFO ("poly_channels " << mask);
    auto proc = rcvtest::makeProcessor();
    if (rcvtest::usesStubEngines (*proc))
        SKIP ("Stub engines");
    rcvtest::prepareAudibleDefaults (*proc);                     // long Pulse 1 release, looped DMC sample
    rcvtest::setRaw (*proc, "nes_p2_sw_release", 30.0f);
    rcvtest::setRaw (*proc, "nes_nz_sw_release", 30.0f);
    rcvtest::setRaw (*proc, rcv::ParamIds::polyChannels, static_cast<float> (mask));
    rcvtest::setRaw (*proc, rcv::ParamIds::arpEnabled, 1.0f);
    rcvtest::setRaw (*proc, rcv::ParamIds::arpRateMode, 1.0f);   // free rate
    rcvtest::setRaw (*proc, rcv::ParamIds::arpFreeRate, 40.0f);
    rcvtest::setRaw (*proc, rcv::ParamIds::arpOctaves, 3.0f);
    rcvtest::setRaw (*proc, rcv::ParamIds::arpGate, 90.0f);
    rcvtest::Runner runner (*proc, kSampleRate, kBlock);

    const uint32_t effective = mask == 0u ? rcv::ParamRegistry::defaultPolyMask (chipdsp::ChipId::Nes) : mask;
    const auto& nes = proc->engineHost().engine (chipdsp::ChipId::Nes);
    int maxActive = 0;
    std::set<int> usedChannels;
    for (int b = 0; b < 750; ++b)   // 1 s
    {
        if (b == 1)
            for (int note : { 48, 55, 60, 64, 67, 71 })
                runner.noteOn (1, note, 100, 3);
        if (b == 400)
            for (int note : { 48, 55, 60 })
                runner.noteOff (1, note, 7);
        if (b == 600)
            for (int note : { 64, 67, 71 })
                runner.noteOff (1, note, 9);
        runner.process();

        int active = 0;
        for (int ch = 0; ch < nes.numChannels(); ++ch)
        {
            if (! nes.isChannelActive (ch))
                continue;
            ++active;
            usedChannels.insert (ch);
            if ((effective & (1u << ch)) == 0u)
                FAIL_CHECK ("block " << b << ": channel " << ch << " outside poly_channels is active");
        }
        maxActive = std::max (maxActive, active);
        CHECK (active <= 5);
    }
    const int allowed = juce::countNumberOfBits (effective);
    CHECK (maxActive <= allowed);
    CHECK (maxActive >= 2);   // the releases overlap: the allocator rotates through the channels
    WARN ("poly_channels " << mask << ": at most " << maxActive << " of " << allowed << " channels active, "
          << usedChannels.size() << " distinct channels used");
}

TEST_CASE ("MIDI learn mappings survive getState/setState and drive the real engines", "[midilearn][state][real]")
{
    auto source = rcvtest::makeProcessor();
    if (rcvtest::usesStubEngines (*source))
        SKIP ("Stub engines");
    struct Mapping
    {
        int cc;
        chipdsp::ChipId chip;
        const char* key;
    };
    const Mapping mappings[] = { { 20, chipdsp::ChipId::Nes, "p1_volume" },
                                 { 74, chipdsp::ChipId::Snes, "volume" },
                                 { 71, chipdsp::ChipId::Genesis, "lfo_freq" } };
    for (const auto& m : mappings)
    {
        const auto* info = source->paramRegistry().findByKey (m.chip, m.key);
        INFO ("parameter " << chipdsp::chipKey (m.chip) << " " << m.key);
        REQUIRE (info != nullptr);
        source->midiLearn().setMapping (m.cc, info->id);
    }

    juce::MemoryBlock state;
    source->getStateInformation (state);
    auto restored = rcvtest::makeProcessor();
    restored->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    rcvtest::Runner runner (*restored, kSampleRate, kBlock);

    for (const auto& m : mappings)
    {
        const auto* info = restored->paramRegistry().findByKey (m.chip, m.key);
        INFO ("CC " << m.cc << " -> " << info->id);
        CHECK (restored->midiLearn().controllerForParam (info->id) == m.cc);
        CHECK (restored->midiLearn().paramForController (m.cc) == info->id);

        // CC 127 then CC 0 reach the engine at its maximum and minimum.
        const auto& engine = restored->engineHost().engine (m.chip);
        for (const int value : { 127, 0 })
        {
            runner.controller (1, m.cc, value);
            runner.process();
            restored->midiLearn().drain();
            runner.process();
            CHECK (engine.getParameter (info->engineParamId) == (value == 127 ? info->desc.maxValue : info->desc.minValue));
        }
    }
}
