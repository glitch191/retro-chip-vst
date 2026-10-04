// Engine-level tests: parameters, driver register conversion, rendered fundamentals,
// per-channel outputs and real-time safety (no allocation on the audio path).

#include "chipdsp/nes/Nes2A03Engine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <new>
#include <numbers>
#include <set>
#include <string>
#include <vector>

using chipdsp::ClockStandard;
using chipdsp::Nes2A03Engine;
using chipdsp::ParamDesc;
using namespace chipdsp::nes;
using Catch::Approx;

// ----- allocation counter (global replacement, active only while gCountAllocations is set) ----

namespace
{
    std::atomic<bool> gCountAllocations { false };
    std::atomic<int> gAllocations { 0 };
} // namespace

void* operator new(std::size_t size)
{
    if (gCountAllocations.load())
        gAllocations.fetch_add(1);
    if (void* p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size)
{
    return ::operator new(size);
}
void operator delete(void* p) noexcept
{
    std::free(p);
}
void operator delete[](void* p) noexcept
{
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}

namespace
{
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 256;

    struct Rendered
    {
        std::vector<float> main;
        std::vector<std::vector<float>> channels;
    };

    Rendered render(Nes2A03Engine& e, int numSamples, bool withChannels = false)
    {
        Rendered r;
        r.main.assign(static_cast<size_t>(numSamples), 0.0f);
        std::vector<float> right(static_cast<size_t>(numSamples), 0.0f);
        r.channels.assign(5, std::vector<float>(static_cast<size_t>(numSamples), 0.0f));
        std::vector<float> rightCh(static_cast<size_t>(kBlock), 0.0f);
        for (int pos = 0; pos < numSamples; pos += kBlock)
        {
            const int n = std::min(kBlock, numSamples - pos);
            float* chL[5];
            float* chR[5];
            for (int c = 0; c < 5; ++c)
            {
                chL[c] = r.channels[static_cast<size_t>(c)].data() + pos;
                chR[c] = rightCh.data();
            }
            e.renderBlock(r.main.data() + pos, right.data() + pos, withChannels ? chL : nullptr,
                          withChannels ? chR : nullptr, n);
        }
        return r;
    }

    // Frequency from rising zero crossings (linear interpolation), skipping the first 'skip' samples.
    double measureFrequency(const std::vector<float>& x, int skip)
    {
        double first = -1.0, last = -1.0;
        int count = 0;
        for (size_t i = static_cast<size_t>(skip) + 1; i < x.size(); ++i)
        {
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            {
                const double frac = x[i - 1] / static_cast<double>(x[i - 1] - x[i]);
                const double t = static_cast<double>(i - 1) + frac;
                if (first < 0.0)
                    first = t;
                last = t;
                ++count;
            }
        }
        if (count < 2)
            return 0.0;
        return (count - 1) * kRate / (last - first);
    }

    double rms(const std::vector<float>& x, size_t from)
    {
        double s = 0.0;
        for (size_t i = from; i < x.size(); ++i)
            s += static_cast<double>(x[i]) * x[i];
        return std::sqrt(s / static_cast<double>(x.size() - from));
    }

    Nes2A03Engine* makeEngine(bool pal)
    {
        auto* e = new Nes2A03Engine();
        e->setParameter(Nes2A03Engine::Clock, pal ? 1.0f : 0.0f);
        e->setParameter(Nes2A03Engine::ConsoleFilter, 0.0f); // keep square edges clean for crossings
        e->prepare(kRate, kBlock);
        return e;
    }
} // namespace

TEST_CASE("NES engine parameter descriptors: every ENGINE_SPECS parameter with native bounds", "[nes][engine][params]")
{
    Nes2A03Engine e;
    const auto descs = e.parameterDescriptors();
    REQUIRE(descs.size() == static_cast<size_t>(Nes2A03Engine::NumParams));
    REQUIRE(descs.size() == 65);

    std::set<std::string> keys;
    for (size_t i = 0; i < descs.size(); ++i)
    {
        const ParamDesc& d = descs[i];
        INFO(d.key);
        REQUIRE(d.id == static_cast<int>(i));
        REQUIRE(keys.insert(d.key).second);
        REQUIRE(d.minValue <= d.defaultValue);
        REQUIRE(d.defaultValue <= d.maxValue);
        REQUIRE(d.isInteger);
        REQUIRE(e.getParameter(d.id) == d.defaultValue);
        if (d.choiceLabels != nullptr)
            for (int k = 0; k <= static_cast<int>(d.maxValue - d.minValue); ++k)
                REQUIRE(d.choiceLabels[k] != nullptr);
    }

    // Spot checks against ENGINE_SPECS.md ranges and defaults.
    auto find = [&](const char* key) -> const ParamDesc& {
        for (const auto& d : descs)
            if (std::string(d.key) == key)
                return d;
        FAIL("missing key " << key);
        return descs[0];
    };
    REQUIRE(find("clock").maxValue == 1.0f);
    REQUIRE(find("console_filter").defaultValue == 1.0f);
    REQUIRE(find("p1_duty").defaultValue == 2.0f);
    REQUIRE(find("p2_volume").defaultValue == 12.0f);
    REQUIRE(find("p1_vibrato_depth").maxValue == 31.0f);
    REQUIRE(find("p2_pitch_env_depth").minValue == -64.0f);
    REQUIRE(find("p1_transpose").minValue == -24.0f);
    REQUIRE(find("tri_linear_length").maxValue == 127.0f);
    REQUIRE(find("tri_attack_frames").maxValue == 8.0f);
    REQUIRE(find("nz_pitch_env_depth").maxValue == 15.0f);
    REQUIRE(find("nz_period").maxValue == 15.0f);
    REQUIRE(find("dmc_direct_level").maxValue == 127.0f);
    REQUIRE(find("dmc_rate").maxValue == 15.0f);
    REQUIRE(find("frame_mode").maxValue == 1.0f);
    REQUIRE(find("frame_mode").defaultValue == 0.0f);

    // setParameter clamps to the hardware bounds and rounds to whole register values.
    e.setParameter(Nes2A03Engine::P1Duty, 7.0f);
    REQUIRE(e.getParameter(Nes2A03Engine::P1Duty) == 3.0f);
    e.setParameter(Nes2A03Engine::P2PitchEnvDepth, -100.0f);
    REQUIRE(e.getParameter(Nes2A03Engine::P2PitchEnvDepth) == -64.0f);
    e.setParameter(Nes2A03Engine::NzVolume, 6.6f);
    REQUIRE(e.getParameter(Nes2A03Engine::NzVolume) == 7.0f);
    e.setParameter(9999, 1.0f); // ignored
    REQUIRE(e.getParameter(9999) == 0.0f);

    REQUIRE(std::string(e.channelInfo(0).shortName) == "P1");
    REQUIRE(std::string(e.channelInfo(2).name) == "Triangle");
    REQUIRE(std::string(e.channelInfo(4).shortName) == "DMC");
    REQUIRE(e.numChannels() == 5);
    REQUIRE(e.nativeSampleRate() == kCpuHzNtsc);
    e.setClockStandard(ClockStandard::Pal);
    REQUIRE(e.nativeSampleRate() == kCpuHzPal);
}

TEST_CASE("NES driver converts MIDI notes to the documented timer values", "[nes][engine][driver]")
{
    for (bool pal : { false, true })
    {
        Nes2A03Engine* e = makeEngine(pal);
        e->noteOn(0, 69.0f, 1.0f);
        e->noteOn(1, 60.0f, 1.0f);
        e->noteOn(2, 69.0f, 1.0f);
        REQUIRE(e->apu().pulse1.timerPeriod() == (pal ? 235 : 253));  // items 16, 49
        REQUIRE(e->apu().pulse2.timerPeriod() == (pal ? 396 : 427));  // item 17
        REQUIRE(e->apu().triangle.timerPeriod() == (pal ? 117 : 126));
        delete e;
    }

    // Transpose is applied in semitones before the conversion.
    Nes2A03Engine* e = makeEngine(false);
    e->setParameter(Nes2A03Engine::P1Transpose, 12.0f);
    e->noteOn(0, 57.0f, 1.0f);
    REQUIRE(e->apu().pulse1.timerPeriod() == 253);
    delete e;
}

TEST_CASE("NES driver velocity mapping and hardware envelope selection", "[nes][engine][driver]")
{
    Nes2A03Engine* e = makeEngine(false);
    e->setParameter(Nes2A03Engine::P1Volume, 15.0f);
    e->noteOn(0, 69.0f, 1.0f);
    REQUIRE(e->apu().pulse1.volume() == 15);
    e->noteOn(0, 69.0f, 0.5f);
    REQUIRE(e->apu().pulse1.volume() == 8);  // round(0.5 * 15)
    e->setParameter(Nes2A03Engine::P1Volume, 12.0f);
    e->noteOn(0, 69.0f, 0.5f);
    REQUIRE(e->apu().pulse1.volume() == 6);  // round(0.5 * 12)

    // Hardware envelope: C = 0, the decay starts at 15 whatever the velocity.
    e->setParameter(Nes2A03Engine::P2EnvEnable, 1.0f);
    e->setParameter(Nes2A03Engine::P2Volume, 3.0f);
    e->noteOn(1, 69.0f, 0.2f);
    REQUIRE_FALSE(e->apu().pulse2.env().constant);
    REQUIRE(e->apu().pulse2.env().param == 3);
    render(*e, 2400);
    REQUIRE(e->apu().pulse2.volume() <= 15);
    REQUIRE(e->apu().pulse2.volume() > 0);
    delete e;
}

TEST_CASE("NES driver lowest-note rule: NTSC refuses MIDI 32, PAL plays it (A16)", "[nes][engine][driver]")
{
    Nes2A03Engine* ntsc = makeEngine(false);
    ntsc->noteOn(0, 32.0f, 1.0f);
    REQUIRE(ntsc->apu().pulse1.volume() == 0);
    const Rendered a = render(*ntsc, 24000);
    REQUIRE(rms(a.main, 4800) < 1e-4);
    ntsc->noteOn(0, 33.0f, 1.0f);
    REQUIRE(ntsc->apu().pulse1.timerPeriod() == 2033);
    REQUIRE(ntsc->apu().pulse1.volume() == 12);
    // Sweep off is written as $08 (negate, shift 0) so a period >= $400 is not muted.
    REQUIRE_FALSE(ntsc->apu().pulse1.isMuted());
    const Rendered low = render(*ntsc, 24000);
    REQUIRE(rms(low.main, 4800) > 0.01);
    delete ntsc;

    Nes2A03Engine* pal = makeEngine(true);
    pal->noteOn(0, 32.0f, 1.0f);
    REQUIRE(pal->apu().pulse1.timerPeriod() == 2001);
    REQUIRE(pal->apu().pulse1.volume() == 12);
    const Rendered b = render(*pal, 24000);
    REQUIRE(rms(b.main, 4800) > 0.01);
    delete pal;
}

TEST_CASE("NES driver helpers: pitch envelope, vibrato, keyed noise and DMC mapping", "[nes][engine][driver]")
{
    REQUIRE(NesDriver::pitchEnvelopeOffset(64, 4, 0) == 64);
    REQUIRE(NesDriver::pitchEnvelopeOffset(64, 4, 1) == 48);
    REQUIRE(NesDriver::pitchEnvelopeOffset(-64, 4, 2) == -32);
    REQUIRE(NesDriver::pitchEnvelopeOffset(64, 4, 4) == 0);
    REQUIRE(NesDriver::pitchEnvelopeOffset(64, 0, 0) == 0);

    REQUIRE(NesDriver::vibratoOffset(0, 10, 0, 5) == 0);
    REQUIRE(NesDriver::vibratoOffset(3, 4, 10, 9) == 0); // still in the delay
    int lo = 0, hi = 0;
    for (int f = 10; f < 40; ++f)
    {
        const int v = NesDriver::vibratoOffset(3, 4, 10, f);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    REQUIRE(lo == -4);
    REQUIRE(hi == 4);
    REQUIRE(NesDriver::vibratoOffset(1, 5, 0, 0) == 5);  // rate 1: alternates every frame
    REQUIRE(NesDriver::vibratoOffset(1, 5, 0, 1) == -5);
    // vibrato_depth is in period units for every rate, odd or even (ENGINE_SPECS).
    for (int rate = 1; rate <= 15; ++rate)
    {
        for (int depth : { 1, 8, 31 })
        {
            INFO("rate " << rate << " depth " << depth);
            int mn = 0, mx = 0;
            for (int f = 0; f < 2 * rate; ++f)
            {
                const int v = NesDriver::vibratoOffset(rate, depth, 0, f);
                mn = std::min(mn, v);
                mx = std::max(mx, v);
            }
            REQUIRE(mx == depth);
            REQUIRE(mn == -depth);
        }
    }
    // Rate 2 samples the triangle at 1/8, 3/8, 5/8, 7/8: +d, +d, -d, -d.
    REQUIRE(NesDriver::vibratoOffset(2, 8, 0, 0) == 8);
    REQUIRE(NesDriver::vibratoOffset(2, 8, 0, 1) == 8);
    REQUIRE(NesDriver::vibratoOffset(2, 8, 0, 2) == -8);
    REQUIRE(NesDriver::vibratoOffset(2, 8, 0, 3) == -8);
    // Rate 4: 1/3, 1, 1, 1/3 of the depth on the rising and falling quarter.
    REQUIRE(NesDriver::vibratoOffset(4, 9, 0, 0) == 3);
    REQUIRE(NesDriver::vibratoOffset(4, 9, 0, 1) == 9);

    // Item 51: note 36 + n -> period 15 - n, clamped.
    REQUIRE(NesDriver::noiseIndexForNote(36.0f) == 15);
    REQUIRE(NesDriver::noiseIndexForNote(37.0f) == 14);
    REQUIRE(NesDriver::noiseIndexForNote(51.0f) == 0);
    REQUIRE(NesDriver::noiseIndexForNote(20.0f) == 15);
    REQUIRE(NesDriver::noiseIndexForNote(80.0f) == 0);

    REQUIRE(NesDriver::dmcRateForNote(15, 60.0f) == 15);
    REQUIRE(NesDriver::dmcRateForNote(15, 59.0f) == 14);
    REQUIRE(NesDriver::dmcRateForNote(8, 64.0f) == 12);
    REQUIRE(NesDriver::dmcRateForNote(0, 40.0f) == 0);
    REQUIRE(NesDriver::dmcRateForNote(10, 80.0f) == 15);

    // Software envelope: attack 4 frames to 12, decay 4 frames to sustain 6/15 of the peak.
    SoftwareEnvelope env;
    env.start(12);
    std::vector<int> levels;
    for (int f = 0; f < 12; ++f)
    {
        levels.push_back(env.compute(4, 4, 6, 3));
        ++env.frame;
    }
    // Sustain = round(12 * 6 / 15) = 5; decay steps 12 - round(7 f / 4).
    const std::vector<int> expected = { 0, 3, 6, 9, 12, 10, 8, 7, 5, 5, 5, 5 };
    REQUIRE(levels == expected);
    env.release();
    REQUIRE(env.compute(4, 4, 6, 3) == 5);
    env.frame = 1;
    REQUIRE(env.compute(4, 4, 6, 3) == 3);
    env.frame = 3;
    REQUIRE(env.compute(4, 4, 6, 3) == 0);
    REQUIRE(env.stage == SoftwareEnvelope::Stage::Off);
}

TEST_CASE("NES driver keyed noise writes the period index; vibrato and sweep move the timer", "[nes][engine][driver]")
{
    Nes2A03Engine* e = makeEngine(false);
    e->setParameter(Nes2A03Engine::NzKeyed, 1.0f);
    e->noteOn(3, 36.0f, 1.0f);
    // Index 15 -> 4068 CPU cycles between LFSR clocks.
    uint16_t prev = e->apu().noise.shiftRegister();
    render(*e, 4800);
    REQUIRE(e->apu().noise.shiftRegister() != prev);

    // Vibrato rate 3, depth 4: the period swings 253 +/- 4 once the delay has passed.
    e->setParameter(Nes2A03Engine::P1VibratoRate, 3.0f);
    e->setParameter(Nes2A03Engine::P1VibratoDepth, 4.0f);
    e->noteOn(0, 69.0f, 1.0f);
    std::set<int> periods;
    for (int f = 0; f < 60; ++f)
    {
        render(*e, 800); // one video frame at 48 kHz is 798.7 samples
        periods.insert(e->apu().pulse1.timerPeriod());
    }
    REQUIRE(*periods.begin() == 249);
    REQUIRE(*periods.rbegin() == 257);

    // Hardware sweep: shift 1, not negated, divider period 0: the period grows every half frame
    // until the target passes $7FF and the channel mutes.
    e->setParameter(Nes2A03Engine::P2SweepEnable, 1.0f);
    e->setParameter(Nes2A03Engine::P2SweepShift, 1.0f);
    e->noteOn(1, 81.0f, 1.0f);   // t = 126
    REQUIRE(e->apu().pulse2.timerPeriod() == 126);
    render(*e, 2400);
    REQUIRE(e->apu().pulse2.timerPeriod() > 126);
    render(*e, 48000);
    REQUIRE(e->apu().pulse2.isMuted());
    delete e;
}

TEST_CASE("NES engine renders the expected fundamental (pulse and triangle, NTSC and PAL)", "[nes][engine][render]")
{
    struct Case
    {
        bool pal;
        int channel;
        double expectedHz;
    };
    // Item 49: MIDI 69 -> pulse t = 253 / 235, triangle t = 126 / 117.
    const Case cases[] = {
        { false, 0, 440.396900 },
        { true, 0, 440.309057 },
        { false, 2, 440.396900 },
        { true, 2, 440.309057 },
    };
    for (const Case& c : cases)
    {
        INFO("pal " << c.pal << " channel " << c.channel);
        Nes2A03Engine* e = makeEngine(c.pal);
        e->noteOn(c.channel, 69.0f, 1.0f);
        const Rendered r = render(*e, static_cast<int>(2.0 * kRate));
        const double f = measureFrequency(r.main, static_cast<int>(0.3 * kRate));
        REQUIRE(f == Approx(c.expectedHz).margin(0.02));
        REQUIRE(std::abs(f - 440.0) > 0.25); // the 11-bit quantisation is audible, not corrected
        delete e;
    }

    // MIDI 108 on NTSC: t = 26 -> 4142.993 Hz, 17.9 cents flat (item 50).
    Nes2A03Engine* e = makeEngine(false);
    e->noteOn(0, 108.0f, 1.0f);
    const Rendered r = render(*e, static_cast<int>(1.0 * kRate));
    REQUIRE(measureFrequency(r.main, 4800) == Approx(4142.993056).margin(0.5));
    delete e;
}

TEST_CASE("NES per-channel outputs: a channel alone matches the main output", "[nes][engine][render]")
{
    Nes2A03Engine* e = makeEngine(false);
    e->noteOn(0, 64.0f, 1.0f);
    const Rendered r = render(*e, 9600, true);
    double maxDiff = 0.0, peak = 0.0;
    for (size_t i = 0; i < r.main.size(); ++i)
    {
        maxDiff = std::max(maxDiff, static_cast<double>(std::abs(r.main[i] - r.channels[0][i])));
        peak = std::max(peak, static_cast<double>(std::abs(r.channels[0][i])));
    }
    REQUIRE(peak > 0.05);
    REQUIRE(maxDiff < 1e-4);
    // The silent channels stay silent.
    REQUIRE(rms(r.channels[1], 0) < 1e-6);
    REQUIRE(rms(r.channels[3], 0) < 1e-6);
    delete e;

    // Non-linearity: pulse 1 + pulse 2 in the main output is less than the sum of the two alone.
    e = makeEngine(false);
    e->setParameter(Nes2A03Engine::P1Volume, 15.0f);
    e->setParameter(Nes2A03Engine::P2Volume, 15.0f);
    e->noteOn(0, 69.0f, 1.0f);
    e->noteOn(1, 69.0f, 1.0f);
    const Rendered both = render(*e, 9600, true);
    const double mainRms = rms(both.main, 2400);
    const double sumRms = rms(both.channels[0], 2400) + rms(both.channels[1], 2400);
    REQUIRE(mainRms < 0.95 * sumRms);
    delete e;
}

TEST_CASE("NES engine channel activity follows the envelopes", "[nes][engine]")
{
    Nes2A03Engine* e = makeEngine(false);
    for (int c = 0; c < 5; ++c)
        REQUIRE_FALSE(e->isChannelActive(c));

    e->setParameter(Nes2A03Engine::P1SwRelease, 10.0f);
    e->noteOn(0, 69.0f, 1.0f);
    e->noteOn(2, 45.0f, 1.0f);
    render(*e, 4800);
    REQUIRE(e->isChannelActive(0));
    REQUIRE(e->isChannelActive(2));
    e->noteOff(0);
    e->noteOff(2);
    REQUIRE(e->isChannelActive(0));       // software release of 10 frames still running
    REQUIRE_FALSE(e->isChannelActive(2)); // triangle is silenced at once
    render(*e, 48000 / 60 * 12);
    REQUIRE_FALSE(e->isChannelActive(0));

    // Noise with the hardware envelope (no loop) decays to 0 by itself.
    e->setParameter(Nes2A03Engine::NzEnvEnable, 1.0f);
    e->setParameter(Nes2A03Engine::NzVolume, 1.0f); // 8.3 ms per step: 15 -> 0 in 0.125 s
    e->noteOn(3, 60.0f, 1.0f);
    e->noteOff(3);
    REQUIRE(e->isChannelActive(3));
    render(*e, 12000);
    REQUIRE_FALSE(e->isChannelActive(3));

    // DMC one-shot: active while the sample plays.
    std::vector<float> pcm(2205, 0.0f);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 200.0 * static_cast<double>(i) / 44100.0));
    REQUIRE(e->loadSample(0, pcm.data(), static_cast<int>(pcm.size()), 44100.0));
    e->noteOn(4, 60.0f, 1.0f);
    e->noteOff(4);
    render(*e, 256);
    REQUIRE(e->isChannelActive(4));
    render(*e, 9600); // 50 ms sample
    REQUIRE_FALSE(e->isChannelActive(4));
    delete e;
}

