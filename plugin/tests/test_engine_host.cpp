// Engine host behaviour seen through processBlock(): chip switch crossfade, MIDI-channel
// routing, Poly channel mask, channel buses.

#include "TestHelpers.h"

#include "chipdsp/EngineFactory.h"

#include "chipdsp/factory/StubEngine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cmath>
#include <functional>
#include <vector>

namespace
{
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlock = 64;
    constexpr float kPi = 3.14159265358979f;

    using BlockHook = std::function<void (rcv::RetroChipProcessor&, rcvtest::Runner&, int block)>;

    // Runs numBlocks blocks on a fresh processor; hook() queues MIDI / sets parameters before
    // each block. Blocks in [piecesFrom, piecesTo) are handed over as 32-sample pieces, the
    // sub-block size the engine host uses during a crossfade, so a reference rendered this
    // way sees the same renderBlock() boundaries as the crossfading processor. Returns the
    // main left channel.
    std::vector<float> renderMain (int numBlocks, const BlockHook& hook, int piecesFrom = -1, int piecesTo = -1)
    {
        auto proc = rcvtest::makeProcessor();
        rcvtest::Runner runner (*proc, kSampleRate, kBlock);
        std::vector<float> out;
        out.reserve (static_cast<size_t> (numBlocks * kBlock));
        for (int b = 0; b < numBlocks; ++b)
        {
            hook (*proc, runner, b);
            if (b >= piecesFrom && b < piecesTo)
                runner.processInPieces (rcv::EngineHost::kControlIntervalSamples);
            else
                runner.process();
            out.insert (out.end(), runner.mainChannel (0), runner.mainChannel (0) + kBlock);
        }
        return out;
    }

    float maxAbs (const std::vector<float>& x, size_t from, size_t to)
    {
        float m = 0.0f;
        for (size_t i = from; i < to && i < x.size(); ++i)
            m = std::max (m, std::abs (x[i]));
        return m;
    }

    float maxStep (const std::vector<float>& x, size_t from, size_t to)
    {
        float m = 0.0f;
        for (size_t i = std::max<size_t> (from, 1); i < to && i < x.size(); ++i)
            m = std::max (m, std::abs (x[i] - x[i - 1]));
        return m;
    }

    int countActive (const chipdsp::IChipEngine& engine)
    {
        int n = 0;
        for (int c = 0; c < engine.numChannels(); ++c)
            if (engine.isChannelActive (c))
                ++n;
        return n;
    }
} // namespace

