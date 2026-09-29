#include "chipdsp/snes/SnesDriver.h"

#include <algorithm>
#include <cmath>

namespace chipdsp::snes
{

int SnesDriver::pitchRegister(float midiNote, float rootNote, double storedRate) noexcept
{
    const double ratio = storedRate / kSampleRate * std::pow(2.0, (static_cast<double>(midiNote) - rootNote) / 12.0);
    const long p = std::lround(4096.0 * ratio);
    return static_cast<int>(std::clamp(p, 0L, 0x3FFFL));
}

int SnesDriver::vibratoOffset(int tickPhase, int rate, int depth) noexcept
{
    // Driver: triangle of period 2 * rate ticks between -depth and +depth, sampled once per
    // tick. Position x on the cycle rises from -depth (x = 0) to +depth (x = rate) and falls
    // back. Tick 0 sits at x = ceil(rate / 2): exactly 0 for even rates, the first value
    // above 0 (+depth / rate) for odd rates, so both peaks are reached for every rate 1..15.
    if (rate <= 0 || depth <= 0)
        return 0;
    const int x = (tickPhase % (2 * rate) + (rate + 1) / 2) % (2 * rate);
    const int num = x <= rate ? depth * (2 * x - rate) : depth * (3 * rate - 2 * x);
    return static_cast<int>(std::lround(static_cast<double>(num) / rate));
}

void SnesDriver::voiceVolumes(int volume, float velocity, int pan, int& left, int& right) noexcept
{
    // Driver: velocity scales the volume linearly, pan is a balance law (the far side is
    // attenuated linearly, the near side keeps the full value). Registers stay 0..127.
    const double v = std::clamp(static_cast<double>(volume), 0.0, 127.0) * std::clamp(static_cast<double>(velocity), 0.0, 1.0);
    const int p = std::clamp(pan, -64, 64);
    left = static_cast<int>(std::lround(v * std::min(64, 64 - p) / 64.0));
    right = static_cast<int>(std::lround(v * std::min(64, 64 + p) / 64.0));
}

uint8_t SnesDriver::adsr1Value(const SnesDriverParams& p) noexcept
{
    return static_cast<uint8_t>((p.adsrEnable != 0 ? 0x80 : 0) | ((p.decay & 7) << 4) | (p.attack & 0x0F));
}

uint8_t SnesDriver::adsr2Value(const SnesDriverParams& p) noexcept
{
    return static_cast<uint8_t>(((p.sustainLevel & 7) << 5) | (p.sustainRate & 0x1F));
}

uint8_t SnesDriver::gainValue(const SnesDriverParams& p) noexcept
{
    // gain_mode 0 direct (value 0..127), 1..4 = GAIN modes 0..3 with the value as a 5-bit rate.
    const int rate = std::clamp(p.gainValue, 0, 31);
    switch (p.gainMode)
    {
        case 1: return static_cast<uint8_t>(0x80 | rate);   // linear decrease
        case 2: return static_cast<uint8_t>(0xA0 | rate);   // exponential decrease
        case 3: return static_cast<uint8_t>(0xC0 | rate);   // linear increase
        case 4: return static_cast<uint8_t>(0xE0 | rate);   // bent-line increase
        default: return static_cast<uint8_t>(std::clamp(p.gainValue, 0, 127));
    }
}

int SnesDriver::maxEchoDelay(int sampleDataEnd) noexcept
{
    for (int e = 15; e >= 1; --e)
        if (echoBufferStart(e) >= sampleDataEnd)
            return e;
    return 0;
}

void SnesDriver::reset(SnesDsp& dsp, const SnesDriverParams& p, int sampleDataEnd) noexcept
{
    for (auto& c : channels)
        c = Channel{};
    pendingKon = 0;
    pendingKoff = 0;
    koffMask = 0;
    samplesSinceKon = 2;
    tickCounter = 0;
    dataEnd = sampleDataEnd;

    dsp.writeRegister(kRegKon, 0);
    dsp.writeRegister(kRegKoff, 0);
    for (int v = 0; v < kNumVoices; ++v)
    {
        dsp.writeRegister(v * 16 + kRegVolL, 0);
        dsp.writeRegister(v * 16 + kRegVolR, 0);
        dsp.writeRegister(v * 16 + kRegSrcn, 0);
    }

    // The chip was just reset, so its echo index is 0 and the new EDL latches on the first
    // sample: no 240 ms wait is needed here.
    edl = std::min(std::clamp(p.echoDelay, 0, 15), maxEchoDelay(dataEnd));
    programEchoBuffer(dsp, edl);
    echoWait = 0;
    echoRunning = p.echoEnable != 0;
    writeGlobals(dsp, p);
}

void SnesDriver::noteOn(int channel, float midiNote, float velocity, const SnesDriverParams& p) noexcept
{
    if (channel < 0 || channel >= kNumVoices)
        return;
    const auto bit = static_cast<uint8_t>(1 << channel);
    channels[channel].note = midiNote;
    channels[channel].velocity = std::clamp(velocity, 0.0f, 1.0f);
    channels[channel].instrument = p;
    pendingKoff = static_cast<uint8_t>(pendingKoff & ~bit);
    pendingKon = static_cast<uint8_t>(pendingKon | bit);
}

void SnesDriver::noteOff(int channel) noexcept
{
    if (channel < 0 || channel >= kNumVoices)
        return;
    const auto bit = static_cast<uint8_t>(1 << channel);
    // A note released before its key-on reached the chip is dropped; the previous note on
    // the voice (if any) is released instead.
    pendingKon = static_cast<uint8_t>(pendingKon & ~bit);
    pendingKoff = static_cast<uint8_t>(pendingKoff | bit);
}

void SnesDriver::setPitch(int channel, float midiNote) noexcept
{
    if (channel >= 0 && channel < kNumVoices)
        channels[channel].note = midiNote;   // picked up at the next tick, like a pitch slide
}

bool SnesDriver::isActive(int channel, const SnesDsp& dsp) const noexcept
{
    if (channel < 0 || channel >= kNumVoices)
        return false;
    if ((pendingKon & (1 << channel)) != 0)
        return true;
    if (!dsp.isVoiceSounding(channel))
        return false;
    // Engine query (not driver behaviour): a GAIN release has fully released when E reaches
    // 0, before the next tick's formal KOFF (ENGINE_SPECS "isChannelActive"). gainRelease is
    // cleared when the next key-on is written, so a pending KON is never hidden here.
    const Voice& vc = dsp.voice(channel);
    return !(channels[channel].gainRelease && vc.konDelay == 0 && vc.env == 0);
}

int SnesDriver::channelPitch(const Channel& c, const SnesDriverParams& p, int vibratoOffset) const noexcept
{
    const float note = c.note + static_cast<float>(p.transpose) + static_cast<float>(p.fineTune) / 100.0f;
    return std::clamp(pitchRegister(note, c.rootNote, c.storedRate) + vibratoOffset, 0, 0x3FFF);
}

void SnesDriver::beforeSample(SnesDsp& dsp, const SnesDriverParams& p, const SampleSlot* slots) noexcept
{
    if (pendingKoff != 0)
        flushKeyOffs(dsp);
    // KON is polled every second sample and a second write before the poll replaces the
    // first, so the driver never writes KON twice within two samples (research "Key-on").
    if (pendingKon != 0 && samplesSinceKon >= 2)
        flushKeyOns(dsp, p, slots);
    if (samplesSinceKon < 2)
        ++samplesSinceKon;

    if (tickCounter == 0)
        tick(dsp, p);
    if (++tickCounter == kSamplesPerTick)
        tickCounter = 0;
}

void SnesDriver::flushKeyOffs(SnesDsp& dsp) noexcept
{
    for (int v = 0; v < kNumVoices; ++v)
    {
        const auto bit = static_cast<uint8_t>(1 << v);
        if ((pendingKoff & bit) == 0)
            continue;
        const SnesDriverParams& p = channels[v].instrument;
        if (p.releaseMode == 0)
        {
            koffMask = static_cast<uint8_t>(koffMask | bit);          // hardware KOFF: -8 per sample
        }
        else
        {
            // Driver release: GAIN exponential decrease at release_rate. GAIN is written before
            // ADSR1 because the DSP reads ADSR1 and GAIN at different cycles (research, SNESdev race).
            dsp.writeRegister(v * 16 + kRegGain, static_cast<uint8_t>(0xA0 | std::clamp(p.releaseRate, 0, 31)));
            dsp.writeRegister(v * 16 + kRegAdsr1, static_cast<uint8_t>(dsp.readRegister(v * 16 + kRegAdsr1) & 0x7F));
            channels[v].gainRelease = true;
        }
    }
    dsp.writeRegister(kRegKoff, koffMask);
    pendingKoff = 0;
}

void SnesDriver::flushKeyOns(SnesDsp& dsp, const SnesDriverParams& p, const SampleSlot* slots) noexcept
{
    writeGlobals(dsp, p);
    for (int v = 0; v < kNumVoices; ++v)
    {
        const auto bit = static_cast<uint8_t>(1 << v);
        if ((pendingKon & bit) == 0)
            continue;
        Channel& c = channels[v];
        const SnesDriverParams& inst = c.instrument;
        const int slotIndex = std::clamp(inst.sample, 0, kNumSampleSlots - 1);
        const SampleSlot& slot = slots[slotIndex];
        // Directory entry: 2s = one-shot variant, 2s + 1 = looped variant (see the RAM map).
        bool looped = slot.hasLoop;
        if (inst.loopOverride == 1)
            looped = false;
        else if (inst.loopOverride == 2)
            looped = true;
        const auto srcn = static_cast<uint8_t>(slotIndex * 2 + (looped ? 1 : 0));

        c.gainRelease = false;
        c.ticks = 0;
        c.vibratoPhase = 0;
        c.rootNote = slot.rootNote;
        c.storedRate = slot.storedRate;
        c.pitch = channelPitch(c, p, 0);

        int left = 0, right = 0;
        voiceVolumes(inst.volume, c.velocity, inst.pan, left, right);
        const int base = v * 16;
        dsp.writeRegister(base + kRegSrcn, srcn);
        dsp.writeRegister(base + kRegAdsr2, adsr2Value(inst));   // ADSR2/GAIN before ADSR1
        dsp.writeRegister(base + kRegGain, gainValue(inst));
        dsp.writeRegister(base + kRegAdsr1, adsr1Value(inst));
        dsp.writeRegister(base + kRegVolL, static_cast<uint8_t>(left));
        dsp.writeRegister(base + kRegVolR, static_cast<uint8_t>(right));
        dsp.writeRegister(base + kRegPitchL, static_cast<uint8_t>(c.pitch & 0xFF));
        dsp.writeRegister(base + kRegPitchH, static_cast<uint8_t>(c.pitch >> 8));
    }
    // KOFF must be clear for the voice or the chip keys it off again at the next poll.
    koffMask = static_cast<uint8_t>(koffMask & ~pendingKon);
    dsp.writeRegister(kRegKoff, koffMask);
    dsp.writeRegister(kRegKon, pendingKon);
    pendingKon = 0;
    samplesSinceKon = 0;
}

void SnesDriver::tick(SnesDsp& dsp, const SnesDriverParams& p) noexcept
{
    updateEcho(dsp, p);   // also rewrites the global registers from the parameters

    for (int v = 0; v < kNumVoices; ++v)
    {
        Channel& c = channels[v];
        const auto bit = static_cast<uint8_t>(1 << v);
        if (!dsp.isVoiceSounding(v))
            continue;

        // Driver vibrato: a triangle of +/- depth pitch-register units, 'rate' ticks per
        // half cycle, starting 'delay' ticks after key-on and rising from 0.
        int offset = 0;
        if (p.vibratoRate > 0 && p.vibratoDepth > 0 && c.ticks >= p.vibratoDelay)
        {
            offset = vibratoOffset(c.vibratoPhase, p.vibratoRate, p.vibratoDepth);
            c.vibratoPhase = (c.vibratoPhase + 1) % (2 * p.vibratoRate);
        }
        const int pitch = channelPitch(c, p, offset);
        if (pitch != c.pitch)
        {
            c.pitch = pitch;
            dsp.writeRegister(v * 16 + kRegPitchL, static_cast<uint8_t>(pitch & 0xFF));
            dsp.writeRegister(v * 16 + kRegPitchH, static_cast<uint8_t>(pitch >> 8));
        }

        // VxVOL stays as written at key-on (velocity, volume and pan are latched per note).

        // A GAIN release that reached 0 is keyed off formally so the voice counts as free.
        // Like an SPC700 program, the driver reads ENVX ($x8 = E >> 4), not the chip's E.
        if (c.gainRelease && dsp.readRegister(v * 16 + kRegEnvx) == 0)
        {
            c.gainRelease = false;
            koffMask = static_cast<uint8_t>(koffMask | bit);
            dsp.writeRegister(kRegKoff, koffMask);
        }
        ++c.ticks;
    }
}

void SnesDriver::writeGlobals(SnesDsp& dsp, const SnesDriverParams& p) noexcept
{
    const auto mvol = static_cast<uint8_t>(std::clamp(p.mainVolume, 0, 127));
    dsp.writeRegister(kRegDir, static_cast<uint8_t>(kDirPage));
    dsp.writeRegister(kRegMvolL, mvol);
    dsp.writeRegister(kRegMvolR, mvol);
    dsp.writeRegister(kRegEfb, static_cast<uint8_t>(static_cast<int8_t>(std::clamp(p.echoFeedback, -128, 127))));
    dsp.writeRegister(kRegEon, static_cast<uint8_t>(p.eonMask & 0xFF));
    dsp.writeRegister(kRegNon, static_cast<uint8_t>(p.noiseEnable != 0 ? 0xFF : 0x00));
    dsp.writeRegister(kRegPmon, static_cast<uint8_t>(p.pmon != 0 ? 0xFE : 0x00));   // voices 1..7

    const FirPreset& fir = kFirPresets[std::clamp(p.firPreset, 0, kNumFirPresets - 1)];
    for (int k = 0; k < 8; ++k)
        dsp.writeRegister(k * 16 + kRegFir, static_cast<uint8_t>(fir.taps[k]));

    const auto evol = static_cast<uint8_t>(echoRunning ? static_cast<int8_t>(std::clamp(p.echoVolume, -128, 127)) : 0);
    dsp.writeRegister(kRegEvolL, evol);
    dsp.writeRegister(kRegEvolR, evol);
    const int flg = (p.noiseClock & kFlgNoiseMask) | (echoRunning ? 0 : kFlgEchoWriteDisable);
    dsp.writeRegister(kRegFlg, static_cast<uint8_t>(flg));
}

void SnesDriver::updateEcho(SnesDsp& dsp, const SnesDriverParams& p) noexcept
{
    const int wanted = std::min(std::clamp(p.echoDelay, 0, 15), maxEchoDelay(dataEnd));
    if (wanted != edl)
    {
        // Re-initialise like a real driver: echo writes off and echo return muted, move and
        // clear the buffer, then wait 240 ms so the old EDL has run out (research "Echo").
        edl = wanted;
        echoRunning = false;
        programEchoBuffer(dsp, edl);
        echoWait = kEchoWaitTicks;      // counted down by the next 60 ticks: 7680 samples
    }
    else if (echoWait > 0)
    {
        --echoWait;
    }

    const bool enable = p.echoEnable != 0 && echoWait == 0;
    if (enable && !echoRunning)
        clearEchoBuffer(dsp);            // stale content from before the echo was switched off
    echoRunning = enable;
    writeGlobals(dsp, p);
}

void SnesDriver::programEchoBuffer(SnesDsp& dsp, int newEdl) noexcept
{
    dsp.writeRegister(kRegFlg, static_cast<uint8_t>(dsp.readRegister(kRegFlg) | kFlgEchoWriteDisable));
    dsp.writeRegister(kRegEvolL, 0);
    dsp.writeRegister(kRegEvolR, 0);
    dsp.writeRegister(kRegEsa, static_cast<uint8_t>(echoBufferStart(newEdl) >> 8));
    dsp.writeRegister(kRegEdl, static_cast<uint8_t>(newEdl));
    clearEchoBuffer(dsp);
}

void SnesDriver::clearEchoBuffer(SnesDsp& dsp) noexcept
{
    const int start = echoBufferStart(edl);
    const int bytes = SnesDsp::echoLengthForEdl(edl) * 4;
    std::fill(dsp.ram() + start, dsp.ram() + start + bytes, uint8_t{ 0 });
}

void SnesDriver::onSampleMemoryChanged(SnesDsp& dsp, int sampleDataEnd) noexcept
{
    dataEnd = sampleDataEnd;
    if (edl > maxEchoDelay(dataEnd))
    {
        echoRunning = false;
        dsp.writeRegister(kRegFlg, static_cast<uint8_t>(dsp.readRegister(kRegFlg) | kFlgEchoWriteDisable));
        dsp.writeRegister(kRegEvolL, 0);
        dsp.writeRegister(kRegEvolR, 0);
    }
}

} // namespace chipdsp::snes
