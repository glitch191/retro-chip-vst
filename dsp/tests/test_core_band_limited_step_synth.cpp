#include "chipdsp/util/BandLimitedStepSynth.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

using chipdsp::BandLimitedStepSynth;
using Catch::Approx;

namespace
{
    // Goertzel magnitude of one bin (normalised so a full-scale sine returns its amplitude).
    double toneAmplitude(const std::vector<float>& x, double freqHz, double sampleRate)
    {
        const double w = 2.0 * std::numbers::pi * freqHz / sampleRate;
        double re = 0.0, im = 0.0;
        for (size_t n = 0; n < x.size(); ++n)
        {
            re += x[n] * std::cos(w * static_cast<double>(n));
            im -= x[n] * std::sin(w * static_cast<double>(n));
        }
        return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(x.size());
    }
} // namespace

TEST_CASE("BandLimitedStepSynth: a single step settles exactly to its delta", "[resampler]")
{
    BandLimitedStepSynth synth;
    synth.prepare(1789773.0, 48000.0, 512);

    std::vector<float> out(512, 0.0f);
    synth.addDelta(100.3, 0.75f);
    synth.endBlock(out.data(), 512);

    // Before the step: silence. Long after: the step level (within float rounding).
    REQUIRE(std::abs(out[50]) < 1e-6f);
    REQUIRE(out[511] == Approx(0.75f).margin(1e-4));
    REQUIRE(synth.currentLevel() == Approx(0.75f).margin(1e-4));
}

TEST_CASE("BandLimitedStepSynth: kernel has unit DC gain for every phase", "[resampler]")
{
    BandLimitedStepSynth synth;
    synth.prepare(32000.0, 44100.0, 256);

    for (int phase = 0; phase < BandLimitedStepSynth::kPhases; ++phase)
    {
        synth.reset();
        std::vector<float> out(256, 0.0f);
        const double t = 60.0 + static_cast<double>(phase) / BandLimitedStepSynth::kPhases;
        synth.addDelta(t, 1.0f);
        synth.endBlock(out.data(), 256);
        REQUIRE(out[255] == Approx(1.0f).margin(1e-4));
    }
}

TEST_CASE("BandLimitedStepSynth: 1 kHz square wave keeps its fundamental and rejects aliases", "[resampler]")
{
    // A 50 % square wave of amplitude 1 has fundamental amplitude 4/pi.
    const double native = 1789773.0;
    const double host = 48000.0;
    const double freq = 1000.0;
    BandLimitedStepSynth synth;
    synth.prepare(native, host, 4800);

    const int numSamples = 4800; // 100 ms
    std::vector<float> out(numSamples, 0.0f);

    const double halfPeriodClocks = native / freq / 2.0;
    double tClock = 0.0;
    float level = -1.0f;
    synth.addDelta(0.0, level); // start at -1 (single step, tolerated by the test window)
    while (tClock < numSamples / synth.hostSamplesPerClock())
    {
        tClock += halfPeriodClocks;
        level = -level;
        synth.addDelta(tClock * synth.hostSamplesPerClock(), 2.0f * level);
    }
    synth.endBlock(out.data(), numSamples);

    // Skip the initial transient.
    std::vector<float> steady(out.begin() + 480, out.end());
    const double fundamental = toneAmplitude(steady, freq, host);
    REQUIRE(fundamental == Approx(4.0 / std::numbers::pi).epsilon(0.03));

    // Harmonic 47 (47 kHz) folds to 1 kHz; harmonic 49 folds to 1 kHz as well. A naive
    // decimation would leak roughly 4/(47 pi) ~ 0.027 there. Check a clean bin instead:
    // 22.5 kHz has no odd harmonic and must be far below the fundamental.
    const double quiet = toneAmplitude(steady, 22500.0, host);
    REQUIRE(quiet < 0.01);
}

TEST_CASE("BandLimitedStepSynth: raw mode is an exact zero-order hold", "[resampler]")
{
    BandLimitedStepSynth synth;
    synth.prepare(1789773.0, 48000.0, 64);
    synth.setRaw(true);

    std::vector<float> out(64, 0.0f);
    synth.addDelta(10.2, 1.0f);   // level becomes 1 at t=10.2 -> first affected sample is 11
    synth.addDelta(20.0, -0.5f);  // level becomes 0.5 at t=20.0 -> sample 20 already sees 0.5
    synth.endBlock(out.data(), 64);

    REQUIRE(out[10] == 0.0f);
    REQUIRE(out[11] == 1.0f);
    REQUIRE(out[19] == 1.0f);
    REQUIRE(out[20] == 0.5f);
    REQUIRE(out[63] == 0.5f);
}