// A note held on the outgoing chip keeps sounding; at block 100 the chip parameter switches
// (optionally with a new note on the new chip). Three processors are rendered, all starting
// on the outgoing chip (its start-up switch away from NES is over before the note at block 20):
//   Y: the note on the outgoing chip, switch at block 100 (the case under test);
//   B: the note, note-off at block 100, no switch (what the outgoing engine plays);
//   A: no note, switch at block 100 (the incoming engine with its fade-in alone).
// The engines are deterministic and the outgoing engine's fade gain is applied to B only,
// so during the 20 ms equal-power fade (L = 0.020 * 48000 = 960 samples)
//     Y[n] = A[n] + cos(pi/2 * (k + 1) / L) * B[n],   k = n - switchSample,
// and after it Y = A. Step bound, from |cos'| <= (pi/2) / L per sample:
//     |Y[n] - Y[n-1]| <= |dA[n]| + |dB[n]| + |B[n-1]| * (pi/2) / L.
// A hard cut instead of the fade would step by |B[switch - 1]|. With the real engines the
// NES note has a one-second release and the SNES plays a looped sample (prepareAudibleDefaults),
// so the fade always shapes an audible tail.
TEST_CASE ("Chip switch crossfades without clicks", "[enginehost][crossfade]")
{
    constexpr int kBlocks = 200;
    constexpr int kNoteBlock = 20;
    constexpr int kSwitchBlock = 100;
    constexpr size_t kSwitch = static_cast<size_t> (kSwitchBlock * kBlock);
    const int fadeLength = static_cast<int> (std::lround (rcv::EngineHost::kCrossfadeSeconds * kSampleRate));
    REQUIRE (fadeLength == 960);
    const float maxGainStep = (kPi * 0.5f) / static_cast<float> (fadeLength);
    const int fadeBlocks = fadeLength / kBlock + 1;

    struct Switch
    {
        chipdsp::ChipId from, to;
    };
    const auto sw = GENERATE (Switch { chipdsp::ChipId::Nes, chipdsp::ChipId::Snes },
                              Switch { chipdsp::ChipId::Snes, chipdsp::ChipId::Genesis },
                              Switch { chipdsp::ChipId::Genesis, chipdsp::ChipId::Nes });
    const bool noteOnNewChip = GENERATE (false, true);
    INFO ("switch " << chipdsp::chipKey (sw.from) << " -> " << chipdsp::chipKey (sw.to)
                    << ", new note on the incoming chip: " << (noteOnNewChip ? "yes" : "no"));
    const float fromChip = static_cast<float> (static_cast<int> (sw.from));
    const float toChip = static_cast<float> (static_cast<int> (sw.to));

    bool crossfadingAfterSwitch = false;
    bool crossfadingAtEnd = true;
    chipdsp::ChipId activeAtEnd = sw.from;

    const auto start = [&] (rcv::RetroChipProcessor& proc)
    {
        rcvtest::prepareAudibleDefaults (proc);
        rcvtest::setRaw (proc, rcv::ParamIds::chip, fromChip);
    };

    const auto switched = renderMain (kBlocks, [&] (rcv::RetroChipProcessor& proc, rcvtest::Runner& runner, int b)
    {
        if (b == 0)
            start (proc);
        if (b == kNoteBlock)
            runner.noteOn (1, 60, 100);
        if (b == kSwitchBlock)
        {
            rcvtest::setRaw (proc, rcv::ParamIds::chip, toChip);
            if (noteOnNewChip)
                runner.noteOn (1, 67, 100);
        }
        if (b == kSwitchBlock + 1)
            crossfadingAfterSwitch = proc.engineHost().isCrossfading();
        if (b == kBlocks - 1)
        {
            crossfadingAtEnd = proc.engineHost().isCrossfading();
            activeAtEnd = proc.engineHost().activeChip();
        }
    });

    const auto outgoing = renderMain (kBlocks, [&] (rcv::RetroChipProcessor& proc, rcvtest::Runner& runner, int b)
    {
        if (b == 0)
            start (proc);
        if (b == kNoteBlock)
            runner.noteOn (1, 60, 100);
        if (b == kSwitchBlock)
            runner.noteOff (1, 60);
    }, kSwitchBlock, kSwitchBlock + fadeBlocks);

    const auto incoming = renderMain (kBlocks, [&] (rcv::RetroChipProcessor& proc, rcvtest::Runner& runner, int b)
    {
        if (b == 0)
            start (proc);
        if (b == kSwitchBlock)
        {
            rcvtest::setRaw (proc, rcv::ParamIds::chip, toChip);
            if (noteOnNewChip)
                runner.noteOn (1, 67, 100);
        }
    });

    REQUIRE (switched.size() == outgoing.size());
    REQUIRE (incoming.size() == outgoing.size());

    // Before the switch the processor renders the outgoing reference; without a note the
    // chip is silent (real engines: below one 16-bit LSB of output-stage settling).
    REQUIRE (maxAbs (outgoing, kSwitch / 2, kSwitch) > 1.0e-3f);   // the held note is audible
    for (size_t n = 0; n < kSwitch; ++n)
        if (std::abs (switched[n] - outgoing[n]) > 1.0e-6f)
            FAIL ("sample " << n << " differs before the switch");
    REQUIRE (maxAbs (incoming, static_cast<size_t> (kNoteBlock * kBlock), kSwitch) < 1.0e-4f);

    const size_t fadeEnd = kSwitch + static_cast<size_t> (fadeLength);
    float fadeMaxStep = 0.0f;
    float maxShapeError = 0.0f;
    for (size_t n = kSwitch; n < fadeEnd + 64; ++n)
    {
        const float a = incoming[n];
        const float b = outgoing[n];
        if (n < fadeEnd)
        {
            const float t = (static_cast<float> (n - kSwitch) + 1.0f) / static_cast<float> (fadeLength);
            const float expected = a + std::cos (t * kPi * 0.5f) * b;
            const float error = std::abs (switched[n] - expected);
            maxShapeError = std::max (maxShapeError, error);
            if (error > std::abs (b) * maxGainStep + 1.0e-5f)
                FAIL_CHECK ("sample " << n << ": " << switched[n] << " expected " << expected);
        }
        else if (std::abs (switched[n] - a) > 1.0e-6f)
        {
            FAIL_CHECK ("sample " << n << " after the fade: " << switched[n] << " expected " << a);
        }

        const float step = std::abs (switched[n] - switched[n - 1]);
        const float bound = std::abs (a - incoming[n - 1]) + std::abs (b - outgoing[n - 1])
                          + std::abs (outgoing[n - 1]) * maxGainStep * 1.05f + 1.0e-5f;
        fadeMaxStep = std::max (fadeMaxStep, step);
        if (step > bound)
            FAIL_CHECK ("sample " << n << ": step " << step << " exceeds " << bound);
    }

    // Whole-run threshold: both references' largest natural steps plus the outgoing peak
    // times the largest per-sample fade gain change.
    const float threshold = maxStep (outgoing, 1, outgoing.size()) + maxStep (incoming, 1, incoming.size())
                          + maxAbs (outgoing, 0, outgoing.size()) * maxGainStep + 1.0e-5f;
    const float runMaxStep = maxStep (switched, 1, switched.size());
    const float hardCutStep = std::abs (outgoing[kSwitch - 1]);
    WARN ("chip switch " << chipdsp::chipKey (sw.from) << " -> " << chipdsp::chipKey (sw.to) << " ("
          << (noteOnNewChip ? "with" : "without") << " new note): max step " << runMaxStep
          << " over the run, " << fadeMaxStep << " around the fade, threshold " << threshold
          << "; fade gain step <= " << maxGainStep << ", max deviation from the cos fade " << maxShapeError
          << ", a hard cut would drop " << hardCutStep << " in one sample");
    CHECK (runMaxStep <= threshold);

    CHECK (crossfadingAfterSwitch);
    CHECK_FALSE (crossfadingAtEnd);
    CHECK (activeAtEnd == sw.to);

    // Nothing plays on the incoming chip without a note: the real output stages may settle
    // by less than one 16-bit LSB (3e-5) after their reset, so "silent" is below -80 dBFS.
    if (! noteOnNewChip)
        CHECK (maxAbs (switched, fadeEnd, switched.size()) < 1.0e-4f);
    else
        CHECK (maxAbs (switched, fadeEnd, switched.size()) > 1.0e-3f);
}

