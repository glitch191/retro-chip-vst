// CPU-cost benchmark for the chip engines.
//
// For each engine and each host sample rate (44100 and 48000 Hz), renders 512-sample
// blocks with a note held on every hardware channel (plus echo/LFO enabled where the
// engine has such parameters) and reports the mean time per block, the percentage of
// real time it represents and the worst block. Two configurations are measured: main
// output only, and main + per-channel outputs (multi-out buses enabled).
//
// Usage: chipdsp_bench [--seconds N] [--chip nes|snes|genesis]
// The numbers quoted in README.md come from this program on the developer machine.

#include "chipdsp/EngineFactory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace
{
    constexpr int kBlock = 512;

    struct Result
    {
        double meanMicros = 0.0;
        double worstMicros = 0.0;
        double percentRealtime = 0.0;
    };

    void enableIfPresent(chipdsp::IChipEngine& engine, const char* keySubstring, float value)
    {
        for (const auto& d : engine.parameterDescriptors())
            if (std::strstr(d.key, keySubstring) != nullptr)
                engine.setParameter(d.id, std::clamp(value, d.minValue, d.maxValue));
    }

    Result measure(chipdsp::IChipEngine& engine, double sampleRate, bool perChannel, double seconds)
    {
        engine.prepare(sampleRate, kBlock);
        engine.reset();

        // Typical "everything on" configuration.
        enableIfPresent(engine, "echo_enable", 1.0f);
        enableIfPresent(engine, "v1_echo", 1.0f);
        enableIfPresent(engine, "lfo_enable", 1.0f);
        enableIfPresent(engine, "vibrato_rate", 6.0f);
        enableIfPresent(engine, "vibrato_depth", 4.0f);

        const int channels = engine.numChannels();
        std::vector<float> mainL(kBlock), mainR(kBlock);
        std::vector<std::vector<float>> outsL(static_cast<size_t>(channels), std::vector<float>(kBlock));
        std::vector<std::vector<float>> outsR(static_cast<size_t>(channels), std::vector<float>(kBlock));
        std::vector<float*> ptrL, ptrR;
        for (int c = 0; c < channels; ++c)
        {
            ptrL.push_back(outsL[static_cast<size_t>(c)].data());
            ptrR.push_back(outsR[static_cast<size_t>(c)].data());
        }

        for (int c = 0; c < channels; ++c)
            engine.noteOn(c, 48.0f + static_cast<float>(c) * 3.0f, 0.9f);

        // Warm-up.
        for (int i = 0; i < 20; ++i)
            engine.renderBlock(mainL.data(), mainR.data(), perChannel ? ptrL.data() : nullptr,
                               perChannel ? ptrR.data() : nullptr, kBlock);

        const int blocks = static_cast<int>(seconds * sampleRate / kBlock);
        double total = 0.0, worst = 0.0;
        for (int i = 0; i < blocks; ++i)
        {
            if (i % 200 == 100)
                for (int c = 0; c < channels; ++c)
                    engine.noteOn(c, 48.0f + static_cast<float>((c + i / 200) % 12), 0.9f);

            const auto t0 = std::chrono::steady_clock::now();
            engine.renderBlock(mainL.data(), mainR.data(), perChannel ? ptrL.data() : nullptr,
                               perChannel ? ptrR.data() : nullptr, kBlock);
            const auto t1 = std::chrono::steady_clock::now();
            const double micros = std::chrono::duration<double, std::micro>(t1 - t0).count();
            total += micros;
            worst = std::max(worst, micros);
        }

        Result r;
        r.meanMicros = total / blocks;
        r.worstMicros = worst;
        const double blockMicros = 1.0e6 * kBlock / sampleRate;
        r.percentRealtime = 100.0 * r.meanMicros / blockMicros;
        return r;
    }
} // namespace

int main(int argc, char** argv)
{
    double seconds = 5.0;
    std::string onlyChip;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--chip") == 0 && i + 1 < argc)
            onlyChip = argv[++i];
    }

    std::printf("chipdsp benchmark: %d-sample blocks, %.1f s per configuration\n\n", kBlock, seconds);
    std::printf("| Engine  | Rate  | Outputs        | Mean / block | Worst / block | %% of real time |\n");
    std::printf("|---------|-------|----------------|--------------|---------------|----------------|\n");

    for (const auto chip : { chipdsp::ChipId::Nes, chipdsp::ChipId::Snes, chipdsp::ChipId::Genesis })
    {
        if (!onlyChip.empty() && onlyChip != chipdsp::chipKey(chip))
            continue;

        for (const double rate : { 44100.0, 48000.0 })
        {
            for (const bool perChannel : { false, true })
            {
                auto engine = chipdsp::createEngine(chip);
                const Result r = measure(*engine, rate, perChannel, seconds);
                std::printf("| %-7s | %5.0f | %-14s | %9.1f us | %10.1f us | %13.2f %% |\n",
                            chipdsp::chipName(chip), rate, perChannel ? "main + channels" : "main only",
                            r.meanMicros, r.worstMicros, r.percentRealtime);
            }
        }
    }
    return 0;
}