TEST_CASE("BandLimitedStepSynth: level carries across blocks", "[resampler]")
{
    BandLimitedStepSynth synth;
    synth.prepare(53267.0, 48000.0, 128);

    std::vector<float> a(128, 0.0f), b(128, 0.0f);
    synth.addDelta(120.0, 0.5f); // tail spills into the next block
    synth.endBlock(a.data(), 128);
    synth.endBlock(b.data(), 128);
    REQUIRE(b[127] == Approx(0.5f).margin(1e-4));
}

namespace
{
    // Fundamental of a +-1 square wave of frequency f built from band-limited steps, relative to
    // the ideal 4/pi, in dB. 4410 samples at 44.1 kHz = an integer number of periods for f in 10 Hz steps.
    double squareFundamentalDb(BandLimitedStepSynth::Kernel kernel, double freq)
    {
        const double host = 44100.0;
        const int n = 4410;
        const int lead = 441;
        BandLimitedStepSynth synth;
        synth.prepare(1789773.0, host, lead + n + 64, kernel);
        std::vector<float> out(static_cast<size_t>(lead + n), 0.0f);
        const double half = host / freq / 2.0;
        float level = 1.0f;
        synth.addDelta(0.0, level);
        for (double t = half; t < lead + n + 32; t += half)
        {
            synth.addDelta(t, -2.0f * level);
            level = -level;
        }
        synth.endBlock(out.data(), lead + n);
        const std::vector<float> steady(out.begin() + lead, out.end());
        return 20.0 * std::log10(toneAmplitude(steady, freq, host) / (4.0 / std::numbers::pi));
    }
} // namespace

TEST_CASE("BandLimitedStepSynth: integrated-step kernel settles exactly and is centred on the event", "[resampler]")
{
    BandLimitedStepSynth synth;
    for (double native : { 32000.0, 53267.0, 223722.0 })
    {
        synth.prepare(native, 44100.0, 256, BandLimitedStepSynth::Kernel::IntegratedStep);
        for (int phase = 0; phase < BandLimitedStepSynth::kPhases; ++phase)
        {
            synth.reset();
            std::vector<float> out(256, 0.0f);
            synth.addDelta(60.0 + static_cast<double>(phase) / BandLimitedStepSynth::kPhases, 1.0f);
            synth.endBlock(out.data(), 256);
            REQUIRE(out[40] == Approx(0.0f).margin(1e-6));
            REQUIRE(out[255] == Approx(1.0f).margin(1e-6));
        }
        // A step at t = 60.5 is symmetric about 60.5: S(-0.5) + S(0.5) = 1.
        synth.reset();
        std::vector<float> out(256, 0.0f);
        synth.addDelta(60.5, 1.0f);
        synth.endBlock(out.data(), 256);
        CHECK(out[60] + out[61] == Approx(1.0f).margin(1e-5));
        CHECK(out[60] < 0.5f);
    }
}

TEST_CASE("BandLimitedStepSynth: integrated-step kernel is flat, impulse-sum kernel boosts by (w/2)/sin(w/2)", "[resampler]")
{
    // refcheck-report.md finding F2: sampled impulses integrated by a discrete running sum give
    // H(w) / (1 - e^-jw) instead of H(w) / (jw). The integrated-step kernel removes that; the
    // impulse-sum kernel (kept for the NES) keeps it.
    for (double f : { 1000.0, 5000.0, 11190.0, 13980.0 })
    {
        const double w = 2.0 * std::numbers::pi * f / 44100.0;
        const double boostDb = 20.0 * std::log10((w / 2.0) / std::sin(w / 2.0));
        INFO("f = " << f << " Hz, formula boost " << boostDb << " dB");
        CHECK(std::abs(squareFundamentalDb(BandLimitedStepSynth::Kernel::IntegratedStep, f)) < 0.05);
        CHECK(squareFundamentalDb(BandLimitedStepSynth::Kernel::ImpulseSum, f) == Approx(boostDb).margin(0.05));
    }
}
