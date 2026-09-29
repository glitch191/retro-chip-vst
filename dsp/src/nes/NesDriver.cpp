#include "chipdsp/nes/NesDriver.h"

#include <algorithm>
#include <cmath>

namespace chipdsp::nes
{

namespace
{
    constexpr int kPulse1 = 0, kPulse2 = 1, kTriangle = 2, kNoise = 3, kDmc = 4;
    constexpr uint8_t kToneChannelsMask = 0x0F; // pulse 1, pulse 2, triangle, noise always enabled

    // round(a / b) for a >= 0, b > 0.
    int divRound(int a, int b) noexcept { return (2 * a + b) / (2 * b); }
} // namespace

// ===== SoftwareEnvelope ======================================================================

int SoftwareEnvelope::compute(int attack, int decay, int sustain, int releaseFrames) noexcept
{
    const int sustainLevel = divRound(peak * std::clamp(sustain, 0, 15), 15);
    switch (stage)
    {
        case Stage::Attack:
            if (frame < attack)
            {
                level = divRound(peak * frame, attack);
                break;
            }
            stage = Stage::Decay;
            frame -= attack;
            [[fallthrough]];
        case Stage::Decay:
            if (frame < decay)
            {
                level = peak - divRound((peak - sustainLevel) * frame, decay);
                break;
            }
            stage = Stage::Sustain;
            [[fallthrough]];
        case Stage::Sustain:
            level = sustainLevel;
            break;
        case Stage::Release:
            if (frame < releaseFrames)
            {
                level = divRound(releaseFrom * (releaseFrames - frame), releaseFrames);
                break;
            }
            stage = Stage::Off;
            [[fallthrough]];
        case Stage::Off:
            level = 0;
            break;
    }
    return level;
}

// ===== helpers ===============================================================================

int NesDriver::pitchEnvelopeOffset(int depth, int speed, int frame) noexcept
{
    // Linear ramp from 'depth' period units at note start to 0 after 'speed' frames.
    if (speed <= 0 || frame >= speed)
        return 0;
    return depth * (speed - frame) / speed;
}

int NesDriver::vibratoOffset(int rate, int depth, int delay, int frame) noexcept
{
    // Triangle LFO in period units: 'rate' frames per half cycle, sampled at mid-frame so that
    // even rate 1 alternates +depth / -depth. Sampled at mid-frame, the largest sample is 1 for
    // odd rates but 1 - 1 / rate for even rates (the peak falls between two frames), so the
    // samples are scaled by that peak: every rate then swings exactly +/- depth.
    if (rate <= 0 || depth <= 0 || frame < delay)
        return 0;
    const int cycle = 2 * rate;
    const double x = (static_cast<double>((frame - delay) % cycle) + 0.5) / static_cast<double>(cycle);
    const double tri = x < 0.25 ? 4.0 * x : (x < 0.75 ? 2.0 - 4.0 * x : 4.0 * x - 4.0);
    const double peak = rate % 2 == 0 ? 1.0 - 1.0 / static_cast<double>(rate) : 1.0;
    return static_cast<int>(std::lround(static_cast<double>(depth) * tri / peak));
}

int NesDriver::noiseIndexForNote(float midiNote) noexcept
{
    const int n = static_cast<int>(std::lround(midiNote)) - 36;
    return std::clamp(15 - n, 0, 15);
}

int NesDriver::dmcRateForNote(int baseRate, float midiNote) noexcept
{
    return std::clamp(baseRate + static_cast<int>(std::lround(midiNote)) - 60, 0, 15);
}

bool NesDriver::isGateOn(int channel) const noexcept
{
    if (channel >= 0 && channel < 4)
        return voices[static_cast<size_t>(channel)].gate;
    return channel == kDmc && dmcGate;
}

bool NesDriver::isAttackPending(int channel, const DriverSettings& s) const noexcept
{
    if (channel < 0 || channel > kNoise)
        return false;
    const ToneVoice& v = voices[static_cast<size_t>(channel)];
    if (!v.active || v.refused)
        return false;
    const auto softwareAttack = [&v](int envEnable) {
        return envEnable == 0 && v.env.stage == SoftwareEnvelope::Stage::Attack && v.env.peak > 0;
    };
    switch (channel)
    {
        case kPulse1:
        case kPulse2: return softwareAttack(s.pulse[static_cast<size_t>(channel)].envEnable);
        case kTriangle: return v.pendingStart || (v.gate && s.triangle.gateFrames > 0);
        default: return softwareAttack(s.noise.envEnable);
    }
}

// ===== lifecycle =============================================================================

void NesDriver::reset(NesApu& apu) noexcept
{
    voices = {};
    dmcGate = false;

    // NESdev "APU basics" initialisation; $4017 = $40: 4-step sequence, IRQ inhibited (A15).
    apu.write(0x4000, 0x30);
    apu.write(0x4004, 0x30);
    apu.write(0x4001, 0x08);
    apu.write(0x4005, 0x08);
    apu.write(0x4008, 0x80);
    apu.write(0x400C, 0x30);
    apu.write(0x4015, kToneChannelsMask);
    apu.frame.reset(0x40); // applied immediately at engine reset, not 3 cycles later
}

void NesDriver::writeStatus(NesApu& apu, uint8_t channelMask) noexcept
{
    // Keep the DMC bit as it is: writing D = 1 while the sample has ended would restart it,
    // writing D = 0 while it plays would stop it.
    const uint8_t dmcBit = apu.dmc.bytesRemainingCount() > 0 ? 0x10 : 0x00;
    apu.write(0x4015, static_cast<uint8_t>(channelMask | dmcBit));
}

void NesDriver::silenceTriangle(NesApu& apu) noexcept
{
    // Clearing the enable bit forces the length counter to 0: the sequencer stops at once and
    // holds its current step (A3). Re-enabling right away lets the next $400B write load again.
    writeStatus(apu, static_cast<uint8_t>(kToneChannelsMask & ~0x04));
    writeStatus(apu, kToneChannelsMask);
}

// ===== note events ===========================================================================

void NesDriver::noteOn(NesApu& apu, int channel, float midiNote, float velocity, const DriverSettings& s,
                       const DmcSampleRef& dmcSample) noexcept
{
    velocity = std::clamp(velocity, 0.0f, 1.0f);

    if (channel == kDmc)
    {
        const DmcSettings& d = s.dmc;
        const int rate = d.keyed != 0 ? dmcRateForNote(d.rate, midiNote) : std::clamp(d.rate, 0, 15);
        dmcGate = true;

        apu.write(0x4015, kToneChannelsMask); // D = 0: stop the current sample
        apu.write(0x4010, static_cast<uint8_t>((d.loop != 0 ? 0x40 : 0x00) | rate));
        apu.write(0x4011, static_cast<uint8_t>(std::clamp(d.directLevel, 0, 127)));
        // The previous sample is stopped (bytes remaining 0), so its memory is no longer read.
        if (dmcSample.bytes != nullptr && dmcSample.length > 0)
        {
            apu.dmc.setMemory(dmcSample.bytes, dmcSample.capacity);
            apu.write(0x4012, 0x00);                                            // $C000
            apu.write(0x4013, static_cast<uint8_t>((dmcSample.length - 1) / 16)); // L * 16 + 1 bytes
            apu.write(0x4015, static_cast<uint8_t>(kToneChannelsMask | 0x10));   // start
        }
        else
        {
            apu.dmc.setMemory(nullptr, 0); // empty slot: nothing mapped at $C000
        }
        return;
    }
    if (channel < 0 || channel > kNoise)
        return;

    ToneVoice& v = voices[static_cast<size_t>(channel)];
    v.gate = true;
    v.active = true;
    v.note = midiNote;
    v.velocity = velocity;
    v.frame = 0;
    v.refused = false;
    v.lastCtrl = v.lastHigh = v.lastLow = v.lastSweep = -1;

    switch (channel)
    {
        case kPulse1:
        case kPulse2:
        {
            const PulseSettings& p = s.pulse[static_cast<size_t>(channel)];
            v.env.start(static_cast<int>(std::lround(velocity * static_cast<float>(std::clamp(p.volume, 0, 15)))));
            updatePulse(apu, channel, p, true);
            break;
        }
        case kTriangle:
        {
            const TriangleSettings& t = s.triangle;
            v.delayFrames = std::max(0, t.attackFrames);
            v.pendingStart = v.delayFrames > 0;
            if (v.pendingStart)
                silenceTriangle(apu); // attack_frames: muted first, started later
            else
                updateTriangle(apu, t, true);
            break;
        }
        case kNoise:
        {
            const NoiseSettings& n = s.noise;
            v.env.start(static_cast<int>(std::lround(velocity * static_cast<float>(std::clamp(n.volume, 0, 15)))));
            updateNoise(apu, n, true);
            break;
        }
        default:
            break;
    }
}

void NesDriver::noteOff(NesApu& apu, int channel, const DriverSettings& s) noexcept
{
    if (channel == kDmc)
    {
        dmcGate = false;
        // Looping samples stop; one-shots play out. The decision uses the loop flag the chip
        // holds ($4010 written at note-on), not the live dmc_loop parameter.
        if (apu.dmc.loopFlag())
            apu.write(0x4015, kToneChannelsMask);
        return;
    }
    if (channel < 0 || channel > kNoise)
        return;

    ToneVoice& v = voices[static_cast<size_t>(channel)];
    if (!v.gate)
        return;
    v.gate = false;

    switch (channel)
    {
        case kPulse1:
        case kPulse2:
            v.env.release();
            updatePulse(apu, channel, s.pulse[static_cast<size_t>(channel)], false);
            break;
        case kTriangle:
            v.active = false;
            v.pendingStart = false;
            silenceTriangle(apu);
            break;
        case kNoise:
            v.env.release();
            updateNoise(apu, s.noise, false);
            break;
        default:
            break;
    }
}

void NesDriver::setPitch(int channel, float midiNote) noexcept
{
    if (channel >= 0 && channel < 4)
        voices[static_cast<size_t>(channel)].note = midiNote; // picked up at the next tick
}

// ===== per-frame update ======================================================================

void NesDriver::deferFirstTick(int channel) noexcept
{
    if (channel >= 0 && channel < 4)
        voices[static_cast<size_t>(channel)].skipTick = true;
}

void NesDriver::tick(NesApu& apu, const DriverSettings& s) noexcept
{
    for (int c = 0; c < 4; ++c)
    {
        ToneVoice& v = voices[static_cast<size_t>(c)];
        if (!v.active)
            continue;
        if (v.skipTick)
        {
            v.skipTick = false;
            continue;
        }

        if (c == kTriangle && v.pendingStart)
        {
            if (--v.delayFrames <= 0)
            {
                v.pendingStart = false;
                v.frame = 0;
                updateTriangle(apu, s.triangle, true);
            }
            continue;
        }

        ++v.frame;
        ++v.env.frame;
        if (c == kPulse1 || c == kPulse2)
            updatePulse(apu, c, s.pulse[static_cast<size_t>(c)], false);
        else if (c == kTriangle)
            updateTriangle(apu, s.triangle, false);
        else
            updateNoise(apu, s.noise, false);
    }
}

void NesDriver::updatePulse(NesApu& apu, int index, const PulseSettings& s, bool noteStart) noexcept
{
    ToneVoice& v = voices[static_cast<size_t>(index)];
    const uint16_t base = static_cast<uint16_t>(0x4000 + 4 * index);
    const PulseChannel& hw = index == 0 ? apu.pulse1 : apu.pulse2;
    const int duty = std::clamp(s.duty, 0, 3) << 6;

    // Volume: hardware envelope ($4000 C = 0, V = decay period) or the software ADSR written
    // as a constant volume with the length counter halted.
    int ctrl;
    if (s.envEnable != 0)
    {
        const bool loop = s.envLoop != 0 && v.gate; // note off clears loop: the decay runs out
        ctrl = duty | (loop ? 0x20 : 0x00) | std::clamp(s.volume, 0, 15);
        if (!v.gate && !noteStart && hw.volume() == 0)
            v.active = false;
    }
    else
    {
        const int vol = v.env.compute(s.swAttack, s.swDecay, s.swSustain, s.swRelease);
        ctrl = duty | 0x30 | vol;
        if (!v.gate && v.env.stage == SoftwareEnvelope::Stage::Off)
            v.active = false;
    }

    // Period: note -> 11-bit timer, plus the software pitch envelope and vibrato.
    const int basePeriod = pulsePeriodForNote(static_cast<double>(v.note) + s.transpose, apu.cpuHz());
    const bool refused = basePeriod > 0x7FF; // not representable (A16): the driver keeps it silent
    v.refused = refused;
    if (refused)
        ctrl = duty | 0x30;
    int period = std::clamp(basePeriod + pitchEnvelopeOffset(s.pitchEnvDepth, s.pitchEnvSpeed, v.frame)
                                + vibratoOffset(s.vibratoRate, s.vibratoDepth, s.vibratoDelay, v.frame),
                            0, 0x7FF);
    // Released hardware-envelope note: a $4003 write would set the envelope start flag (decay
    // back to 15) and reload the length counter, so the note would never end. The driver then
    // keeps the timer inside the page of the last high bits written and only rewrites the low
    // byte (A24).
    const bool holdHighBits = !noteStart && !v.gate && s.envEnable != 0 && v.lastHigh >= 0;
    if (holdHighBits)
        period = std::clamp(period, v.lastHigh << 8, (v.lastHigh << 8) | 0xFF);
    const int low = period & 0xFF;
    const int high = period >> 8;

    if (noteStart || ctrl != v.lastCtrl)
    {
        apu.write(base, static_cast<uint8_t>(ctrl));
        v.lastCtrl = ctrl;
    }

    // Sweep off: $08 like NESdev "APU basics". With negate clear and shift 0 the target would be
    // 2 * period and every period >= $400 (below about MIDI 45) would be muted even with E = 0.
    const int sweep = s.sweepEnable == 0
                          ? 0x08
                          : 0x80 | (std::clamp(s.sweepPeriod, 0, 7) << 4) | (s.sweepNegate != 0 ? 0x08 : 0x00)
                                | std::clamp(s.sweepShift, 0, 7);
    if (noteStart || sweep != v.lastSweep)
    {
        apu.write(static_cast<uint16_t>(base + 1), static_cast<uint8_t>(sweep)); // sets the sweep reload flag
        v.lastSweep = sweep;
    }

    if (noteStart)
    {
        apu.write(static_cast<uint16_t>(base + 2), static_cast<uint8_t>(low));
        apu.write(static_cast<uint16_t>(base + 3), static_cast<uint8_t>((kLengthIndexLongest << 3) | high));
        v.lastLow = low;
        v.lastHigh = high;
        return;
    }

    // While the hardware sweep is moving the period, the driver leaves the timer alone.
    const bool sweeping = s.sweepEnable != 0 && s.sweepShift > 0;
    if (sweeping || refused)
        return;
    if (low != v.lastLow)
    {
        apu.write(static_cast<uint16_t>(base + 2), static_cast<uint8_t>(low));
        v.lastLow = low;
    }
    // $4003 restarts the sequencer (audible phase reset): only written when the high bits change.
    if (high != v.lastHigh)
    {
        apu.write(static_cast<uint16_t>(base + 3), static_cast<uint8_t>((kLengthIndexLongest << 3) | high));
        v.lastHigh = high;
    }
}

void NesDriver::updateTriangle(NesApu& apu, const TriangleSettings& s, bool noteStart) noexcept
{
    ToneVoice& v = voices[kTriangle];

    // linear_length 127 = hold: control flag set, the counter is reloaded every quarter frame.
    const int ctrl = s.linearLength >= 127 ? 0xFF : std::clamp(s.linearLength, 0, 127);
    const int basePeriod = trianglePeriodForNote(static_cast<double>(v.note) + s.transpose, apu.cpuHz());

    // Same rule as the pulses (A16): a base period above $7FF is not representable, the note is
    // refused (triangle halted) instead of being clamped to a wrong pitch. NTSC MIDI <= 20 and
    // PAL MIDI <= 19.
    if (basePeriod > 0x7FF)
    {
        if (noteStart || !v.refused)
            silenceTriangle(apu);
        v.refused = true;
        v.lastLow = v.lastHigh = -1; // the next representable pitch writes $400A / $400B again
        return;
    }
    v.refused = false;

    const int period = std::clamp(basePeriod + pitchEnvelopeOffset(s.pitchEnvDepth, s.pitchEnvSpeed, v.frame)
                                      + vibratoOffset(s.vibratoRate, s.vibratoDepth, s.vibratoDelay, v.frame),
                                  0, 0x7FF);
    const int low = period & 0xFF;
    const int high = period >> 8;
    const uint8_t highByte = static_cast<uint8_t>((kLengthIndexLongest << 3) | high);

    if (noteStart || ctrl != v.lastCtrl)
    {
        apu.write(0x4008, static_cast<uint8_t>(ctrl));
        v.lastCtrl = ctrl;
    }
    if (noteStart || low != v.lastLow)
    {
        apu.write(0x400A, static_cast<uint8_t>(low));
        v.lastLow = low;
    }
    // $400B sets the linear counter reload flag: written at note start, when the high bits
    // change, and every gate_frames frames (retrigger, driver behaviour).
    const bool retrigger = !noteStart && s.gateFrames > 0 && v.frame % s.gateFrames == 0;
    if (noteStart || high != v.lastHigh || retrigger)
    {
        apu.write(0x400B, highByte);
        v.lastHigh = high;
    }
}

void NesDriver::updateNoise(NesApu& apu, const NoiseSettings& s, bool noteStart) noexcept
{
    ToneVoice& v = voices[kNoise];

    int ctrl;
    if (s.envEnable != 0)
    {
        const bool loop = s.envLoop != 0 && v.gate;
        ctrl = (loop ? 0x20 : 0x00) | std::clamp(s.volume, 0, 15);
        if (!v.gate && !noteStart && apu.noise.volume() == 0)
            v.active = false;
    }
    else
    {
        const int vol = v.env.compute(s.swAttack, s.swDecay, s.swSustain, s.swRelease);
        ctrl = 0x30 | vol;
        if (!v.gate && v.env.stage == SoftwareEnvelope::Stage::Off)
            v.active = false;
    }

    const int baseIndex = s.keyed != 0 ? noiseIndexForNote(v.note) : std::clamp(s.period, 0, 15);
    const int index = std::clamp(baseIndex + pitchEnvelopeOffset(s.pitchEnvDepth, s.pitchEnvSpeed, v.frame), 0, 15);
    const int periodReg = (s.mode != 0 ? 0x80 : 0x00) | index;

    if (noteStart || ctrl != v.lastCtrl)
    {
        apu.write(0x400C, static_cast<uint8_t>(ctrl));
        v.lastCtrl = ctrl;
    }
    if (noteStart || periodReg != v.lastLow)
    {
        apu.write(0x400E, static_cast<uint8_t>(periodReg));
        v.lastLow = periodReg;
    }
    if (noteStart)
        apu.write(0x400F, static_cast<uint8_t>(kLengthIndexLongest << 3)); // length load + envelope start
}

} // namespace chipdsp::nes