TEST_CASE ("MIDI channel mode routes MIDI channel N to hardware channel N-1", "[enginehost][routing]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::setRaw (*proc, rcv::ParamIds::voiceMode, static_cast<float> (rcv::VoiceMode::MidiChannel));
    rcvtest::enableChannelBuses (*proc, 5);   // NES: Out 1 Pulse 1 ... Out 5 DMC
    rcvtest::Runner runner (*proc, kSampleRate, kBlock);

    std::array<double, 6> energy {};
    double mainEnergy = 0.0;
    for (int b = 0; b < 30; ++b)
    {
        if (b == 0)
            runner.noteOn (3, 64, 100);   // MIDI channel 3 -> hardware channel 2 (Triangle, Out 3)
        runner.process();
        for (int bus = 1; bus <= 5; ++bus)
        {
            energy[static_cast<size_t> (bus)] += rcvtest::rms (runner.busChannel (bus, 0), kBlock)
                                               + rcvtest::rms (runner.busChannel (bus, 1), kBlock);
        }
        mainEnergy += rcvtest::rms (runner.mainChannel (0), kBlock);
    }

    const auto& nes = proc->engineHost().engine (chipdsp::ChipId::Nes);
    CHECK (proc->engineHost().voiceAllocator().midiChannelMode());
    CHECK (nes.isChannelActive (2));
    CHECK (countActive (nes) == 1);
    CHECK (proc->engineHost().voiceAllocator().channelMidiChannel (2) == 3);
    CHECK (proc->engineHost().voiceAllocator().channelNote (2) == 64);

    CHECK (mainEnergy > 1.0e-3);
    for (int bus = 1; bus <= 5; ++bus)
    {
        INFO ("Out " << bus << " energy " << energy[static_cast<size_t> (bus)]);
        if (bus == 3)
            CHECK (energy[static_cast<size_t> (bus)] > 1.0e-3);
        else
            CHECK (energy[static_cast<size_t> (bus)] == 0.0);
    }

    // A second MIDI channel adds its own hardware channel; a channel beyond the chip's five
    // (MIDI 7 -> hardware 6) is ignored.
    runner.noteOn (1, 60, 100);
    runner.noteOn (7, 62, 100);
    for (int b = 0; b < 10; ++b)
        runner.process();
    CHECK (nes.isChannelActive (0));
    CHECK (nes.isChannelActive (2));
    CHECK (countActive (nes) == 2);
    CHECK (rcvtest::rms (runner.busChannel (1, 0), kBlock) > 1.0e-4);
    CHECK (rcvtest::rms (runner.busChannel (2, 0), kBlock) == 0.0);
    CHECK (rcvtest::rms (runner.busChannel (4, 0), kBlock) == 0.0);
    CHECK (rcvtest::rms (runner.busChannel (5, 0), kBlock) == 0.0);
}

