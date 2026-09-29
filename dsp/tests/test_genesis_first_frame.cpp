// Regression: a note keyed right before a driver frame tick used to lose its first software
// envelope step, because the tick advanced the envelope at the same instant. After a reset
// the first tick is at t = 0, so every short PSG drum lost half its length and a soft
// 2-frame hat was silent. GenesisDriver::deferFirstTick() keeps the first step for 0.5..1.5
// frames (one frame on average).

#include "chipdsp/genesis/GenesisEngine.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using chipdsp::GenesisEngine;

namespace
{
    double rms(const std::vector<float>& x, size_t begin, size_t end)
    {
        double s = 0.0;
        for (size_t i = begin; i < end; ++i)
            s += static_cast<double>(x[i]) * x[i];
        return std::sqrt(s / static_cast<double>(end - begin));
    }

    std::vector<float> renderNoiseHit(int decayFrames, int sustain)
    {
        GenesisEngine e;
        e.prepare(48000.0, 4800);
        e.setParameter(GenesisEngine::PsgnAtt, 0.0f);
        e.setParameter(GenesisEngine::PsgNoiseMode, 1.0f);   // white
        e.setParameter(GenesisEngine::PsgNoiseRate, 0.0f);
        e.setParameter(GenesisEngine::PsgSwAttack, 0.0f);
        e.setParameter(GenesisEngine::PsgSwDecay, static_cast<float>(decayFrames));
        e.setParameter(GenesisEngine::PsgSwSustain, static_cast<float>(sustain));
        e.setParameter(GenesisEngine::PsgSwRelease, 0.0f);
        e.reset();
        e.noteOn(9, 60.0f, 1.0f);
        std::vector<float> l(4800), r(4800);
        e.renderBlock(l.data(), r.data(), nullptr, nullptr, 4800);
        return l;
    }
} // namespace

TEST_CASE("Genesis: a 2-frame PSG hit keeps its first envelope step after reset", "[genesis][psg][driver]")
{
    const auto hit = renderNoiseHit(2, 0);        // full level for one frame, then half, then off
    const auto held = renderNoiseHit(0, 15);      // reference: full level sustained
    // First 14 ms (< one NTSC frame of 16.7 ms): the hit must be at the sustained level.
    const double first = rms(hit, 0, 672);
    const double reference = rms(held, 0, 672);
    INFO("first step rms " << first << ", sustained rms " << reference);
    REQUIRE(reference > 1e-4);
    REQUIRE(first > 0.9 * reference);
    // After three frames (50 ms) the noise is off; what remains is the coupling-capacitor tail
    // of the unipolar PSG output's DC, more than 20 dB down.
    REQUIRE(rms(hit, 2880, 4800) < 0.1 * reference);
}
