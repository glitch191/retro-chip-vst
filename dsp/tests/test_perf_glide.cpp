#include "chipdsp/perf/Glide.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using chipdsp::Glide;
using chipdsp::kMaxHardwareChannels;

TEST_CASE("Glide: reaches the target in the configured time, linearly in semitones", "[perf][glide]")
{
    // 100 ms at 48 kHz = 4800 samples. C4 (60) -> C5 (72): 12 semitones, so a quarter of the
    // ramp (1200 samples) adds 3 semitones and half of it (2400) adds 6.
    Glide glide;
    glide.setParams({ 100.0f, Glide::Mode::Always });

    glide.setTarget(0, 60.0f, false); // first note on the channel: jumps
    REQUIRE(glide.currentNote(0) == 60.0f);
    REQUIRE_FALSE(glide.isGliding(0));

    glide.setTarget(0, 72.0f, false); // Always: glides even without legato
    REQUIRE(glide.isGliding(0));
    REQUIRE(glide.currentNote(0) == 60.0f); // nothing has elapsed yet
    REQUIRE(glide.targetNote(0) == 72.0f);

    glide.advance(1200, 48000.0);
    REQUIRE(glide.currentNote(0) == Approx(63.0f).margin(1e-5));
    glide.advance(1200, 48000.0);
    REQUIRE(glide.currentNote(0) == Approx(66.0f).margin(1e-5));
    REQUIRE(glide.isGliding(0));

    // 4799 samples in: still 1 sample short of the target.
    glide.advance(2399, 48000.0);
    REQUIRE(glide.currentNote(0) == Approx(60.0 + 12.0 * 4799.0 / 4800.0).margin(1e-5));
    REQUIRE(glide.isGliding(0));

    // Exactly 4800 samples: on target, ramp finished.
    glide.advance(1, 48000.0);
    REQUIRE(glide.currentNote(0) == 72.0f);
    REQUIRE_FALSE(glide.isGliding(0));

    // Stays there.
    glide.advance(10000, 48000.0);
    REQUIRE(glide.currentNote(0) == 72.0f);
}

TEST_CASE("Glide: time is in milliseconds whatever the sample rate", "[perf][glide]")
{
    // 50 ms at 44.1 kHz = 2205 samples.
    Glide glide;
    glide.setTimeMs(50.0f);
    glide.setMode(Glide::Mode::Always);
    glide.setTarget(2, 48.0f, false);
    glide.setTarget(2, 36.0f, false); // downward glide, 12 semitones

    glide.advance(2204, 44100.0);
    REQUIRE(glide.isGliding(2));
    REQUIRE(glide.currentNote(2) == Approx(48.0 - 12.0 * 2204.0 / 2205.0).margin(1e-5));
    glide.advance(1, 44100.0);
    REQUIRE(glide.currentNote(2) == 36.0f);
    REQUIRE_FALSE(glide.isGliding(2));

    // The ramp is fixed-time: one semitone takes as long as an octave.
    glide.setTarget(2, 37.0f, false);
    glide.advance(441, 44100.0); // 10 ms of 50 ms = 20 %
    REQUIRE(glide.currentNote(2) == Approx(36.2f).margin(1e-5));
}

TEST_CASE("Glide: LegatoOnly jumps when the previous note was released", "[perf][glide]")
{
    Glide glide;
    glide.setParams({ 100.0f, Glide::Mode::LegatoOnly });

    glide.setTarget(0, 60.0f, false);
    REQUIRE(glide.currentNote(0) == 60.0f);

    // The previous note was released before this one: no glide, immediate jump.
    glide.setTarget(0, 72.0f, false);
    REQUIRE_FALSE(glide.isGliding(0));
    REQUIRE(glide.currentNote(0) == 72.0f);
    glide.advance(100, 48000.0);
    REQUIRE(glide.currentNote(0) == 72.0f);

    // Legato (the previous note was still held): glides, 2400 of 4800 samples = halfway.
    glide.setTarget(0, 60.0f, true);
    REQUIRE(glide.isGliding(0));
    glide.advance(2400, 48000.0);
    REQUIRE(glide.currentNote(0) == Approx(66.0f).margin(1e-5));

    // A non-legato note during a ramp cancels it and jumps.
    glide.setTarget(0, 48.0f, false);
    REQUIRE_FALSE(glide.isGliding(0));
    REQUIRE(glide.currentNote(0) == 48.0f);
}

