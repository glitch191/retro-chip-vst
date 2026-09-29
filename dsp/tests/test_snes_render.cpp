// Renders a short demo through the whole engine and, when RCV_RENDER_DIR is set, writes
// 16-bit stereo WAV files (main + one per hardware voice, 2 seconds) for visual inspection.

#include "chipdsp/snes/SnesDspEngine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using chipdsp::SnesDspEngine;

namespace
{
    std::string renderDirectory()
    {
#if defined(_MSC_VER)
        char* value = nullptr;
        size_t length = 0;
        if (_dupenv_s(&value, &length, "RCV_RENDER_DIR") != 0 || value == nullptr)
            return {};
        std::string dir(value);
        std::free(value);
        return dir;
#else
        const char* value = std::getenv("RCV_RENDER_DIR");
        return value != nullptr ? std::string(value) : std::string();
#endif
    }

    // Minimal 16-bit PCM WAV writer (interleaved channels).
    void writeWav(const std::filesystem::path& path, const std::vector<std::vector<float>>& channels, int sampleRate)
    {
        const auto numChannels = static_cast<uint16_t>(channels.size());
        const auto frames = static_cast<uint32_t>(channels.front().size());
        const uint32_t dataBytes = frames * numChannels * 2u;
        std::ofstream f(path, std::ios::binary);
        auto u32 = [&f](uint32_t v) { const char b[4] = { char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char((v >> 24) & 0xFF) }; f.write(b, 4); };
        auto u16 = [&f](uint16_t v) { const char b[2] = { char(v & 0xFF), char((v >> 8) & 0xFF) }; f.write(b, 2); };
        f.write("RIFF", 4);
        u32(36u + dataBytes);
        f.write("WAVEfmt ", 8);
        u32(16u);
        u16(1u);
        u16(numChannels);
        u32(static_cast<uint32_t>(sampleRate));
        u32(static_cast<uint32_t>(sampleRate) * numChannels * 2u);
        u16(static_cast<uint16_t>(numChannels * 2u));
        u16(16u);
        f.write("data", 4);
        u32(dataBytes);
        for (uint32_t i = 0; i < frames; ++i)
        {
            for (const auto& ch : channels)
            {
                const float x = std::clamp(ch[i], -1.0f, 1.0f);
                u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(x * 32767.0f))));
            }
        }
    }

    // Band-limited saw with 8 cycles in 1024 frames (A3-ish at 32 kHz: 250 Hz), looped.
    std::vector<float> sawLoop()
    {
        std::vector<float> s(1024, 0.0f);
        for (size_t i = 0; i < s.size(); ++i)
            for (int h = 1; h <= 30; ++h)
                s[i] += static_cast<float>(0.5 / h * std::sin(2.0 * std::numbers::pi * h * 8.0 * static_cast<double>(i) / 1024.0));
        return s;
    }

    // Kick-like one-shot: pitch-swept sine with an exponential decay, 0.3 s.
    std::vector<float> kick()
    {
        std::vector<float> s(9600);
        double phase = 0.0;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double t = static_cast<double>(i) / 32000.0;
            phase += 2.0 * std::numbers::pi * (50.0 + 150.0 * std::exp(-t * 30.0)) / 32000.0;
            s[i] = static_cast<float>(0.9 * std::exp(-t * 9.0) * std::sin(phase));
        }
        return s;
    }
} // namespace

