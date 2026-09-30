// chiptool: command-line companion of the preset generator (tools/gen_presets.py).
//
//   chiptool dump-params <nes|snes|genesis>
//       Parameter table of the engine as JSON on stdout, same schema as
//       tools/presetgen/params/<chip>.json.
//
//   chiptool render <preset.json> <out.wav> [--note 60] [--seconds 2] [--rate 48000]
//                   [--velocity 100] [--channel N] [--notes 60,62,64 [--step 0.4] [--gate 0.8]]
//       Applies the preset's "params" through the engine's parameterDescriptors() keys,
//       plays the note for 60 % of the duration and writes a stereo 16-bit WAV, scaled by
//       the preset's global.preset_gain (dB, as the plugin applies it after the chip).
//       --notes plays a monophonic phrase on the same channel instead: note i starts at
//       i * step seconds and is released gate * step seconds later; the render lasts
//       --seconds in total (the rest is the release tail).
//
//   chiptool features <bank.json> <features.json> [--jobs N] [--samples DIR]
//       Renders every preset of the bank twice at 48 kHz, each after a 0.4 s silent pre-roll
//       (C4 for 2 s held 1.4 s, and C3 for 0.5 s held 80 ms) and writes
//       {name: {"mel": [40 left + 40 right log-energies in dB], "env": [20 RMS points],
//               "pitch": [per-frame f0, cents re A4 + 10000, 0 = unvoiced],
//               "mel_short": [...], "env_short": [...], "pitch_short": [...],
//               "held_rms_db": R, "peak_db": P, "peak_short_db": S}}, computed
//       with an inline radix-2 FFT and a 40-band mel filterbank. R is the stereo RMS (dBFS,
//       full-scale sine = -3 dBFS) of the long pass between note-on and note-off over the
//       10 ms windows within 20 dB of the loudest one (the part where the note sounds); P and
//       S are the largest |sample| of either channel over each pass (dBFS). They measure the
//       chip output: preset_gain is not applied (tools/presetgen derives it from them).
//       The hardware channel is the lowest bit of the
//       preset's global.poly_channels. Presets are rendered in
//       parallel with std::thread, with a fresh engine instance per preset so that sample
//       memory (SNES APU RAM budget) never carries over from one preset to the next.
//
//   chiptool regs genesis <stimulus.vgm> <out.wav> [--rate 44100] [--lowpass] [--asic]
//   chiptool regs snes <stimulus.spc> <stimulus.events> <out.wav> [--seconds S]
//   chiptool regs nes <stimulus.vgm> <out.wav> [--rate 44100] [--kernel impulse|integrated]
//       Register-level render for the differential check against a reference emulator
//       (tools/refcheck). Genesis: plays the YM2612 (0x52/0x53) and SN76489 (0x50) writes and
//       waits of a VGM file straight into Ym2612Core / Sn76489Core at their VGM sample times
//       (44100 Hz base, converted to master clocks) and renders them through the engine's
//       output path: same event loop in master clocks, same BandLimitedStepSynth resamplers,
//       scales and PSG/FM ratio as GenesisEngine::renderChunk, then the 5 Hz coupling
//       capacitor and, with --lowpass, the Model 1 low-pass. Ladder effect on (discrete
//       YM2612) unless --asic. SNES: loads the SPC file's 64 KiB RAM image and DSP register
//       snapshot into SnesDsp, applies the "sample register value" writes of the events file
//       (no SPC700 CPU here: the stimulus generator lists what its SPC700 program writes and
//       when) and writes the native 32 kHz main output unchanged (16-bit, no resampling).
//       NES: plays the NES APU writes (0xB4) of a VGM 1.61 file straight into NesApu at the CPU
//       cycle of their VGM sample time, with the DMC sample memory from its "NES APU RAM write"
//       data blocks (0x67, type 0xC2) mapped at $C000, and renders through the output path of
//       Nes2A03Engine::renderChunk (one CPU cycle per step, NesMixer, BandLimitedStepSynth with
//       the engine's IntegratedStep kernel (--kernel impulse: the legacy ImpulseSum kernel the
//       engine used before 2026-09-30, for the refcheck F2 measurement), NesOutputStage with
//       console_filter = 0, i.e. only the 5 Hz DC blocker). No driver: nothing but the file's
//       writes reaches the chip. NTSC or PAL from the header clock.
//
//   Samples: a preset's "samples" object maps a slot parameter key to a sample name
//   ({"dmc_sample": "kick_short"}). The sample is looked up in <DIR>/index.json
//   (default: <bank dir>/../samples, then ./assets/samples), loaded into the slot given by
//   that parameter's value, and its root note / loop start are passed to setSampleInfo().
//   A preset whose sample cannot be found or does not fit is an error, never a silent render.
//
// Tool code: exceptions and allocation are fine here (nothing runs on an audio thread).

#include "MiniJson.h"

#include "chipdsp/EngineFactory.h"
#include "chipdsp/IChipEngine.h"
#include "chipdsp/genesis/GenesisTables.h"
#include "chipdsp/genesis/Sn76489Core.h"
#include "chipdsp/genesis/Ym2612Core.h"
#include "chipdsp/nes/NesApu.h"
#include "chipdsp/snes/SnesDsp.h"
#include "chipdsp/util/BandLimitedStepSynth.h"
#include "chipdsp/util/Filters.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
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
constexpr double kPreRollSeconds = 0.4;      // silent render between preset load and note-on
constexpr double kFeatureSeconds = 2.0;      // long pass: held 1.4 s (> the 1 s maximum vibrato delay)
constexpr double kFeatureHoldSeconds = 1.4;
constexpr double kShortSeconds = 0.5;        // short pass: 80 ms hit, exposes release/choke variants
constexpr double kShortHoldSeconds = 0.08;
// C3 for the short pass: keyed variants differ away from C4, and C3 sits inside the useful
// range of the NES keyed noise (notes 36..51 map to indexes 15..0; C4 and above clamp to 0).
constexpr int kShortNote = 48;
constexpr double kFeatureRate = 48000.0;
constexpr double kLevelWindowSeconds = 0.010;   // held_rms_db: 10 ms windows
constexpr double kLevelActiveRangeDb = 20.0;    // windows this far below the loudest are not the note
constexpr double kLevelFloorDb = -120.0;        // level of silence
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
    // Generated presets name their hardware channels in global.poly_channels (bit mask, as
    // the plugin's voice allocator uses it): play on the lowest channel of the mask.
    if (const Value* g = preset.find("global"); g != nullptr && g->isObject())
        if (const Value* mask = g->find("poly_channels"); mask != nullptr && mask->isNumber())
        {
            const auto bits = static_cast<unsigned>(std::lround(mask->asNumber()));
            for (int c = 0; c < chipdsp::kMaxHardwareChannels; ++c)
                if ((bits >> c) & 1u)
                    return c;
        }

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

