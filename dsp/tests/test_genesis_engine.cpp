// GenesisEngine: parameters, driver behaviour, rendering, real-time safety and WAV renders.

#if defined(_MSC_VER)
    #define _CRT_SECURE_NO_WARNINGS   // std::fopen / std::getenv in the test-only WAV writer
#endif

#include "chipdsp/genesis/GenesisEngine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace chipdsp;
using namespace chipdsp::genesis;
using Catch::Approx;

// ----- allocation counter (global operator new replacement for this test executable) -----------

namespace
{
    std::atomic<bool> gCountAllocations { false };
    std::atomic<int> gAllocations { 0 };

    void* countedAlloc(std::size_t n)
    {
        if (gCountAllocations.load(std::memory_order_relaxed))
            gAllocations.fetch_add(1, std::memory_order_relaxed);
        if (void* p = std::malloc(n == 0 ? 1 : n))
            return p;
        throw std::bad_alloc();
    }
} // namespace

void* operator new(std::size_t n) { return countedAlloc(n); }
void* operator new[](std::size_t n) { return countedAlloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace
{
    using E = GenesisEngine;
    constexpr double kRate = 48000.0;
    constexpr int kBlock = 256;

    struct Render
    {
        std::vector<float> l, r;
        std::vector<std::vector<float>> chL, chR;
    };

    Render render(GenesisEngine& e, int numSamples, bool perChannel = false)
    {
        Render out;
        out.l.assign(static_cast<size_t>(numSamples), 0.0f);
        out.r.assign(static_cast<size_t>(numSamples), 0.0f);
        if (perChannel)
        {
            out.chL.assign(kGenesisChannels, std::vector<float>(static_cast<size_t>(numSamples), 0.0f));
            out.chR.assign(kGenesisChannels, std::vector<float>(static_cast<size_t>(numSamples), 0.0f));
        }
        for (int pos = 0; pos < numSamples; pos += kBlock)
        {
            const int n = std::min(kBlock, numSamples - pos);
            float* pl[kGenesisChannels] = {};
            float* pr[kGenesisChannels] = {};
            if (perChannel)
                for (int c = 0; c < kGenesisChannels; ++c)
                {
                    pl[c] = out.chL[static_cast<size_t>(c)].data() + pos;
                    pr[c] = out.chR[static_cast<size_t>(c)].data() + pos;
                }
            e.renderBlock(out.l.data() + pos, out.r.data() + pos, perChannel ? pl : nullptr, perChannel ? pr : nullptr, n);
        }
        return out;
    }

    // Frequency from rising crossings of +h (linear interpolation) over [from, end), with
    // hysteresis: a crossing only counts after the signal went below -h. h = 25 % of the peak,
    // which ignores the zero-order-hold staircase of low-rate DAC playback.
    double measureFrequency(const std::vector<float>& x, size_t from)
    {
        float peak = 0.0f;
        for (size_t i = from; i < x.size(); ++i)
            peak = std::max(peak, std::abs(x[i]));
        const float h = 0.25f * peak;
        double first = -1.0, last = -1.0;
        int count = 0;
        bool armed = false;
        for (size_t i = from + 1; i < x.size(); ++i)
        {
            if (x[i] < -h)
                armed = true;
            if (armed && x[i - 1] < h && x[i] >= h)
            {
                const double t = static_cast<double>(i - 1) + (h - x[i - 1]) / static_cast<double>(x[i] - x[i - 1]);
                if (first < 0.0)
                    first = t;
                last = t;
                ++count;
                armed = false;
            }
        }
        return count < 2 ? 0.0 : (count - 1) / (last - first) * kRate;
    }

    double rms(const std::vector<float>& x, size_t from, size_t to)
    {
        double s = 0.0;
        for (size_t i = from; i < to; ++i)
            s += static_cast<double>(x[i]) * x[i];
        return std::sqrt(s / static_cast<double>(to - from));
    }

    // Pure sine on FM: algorithm 7, only S4 audible, no console filter, no ladder.
    void sinePatch(GenesisEngine& e)
    {
        e.setParameter(E::Model1Lowpass, 0);
        e.setParameter(E::ChipRevision, 1);
        e.setParameter(E::Algorithm, 7);
        e.setParameter(E::Feedback, 0);
        for (int op = 0; op < 4; ++op)
        {
            e.setParameter(E::opParam(op, E::OpTl), op == 3 ? 0.0f : 127.0f);
            e.setParameter(E::opParam(op, E::OpAr), 31);
            e.setParameter(E::opParam(op, E::OpDr), 0);
            e.setParameter(E::opParam(op, E::OpSr), 0);
            e.setParameter(E::opParam(op, E::OpSl), 0);
            e.setParameter(E::opParam(op, E::OpRr), 15);
            e.setParameter(E::opParam(op, E::OpMul), 1);
            e.setParameter(E::opParam(op, E::OpDt), 0);
        }
    }

    // ----- tiny WAV writer (16-bit PCM) ------------------------------------------------------
    void put16(std::FILE* f, int v)
    {
        const unsigned char b[2] = { static_cast<unsigned char>(v & 0xFF), static_cast<unsigned char>((v >> 8) & 0xFF) };
        std::fwrite(b, 1, 2, f);
    }
    void put32(std::FILE* f, unsigned v)
    {
        const unsigned char b[4] = { static_cast<unsigned char>(v & 0xFF), static_cast<unsigned char>((v >> 8) & 0xFF),
                                     static_cast<unsigned char>((v >> 16) & 0xFF), static_cast<unsigned char>((v >> 24) & 0xFF) };
        std::fwrite(b, 1, 4, f);
    }
    bool writeWav(const std::string& path, const std::vector<float>& l, const std::vector<float>* r, int rate)
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f == nullptr)
            return false;
        const unsigned channels = r != nullptr ? 2u : 1u;
        const unsigned frames = static_cast<unsigned>(l.size());
        const unsigned dataBytes = frames * channels * 2u;
        std::fwrite("RIFF", 1, 4, f);
        put32(f, 36u + dataBytes);
        std::fwrite("WAVEfmt ", 1, 8, f);
        put32(f, 16u);
        put16(f, 1);
        put16(f, static_cast<int>(channels));
        put32(f, static_cast<unsigned>(rate));
        put32(f, static_cast<unsigned>(rate) * channels * 2u);
        put16(f, static_cast<int>(channels * 2u));
        put16(f, 16);
        std::fwrite("data", 1, 4, f);
        put32(f, dataBytes);
        auto sample = [](float x) { return static_cast<int>(std::lround(std::clamp(x, -1.0f, 1.0f) * 32767.0f)); };
        for (unsigned i = 0; i < frames; ++i)
        {
            put16(f, sample(l[i]));
            if (r != nullptr)
                put16(f, sample((*r)[i]));
        }
        std::fclose(f);
        return true;
    }
} // namespace

