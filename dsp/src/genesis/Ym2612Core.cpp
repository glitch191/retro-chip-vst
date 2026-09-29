#include "chipdsp/genesis/Ym2612Core.h"

#include <algorithm>

namespace chipdsp::genesis
{

void Ym2612Core::reset() noexcept
{
    for (auto& ch : channels)
        ch = Channel{};
    address = 0;
    addressBank = 0;
    lfoOn = false;
    lfoFreq = 0;
    ch3Mode = 0;
    dacOn = false;
    dacData = 0x80;
    lfo = 0;
    lfoDivider = 0;
    egSub = 0;
    egCount = 0;
}

void Ym2612Core::writePort(int port, uint8_t value) noexcept
{
    // Research "Registers": one data port; the address write selects the bank.
    if ((port & 1) == 0)
    {
        address = value;
        addressBank = (port >> 1) & 1;
    }
    else
    {
        writeRegister(addressBank, address, value);
    }
}

void Ym2612Core::write(int bank, uint8_t reg, uint8_t value) noexcept
{
    writePort((bank & 1) * 2, reg);
    writePort((bank & 1) * 2 + 1, value);
}

void Ym2612Core::writeRegister(int bank, uint8_t reg, uint8_t value) noexcept
{
    if (reg < 0x30)
    {
        // Global registers exist in bank 0 only.
        if (bank != 0)
            return;
        switch (reg)
        {
            case 0x22:
                lfoOn = (value & 0x08) != 0;
                lfoFreq = static_cast<uint8_t>(value & 7);
                if (!lfoOn)
                {
                    // Research "LFO": disabling holds the counter at 0.
                    lfo = 0;
                    lfoDivider = 0;
                }
                break;
            case 0x27: ch3Mode = static_cast<uint8_t>(value >> 6); break;
            case 0x28: writeKeyOnOff(value); break;
            case 0x2A: dacData = value; break;
            case 0x2B: dacOn = (value & 0x80) != 0; break;
            default: break;   // timers and test registers: not modelled
        }
        return;
    }

    const int c = reg & 3;
    if (c == 3)
        return;
    Channel& ch = channels[bank * 3 + c];

    if (reg < 0xA0)
    {
        // Operator order in the address is S1, S3, S2, S4 (research "Registers").
        const int slot = ((reg >> 3) & 1) | ((reg >> 1) & 2);
        Operator& op = ch.op[slot];
        switch (reg & 0xF0)
        {
            case 0x30:
                op.dt = static_cast<uint8_t>((value >> 4) & 7);
                op.mul = static_cast<uint8_t>(value & 15);
                break;
            case 0x40: op.tl = static_cast<uint8_t>(value & 0x7F); break;
            case 0x50:
                op.rs = static_cast<uint8_t>(value >> 6);
                op.ar = static_cast<uint8_t>(value & 0x1F);
                break;
            case 0x60:
                op.amOn = static_cast<uint8_t>(value >> 7);
                op.dr = static_cast<uint8_t>(value & 0x1F);
                break;
            case 0x70: op.sr = static_cast<uint8_t>(value & 0x1F); break;
            case 0x80:
                op.sl = static_cast<uint8_t>(value >> 4);
                op.rr = static_cast<uint8_t>(value & 15);
                break;
            case 0x90: op.ssg = static_cast<uint8_t>(value & 15); break;
            default: break;
        }
        return;
    }

    switch (reg & 0xFC)
    {
        case 0xA0:
            // The low byte applies the latched block/fnum-high write (research "Frequency write order").
            ch.fnum = static_cast<uint16_t>(((ch.fnumHighLatch & 7) << 8) | value);
            ch.block = static_cast<uint8_t>((ch.fnumHighLatch >> 3) & 7);
            break;
        case 0xA4: ch.fnumHighLatch = static_cast<uint8_t>(value & 0x3F); break;
        case 0xA8:
        case 0xAC:
            break;   // channel 3 special-mode frequencies: not modelled
        case 0xB0:
            ch.feedback = static_cast<uint8_t>((value >> 3) & 7);
            ch.algorithm = static_cast<uint8_t>(value & 7);
            break;
        case 0xB4: ch.panAmsFms = static_cast<uint8_t>(value & 0xF7); break;
        default: break;
    }
}

void Ym2612Core::writeKeyOnOff(uint8_t value) noexcept
{
    const int c = kKeyOnChannel[value & 7];
    if (c < 0)
        return;
    Channel& ch = channels[c];
    for (int s = 0; s < 4; ++s)
    {
        if ((value >> (4 + s)) & 1)
            keyOn(ch, ch.op[s]);
        else
            keyOff(ch.op[s]);
    }
}

void Ym2612Core::keyOn(Channel& ch, Operator& op) noexcept
{
    // Research "Phase transitions": only an off -> on change acts. Attenuation is not reset,
    // except that rates 62/63 skip the attack.
    if (op.keyed)
        return;
    op.keyed = true;
    op.phase = EgPhase::Attack;
    op.phase20 = 0;
    op.ssgInvert = false;
    if (attackRate(ch, op) >= 62)
        op.att = 0;
}

void Ym2612Core::keyOff(Operator& op) noexcept
{
    if (!op.keyed)
        return;
    op.keyed = false;
    // Research "SSG-EG": key-off applies the output inversion to the stored value.
    const bool invertedOutput = ((op.ssg >> 2) & 1) != (op.ssgInvert ? 1 : 0);
    if ((op.ssg & 8) && op.phase != EgPhase::Release && invertedOutput)
        op.att = static_cast<uint16_t>((0x200 - op.att) & 0x3FF);
    op.ssgInvert = false;
    op.phase = EgPhase::Release;
}

int Ym2612Core::attackRate(const Channel& ch, const Operator& op) const noexcept
{
    return envelopeRate(op.ar, keyCode(ch.block, ch.fnum), op.rs);
}

int Ym2612Core::currentRate(const Channel& ch, const Operator& op) const noexcept
{
    // Research "Rate calculation"; rates follow register changes immediately (Ambiguities 6).
    const int kc = keyCode(ch.block, ch.fnum);
    switch (op.phase)
    {
        case EgPhase::Attack: return envelopeRate(op.ar, kc, op.rs);
        case EgPhase::Decay: return envelopeRate(op.dr, kc, op.rs);
        case EgPhase::Sustain: return envelopeRate(op.sr, kc, op.rs);
        case EgPhase::Release: break;
    }
    return envelopeRate(op.rr * 2 + 1, kc, op.rs);
}

int Ym2612Core::egOutput(const Channel& ch, const Operator& op) const noexcept
{
    int att = op.att;
    const bool invertedOutput = ((op.ssg >> 2) & 1) != (op.ssgInvert ? 1 : 0);
    if ((op.ssg & 8) && op.phase != EgPhase::Release && invertedOutput)
        att = (0x200 - att) & 0x3FF;
    int v = att + totalLevelToAttenuation(op.tl);
    if (op.amOn)
        v += amAttenuation(lfo, (ch.panAmsFms >> 4) & 3);
    return std::min(v, kEgMaxAttenuation);
}

void Ym2612Core::ssgUpdate(Channel& ch, Operator& op) noexcept
{
    // Research "SSG-EG": runs every FM sample, only once the attenuation reached 0x200.
    if (op.att < 0x200)
        return;
    const bool alt = (op.ssg & 2) != 0;
    const bool hold = (op.ssg & 1) != 0;
    const bool attBit = (op.ssg & 4) != 0;

    if (alt)
        op.ssgInvert = hold ? true : !op.ssgInvert;
    if (!alt && !hold)
        op.phase20 = 0;
    if (op.keyed && !hold)
    {
        op.phase = EgPhase::Attack;
        if (attackRate(ch, op) >= 62)
            op.att = 0;
    }
    if (hold && op.phase != EgPhase::Attack && attBit == op.ssgInvert)
        op.att = kEgMaxAttenuation;
    if (!op.keyed)
        op.att = kEgMaxAttenuation;
}

void Ym2612Core::egUpdate(Channel& ch, Operator& op) noexcept
{
    // Phase transitions (research "Phase transitions"). The decay check here covers SL = 0, a
    // lowered SL register and the first cycle after the attack; the step below checks again.
    if (op.phase == EgPhase::Attack && op.att == 0)
        op.phase = EgPhase::Decay;
    if (op.phase == EgPhase::Decay)
    {
        const int sl10 = sustainLevelToAttenuation(op.sl);
        if (op.att >= sl10)
        {
            op.att = static_cast<uint16_t>(sl10);
            op.phase = EgPhase::Sustain;
        }
    }

    // Update tables (research "Global counter and update tables").
    const int rate = currentRate(ch, op);
    const int shift = kEgShift[rate];
    if ((egCount & ((1 << shift) - 1)) != 0)
        return;
    const int inc = kEgIncrement[rate][(egCount >> shift) & 7];

    if (op.phase == EgPhase::Attack)
    {
        if (rate < 62)
            op.att = static_cast<uint16_t>(attackStep(op.att, inc));
        return;
    }
    if (op.ssg & 8)
    {
        // SSG-EG: x4 below 0x200, frozen at or above (Ambiguities 7).
        if (op.att < 0x200)
            op.att = static_cast<uint16_t>(std::min(op.att + 4 * inc, kEgMaxAttenuation));
    }
    else
    {
        op.att = static_cast<uint16_t>(std::min(op.att + inc, kEgMaxAttenuation));
    }

    // Decay -> sustain is checked immediately after the step, with the clamp to SL10 (research
    // "Phase transitions", Nemesis page 12): a decay step never leaves att above the sustain level.
    if (op.phase == EgPhase::Decay)
    {
        const int sl10 = sustainLevelToAttenuation(op.sl);
        if (op.att >= sl10)
        {
            op.att = static_cast<uint16_t>(sl10);
            op.phase = EgPhase::Sustain;
        }
    }
}

uint32_t Ym2612Core::phaseIncrement(const Channel& ch, const Operator& op) const noexcept
{
    // Research "Phase increment" and "Phase (frequency) modulation": the LFO modulates the fnum
    // with one extra fraction bit; key code and detune use the unmodulated fnum.
    const int fnum12 = modulatedFnum12(ch.fnum, ch.panAmsFms & 7, lfo);
    const int kc = keyCode(ch.block, ch.fnum);
    const uint32_t inc17 = phaseIncrement17(fnum12, ch.block, detuneDelta(kc, op.dt));
    return applyMultiplier(inc17, op.mul);
}

uint32_t Ym2612Core::operatorPhaseIncrement(int ch, int op) const noexcept
{
    return phaseIncrement(channels[ch], channels[ch].op[op]);
}

int Ym2612Core::computeChannel(Channel& ch) noexcept
{
    const AlgoDef& algo = kAlgorithm[ch.algorithm];
    for (auto& op : ch.op)
        op.prevOut = op.out;

    for (int k = 0; k < 4; ++k)
    {
        const int s = kEvalOrder[k];
        Operator& op = ch.op[s];
        int mod = 0;
        if (s == 0)
        {
            // S1 feedback: average of the last two outputs (research "Phase modulation input").
            if (ch.feedback != 0)
                mod = (ch.fb1 + ch.fb2) >> (10 - ch.feedback);
        }
        else
        {
            int sum = 0;
            for (int j = 0; j < 4; ++j)
                if ((algo.modMask[s] >> j) & 1)
                    sum += modulatorIsDelayed(s, j) ? ch.op[j].prevOut : ch.op[j].out;
            mod = sum >> 1;
        }
        op.out = operatorOutput(static_cast<int>(op.phase20 >> 10) + mod, egOutput(ch, op));
        if (s == 0)
        {
            ch.fb2 = ch.fb1;
            ch.fb1 = op.out;
        }
    }

    int sum = 0;
    for (int s = 0; s < 4; ++s)
        if ((algo.carrierMask >> s) & 1)
            sum += carrierTo9Bit(ch.op[s].out);

    for (auto& op : ch.op)
        op.phase20 = (op.phase20 + phaseIncrement(ch, op)) & 0xFFFFFu;

    return clampChannel9(sum);
}

void Ym2612Core::clockSample() noexcept
{
    // LFO: 7-bit counter advanced every kLfoSamplesPerStep FM samples.
    if (lfoOn && ++lfoDivider >= kLfoSamplesPerStep[lfoFreq])
    {
        lfoDivider = 0;
        lfo = (lfo + 1) & 0x7F;
    }

    // SSG-EG state logic: every sample, before the EG update.
    for (auto& ch : channels)
        for (auto& op : ch.op)
            if (op.ssg & 8)
                ssgUpdate(ch, op);

    // Envelope generator: once per 3 FM samples, 12-bit global counter skipping 0.
    if (++egSub >= 3)
    {
        egSub = 0;
        if (++egCount >= 4096)
            egCount = 1;
        for (auto& ch : channels)
            for (auto& op : ch.op)
                egUpdate(ch, op);
    }

    for (int c = 0; c < 6; ++c)
    {
        Channel& ch = channels[c];
        ch.out9 = computeChannel(ch);
        // Research "DAC": the DAC value replaces channel 6 at the output stage.
        if (c == 5 && dacOn)
            ch.out9 = dacSample9(dacData);
    }
}

int Ym2612Core::outputLeft() const noexcept
{
    int sum = 0;
    for (int c = 0; c < 6; ++c)
        sum += channelOutputLeft(c);
    return sum;
}

int Ym2612Core::outputRight() const noexcept
{
    int sum = 0;
    for (int c = 0; c < 6; ++c)
        sum += channelOutputRight(c);
    return sum;
}

bool Ym2612Core::channelIdle(int c) const noexcept
{
    const Channel& ch = channels[c];
    for (const auto& op : ch.op)
        if (op.keyed)
            return false;
    const uint8_t carriers = kAlgorithm[ch.algorithm].carrierMask;
    for (int s = 0; s < 4; ++s)
        if (((carriers >> s) & 1) && ch.op[s].att < kEgSilentAttenuation)
            return false;
    return true;
}

} // namespace chipdsp::genesis
