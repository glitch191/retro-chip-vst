// Renders a short NES arrangement to WAV files for waveform inspection.
// Only runs its file output when the environment variable RCV_RENDER_DIR is set; otherwise
// it renders without writing anything. One mono file per hardware channel plus the stereo main.

#include "chipdsp/nes/Nes2A03Engine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

using chipdsp::Nes2A03Engine;

namespace
{
    // Minimal 16-bit PCM WAV writer (interleaved when channels == 2).
    bool writeWav(const std::string& path, const std::vector<float>& left, const std::vector<float>* right, int sampleRate)
    {
        std::ofstream f(path, std::ios::binary);
        if (!f)
            return false;
        const uint16_t channels = right != nullptr ? 2 : 1;
        const uint32_t frames = static_cast<uint32_t>(left.size());
        const uint32_t dataBytes = frames * channels * 2u;
        auto u32 = [&](uint32_t v) { f.put(static_cast<char>(v & 0xFF)); f.put(static_cast<char>((v >> 8) & 0xFF));
                                     f.put(static_cast<char>((v >> 16) & 0xFF)); f.put(static_cast<char>((v >> 24) & 0xFF)); };
        auto u16 = [&](uint16_t v) { f.put(static_cast<char>(v & 0xFF)); f.put(static_cast<char>((v >> 8) & 0xFF)); };
        f.write("RIFF", 4);
        u32(36u + dataBytes);
        f.write("WAVE", 4);
        f.write("fmt ", 4);
        u32(16);
        u16(1); // PCM
        u16(channels);
        u32(static_cast<uint32_t>(sampleRate));
        u32(static_cast<uint32_t>(sampleRate) * channels * 2u);
        u16(static_cast<uint16_t>(channels * 2));
        u16(16);
        f.write("data", 4);
        u32(dataBytes);
        auto sample = [&](float x) {
            const float c = std::clamp(x, -1.0f, 1.0f);
            u16(static_cast<uint16_t>(static_cast<int16_t>(std::lround(c * 32767.0f))));
        };
        for (uint32_t i = 0; i < frames; ++i)
        {
            sample(left[i]);
            if (right != nullptr)
                sample((*right)[i]);
        }
        return static_cast<bool>(f);
    }
} // namespace