// ----- samples ----------------------------------------------------------------------------------

// Mono float PCM from a 16-bit PCM WAV (multi-channel files are averaged).
std::vector<float> readWavMono(const std::string& path, double& rate)
{
    const std::string data = readFile(path);
    auto byteAt = [&](size_t o) { return static_cast<uint32_t>(static_cast<uint8_t>(data[o])); };
    auto u16 = [&](size_t o) { return byteAt(o) | (byteAt(o + 1) << 8); };
    auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
    if (data.size() < 12 || data.compare(0, 4, "RIFF") != 0 || data.compare(8, 4, "WAVE") != 0)
        throw std::runtime_error(path + ": not a RIFF/WAVE file");

    uint32_t format = 0, channels = 0, bits = 0;
    rate = 0.0;
    std::vector<float> mono;
    size_t pos = 12;
    while (pos + 8 <= data.size())
    {
        const std::string id = data.substr(pos, 4);
        const size_t size = u32(pos + 4);
        const size_t body = pos + 8;
        if (body + size > data.size())
            throw std::runtime_error(path + ": truncated chunk " + id);
        if (id == "fmt " && size >= 16)
        {
            format = u16(body);
            channels = u16(body + 2);
            rate = static_cast<double>(u32(body + 4));
            bits = u16(body + 14);
        }
        else if (id == "data")
        {
            if (format != 1 || bits != 16 || channels == 0)
                throw std::runtime_error(path + ": only 16-bit PCM WAV is supported");
            const size_t frames = size / (2u * channels);
            mono.resize(frames);
            for (size_t f = 0; f < frames; ++f)
            {
                double sum = 0.0;
                for (uint32_t c = 0; c < channels; ++c)
                    sum += static_cast<int16_t>(u16(body + 2 * (f * channels + c)));
                mono[f] = static_cast<float>(sum / channels / 32768.0);
            }
        }
        pos = body + size + (size & 1u);
    }
    if (mono.empty() || rate <= 0.0)
        throw std::runtime_error(path + ": no audio data");
    return mono;
}

struct SampleEntry
{
    std::vector<float> pcm;
    double rate = 0.0;
    float rootNote = 60.0f;
    int loopStart = -1;
};

// Read-only after load(); shared by the render threads.
class SampleLibrary
{
public:
    // Loads <dir>/index.json and every WAV it lists. Returns false when the index is missing.
    bool load(const std::filesystem::path& dir)
    {
        const auto indexPath = dir / "index.json";
        if (!std::filesystem::exists(indexPath))
            return false;
        const Value index = minijson::parse(readFile(indexPath.string()));
        if (!index.isArray())
            throw std::runtime_error(indexPath.string() + ": expected a JSON array");
        for (const auto& e : index.asArray())
        {
            const Value* chip = e.find("chip");
            const Value* name = e.find("name");
            const Value* file = e.find("file");
            if (chip == nullptr || name == nullptr || file == nullptr || !chip->isString() || !name->isString() ||
                !file->isString())
                throw std::runtime_error(indexPath.string() + ": entry without chip/name/file");
            SampleEntry s;
            s.pcm = readWavMono((dir / file->asString()).string(), s.rate);
            if (const Value* root = e.find("root_note"); root != nullptr && root->isNumber())
                s.rootNote = static_cast<float>(root->asNumber());
            if (const Value* loop = e.find("loop_start"); loop != nullptr && loop->isNumber())
                s.loopStart = static_cast<int>(std::lround(loop->asNumber()));
            entries[chip->asString() + "/" + name->asString()] = std::move(s);
        }
        return true;
    }

    const SampleEntry* find(const std::string& chip, const std::string& name) const
    {
        const auto it = entries.find(chip + "/" + name);
        return it != entries.end() ? &it->second : nullptr;
    }

    size_t size() const { return entries.size(); }

private:
    std::map<std::string, SampleEntry> entries;
};

// Explicit --samples dir, else <hint dir>/../samples, else ./assets/samples.
std::filesystem::path resolveSamplesDir(const std::string& explicitDir, const std::string& hintFile)
{
    if (!explicitDir.empty())
        return explicitDir;
    const auto fromHint = std::filesystem::absolute(hintFile).parent_path().parent_path() / "samples";
    if (std::filesystem::exists(fromHint / "index.json"))
        return fromHint;
    return std::filesystem::path("assets") / "samples";
}

// Loads the preset's samples into the slots named by its parameters. Must run after
// applyPreset(): the SNES budget depends on echo_delay.
void loadPresetSamples(chipdsp::IChipEngine& engine, const Value& preset, const SampleLibrary* library)
{
    const Value* samples = preset.find("samples");
    if (samples == nullptr || !samples->isObject() || samples->asObject().empty())
        return;
    if (library == nullptr || library->size() == 0)
        throw std::runtime_error("preset uses samples but no samples/index.json was found (use --samples DIR)");

    const Value* params = preset.find("params");
    const std::string chip = chipdsp::chipKey(engine.chipId());
    for (const auto& [slotKey, nameValue] : samples->asObject())
    {
        if (!nameValue.isString())
            throw std::runtime_error("samples." + slotKey + " is not a string");
        int slot = 0;
        if (params != nullptr && params->isObject())
            if (const Value* v = params->find(slotKey); v != nullptr && v->isNumber())
                slot = static_cast<int>(std::lround(v->asNumber()));
        const SampleEntry* s = library->find(chip, nameValue.asString());
        if (s == nullptr)
            throw std::runtime_error("unknown sample " + chip + "/" + nameValue.asString());
        if (!engine.loadSample(slot, s->pcm.data(), static_cast<int>(s->pcm.size()), s->rate))
            throw std::runtime_error("loadSample failed for " + nameValue.asString() + " in slot " +
                                     std::to_string(slot) + " (invalid slot or memory budget exceeded)");
        engine.setSampleInfo(slot, s->rootNote, s->loopStart, s->rate); // false = no metadata on this chip
    }
}

// ----- rendering --------------------------------------------------------------------------------

