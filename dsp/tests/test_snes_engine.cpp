// SnesDspEngine: parameters, APU RAM budget, driver behaviour, rendering and real-time safety.

#include "chipdsp/snes/SnesDspEngine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <new>
#include <numbers>
#include <string>
#include <vector>

using chipdsp::SnesDspEngine;
using namespace chipdsp::snes;

// ----- allocation counter (replaces the global operator new for this test executable) -----
namespace
{
    std::atomic<bool> gCountAllocations{ false };
    std::atomic<int> gAllocations{ 0 };
} // namespace

void* operator new(std::size_t size)
{
    if (gCountAllocations.load(std::memory_order_relaxed))
        gAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace
{
    struct Rig
    {
        std::unique_ptr<SnesDspEngine> engine = std::make_unique<SnesDspEngine>();
        int block = 480;
        std::vector<float> l, r;
        std::vector<std::vector<float>> chL, chR;
        std::vector<float*> ptrL, ptrR;

        explicit Rig(double rate = 48000.0, int blockSize = 480) : block(blockSize)
        {
            engine->prepare(rate, block);
            l.assign(static_cast<size_t>(block), 0.0f);
            r.assign(static_cast<size_t>(block), 0.0f);
            chL.assign(8, std::vector<float>(static_cast<size_t>(block), 0.0f));
            chR.assign(8, std::vector<float>(static_cast<size_t>(block), 0.0f));
            for (int v = 0; v < 8; ++v)
            {
                ptrL.push_back(chL[static_cast<size_t>(v)].data());
                ptrR.push_back(chR[static_cast<size_t>(v)].data());
            }
        }
        void render(bool perVoice = false)
        {
            engine->renderBlock(l.data(), r.data(), perVoice ? ptrL.data() : nullptr, perVoice ? ptrR.data() : nullptr, block);
        }
        // Renders 'seconds' and returns the left main output.
        std::vector<float> renderSeconds(double seconds, bool perVoice = false)
        {
            std::vector<float> out;
            const int blocks = static_cast<int>(std::ceil(seconds * 48000.0 / block));
            for (int b = 0; b < blocks; ++b)
            {
                render(perVoice);
                out.insert(out.end(), l.begin(), l.end());
            }
            return out;
        }
    };

    // One cycle of a sine every 64 samples (500 Hz at 32 kHz), 'cycles' cycles.
    std::vector<float> sineCycles(int cycles, float amplitude = 0.8f)
    {
        std::vector<float> s(static_cast<size_t>(64 * cycles));
        for (size_t i = 0; i < s.size(); ++i)
            s[i] = amplitude * static_cast<float>(std::sin(2.0 * std::numbers::pi * static_cast<double>(i) / 64.0));
        return s;
    }

    double rms(const std::vector<float>& x, size_t from, size_t to)
    {
        double s = 0.0;
        for (size_t i = from; i < to; ++i)
            s += static_cast<double>(x[i]) * x[i];
        return std::sqrt(s / static_cast<double>(to - from));
    }
} // namespace

TEST_CASE("Engine: parameter descriptors match ENGINE_SPECS", "[snes][engine]")
{
    SnesDspEngine engine;
    const auto descs = engine.parameterDescriptors();
    REQUIRE(descs.size() == static_cast<size_t>(SnesDspEngine::NumParams));
    REQUIRE(descs.size() == 35);

    struct Spec { const char* key; float min, max, def; };
    const Spec specs[] = {
        { "sample", 0, 31, 0 }, { "adsr_enable", 0, 1, 1 }, { "attack", 0, 15, 15 }, { "decay", 0, 7, 7 },
        { "sustain_level", 0, 7, 7 }, { "sustain_rate", 0, 31, 0 }, { "gain_mode", 0, 4, 0 },
        { "gain_value", 0, 127, 127 }, { "release_mode", 0, 1, 0 }, { "release_rate", 0, 31, 31 },
        { "volume", 0, 127, 100 }, { "pan", -64, 64, 0 }, { "transpose", -24, 24, 0 }, { "fine_tune", -100, 100, 0 },
        { "vibrato_rate", 0, 15, 0 }, { "vibrato_depth", 0, 64, 0 }, { "vibrato_delay", 0, 60, 0 },
        { "noise_enable", 0, 1, 0 }, { "noise_clock", 0, 31, 0 }, { "pmon", 0, 1, 0 }, { "loop_override", 0, 2, 0 },
        { "echo_enable", 0, 1, 0 }, { "echo_delay", 0, 15, 0 }, { "echo_feedback", -128, 127, 0 },
        { "echo_volume", -128, 127, 0 }, { "fir_preset", 0, 7, 0 }, { "v1_echo", 0, 1, 0 }, { "v2_echo", 0, 1, 0 },
        { "v3_echo", 0, 1, 0 }, { "v4_echo", 0, 1, 0 }, { "v5_echo", 0, 1, 0 }, { "v6_echo", 0, 1, 0 },
        { "v7_echo", 0, 1, 0 }, { "v8_echo", 0, 1, 0 }, { "main_volume", 0, 127, 127 },
    };
    REQUIRE(std::size(specs) == descs.size());
    for (size_t i = 0; i < descs.size(); ++i)
    {
        const auto& d = descs[i];
        INFO(d.key);
        REQUIRE(d.id == static_cast<int>(i));
        REQUIRE(std::string(d.key) == specs[i].key);
        REQUIRE(d.minValue == specs[i].min);
        REQUIRE(d.maxValue == specs[i].max);
        REQUIRE(d.defaultValue == specs[i].def);
        REQUIRE(d.isInteger);
        REQUIRE(engine.getParameter(d.id) == d.defaultValue);
        if (d.choiceLabels != nullptr)
            for (int k = 0; k <= static_cast<int>(d.maxValue - d.minValue); ++k)
                REQUIRE(d.choiceLabels[k] != nullptr);
    }
    REQUIRE(std::string(descs[SnesDspEngine::FirPreset].choiceLabels[5]) == "Comb");
    REQUIRE(engine.numChannels() == 8);
    REQUIRE(std::string(engine.channelInfo(0).name) == "Voice 1");
    REQUIRE(std::string(engine.channelInfo(7).shortName) == "V8");
    REQUIRE(engine.nativeSampleRate() == 32000.0);
    REQUIRE(engine.numSampleSlots() == 32);
}

TEST_CASE("Engine: sample budget is 64 KiB minus the echo buffer", "[snes][engine][ram]")
{
    SnesDspEngine engine;
    REQUIRE(sampleCapacityBytes(0) == 0x10000 - 0x0400);
    REQUIRE(sampleCapacityBytes(15) == 0x10000 - 15 * 2048 - 0x0400);
    REQUIRE(engine.freeSampleBytes() == sampleCapacityBytes(0));

    std::vector<float> big(100000, 0.1f);          // 6250 blocks = 56250 bytes
    REQUIRE(engine.loadSample(0, big.data(), static_cast<int>(big.size()), 32000.0));
    REQUIRE(engine.sampleInfo(0).brrBytes == 56250);
    REQUIRE(engine.freeSampleBytes() == sampleCapacityBytes(0) - 56250);

    std::vector<float> more(20000, 0.1f);          // 11250 bytes: does not fit any more
    REQUIRE(!engine.loadSample(1, more.data(), static_cast<int>(more.size()), 32000.0));
    REQUIRE(!engine.sampleInfo(1).loaded);
    std::vector<float> small(8000, 0.1f);          // 4500 bytes: fits
    REQUIRE(engine.loadSample(1, small.data(), static_cast<int>(small.size()), 32000.0));

    // A long echo leaves less room: EDL 15 takes 30 KiB.
    SnesDspEngine echoEngine;
    echoEngine.setParameter(SnesDspEngine::EchoDelay, 15.0f);
    std::vector<float> fits(60000, 0.1f);          // 3750 blocks = 33750 bytes <= 33792
    REQUIRE(echoEngine.loadSample(0, fits.data(), static_cast<int>(fits.size()), 32000.0));
    std::vector<float> tooBig(61000, 0.1f);        // 34317 bytes
    REQUIRE(!echoEngine.loadSample(0, tooBig.data(), static_cast<int>(tooBig.size()), 32000.0));
    REQUIRE(echoEngine.sampleInfo(0).brrBytes == 33750);   // failed load leaves the slot untouched

    // Raising echo_delay after loading does not block edits that keep the size.
    engine.setParameter(SnesDspEngine::EchoDelay, 15.0f);
    REQUIRE(engine.freeSampleBytes() < 0);
    REQUIRE(engine.setSampleRootNote(0, 48.0f));
    REQUIRE(engine.setSampleLoop(0, 100));
    REQUIRE(!engine.loadSample(2, small.data(), 100, 32000.0));   // but new data does not fit

    REQUIRE(!engine.loadSample(32, small.data(), 10, 32000.0));
    REQUIRE(!engine.loadSample(0, nullptr, 10, 32000.0));
}

TEST_CASE("Engine: sources above 32 kHz are stored at 32 kHz, lower rates as-is", "[snes][engine][ram]")
{
    SnesDspEngine engine;
    std::vector<float> s(4800, 0.2f);
    REQUIRE(engine.loadSample(0, s.data(), 4800, 48000.0));
    REQUIRE(engine.sampleInfo(0).storedRate == 32000.0);
    REQUIRE(engine.sampleInfo(0).numBlocks == 200);          // 3200 frames at 32 kHz
    REQUIRE(engine.loadSample(1, s.data(), 4800, 16000.0));
    REQUIRE(engine.sampleInfo(1).storedRate == 16000.0);
    REQUIRE(engine.sampleInfo(1).numBlocks == 300);
    REQUIRE(engine.setSampleLoop(1, 10));
    REQUIRE(engine.sampleInfo(1).loopStartBlock == 10);
    REQUIRE(!engine.setSampleLoop(1, 300));
    REQUIRE(engine.setSampleRootNote(1, 57.0f));
    REQUIRE(engine.sampleInfo(1).rootNote == 57.0f);
}

TEST_CASE("Engine: driver keeps the echo buffer clear of sample data", "[snes][engine][ram]")
{
    Rig rig;
    std::vector<float> big(90000);                // 5625 blocks = 50625 bytes, data ends at 51649
    for (size_t i = 0; i < big.size(); ++i)
        big[i] = 0.5f * static_cast<float>(std::sin(static_cast<double>(i) * 0.05));
    REQUIRE(rig.engine->loadSample(0, big.data(), static_cast<int>(big.size()), 32000.0));
    rig.render();
    const int dataEnd = kSampleDataStart + 50625;
    // EDL 7 would start at 0x10000 - 7 * 2048 = 51200 < 51649: the largest fitting EDL is 6.
    REQUIRE(SnesDriver::maxEchoDelay(dataEnd) == 6);
    const std::vector<uint8_t> before(rig.engine->chip().ram() + kSampleDataStart, rig.engine->chip().ram() + dataEnd);

    rig.engine->setParameter(SnesDspEngine::EchoEnable, 1.0f);
    rig.engine->setParameter(SnesDspEngine::EchoDelay, 15.0f);
    rig.engine->setParameter(SnesDspEngine::EchoFeedback, 100.0f);
    rig.engine->setParameter(SnesDspEngine::EchoVolume, 60.0f);
    rig.engine->setParameter(SnesDspEngine::V1Echo, 1.0f);
    rig.engine->noteOn(0, 60.0f, 1.0f);
    rig.renderSeconds(1.0);
    REQUIRE(rig.engine->driver().programmedEchoDelay() == 6);
    REQUIRE(rig.engine->driver().echoWritesEnabled());
    REQUIRE(echoBufferStart(6) >= dataEnd);
    const std::vector<uint8_t> after(rig.engine->chip().ram() + kSampleDataStart, rig.engine->chip().ram() + dataEnd);
    REQUIRE(before == after);
    // The echo buffer does hold data.
    bool echoWritten = false;
    for (int a = echoBufferStart(6); a < 0x10000; ++a)
        echoWritten = echoWritten || rig.engine->chip().ram()[a] != 0;
    REQUIRE(echoWritten);
}

TEST_CASE("Engine: echo re-initialisation waits 240 ms with writes disabled", "[snes][engine]")
{
    Rig rig;
    rig.engine->setParameter(SnesDspEngine::EchoEnable, 1.0f);
    rig.engine->setParameter(SnesDspEngine::EchoDelay, 4.0f);
    rig.engine->reset();
    rig.render();
    REQUIRE(rig.engine->driver().programmedEchoDelay() == 4);
    REQUIRE(rig.engine->driver().echoWritesEnabled());
    REQUIRE(rig.engine->chip().readRegister(kRegEsa) == (0x10000 - 4 * 2048) >> 8);

    rig.engine->setParameter(SnesDspEngine::EchoDelay, 8.0f);
    rig.render();                                     // 10 ms: the change is seen at a tick
    REQUIRE(rig.engine->driver().programmedEchoDelay() == 8);
    REQUIRE(!rig.engine->driver().echoWritesEnabled());
    REQUIRE((rig.engine->chip().readRegister(kRegFlg) & kFlgEchoWriteDisable) != 0);
    REQUIRE(rig.engine->chip().readRegister(kRegEdl) == 8);
    rig.renderSeconds(0.2);
    REQUIRE(!rig.engine->driver().echoWritesEnabled());   // 210 ms < 240 ms
    rig.renderSeconds(0.02);
    REQUIRE(!rig.engine->driver().echoWritesEnabled());   // 230 ms, still < 240 ms
    rig.renderSeconds(0.03);
    REQUIRE(rig.engine->driver().echoWritesEnabled());
}

TEST_CASE("Engine: MIDI note sets the pitch register; the tone has the quantised frequency", "[snes][engine]")
{
    Rig rig;
    const auto cycle = sineCycles(4);
    REQUIRE(rig.engine->loadSample(0, cycle.data(), static_cast<int>(cycle.size()), 32000.0));
    REQUIRE(rig.engine->setSampleLoop(0, 0));
    const float root = 69.0f + 12.0f * static_cast<float>(std::log2(500.0 / 440.0));
    REQUIRE(rig.engine->setSampleRootNote(0, root));

    rig.engine->noteOn(0, 69.0f, 1.0f);
    const auto out = rig.renderSeconds(1.0);
    // round(4096 * 2^((69 - root) / 12)) = round(4096 * 440 / 500) = round(3604.48) = 3604
    // (reference value 30's formula).
    const int p = 3604;
    REQUIRE((rig.engine->chip().readRegister(kRegPitchL) | (rig.engine->chip().readRegister(kRegPitchH) << 8)) == p);
    const double expected = 32000.0 * p / 4096.0 / 64.0;

    // Frequency from rising zero crossings (linear interpolation), skipping 100 ms.
    double firstCross = -1.0, lastCross = -1.0;
    int crossings = 0;
    for (size_t i = 4800; i + 1 < out.size(); ++i)
    {
        if (out[i] < 0.0f && out[i + 1] >= 0.0f)
        {
            const double t = static_cast<double>(i) + out[i] / (out[i] - out[i + 1]);
            if (firstCross < 0.0)
                firstCross = t;
            lastCross = t;
            ++crossings;
        }
    }
    REQUIRE(crossings > 100);
    const double measured = (crossings - 1) / ((lastCross - firstCross) / 48000.0);
    REQUIRE(measured == Catch::Approx(expected).epsilon(0.0005));
    REQUIRE(std::abs(measured - 440.0) < 1.0);
}

TEST_CASE("Engine: velocity and pan map to VxVOL", "[snes][engine]")
{
    int left = 0, right = 0;
    SnesDriver::voiceVolumes(100, 1.0f, 0, left, right);
    REQUIRE((left == 100 && right == 100));
    SnesDriver::voiceVolumes(100, 0.5f, 0, left, right);
    REQUIRE((left == 50 && right == 50));
    SnesDriver::voiceVolumes(127, 1.0f, 64, left, right);
    REQUIRE((left == 0 && right == 127));
    SnesDriver::voiceVolumes(127, 1.0f, -32, left, right);
    REQUIRE((left == 127 && right == 64));

    Rig rig;
    rig.engine->noteOn(2, 60.0f, 0.5f);
    rig.render();
    REQUIRE(rig.engine->chip().readRegister(0x20) == 50);
    REQUIRE(rig.engine->chip().readRegister(0x21) == 50);

    // The instrument is latched per note at noteOn, even when both key-ons reach the chip
    // in the same block.
    rig.engine->setParameter(SnesDspEngine::Pan, -64.0f);
    rig.engine->noteOn(4, 60.0f, 1.0f);
    rig.engine->setParameter(SnesDspEngine::Pan, 64.0f);
    rig.engine->setParameter(SnesDspEngine::Attack, 3.0f);
    rig.engine->noteOn(5, 60.0f, 1.0f);
    rig.render();
    REQUIRE(rig.engine->chip().readRegister(0x40) == 100);
    REQUIRE(rig.engine->chip().readRegister(0x41) == 0);
    REQUIRE(rig.engine->chip().readRegister(0x40 + kRegAdsr1) == 0xFF);   // A15 D7
    REQUIRE(rig.engine->chip().readRegister(0x50) == 0);
    REQUIRE(rig.engine->chip().readRegister(0x51) == 100);
    REQUIRE(rig.engine->chip().readRegister(0x50 + kRegAdsr1) == 0xF3);   // A3 D7
    // Changing volume later does not touch a playing note (VxVOL is written at key-on).
    rig.engine->setParameter(SnesDspEngine::Volume, 10.0f);
    rig.render();
    REQUIRE(rig.engine->chip().readRegister(0x51) == 100);
}

TEST_CASE("Engine: hardware KOFF release frees the voice in about 8 ms", "[snes][engine]")
{
    Rig rig(48000.0, 48);                           // 1 ms blocks
    const auto cycle = sineCycles(4);
    REQUIRE(rig.engine->loadSample(0, cycle.data(), static_cast<int>(cycle.size()), 32000.0));
    REQUIRE(rig.engine->setSampleLoop(0, 0));
    rig.engine->noteOn(0, 60.0f, 1.0f);
    REQUIRE(rig.engine->isChannelActive(0));        // pending key-on counts as active
    for (int i = 0; i < 50; ++i)
        rig.render();
    REQUIRE(rig.engine->isChannelActive(0));
    rig.engine->noteOff(0);
    int ms = 0;
    while (rig.engine->isChannelActive(0) && ms < 100)
    {
        rig.render();
        ++ms;
    }
    REQUIRE(ms >= 7);
    REQUIRE(ms <= 10);                              // 256 samples = 8 ms, plus the poll
}

TEST_CASE("Engine: GAIN release mode fades then keys off", "[snes][engine]")
{
    Rig rig(48000.0, 48);
    const auto cycle = sineCycles(4);
    REQUIRE(rig.engine->loadSample(0, cycle.data(), static_cast<int>(cycle.size()), 32000.0));
    REQUIRE(rig.engine->setSampleLoop(0, 0));
    rig.engine->setParameter(SnesDspEngine::ReleaseMode, 1.0f);
    rig.engine->setParameter(SnesDspEngine::ReleaseRate, 31.0f);   // exponential, every sample: 695 steps
    rig.engine->noteOn(0, 60.0f, 1.0f);
    for (int i = 0; i < 20; ++i)
        rig.render();
    rig.engine->noteOff(0);
    rig.render();
    REQUIRE(rig.engine->chip().readRegister(kRegGain) == 0xBF);
    REQUIRE((rig.engine->chip().readRegister(kRegAdsr1) & 0x80) == 0);
    int ms = 1;
    while (rig.engine->isChannelActive(0) && ms < 200)
    {
        rig.render();
        ++ms;
    }
    // 695 samples = 21.7 ms to envelope 0, then the next 4 ms tick writes KOFF.
    REQUIRE(ms >= 21);
    REQUIRE(ms <= 28);
}

TEST_CASE("Engine: one-shot samples end by themselves; loop override forces a loop", "[snes][engine]")
{
    const auto hit = sineCycles(20);                // 1280 frames = 40 ms at 32 kHz
    for (const int mode : { 0, 1, 2 })
    {
        Rig rig;
        REQUIRE(rig.engine->loadSample(0, hit.data(), static_cast<int>(hit.size()), 32000.0));
        rig.engine->setParameter(SnesDspEngine::LoopOverride, static_cast<float>(mode));
        rig.engine->noteOn(0, 60.0f, 1.0f);
        rig.renderSeconds(0.1);
        INFO("loop_override " << mode);
        REQUIRE(rig.engine->isChannelActive(0) == (mode == 2));
    }
    // A looped sample plays as one-shot with loop_override 1.
    Rig rig;
    REQUIRE(rig.engine->loadSample(0, hit.data(), static_cast<int>(hit.size()), 32000.0));
    REQUIRE(rig.engine->setSampleLoop(0, 10));
    rig.engine->noteOn(0, 60.0f, 1.0f);
    rig.renderSeconds(0.1);
    REQUIRE(rig.engine->isChannelActive(0));
    rig.engine->setParameter(SnesDspEngine::LoopOverride, 1.0f);
    rig.engine->noteOn(0, 60.0f, 1.0f);
    rig.renderSeconds(0.1);
    REQUIRE(!rig.engine->isChannelActive(0));
}

TEST_CASE("Engine: per-voice buses carry each voice alone and sum to the main mix", "[snes][engine]")
{
    Rig rig;
    const auto cycle = sineCycles(4, 0.4f);
    REQUIRE(rig.engine->loadSample(0, cycle.data(), static_cast<int>(cycle.size()), 32000.0));
    REQUIRE(rig.engine->setSampleLoop(0, 0));
    rig.engine->noteOn(0, 60.0f, 1.0f);
    rig.engine->noteOn(3, 67.0f, 1.0f);

    std::vector<float> main, v0, v3, v5;
    for (int b = 0; b < 50; ++b)
    {
        rig.render(true);
        main.insert(main.end(), rig.l.begin(), rig.l.end());
        v0.insert(v0.end(), rig.chL[0].begin(), rig.chL[0].end());
        v3.insert(v3.end(), rig.chL[3].begin(), rig.chL[3].end());
        v5.insert(v5.end(), rig.chL[5].begin(), rig.chL[5].end());
    }
    const size_t from = 4800, to = main.size();
    REQUIRE(rms(v0, from, to) > 0.05);
    REQUIRE(rms(v3, from, to) > 0.05);
    REQUIRE(rms(v5, from, to) < 1e-4);
    double err = 0.0;
    for (size_t i = from; i < to; ++i)
        err = std::max(err, std::abs(static_cast<double>(main[i]) - v0[i] - v3[i]));
    REQUIRE(err < 2e-3);   // a few LSB of MVOL rounding plus the inversion offsets
}

TEST_CASE("Engine: audio-thread calls never allocate", "[snes][engine][realtime]")
{
    Rig rig;
    const auto cycle = sineCycles(8);
    REQUIRE(rig.engine->loadSample(0, cycle.data(), static_cast<int>(cycle.size()), 32000.0));
    REQUIRE(rig.engine->setSampleLoop(0, 0));
    rig.engine->setParameter(SnesDspEngine::EchoEnable, 1.0f);
    rig.engine->setParameter(SnesDspEngine::EchoDelay, 3.0f);

    gAllocations.store(0);
    gCountAllocations.store(true);
    rig.engine->reset();
    for (int b = 0; b < 200; ++b)
    {
        if (b % 20 == 0)
        {
            rig.engine->noteOn(b / 20 % 8, 60.0f + static_cast<float>(b % 12), 0.8f);
            rig.engine->setParameter(SnesDspEngine::EchoDelay, static_cast<float>(b / 20 % 5));
            rig.engine->setParameter(SnesDspEngine::FirPreset, static_cast<float>(b / 20 % 8));
            rig.engine->setChannelPitch(0, 62.0f);
        }
        if (b % 20 == 10)
            rig.engine->noteOff(b / 20 % 8);
        (void)rig.engine->isChannelActive(b % 8);
        rig.render(b % 2 == 0);
    }
    rig.engine->setRawOutput(true);
    rig.render(true);
    gCountAllocations.store(false);
    REQUIRE(gAllocations.load() == 0);

    for (const float x : rig.l)
        REQUIRE(std::isfinite(x));
}

TEST_CASE("Engine: raw output is a zero-order hold of the 32 kHz stream", "[snes][engine]")
{
    Rig rig(96000.0, 96);
    const auto cycle = sineCycles(4);
    REQUIRE(rig.engine->loadSample(0, cycle.data(), static_cast<int>(cycle.size()), 32000.0));
    REQUIRE(rig.engine->setSampleLoop(0, 0));
    rig.engine->setRawOutput(true);
    rig.engine->noteOn(0, 60.0f, 1.0f);
    for (int b = 0; b < 20; ++b)
        rig.render();
    // At 96 kHz every native sample is held for exactly 3 host samples (before the DC blocker,
    // whose 5 Hz pole barely moves consecutive values).
    int holds = 0;
    for (int i = 0; i + 2 < 96; i += 3)
        holds += std::abs(rig.l[static_cast<size_t>(i)] - rig.l[static_cast<size_t>(i + 1)]) < 1e-3f ? 1 : 0;
    REQUIRE(holds >= 30);
}