TEST_CASE("NES render: 2 s arrangement to WAV (RCV_RENDER_DIR)", "[nes][render][wav]")
{
    constexpr int kRate = 48000;
    constexpr int kBlock = 240;             // 5 ms: note events land on block boundaries
    constexpr int kTotal = 2 * kRate;

    Nes2A03Engine engine;
    engine.setParameter(Nes2A03Engine::P1Duty, 1.0f);
    engine.setParameter(Nes2A03Engine::P1SwDecay, 20.0f);
    engine.setParameter(Nes2A03Engine::P1SwSustain, 6.0f);
    engine.setParameter(Nes2A03Engine::P1SwRelease, 6.0f);
    engine.setParameter(Nes2A03Engine::P1VibratoRate, 4.0f);
    engine.setParameter(Nes2A03Engine::P1VibratoDepth, 3.0f);
    engine.setParameter(Nes2A03Engine::P1VibratoDelay, 15.0f);
    engine.setParameter(Nes2A03Engine::P2Duty, 2.0f);
    engine.setParameter(Nes2A03Engine::P2EnvEnable, 1.0f);
    engine.setParameter(Nes2A03Engine::P2Volume, 6.0f);
    engine.setParameter(Nes2A03Engine::NzSwDecay, 8.0f);
    engine.setParameter(Nes2A03Engine::NzSwSustain, 0.0f);
    engine.setParameter(Nes2A03Engine::NzPeriod, 4.0f);
    engine.setParameter(Nes2A03Engine::DmcRate, 15.0f);
    engine.prepare(kRate, kBlock);

    // Procedural kick for the DMC: decaying sine with a falling pitch.
    std::vector<float> kick(static_cast<size_t>(0.12 * 44100.0));
    double phase = 0.0;
    for (size_t i = 0; i < kick.size(); ++i)
    {
        const double t = static_cast<double>(i) / 44100.0;
        phase += 2.0 * std::numbers::pi * (50.0 + 150.0 * std::exp(-t * 30.0)) / 44100.0;
        kick[i] = static_cast<float>(0.9 * std::exp(-t * 18.0) * std::sin(phase));
    }
    REQUIRE(engine.loadSample(0, kick.data(), static_cast<int>(kick.size()), 44100.0));

    struct Ev
    {
        double time;
        int channel;
        float note;  // < 0: note off
    };
    const float melody[] = { 72, 76, 79, 84, 79, 76, 72, 67 };
    std::vector<Ev> events;
    for (int i = 0; i < 8; ++i)
    {
        events.push_back({ 0.25 * i, 0, melody[i] });
        events.push_back({ 0.25 * i + 0.2, 0, -1.0f });
    }
    events.push_back({ 0.0, 1, 64.0f });
    events.push_back({ 1.0, 1, 67.0f });
    events.push_back({ 0.0, 2, 48.0f });
    events.push_back({ 0.45, 2, -1.0f });
    events.push_back({ 0.5, 2, 43.0f });
    events.push_back({ 0.95, 2, -1.0f });
    events.push_back({ 1.0, 2, 45.0f });
    events.push_back({ 1.45, 2, -1.0f });
    events.push_back({ 1.5, 2, 41.0f });
    events.push_back({ 1.95, 2, -1.0f });
    for (int i = 0; i < 8; ++i)
        events.push_back({ 0.125 + 0.25 * i, 3, 60.0f });
    for (int i = 0; i < 4; ++i)
        events.push_back({ 0.5 * i, 4, 60.0f });
    std::stable_sort(events.begin(), events.end(), [](const Ev& a, const Ev& b) { return a.time < b.time; });

    std::vector<float> mainL(kTotal), mainR(kTotal);
    std::vector<std::vector<float>> ch(5, std::vector<float>(kTotal));
    std::vector<std::vector<float>> chR(5, std::vector<float>(kBlock));
    size_t next = 0;
    for (int pos = 0; pos < kTotal; pos += kBlock)
    {
        while (next < events.size() && events[next].time * kRate <= pos)
        {
            const Ev& ev = events[next++];
            if (ev.note < 0.0f)
                engine.noteOff(ev.channel);
            else
                engine.noteOn(ev.channel, ev.note, 1.0f);
        }
        float* pl[5];
        float* pr[5];
        for (int c = 0; c < 5; ++c)
        {
            pl[c] = ch[static_cast<size_t>(c)].data() + pos;
            pr[c] = chR[static_cast<size_t>(c)].data();
        }
        engine.renderBlock(mainL.data() + pos, mainR.data() + pos, pl, pr, kBlock);
    }

    // Sanity: every channel produced signal, the main output stays in range.
    for (int c = 0; c < 5; ++c)
    {
        float peak = 0.0f;
        for (float x : ch[static_cast<size_t>(c)])
            peak = std::max(peak, std::abs(x));
        INFO("channel " << c);
        REQUIRE(peak > 0.01f);
    }
    float peak = 0.0f;
    for (float x : mainL)
        peak = std::max(peak, std::abs(x));
    REQUIRE(peak > 0.05f);
    REQUIRE(peak < 1.0f);

    std::string dir;
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, "RCV_RENDER_DIR") == 0 && value != nullptr)
        dir = value;
    std::free(value);
#else
    if (const char* value = std::getenv("RCV_RENDER_DIR"))
        dir = value;
#endif
    if (dir.empty())
        return;
    const std::string base = dir + "/nes_";
    const char* names[5] = { "p1", "p2", "tri", "noise", "dmc" };
    REQUIRE(writeWav(base + "main.wav", mainL, &mainR, kRate));
    for (int c = 0; c < 5; ++c)
        REQUIRE(writeWav(base + names[c] + ".wav", ch[static_cast<size_t>(c)], nullptr, kRate));
}
