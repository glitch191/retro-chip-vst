// chiptool: command-line companion of the preset generator (tools/gen_presets.py).
//
//   chiptool dump-params <nes|snes|genesis>
//       Parameter table of the engine as JSON on stdout, same schema as
//       tools/presetgen/params/<chip>.json.
//
//   chiptool render <preset.json> <out.wav> [--note 60] [--seconds 2] [--rate 48000]
//                   [--velocity 100] [--channel N]
//       Applies the preset's "params" through the engine's parameterDescriptors() keys,
//       plays the note for 60 % of the duration and writes a stereo 16-bit WAV.
//
//   chiptool features <bank.json> <features.json> [--jobs N]
//       Renders every preset of the bank (1 s, C4, 48 kHz) and writes
//       {name: {"mel": [40 log-energies in dB], "env": [20 RMS points]}}, computed with
//       an inline radix-2 FFT and a 40-band mel filterbank. Presets are rendered in
//       parallel with std::thread, one engine instance per thread.
//
// Tool code: exceptions and allocation are fine here (nothing runs on an audio thread).

#include "MiniJson.h"

#include "chipdsp/EngineFactory.h"
#include "chipdsp/IChipEngine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using minijson::Value;

namespace
{

constexpr int kBlockSize = 512;
constexpr double kNoteOnFraction = 0.6; // the note is held for 60 % of the render

// Feature extraction constants (must match tools/presetgen/qa.py).
constexpr int kMelBands = 40;
constexpr int kEnvelopePoints = 20;
constexpr int kFftSize = 2048;
constexpr int kFftHop = 1024;
constexpr double kMelFloorDb = -100.0;
constexpr double kFeatureSeconds = 1.0;
constexpr double kFeatureRate = 48000.0;
constexpr int kFeatureNote = 60;
constexpr int kFeatureVelocity = 100;

// ----- files ------------------------------------------------------------------------------------

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeFile(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("cannot write " + path);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// ----- chips and parameters ---------------------------------------------------------------------

bool chipFromKey(const std::string& key, chipdsp::ChipId& out)
{
    if (key == "nes")     { out = chipdsp::ChipId::Nes; return true; }
    if (key == "snes")    { out = chipdsp::ChipId::Snes; return true; }
    if (key == "genesis") { out = chipdsp::ChipId::Genesis; return true; }
    return false;
}

Value paramTableJson(const chipdsp::IChipEngine& engine, const std::string& source)
{
    Value::Array params;
    for (const auto& d : engine.parameterDescriptors())
    {
        Value entry{Value::Object{}};
        entry["id"] = Value(d.id);
        entry["key"] = Value(d.key != nullptr ? d.key : "");
        entry["name"] = Value(d.name != nullptr ? d.name : "");
        entry["group"] = Value(d.group != nullptr ? d.group : "");
        entry["min"] = Value(static_cast<double>(d.minValue));
        entry["max"] = Value(static_cast<double>(d.maxValue));
        entry["default"] = Value(static_cast<double>(d.defaultValue));
        entry["isInteger"] = Value(d.isInteger);
        entry["unit"] = Value(d.unit != nullptr ? d.unit : "");
        if (d.choiceLabels != nullptr)
        {
            Value::Array labels;
            const int count = static_cast<int>(std::lround(d.maxValue - d.minValue)) + 1;
            for (int i = 0; i < count; ++i)
                labels.emplace_back(d.choiceLabels[i] != nullptr ? d.choiceLabels[i] : "");
            entry["labels"] = Value(std::move(labels));
        }
        params.push_back(std::move(entry));
    }
    Value meta{Value::Object{}};
    meta["chip"] = Value(chipdsp::chipKey(engine.chipId()));
    meta["source"] = Value(source);
    Value doc{Value::Object{}};
    doc["meta"] = std::move(meta);
    doc["params"] = Value(std::move(params));
    return doc;
}

// Set every parameter to its default, then apply the preset's "params" by key.
// Unknown keys are reported on stderr when 'warn' is set (one line each).
void applyPreset(chipdsp::IChipEngine& engine, const Value& preset, bool warn)
{
    const auto descs = engine.parameterDescriptors();
    for (const auto& d : descs)
        engine.setParameter(d.id, d.defaultValue);

    const Value* params = preset.find("params");
    if (params == nullptr || !params->isObject())
        return;
    for (const auto& [key, value] : params->asObject())
    {
        if (!value.isNumber())
        {
            if (warn)
                std::fprintf(stderr, "warning: parameter %s is not a number, ignored\n", key.c_str());
            continue;
        }
        const chipdsp::ParamDesc* match = nullptr;
        for (const auto& d : descs)
            if (d.key != nullptr && key == d.key) { match = &d; break; }
        if (match == nullptr)
        {
            if (warn)
                std::fprintf(stderr, "warning: unknown parameter %s, ignored\n", key.c_str());
            continue;
        }
        const float v = std::clamp(static_cast<float>(value.asNumber()), match->minValue, match->maxValue);
        engine.setParameter(match->id, v);
    }
}

bool hasKeyWithPrefix(const Value::Object& params, const char* prefix)
{
    for (const auto& [key, value] : params)
        if (key.rfind(prefix, 0) == 0)
            return true;
    return false;
}

// Hardware channel that carries the preset's sound when the caller gives none:
// NES presets that only set triangle/noise/DMC keys play on that channel, Genesis presets
// without FM operator keys play on a PSG tone (or noise) channel or the DAC channel.
int guessChannel(chipdsp::ChipId chip, const Value& preset)
{
    const Value* p = preset.find("params");
    if (p == nullptr || !p->isObject())
        return 0;
    const auto& params = p->asObject();
    switch (chip)
    {
        case chipdsp::ChipId::Nes:
        {
            const bool pulse = hasKeyWithPrefix(params, "p1_") || hasKeyWithPrefix(params, "p2_");
            if (pulse) return 0;
            if (hasKeyWithPrefix(params, "tri_")) return 2;
            if (hasKeyWithPrefix(params, "nz_")) return 3;
            if (hasKeyWithPrefix(params, "dmc_")) return 4;
            return 0;
        }
        case chipdsp::ChipId::Genesis:
        {
            const bool fm = hasKeyWithPrefix(params, "op") || params.count("algorithm") > 0;
            if (fm) return 0;
            if (params.count("dac_enable") > 0) return 5;
            if (params.count("psgn_att") > 0 && !hasKeyWithPrefix(params, "psg1_") &&
                !hasKeyWithPrefix(params, "psg2_") && !hasKeyWithPrefix(params, "psg3_"))
                return 9;
            if (hasKeyWithPrefix(params, "psg")) return 6;
            return 0;
        }
        case chipdsp::ChipId::Snes:
            return 0;
    }
    return 0;
}

// ----- rendering --------------------------------------------------------------------------------

struct RenderOptions
{
    int note = 60;
    double seconds = 2.0;
    double rate = 48000.0;
    int velocity = 100; // MIDI 0..127
    int channel = -1;   // -1: guess from the preset
};

// Engine must already be prepared at opts.rate.
void renderPreset(chipdsp::IChipEngine& engine, const Value& preset, const RenderOptions& opts, bool warn,
                  std::vector<float>& left, std::vector<float>& right)
{
    const int total = std::max(1, static_cast<int>(std::lround(opts.seconds * opts.rate)));
    const int noteOffAt = static_cast<int>(std::lround(static_cast<double>(total) * kNoteOnFraction));
    left.assign(static_cast<size_t>(total), 0.0f);
    right.assign(static_cast<size_t>(total), 0.0f);

    engine.reset();
    applyPreset(engine, preset, warn);

    int channel = opts.channel >= 0 ? opts.channel : guessChannel(engine.chipId(), preset);
    channel = std::clamp(channel, 0, engine.numChannels() - 1);
    const float velocity = static_cast<float>(std::clamp(opts.velocity, 0, 127)) / 127.0f;

    engine.noteOn(channel, static_cast<float>(opts.note), velocity);
    bool released = false;
    for (int pos = 0; pos < total; pos += kBlockSize)
    {
        if (!released && pos >= noteOffAt)
        {
            engine.noteOff(channel);
            released = true;
        }
        const int n = std::min(kBlockSize, total - pos);
        engine.renderBlock(left.data() + pos, right.data() + pos, nullptr, nullptr, n);
    }
}

// ----- WAV --------------------------------------------------------------------------------------

void put16(std::string& out, uint16_t v)
{
    out += static_cast<char>(v & 0xFF);
    out += static_cast<char>((v >> 8) & 0xFF);
}

void put32(std::string& out, uint32_t v)
{
    put16(out, static_cast<uint16_t>(v & 0xFFFF));
    put16(out, static_cast<uint16_t>((v >> 16) & 0xFFFF));
}

std::string wavStereo16(const std::vector<float>& left, const std::vector<float>& right, int rate)
{
    const uint32_t frames = static_cast<uint32_t>(left.size());
    const uint32_t dataBytes = frames * 4u;
    std::string out;
    out.reserve(44 + dataBytes);
    out += "RIFF";
    put32(out, 36u + dataBytes);
    out += "WAVE";
    out += "fmt ";
    put32(out, 16u);
    put16(out, 1u);                                  // PCM
    put16(out, 2u);                                  // stereo
    put32(out, static_cast<uint32_t>(rate));
    put32(out, static_cast<uint32_t>(rate) * 4u);    // byte rate
    put16(out, 4u);                                  // block align
    put16(out, 16u);                                 // bits per sample
    out += "data";
    put32(out, dataBytes);
    for (uint32_t i = 0; i < frames; ++i)
    {
        const float l = std::clamp(left[i], -1.0f, 1.0f);
        const float r = std::clamp(right[i], -1.0f, 1.0f);
        put16(out, static_cast<uint16_t>(static_cast<int16_t>(std::lround(l * 32767.0f))));
        put16(out, static_cast<uint16_t>(static_cast<int16_t>(std::lround(r * 32767.0f))));
    }
    return out;
}

// ----- features ---------------------------------------------------------------------------------

// In-place iterative radix-2 FFT; a.size() must be a power of two.
void fft(std::vector<std::complex<double>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    const double pi = 3.14159265358979323846;
    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double angle = -2.0 * pi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len)
        {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k)
            {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

double hzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double melToHz(double mel) { return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0); }

// 40 log-mel band energies (dB, floored at kMelFloorDb) of the averaged power spectrum
// over Hann-windowed frames of kFftSize with hop kFftHop.
std::vector<double> melFeatures(const std::vector<float>& mono, double rate)
{
    const size_t bins = static_cast<size_t>(kFftSize / 2 + 1);
    std::vector<double> power(bins, 0.0);
    std::vector<double> window(static_cast<size_t>(kFftSize));
    const double pi = 3.14159265358979323846;
    double windowSum = 0.0;
    for (int i = 0; i < kFftSize; ++i)
    {
        window[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i) / kFftSize);
        windowSum += window[static_cast<size_t>(i)];
    }
    const double norm = 1.0 / (windowSum * windowSum);

    int frames = 0;
    std::vector<std::complex<double>> buffer(static_cast<size_t>(kFftSize));
    for (size_t start = 0; start + static_cast<size_t>(kFftSize) <= mono.size(); start += static_cast<size_t>(kFftHop))
    {
        for (size_t i = 0; i < static_cast<size_t>(kFftSize); ++i)
            buffer[i] = std::complex<double>(static_cast<double>(mono[start + i]) * window[i], 0.0);
        fft(buffer);
        for (size_t b = 0; b < bins; ++b)
            power[b] += std::norm(buffer[b]) * norm;
        ++frames;
    }
    if (frames > 0)
        for (auto& p : power)
            p /= frames;

    // Triangular filters equally spaced on the mel scale between 0 Hz and Nyquist.
    const double melHi = hzToMel(rate / 2.0);
    std::vector<double> edges(static_cast<size_t>(kMelBands + 2));
    for (size_t i = 0; i < edges.size(); ++i)
        edges[i] = melToHz(melHi * static_cast<double>(i) / static_cast<double>(kMelBands + 1)) * kFftSize / rate;

    std::vector<double> out(static_cast<size_t>(kMelBands), 0.0);
    for (size_t band = 0; band < static_cast<size_t>(kMelBands); ++band)
    {
        const double lo = edges[band], mid = edges[band + 1], hi = edges[band + 2];
        double energy = 0.0;
        for (size_t b = 0; b < bins; ++b)
        {
            const double x = static_cast<double>(b);
            double w = 0.0;
            if (x >= lo && x <= mid && mid > lo)
                w = (x - lo) / (mid - lo);
            else if (x > mid && x <= hi && hi > mid)
                w = (hi - x) / (hi - mid);
            energy += w * power[b];
        }
        out[band] = std::max(kMelFloorDb, 10.0 * std::log10(std::max(energy, 1e-30)));
    }
    return out;
}

// 20 RMS points over equal segments (linear amplitude).
std::vector<double> envelopeFeatures(const std::vector<float>& mono)
{
    std::vector<double> out(static_cast<size_t>(kEnvelopePoints), 0.0);
    const size_t n = mono.size();
    for (size_t k = 0; k < static_cast<size_t>(kEnvelopePoints); ++k)
    {
        const size_t begin = n * k / static_cast<size_t>(kEnvelopePoints);
        const size_t end = n * (k + 1) / static_cast<size_t>(kEnvelopePoints);
        double sum = 0.0;
        for (size_t i = begin; i < end; ++i)
            sum += static_cast<double>(mono[i]) * static_cast<double>(mono[i]);
        out[k] = end > begin ? std::sqrt(sum / static_cast<double>(end - begin)) : 0.0;
    }
    return out;
}

Value toArray(const std::vector<double>& values)
{
    Value::Array a;
    a.reserve(values.size());
    for (double v : values)
        a.emplace_back(v);
    return Value(std::move(a));
}

// ----- commands ---------------------------------------------------------------------------------

int usage()
{
    std::fprintf(stderr,
                 "usage:\n"
                 "  chiptool dump-params <nes|snes|genesis>\n"
                 "  chiptool render <preset.json> <out.wav> [--note 60] [--seconds 2] [--rate 48000]\n"
                 "                  [--velocity 100] [--channel N]\n"
                 "  chiptool features <bank.json> <features.json> [--jobs N]\n");
    return 1;
}

bool optionValue(int argc, char** argv, int& i, const char* name, std::string& out)
{
    if (std::strcmp(argv[i], name) != 0)
        return false;
    if (i + 1 >= argc)
        throw std::runtime_error(std::string("missing value for ") + name);
    out = argv[++i];
    return true;
}

int cmdDumpParams(const std::string& chipKey)
{
    chipdsp::ChipId chip;
    if (!chipFromKey(chipKey, chip))
        throw std::runtime_error("unknown chip " + chipKey);
    auto engine = chipdsp::createEngine(chip);
    const std::string text = minijson::serialize(paramTableJson(*engine, "chiptool dump-params"));
    std::fwrite(text.data(), 1, text.size(), stdout);
    return 0;
}

int cmdRender(int argc, char** argv)
{
    if (argc < 4)
        return usage();
    const std::string presetPath = argv[2];
    const std::string wavPath = argv[3];
    RenderOptions opts;
    for (int i = 4; i < argc; ++i)
    {
        std::string v;
        if (optionValue(argc, argv, i, "--note", v))          opts.note = std::stoi(v);
        else if (optionValue(argc, argv, i, "--seconds", v))  opts.seconds = std::stod(v);
        else if (optionValue(argc, argv, i, "--rate", v))     opts.rate = std::stod(v);
        else if (optionValue(argc, argv, i, "--velocity", v)) opts.velocity = std::stoi(v);
        else if (optionValue(argc, argv, i, "--channel", v))  opts.channel = std::stoi(v);
        else throw std::runtime_error(std::string("unknown option ") + argv[i]);
    }
    if (opts.seconds <= 0.0 || opts.rate < 8000.0)
        throw std::runtime_error("invalid --seconds or --rate");

    const Value preset = minijson::parse(readFile(presetPath));
    const Value* chipValue = preset.find("chip");
    if (chipValue == nullptr || !chipValue->isString())
        throw std::runtime_error("preset has no \"chip\" string");
    chipdsp::ChipId chip;
    if (!chipFromKey(chipValue->asString(), chip))
        throw std::runtime_error("unknown chip " + chipValue->asString());

    auto engine = chipdsp::createEngine(chip);
    engine->prepare(opts.rate, kBlockSize);
    std::vector<float> left, right;
    renderPreset(*engine, preset, opts, true, left, right);
    writeFile(wavPath, wavStereo16(left, right, static_cast<int>(std::lround(opts.rate))));
    return 0;
}

int cmdFeatures(int argc, char** argv)
{
    if (argc < 4)
        return usage();
    const std::string bankPath = argv[2];
    const std::string outPath = argv[3];
    int jobs = static_cast<int>(std::thread::hardware_concurrency()) - 1;
    for (int i = 4; i < argc; ++i)
    {
        std::string v;
        if (optionValue(argc, argv, i, "--jobs", v)) jobs = std::stoi(v);
        else throw std::runtime_error(std::string("unknown option ") + argv[i]);
    }
    jobs = std::max(1, jobs);

    const Value bank = minijson::parse(readFile(bankPath));
    if (!bank.isArray())
        throw std::runtime_error("bank must be a JSON array of presets");
    const auto& presets = bank.asArray();
    const size_t count = presets.size();
    jobs = static_cast<int>(std::min<size_t>(static_cast<size_t>(jobs), std::max<size_t>(count, 1)));

    std::vector<std::string> names(count);
    std::vector<Value> results(count);
    std::vector<std::string> errors(count);
    std::atomic<size_t> next{0};

    auto worker = [&]() {
        std::unique_ptr<chipdsp::IChipEngine> engines[3];
        std::vector<float> left, right, mono;
        RenderOptions opts;
        opts.note = kFeatureNote;
        opts.seconds = kFeatureSeconds;
        opts.rate = kFeatureRate;
        opts.velocity = kFeatureVelocity;
        for (;;)
        {
            const size_t index = next.fetch_add(1);
            if (index >= count)
                return;
            try
            {
                const Value& preset = presets[index];
                const Value* nameValue = preset.find("name");
                const Value* chipValue = preset.find("chip");
                if (nameValue == nullptr || !nameValue->isString())
                    throw std::runtime_error("preset without a \"name\"");
                if (chipValue == nullptr || !chipValue->isString())
                    throw std::runtime_error("preset without a \"chip\"");
                chipdsp::ChipId chip;
                if (!chipFromKey(chipValue->asString(), chip))
                    throw std::runtime_error("unknown chip " + chipValue->asString());
                auto& engine = engines[static_cast<int>(chip)];
                if (engine == nullptr)
                {
                    engine = chipdsp::createEngine(chip);
                    engine->prepare(kFeatureRate, kBlockSize);
                }
                renderPreset(*engine, preset, opts, false, left, right);
                mono.resize(left.size());
                for (size_t i = 0; i < left.size(); ++i)
                    mono[i] = 0.5f * (left[i] + right[i]);

                Value entry{Value::Object{}};
                entry["mel"] = toArray(melFeatures(mono, kFeatureRate));
                entry["env"] = toArray(envelopeFeatures(mono));
                names[index] = nameValue->asString();
                results[index] = std::move(entry);
            }
            catch (const std::exception& e)
            {
                errors[index] = e.what();
            }
        }
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < jobs; ++t)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();

    int failures = 0;
    Value::Object out;
    for (size_t i = 0; i < count; ++i)
    {
        if (!errors[i].empty())
        {
            std::fprintf(stderr, "error: preset %zu: %s\n", i, errors[i].c_str());
            ++failures;
            continue;
        }
        if (out.count(names[i]) > 0)
            std::fprintf(stderr, "warning: duplicate preset name %s, last one wins\n", names[i].c_str());
        out[names[i]] = std::move(results[i]);
    }
    writeFile(outPath, minijson::serialize(Value(std::move(out))));
    std::fprintf(stderr, "features: %zu presets, %d failures, %d threads -> %s\n", count - static_cast<size_t>(failures),
                 failures, jobs, outPath.c_str());
    return failures == 0 ? 0 : 2;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
        return usage();
    const std::string command = argv[1];
    try
    {
        if (command == "dump-params")
            return argc == 3 ? cmdDumpParams(argv[2]) : usage();
        if (command == "render")
            return cmdRender(argc, argv);
        if (command == "features")
            return cmdFeatures(argc, argv);
        return usage();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
}