struct RenderOptions
{
    int note = 60;
    double seconds = 2.0;
    double rate = 48000.0;
    int velocity = 100; // MIDI 0..127
    int channel = -1;   // -1: guess from the preset
    double holdSeconds = -1.0; // note-off time; < 0: kNoteOnFraction of 'seconds'
    std::vector<int> notes;    // non-empty: phrase mode (replaces 'note' and 'holdSeconds')
    double stepSeconds = 0.4;  // phrase: time between note-ons
    double gate = 0.8;         // phrase: held fraction of each step
};

// Engine must already be prepared at opts.rate.
void renderPreset(chipdsp::IChipEngine& engine, const Value& preset, const RenderOptions& opts, bool warn,
                  const SampleLibrary* library, std::vector<float>& left, std::vector<float>& right)
{
    const int total = std::max(1, static_cast<int>(std::lround(opts.seconds * opts.rate)));
    const int noteOffAt = opts.holdSeconds >= 0.0
                              ? static_cast<int>(std::lround(opts.holdSeconds * opts.rate))
                              : static_cast<int>(std::lround(static_cast<double>(total) * kNoteOnFraction));
    left.assign(static_cast<size_t>(total), 0.0f);
    right.assign(static_cast<size_t>(total), 0.0f);

    engine.reset();
    applyPreset(engine, preset, warn);
    loadPresetSamples(engine, preset, library);

    // Pre-roll (discarded): a preset is loaded before it is played. This lets the drivers
    // finish their set-up, e.g. the SNES driver keeps echo writes off for 240 ms after it
    // programs EDL (research "Echo"), which would otherwise hide the echo of short notes.
    {
        std::vector<float> scratchL(static_cast<size_t>(kBlockSize)), scratchR(static_cast<size_t>(kBlockSize));
        const int preRoll = static_cast<int>(std::lround(kPreRollSeconds * opts.rate));
        for (int pos = 0; pos < preRoll; pos += kBlockSize)
            engine.renderBlock(scratchL.data(), scratchR.data(), nullptr, nullptr, std::min(kBlockSize, preRoll - pos));
    }

    int channel = opts.channel >= 0 ? opts.channel : guessChannel(engine.chipId(), preset);
    channel = std::clamp(channel, 0, engine.numChannels() - 1);
    const float velocity = static_cast<float>(std::clamp(opts.velocity, 0, 127)) / 127.0f;

    if (opts.notes.empty())
    {
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
        return;
    }

    // Phrase: events at exact sample positions, blocks split at each event.
    struct Event { int at; bool on; int note; };
    std::vector<Event> events;
    const int step = static_cast<int>(std::lround(opts.stepSeconds * opts.rate));
    const int hold = std::max(1, static_cast<int>(std::lround(opts.gate * opts.stepSeconds * opts.rate)));
    for (size_t i = 0; i < opts.notes.size(); ++i)
    {
        const int start = static_cast<int>(i) * step;
        events.push_back({ start, true, opts.notes[i] });
        events.push_back({ start + hold, false, opts.notes[i] });
    }
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
        return a.at != b.at ? a.at < b.at : (!a.on && b.on); // a note-off before a note-on at the same time
    });
    size_t next = 0;
    for (int pos = 0; pos < total;)
    {
        while (next < events.size() && events[next].at <= pos)
        {
            if (events[next].on)
                engine.noteOn(channel, static_cast<float>(events[next].note), velocity);
            else
                engine.noteOff(channel);
            ++next;
        }
        int n = std::min(kBlockSize, total - pos);
        if (next < events.size())
            n = std::min(n, events[next].at - pos);
        engine.renderBlock(left.data() + pos, right.data() + pos, nullptr, nullptr, n);
        pos += n;
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

// 16-bit WAV whose samples are exactly round(x * 32768), clamped: an int16 divided by 32768
// comes back unchanged (bit-exact SNES output).
std::string wavStereo16Exact(const std::vector<float>& left, const std::vector<float>& right, int rate)
{
    std::string out = wavStereo16(left, right, rate);
    const auto q = [](float x) {
        return static_cast<uint16_t>(static_cast<int16_t>(std::clamp(std::lround(x * 32768.0f), -32768L, 32767L)));
    };
    for (size_t i = 0; i < left.size(); ++i)
    {
        const uint16_t l = q(left[i]), r = q(right[i]);
        out[44 + 4 * i + 0] = static_cast<char>(l & 0xFF);
        out[44 + 4 * i + 1] = static_cast<char>(l >> 8);
        out[44 + 4 * i + 2] = static_cast<char>(r & 0xFF);
        out[44 + 4 * i + 3] = static_cast<char>(r >> 8);
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

// 40 log-mel band energies (dB, floored at kMelFloorDb) of the power spectrum averaged over
// the active Hann-windowed frames (kFftSize, hop kFftHop): frames within kActiveFrameDb of
// the loudest frame. Averaging only active frames keeps a 30 ms drum hit from being
// flattened into the silence that follows it.
std::vector<double> melFeatures(const std::vector<float>& mono, double rate)
{
    constexpr double kActiveFrameDb = 30.0;
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

    std::vector<std::vector<double>> framePower;
    std::vector<double> frameEnergy;
    std::vector<std::complex<double>> buffer(static_cast<size_t>(kFftSize));
    for (size_t start = 0; start + static_cast<size_t>(kFftSize) <= mono.size(); start += static_cast<size_t>(kFftHop))
    {
        for (size_t i = 0; i < static_cast<size_t>(kFftSize); ++i)
            buffer[i] = std::complex<double>(static_cast<double>(mono[start + i]) * window[i], 0.0);
        fft(buffer);
        std::vector<double> p(bins);
        double e = 0.0;
        for (size_t b = 0; b < bins; ++b)
        {
            p[b] = std::norm(buffer[b]) * norm;
            e += p[b];
        }
        framePower.push_back(std::move(p));
        frameEnergy.push_back(e);
    }
    const double loudest = frameEnergy.empty() ? 0.0 : *std::max_element(frameEnergy.begin(), frameEnergy.end());
    const double gate = loudest * std::pow(10.0, -kActiveFrameDb / 10.0);
    int frames = 0;
    for (size_t f = 0; f < framePower.size(); ++f)
    {
        if (loudest <= 0.0 || frameEnergy[f] < gate)
            continue;
        for (size_t b = 0; b < bins; ++b)
            power[b] += framePower[f][b];
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

// Per-frame fundamental in cents relative to A4, 0 for unvoiced/silent frames (frames of
// kFftSize, hop kFftHop). Autocorrelation through the FFT (Wiener-Khinchin) of a Hann-
// windowed frame, normalised by the window's own autocorrelation; the smallest lag whose
// value reaches 90 % of the best peak above kVoicedThreshold wins (avoids sub-octave
// picks), refined by parabolic interpolation. Used to tell vibrato/pitch-envelope variants
// apart, which an averaged spectrum cannot.
std::vector<double> pitchTrack(const std::vector<float>& mono, double rate)
{
    constexpr int kPadded = 2 * kFftSize;
    constexpr double kVoicedThreshold = 0.6;
    const int minLag = std::max(2, static_cast<int>(rate / 2000.0));
    const int maxLag = std::min(kFftSize / 2, static_cast<int>(rate / 50.0));
    constexpr double pi = 3.14159265358979323846;

    // Autocorrelation of the window itself, for the normalisation.
    std::vector<std::complex<double>> w(kPadded);
    std::vector<double> window(static_cast<size_t>(kFftSize));
    for (int i = 0; i < kFftSize; ++i)
    {
        window[static_cast<size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * pi * i / (kFftSize - 1));
        w[static_cast<size_t>(i)] = window[static_cast<size_t>(i)];
    }
    fft(w);
    for (auto& c : w)
        c = std::norm(c);
    fft(w);
    const double w0 = w[0].real();

    std::vector<double> out;
    std::vector<std::complex<double>> a(kPadded);
    for (size_t start = 0; start + static_cast<size_t>(kFftSize) <= mono.size(); start += static_cast<size_t>(kFftHop))
    {
        double energy = 0.0;
        for (int i = 0; i < kPadded; ++i)
        {
            const double x = i < kFftSize ? static_cast<double>(mono[start + static_cast<size_t>(i)]) * window[static_cast<size_t>(i)] : 0.0;
            a[static_cast<size_t>(i)] = x;
            energy += x * x;
        }
        if (energy < 1e-7)
        {
            out.push_back(0.0);
            continue;
        }
        fft(a);
        for (auto& c : a)
            c = std::norm(c);
        fft(a); // real, symmetric: forward FFT = kPadded * autocorrelation
        const double r0 = a[0].real();
        auto norm = [&](int lag) {
            const double wl = w[static_cast<size_t>(lag)].real() / w0;
            return wl > 1e-6 ? (a[static_cast<size_t>(lag)].real() / r0) / wl : 0.0;
        };
        double best = 0.0;
        for (int lag = minLag; lag <= maxLag; ++lag)
            best = std::max(best, norm(lag));
        if (best < kVoicedThreshold)
        {
            out.push_back(0.0);
            continue;
        }
        int pick = -1;
        for (int lag = minLag + 1; lag < maxLag; ++lag)
        {
            const double v = norm(lag);
            if (v >= 0.9 * best && v >= norm(lag - 1) && v >= norm(lag + 1))
            {
                pick = lag;
                break;
            }
        }
        if (pick < 0)
        {
            out.push_back(0.0);
            continue;
        }
        const double ym = norm(pick - 1), y0 = norm(pick), yp = norm(pick + 1);
        const double denom = ym - 2.0 * y0 + yp;
        const double shift = std::abs(denom) > 1e-12 ? 0.5 * (ym - yp) / denom : 0.0;
        const double f0 = rate / (static_cast<double>(pick) + std::clamp(shift, -0.5, 0.5));
        out.push_back(1200.0 * std::log2(f0 / 440.0) + 10000.0); // +10000 keeps voiced frames non-zero
    }
    return out;
}

// RMS envelope in consecutive 10 ms windows (linear amplitude): 120 points for the long
// pass, 50 for the short one. 10 ms resolves the 1-frame (16.7 ms) steps of the software
// envelopes the NES and PSG drivers run.
std::vector<double> envelopeFeatures(const std::vector<float>& mono)
{
    const size_t window = static_cast<size_t>(kFeatureRate * 0.010);
    const size_t n = mono.size();
    const size_t points = std::max<size_t>(1, n / window);
    std::vector<double> out(points, 0.0);
    for (size_t k = 0; k < points; ++k)
    {
        const size_t begin = k * window;
        const size_t end = std::min(n, begin + window);
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

// Playing level of a stereo render (see the features description at the top of the file).
double toDb(double linear)
{
    return linear > 0.0 ? std::max(kLevelFloorDb, 20.0 * std::log10(linear)) : kLevelFloorDb;
}

double heldRmsDb(const std::vector<float>& left, const std::vector<float>& right, size_t heldSamples)
{
    const size_t window = static_cast<size_t>(std::lround(kFeatureRate * kLevelWindowSeconds));
    const size_t n = std::min({ heldSamples, left.size(), right.size() });
    std::vector<double> power;   // mean square of L and R per window
    for (size_t begin = 0; begin + window <= n; begin += window)
    {
        double sum = 0.0;
        for (size_t i = begin; i < begin + window; ++i)
            sum += static_cast<double>(left[i]) * left[i] + static_cast<double>(right[i]) * right[i];
        power.push_back(sum / static_cast<double>(2 * window));
    }
    const double loudest = power.empty() ? 0.0 : *std::max_element(power.begin(), power.end());
    if (loudest <= 0.0)
        return kLevelFloorDb;
    const double threshold = loudest * std::pow(10.0, -kLevelActiveRangeDb / 10.0);
    double sum = 0.0;
    int count = 0;
    for (double p : power)
        if (p >= threshold)
        {
            sum += p;
            ++count;
        }
    return toDb(std::sqrt(sum / count));
}

double peakDb(const std::vector<float>& left, const std::vector<float>& right)
{
    float peak = 0.0f;
    for (size_t i = 0; i < left.size(); ++i)
        peak = std::max({ peak, std::abs(left[i]), std::abs(right[i]) });
    return toDb(peak);
}

// global.preset_gain of a preset (dB, clamped to the plugin's -24..+36 range) as a linear gain.
float presetGain(const Value& preset)
{
    if (const Value* g = preset.find("global"); g != nullptr && g->isObject())
        if (const Value* db = g->find("preset_gain"); db != nullptr && db->isNumber())
            return static_cast<float>(std::pow(10.0, std::clamp(db->asNumber(), -24.0, 36.0) / 20.0));
    return 1.0f;
}

// ----- commands ---------------------------------------------------------------------------------

int usage()
{
    std::fprintf(stderr,
                 "usage:\n"
                 "  chiptool dump-params <nes|snes|genesis>\n"
                 "  chiptool render <preset.json> <out.wav> [--note 60] [--seconds 2] [--rate 48000]\n"
                 "                  [--velocity 100] [--channel N] [--samples DIR]\n"
                 "                  [--notes 60,62,64 [--step 0.4] [--gate 0.8]]\n"
                 "  chiptool features <bank.json> <features.json> [--jobs N] [--samples DIR]\n"
                 "  chiptool regs genesis <stimulus.vgm> <out.wav> [--rate 44100] [--lowpass] [--asic]\n"
                 "  chiptool regs snes <stimulus.spc> <stimulus.events> <out.wav> [--seconds S]\n"
                 "  chiptool regs nes <stimulus.vgm> <out.wav> [--rate 44100] [--kernel impulse|integrated]\n");
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
    std::string samplesDir;
    for (int i = 4; i < argc; ++i)
    {
        std::string v;
        if (optionValue(argc, argv, i, "--note", v))          opts.note = std::stoi(v);
        else if (optionValue(argc, argv, i, "--seconds", v))  opts.seconds = std::stod(v);
        else if (optionValue(argc, argv, i, "--rate", v))     opts.rate = std::stod(v);
        else if (optionValue(argc, argv, i, "--velocity", v)) opts.velocity = std::stoi(v);
        else if (optionValue(argc, argv, i, "--channel", v))  opts.channel = std::stoi(v);
        else if (optionValue(argc, argv, i, "--samples", v))  samplesDir = v;
        else if (optionValue(argc, argv, i, "--step", v))     opts.stepSeconds = std::stod(v);
        else if (optionValue(argc, argv, i, "--gate", v))     opts.gate = std::stod(v);
        else if (optionValue(argc, argv, i, "--notes", v))
        {
            std::stringstream list(v);
            for (std::string item; std::getline(list, item, ',');)
                opts.notes.push_back(std::stoi(item));
        }
        else throw std::runtime_error(std::string("unknown option ") + argv[i]);
    }
    if (opts.seconds <= 0.0 || opts.rate < 8000.0)
        throw std::runtime_error("invalid --seconds or --rate");
    if (opts.stepSeconds <= 0.0 || opts.gate <= 0.0 || opts.gate > 1.0)
        throw std::runtime_error("invalid --step or --gate (step > 0, 0 < gate <= 1)");

    const Value preset = minijson::parse(readFile(presetPath));
    const Value* chipValue = preset.find("chip");
    if (chipValue == nullptr || !chipValue->isString())
        throw std::runtime_error("preset has no \"chip\" string");
    chipdsp::ChipId chip;
    if (!chipFromKey(chipValue->asString(), chip))
        throw std::runtime_error("unknown chip " + chipValue->asString());

    SampleLibrary library;
    library.load(resolveSamplesDir(samplesDir, presetPath));

    auto engine = chipdsp::createEngine(chip);
    engine->prepare(opts.rate, kBlockSize);
    std::vector<float> left, right;
    renderPreset(*engine, preset, opts, true, &library, left, right);
    const float gain = presetGain(preset);
    for (size_t i = 0; i < left.size(); ++i)
    {
        left[i] *= gain;
        right[i] *= gain;
    }
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
    std::string samplesDir;
    for (int i = 4; i < argc; ++i)
    {
        std::string v;
        if (optionValue(argc, argv, i, "--jobs", v)) jobs = std::stoi(v);
        else if (optionValue(argc, argv, i, "--samples", v)) samplesDir = v;
        else throw std::runtime_error(std::string("unknown option ") + argv[i]);
    }
    jobs = std::max(1, jobs);

    SampleLibrary library;
    library.load(resolveSamplesDir(samplesDir, bankPath));

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
        std::vector<float> left, right, mono;
        RenderOptions opts;
        opts.note = kFeatureNote;
        opts.seconds = kFeatureSeconds;
        opts.holdSeconds = kFeatureHoldSeconds;
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
                // Two passes, each with a fresh engine: "mel" holds 40 bands of the left
                // channel then 40 of the right (so pan and stereo echo count), "env" the
                // 20-point RMS envelope of the mid signal.
                Value entry{Value::Object{}};
                for (int pass = 0; pass < 2; ++pass)
                {
                    RenderOptions passOpts = opts;
                    if (pass == 1)
                    {
                        passOpts.seconds = kShortSeconds;
                        passOpts.holdSeconds = kShortHoldSeconds;
                        passOpts.note = kShortNote;
                    }
                    auto engine = chipdsp::createEngine(chip);
                    engine->prepare(kFeatureRate, kBlockSize);
                    renderPreset(*engine, preset, passOpts, false, &library, left, right);
                    mono.resize(left.size());
                    for (size_t i = 0; i < left.size(); ++i)
                        mono[i] = 0.5f * (left[i] + right[i]);
                    std::vector<double> mel = melFeatures(left, kFeatureRate);
                    const std::vector<double> melRight = melFeatures(right, kFeatureRate);
                    mel.insert(mel.end(), melRight.begin(), melRight.end());
                    entry[pass == 0 ? "mel" : "mel_short"] = toArray(mel);
                    entry[pass == 0 ? "env" : "env_short"] = toArray(envelopeFeatures(mono));
                    entry[pass == 0 ? "pitch" : "pitch_short"] = toArray(pitchTrack(mono, kFeatureRate));
                    entry[pass == 0 ? "peak_db" : "peak_short_db"] = Value(peakDb(left, right));
                    if (pass == 0)
                        entry["held_rms_db"] = Value(heldRmsDb(left, right,
                                                               static_cast<size_t>(std::lround(kFeatureHoldSeconds * kFeatureRate))));
                }
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


// ----- register-level renders (regs) -----------------------------------------------------------

struct VgmEvent
{
    uint32_t sample = 0;   // VGM sample time (44100 Hz)
    int kind = 0;          // 0 = YM2612 bank 0, 1 = YM2612 bank 1, 2 = PSG
    uint8_t reg = 0;
    uint8_t value = 0;
};

// Parses the commands the refcheck stimuli use (VGM specification: 0x50, 0x52, 0x53, 0x61,
// 0x62, 0x63, 0x7n, 0x66). Anything else is an error, never silently skipped.
std::vector<VgmEvent> parseVgm(const std::string& data, uint32_t& totalSamples)
{
    auto u8 = [&](size_t o) {
        if (o >= data.size())
            throw std::runtime_error("VGM: truncated");
        return static_cast<uint8_t>(data[o]);
    };
    auto u32 = [&](size_t o) {
        return static_cast<uint32_t>(u8(o)) | (static_cast<uint32_t>(u8(o + 1)) << 8) |
               (static_cast<uint32_t>(u8(o + 2)) << 16) | (static_cast<uint32_t>(u8(o + 3)) << 24);
    };
    if (data.size() < 0x40 || data.compare(0, 4, "Vgm ") != 0)
        throw std::runtime_error("not a VGM file");
    const uint32_t version = u32(0x08);
    totalSamples = u32(0x18);
    size_t pos = (version >= 0x150 && u32(0x34) != 0) ? 0x34 + u32(0x34) : 0x40;
    std::vector<VgmEvent> events;
    uint32_t now = 0;
    for (;;)
    {
        const uint8_t cmd = u8(pos);
        if (cmd == 0x66)
            break;
        if (cmd == 0x50)
        {
            events.push_back({ now, 2, 0, u8(pos + 1) });
            pos += 2;
        }
        else if (cmd == 0x52 || cmd == 0x53)
        {
            events.push_back({ now, cmd - 0x52, u8(pos + 1), u8(pos + 2) });
            pos += 3;
        }
        else if (cmd == 0x61)
        {
            now += static_cast<uint32_t>(u8(pos + 1)) | (static_cast<uint32_t>(u8(pos + 2)) << 8);
            pos += 3;
        }
        else if (cmd == 0x62) { now += 735; pos += 1; }
        else if (cmd == 0x63) { now += 882; pos += 1; }
        else if ((cmd & 0xF0) == 0x70) { now += (cmd & 15u) + 1u; pos += 1; }
        else
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, "VGM: unsupported command 0x%02X at 0x%zX", cmd, pos);
            throw std::runtime_error(buf);
        }
    }
    totalSamples = std::max(totalSamples, now);
    return events;
}

// Mirror of GenesisEngine::OutputFilters (coupling capacitor, optional Model 1 low-pass).
struct GenesisOutputFilter
{
    chipdsp::OnePoleHighPass dc;
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f;
    void prepare(double rate)
    {
        dc.prepare(chipdsp::genesis::kDcBlockHz, rate);
        const auto c = chipdsp::genesis::rcLowPass(chipdsp::genesis::kModel1LowPassHz, rate);
        b0 = static_cast<float>(c.b0);
        b1 = static_cast<float>(c.b1);
        b2 = static_cast<float>(c.b2);
        a1 = static_cast<float>(c.a1);
    }
    float process(float x, bool lowPassOn)
    {
        const float y = dc.process(x);
        const float z = b0 * y + b1 * x1 + b2 * x2 - a1 * y1;
        x2 = x1;
        x1 = y;
        y1 = z;
        return lowPassOn ? z : y;
    }
};

int cmdRegsGenesis(int argc, char** argv)
{
    using namespace chipdsp::genesis;
    if (argc < 5)
        return usage();
    const std::string vgmPath = argv[3];
    const std::string wavPath = argv[4];
    double rate = 44100.0;
    bool lowPass = false;
    bool ladder = true;
    for (int i = 5; i < argc; ++i)
    {
        std::string v;
        if (optionValue(argc, argv, i, "--rate", v)) rate = std::stod(v);
        else if (std::strcmp(argv[i], "--lowpass") == 0) lowPass = true;
        else if (std::strcmp(argv[i], "--asic") == 0) ladder = false;
        else throw std::runtime_error(std::string("unknown option ") + argv[i]);
    }
    uint32_t totalSamples = 0;
    const std::vector<VgmEvent> events = parseVgm(readFile(vgmPath), totalSamples);

    const chipdsp::ClockStandard clockStd = chipdsp::ClockStandard::Ntsc;
    const double master = masterClock(clockStd);
    const double hostPerMaster = rate / master;
    const double masterPerVgm = master / 44100.0;

    Ym2612Core ym;
    Sn76489Core psg;
    ym.setLadderEffect(ladder);
    chipdsp::BandLimitedStepSynth fmL, fmR, psgSynth;
    constexpr auto kKernel = chipdsp::BandLimitedStepSynth::Kernel::IntegratedStep;   // as GenesisEngine
    fmL.prepare(fmSampleRate(clockStd), rate, kBlockSize, kKernel);
    fmR.prepare(fmSampleRate(clockStd), rate, kBlockSize, kKernel);
    psgSynth.prepare(psgTickRate(clockStd), rate, kBlockSize, kKernel);
    GenesisOutputFilter filterL, filterR;
    filterL.prepare(rate);
    filterR.prepare(rate);

    // As GenesisEngine::reset(): settle the idle chip, then start the level trackers there.
    for (int i = 0; i < 16; ++i)
    {
        ym.clockSample();
        psg.clock();
    }
    int levelL = ym.outputLeft(), levelR = ym.outputRight(), levelPsg = psg.mix();

    const int total = static_cast<int>(std::lround(static_cast<double>(totalSamples) / 44100.0 * rate));
    std::vector<float> left(static_cast<size_t>(total), 0.0f), right(static_cast<size_t>(total), 0.0f);
    std::vector<float> tmp(static_cast<size_t>(kBlockSize));
    constexpr float kPsgScale = static_cast<float>(kPsgToFmGain) * kOutputScale;

    size_t nextEvent = 0;
    double nextFm = 0.0, nextPsg = 0.0, blockStart = 0.0;
    for (int pos = 0; pos < total; pos += kBlockSize)
    {
        const int n = std::min(kBlockSize, total - pos);
        const double blockEnd = static_cast<double>(pos + n) / hostPerMaster;
        for (;;)
        {
            const double tEvent = nextEvent < events.size() ? events[nextEvent].sample * masterPerVgm : 1e300;
            const double t = std::min(tEvent, std::min(nextFm, nextPsg));
            if (t >= blockEnd)
                break;
            const double hostTime = (t - blockStart) * hostPerMaster;
            if (tEvent <= t)
            {
                const VgmEvent& e = events[nextEvent++];
                if (e.kind == 2)
                    psg.write(e.value);
                else
                    ym.write(e.kind, e.reg, e.value);
            }
            else if (nextFm <= t)
            {
                ym.clockSample();
                const int l = ym.outputLeft(), r = ym.outputRight();
                if (l != levelL) { fmL.addDelta(hostTime, static_cast<float>(l - levelL) * kOutputScale); levelL = l; }
                if (r != levelR) { fmR.addDelta(hostTime, static_cast<float>(r - levelR) * kOutputScale); levelR = r; }
                nextFm += kMasterClocksPerFmSample;
            }
            else
            {
                psg.clock();
                const int m = psg.mix();
                if (m != levelPsg) { psgSynth.addDelta(hostTime, static_cast<float>(m - levelPsg) * kPsgScale); levelPsg = m; }
                nextPsg += kMasterClocksPerPsgTick;
            }
        }
        float* outL = left.data() + pos;
        float* outR = right.data() + pos;
        fmL.endBlock(outL, n);
        fmR.endBlock(outR, n);
        psgSynth.endBlockReplace(tmp.data(), n);
        for (int i = 0; i < n; ++i)
        {
            outL[i] = filterL.process(outL[i] + tmp[static_cast<size_t>(i)], lowPass);
            outR[i] = filterR.process(outR[i] + tmp[static_cast<size_t>(i)], lowPass);
        }
        blockStart = blockEnd;
    }
    writeFile(wavPath, wavStereo16(left, right, static_cast<int>(std::lround(rate))));
    return 0;
}

int cmdRegsSnes(int argc, char** argv)
{
    if (argc < 6)
        return usage();
    const std::string spcPath = argv[3];
    const std::string eventsPath = argv[4];
    const std::string wavPath = argv[5];
    double seconds = 1.0;
    for (int i = 6; i < argc; ++i)
    {
        std::string v;
        if (optionValue(argc, argv, i, "--seconds", v)) seconds = std::stod(v);
        else throw std::runtime_error(std::string("unknown option ") + argv[i]);
    }
    const std::string spc = readFile(spcPath);
    if (spc.size() < 0x10180 || spc.compare(0, 27, "SNES-SPC700 Sound File Data") != 0)
        throw std::runtime_error(spcPath + ": not an SPC file");

    struct Write { long sample; int reg; int value; };
    std::vector<Write> writes;
    {
        std::istringstream in(readFile(eventsPath));
        Write w{};
        while (in >> w.sample >> w.reg >> w.value)
            writes.push_back(w);
        std::stable_sort(writes.begin(), writes.end(), [](const Write& a, const Write& b) { return a.sample < b.sample; });
    }

    // State load: RAM image, then the DSP snapshot (KON/KOFF/ENDX skipped, FLG last).
    auto dsp = std::make_unique<chipdsp::snes::SnesDsp>();
    std::memcpy(dsp->ram(), spc.data() + 0x100, 0x10000);
    const auto* regs = reinterpret_cast<const uint8_t*>(spc.data() + 0x10100);
    for (int a = 0; a < 128; ++a)
        if (a != 0x4C && a != 0x5C && a != 0x7C && a != 0x6C)
            dsp->writeRegister(a, regs[a]);
    dsp->writeRegister(0x6C, regs[0x6C]);

    const long total = std::lround(seconds * 32000.0);
    std::vector<float> left(static_cast<size_t>(total)), right(static_cast<size_t>(total));
    chipdsp::snes::SnesDspOutput out;
    size_t next = 0;
    for (long s = 0; s < total; ++s)
    {
        while (next < writes.size() && writes[next].sample <= s)
        {
            dsp->writeRegister(writes[next].reg, static_cast<uint8_t>(writes[next].value));
            ++next;
        }
        dsp->step(out);
        left[static_cast<size_t>(s)] = static_cast<float>(out.mainL) / 32768.0f;
        right[static_cast<size_t>(s)] = static_cast<float>(out.mainR) / 32768.0f;
    }
    writeFile(wavPath, wavStereo16Exact(left, right, 32000));
    return 0;
}

// NES VGM (VGM specification 1.61+: header 0x84 = NES APU clock, command 0xB4 aa dd = write dd
// to $4000 + aa for aa = 0x00..0x1F, data block 0x67 0x66 0xC2 = NES APU RAM write with a 16-bit
// start address). FDS registers, other chips and other data block types are errors.
struct NesVgm
{
    uint32_t clock = 0;
    uint32_t totalSamples = 0;
    std::vector<VgmEvent> writes;           // kind unused, reg = offset from $4000
    std::vector<uint8_t> memory = std::vector<uint8_t>(0x10000, 0);
};

NesVgm parseVgmNes(const std::string& data)
{
    auto u8 = [&](size_t o) {
        if (o >= data.size())
            throw std::runtime_error("VGM: truncated");
        return static_cast<uint8_t>(data[o]);
    };
    auto u32 = [&](size_t o) {
        return static_cast<uint32_t>(u8(o)) | (static_cast<uint32_t>(u8(o + 1)) << 8) |
               (static_cast<uint32_t>(u8(o + 2)) << 16) | (static_cast<uint32_t>(u8(o + 3)) << 24);
    };
    if (data.size() < 0x40 || data.compare(0, 4, "Vgm ") != 0)
        throw std::runtime_error("not a VGM file");
    NesVgm out;
    const uint32_t version = u32(0x08);
    const size_t start = (version >= 0x150 && u32(0x34) != 0) ? 0x34 + u32(0x34) : 0x40;
    if (version < 0x161 || start <= 0x84)
        throw std::runtime_error("VGM: NES APU needs version 1.61+ with a header covering 0x84");
    out.clock = u32(0x84);
    if (out.clock == 0 || (out.clock & 0x80000000u) != 0)
        throw std::runtime_error("VGM: no NES APU clock, or FDS requested (not supported)");
    out.totalSamples = u32(0x18);
    size_t pos = start;
    uint32_t now = 0;
    for (;;)
    {
        const uint8_t cmd = u8(pos);
        if (cmd == 0x66)
            break;
        if (cmd == 0xB4)
        {
            const uint8_t reg = u8(pos + 1);
            if (reg > 0x1F)
                throw std::runtime_error("VGM: NES register outside $4000-$401F (FDS not supported)");
            out.writes.push_back({ now, 0, reg, u8(pos + 2) });
            pos += 3;
        }
        else if (cmd == 0x67)
        {
            if (u8(pos + 1) != 0x66 || u8(pos + 2) != 0xC2)
                throw std::runtime_error("VGM: only data blocks of type 0xC2 (NES APU RAM write) are supported");
            const uint32_t size = u32(pos + 3);
            if (size < 2)
                throw std::runtime_error("VGM: empty RAM write block");
            const uint32_t address = static_cast<uint32_t>(u8(pos + 7)) | (static_cast<uint32_t>(u8(pos + 8)) << 8);
            if (address + (size - 2) > 0x10000)
                throw std::runtime_error("VGM: RAM write block beyond $FFFF");
            for (uint32_t i = 0; i < size - 2; ++i)
                out.memory[address + i] = u8(pos + 9 + i);
            pos += 7 + size;
        }
        else if (cmd == 0x61)
        {
            now += static_cast<uint32_t>(u8(pos + 1)) | (static_cast<uint32_t>(u8(pos + 2)) << 8);
            pos += 3;
        }
        else if (cmd == 0x62) { now += 735; pos += 1; }
        else if (cmd == 0x63) { now += 882; pos += 1; }
        else if ((cmd & 0xF0) == 0x70) { now += (cmd & 15u) + 1u; pos += 1; }
        else
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, "VGM: unsupported command 0x%02X at 0x%zX", cmd, pos);
            throw std::runtime_error(buf);
        }
    }
    out.totalSamples = std::max(out.totalSamples, now);
    return out;
}

int cmdRegsNes(int argc, char** argv)
{
    using namespace chipdsp::nes;
    if (argc < 5)
        return usage();
    const std::string vgmPath = argv[3];
    const std::string wavPath = argv[4];
    double rate = 44100.0;
    auto kernel = chipdsp::BandLimitedStepSynth::Kernel::IntegratedStep;   // as Nes2A03Engine
    for (int i = 5; i < argc; ++i)
    {
        std::string v;
        if (optionValue(argc, argv, i, "--rate", v)) rate = std::stod(v);
        else if (optionValue(argc, argv, i, "--kernel", v))
        {
            if (v == "impulse") kernel = chipdsp::BandLimitedStepSynth::Kernel::ImpulseSum;
            else if (v == "integrated") kernel = chipdsp::BandLimitedStepSynth::Kernel::IntegratedStep;
            else throw std::runtime_error("--kernel must be impulse or integrated");
        }
        else throw std::runtime_error(std::string("unknown option ") + argv[i]);
    }
    const NesVgm vgm = parseVgmNes(readFile(vgmPath));
    // The header clock selects the region; the chip then runs at the engine's integer clock.
    const bool pal = vgm.clock < 1720000u;
    const double cpuHz = pal ? kCpuHzPal : kCpuHzNtsc;
    if (std::abs(static_cast<double>(vgm.clock) - cpuHz) > 10.0)
        std::fprintf(stderr, "warning: VGM NES clock %u Hz differs from the engine clock %.0f Hz\n", vgm.clock, cpuHz);

    NesApu chip;
    chip.setRegion(pal);
    chip.reset();
    chip.dmc.setMemory(vgm.memory.data() + 0xC000, 0x4000);
    NesMixer mixer;
    mixer.build();
    chipdsp::BandLimitedStepSynth synth;
    synth.prepare(kCpuHzNtsc, rate, kBlockSize, kernel);   // as Nes2A03Engine::prepare
    NesOutputStage stage;
    stage.prepare(rate);

    // Mirror of Nes2A03Engine::readLevels (ultrasonic triangle: constant 7.5 in the mixer, A2).
    struct Levels
    {
        uint8_t p1 = 0, p2 = 0, tri = 0, noise = 0, dmc = 0;
        bool triUltrasonic = false;
        bool operator==(const Levels&) const = default;
    };
    auto readLevels = [&]() {
        Levels lv;
        lv.p1 = chip.pulse1.output();
        lv.p2 = chip.pulse2.output();
        lv.triUltrasonic = chip.triangle.isUltrasonic();
        lv.tri = lv.triUltrasonic ? uint8_t { 0 } : chip.triangle.output();
        lv.noise = chip.noise.output();
        lv.dmc = chip.dmc.output();
        return lv;
    };
    auto mixOf = [&](const Levels& lv) { return mixer.mix(lv.p1, lv.p2, lv.tri, lv.triUltrasonic, lv.noise, lv.dmc); };
    // As Nes2A03Engine::reset(): the synth tracks changes relative to the power-up level.
    Levels last = readLevels();
    float lastMix = mixOf(last);

    const double hostPerClock = rate / cpuHz;
    const int total = static_cast<int>(std::lround(static_cast<double>(vgm.totalSamples) / 44100.0 * rate));
    std::vector<float> out(static_cast<size_t>(total), 0.0f);
    size_t next = 0;
    uint64_t cycle = 0;
    double t = 0.0;                                         // block-relative host time of this cycle
    for (int pos = 0; pos < total; pos += kBlockSize)
    {
        const int n = std::min(kBlockSize, total - pos);
        const double end = static_cast<double>(n);
        while (t < end)
        {
            // Writes of VGM sample s land on CPU cycle round(s * cpu / 44100), before that
            // cycle's clock (the engine's driver also writes before chip.clock()).
            while (next < vgm.writes.size() &&
                   static_cast<uint64_t>(std::llround(vgm.writes[next].sample * cpuHz / 44100.0)) <= cycle)
            {
                chip.write(static_cast<uint16_t>(0x4000 + vgm.writes[next].reg), vgm.writes[next].value);
                ++next;
            }
            chip.clock();
            ++cycle;
            const Levels lv = readLevels();
            if (!(lv == last))
            {
                const float mix = mixOf(lv);
                if (mix != lastMix)
                {
                    synth.addDelta(t, mix - lastMix);
                    lastMix = mix;
                }
                last = lv;
            }
            t += hostPerClock;
        }
        t -= end;
        float* dst = out.data() + pos;
        synth.endBlockReplace(dst, n);
        stage.process(dst, n, false);
    }
    writeFile(wavPath, wavStereo16(out, out, static_cast<int>(std::lround(rate))));
    return 0;
}

int cmdRegs(int argc, char** argv)
{
    if (argc < 3)
        return usage();
    const std::string chip = argv[2];
    if (chip == "genesis")
        return cmdRegsGenesis(argc, argv);
    if (chip == "snes")
        return cmdRegsSnes(argc, argv);
    if (chip == "nes")
        return cmdRegsNes(argc, argv);
    throw std::runtime_error("regs: unknown chip " + chip + " (genesis, snes or nes)");
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
        if (command == "regs")
            return cmdRegs(argc, argv);
        return usage();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
}