TEST_CASE("NES engine: no allocation on the audio path", "[nes][engine][realtime]")
{
    Nes2A03Engine* e = makeEngine(false);
    std::vector<float> pcm(4000, 0.25f);
    REQUIRE(e->loadSample(2, pcm.data(), 4000, 44100.0));
    e->setParameter(Nes2A03Engine::DmcSample, 2.0f);

    std::vector<float> l(kBlock), r(kBlock);
    std::vector<std::vector<float>> chl(5, std::vector<float>(kBlock)), chr(5, std::vector<float>(kBlock));
    float* pl[5];
    float* pr[5];
    for (int c = 0; c < 5; ++c)
    {
        pl[c] = chl[static_cast<size_t>(c)].data();
        pr[c] = chr[static_cast<size_t>(c)].data();
    }

    gAllocations.store(0);
    gCountAllocations.store(true);
    for (int c = 0; c < 5; ++c)
        e->noteOn(c, 60.0f + static_cast<float>(c), 0.8f);
    for (int b = 0; b < 200; ++b)
    {
        if (b == 50)
        {
            e->setParameter(Nes2A03Engine::P1Duty, 1.0f);
            e->setParameter(Nes2A03Engine::TriLinearLength, 20.0f);
            e->setChannelPitch(0, 62.5f);
        }
        if (b == 80)
            e->setClockStandard(ClockStandard::Pal);
        if (b == 100)
            e->setRawOutput(true);
        if (b == 120)
            for (int c = 0; c < 5; ++c)
                e->noteOff(c);
        e->renderBlock(l.data(), r.data(), (b % 2) == 0 ? pl : nullptr, (b % 2) == 0 ? pr : nullptr, kBlock);
    }
    e->reset();
    e->renderBlock(l.data(), r.data(), pl, pr, kBlock);
    gCountAllocations.store(false);
    REQUIRE(gAllocations.load() == 0);
    delete e;
}