TEST_CASE("Genesis parameter descriptors", "[genesis][engine][params]")
{
    GenesisEngine e;
    const auto descs = e.parameterDescriptors();
    REQUIRE(descs.size() == static_cast<size_t>(E::NumParams));
    CHECK(descs.size() == 86);
    std::set<std::string> keys;
    for (size_t i = 0; i < descs.size(); ++i)
    {
        const ParamDesc& d = descs[i];
        INFO(d.key);
        CHECK(d.id == static_cast<int>(i));
        CHECK(keys.insert(d.key).second);
        CHECK(d.minValue <= d.defaultValue);
        CHECK(d.defaultValue <= d.maxValue);
        CHECK(e.getParameter(d.id) == d.defaultValue);
        CHECK(d.isInteger);
    }
    // Every key listed in docs/ENGINE_SPECS.md for this engine
    const char* const required[] = {
        "clock", "chip_revision", "model1_lowpass", "lfo_enable", "lfo_freq", "algorithm", "feedback", "ams", "fms",
        "transpose", "fine_tune", "vibrato_rate", "vibrato_depth", "vibrato_delay", "unison_detune", "fm1_pan",
        "fm2_pan", "fm3_pan", "fm4_pan", "fm5_pan", "fm6_pan", "velocity_depth", "dac_enable", "dac_sample",
        "dac_rate", "dac_keyed", "dac_loop", "dac_volume", "psg1_att", "psg2_att", "psg3_att", "psgn_att",
        "psg_noise_mode", "psg_noise_rate", "psg_sw_attack", "psg_sw_decay", "psg_sw_sustain", "psg_sw_release",
        "psg_vibrato_rate", "psg_vibrato_depth", "psg_unison_detune", "psg_transpose" };
    for (const char* k : required)
    {
        INFO(k);
        CHECK(keys.count(k) == 1);
    }
    const char* const opFields[] = { "tl", "ar", "dr", "sr", "rr", "sl", "mul", "dt", "rs", "am", "ssg" };
    for (int op = 1; op <= 4; ++op)
        for (const char* f : opFields)
            CHECK(keys.count("op" + std::to_string(op) + "_" + f) == 1);

    // Hardware ranges
    CHECK(descs[E::opParam(0, E::OpTl)].maxValue == 127);
    CHECK(descs[E::opParam(2, E::OpRr)].maxValue == 15);
    CHECK(descs[E::opParam(3, E::OpSsg)].maxValue == 8);
    CHECK(descs[E::DacRate].minValue == 4000);
    CHECK(descs[E::DacRate].maxValue == 32000);

    // setParameter clamps and rounds to register values
    e.setParameter(E::opParam(0, E::OpTl), 300.0f);
    CHECK(e.getParameter(E::opParam(0, E::OpTl)) == 127.0f);
    e.setParameter(E::Algorithm, 2.6f);
    CHECK(e.getParameter(E::Algorithm) == 3.0f);
}

