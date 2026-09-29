// Engine host event handling: poly_channels chip default, event capacity (no dropped
// note-offs), out-of-range offsets, MidiBuffer order at one offset, channel-mode messages
// and a second chip switch during a running crossfade.

#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

namespace
{
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlock = 64;
    constexpr int kReleaseBlocks = 60;   // 80 ms, longer than the 50 ms stub release

    bool anyActive (const chipdsp::IChipEngine& engine)
    {
        for (int c = 0; c < engine.numChannels(); ++c)
            if (engine.isChannelActive (c))
                return true;
        return false;
    }

    void run (rcvtest::Runner& runner, int blocks)
    {
        for (int b = 0; b < blocks; ++b)
            runner.process();
    }
} // namespace

TEST_CASE ("poly_channels 0 follows the chip default on the first block after a switch", "[enginehost][poly]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::Runner runner (*proc, kSampleRate, kBlock);
    const auto& host = proc->engineHost();
    CHECK (rcvtest::getRaw (*proc, rcv::ParamIds::polyChannels) == 0.0f);

    runner.process();
    CHECK (host.voiceAllocator().enabledMask() == 0x07u);   // NES: pulses + triangle

    // Chip automation with no message loop in between: the switch block already uses the
    // new chip's default, and the plugin never writes poly_channels itself.
    rcvtest::setRaw (*proc, rcv::ParamIds::chip, 1.0f);
    runner.process();
    CHECK (host.activeChip() == chipdsp::ChipId::Snes);
    CHECK (host.voiceAllocator().enabledMask() == 0xFFu);

    run (runner, 20);   // let the fade finish
    rcvtest::setRaw (*proc, rcv::ParamIds::chip, 2.0f);
    runner.process();
    CHECK (host.activeChip() == chipdsp::ChipId::Genesis);
    CHECK (host.voiceAllocator().enabledMask() == 0x3Fu);   // FM 1..6
    CHECK (rcvtest::getRaw (*proc, rcv::ParamIds::polyChannels) == 0.0f);

    // An explicit mask is kept across chips (intersected with the chip's channels).
    rcvtest::setRaw (*proc, rcv::ParamIds::polyChannels, 768.0f);   // 0x300:   // Genesis PSG 3 + noise
    runner.process();
    CHECK (host.voiceAllocator().enabledMask() == 0x300u);
    run (runner, 20);
    rcvtest::setRaw (*proc, rcv::ParamIds::chip, 0.0f);   // NES has 5 channels: nothing left
    runner.process();
    CHECK (host.voiceAllocator().enabledMask() == 0x07u); // empty intersection -> chip default
}

TEST_CASE ("Dense MIDI never drops note-offs or sustain-off", "[enginehost][events]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::setRaw (*proc, rcv::ParamIds::polyChannels, 31.0f);   // all five NES channels
    // The DMC plays a one-shot sample to its end whatever the note-off: point it at an empty
    // slot (the start-up sample sits in slot 0) so only the envelopes decide activity.
    if (! rcvtest::usesStubEngines (*proc))
        rcvtest::setRaw (*proc, "nes_dmc_sample", 1.0f);
    rcvtest::Runner runner (*proc, kSampleRate, kBlock);
    const auto& host = proc->engineHost();
    const auto& nes = host.engine (chipdsp::ChipId::Nes);

    SECTION ("more notes in one block than one slice holds")
    {
        for (int i = 0; i < 100; ++i)
            runner.noteOn (1, 20 + i, 100, 0);
        for (int i = 0; i < 100; ++i)
            runner.noteOff (1, 20 + i, 1);
        runner.process();
        CHECK (host.voiceAllocator().numHeldNotes() == 0);
        run (runner, kReleaseBlocks);
        CHECK_FALSE (anyActive (nes));
    }

    SECTION ("a sustain-off after more control events than one slice holds")
    {
        runner.noteOn (1, 60, 100);
        runner.controller (1, 64, 127);
        runner.process();
        runner.noteOff (1, 60, 2);
        for (int i = 0; i < 600; ++i)   // a heavy pitch-bend stream
            runner.midi().addEvent (juce::MidiMessage::pitchWheel (1, 8192 + (i % 64)), 3 + i % 40);
        runner.controller (1, 64, 0, 50);
        runner.process();
        run (runner, kReleaseBlocks);
        CHECK_FALSE (anyActive (nes));
    }
}

TEST_CASE ("An event at the end of the block is applied, not dropped", "[enginehost][events]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::Runner runner (*proc, kSampleRate, kBlock);
    const auto& host = proc->engineHost();

    runner.noteOn (1, 60, 100, -5);   // before the block: first sample
    runner.process();
    CHECK (host.voiceAllocator().numHeldNotes() == 1);

    runner.noteOff (1, 60, kBlock);   // samplePosition == numSamples: last sample
    runner.process();
    CHECK (host.voiceAllocator().numHeldNotes() == 0);
    run (runner, kReleaseBlocks);
    CHECK_FALSE (anyActive (host.engine (chipdsp::ChipId::Nes)));
}