TEST_CASE("NES driver: a released hardware-envelope note with vibrato decays out (A24)", "[nes][engine][driver]")
{
    Nes2A03Engine* e = makeEngine(false);
    e->setParameter(Nes2A03Engine::P1EnvEnable, 1.0f);
    e->setParameter(Nes2A03Engine::P1EnvLoop, 1.0f);
    e->setParameter(Nes2A03Engine::P1Volume, 7.0f); // 8 quarter frames per step: 15 -> 0 in 0.5 s
    e->setParameter(Nes2A03Engine::P1VibratoRate, 2.0f);
    e->setParameter(Nes2A03Engine::P1VibratoDepth, 31.0f);
    REQUIRE(pulsePeriodForNote(57.0, kCpuHzNtsc) == 507); // research period table, MIDI 57 = $1FB
    e->noteOn(0, 57.0f, 1.0f);

    // Key held: 507 +/- 31 crosses $200 ($4003 rewrites, as in games); rate 2 reaches the full
    // depth (vibrato even-rate scaling).
    std::set<int> held;
    for (int f = 0; f < 30; ++f)
    {
        render(*e, 800);
        held.insert(e->apu().pulse1.timerPeriod());
    }
    REQUIRE(*held.begin() == 476);
    REQUIRE(*held.rbegin() == 538);

    e->noteOff(0);
    const int page = e->apu().pulse1.timerPeriod() >> 8;
    render(*e, 400); // any envelope start set before the note-off has been served
    int prev = e->apu().pulse1.volume();
    for (int b = 0; b < 72; ++b) // 0.6 s
    {
        render(*e, 400);
        const int vol = e->apu().pulse1.volume();
        REQUIRE(vol <= prev);                                   // never restarted at 15
        REQUIRE((e->apu().pulse1.timerPeriod() >> 8) == page);  // no $4003 write after the release
        prev = vol;
    }
    REQUIRE(e->apu().pulse1.volume() == 0);
    REQUIRE_FALSE(e->isChannelActive(0));

    // A new pitch after the note-off does not restart the decay either.
    e->setParameter(Nes2A03Engine::P1VibratoDepth, 0.0f);
    e->noteOn(0, 57.0f, 1.0f);
    render(*e, 2400);
    e->noteOff(0);
    e->setChannelPitch(0, 45.0f); // t = 1016 = $3F8: other high bits
    render(*e, 400);
    prev = e->apu().pulse1.volume();
    for (int b = 0; b < 72; ++b)
    {
        render(*e, 400);
        REQUIRE(e->apu().pulse1.volume() <= prev);
        prev = e->apu().pulse1.volume();
    }
    REQUIRE_FALSE(e->isChannelActive(0));
    delete e;
}