TEST_CASE("Genesis channel layout", "[genesis][engine]")
{
    GenesisEngine e;
    CHECK(e.numChannels() == 10);
    CHECK(std::string(e.channelInfo(0).shortName) == "FM1");
    CHECK(std::string(e.channelInfo(5).shortName) == "FM6");
    CHECK(std::string(e.channelInfo(6).shortName) == "PSG1");
    CHECK(std::string(e.channelInfo(9).shortName) == "PSGN");
    CHECK_FALSE(e.channelInfo(9).isPitched);
    CHECK(e.nativeSampleRate() == Approx(53267.0387).margin(1e-3));
    e.setClockStandard(ClockStandard::Pal);
    CHECK(e.nativeSampleRate() == Approx(52781.1746).margin(1e-3));
}

TEST_CASE("FM note renders the expected fundamental", "[genesis][engine]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    sinePatch(e);
    e.noteOn(0, 69.0f, 1.0f);
    CHECK(e.ym2612().channelBlock(0) == 4);
    CHECK(e.ym2612().channelFnum(0) == 1083);
    const Render out = render(e, 48000);
    // block 4, fnum 1083: 440.126 Hz (research ref 11)
    CHECK(measureFrequency(out.l, 9600) == Approx(440.126).margin(0.02));
    CHECK(rms(out.l, 9600, 48000) > 0.05);
    CHECK(out.l == out.r);   // centre pan
}

TEST_CASE("FM note on PAL uses the PAL fnum", "[genesis][engine]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    sinePatch(e);
    e.setClockStandard(ClockStandard::Pal);
    e.noteOn(0, 69.0f, 1.0f);
    CHECK(e.ym2612().channelFnum(0) == 1093);
    const Render out = render(e, 48000);
    const double expected = 1093.0 * 16.0 / 2.0 / 1048576.0 * fmSampleRate(ClockStandard::Pal);
    CHECK(measureFrequency(out.l, 9600) == Approx(expected).margin(0.02));
}

TEST_CASE("PSG note renders the expected fundamental", "[genesis][engine]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    e.setParameter(E::Model1Lowpass, 0);
    e.noteOn(6, 69.0f, 1.0f);
    CHECK(e.sn76489().tonePeriod(0) == 254);
    CHECK(e.sn76489().attenuation(0) == 0);
    const Render out = render(e, 48000);
    CHECK(measureFrequency(out.l, 14400) == Approx(440.397).margin(0.05));
    e.noteOff(6);
    CHECK(e.sn76489().attenuation(0) == 15);   // release 0 frames: silenced at note off
    CHECK_FALSE(e.isChannelActive(6));
}

TEST_CASE("noteOff keys the operators off and the channel stays active until released", "[genesis][engine]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    for (int op = 0; op < 4; ++op)
        e.setParameter(E::opParam(op, E::OpRr), 15);
    e.noteOn(2, 60.0f, 1.0f);
    render(e, 2000);
    CHECK(e.isChannelActive(2));
    for (int op = 0; op < 4; ++op)
        CHECK(e.ym2612().operatorPhase(2, op) != Ym2612Core::EgPhase::Release);
    e.noteOff(2);
    for (int op = 0; op < 4; ++op)
        CHECK(e.ym2612().operatorPhase(2, op) == Ym2612Core::EgPhase::Release);
    CHECK(e.isChannelActive(2));   // release still sounding
    render(e, 4800);                // 100 ms >> 7.2 ms release at RR 15
    CHECK_FALSE(e.isChannelActive(2));
}

TEST_CASE("Velocity adds TL to carriers only", "[genesis][engine]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    e.setParameter(E::VelocityDepth, 40);
    e.setParameter(E::Algorithm, 4);   // carriers S2, S4
    e.noteOn(0, 60.0f, 0.0f);
    render(e, 256);
    // After the instant attack (AR 31) the EG output equals TL << 3 plus the decay so far;
    // compare against velocity 1 on another channel.
    e.noteOn(1, 60.0f, 1.0f);
    render(e, 16);
    const int s2Soft = e.ym2612().operatorEgOutput(0, 1) - e.ym2612().operatorAttenuation(0, 1);
    const int s2Loud = e.ym2612().operatorEgOutput(1, 1) - e.ym2612().operatorAttenuation(1, 1);
    const int s1Soft = e.ym2612().operatorEgOutput(0, 0) - e.ym2612().operatorAttenuation(0, 0);
    const int s1Loud = e.ym2612().operatorEgOutput(1, 0) - e.ym2612().operatorAttenuation(1, 0);
    CHECK(s2Soft - s2Loud == 40 * 8);
    CHECK(s1Soft == s1Loud);   // S1 is a modulator in algorithm 4
}

TEST_CASE("setChannelPitch is applied at the next driver frame", "[genesis][engine]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    e.noteOn(0, 60.0f, 1.0f);
    render(e, 256);
    CHECK(e.ym2612().channelFnum(0) == 644);
    e.setChannelPitch(0, 69.0f);
    CHECK(e.ym2612().channelFnum(0) == 644);
    render(e, 1024);   // > one frame (801 host samples at 48 kHz)
    CHECK(e.ym2612().channelFnum(0) == 1083);
}