TEST_CASE ("Poly mode never uses more channels than the poly_channels mask allows", "[enginehost][poly]")
{
    struct Case
    {
        chipdsp::ChipId chip;
        uint32_t mask;   // exactly three channels
    };
    const auto c = GENERATE (Case { chipdsp::ChipId::Snes, 0x015u },      // voices 1, 3, 5
                             Case { chipdsp::ChipId::Genesis, 0x221u },   // FM 1, FM 6, PSG noise
                             Case { chipdsp::ChipId::Nes, 0x007u });      // pulses + triangle (default)
    INFO ("chip " << chipdsp::chipKey (c.chip) << " mask " << c.mask);

    auto proc = rcvtest::makeProcessor();
    rcvtest::setRaw (*proc, rcv::ParamIds::chip, static_cast<float> (static_cast<int> (c.chip)));
    rcvtest::setRaw (*proc, rcv::ParamIds::polyChannels, static_cast<float> (c.mask));
    rcvtest::Runner runner (*proc, kSampleRate, kBlock);

    // Let the start-up switch away from NES finish.
    for (int b = 0; b < 20; ++b)
        runner.process();
    REQUIRE (proc->engineHost().activeChip() == c.chip);
    CHECK (proc->engineHost().voiceAllocator().enabledMask() == c.mask);
    CHECK (proc->engineHost().voiceAllocator().numEnabledChannels() == 3);

    const auto& engine = proc->engineHost().engine (c.chip);
    int maxActive = 0;
    int heldNotes = 0;
    for (int b = 0; b < 150; ++b)
    {
        // Two note-ons per block for 40 blocks (up to 80 held keys), then release them one by one.
        if (b < 40)
        {
            runner.noteOn (1, 40 + b, 90, 3);
            runner.noteOn (2, 41 + b, 90, 40);
            heldNotes += 2;
        }
        else if (b < 120)
        {
            const int i = b - 40;
            runner.noteOff (i % 2 == 0 ? 1 : 2, 40 + i / 2 + (i % 2), 10);
            --heldNotes;
        }
        runner.process();

        const int active = countActive (engine);
        maxActive = std::max (maxActive, active);
        for (int ch = 0; ch < engine.numChannels(); ++ch)
            if (engine.isChannelActive (ch) && (c.mask & (1u << ch)) == 0u)
                FAIL_CHECK ("block " << b << ": channel " << ch << " outside the mask is active");
        if (active > 3)
            FAIL_CHECK ("block " << b << ": " << active << " channels active");
        if (b >= 2 && b < 40)
            CHECK (active == 3);   // more keys held than channels: all three are busy
    }
    CHECK (maxActive == 3);
    CHECK (heldNotes == 0);
}

