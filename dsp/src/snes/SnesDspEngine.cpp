#include "chipdsp/snes/SnesDspEngine.h"

#include "chipdsp/snes/BrrCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <thread>

namespace chipdsp
{

using namespace snes;

namespace
{
    const char* const kOffOn[] = { "Off", "On" };
    const char* const kGainModes[] = { "Direct", "Linear decrease", "Exponential decrease", "Linear increase",
                                       "Bent-line increase" };
    const char* const kReleaseModes[] = { "Hardware KOFF", "GAIN exponential" };
    const char* const kLoopOverrides[] = { "Sample default", "Force one-shot", "Force loop" };
    const char* const kFirNames[] = { kFirPresets[0].name, kFirPresets[1].name, kFirPresets[2].name, kFirPresets[3].name,
                                      kFirPresets[4].name, kFirPresets[5].name, kFirPresets[6].name, kFirPresets[7].name };
    static_assert(kNumFirPresets == 8, "update kFirNames");

    using E = SnesDspEngine;
    constexpr ParamDesc kDescs[] = {
        { E::Sample,       "sample",        "Sample",           "Instrument", 0.0f, 31.0f, 0.0f, true, "", nullptr },
        { E::AdsrEnable,   "adsr_enable",   "ADSR",             "Instrument", 0.0f, 1.0f, 1.0f, true, "", kOffOn },
        { E::Attack,       "attack",        "Attack",           "Instrument", 0.0f, 15.0f, 15.0f, true, "", nullptr },
        { E::Decay,        "decay",         "Decay",            "Instrument", 0.0f, 7.0f, 7.0f, true, "", nullptr },
        { E::SustainLevel, "sustain_level", "Sustain Level",    "Instrument", 0.0f, 7.0f, 7.0f, true, "", nullptr },
        { E::SustainRate,  "sustain_rate",  "Sustain Rate",     "Instrument", 0.0f, 31.0f, 0.0f, true, "", nullptr },
        { E::GainMode,     "gain_mode",     "Gain Mode",        "Instrument", 0.0f, 4.0f, 0.0f, true, "", kGainModes },
        { E::GainValue,    "gain_value",    "Gain Value",       "Instrument", 0.0f, 127.0f, 127.0f, true, "", nullptr },
        { E::ReleaseMode,  "release_mode",  "Release Mode",     "Instrument", 0.0f, 1.0f, 0.0f, true, "", kReleaseModes },
        { E::ReleaseRate,  "release_rate",  "Release Rate",     "Instrument", 0.0f, 31.0f, 31.0f, true, "", nullptr },
        { E::Volume,       "volume",        "Volume",           "Instrument", 0.0f, 127.0f, 100.0f, true, "", nullptr },
        { E::Pan,          "pan",           "Pan",              "Instrument", -64.0f, 64.0f, 0.0f, true, "", nullptr },
        { E::Transpose,    "transpose",     "Transpose",        "Instrument", -24.0f, 24.0f, 0.0f, true, "st", nullptr },
        { E::FineTune,     "fine_tune",     "Fine Tune",        "Instrument", -100.0f, 100.0f, 0.0f, true, "cents", nullptr },
        // Driver vibrato in 4 ms ticks: rate = ticks per half cycle, 63 -> 2.0 Hz, 23 -> 5.4 Hz,
        // 15 -> 8.3 Hz (a 0..15 range could not reach the 4.5-6.5 Hz of strings and voices).
        { E::VibratoRate,  "vibrato_rate",  "Vibrato Rate",     "Instrument", 0.0f, 63.0f, 0.0f, true, "ticks", nullptr },
        { E::VibratoDepth, "vibrato_depth", "Vibrato Depth",    "Instrument", 0.0f, 64.0f, 0.0f, true, "", nullptr },
        { E::VibratoDelay, "vibrato_delay", "Vibrato Delay",    "Instrument", 0.0f, 250.0f, 0.0f, true, "ticks", nullptr },
        { E::NoiseEnable,  "noise_enable",  "Noise",            "Instrument", 0.0f, 1.0f, 0.0f, true, "", kOffOn },
        { E::NoiseClock,   "noise_clock",   "Noise Clock",      "Instrument", 0.0f, 31.0f, 0.0f, true, "", nullptr },
        { E::Pmon,         "pmon",          "Pitch Modulation", "Instrument", 0.0f, 1.0f, 0.0f, true, "", kOffOn },
        { E::LoopOverride, "loop_override", "Loop Override",    "Instrument", 0.0f, 2.0f, 0.0f, true, "", kLoopOverrides },
        { E::EchoEnable,   "echo_enable",   "Echo",             "Echo", 0.0f, 1.0f, 0.0f, true, "", kOffOn },
        { E::EchoDelay,    "echo_delay",    "Echo Delay",       "Echo", 0.0f, 15.0f, 0.0f, true, "", nullptr },
        { E::EchoFeedback, "echo_feedback", "Echo Feedback",    "Echo", -128.0f, 127.0f, 0.0f, true, "", nullptr },
        { E::EchoVolume,   "echo_volume",   "Echo Volume",      "Echo", -128.0f, 127.0f, 0.0f, true, "", nullptr },
        { E::FirPreset,    "fir_preset",    "FIR Preset",       "Echo", 0.0f, 7.0f, 0.0f, true, "", kFirNames },
        { E::V1Echo,       "v1_echo",       "Voice 1 Echo",     "Echo", 0.0f, 1.0f, 0.0f, true, "", kOffOn },
        { E::V2Echo,       "v2_echo",       "Voice 2 Echo",     "Echo", 0.0f, 1.0f, 0.0f, true, "", kOffOn },
        { E::V3Echo,       "v3_echo",       "Voice 3 Echo",     "Echo", 0.0f, 1.0f, 0.0f, true, "", kOffOn },
        { E::V4Echo,       "v4_echo",       "Voice 4 Echo",     "Echo", 0.0f, 1.0f, 0.0f, true, "", kOffOn },
        { E::V5Echo,       "v5_echo",       "Voice 5 Echo",     "Echo", 0.0f, 1.0f, 0.0f, true, "", kOffOn },
        { E::V6Echo,       "v6_echo",       "Voice 6 Echo",     "Echo", 0.0f, 1.0f, 0.0f, true, "", kOffOn },
        { E::V7Echo,       "v7_echo",       "Voice 7 Echo",     "Echo", 0.0f, 1.0f, 0.0f, true, "", kOffOn },
        { E::V8Echo,       "v8_echo",       "Voice 8 Echo",     "Echo", 0.0f, 1.0f, 0.0f, true, "", kOffOn },
        { E::MainVolume,   "main_volume",   "Main Volume",      "Global", 0.0f, 127.0f, 127.0f, true, "", nullptr },
    };
    static_assert(static_cast<int>(std::size(kDescs)) == E::NumParams, "one descriptor per parameter");