TEST_CASE("NES engine channel activity: a held key on a silent channel is inactive", "[nes][engine]")
{
    // ENGINE_SPECS "Conventions": active until the envelope has fully released or the channel
    // was silenced.
    Nes2A03Engine* e = makeEngine(false);

    // Hardware envelope, loop off, V = 0: 15 quarter frames (62.5 ms) to 0, key held.
    e->setParameter(Nes2A03Engine::P1EnvEnable, 1.0f);
    e->setParameter(Nes2A03Engine::P1Volume, 0.0f);
    e->noteOn(0, 69.0f, 1.0f);
    REQUIRE(e->isChannelActive(0)); // envelope start pending
    render(*e, 24000);
    REQUIRE(e->apu().pulse1.volume() == 0);
    REQUIRE_FALSE(e->isChannelActive(0));

    // Looping envelope, key held: passes through 0 and wraps, always active.
    e->setParameter(Nes2A03Engine::P1EnvLoop, 1.0f);
    e->noteOn(0, 69.0f, 1.0f);
    bool sawZero = false;
    for (int b = 0; b < 240; ++b)
    {
        render(*e, 100);
        sawZero = sawZero || e->apu().pulse1.volume() == 0;
        REQUIRE(e->isChannelActive(0));
    }
    REQUIRE(sawZero);

    // Software envelope: attack from 0 is active; sustain 0 reached with the key held is not.
    e->setParameter(Nes2A03Engine::P2SwAttack, 10.0f);
    e->noteOn(1, 69.0f, 1.0f);
    REQUIRE(e->apu().pulse2.volume() == 0);
    REQUIRE(e->isChannelActive(1));
    e->setParameter(Nes2A03Engine::P2SwAttack, 0.0f);
    e->setParameter(Nes2A03Engine::P2SwDecay, 5.0f);
    e->setParameter(Nes2A03Engine::P2SwSustain, 0.0f);
    e->noteOn(1, 69.0f, 1.0f);
    REQUIRE(e->isChannelActive(1));
    render(*e, 24000);
    REQUIRE(e->apu().pulse2.volume() == 0);
    REQUIRE_FALSE(e->isChannelActive(1));

    // Triangle, linear counter 10 quarter frames (41.7 ms), key held.
    e->setParameter(Nes2A03Engine::TriLinearLength, 10.0f);
    e->noteOn(2, 57.0f, 1.0f);
    REQUIRE(e->isChannelActive(2));
    render(*e, 24000);
    REQUIRE(e->apu().triangle.linearValue() == 0);
    REQUIRE_FALSE(e->isChannelActive(2));
    // With gate_frames the driver retriggers the linear counter: still active.
    e->setParameter(Nes2A03Engine::TriGateFrames, 5.0f);
    e->noteOn(2, 57.0f, 1.0f);
    render(*e, 24000);
    REQUIRE(e->isChannelActive(2));
    e->noteOff(2);
    REQUIRE_FALSE(e->isChannelActive(2));
    // attack_frames: halted at first, but the note is about to start.
    e->setParameter(Nes2A03Engine::TriGateFrames, 0.0f);
    e->setParameter(Nes2A03Engine::TriLinearLength, 127.0f);
    e->setParameter(Nes2A03Engine::TriAttackFrames, 4.0f);
    e->noteOn(2, 57.0f, 1.0f);
    REQUIRE_FALSE(e->apu().triangle.isRunning());
    REQUIRE(e->isChannelActive(2));
    render(*e, 4800);
    REQUIRE(e->apu().triangle.isRunning());

    // One-shot DMC sample that has finished while the key is held.
    std::vector<float> pcm(2205, 0.25f);
    REQUIRE(e->loadSample(0, pcm.data(), static_cast<int>(pcm.size()), 44100.0));
    e->noteOn(4, 60.0f, 1.0f);
    render(*e, 256);
    REQUIRE(e->isChannelActive(4));
    render(*e, 9600); // 50 ms sample
    REQUIRE_FALSE(e->isChannelActive(4));
    delete e;
}