TEST_CASE ("Channel buses carry the hardware channels and Main still sums", "[enginehost][buses]")
{
    const int enabledBuses = GENERATE (5, 10);
    INFO ("enabled buses: Out 1..Out " << enabledBuses);

    constexpr int kBlocks = 40;
    const int notes[] = { 60, 64, 67, 71, 74 };

    auto run = [&] (int busesToEnable, std::vector<std::vector<float>>& busOut, std::vector<float>& mainOut,
                    std::unique_ptr<rcv::RetroChipProcessor>& owner)
    {
        owner = rcvtest::makeProcessor();
        rcvtest::setRaw (*owner, rcv::ParamIds::polyChannels, 31.0f);   // all five NES channels
        rcvtest::prepareAudibleDefaults (*owner);                       // a looped DMC sample for the fifth note
        rcvtest::enableChannelBuses (*owner, busesToEnable);
        rcvtest::Runner runner (*owner, kSampleRate, kBlock);
        REQUIRE (owner->getTotalNumOutputChannels() == 2 + 2 * busesToEnable);
        busOut.assign (static_cast<size_t> (2 * busesToEnable), {});
        for (int b = 0; b < kBlocks; ++b)
        {
            if (b == 0)
                for (int n : notes)
                    runner.noteOn (1, n, 100);
            runner.process();
            mainOut.insert (mainOut.end(), runner.mainChannel (0), runner.mainChannel (0) + kBlock);
            for (int bus = 1; bus <= busesToEnable; ++bus)
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float* p = runner.busChannel (bus, ch);
                    REQUIRE (p != nullptr);
                    auto& dst = busOut[static_cast<size_t> (2 * (bus - 1) + ch)];
                    dst.insert (dst.end(), p, p + kBlock);
                }
        }
    };

    std::vector<std::vector<float>> buses, noBuses;
    std::vector<float> mainWithBuses, mainWithoutBuses;
    std::unique_ptr<rcv::RetroChipProcessor> proc, ref;
    run (enabledBuses, buses, mainWithBuses, proc);
    run (0, noBuses, mainWithoutBuses, ref);

    const auto& nes = proc->engineHost().engine (chipdsp::ChipId::Nes);
    CHECK (countActive (nes) == 5);

    const auto total = static_cast<int> (mainWithBuses.size());
    for (int bus = 1; bus <= enabledBuses; ++bus)
    {
        INFO ("Out " << bus);
        const auto& l = buses[static_cast<size_t> (2 * (bus - 1))];
        const auto& r = buses[static_cast<size_t> (2 * (bus - 1) + 1)];
        if (bus <= nes.numChannels())
        {
            CHECK (rcvtest::rms (l.data(), total) > 1.0e-3);
            CHECK (rcvtest::rms (r.data(), total) > 1.0e-3);
        }
        else
        {
            CHECK (rcvtest::peak (l.data(), total) == 0.0f);   // NES has five channels
            CHECK (rcvtest::peak (r.data(), total) == 0.0f);
        }
    }

    // Enabling the buses does not change the main mix.
    REQUIRE (mainWithoutBuses.size() == mainWithBuses.size());
    CHECK (rcvtest::rms (mainWithBuses.data(), total) > 1.0e-3);
    float maxDiff = 0.0f;
    for (size_t i = 0; i < mainWithBuses.size(); ++i)
        maxDiff = std::max (maxDiff, std::abs (mainWithBuses[i] - mainWithoutBuses[i]));
    CHECK (maxDiff <= 1.0e-6f);

    // The stub engine mixes linearly, so there Main is the sum of the channel feeds. Real
    // engines may not (the NES mixer is non-linear), so this part only runs on the stub.
    if (dynamic_cast<const chipdsp::StubEngine*> (&nes) != nullptr)
    {
        float maxSumError = 0.0f;
        for (size_t i = 0; i < mainWithBuses.size(); ++i)
        {
            float sum = 0.0f;
            for (int bus = 1; bus <= std::min (enabledBuses, nes.numChannels()); ++bus)
                sum += buses[static_cast<size_t> (2 * (bus - 1))][i];
            maxSumError = std::max (maxSumError, std::abs (sum - mainWithBuses[i]));
        }
        INFO ("max |Main - sum(Out 1..5)| = " << maxSumError);
        CHECK (maxSumError <= 1.0e-4f);
    }
}