    const char* const kVoiceNames[kSnesChannels] = { "Voice 1", "Voice 2", "Voice 3", "Voice 4",
                                                     "Voice 5", "Voice 6", "Voice 7", "Voice 8" };
    const char* const kVoiceShort[kSnesChannels] = { "V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8" };

    // Windowed-sinc decimation to 32 kHz for sources above the S-DSP rate (message thread).
    // A composer would have downsampled the recording the same way before BRR encoding.
    std::vector<float> resampleTo32k(const float* x, int n, double srcRate)
    {
        const double ratio = kSampleRate / srcRate;                 // < 1
        const int outN = std::max(1, static_cast<int>(std::floor(n * ratio)));
        const double cutoff = 0.5 * ratio * 0.95;                   // cycles per source sample
        const int halfWidth = static_cast<int>(std::ceil(16.0 / ratio));
        std::vector<float> out(static_cast<size_t>(outN), 0.0f);
        for (int m = 0; m < outN; ++m)
        {
            const double t = m / ratio;
            const int centre = static_cast<int>(std::floor(t));
            double sum = 0.0, norm = 0.0;
            for (int k = centre - halfWidth + 1; k <= centre + halfWidth; ++k)
            {
                const double dx = t - k;
                const double w = 0.42 + 0.5 * std::cos(std::numbers::pi * dx / halfWidth)
                               + 0.08 * std::cos(2.0 * std::numbers::pi * dx / halfWidth);   // Blackman
                const double arg = 2.0 * cutoff * dx;
                const double sinc = std::abs(arg) < 1e-12 ? 1.0 : std::sin(std::numbers::pi * arg) / (std::numbers::pi * arg);
                const double h = sinc * w;
                norm += h;
                if (k >= 0 && k < n)
                    sum += h * x[k];
            }
            out[static_cast<size_t>(m)] = static_cast<float>(norm != 0.0 ? sum / norm : 0.0);
        }
        return out;
    }
} // namespace

SnesDspEngine::SnesDspEngine()
    : dsp(std::make_unique<SnesDsp>())
{
    for (const auto& d : kDescs)
        params[static_cast<size_t>(d.id)].store(d.defaultValue, std::memory_order_relaxed);
    banks[0] = std::make_unique<SampleBank>();
    banks[1] = std::make_unique<SampleBank>();
    publish(master);   // empty slots: every directory entry points at the silent loop block
}

SnesDspEngine::~SnesDspEngine() = default;

ChannelInfo SnesDspEngine::channelInfo(int channel) const noexcept
{
    const int i = std::clamp(channel, 0, kSnesChannels - 1);
    return { kVoiceNames[i], kVoiceShort[i], true };
}

std::span<const ParamDesc> SnesDspEngine::parameterDescriptors() const noexcept
{
    return { kDescs, std::size(kDescs) };
}

void SnesDspEngine::prepare(double hostSampleRate, int maxBlockSize)
{
    maxBlock = std::max(1, maxBlockSize);
    for (int c = 0; c < 2; ++c)
    {
        mainSynth[c].prepare(kSampleRate, hostSampleRate, maxBlock);
        mainDc[c].prepare(5.0, hostSampleRate);
        for (int v = 0; v < kSnesChannels; ++v)
        {
            voiceSynth[v][c].prepare(kSampleRate, hostSampleRate, maxBlock);
            voiceDc[v][c].prepare(5.0, hostSampleRate);
        }
    }
    hostPerNative = mainSynth[0].hostSamplesPerClock();
    prepared = true;
    reset();
}

// ----- parameters ------------------------------------------------------------------------------

void SnesDspEngine::setParameter(int id, float value) noexcept
{
    if (id >= 0 && id < NumParams)
        params[static_cast<size_t>(id)].store(value, std::memory_order_relaxed);
}

float SnesDspEngine::getParameter(int id) const noexcept
{
    return (id >= 0 && id < NumParams) ? params[static_cast<size_t>(id)].load(std::memory_order_relaxed) : 0.0f;
}

int SnesDspEngine::paramInt(int id) const noexcept
{
    const ParamDesc& d = kDescs[id];
    const float v = params[static_cast<size_t>(id)].load(std::memory_order_relaxed);
    const float c = std::isfinite(v) ? std::clamp(v, d.minValue, d.maxValue) : d.defaultValue;
    return static_cast<int>(std::lround(c));
}

SnesDriverParams SnesDspEngine::readParams() const noexcept
{
    SnesDriverParams p;
    p.sample = paramInt(Sample);
    p.adsrEnable = paramInt(AdsrEnable);
    p.attack = paramInt(Attack);
    p.decay = paramInt(Decay);
    p.sustainLevel = paramInt(SustainLevel);
    p.sustainRate = paramInt(SustainRate);
    p.gainMode = paramInt(GainMode);
    p.gainValue = paramInt(GainValue);
    p.releaseMode = paramInt(ReleaseMode);
    p.releaseRate = paramInt(ReleaseRate);
    p.volume = paramInt(Volume);
    p.pan = paramInt(Pan);
    p.transpose = paramInt(Transpose);
    p.fineTune = paramInt(FineTune);
    p.vibratoRate = paramInt(VibratoRate);
    p.vibratoDepth = paramInt(VibratoDepth);
    p.vibratoDelay = paramInt(VibratoDelay);
    p.noiseEnable = paramInt(NoiseEnable);
    p.noiseClock = paramInt(NoiseClock);
    p.pmon = paramInt(Pmon);
    p.loopOverride = paramInt(LoopOverride);
    p.echoEnable = paramInt(EchoEnable);
    p.echoDelay = paramInt(EchoDelay);
    p.echoFeedback = paramInt(EchoFeedback);
    p.echoVolume = paramInt(EchoVolume);
    p.firPreset = paramInt(FirPreset);
    p.eonMask = 0;
    for (int v = 0; v < kSnesChannels; ++v)
        p.eonMask |= paramInt(V1Echo + v) != 0 ? (1 << v) : 0;
    p.mainVolume = paramInt(MainVolume);
    return p;
}

// ----- samples (message thread) ------------------------------------------------------------------

int SnesDspEngine::totalBrrBytes(const std::array<MasterSlot, kNumSampleSlots>& slots)
{
    int total = 0;
    for (const auto& s : slots)
        if (s.loaded)
            total += static_cast<int>(s.brr.size());
    return total;
}

bool SnesDspEngine::publish(const std::array<MasterSlot, kNumSampleSlots>& slots)
{
    // Physical limit only; the echo-dependent budget is checked by loadSample. If echo_delay
    // is raised later the driver shortens the echo instead (SnesDriver::maxEchoDelay).
    if (totalBrrBytes(slots) > sampleCapacityBytes(0))
        return false;

    // Write into the bank the audio thread is not using, then flip (ARCHITECTURE.md).
    const uint32_t state = bankState.load();
    const int target = 1 - static_cast<int>(state & 1u);
    while (bankReading.load() == target)
        std::this_thread::yield();

    SampleBank& bank = *banks[target];
    bank.image.fill(0);
    bank.image[kSilentLoopBlock] = kBrrFlagEnd | kBrrFlagLoop;   // code 3, zeros, loops to itself
    bank.image[kSilentEndBlock] = kBrrFlagEnd;                   // code 1, releases the voice

    auto writeEntry = [&bank](int entry, int start, int loop) {
        const int a = kDirAddress + entry * 4;
        bank.image[static_cast<size_t>(a + 0)] = static_cast<uint8_t>(start & 0xFF);
        bank.image[static_cast<size_t>(a + 1)] = static_cast<uint8_t>(start >> 8);
        bank.image[static_cast<size_t>(a + 2)] = static_cast<uint8_t>(loop & 0xFF);
        bank.image[static_cast<size_t>(a + 3)] = static_cast<uint8_t>(loop >> 8);
    };

    int addr = kSampleDataStart;
    for (int s = 0; s < kNumSampleSlots; ++s)
    {
        const MasterSlot& m = slots[static_cast<size_t>(s)];
        SampleSlot& info = bank.slots[static_cast<size_t>(s)];
        info = SampleSlot{};
        info.rootNote = m.rootNote;
        if (!m.loaded)
        {
            writeEntry(s * 2, kSilentLoopBlock, kSilentLoopBlock);
            writeEntry(s * 2 + 1, kSilentLoopBlock, kSilentLoopBlock);
            continue;
        }
        std::copy(m.brr.begin(), m.brr.end(), bank.image.begin() + addr);
        const int loopBlock = std::max(m.loopStartBlock, 0);
        writeEntry(s * 2, addr, kSilentEndBlock);                          // one-shot variant
        writeEntry(s * 2 + 1, addr, addr + loopBlock * kBrrBlockBytes);    // looped variant
        info.loaded = true;
        info.hasLoop = m.loopStartBlock >= 0;
        info.storedRate = m.storedRate;
        info.startAddress = addr;
        info.numBlocks = static_cast<int>(m.brr.size()) / kBrrBlockBytes;
        info.loopStartBlock = m.loopStartBlock;
        addr += static_cast<int>(m.brr.size());
    }
    bank.dataEnd = addr;

    bankState.store((((state >> 1) + 1u) << 1) | static_cast<uint32_t>(target));
    return true;
}

bool SnesDspEngine::loadSample(int slot, const float* mono, int numFrames, double sourceSampleRate)
{
    if (slot < 0 || slot >= kNumSampleSlots || mono == nullptr || numFrames <= 0 || !(sourceSampleRate > 0.0))
        return false;

    std::vector<float> pcm(mono, mono + numFrames);
    double storedRate = sourceSampleRate;
    if (sourceSampleRate > kSampleRate)
    {
        pcm = resampleTo32k(mono, numFrames, sourceSampleRate);
        storedRate = kSampleRate;
    }

    auto candidate = master;
    MasterSlot& m = candidate[static_cast<size_t>(slot)];
    m.pcm15.resize(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i)
    {
        const float x = std::isfinite(pcm[i]) ? pcm[i] : 0.0f;
        m.pcm15[i] = static_cast<int16_t>(std::clamp(std::lround(x * 16383.0f), -16384L, 16383L));
    }
    m.loopStartBlock = -1;
    m.storedRate = storedRate;
    m.brr = brrEncode(m.pcm15.data(), static_cast<int>(m.pcm15.size()), -1, BrrEndCode::EndLoop);
    m.loaded = true;

    // Budget: 64 KiB minus the driver's reserved low RAM minus the echo buffer of echo_delay.
    if (totalBrrBytes(candidate) > sampleCapacityBytes(paramInt(EchoDelay)))
        return false;
    if (!publish(candidate))
        return false;
    master = std::move(candidate);
    return true;
}

bool SnesDspEngine::setSampleLoop(int slot, int loopStartBlock)
{
    if (slot < 0 || slot >= kNumSampleSlots)
        return false;
    auto candidate = master;
    MasterSlot& m = candidate[static_cast<size_t>(slot)];
    const int numBlocks = static_cast<int>(m.brr.size()) / kBrrBlockBytes;
    if (!m.loaded || loopStartBlock < -1 || loopStartBlock >= numBlocks)
        return false;
    m.loopStartBlock = loopStartBlock;
    m.brr = brrEncode(m.pcm15.data(), static_cast<int>(m.pcm15.size()), loopStartBlock, BrrEndCode::EndLoop);
    if (!publish(candidate))
        return false;
    master = std::move(candidate);
    return true;
}

bool SnesDspEngine::setSampleRootNote(int slot, float midiNote)
{
    if (slot < 0 || slot >= kNumSampleSlots || !std::isfinite(midiNote))
        return false;
    auto candidate = master;
    candidate[static_cast<size_t>(slot)].rootNote = midiNote;
    if (!publish(candidate))
        return false;
    master = std::move(candidate);
    return true;
}

bool SnesDspEngine::setSampleInfo(int slot, float rootNote, int loopStartFrame, double sourceSampleRate)
{
    if (slot < 0 || slot >= kNumSampleSlots || !master[static_cast<size_t>(slot)].loaded || !(sourceSampleRate > 0.0))
        return false;
    if (!setSampleRootNote(slot, rootNote))
        return false;
    int loopBlock = -1;
    if (loopStartFrame >= 0)
    {
        const double stored = master[static_cast<size_t>(slot)].storedRate;
        const double frame = static_cast<double>(loopStartFrame) * stored / sourceSampleRate;
        loopBlock = static_cast<int>(std::floor(frame / 16.0));
    }
    return setSampleLoop(slot, loopBlock);
}

SnesDspEngine::SampleInfo SnesDspEngine::sampleInfo(int slot) const
{
    SampleInfo info;
    if (slot < 0 || slot >= kNumSampleSlots)
        return info;
    const MasterSlot& m = master[static_cast<size_t>(slot)];
    info.loaded = m.loaded;
    info.brrBytes = m.loaded ? static_cast<int>(m.brr.size()) : 0;
    info.numBlocks = info.brrBytes / kBrrBlockBytes;
    info.loopStartBlock = m.loopStartBlock;
    info.rootNote = m.rootNote;
    info.storedRate = m.storedRate;
    return info;
}

int SnesDspEngine::freeSampleBytes() const
{
    return sampleCapacityBytes(paramInt(EchoDelay)) - totalBrrBytes(master);
}

// ----- audio thread ------------------------------------------------------------------------------

void SnesDspEngine::syncSampleBank(bool force) noexcept
{
    // Claim the active bank, re-checking that it did not flip before the claim was visible.
    uint32_t state = 0;
    int index = 0;
    do
    {
        state = bankState.load();
        index = static_cast<int>(state & 1u);
        bankReading.store(index);
    } while (bankState.load() != state);

    if (force || state != bankStateCopied)
    {
        const SampleBank& bank = *banks[index];
        uint8_t* ram = dsp->ram();
        // Directory page, then the dummy blocks and BRR data (the 4-byte EDL-0 echo buffer at
        // 0x0300 is left alone).
        std::memcpy(ram + kDirAddress, bank.image.data() + kDirAddress, 0x100);
        std::memcpy(ram + kSilentLoopBlock, bank.image.data() + kSilentLoopBlock,
                    static_cast<size_t>(bank.dataEnd - kSilentLoopBlock));
        liveSlots = bank.slots;
        liveDataEnd = bank.dataEnd;
        bankStateCopied = state;
        drv.onSampleMemoryChanged(*dsp, liveDataEnd);
    }
    bankReading.store(-1);
}

void SnesDspEngine::reset() noexcept
{
    dsp->reset();
    syncSampleBank(true);
    drv.reset(*dsp, readParams(), liveDataEnd);
    for (int c = 0; c < 2; ++c)
    {
        mainSynth[c].reset();
        mainDc[c].reset();
        mainLevel[c] = 0;
        for (int v = 0; v < kSnesChannels; ++v)
        {
            voiceSynth[v][c].reset();
            voiceDc[v][c].reset();
            voiceLevel[v][c] = 0;
        }
    }
    nativeTime = 0.0;
}

void SnesDspEngine::noteOn(int channel, float midiNote, float velocity) noexcept
{
    drv.noteOn(channel, midiNote, velocity, readParams());
}

void SnesDspEngine::noteOff(int channel) noexcept
{
    drv.noteOff(channel);
}

void SnesDspEngine::setChannelPitch(int channel, float midiNote) noexcept
{
    drv.setPitch(channel, midiNote);
}

bool SnesDspEngine::isChannelActive(int channel) const noexcept
{
    return drv.isActive(channel, *dsp);
}

void SnesDspEngine::setRawOutput(bool raw) noexcept
{
    for (int c = 0; c < 2; ++c)
    {
        mainSynth[c].setRaw(raw);
        for (int v = 0; v < kSnesChannels; ++v)
            voiceSynth[v][c].setRaw(raw);
    }
}

void SnesDspEngine::renderBlock(float* mainL, float* mainR, float* const* channelOutsL, float* const* channelOutsR,
                                int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (!prepared)
    {
        std::fill(mainL, mainL + numSamples, 0.0f);
        std::fill(mainR, mainR + numSamples, 0.0f);
        return;
    }
    syncSampleBank(false);
    for (int offset = 0; offset < numSamples; offset += maxBlock)
        renderChunk(mainL, mainR, channelOutsL, channelOutsR, offset, std::min(maxBlock, numSamples - offset));
}

void SnesDspEngine::renderChunk(float* mainL, float* mainR, float* const* chL, float* const* chR, int offset,
                                int n) noexcept
{
    const SnesDriverParams p = readParams();
    const bool perVoice = chL != nullptr && chR != nullptr;
    constexpr float kScale = 1.0f / 32768.0f;

    // Run the chip at exactly 32 kHz; each output sample is a step at its host-time position.
    SnesDspOutput out;
    while (nativeTime < static_cast<double>(n))
    {
        drv.beforeSample(*dsp, p, liveSlots.data());
        dsp->step(out);

        const int16_t levels[2] = { out.mainL, out.mainR };
        for (int c = 0; c < 2; ++c)
        {
            if (levels[c] != mainLevel[c])
            {
                mainSynth[c].addDelta(nativeTime, static_cast<float>(levels[c] - mainLevel[c]) * kScale);
                mainLevel[c] = levels[c];
            }
        }
        if (perVoice)
        {
            for (int v = 0; v < kSnesChannels; ++v)
            {
                const int16_t vl[2] = { out.voiceL[v], out.voiceR[v] };
                for (int c = 0; c < 2; ++c)
                {
                    if (vl[c] != voiceLevel[v][c])
                    {
                        voiceSynth[v][c].addDelta(nativeTime, static_cast<float>(vl[c] - voiceLevel[v][c]) * kScale);
                        voiceLevel[v][c] = vl[c];
                    }
                }
            }
        }
        nativeTime += hostPerNative;
    }
    nativeTime -= static_cast<double>(n);

    float* mains[2] = { mainL + offset, mainR + offset };
    for (int c = 0; c < 2; ++c)
    {
        mainSynth[c].endBlockReplace(mains[c], n);
        for (int i = 0; i < n; ++i)
            mains[c][i] = mainDc[c].process(mains[c][i]);
    }
    if (!perVoice)
        return;
    for (int v = 0; v < kSnesChannels; ++v)
    {
        float* outs[2] = { chL[v], chR[v] };
        for (int c = 0; c < 2; ++c)
        {
            if (outs[c] == nullptr)
            {
                voiceSynth[v][c].reset();   // keep the unused synth from accumulating a tail
                voiceLevel[v][c] = 0;
                continue;
            }
            float* dst = outs[c] + offset;
            voiceSynth[v][c].endBlockReplace(dst, n);
            for (int i = 0; i < n; ++i)
                dst[i] = voiceDc[v][c].process(dst[i]);
        }
    }
}

} // namespace chipdsp