TEST_CASE("Software vibrato moves the fnum once per frame", "[genesis][engine]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    e.setParameter(E::VibratoRate, 4);
    e.setParameter(E::VibratoDepth, 8);
    e.setParameter(E::VibratoDelay, 0);
    e.noteOn(0, 69.0f, 1.0f);
    std::set<int> seen;
    for (int i = 0; i < 60; ++i)
    {
        render(e, 801);
        seen.insert(e.ym2612().channelFnum(0));
    }
    CHECK(*seen.begin() == 1083 - 8);
    CHECK(*seen.rbegin() == 1083 + 8);
    CHECK(GenesisDriver::vibratoOffset(0, 4, 8, 0) == 0);   // starts at the centre
    CHECK(GenesisDriver::vibratoOffset(1, 4, 8, 0) == 4);
    CHECK(GenesisDriver::vibratoOffset(5, 4, 8, 10) == 0);   // still in the delay
}

TEST_CASE("FM unison drives a second channel detuned by fnum units", "[genesis][engine]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    e.setParameter(E::UnisonDetune, 5);
    e.noteOn(0, 69.0f, 1.0f);
    CHECK(e.isChannelActive(1));
    CHECK(e.ym2612().channelFnum(0) == 1083);
    CHECK(e.ym2612().channelFnum(1) == 1088);
    e.noteOff(0);
    CHECK(e.ym2612().operatorPhase(1, 3) == Ym2612Core::EgPhase::Release);
}

TEST_CASE("PSG software envelope, unison and noise", "[genesis][engine][psg]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    e.setParameter(E::PsgSwAttack, 10);
    e.setParameter(E::PsgSwRelease, 5);
    e.setParameter(E::PsgUnisonDetune, 3);
    e.noteOn(6, 69.0f, 1.0f);
    CHECK(e.sn76489().attenuation(0) == 15);   // attack starts silent
    CHECK(e.isChannelActive(7));
    CHECK(e.sn76489().tonePeriod(1) == 254 + 3);
    render(e, 801 * 11);
    CHECK(e.sn76489().attenuation(0) == 0);
    e.noteOff(6);
    render(e, 801 * 7);
    CHECK(e.sn76489().attenuation(0) == 15);
    CHECK_FALSE(e.isChannelActive(6));
    CHECK_FALSE(e.isChannelActive(7));

    // Noise: rate 3 takes tone 3 over and mutes it.
    e.setParameter(E::PsgSwAttack, 0);
    e.setParameter(E::PsgNoiseRate, 3);
    e.setParameter(E::PsgNoiseMode, 1);
    e.noteOn(9, 60.0f, 1.0f);
    CHECK(e.sn76489().noiseControl() == 7);
    CHECK(e.sn76489().attenuation(2) == 15);
    CHECK(e.sn76489().attenuation(3) == 0);
    CHECK(e.sn76489().tonePeriod(2) == 428);
    CHECK(e.isChannelActive(8));
    CHECK(e.driver().velocityToPsgAttenuation(0.0f) == 15);
}

TEST_CASE("DAC plays a loaded sample on channel 6", "[genesis][engine][dac]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    e.setParameter(E::Model1Lowpass, 0);
    std::vector<float> pcm(8000);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = 0.8f * static_cast<float>(std::sin(2.0 * 3.14159265358979 * 500.0 * static_cast<double>(i) / 16000.0));
    REQUIRE(e.loadSample(3, pcm.data(), static_cast<int>(pcm.size()), 16000.0));
    CHECK_FALSE(e.loadSample(16, pcm.data(), 10, 16000.0));
    std::vector<float> tooLong(70000, 0.0f);
    CHECK_FALSE(e.loadSample(0, tooLong.data(), static_cast<int>(tooLong.size()), 16000.0));

    e.setParameter(E::DacEnable, 1);
    e.setParameter(E::DacSample, 3);
    e.noteOn(5, 60.0f, 1.0f);
    CHECK(e.isChannelActive(5));
    const Render out = render(e, 12000, true);   // 0.25 s of the 0.5 s sample
    CHECK(e.ym2612().dacEnabled());
    CHECK(measureFrequency(out.chL[5], 4800) == Approx(500.0).margin(1.0));
    CHECK(rms(out.chL[0], 4800, 12000) < 5e-4);   // idle FM1: only the decaying ladder DC step
    render(e, 14000);
    CHECK_FALSE(e.isChannelActive(5));   // one-shot sample finished

    // clearSample: the slot is empty afterwards, a new DAC note plays nothing.
    REQUIRE(e.loadSample(4, pcm.data(), static_cast<int>(pcm.size()), 16000.0));
    REQUIRE(e.clearSample(3));
    CHECK_FALSE(e.clearSample(16));
    e.noteOn(5, 60.0f, 1.0f);
    const Render cleared = render(e, 4800, true);
    // No tone: at most the DAC's DC level step at key-on, far below the sample-to-sample
    // steps of the 500 Hz tone played from the slot before.
    auto maxStep = [](const std::vector<float>& x, size_t from, size_t to) {
        double m = 0.0;
        for (size_t i = from + 1; i < to; ++i)
            m = std::max(m, static_cast<double>(std::abs(x[i] - x[i - 1])));
        return m;
    };
    CHECK(maxStep(cleared.chL[5], 0, 4800) < 0.2 * maxStep(out.chL[5], 4800, 12000));
    e.noteOff(5);
    e.setParameter(E::DacSample, 4);   // the other slot survived the bank flip
    e.noteOn(5, 60.0f, 1.0f);
    const Render other = render(e, 9600, true);
    CHECK(measureFrequency(other.chL[5], 4800) == Approx(500.0).margin(1.0));
}