TEST_CASE ("Events at one sample offset apply in MidiBuffer order", "[enginehost][events]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::setRaw (*proc, rcv::ParamIds::voiceMode, static_cast<float> (rcv::VoiceMode::MidiChannel));
    rcvtest::Runner runner (*proc, kSampleRate, kBlock);
    const auto& nes = proc->engineHost().engine (chipdsp::ChipId::Nes);

    runner.noteOn (1, 60, 100);
    runner.process();
    REQUIRE (nes.isChannelActive (0));

    // Note-off first, then the pedal goes down at the same sample: the note was released
    // before the pedal, so the pedal does not hold it.
    runner.noteOff (1, 60, 10);
    runner.controller (1, 64, 127, 10);
    runner.process();
    run (runner, kReleaseBlocks);
    CHECK_FALSE (nes.isChannelActive (0));

    // Pedal still down: a note released now is held.
    runner.noteOn (1, 62, 100, 5);
    runner.process();
    runner.noteOff (1, 62, 5);
    runner.process();
    run (runner, kReleaseBlocks);
    CHECK (nes.isChannelActive (0));

    // Pedal up, then a new note at the same sample: the held note is released by the pedal
    // and the new note plays.
    runner.controller (1, 64, 0, 7);
    runner.noteOn (1, 64, 100, 7);
    runner.process();
    CHECK (proc->engineHost().voiceAllocator().channelNote (0) == 64);
    CHECK (proc->engineHost().voiceAllocator().isChannelHeld (0));
}

TEST_CASE ("Channel-mode messages act on their own MIDI channel", "[enginehost][events]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::setRaw (*proc, rcv::ParamIds::voiceMode, static_cast<float> (rcv::VoiceMode::MidiChannel));
    rcvtest::Runner runner (*proc, kSampleRate, kBlock);
    const auto& host = proc->engineHost();
    const auto& nes = host.engine (chipdsp::ChipId::Nes);

    runner.noteOn (1, 60, 100);
    runner.noteOn (2, 64, 100);
    runner.process();
    REQUIRE (host.voiceAllocator().isChannelHeld (0));
    REQUIRE (host.voiceAllocator().isChannelHeld (1));

    SECTION ("All Notes Off (123)")
    {
        runner.controller (2, 123, 0);
        runner.process();
        CHECK (host.voiceAllocator().isChannelHeld (0));
        CHECK_FALSE (host.voiceAllocator().isChannelHeld (1));
    }

    SECTION ("All Sound Off (120) cuts at once when no other channel sounds")
    {
        runner.controller (2, 120, 0);
        runner.process();
        CHECK (host.voiceAllocator().isChannelHeld (0));   // channel 1 untouched
        CHECK_FALSE (host.voiceAllocator().isChannelHeld (1));
        CHECK (nes.isChannelActive (1));                   // another channel sounds: normal release

        runner.controller (1, 120, 0);
        runner.process();
        CHECK_FALSE (host.voiceAllocator().isChannelHeld (0));
        CHECK_FALSE (anyActive (nes));                     // cut immediately, no release tail
    }

    SECTION ("Reset All Controllers (121)")
    {
        runner.controller (1, 64, 127);
        runner.controller (2, 64, 127);
        runner.process();
        runner.controller (2, 121, 0);   // releases channel 2's pedal only
        runner.noteOff (1, 60, 5);
        runner.noteOff (2, 64, 5);
        runner.process();
        run (runner, kReleaseBlocks);
        CHECK (nes.isChannelActive (0));   // still held by channel 1's pedal
        CHECK_FALSE (nes.isChannelActive (1));
    }
}

// NES plays a held note (Pulse 1 with a one-second release, so it is still audible when
// released); the chip switches to SNES at block 100 and to Genesis at block 105,
// in the middle of the 960-sample fade. SNES and Genesis play nothing, so the output must
// equal the single NES -> SNES switch sample for sample: the NES engine keeps fading along
// its cos curve and the second switch waits for the fade to end.
TEST_CASE ("A second chip switch during a fade does not cut the fading engine", "[enginehost][crossfade]")
{
    constexpr int kBlocks = 160;
    auto render = [] (bool secondSwitch, chipdsp::ChipId& activeAtEnd)
    {
        auto proc = rcvtest::makeProcessor();
        rcvtest::Runner runner (*proc, kSampleRate, kBlock);
        std::vector<float> out;
        for (int b = 0; b < kBlocks; ++b)
        {
            if (b == 0)
            {
                rcvtest::prepareAudibleDefaults (*proc);   // NES Pulse 1 release: the fade has a tail to shape
                runner.noteOn (1, 60, 100);
            }
            if (b == 100)
                rcvtest::setRaw (*proc, rcv::ParamIds::chip, 1.0f);
            if (b == 105 && secondSwitch)
                rcvtest::setRaw (*proc, rcv::ParamIds::chip, 2.0f);
            runner.process();
            out.insert (out.end(), runner.mainChannel (0), runner.mainChannel (0) + kBlock);
        }
        activeAtEnd = proc->engineHost().activeChip();
        return out;
    };

    chipdsp::ChipId singleEnd {}, doubleEnd {};
    const auto single = render (false, singleEnd);
    const auto twice = render (true, doubleEnd);
    REQUIRE (single.size() == twice.size());

    float fadingPeak = 0.0f;
    for (size_t n = 105 * kBlock; n < 110 * kBlock; ++n)
        fadingPeak = std::max (fadingPeak, std::abs (single[n]));
    REQUIRE (fadingPeak > 1.0e-3f);   // the NES note is still fading out at the second switch

    // The idle SNES and Genesis output stages of the real engines settle by less than one
    // 16-bit LSB after their reset, and that residue is faded differently in the two runs:
    // the tolerance is -80 dBFS (the NES tail being cut would differ by up to fadingPeak).
    float maxDiff = 0.0f;
    for (size_t n = 0; n < single.size(); ++n)
        maxDiff = std::max (maxDiff, std::abs (single[n] - twice[n]));
    CHECK (maxDiff <= 1.0e-4f);
    CHECK (singleEnd == chipdsp::ChipId::Snes);
    CHECK (doubleEnd == chipdsp::ChipId::Genesis);
}