TEST_CASE("NES driver: DMC note-off stops the sample according to the loop flag in the chip", "[nes][engine][dmc]")
{
    std::vector<float> pcm(2205, 0.0f);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 200.0 * static_cast<double>(i) / 44100.0));

    // Looping at note-on, dmc_loop switched off during the note: the note-off still stops it.
    Nes2A03Engine* e = makeEngine(false);
    REQUIRE(e->loadSample(0, pcm.data(), static_cast<int>(pcm.size()), 44100.0));
    e->setParameter(Nes2A03Engine::DmcLoop, 1.0f);
    e->noteOn(4, 60.0f, 1.0f);
    render(*e, 256);
    e->setParameter(Nes2A03Engine::DmcLoop, 0.0f);
    e->noteOff(4);
    render(*e, 48000);
    REQUIRE(e->apu().dmc.bytesRemainingCount() == 0);
    REQUIRE_FALSE(e->isChannelActive(4));

    // One-shot at note-on, dmc_loop switched on during the note: it plays out and ends.
    e->noteOn(4, 60.0f, 1.0f);
    render(*e, 256);
    e->setParameter(Nes2A03Engine::DmcLoop, 1.0f);
    e->noteOff(4);
    REQUIRE(e->apu().dmc.bytesRemainingCount() > 0); // not cut
    render(*e, 48000);
    REQUIRE_FALSE(e->isChannelActive(4));
    delete e;
}

