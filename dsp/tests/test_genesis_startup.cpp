// Regression: after prepare()/reset() the idle chip output (ladder-effect offsets on the
// silent FM channels, the PSG's unipolar levels) must not reach the output as a step. It
// used to produce a -37 dB click lasting ~150 ms at every reset and bus activation.

#include "chipdsp/genesis/GenesisEngine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using chipdsp::GenesisEngine;

namespace
{
    float peakOf(const std::vector<float>& v)
    {
        float p = 0.0f;
        for (float x : v)
            p = std::max(p, std::abs(x));
        return p;
    }
} // namespace

TEST_CASE("Genesis: an idle engine is silent after reset, for both chip revisions", "[genesis][startup]")
{
    for (int revision = 0; revision <= 1; ++revision)
    {
        GenesisEngine engine;
        engine.setParameter(GenesisEngine::ChipRevision, static_cast<float>(revision));
        engine.prepare(48000.0, 512);
        engine.reset();

        std::vector<float> l(512), r(512);
        float peak = 0.0f;
        for (int block = 0; block < 20; ++block)
        {
            engine.renderBlock(l.data(), r.data(), nullptr, nullptr, 512);
            peak = std::max({ peak, peakOf(l), peakOf(r) });
        }
        INFO("revision " << revision << " peak " << peak);
        REQUIRE(peak < 1e-4f); // -80 dBFS
    }
}

TEST_CASE("Genesis: enabling the per-channel outputs mid-stream does not click", "[genesis][startup]")
{
    GenesisEngine engine;
    engine.prepare(48000.0, 256);
    engine.reset();

    std::vector<float> l(256), r(256);
    engine.renderBlock(l.data(), r.data(), nullptr, nullptr, 256);

    std::array<std::vector<float>, chipdsp::kGenesisChannels> chL, chR;
    std::array<float*, chipdsp::kGenesisChannels> pl{}, pr{};
    for (int c = 0; c < chipdsp::kGenesisChannels; ++c)
    {
        chL[static_cast<size_t>(c)].assign(256, 0.0f);
        chR[static_cast<size_t>(c)].assign(256, 0.0f);
        pl[static_cast<size_t>(c)] = chL[static_cast<size_t>(c)].data();
        pr[static_cast<size_t>(c)] = chR[static_cast<size_t>(c)].data();
    }
    float peak = 0.0f;
    for (int block = 0; block < 20; ++block)
    {
        engine.renderBlock(l.data(), r.data(), pl.data(), pr.data(), 256);
        for (int c = 0; c < chipdsp::kGenesisChannels; ++c)
            peak = std::max({ peak, peakOf(chL[static_cast<size_t>(c)]), peakOf(chR[static_cast<size_t>(c)]) });
    }
    INFO("per-channel peak " << peak);
    REQUIRE(peak < 1e-4f);
}