TEST_CASE("stageParameter sets the DAC encoding rate before setParameter", "[genesis][engine][dac]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    std::vector<float> pcm(40000, 0.0f);   // 2.5 s at 16 kHz
    e.stageParameter(E::DacRate, 32000.0f);
    CHECK(e.getParameter(E::DacRate) == 32000.0f);
    CHECK_FALSE(e.loadSample(0, pcm.data(), 40000, 16000.0));   // 80000 bytes > 65536
    e.stageParameter(E::DacRate, 4000.0f);
    CHECK(e.loadSample(0, pcm.data(), 40000, 16000.0));         // 10000 bytes
}

TEST_CASE("Per-channel outputs sum to the main output", "[genesis][engine]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    e.setParameter(E::Fm2Pan, 0);
    e.noteOn(0, 60.0f, 1.0f);
    e.noteOn(1, 67.0f, 0.8f);
    e.noteOn(6, 72.0f, 1.0f);
    e.noteOn(9, 60.0f, 1.0f);
    const Render out = render(e, 9600, true);
    double maxErr = 0.0;
    for (size_t i = 0; i < out.l.size(); ++i)
    {
        double sl = 0.0, sr = 0.0;
        for (int c = 0; c < kGenesisChannels; ++c)
        {
            sl += out.chL[static_cast<size_t>(c)][i];
            sr += out.chR[static_cast<size_t>(c)][i];
        }
        maxErr = std::max(maxErr, std::abs(sl - out.l[i]));
        maxErr = std::max(maxErr, std::abs(sr - out.r[i]));
    }
    CHECK(maxErr < 1e-4);
    CHECK(rms(out.chR[1], 4800, 9600) < 0.1 * rms(out.chL[1], 4800, 9600));   // FM2 panned left (ladder crosstalk only)
    CHECK(rms(out.chL[1], 4800, 9600) > 0.01);
    CHECK(rms(out.chL[3], 4800, 9600) < 1e-3);   // unused channel
}

TEST_CASE("Chip revision switches the ladder effect", "[genesis][engine][dac]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    render(e, 64);
    CHECK(e.ym2612().ladderEffect());
    CHECK(e.ym2612().outputLeft() == 24);
    e.setParameter(E::ChipRevision, 1);
    render(e, 64);
    CHECK_FALSE(e.ym2612().ladderEffect());
    CHECK(e.ym2612().outputLeft() == 0);
}

TEST_CASE("renderBlock, notes and parameters do not allocate", "[genesis][engine][realtime]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    std::vector<float> pcm(4000, 0.25f);
    REQUIRE(e.loadSample(0, pcm.data(), static_cast<int>(pcm.size()), 22050.0));
    std::vector<float> l(kBlock), r(kBlock);
    std::vector<std::vector<float>> chL(kGenesisChannels, std::vector<float>(kBlock));
    std::vector<std::vector<float>> chR(kGenesisChannels, std::vector<float>(kBlock));
    float* pl[kGenesisChannels];
    float* pr[kGenesisChannels];
    for (int c = 0; c < kGenesisChannels; ++c)
    {
        pl[c] = chL[static_cast<size_t>(c)].data();
        pr[c] = chR[static_cast<size_t>(c)].data();
    }

    gAllocations.store(0);
    gCountAllocations.store(true);
    e.setParameter(E::DacEnable, 1);
    e.setParameter(E::UnisonDetune, 3);
    e.setParameter(E::LfoEnable, 1);
    e.setParameter(E::Fms, 5);
    e.noteOn(0, 60.0f, 1.0f);
    e.noteOn(5, 60.0f, 1.0f);
    e.noteOn(6, 64.0f, 0.7f);
    e.noteOn(9, 50.0f, 1.0f);
    for (int b = 0; b < 200; ++b)
    {
        if (b == 50)
            e.setChannelPitch(0, 62.5f);
        if (b == 100)
        {
            e.noteOff(0);
            e.noteOff(6);
            e.setRawOutput(true);
        }
        e.renderBlock(l.data(), r.data(), b % 2 ? pl : nullptr, b % 2 ? pr : nullptr, kBlock);
    }
    e.reset();
    e.renderBlock(l.data(), r.data(), nullptr, nullptr, kBlock);
    gCountAllocations.store(false);
    CHECK(gAllocations.load() == 0);
}

