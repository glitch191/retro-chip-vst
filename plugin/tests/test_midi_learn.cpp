// MIDI learn: CC values reach the mapped parameter through processBlock() and the
// message-thread drain; the mapping survives the plugin state.

#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;

namespace
{
    // CC value v maps linearly to the normalised value v / 127.
    constexpr double kNorm0 = 0.0;
    constexpr double kNorm64 = 64.0 / 127.0;   // 0.50394
    constexpr double kNorm127 = 1.0;

    float sendControllerAndDrain (rcv::RetroChipProcessor& proc, rcvtest::Runner& runner, const juce::String& id,
                                  int midiChannel, int cc, int value)
    {
        runner.controller (midiChannel, cc, value, 5);
        runner.process();
        proc.midiLearn().drain();
        return proc.parameters().getParameter (id)->getValue();
    }
} // namespace

TEST_CASE ("MIDI learn maps CC values 0, 64 and 127 to normalised 0, 0.504 and 1", "[midilearn]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::Runner runner (*proc, 48000.0, 64);

    // A continuous parameter, so the normalised value is not snapped to a step.
    const juce::String target (rcv::ParamIds::masterGain);
    proc->midiLearn().setMapping (74, target);
    REQUIRE (proc->midiLearn().isMapped (target));

    CHECK_THAT (sendControllerAndDrain (*proc, runner, target, 1, 74, 0), WithinAbs (kNorm0, 1.0e-6));
    CHECK_THAT (sendControllerAndDrain (*proc, runner, target, 1, 74, 64), WithinAbs (kNorm64, 1.0e-4));
    CHECK_THAT (sendControllerAndDrain (*proc, runner, target, 1, 74, 127), WithinAbs (kNorm127, 1.0e-6));

    // Native value of 64: -24 + 0.50394 * 36 = -5.858 dB.
    CHECK_THAT (sendControllerAndDrain (*proc, runner, target, 9, 74, 64), WithinAbs (kNorm64, 1.0e-4));
    CHECK_THAT (rcvtest::getRaw (*proc, target), WithinAbs (-24.0 + kNorm64 * 36.0, 1.0e-3));

    // An unmapped CC changes nothing.
    const float before = proc->parameters().getParameter (target)->getValue();
    CHECK_THAT (sendControllerAndDrain (*proc, runner, target, 1, 75, 0), WithinAbs (before, 1.0e-7));

    // Nothing reaches the parameter before the message thread drains the FIFO.
    runner.controller (1, 74, 0);
    runner.process();
    CHECK_THAT (proc->parameters().getParameter (target)->getValue(), WithinAbs (before, 1.0e-7));
    proc->midiLearn().drain();
    CHECK_THAT (proc->parameters().getParameter (target)->getValue(), WithinAbs (kNorm0, 1.0e-6));
}

