// Non-linear mixer against the reference values of docs/research/nes.md ("Mixer", items 36-42b)
// and the output-stage coefficients (item 43).

#include "chipdsp/nes/NesApu.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace chipdsp::nes;
using Catch::Approx;

namespace
{
    struct Row
    {
        int p1, p2, t, n, d;
        double pulse, tnd, total, linear, lut;
    };

    // research "Mixer", reference table (gen_nes.py, exact formula, 6 decimals).
    constexpr Row kRows[] = {
        { 0, 0, 0, 0, 0, 0.000000, 0.000000, 0.000000, 0.000000, 0.000000 },
        { 15, 0, 0, 0, 0, 0.149377, 0.000000, 0.149377, 0.112800, 0.148816 },
        { 15, 15, 0, 0, 0, 0.258483, 0.000000, 0.258483, 0.225600, 0.257513 },
        { 0, 0, 15, 0, 0, 0.000000, 0.246412, 0.246412, 0.127650, 0.255477 },
        { 0, 0, 0, 15, 0, 0.000000, 0.174431, 0.174431, 0.074100, 0.179666 },
        { 0, 0, 0, 0, 127, 0.000000, 0.574264, 0.574264, 0.425450, 0.561346 },
        { 0, 0, 15, 15, 127, 0.000000, 0.741516, 0.741516, 0.627200, 0.742468 },
        { 15, 15, 15, 15, 127, 0.258483, 0.741516, 0.999999, 0.852800, 0.999980 },
        { 1, 0, 0, 0, 0, 0.011653, 0.000000, 0.011653, 0.007520, 0.011609 },
        { 0, 0, 1, 0, 0, 0.000000, 0.019189, 0.019189, 0.008510, 0.019936 },
        { 0, 0, 0, 1, 0, 0.000000, 0.012948, 0.012948, 0.004940, 0.013345 },
        { 0, 0, 0, 0, 1, 0.000000, 0.007027, 0.007027, 0.003350, 0.006700 },
        { 8, 8, 0, 0, 0, 0.157697, 0.000000, 0.157697, 0.120320, 0.157105 },
        { 0, 0, 8, 0, 0, 0.000000, 0.141611, 0.141611, 0.068080, 0.146959 },
        { 0, 0, 0, 8, 0, 0.000000, 0.098023, 0.098023, 0.039520, 0.100996 },
        { 0, 0, 0, 0, 64, 0.000000, 0.352179, 0.352179, 0.214400, 0.340879 },
        { 15, 15, 15, 15, 0, 0.258483, 0.373329, 0.631812, 0.427350, 0.643175 },
        { 0, 0, 15, 15, 0, 0.000000, 0.373329, 0.373329, 0.201750, 0.385662 },
        { 12, 12, 15, 10, 0, 0.218571, 0.333758, 0.552329, 0.357530, 0.562833 },
        { 15, 0, 15, 0, 127, 0.149377, 0.681321, 0.830698, 0.665900, 0.826685 },
        { 15, 15, 0, 0, 127, 0.258483, 0.574264, 0.832747, 0.651050, 0.818859 },
        { 0, 0, 15, 0, 127, 0.000000, 0.681321, 0.681321, 0.553100, 0.677869 },
        { 0, 0, 0, 15, 127, 0.000000, 0.648770, 0.648770, 0.499550, 0.641939 },
        { 0, 0, 15, 0, 64, 0.000000, 0.507211, 0.507211, 0.342050, 0.506402 },
    };

    // Tolerance: reference printed with 6 decimals, tables stored as float.
    constexpr double kTol = 1.0e-6;
} // namespace

TEST_CASE("NES mixer exact formula matches the 24 reference combinations", "[nes][mixer]")
{
    NesMixer mixer;
    mixer.build();
    for (const Row& r : kRows)
    {
        INFO("p1 " << r.p1 << " p2 " << r.p2 << " t " << r.t << " n " << r.n << " d " << r.d);
        REQUIRE(mixPulse(r.p1, r.p2) == Approx(r.pulse).margin(kTol));
        REQUIRE(mixTnd(r.t, r.n, r.d) == Approx(r.tnd).margin(kTol));
        REQUIRE(mixer.pulse(r.p1, r.p2) == Approx(r.pulse).margin(kTol));
        REQUIRE(mixer.tnd(r.t, r.n, r.d) == Approx(r.tnd).margin(kTol));
        REQUIRE(mixer.mix(r.p1, r.p2, r.t, false, r.n, r.d) == Approx(r.total).margin(2 * kTol));

        // The wiki's linear and lookup-table approximations are comparison values only.
        const double linear = 0.00752 * (r.p1 + r.p2) + 0.00851 * r.t + 0.00494 * r.n + 0.00335 * r.d;
        const int sp = r.p1 + r.p2;
        const int st = 3 * r.t + 2 * r.n + r.d;
        const double lut = (sp == 0 ? 0.0 : 95.52 / (8128.0 / sp + 100.0)) + (st == 0 ? 0.0 : 163.67 / (24329.0 / st + 100.0));
        REQUIRE(linear == Approx(r.linear).margin(kTol));
        REQUIRE(lut == Approx(r.lut).margin(kTol));
    }
}