TEST_CASE ("Visualizer buffers receive the main and channel signals when enabled", "[enginehost][visualizer]")
{
    auto proc = rcvtest::makeProcessor();
    auto& scopes = proc->visualizer();
    CHECK_FALSE (scopes.isEnabled());
    CHECK (scopes.activeChannelCount() == 5);   // NES

    rcvtest::Runner runner (*proc, kSampleRate, kBlock);
    runner.noteOn (1, 60, 100);
    runner.process();
    CHECK (scopes.generation (rcv::VisualizerBuffers::kMainSlot) == 0u);   // disabled: nothing pushed

    scopes.setEnabled (true);
    for (int b = 0; b < 10; ++b)
        runner.process();
    CHECK (scopes.generation (rcv::VisualizerBuffers::kMainSlot) == 10u);
    CHECK (scopes.generation (rcv::VisualizerBuffers::channelSlot (0)) == 10u);

    std::vector<float> l (512), r (512);
    scopes.readLatest (rcv::VisualizerBuffers::kMainSlot, l.data(), r.data(), 512);
    CHECK (rcvtest::rms (l.data(), 512) > 1.0e-3);
    // The newest samples in the ring are the last block of the main output.
    for (int i = 0; i < kBlock; ++i)
        CHECK (l[static_cast<size_t> (512 - kBlock + i)] == runner.mainChannel (0)[i]);

    scopes.readLatest (rcv::VisualizerBuffers::channelSlot (0), l.data(), r.data(), 512);
    CHECK (rcvtest::rms (l.data(), 512) > 1.0e-3);   // Poly: the first note lands on channel 0
    scopes.readLatest (rcv::VisualizerBuffers::channelSlot (3), l.data(), r.data(), 512);
    CHECK (rcvtest::peak (l.data(), 512) == 0.0f);
}

TEST_CASE ("Host blocks longer than the prepared size are sliced without changing the result", "[enginehost][slicing]")
{
    constexpr int kPrepared = 64;
    constexpr int kHostBlock = 200;   // sliced as 64 + 64 + 64 + 8
    constexpr int kBlocks = 12;

    auto makePrepared = []
    {
        auto p = rcvtest::makeProcessor();
        rcvtest::setRaw (*p, rcv::ParamIds::polyChannels, 31.0f);
        rcvtest::enableChannelBuses (*p, 5);
        p->setRateAndBufferSizeDetails (kSampleRate, kPrepared);
        p->prepareToPlay (kSampleRate, kPrepared);
        return p;
    };
    auto sliced = makePrepared();
    auto pieces = makePrepared();
    const int numChannels = sliced->getTotalNumOutputChannels();
    REQUIRE (numChannels == 12);

    juce::AudioBuffer<float> big (numChannels, kHostBlock);
    juce::AudioBuffer<float> manual (numChannels, kHostBlock);
    double energy = 0.0;
    float maxDiff = 0.0f;

    for (int b = 0; b < kBlocks; ++b)
    {
        // Events in the first and third slices; the second note starts inside a later slice.
        juce::MidiBuffer midi;
        if (b == 0)
        {
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, static_cast<juce::uint8> (100)), 10);
            midi.addEvent (juce::MidiMessage::noteOn (1, 67, static_cast<juce::uint8> (100)), 130);
        }
        if (b == 6)
            midi.addEvent (juce::MidiMessage::noteOff (1, 60), 195);
        sliced->processBlock (big, midi);

        // The same block handed over as the slices the engine host should cut.
        for (int start = 0; start < kHostBlock; start += kPrepared)
        {
            const int n = std::min (kPrepared, kHostBlock - start);
            juce::MidiBuffer part;
            for (const auto meta : midi)
                if (meta.samplePosition >= start && meta.samplePosition < start + n)
                    part.addEvent (meta.getMessage(), meta.samplePosition - start);
            juce::AudioBuffer<float> view (manual.getArrayOfWritePointers(), numChannels, start, n);
            pieces->processBlock (view, part);
        }

        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < kHostBlock; ++i)
                maxDiff = std::max (maxDiff, std::abs (big.getSample (ch, i) - manual.getSample (ch, i)));
        energy += rcvtest::rms (big.getReadPointer (0), kHostBlock) + rcvtest::rms (big.getReadPointer (4), kHostBlock);
    }

    CHECK (energy > 1.0e-2);   // main and Out 2 carried sound
    CHECK (maxDiff <= 1.0e-6f);
}