TEST_CASE("Render: demo passage through every voice, optional WAV output", "[snes][render]")
{
    constexpr int kRate = 48000;
    constexpr int kBlock = 480;             // 10 ms
    constexpr int kBlocks = 200;            // 2 s

    auto engine = std::make_unique<SnesDspEngine>();
    engine->prepare(kRate, kBlock);
    const auto saw = sawLoop();
    REQUIRE(engine->loadSample(0, saw.data(), static_cast<int>(saw.size()), 32000.0));
    REQUIRE(engine->setSampleLoop(0, 0));
    // 8 cycles in 1024 frames = 250 Hz at the stored rate: root note 59.21 (B3 - 21 cents).
    REQUIRE(engine->setSampleRootNote(0, 69.0f + 12.0f * static_cast<float>(std::log2(250.0 / 440.0))));
    const auto k = kick();
    REQUIRE(engine->loadSample(1, k.data(), static_cast<int>(k.size()), 32000.0));

    using P = SnesDspEngine;
    engine->setParameter(P::Volume, 64.0f);      // headroom: the main bus saturates at 16 bits
    engine->setParameter(P::EchoEnable, 1.0f);
    engine->setParameter(P::EchoDelay, 5.0f);
    engine->setParameter(P::EchoFeedback, 70.0f);
    engine->setParameter(P::EchoVolume, 45.0f);
    engine->setParameter(P::FirPreset, 1.0f);
    for (int v = 0; v < 3; ++v)
        engine->setParameter(P::V1Echo + v, 1.0f);
    engine->reset();

    std::vector<std::vector<float>> mainOut(2), voiceOut(16);
    std::vector<float> l(kBlock), r(kBlock);
    std::vector<std::vector<float>> vl(8, std::vector<float>(kBlock)), vr(8, std::vector<float>(kBlock));
    std::vector<float*> pl, pr;
    for (int v = 0; v < 8; ++v)
    {
        pl.push_back(vl[static_cast<size_t>(v)].data());
        pr.push_back(vr[static_cast<size_t>(v)].data());
    }

    auto instrument = [&](int sample, int attack, int decay, int sl, int sr, int pan) {
        engine->setParameter(P::Sample, static_cast<float>(sample));
        engine->setParameter(P::Attack, static_cast<float>(attack));
        engine->setParameter(P::Decay, static_cast<float>(decay));
        engine->setParameter(P::SustainLevel, static_cast<float>(sl));
        engine->setParameter(P::SustainRate, static_cast<float>(sr));
        engine->setParameter(P::Pan, static_cast<float>(pan));
    };

    for (int b = 0; b < kBlocks; ++b)
    {
        // Events at block boundaries (the driver latches sample/envelope at key-on).
        if (b == 0)
        {
            instrument(0, 12, 3, 5, 0, -30);
            engine->noteOn(0, 60.0f, 0.8f);
            instrument(0, 12, 3, 5, 0, 0);
            engine->noteOn(1, 64.0f, 0.8f);
            instrument(0, 12, 3, 5, 0, 30);
            engine->noteOn(2, 67.0f, 0.8f);
        }
        if (b == 20 || b == 70 || b == 120 || b == 170)
        {
            instrument(1, 15, 7, 7, 0, 0);
            engine->noteOn(3, 48.0f, 1.0f);
        }
        if (b == 100)
        {
            engine->noteOff(0);
            engine->noteOff(1);
            engine->noteOff(2);
            instrument(0, 15, 5, 4, 14, 0);
            engine->setParameter(P::VibratoRate, 8.0f);
            engine->setParameter(P::VibratoDepth, 40.0f);
            engine->setParameter(P::VibratoDelay, 20.0f);
            engine->noteOn(4, 76.0f, 1.0f);
        }
        if (b == 130)
        {
            engine->setParameter(P::VibratoDepth, 0.0f);
            instrument(0, 15, 6, 2, 20, -50);
            engine->noteOn(5, 55.0f, 0.7f);
            instrument(0, 15, 6, 2, 20, 50);
            engine->noteOn(6, 59.0f, 0.7f);
            instrument(0, 15, 6, 2, 20, 0);
            engine->noteOn(7, 62.0f, 0.7f);
        }
        if (b == 150)
            engine->setChannelPitch(4, 79.0f);

        engine->renderBlock(l.data(), r.data(), pl.data(), pr.data(), kBlock);
        mainOut[0].insert(mainOut[0].end(), l.begin(), l.end());
        mainOut[1].insert(mainOut[1].end(), r.begin(), r.end());
        for (int v = 0; v < 8; ++v)
        {
            auto& dl = voiceOut[static_cast<size_t>(2 * v)];
            auto& dr = voiceOut[static_cast<size_t>(2 * v + 1)];
            dl.insert(dl.end(), vl[static_cast<size_t>(v)].begin(), vl[static_cast<size_t>(v)].end());
            dr.insert(dr.end(), vr[static_cast<size_t>(v)].begin(), vr[static_cast<size_t>(v)].end());
        }
    }

    // Every output is finite and every voice sounded.
    for (const auto& ch : mainOut)
        for (const float x : ch)
            REQUIRE(std::isfinite(x));
    for (int v = 0; v < 8; ++v)
    {
        float peak = 0.0f;
        for (const float x : voiceOut[static_cast<size_t>(2 * v)])
            peak = std::max(peak, std::abs(x));
        for (const float x : voiceOut[static_cast<size_t>(2 * v + 1)])
            peak = std::max(peak, std::abs(x));
        INFO("voice " << v + 1);
        REQUIRE(peak > 0.01f);
    }

    const std::string dir = renderDirectory();
    if (dir.empty())
        return;
    std::filesystem::create_directories(dir);
    writeWav(std::filesystem::path(dir) / "snes_main.wav", mainOut, kRate);
    for (int v = 0; v < 8; ++v)
    {
        const std::vector<std::vector<float>> pair = { voiceOut[static_cast<size_t>(2 * v)], voiceOut[static_cast<size_t>(2 * v + 1)] };
        writeWav(std::filesystem::path(dir) / ("snes_voice" + std::to_string(v + 1) + ".wav"), pair, kRate);
    }
}