TEST_CASE("NES driver lowest triangle note: base period above $7FF is refused, not clamped (A16)", "[nes][engine][driver]")
{
    // Triangle MIDI n = pulse MIDI n + 12 (research "Period reference table"): NTSC MIDI 20 gives
    // 2154 > 2047, MIDI 21 gives 2033; PAL MIDI 19 gives 2120, MIDI 20 gives 2001.
    Nes2A03Engine* ntsc = makeEngine(false);
    REQUIRE(trianglePeriodForNote(20.0, kCpuHzNtsc) == 2154);
    ntsc->noteOn(2, 20.0f, 1.0f);
    REQUIRE_FALSE(ntsc->apu().triangle.isRunning());
    REQUIRE_FALSE(ntsc->isChannelActive(2));
    const Rendered a = render(*ntsc, 24000);
    REQUIRE(rms(a.main, 4800) < 1e-4);
    // A glide into the representable range starts the note at the next tick.
    ntsc->setChannelPitch(2, 21.0f);
    render(*ntsc, 1600);
    REQUIRE(ntsc->apu().triangle.timerPeriod() == 2033);
    REQUIRE(ntsc->apu().triangle.isRunning());
    // ... and a glide back below stops it.
    ntsc->setChannelPitch(2, 12.0f);
    render(*ntsc, 1600);
    REQUIRE_FALSE(ntsc->apu().triangle.isRunning());
    // Transpose counts: MIDI 44 - 24 = 20 is refused, MIDI 45 - 24 = 21 plays.
    ntsc->setParameter(Nes2A03Engine::TriTranspose, -24.0f);
    ntsc->noteOn(2, 44.0f, 1.0f);
    REQUIRE_FALSE(ntsc->apu().triangle.isRunning());
    ntsc->noteOn(2, 45.0f, 1.0f);
    REQUIRE(ntsc->apu().triangle.timerPeriod() == 2033);
    render(*ntsc, 1600);
    REQUIRE(ntsc->apu().triangle.isRunning());
    delete ntsc;

    Nes2A03Engine* pal = makeEngine(true);
    pal->noteOn(2, 20.0f, 1.0f);
    REQUIRE(pal->isChannelActive(2)); // linear counter reload pending
    render(*pal, 1600);
    REQUIRE(pal->apu().triangle.timerPeriod() == 2001);
    REQUIRE(pal->apu().triangle.isRunning());
    pal->noteOn(2, 19.0f, 1.0f);
    REQUIRE_FALSE(pal->apu().triangle.isRunning());
    delete pal;
}