TEST_CASE("Glide: time 0 jumps immediately in both modes", "[perf][glide]")
{
    Glide glide;
    glide.setParams({ 0.0f, Glide::Mode::Always });
    glide.setTarget(0, 60.0f, false);
    glide.setTarget(0, 72.0f, true);
    REQUIRE(glide.currentNote(0) == 72.0f);
    REQUIRE_FALSE(glide.isGliding(0));

    glide.setMode(Glide::Mode::LegatoOnly);
    glide.setTarget(0, 65.0f, true);
    REQUIRE(glide.currentNote(0) == 65.0f);
    REQUIRE_FALSE(glide.isGliding(0));

    // Setting the time to 0 in the middle of a ramp finishes it on the next advance.
    glide.setTimeMs(100.0f);
    glide.setTarget(0, 77.0f, true);
    glide.advance(1200, 48000.0);
    REQUIRE(glide.currentNote(0) == Approx(68.0f).margin(1e-5));
    glide.setTimeMs(0.0f);
    glide.advance(1, 48000.0);
    REQUIRE(glide.currentNote(0) == 77.0f);
}

TEST_CASE("Glide: retargeting mid-ramp starts from the current pitch", "[perf][glide]")
{
    // 60 -> 72 over 4800 samples; at 2400 the pitch is 66. Retarget to 60: a fresh 4800-sample
    // ramp from 66, so 2400 later the pitch is 63 and 4800 later it is 60.
    Glide glide;
    glide.setParams({ 100.0f, Glide::Mode::Always });
    glide.setTarget(1, 60.0f, false);
    glide.setTarget(1, 72.0f, true);
    glide.advance(2400, 48000.0);
    REQUIRE(glide.currentNote(1) == Approx(66.0f).margin(1e-5));

    glide.setTarget(1, 60.0f, true);
    glide.advance(2400, 48000.0);
    REQUIRE(glide.currentNote(1) == Approx(63.0f).margin(1e-5));
    glide.advance(2400, 48000.0);
    REQUIRE(glide.currentNote(1) == 60.0f);
    REQUIRE_FALSE(glide.isGliding(1));

    // Same target as the current pitch: nothing to do.
    glide.setTarget(1, 60.0f, true);
    REQUIRE_FALSE(glide.isGliding(1));
}

TEST_CASE("Glide: channels are independent and invalid channels are ignored", "[perf][glide]")
{
    Glide glide;
    glide.setParams({ 100.0f, Glide::Mode::Always });

    for (int ch = 0; ch < kMaxHardwareChannels; ++ch)
    {
        glide.setTarget(ch, 60.0f, false);
        glide.setTarget(ch, 60.0f + static_cast<float>(ch), false); // channel 0 stays, others glide
    }
    REQUIRE_FALSE(glide.isGliding(0));
    for (int ch = 1; ch < kMaxHardwareChannels; ++ch)
        REQUIRE(glide.isGliding(ch));

    glide.advance(2400, 48000.0); // half of the 4800-sample ramp
    for (int ch = 0; ch < kMaxHardwareChannels; ++ch)
        REQUIRE(glide.currentNote(ch) == Approx(60.0f + 0.5f * static_cast<float>(ch)).margin(1e-5));

    // Out-of-range channels: no crash, neutral answers.
    glide.setTarget(-1, 80.0f, false);
    glide.setTarget(kMaxHardwareChannels, 80.0f, false);
    REQUIRE(glide.currentNote(-1) == 0.0f);
    REQUIRE(glide.currentNote(kMaxHardwareChannels) == 0.0f);
    REQUIRE_FALSE(glide.isGliding(kMaxHardwareChannels));

    // reset(): the next target on a channel jumps again.
    glide.reset();
    REQUIRE_FALSE(glide.isGliding(5));
    glide.setTarget(5, 90.0f, true);
    REQUIRE(glide.currentNote(5) == 90.0f);
    REQUIRE_FALSE(glide.isGliding(5));

    // setCurrent() forces the pitch and cancels a ramp.
    glide.setTarget(5, 100.0f, true);
    REQUIRE(glide.isGliding(5));
    glide.setCurrent(5, 42.0f);
    REQUIRE_FALSE(glide.isGliding(5));
    REQUIRE(glide.currentNote(5) == 42.0f);
    REQUIRE(glide.targetNote(5) == 42.0f);
}

TEST_CASE("Glide: parameters are clamped to their range", "[perf][glide]")
{
    Glide glide;
    glide.setTimeMs(5000.0f);
    REQUIRE(glide.params().timeMs == Glide::kMaxTimeMs);
    glide.setTimeMs(-10.0f);
    REQUIRE(glide.params().timeMs == 0.0f);
}
