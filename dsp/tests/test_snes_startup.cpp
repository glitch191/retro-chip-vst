// Regression guard: after prepare()/reset() an idle engine must not produce a DC step
// (a click through the output coupling stage).

#include "chipdsp/snes/SnesDspEngine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

TEST_CASE("SNES: an idle engine is silent after reset", "[snes][startup]")
{
    chipdsp::SnesDspEngine engine;
    engine.prepare(48000.0, 512);
    engine.reset();
    std::vector<float> l(512), r(512);
    float peak = 0.0f;
    for (int block = 0; block < 20; ++block)
    {
        engine.renderBlock(l.data(), r.data(), nullptr, nullptr, 512);
        for (int i = 0; i < 512; ++i)
            peak = std::max({ peak, std::abs(l[static_cast<size_t>(i)]), std::abs(r[static_cast<size_t>(i)]) });
    }
    INFO("peak " << peak);
    REQUIRE(peak < 1e-4f); // -80 dBFS
}