TEST_CASE("NES DMC: replacing the playing slot does not change the playing sample", "[nes][engine][dmc]")
{
    std::vector<float> high(2000, 1.0f), low(2000, -1.0f);
    Nes2A03Engine* e = makeEngine(false);
    REQUIRE(e->loadSample(0, high.data(), 2000, 44100.0));
    e->setParameter(Nes2A03Engine::DmcLoop, 1.0f);
    e->noteOn(4, 60.0f, 1.0f);
    render(*e, 4800);
    REQUIRE(e->apu().dmc.output() >= 100); // all-ones sample: the counter sits at the top

    // Several reloads of the playing slot between two blocks (no waiting, no bank overwrite).
    for (int i = 0; i < 3; ++i)
        REQUIRE(e->loadSample(0, low.data(), 2000, 44100.0));
    int minLevel = 127;
    for (int b = 0; b < 150; ++b) // 0.2 s: the looping sample is still the old one
    {
        render(*e, 64);
        minLevel = std::min(minLevel, static_cast<int>(e->apu().dmc.output()));
    }
    REQUIRE(minLevel >= 100);

    // The next note-on maps the new data.
    e->noteOn(4, 60.0f, 1.0f);
    render(*e, 4800);
    REQUIRE(e->apu().dmc.output() <= 20);
    delete e;
}