TEST_CASE ("MIDI learn mode binds the first CC and the map survives getState/setState", "[midilearn][state]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::Runner runner (*proc, 48000.0, 64);

    const juce::String gain (rcv::ParamIds::masterGain);
    const juce::String glide (rcv::ParamIds::glideTime);
    proc->midiLearn().setMapping (74, gain);

    proc->midiLearn().learn (glide);
    CHECK (proc->midiLearn().isLearning());
    CHECK (proc->midiLearn().learningParamId() == glide);

    runner.controller (2, 21, 127);
    runner.process();
    proc->midiLearn().drain();
    CHECK_FALSE (proc->midiLearn().isLearning());
    CHECK (proc->midiLearn().controllerForParam (glide) == 21);
    CHECK_THAT (proc->parameters().getParameter (glide)->getValue(), WithinAbs (1.0, 1.0e-6));

    // Reassigning a CC: CC 21 moves from glide_time to master_gain, which gives up CC 74.
    proc->midiLearn().setMapping (21, gain);
    CHECK (proc->midiLearn().controllerForParam (gain) == 21);
    CHECK (proc->midiLearn().paramForController (74).isEmpty());
    proc->midiLearn().setMapping (74, gain);
    CHECK (proc->midiLearn().controllerForParam (gain) == 74);
    proc->midiLearn().setMapping (21, glide);

    juce::MemoryBlock state;
    proc->getStateInformation (state);

    auto restored = rcvtest::makeProcessor();
    restored->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
    CHECK (restored->midiLearn().controllerForParam (gain) == 74);
    CHECK (restored->midiLearn().controllerForParam (glide) == 21);
    CHECK (restored->midiLearn().paramForController (74) == gain);

    // The restored map is live: CC 74 = 64 drives master_gain on the restored processor.
    rcvtest::Runner restoredRunner (*restored, 48000.0, 64);
    CHECK_THAT (sendControllerAndDrain (*restored, restoredRunner, gain, 1, 74, 64), WithinAbs (kNorm64, 1.0e-4));

    // Restoring a state without mappings clears the map.
    auto empty = rcvtest::makeProcessor();
    juce::MemoryBlock emptyState;
    empty->getStateInformation (emptyState);
    restored->setStateInformation (emptyState.getData(), static_cast<int> (emptyState.getSize()));
    CHECK (restored->midiLearn().controllerForParam (gain) == -1);
    CHECK (restored->midiLearn().controllerForParam (glide) == -1);
}

TEST_CASE ("A dense CC stream keeps the final value and the learn completes", "[midilearn]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::Runner runner (*proc, 48000.0, 64);
    const juce::String gain (rcv::ParamIds::masterGain);
    const juce::String glide (rcv::ParamIds::glideTime);

    // Learn while the message thread is "stalled": 2000 CC messages arrive before any drain,
    // more than any queue of the old design held. The learn and the last value must survive.
    proc->midiLearn().setMapping (21, gain);
    proc->midiLearn().learn (glide);
    for (int i = 0; i < 2000; ++i)
        runner.controller (1, 30, i % 128, i % 64);
    for (int i = 0; i < 1000; ++i)
        runner.controller (1, 21, i % 100, i % 64);
    runner.controller (1, 30, 127, 63);
    runner.controller (1, 21, 0, 63);
    runner.process();
    proc->midiLearn().drain();

    CHECK_FALSE (proc->midiLearn().isLearning());
    CHECK (proc->midiLearn().controllerForParam (glide) == 30);
    CHECK (proc->midiLearn().controllerForParam (gain) == 21);
    CHECK_THAT (proc->parameters().getParameter (glide)->getValue(), WithinAbs (kNorm127, 1.0e-6));
    CHECK_THAT (proc->parameters().getParameter (gain)->getValue(), WithinAbs (kNorm0, 1.0e-6));

    // A second learn moves CC 30 to master_gain; master_gain gives up CC 21 and glide_time
    // loses its mapping.
    proc->midiLearn().learn (gain);
    runner.controller (1, 30, 64);
    runner.process();
    proc->midiLearn().drain();
    CHECK (proc->midiLearn().controllerForParam (gain) == 30);
    CHECK (proc->midiLearn().paramForController (21).isEmpty());
    CHECK (proc->midiLearn().controllerForParam (glide) == -1);
    CHECK_THAT (proc->parameters().getParameter (gain)->getValue(), WithinAbs (kNorm64, 1.0e-4));
}

TEST_CASE ("Sustain and reserved controllers are not handed to MIDI learn", "[midilearn]")
{
    auto proc = rcvtest::makeProcessor();
    rcvtest::Runner runner (*proc, 48000.0, 64);

    const juce::String gain (rcv::ParamIds::masterGain);
    proc->midiLearn().learn (gain);
    runner.controller (1, 64, 127);   // sustain pedal: handled by the engine host
    runner.process();
    proc->midiLearn().drain();
    CHECK (proc->midiLearn().isLearning());
    CHECK (proc->midiLearn().controllerForParam (gain) == -1);
    proc->midiLearn().cancelLearn();
    CHECK_FALSE (proc->midiLearn().isLearning());
}