TEST_CASE ("Master gain scales the main and channel outputs", "[enginehost][gain]")
{
    constexpr int kBlocks = 30;
    auto render = [] (float gainDb, std::vector<float>& mainOut, std::vector<float>& busOut)
    {
        auto proc = rcvtest::makeProcessor();
        rcvtest::setRaw (*proc, rcv::ParamIds::masterGain, gainDb);
        rcvtest::enableChannelBuses (*proc, 1);
        rcvtest::Runner runner (*proc, kSampleRate, kBlock);
        for (int b = 0; b < kBlocks; ++b)
        {
            if (b == 0)
                runner.noteOn (1, 57, 110);
            runner.process();
            mainOut.insert (mainOut.end(), runner.mainChannel (0), runner.mainChannel (0) + kBlock);
            busOut.insert (busOut.end(), runner.busChannel (1, 0), runner.busChannel (1, 0) + kBlock);
        }
    };

    std::vector<float> unityMain, unityBus, quietMain, quietBus;
    render (0.0f, unityMain, unityBus);
    render (-6.0f, quietMain, quietBus);

    // The gain is set before prepare(), so there is no ramp: every sample is scaled by 10^(-6/20).
    const float expectedGain = std::pow (10.0f, -6.0f / 20.0f);   // 0.501187
    float maxMainError = 0.0f;
    float maxBusError = 0.0f;
    for (size_t i = 0; i < unityMain.size(); ++i)
    {
        maxMainError = std::max (maxMainError, std::abs (quietMain[i] - expectedGain * unityMain[i]));
        maxBusError = std::max (maxBusError, std::abs (quietBus[i] - expectedGain * unityBus[i]));
    }
    CHECK (rcvtest::rms (unityMain.data(), static_cast<int> (unityMain.size())) > 1.0e-3);
    CHECK (maxMainError <= 1.0e-6f);
    CHECK (maxBusError <= 1.0e-6f);
}

TEST_CASE ("Sustain pedal holds note-offs until it is released", "[enginehost][sustain]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::setRaw (*proc, rcv::ParamIds::voiceMode, static_cast<float> (rcv::VoiceMode::MidiChannel));
    rcvtest::Runner runner (*proc, kSampleRate, kBlock);
    const auto& nes = proc->engineHost().engine (chipdsp::ChipId::Nes);

    runner.noteOn (1, 60, 100);
    runner.controller (1, 64, 127);   // pedal down
    runner.process();
    runner.noteOff (1, 60, 10);
    for (int b = 0; b < 40; ++b)      // 40 blocks = 53 ms, longer than the 50 ms stub release
        runner.process();
    CHECK (nes.isChannelActive (0));    // still held by the pedal
    CHECK_FALSE (proc->engineHost().voiceAllocator().isChannelHeld (0));

    runner.controller (1, 64, 0);      // pedal up: the held note-off is applied
    for (int b = 0; b < 60; ++b)
        runner.process();
    CHECK_FALSE (nes.isChannelActive (0));
}