TEST_CASE("NES engine raw output bypasses band-limiting but keeps the level", "[nes][engine][render]")
{
    Nes2A03Engine* e = makeEngine(false);
    e->setRawOutput(true);
    e->noteOn(0, 69.0f, 1.0f);
    const Rendered r = render(*e, 48000);
    REQUIRE(rms(r.main, 4800) > 0.03);
    REQUIRE(measureFrequency(r.main, 14400) == Approx(440.3969).margin(0.1));
    delete e;
}

TEST_CASE("NES frame_mode writes the 5-step sequence to $4017 at the next driver frame", "[nes][engine][frame]")
{
    std::unique_ptr<Nes2A03Engine> e(makeEngine(false));
    std::vector<float> l(static_cast<size_t>(kBlock)), r(static_cast<size_t>(kBlock));
    const auto renderFrames = [&](int blocks) {
        for (int b = 0; b < blocks; ++b)
            e->renderBlock(l.data(), r.data(), nullptr, nullptr, kBlock);
    };

    renderFrames(10);
    REQUIRE_FALSE(e->apu().frame.isFiveStep());   // default: 4-step ($4017 = $40)

    e->setParameter(Nes2A03Engine::FrameMode, 1.0f);
    renderFrames(10);                              // more than one video frame
    REQUIRE(e->apu().frame.isFiveStep());

    e->setParameter(Nes2A03Engine::FrameMode, 0.0f);
    renderFrames(10);
    REQUIRE_FALSE(e->apu().frame.isFiveStep());
}