TEST_CASE("Raw output and oversized blocks render", "[genesis][engine]")
{
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    e.setRawOutput(true);
    sinePatch(e);
    e.noteOn(0, 69.0f, 1.0f);
    std::vector<float> l(48000), r(48000);
    e.renderBlock(l.data(), r.data(), nullptr, nullptr, 48000);   // larger than maxBlockSize
    CHECK(measureFrequency(l, 9600) == Approx(440.126).margin(0.05));
}

TEST_CASE("Model 1 low-pass follows a first-order RC at 3.39 kHz", "[genesis][engine][filter]")
{
    // Research "Console low-pass filters and levels": first-order RC, fc = 3390 Hz (VA0-VA2).
    for (float note : { 93.0f, 104.0f, 110.0f })
    {
        double level[2] = {};
        for (int on = 0; on < 2; ++on)
        {
            GenesisEngine e;
            e.prepare(kRate, kBlock);
            sinePatch(e);
            e.setParameter(E::Model1Lowpass, static_cast<float>(on));
            e.noteOn(0, note, 1.0f);
            const Render out = render(e, 24000);
            level[on] = rms(out.l, 9600, 24000);
        }
        const FmPitch p = fmPitchFromNote(note, ClockStandard::Ntsc);
        const double f = fmFrequency(p.block, p.fnum, ClockStandard::Ntsc);
        const double expectedDb = -10.0 * std::log10(1.0 + (f / 3390.0) * (f / 3390.0));
        INFO("note " << note << " f " << f);
        CHECK(20.0 * std::log10(level[1] / level[0]) == Approx(expectedDb).margin(0.3));
    }
}