TEST_CASE("NES mixer extreme points and channel interaction", "[nes][mixer]")
{
    NesMixer mixer;
    mixer.build();
    REQUIRE(mixer.mix(0, 0, 0, false, 0, 0) == 0.0f);                                 // item 36
    REQUIRE(mixer.mix(15, 15, 15, false, 15, 127) == Approx(0.999999).margin(kTol)); // item 39
    // Item 40: triangle step with the DMC at 127.
    REQUIRE(mixTnd(15, 0, 127) - mixTnd(0, 0, 127) == Approx(0.107057).margin(kTol));
    // Item 42b: noise step with the DMC at 127.
    REQUIRE(mixTnd(0, 15, 127) - mixTnd(0, 0, 127) == Approx(0.074507).margin(kTol));
    // Item 42a: ultrasonic triangle substitute 7.5 (A2).
    REQUIRE(mixTnd(7.5, 0, 0) == Approx(0.133499).margin(kTol));
    REQUIRE(mixer.mix(0, 0, 0, true, 0, 0) == Approx(0.133499).margin(kTol));
    REQUIRE(mixer.mix(0, 0, 3, true, 0, 0) == Approx(0.133499).margin(kTol)); // step value ignored while ultrasonic
}

TEST_CASE("NES mixer full pulse table and single-channel rows", "[nes][mixer]")
{
    constexpr double pulse[31] = {
        0.000000, 0.011653, 0.023026, 0.034129, 0.044972, 0.055563, 0.065912, 0.076026,
        0.085914, 0.095583, 0.105039, 0.114291, 0.123345, 0.132206, 0.140882, 0.149377,
        0.157697, 0.165849, 0.173836, 0.181663, 0.189336, 0.196860, 0.204237, 0.211473,
        0.218571, 0.225536, 0.232371, 0.239080, 0.245666, 0.252133, 0.258483,
    };
    constexpr double tri[16] = { 0.000000, 0.019189, 0.037923, 0.056218, 0.074088, 0.091549, 0.108614, 0.125297,
                                 0.141611, 0.157567, 0.173177, 0.188452, 0.203403, 0.218040, 0.232374, 0.246412 };
    constexpr double noise[16] = { 0.000000, 0.012948, 0.025688, 0.038224, 0.050562, 0.062707, 0.074662, 0.086433,
                                   0.098023, 0.109437, 0.120678, 0.131751, 0.142659, 0.153406, 0.163995, 0.174431 };
    constexpr int dmcIdx[9] = { 0, 1, 2, 32, 64, 96, 125, 126, 127 };
    constexpr double dmc[9] = { 0.000000, 0.007027, 0.013993, 0.197898, 0.352179, 0.475831, 0.568437, 0.571359, 0.574264 };

    NesMixer mixer;
    mixer.build();
    for (int s = 0; s <= 30; ++s)
    {
        const int p1 = s > 15 ? 15 : s;
        REQUIRE(mixer.pulse(p1, s - p1) == Approx(pulse[s]).margin(kTol));
    }
    for (int i = 0; i < 16; ++i)
    {
        REQUIRE(mixer.tnd(i, 0, 0) == Approx(tri[i]).margin(kTol));
        REQUIRE(mixer.tnd(0, i, 0) == Approx(noise[i]).margin(kTol));
    }
    for (int i = 0; i < 9; ++i)
        REQUIRE(mixer.tnd(0, 0, dmcIdx[i]) == Approx(dmc[i]).margin(kTol));
}

TEST_CASE("NES output stage one-pole coefficients", "[nes][mixer]")
{
    // Item 43.
    REQUIRE(NesOutputStage::coefficient(90.0, 48000.0) == Approx(0.988288).margin(1e-6));
    REQUIRE(NesOutputStage::coefficient(440.0, 48000.0) == Approx(0.944031).margin(1e-6));
    REQUIRE(NesOutputStage::coefficient(14000.0, 48000.0) == Approx(0.159998).margin(1e-6));
    REQUIRE(NesOutputStage::coefficient(90.0, 44100.0) == Approx(0.987259).margin(1e-6));
    REQUIRE(NesOutputStage::coefficient(440.0, 44100.0) == Approx(0.939235).margin(1e-6));
    REQUIRE(NesOutputStage::coefficient(14000.0, 44100.0) == Approx(0.136060).margin(1e-6));

    // The console chain removes DC entirely; without it only the DC blocker runs.
    NesOutputStage stage;
    stage.prepare(48000.0);
    float buf[4800];
    for (int i = 0; i < 40; ++i)
    {
        for (float& x : buf)
            x = 0.5f;
        stage.process(buf, 4800, true);
    }
    REQUIRE(std::abs(buf[4799]) < 1e-4f);

    // Console filter on: exactly the three NES-001 sections, research "Output stage" (fs = 48 kHz
    // coefficients 0.988288 / 0.944031 / 0.159998), no extra DC-blocking section.
    {
        NesOutputStage chain;
        chain.prepare(48000.0);
        float step[2400];
        for (float& x : step)
            x = 0.5f;
        chain.process(step, 2400, true);
        const double k90 = 0.988288, k440 = 0.944031, k14k = 0.159998;
        double in1 = 0.0, out1 = 0.0, in2 = 0.0, out2 = 0.0, lp = 0.0;
        for (int i = 0; i < 2400; ++i)
        {
            const double x = 0.5;
            out1 = k90 * (out1 + x - in1);
            in1 = x;
            out2 = k440 * (out2 + out1 - in2);
            in2 = out1;
            lp = (1.0 - k14k) * out2 + k14k * lp;
            REQUIRE(step[i] == Approx(lp).margin(2e-5));
        }
    }

    // Console filter off: only the DC blocker; a step of 0.5 decays as 0.5 * k^(n+1).
    NesOutputStage off;
    off.prepare(48000.0);
    for (float& x : buf)
        x = 0.5f;
    off.process(buf, 4800, false);
    const double k = NesOutputStage::coefficient(NesOutputStage::kDcBlockHz, 48000.0);
    REQUIRE(buf[4799] == Approx(0.5 * std::pow(k, 4800.0)).margin(1e-4));
}
