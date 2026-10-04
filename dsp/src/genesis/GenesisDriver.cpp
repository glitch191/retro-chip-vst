#include "chipdsp/genesis/GenesisDriver.h"

#include <algorithm>
#include <cmath>

namespace chipdsp::genesis
{

namespace
{
    constexpr int kFirstPsg = 6;
    constexpr int kNoiseChannel = 9;

    // pan 0 = L, 1 = C, 2 = R, 3 = off -> $B4 bits 7 (L) and 6 (R).
    constexpr int panBits(int pan) noexcept
    {
        switch (pan)
        {
            case 0: return 0x80;
            case 2: return 0x40;
            case 3: return 0x00;
            default: return 0xC0;
        }
    }

    constexpr int ssgRegister(int ssg) noexcept { return ssg <= 0 ? 0 : 0x08 | ((ssg - 1) & 7); }
} // namespace

// ----- helpers ---------------------------------------------------------------------------------

int GenesisDriver::vibratoOffset(int frames, int rate, int depth, int delay) noexcept
{
    // Software vibrato: triangle, 'rate' frames per half cycle, starting near the centre.
    if (rate <= 0 || depth <= 0 || frames < delay)
        return 0;
    const int p = (frames - delay + rate / 2) % (2 * rate);
    const double v = p < rate ? -depth + 2.0 * depth * p / rate : depth - 2.0 * depth * (p - rate) / rate;
    return static_cast<int>(std::lround(v));
}

int GenesisDriver::velocityToTl(float velocity, int depth) noexcept
{
    const float v = std::clamp(velocity, 0.0f, 1.0f);
    return static_cast<int>(std::lround((1.0f - v) * static_cast<float>(depth)));
}

int GenesisDriver::velocityToPsgAttenuation(float velocity) noexcept
{
    const float v = std::clamp(velocity, 0.0f, 1.0f);
    return static_cast<int>(std::lround((1.0f - v) * 15.0f));
}

FmPitch GenesisDriver::fmPitchFor(int c) const noexcept
{
    const Voice& v = voices[c];
    const double note = static_cast<double>(v.note) + settings.transpose + settings.fineTune / 100.0;
    FmPitch p = fmPitchFromNote(note, settings.clock);
    const int vib = vibratoOffset(v.frames, settings.vibratoRate, settings.vibratoDepth, settings.vibratoDelay);
    p.fnum = std::clamp(p.fnum + vib + v.detune, 0, 2047);
    return p;
}

int GenesisDriver::psgPeriodFor(int c) const noexcept
{
    const Voice& v = voices[c];
    double note = static_cast<double>(v.note) + settings.psgTranspose;
    if (c == kNoiseChannel && settings.noiseMode == 0)
        note += 48.0;   // periodic noise sounds 4 octaves below tone 3 (research "Noise channel")
    const int base = psgPeriodFromNote(note, settings.clock);
    const int vib = c == kNoiseChannel ? 0 : vibratoOffset(v.frames, settings.psgVibratoRate, settings.psgVibratoDepth, 0);
    return std::clamp(base + vib + v.detune, 0, 1023);
}

// ----- register writers --------------------------------------------------------------------------

void GenesisDriver::writeYmForced(int bank, int reg, int value) noexcept
{
    ymShadow[bank][reg] = static_cast<int16_t>(value);
    ym->write(bank, static_cast<uint8_t>(reg), static_cast<uint8_t>(value));
}

void GenesisDriver::writeYm(int bank, int reg, int value) noexcept
{
    if (ymShadow[bank][reg] != value)
        writeYmForced(bank, reg, value);
}

void GenesisDriver::writeFmPatch(int c) noexcept
{
    const int bank = c / 3;
    const int cc = c % 3;
    const Voice& v = voices[c];
    const uint8_t carriers = kAlgorithm[settings.algorithm & 7].carrierMask;
    const int velocityTl = velocityToTl(v.velocity, settings.velocityDepth);

    for (int s = 0; s < 4; ++s)
    {
        const OperatorPatch& p = settings.op[s];
        const int off = kOpRegOffset[cc][s];
        int tl = p.tl;
        if ((carriers >> s) & 1)
            tl = std::min(127, tl + velocityTl);
        writeYm(bank, 0x30 + off, ((p.dt & 7) << 4) | (p.mul & 15));
        writeYm(bank, 0x40 + off, tl & 0x7F);
        writeYm(bank, 0x50 + off, ((p.rs & 3) << 6) | (p.ar & 31));
        writeYm(bank, 0x60 + off, ((p.am & 1) << 7) | (p.dr & 31));
        writeYm(bank, 0x70 + off, p.sr & 31);
        writeYm(bank, 0x80 + off, ((p.sl & 15) << 4) | (p.rr & 15));
        writeYm(bank, 0x90 + off, ssgRegister(p.ssg));
    }
    writeYm(bank, 0xB0 + cc, ((settings.feedback & 7) << 3) | (settings.algorithm & 7));
    writeYm(bank, 0xB4 + cc, panBits(settings.pan[c]) | ((settings.ams & 3) << 4) | (settings.fms & 7));
}

void GenesisDriver::writeFmFrequency(int c, FmPitch p, bool force) noexcept
{
    // Research "Frequency write order": $A4 (block, fnum high) first, then $A0.
    const int packed = (p.block << 11) | p.fnum;
    if (!force && fmBlockFnum[c] == packed)
        return;
    fmBlockFnum[c] = packed;
    const int bank = c / 3;
    const int cc = c % 3;
    writeYmForced(bank, 0xA4 + cc, ((p.block & 7) << 3) | ((p.fnum >> 8) & 7));
    writeYmForced(bank, 0xA0 + cc, p.fnum & 0xFF);
}

void GenesisDriver::writeGlobals() noexcept
{
    writeYm(0, 0x22, settings.lfoEnable ? (0x08 | (settings.lfoFreq & 7)) : 0);
    writeYm(0, 0x2B, dacMode() ? 0x80 : 0);
}

void GenesisDriver::writePsgPeriod(int tone, int period) noexcept
{
    if (psgPeriodShadow[tone] == period)
        return;
    psgPeriodShadow[tone] = period;
    psg->write(static_cast<uint8_t>(0x80 | (tone << 5) | (period & 0x0F)));
    psg->write(static_cast<uint8_t>((period >> 4) & 0x3F));
}

void GenesisDriver::writePsgAttenuation(int ch, int att) noexcept
{
    if (psgAttShadow[ch] == att)
        return;
    psgAttShadow[ch] = att;
    psg->write(static_cast<uint8_t>(0x90 | (ch << 5) | (att & 0x0F)));
}

void GenesisDriver::writeNoiseControl(bool force) noexcept
{
    // Every write resets the LFSR, so the driver only writes on note on or on a change.
    const int value = ((settings.noiseMode & 1) << 2) | (settings.noiseRate & 3);
    if (!force && noiseShadow == value)
        return;
    noiseShadow = value;
    psg->write(static_cast<uint8_t>(0xE0 | value));
}

// ----- lifecycle ---------------------------------------------------------------------------------

void GenesisDriver::reset(const DriverSettings& s) noexcept
{
    settings = s;
    ym->reset();
    psg->reset();
    for (auto& row : ymShadow)
        for (auto& r : row)
            r = -1;
    for (auto& v : voices)
        v = Voice{};
    dac = DacState{};
    noiseOwnsTone3 = false;
    for (auto& f : fmBlockFnum)
        f = -1;
    for (auto& p : psgPeriodShadow)
        p = -1;
    for (auto& a : psgAttShadow)
        a = -1;
    noiseShadow = -1;

    // Init sequence: globals, channel 3 normal mode, all keys off, patches, DAC centred, PSG muted.
    writeGlobals();
    writeYmForced(0, 0x27, 0x00);
    writeYmForced(0, 0x2A, 0x80);
    for (int c = 0; c < 6; ++c)
    {
        writeYmForced(0, 0x28, kKeyOnCode[c]);
        writeFmPatch(c);
    }
    for (int ch = 0; ch < 4; ++ch)
        writePsgAttenuation(ch, 15);
}

void GenesisDriver::setDacBank(const DacSampleView* slots) noexcept
{
    dacBank = slots;
    if (dac.playing && (dacBank == nullptr || dac.pos >= dacBank[dac.slot].length))
        dacStop();
}

// ----- notes -------------------------------------------------------------------------------------

void GenesisDriver::noteOn(int c, float midiNote, float velocity) noexcept
{
    if (c < 0 || c > kNoiseChannel)
        return;
    if (c == kDacChannel && dacMode())
    {
        dacNoteOn(midiNote, velocity);
        return;
    }
    if (c == kFirstPsg + 2 && noiseOwnsTone3)
        return;   // tone 3 is driven by the noise channel

    const bool isFm = c < kFirstPsg;
    const int detune = isFm ? settings.unisonDetune : settings.psgUnisonDetune;
    const bool unison = detune > 0 && c != kNoiseChannel;

    // A retrigger of an owner keeps its own unison partner instead of releasing it and keying
    // yet another channel (the old partner would still be releasing and look busy).
    int reuse = -1;
    if (unison && voices[c].partner >= 0 && !(isFm && voices[c].partner == kDacChannel && dacMode()))
    {
        reuse = voices[c].partner;
        voices[reuse].owner = -1;
        voices[c].partner = -1;
    }
    detachUnison(c);

    Voice& v = voices[c];
    v.gate = true;
    v.started = true;
    v.note = midiNote;
    v.velocity = velocity;
    v.frames = 0;
    v.detune = 0;

    if (isFm)
        fmNoteOn(c);
    else
        psgNoteOn(c);

    // Unison (software): the note also drives the next free channel of the same kind.
    if (unison)
    {
        const int p = reuse >= 0 ? reuse : isFm ? findFreeFm(c) : findFreeTone(c);
        if (p >= 0)
        {
            detachUnison(p);
            Voice& u = voices[p];
            u = v;
            u.partner = -1;
            u.owner = c;
            u.detune = detune;
            v.partner = p;
            if (isFm)
                fmNoteOn(p);
            else
                psgNoteOn(p);
        }
    }
}

void GenesisDriver::noteOff(int c) noexcept
{
    if (c < 0 || c > kNoiseChannel)
        return;
    if (c == kDacChannel && dacMode())
    {
        dac.gate = false;
        if (dac.loop)
            dacStop();   // one-shot samples play to their end
        return;
    }
    Voice& v = voices[c];
    if (v.owner >= 0)
        return;   // a unison partner follows its owner only
    releaseVoice(c);
    if (v.partner >= 0)
    {
        const int p = v.partner;
        voices[p].owner = -1;
        v.partner = -1;
        releaseVoice(p);
    }
}

void GenesisDriver::setPitch(int c, float midiNote) noexcept
{
    if (c < 0 || c > kNoiseChannel)
        return;
    voices[c].note = midiNote;
    if (voices[c].partner >= 0)
        voices[voices[c].partner].note = midiNote;
}

bool GenesisDriver::isActive(int c) const noexcept
{
    if (c < 0 || c > kNoiseChannel)
        return false;
    const Voice& v = voices[c];
    if (c < kFirstPsg)
    {
        if (c == kDacChannel && dacMode())
            return dac.playing;
        return v.gate || (v.started && !ym->channelIdle(c));
    }
    if (c == kFirstPsg + 2 && noiseOwnsTone3)
        return true;
    return v.stage != EnvStage::Off;
}

void GenesisDriver::releaseVoice(int c) noexcept
{
    Voice& v = voices[c];
    v.gate = false;
    if (c < kFirstPsg)
    {
        fmKeyOff(c);
        return;
    }
    envRelease(v);
    psgUpdate(c);
}

void GenesisDriver::detachUnison(int c) noexcept
{
    Voice& v = voices[c];
    if (v.owner >= 0)
    {
        voices[v.owner].partner = -1;
        v.owner = -1;
    }
    if (v.partner >= 0)
    {
        const int p = v.partner;
        voices[p].owner = -1;
        v.partner = -1;
        releaseVoice(p);
    }
}

int GenesisDriver::findFreeFm(int from) const noexcept
{
    for (int i = 1; i < 6; ++i)
    {
        const int c = (from + i) % 6;
        if (c == kDacChannel && dacMode())
            continue;
        if (!isActive(c))
            return c;
    }
    return -1;
}

int GenesisDriver::findFreeTone(int from) const noexcept
{
    for (int i = 1; i < 3; ++i)
    {
        const int c = kFirstPsg + (from - kFirstPsg + i) % 3;
        if (!isActive(c))
            return c;
    }
    return -1;
}

// ----- FM ------------------------------------------------------------------------------------------

void GenesisDriver::fmNoteOn(int c) noexcept
{
    // Key off, patch, frequency ($A4 then $A0), key on all four operators.
    writeYmForced(0, 0x28, kKeyOnCode[c]);
    writeFmPatch(c);
    writeFmFrequency(c, fmPitchFor(c), true);
    writeYmForced(0, 0x28, 0xF0 | kKeyOnCode[c]);
}

void GenesisDriver::fmKeyOff(int c) noexcept
{
    writeYmForced(0, 0x28, kKeyOnCode[c]);
}

// ----- PSG -----------------------------------------------------------------------------------------

void GenesisDriver::envEnterDecay(Voice& v) noexcept
{
    v.level = 15;
    v.stageFrames = 0;
    v.stage = EnvStage::Decay;
    if (settings.psgDecay <= 0)
    {
        v.level = settings.psgSustain;
        v.stage = EnvStage::Sustain;
    }
}

void GenesisDriver::envStart(Voice& v) noexcept
{
    v.stageFrames = 0;
    if (settings.psgAttack <= 0)
    {
        envEnterDecay(v);
        return;
    }
    v.stage = EnvStage::Attack;
    v.level = 0;
}

void GenesisDriver::envRelease(Voice& v) noexcept
{
    if (v.stage == EnvStage::Off)
        return;
    v.releaseFrom = v.level;
    v.stageFrames = 0;
    v.stage = EnvStage::Release;
    if (settings.psgRelease <= 0)
    {
        v.level = 0;
        v.stage = EnvStage::Off;
    }
}

void GenesisDriver::envTick(Voice& v) noexcept
{
    // Software volume envelope in frames (driver behaviour, not chip behaviour).
    switch (v.stage)
    {
        case EnvStage::Off: break;
        case EnvStage::Attack:
        {
            const int a = settings.psgAttack;
            ++v.stageFrames;
            if (a <= 0 || v.stageFrames >= a)
                envEnterDecay(v);
            else
                v.level = (15 * v.stageFrames + a / 2) / a;
            break;
        }
        case EnvStage::Decay:
        {
            const int d = settings.psgDecay;
            const int s = settings.psgSustain;
            ++v.stageFrames;
            if (d <= 0 || v.stageFrames >= d)
            {
                v.level = s;
                v.stage = EnvStage::Sustain;
            }
            else
            {
                v.level = 15 - ((15 - s) * v.stageFrames + d / 2) / d;
            }
            break;
        }
        case EnvStage::Sustain: v.level = settings.psgSustain; break;
        case EnvStage::Release:
        {
            const int r = settings.psgRelease;
            ++v.stageFrames;
            if (r <= 0 || v.stageFrames >= r)
            {
                v.level = 0;
                v.stage = EnvStage::Off;
            }
            else
            {
                v.level = v.releaseFrom - (v.releaseFrom * v.stageFrames + r / 2) / r;
            }
            break;
        }
    }
}

int GenesisDriver::psgAttenuationFor(int c) const noexcept
{
    const Voice& v = voices[c];
    if (v.stage == EnvStage::Off)
        return 15;
    const int base = settings.psgAtt[c - kFirstPsg];
    return std::min(15, base + (15 - v.level) + velocityToPsgAttenuation(v.velocity));
}

void GenesisDriver::psgNoteOn(int c) noexcept
{
    Voice& v = voices[c];
    envStart(v);
    if (c == kNoiseChannel)
    {
        writeNoiseControl(true);
        if ((settings.noiseRate & 3) == 3)
            takeOverTone3();
    }
    psgUpdate(c);
}

void GenesisDriver::takeOverTone3() noexcept
{
    // Tone 3 becomes the noise clock: the driver takes it over and mutes it (research "MIDI note
    // -> registers": channel 8 notes are ignored meanwhile). A tone-3 note that was sounding ends.
    noiseOwnsTone3 = true;
    Voice& t3 = voices[kFirstPsg + 2];
    detachUnison(kFirstPsg + 2);
    t3.stage = EnvStage::Off;
    t3.gate = false;
    writePsgAttenuation(2, 15);
}

void GenesisDriver::psgUpdate(int c) noexcept
{
    const int ch = c - kFirstPsg;
    if (c == kNoiseChannel)
    {
        if (voices[c].stage != EnvStage::Off)
        {
            writeNoiseControl(false);
            const bool wantsTone3 = (settings.noiseRate & 3) == 3;
            if (wantsTone3 && !noiseOwnsTone3)
                takeOverTone3();   // psg_noise_rate switched to 3 during the noise note
            noiseOwnsTone3 = wantsTone3;
            if (noiseOwnsTone3)
            {
                writePsgPeriod(2, psgPeriodFor(c));
                writePsgAttenuation(2, 15);
            }
        }
        else if (noiseOwnsTone3)
        {
            noiseOwnsTone3 = false;
        }
    }
    else if (voices[c].stage != EnvStage::Off)
    {
        writePsgPeriod(ch, psgPeriodFor(c));
    }
    writePsgAttenuation(ch, psgAttenuationFor(c));
}

// ----- DAC -----------------------------------------------------------------------------------------

void GenesisDriver::dacNoteOn(float midiNote, float velocity) noexcept
{
    const int slot = std::clamp(settings.dacSample, 0, kDacSlots - 1);
    if (dacBank == nullptr || dacBank[slot].length <= 0)
    {
        dacStop();
        return;
    }
    double rate = static_cast<double>(settings.dacRate);
    if (settings.dacKeyed)
        rate *= std::pow(2.0, (static_cast<double>(midiNote) - 60.0) / 12.0);
    rate = std::clamp(rate, 100.0, fmSampleRate(settings.clock));

    dac.slot = slot;
    dac.pos = 0;
    dac.loop = settings.dacLoop != 0;
    dac.gate = true;
    dac.gain = static_cast<int>(std::lround(std::clamp(velocity, 0.0f, 1.0f) * static_cast<float>(settings.dacVolume)));
    dac.interval = masterClock(settings.clock) / rate;
    dac.playing = true;
    dac.restart = true;
}

void GenesisDriver::dacStop() noexcept
{
    dac.playing = false;
    dac.restart = false;
    writeYmForced(0, 0x2A, 0x80);
}

void GenesisDriver::dacWriteNext() noexcept
{
    if (!dac.playing || dacBank == nullptr)
        return;
    const DacSampleView& s = dacBank[dac.slot];
    if (dac.pos >= s.length)
    {
        dacStop();
        return;
    }
    // Software volume: the driver scales the signed sample before writing $2A, rounded to
    // nearest (symmetric around 0x80; 127 is odd, so there are no ties).
    const int product = (static_cast<int>(s.data[dac.pos]) - 128) * dac.gain;
    const int rounded = product >= 0 ? (product + 63) / 127 : -((-product + 63) / 127);
    const int scaled = 128 + rounded;
    writeYmForced(0, 0x2A, std::clamp(scaled, 0, 255));
    if (++dac.pos >= s.length)
    {
        if (dac.loop)
            dac.pos = 0;
        else
            dac.playing = false;   // the last byte stays in $2A until the next write
    }
}

// ----- frame tick ----------------------------------------------------------------------------------

void GenesisDriver::frameTick() noexcept
{
    writeGlobals();
    if (!dacMode() && dac.playing)
        dacStop();

    for (int c = 0; c < 6; ++c)
    {
        Voice& v = voices[c];
        if (!v.started || (c == kDacChannel && dacMode()))
            continue;
        ++v.frames;
        writeFmPatch(c);
        writeFmFrequency(c, fmPitchFor(c), false);
        if (!v.gate && ym->channelIdle(c))
            v.started = false;
    }

    for (int c = kFirstPsg; c <= kNoiseChannel; ++c)
    {
        Voice& v = voices[c];
        if (v.stage == EnvStage::Off)
        {
            if (c == kNoiseChannel && noiseOwnsTone3)
                noiseOwnsTone3 = false;
            continue;
        }
        if (v.skipTick)
        {
            v.skipTick = false;
            continue;
        }
        ++v.frames;
        envTick(v);
        psgUpdate(c);
    }
}

void GenesisDriver::deferFirstTick(int c) noexcept
{
    if (c < kFirstPsg || c > kNoiseChannel)
        return;
    voices[c].skipTick = true;
    if (const int p = voices[c].partner; p >= kFirstPsg && p <= kNoiseChannel)
        voices[p].skipTick = true;
}

} // namespace chipdsp::genesis