TEST_CASE("Model 1 low-pass keeps the RC response up to 18 kHz at 44.1 and 48 kHz", "[genesis][engine][filter]")
{
    // Research "Console low-pass filters and levels": |H| = 1 / sqrt(1 + (f / 3390)^2) (first-order
    // RC), independent of the host rate. The bilinear form was 3.4 dB too low at 15 kHz (48 kHz)
    // and 4.3 dB at 44.1 kHz. Pure S4 sines, MUL raising the note into the treble.
    struct Tone { float note; int mul; };
    const Tone tones[] = { { 93.0f, 5 }, { 99.0f, 5 }, { 103.0f, 5 }, { 101.0f, 6 } };   // ~8.8, 12.4, 15.7, 16.6 kHz
    for (double rate : { 44100.0, 48000.0 })
        for (const Tone& t : tones)
        {
            double level[2] = {};
            for (int on = 0; on < 2; ++on)
            {
                GenesisEngine e;
                e.prepare(rate, kBlock);
                sinePatch(e);
                e.setParameter(E::opParam(3, E::OpMul), static_cast<float>(t.mul));
                e.setParameter(E::Model1Lowpass, static_cast<float>(on));
                e.noteOn(0, t.note, 1.0f);
                const Render out = render(e, 24000);
                level[on] = rms(out.l, 9600, 24000);
            }
            const FmPitch p = fmPitchFromNote(t.note, ClockStandard::Ntsc);
            const double f = fmFrequency(p.block, p.fnum, ClockStandard::Ntsc) * t.mul;
            const double expectedDb = -10.0 * std::log10(1.0 + (f / 3390.0) * (f / 3390.0));
            INFO("rate " << rate << " f " << f);
            REQUIRE(f < 0.45 * rate);
            CHECK(20.0 * std::log10(level[1] / level[0]) == Approx(expectedDb).margin(0.2));
        }

    // The coefficients themselves: unity DC gain, -3.01 dB at fc, RC magnitude at 15 kHz.
    for (double rate : { 22050.0, 32000.0, 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const RcLowPassCoefficients c = rcLowPass(3390.0, rate);
        auto magDb = [&](double f) {
            const double w = 2.0 * 3.14159265358979323846 * f / rate;
            const double nr = c.b0 + c.b1 * std::cos(w) + c.b2 * std::cos(2.0 * w);
            const double ni = -c.b1 * std::sin(w) - c.b2 * std::sin(2.0 * w);
            const double dr = 1.0 + c.a1 * std::cos(w);
            const double di = -c.a1 * std::sin(w);
            return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
        };
        INFO("rate " << rate);
        CHECK(magDb(0.0) == Approx(0.0).margin(1e-9));
        CHECK(magDb(3390.0) == Approx(-3.0103).margin(0.14));
        const double top = std::min(20000.0, 0.45 * rate);
        for (double f = 500.0; f <= top; f += 500.0)
        {
            INFO("f " << f);
            CHECK(magDb(f) == Approx(-10.0 * std::log10(1.0 + (f / 3390.0) * (f / 3390.0))).margin(0.14));
        }
    }
}

TEST_CASE("FM unison retrigger reuses the owner's partner", "[genesis][engine]")
{
    // Research "Software features": the partner follows the owner; a retrigger must not walk the
    // unison voice across the free channels while old partners are still releasing.
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    e.setParameter(E::UnisonDetune, 3);
    for (int op = 0; op < 4; ++op)
        e.setParameter(E::opParam(op, E::OpRr), 2);   // long release
    for (int n = 0; n < 6; ++n)
    {
        e.noteOn(0, 69.0f, 1.0f);
        render(e, 2000);
        INFO("retrigger " << n);
        CHECK(e.isChannelActive(0));
        CHECK(e.isChannelActive(1));
        for (int c = 2; c < 6; ++c)
            CHECK_FALSE(e.isChannelActive(c));
        CHECK(e.ym2612().channelFnum(1) == 1083 + 3);
    }
    e.noteOff(0);
    CHECK(e.ym2612().operatorPhase(1, 3) == Ym2612Core::EgPhase::Release);
}

TEST_CASE("Switching the noise rate to tone 3 during a note takes tone 3 over", "[genesis][engine][psg]")
{
    // Research "MIDI note -> registers": with psg_noise_rate 3 the driver mutes tone 3 and keeps it
    // while the noise note sounds; the tone-3 note that was playing ends.
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    e.setParameter(E::PsgNoiseRate, 0);
    e.noteOn(8, 69.0f, 1.0f);
    e.noteOn(9, 60.0f, 1.0f);
    render(e, 1024);
    CHECK(e.sn76489().attenuation(2) == 0);
    CHECK(e.sn76489().tonePeriod(2) == 254);

    e.setParameter(E::PsgNoiseRate, 3);
    render(e, 1024);   // > one frame
    CHECK(e.sn76489().noiseControl() == 7);
    CHECK(e.sn76489().attenuation(2) == 15);
    CHECK(e.sn76489().tonePeriod(2) == 428);   // the noise note sets tone 3's period
    CHECK(e.isChannelActive(8));

    e.noteOff(9);
    render(e, 1024);
    CHECK(e.sn76489().attenuation(2) == 15);   // tone 3 does not come back
    CHECK_FALSE(e.isChannelActive(8));
    CHECK_FALSE(e.isChannelActive(9));
}

TEST_CASE("DAC software volume rounds to nearest, symmetric around 0x80", "[genesis][engine][dac]")
{
    // Research "Velocity": sample scaled by round(velocity * dac_volume) / 127 before $2A.
    // Gain 64: -128 -> -64.50 -> -65, +127 -> 64.00, -1 -> -0.50 -> -1, -64 -> -32.25 -> -32.
    Ym2612Core ym;
    Sn76489Core psg;
    GenesisDriver drv;
    drv.attach(&ym, &psg);
    DriverSettings s;
    s.dacEnable = 1;
    s.dacVolume = 64;
    drv.reset(s);
    ym.setLadderEffect(false);
    const uint8_t bytes[] = { 0x00, 0xFF, 0x7F, 0x81, 0x80, 0x40, 0xC0, 0x01 };
    const int expected[] = { 63, 192, 127, 129, 128, 96, 160, 64 };
    DacSampleView views[kDacSlots];
    views[0] = { bytes, static_cast<int>(sizeof(bytes)) };
    drv.setDacBank(views);
    drv.noteOn(kDacChannel, 60.0f, 1.0f);
    for (size_t i = 0; i < sizeof(bytes); ++i)
    {
        drv.dacWriteNext();
        ym.clockSample();
        INFO("byte " << i);
        CHECK(ym.channelOutput(kDacChannel) == dacSample9(static_cast<uint8_t>(expected[i])));
    }
}

TEST_CASE("DAC sample loads while rendering do not overwrite the bank being read", "[genesis][engine][dac][realtime]")
{
    // IChipEngine: loadSample is safe against a running renderBlock(). Two loads within one audio
    // block used to write the bank the audio thread was still reading. The loader now waits until
    // the audio thread has left that bank. Here: back-to-back loads between blocks, then a loader
    // thread loading continuously while the audio thread plays the looped slot it rewrites.
    GenesisEngine e;
    e.prepare(kRate, kBlock);
    e.setParameter(E::DacEnable, 1);
    e.setParameter(E::DacLoop, 1);
    e.setParameter(E::DacVolume, 127);
    e.setParameter(E::ChipRevision, 1);
    std::vector<float> a(3000, 0.5f), b(3000, -0.5f);
    REQUIRE(e.loadSample(0, a.data(), static_cast<int>(a.size()), 16000.0));
    REQUIRE(e.loadSample(1, b.data(), static_cast<int>(b.size()), 16000.0));
    REQUIRE(e.loadSample(0, b.data(), static_cast<int>(b.size()), 16000.0));   // three loads, no render
    e.noteOn(5, 60.0f, 1.0f);
    render(e, 2048);
    // Slot 0 now holds b: -0.5 -> lround(128 - 63.5) = 65 -> 9-bit (65 - 128) * 2 = -126.
    CHECK(e.ym2612().channelOutput(5) == -126);

    std::atomic<bool> stop { false };
    std::atomic<int> loads { 0 };
    std::thread loader([&] {
        while (!stop.load())
        {
            const auto& pcm = (loads.load() & 1) ? a : b;   // slot 0 alternates between a and b
            if (e.loadSample(0, pcm.data(), static_cast<int>(pcm.size()), 16000.0))
                loads.fetch_add(1);
        }
    });
    std::vector<float> l(64), r(64);
    std::set<int> seen;
    for (int blk = 0; blk < 200000 && (blk < 3000 || loads.load() < 200); ++blk)
    {
        e.renderBlock(l.data(), r.data(), nullptr, nullptr, 64);
        seen.insert(e.ym2612().channelOutput(5));
    }
    stop.store(true);
    loader.join();
    CHECK(loads.load() >= 200);
    // Every DAC value played is one of the two samples: +0.5 -> lround(191.5) = 192 -> +128,
    // -0.5 -> -126; the looped slot keeps playing (same length in both versions).
    CHECK(e.isChannelActive(5));
    for (int v : seen)
    {
        INFO("DAC value " << v);
        CHECK((v == 128 || v == -126));
    }
}
TEST_CASE("Render WAV files for inspection", "[genesis][engine][render]")
{
    const char* dir = std::getenv("RCV_RENDER_DIR");
    if (dir == nullptr || *dir == '\0')
        return;   // skipped silently when no render folder is given

    GenesisEngine e;
    e.prepare(kRate, kBlock);
    // DAC drum on FM6: a decaying 60 -> 40 Hz sine with a noise click, 0.3 s at 22050 Hz.
    std::vector<float> kick(6615);
    uint32_t seed = 12345;
    double phase = 0.0;
    for (size_t i = 0; i < kick.size(); ++i)
    {
        const double t = static_cast<double>(i) / 22050.0;
        phase += 2.0 * 3.14159265358979 * (40.0 + 80.0 * std::exp(-t * 20.0)) / 22050.0;
        seed = seed * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(seed >> 8) / 8388608.0 - 1.0) * std::exp(-t * 200.0);
        kick[i] = static_cast<float>(0.9 * std::exp(-t * 8.0) * std::sin(phase) + 0.3 * noise);
    }
    REQUIRE(e.loadSample(0, kick.data(), static_cast<int>(kick.size()), 22050.0));
    e.setParameter(E::DacEnable, 1);
    e.setParameter(E::DacLoop, 0);
    e.setParameter(E::Fm1Pan, 0);
    e.setParameter(E::Fm3Pan, 2);
    e.setParameter(E::PsgSwDecay, 30);
    e.setParameter(E::PsgSwSustain, 10);
    e.setParameter(E::PsgSwRelease, 20);

    const float fmNotes[5] = { 48.0f, 60.0f, 64.0f, 67.0f, 72.0f };
    for (int c = 0; c < 5; ++c)
        e.noteOn(c, fmNotes[c], 0.9f);
    e.noteOn(5, 60.0f, 1.0f);
    e.noteOn(6, 72.0f, 1.0f);
    e.noteOn(7, 76.0f, 0.8f);
    e.noteOn(8, 79.0f, 0.8f);
    e.noteOn(9, 60.0f, 0.7f);

    const int total = 2 * static_cast<int>(kRate);
    Render first = render(e, total * 3 / 4, true);
    for (int c = 0; c < kGenesisChannels; ++c)
        e.noteOff(c);
    Render second = render(e, total - total * 3 / 4, true);
    auto append = [](std::vector<float>& a, const std::vector<float>& b) { a.insert(a.end(), b.begin(), b.end()); };
    append(first.l, second.l);
    append(first.r, second.r);
    for (int c = 0; c < kGenesisChannels; ++c)
    {
        append(first.chL[static_cast<size_t>(c)], second.chL[static_cast<size_t>(c)]);
        append(first.chR[static_cast<size_t>(c)], second.chR[static_cast<size_t>(c)]);
    }

    const std::string base = std::string(dir) + "/";
    CHECK(writeWav(base + "genesis_main.wav", first.l, &first.r, static_cast<int>(kRate)));
    for (int c = 0; c < kGenesisChannels; ++c)
    {
        const std::string name = base + "genesis_" + e.channelInfo(c).shortName + ".wav";
        if (c < 6)
            CHECK(writeWav(name, first.chL[static_cast<size_t>(c)], &first.chR[static_cast<size_t>(c)], static_cast<int>(kRate)));
        else
            CHECK(writeWav(name, first.chL[static_cast<size_t>(c)], nullptr, static_cast<int>(kRate)));
    }
}

