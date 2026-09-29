#include "chipdsp/snes/SnesDsp.h"

#include <algorithm>

namespace chipdsp::snes
{

void SnesDsp::reset() noexcept
{
    regs.fill(0);
    regs[kRegFlg] = 0xE0;
    aram.fill(0);
    for (auto& v : voices)
        v = Voice{};
    echoState = Echo{};
    konInternal = 0;
    counter = 0;
    noise = 0x4000;
    sampleIndex = 0;
}

void SnesDsp::writeRegister(int address, uint8_t value) noexcept
{
    // Addresses 0x80..0xFF are read-only mirrors: writes have no effect (research "Registers").
    if (address < 0 || address > 0x7F)
        return;
    regs[static_cast<size_t>(address)] = value;
    if (address == kRegKon)
        konInternal = value;            // latched until the next poll; a second write replaces it
    else if (address == kRegEndx)
        regs[kRegEndx] = 0;             // any write clears all ENDX bits
}

bool SnesDsp::isVoiceSounding(int v) const noexcept
{
    const Voice& vc = voices[static_cast<size_t>(v)];
    if ((konInternal & (1 << v)) != 0 || vc.konDelay > 0)
        return true;
    return !(vc.phase == EnvPhase::Release && vc.env == 0);
}

int32_t SnesDsp::gaussianInterpolate(int32_t h0, int32_t h1, int32_t h2, int32_t h3, int d) noexcept
{
    // Anomie's 15-bit form (research "Gaussian interpolation", Ambiguity 1): the first three
    // terms wrap at 15 bits, the fourth addition saturates.
    int32_t out = (kGaussTable[255 - d] * h0) >> 11;
    out += (kGaussTable[511 - d] * h1) >> 11;
    out += (kGaussTable[256 + d] * h2) >> 11;
    out = clip15(out);
    out += (kGaussTable[d] * h3) >> 11;
    return clamp15(out);
}

int32_t SnesDsp::firFilter(const int16_t history[8], const int8_t taps[8]) noexcept
{
    // Taps 0..6 accumulate "without overflow handling" (16-bit wrap), tap 7 (newest) is
    // saturated, then bit 0 is cleared (research "8-tap FIR").
    int32_t sum = 0;
    for (int k = 0; k < 7; ++k)
        sum += (history[k] * taps[k]) >> 6;
    sum = clip16(sum);
    sum = clamp16(sum + ((history[7] * taps[7]) >> 6));
    return sum & ~1;
}

uint16_t SnesDsp::readDirectory(int v, bool loopEntry) const noexcept
{
    // Directory entry: DIR * 0x100 + SRCN * 4; bytes 0-1 start, 2-3 loop (little-endian).
    const int entry = regs[kRegDir] * 0x100 + voiceReg(v, kRegSrcn) * 4 + (loopEntry ? 2 : 0);
    const uint16_t lo = static_cast<uint16_t>(entry & 0xFFFF);
    const uint16_t hi = static_cast<uint16_t>((entry + 1) & 0xFFFF);
    return static_cast<uint16_t>(aram[lo] | (aram[hi] << 8));
}

void SnesDsp::checkBlockEnd(int v) noexcept
{
    // Anomie S3b/S3c: the header of the current block is loaded every sample and a block
    // with E = 1, L = 0 puts the voice in Release with envelope 0 (research "End/loop codes",
    // Ambiguity 7). It runs before the KON handling, so a key-on always wins.
    Voice& vc = voices[static_cast<size_t>(v)];
    const uint8_t header = aram[vc.brrAddr];
    if ((header & kBrrFlagEnd) != 0 && (header & kBrrFlagLoop) == 0)
    {
        vc.phase = EnvPhase::Release;
        vc.env = 0;
    }
}

void SnesDsp::decodeGroup(int v, int slot) noexcept
{
    // Four samples (one byte pair) per decode, into ring group 'slot' (research "Decode granularity").
    Voice& vc = voices[static_cast<size_t>(v)];
    const uint8_t header = aram[vc.brrAddr];
    const int shift = header >> 4;
    const int filter = (header >> 2) & 3;
    for (int b = 0; b < 2; ++b)
    {
        const uint8_t byte = aram[static_cast<uint16_t>(vc.brrAddr + vc.brrOffset + b)];
        for (int nib = 0; nib < 2; ++nib)
        {
            const int raw = nib == 0 ? (byte >> 4) : (byte & 0x0F);
            const int nibble = raw >= 8 ? raw - 16 : raw;
            const int32_t s = brrDecodeSample(nibble, shift, filter, vc.history.old, vc.history.older);
            vc.history.older = vc.history.old;
            vc.history.old = s;
            vc.ring[slot * 4 + b * 2 + nib] = static_cast<int16_t>(s);
        }
    }
    vc.brrOffset += 2;
    if (vc.brrOffset >= kBrrBlockBytes)
        finishGroup(v);
}

void SnesDsp::finishGroup(int v) noexcept
{
    // End of a block: a block with the end flag continues at the directory loop address
    // (decoding never stops, only KON restarts it), otherwise at the next 9 bytes.
    Voice& vc = voices[static_cast<size_t>(v)];
    const uint8_t header = aram[vc.brrAddr];
    if ((header & kBrrFlagEnd) != 0)
        vc.brrAddr = readDirectory(v, true);
    else
        vc.brrAddr = static_cast<uint16_t>(vc.brrAddr + kBrrBlockBytes);
    vc.brrOffset = 1;
    // Ambiguity 8 (recommended "start"): ENDX is set when the header of a block with the end
    // flag becomes the current block. The envelope effect of code 1 is applied by
    // checkBlockEnd() at the next sample's S3c, when the chip reads this header.
    if ((aram[vc.brrAddr] & kBrrFlagEnd) != 0)
        regs[kRegEndx] = static_cast<uint8_t>(regs[kRegEndx] | (1 << v));
}

void SnesDsp::updateEnvelope(int v) noexcept
{
    // research "Envelope steps" and "Exact per-sample update order" (steps 5..7): the new
    // value is computed every sample, stored only on a counter event for the current rate,
    // and the phase transitions are evaluated every sample. Called after the multiply, so
    // the stored value is heard from the next sample on (Ambiguity 24).
    Voice& vc = voices[static_cast<size_t>(v)];
    const int32_t e = vc.env;
    const int32_t expStep = ((e - 1) >> 8) + 1;

    if (vc.phase == EnvPhase::Release)
    {
        const int32_t next = e - 8;           // every sample, overrides ADSR and GAIN
        vc.env = std::max(next, 0);
        vc.hiddenEnv = next & 0x7FF;
        return;
    }

    const int adsr1 = voiceReg(v, kRegAdsr1);
    int32_t next = e;
    int rate = 0;
    bool always = false;                      // direct gain: set every sample
    int sustainLevel = 0;

    if ((adsr1 & 0x80) != 0)
    {
        const int adsr2 = voiceReg(v, kRegAdsr2);
        sustainLevel = adsr2 >> 5;
        switch (vc.phase)
        {
            case EnvPhase::Attack:
                rate = (adsr1 & 0x0F) * 2 + 1;
                next = e + (rate == 31 ? 1024 : 32);
                break;
            case EnvPhase::Decay:
                rate = ((adsr1 >> 4) & 7) * 2 + 16;
                next = e - expStep;
                break;
            default: // Sustain
                rate = adsr2 & 0x1F;
                next = e - expStep;
                break;
        }
    }
    else
    {
        const int gain = voiceReg(v, kRegGain);
        sustainLevel = gain >> 5;             // GAIN bits 7-5 stand in for SL in GAIN mode
        if ((gain & 0x80) == 0)
        {
            next = (gain & 0x7F) << 4;        // direct: E = GAIN << 4
            always = true;
        }
        else
        {
            rate = gain & 0x1F;
            switch ((gain >> 5) & 3)
            {
                case 0: next = e - 32; break;                                  // linear decrease
                case 1: next = e - expStep; break;                             // exponential decrease
                case 2: next = e + 32; break;                                  // linear increase
                default: next = e + (vc.hiddenEnv < 0x600 ? 32 : 8); break;    // bent-line increase
            }
        }
    }

    if (always || rateFires(rate, counter))
        vc.env = std::clamp(next, 0, 0x7FF);

    if (vc.phase == EnvPhase::Decay && (vc.env >> 8) == sustainLevel)
        vc.phase = EnvPhase::Sustain;
    else if (vc.phase == EnvPhase::Attack && (next > 0x7FF || next < 0))
        vc.phase = EnvPhase::Decay;
    vc.hiddenEnv = next & 0x7FF;
}

void SnesDsp::step(SnesDspOutput& out) noexcept
{
    // KON/KOFF are polled every second sample; FLG.7 every sample (research "Key-on, key-off").
    const bool poll = (sampleIndex & 1) == 0;
    uint8_t kon = 0;
    uint8_t koff = 0;
    if (poll)
    {
        kon = konInternal;
        koff = regs[kRegKoff];
        konInternal = 0;
    }
    const uint8_t flg = regs[kRegFlg];
    const uint8_t pmon = regs[kRegPmon];
    const uint8_t non = regs[kRegNon];
    const uint8_t eon = regs[kRegEon];
    const int32_t noiseSample = clip15(noise);

    int32_t mixL = 0, mixR = 0, echoMixL = 0, echoMixR = 0;
    int32_t dryL[kNumVoices], dryR[kNumVoices];

    for (int v = 0; v < kNumVoices; ++v)
    {
        Voice& vc = voices[static_cast<size_t>(v)];
        const int bit = 1 << v;
        const bool starting = (kon & bit) != 0;

        // ----- S3c (research "Exact per-sample update order", Ambiguity 24) ------------------
        // 1. The sample, scaled by the envelope stored on the previous sample. Samples #1..#5
        //    of a key-on output 0; sample #0 still outputs the old voice (Ambiguity 21).
        int32_t env15 = 0;
        if (vc.konDelay == 0)
        {
            int32_t s15;
            if ((non & bit) != 0)
            {
                s15 = noiseSample;              // noise replaces the interpolated sample
            }
            else
            {
                const int base = vc.ringHead + (vc.index >> 12);
                const int d = (vc.index >> 4) & 0xFF;
                s15 = gaussianInterpolate(vc.ring[base % 12], vc.ring[(base + 1) % 12],
                                          vc.ring[(base + 2) % 12], vc.ring[(base + 3) % 12], d);
            }
            env15 = (s15 * vc.env) >> 11;
        }
        vc.out16 = env15 * 2;                   // PMON source for voice v + 1
        regs[static_cast<size_t>(v * 16 + kRegOutx)] = static_cast<uint8_t>((env15 >> 7) & 0xFF);

        // 2. FLG.7, 3. BRR end check (not on sample #1, Anomie), 4. KOFF and KON (KON last,
        //    so it overrides both), 5. envelope update (from sample #5 on, not on #0).
        if ((flg & kFlgReset) != 0 || (koff & bit) != 0)
        {
            vc.phase = EnvPhase::Release;
            if ((flg & kFlgReset) != 0)
                vc.env = 0;
        }
        if (vc.konDelay != 5)
            checkBlockEnd(v);
        if (starting)
        {
            vc.env = 0;
            vc.hiddenEnv = 0;
            vc.phase = EnvPhase::Attack;
            regs[kRegEndx] = static_cast<uint8_t>(regs[kRegEndx] & ~bit);
        }
        else if (vc.konDelay <= 1)
        {
            updateEnvelope(v);
        }

        // ----- S4: pitch step, BRR decode ----------------------------------------------------
        // Pitch step with pitch modulation by the previous voice (research "Pitch counter").
        auto advance = [&]() noexcept {
            int32_t pitch = (voiceReg(v, kRegPitchL) | (voiceReg(v, kRegPitchH) << 8)) & 0x3FFF;
            if (v > 0 && (pmon & bit) != 0 && (non & bit) == 0)       // Ambiguity 4
                pitch = modulatedPitch(pitch, voices[static_cast<size_t>(v - 1)].out16);
            vc.index = std::min(vc.index + pitch, 0x7FFF);             // Ambiguity 3
            if (vc.index >= 0x4000)
            {
                decodeGroup(v, vc.ringHead / 4);
                vc.ringHead = (vc.ringHead + 4) % 12;
                vc.index -= 0x4000;
            }
        };

        if (starting)
        {
            // Sample #0 of the key-on sequence: the last pre-KON decode still happens, then
            // the interpolation index restarts (research "Key-on sequence"). An end block
            // reached by that decode does not set ENDX: the KON clear wins (Anomie S4).
            if (vc.konDelay == 0)
                advance();
            regs[kRegEndx] = static_cast<uint8_t>(regs[kRegEndx] & ~bit);
            vc.index = 0;
            vc.konDelay = 5;
        }
        else if (vc.konDelay > 0)
        {
            // Samples #1..#5 output 0: #1 reads the directory (the first header never sets
            // ENDX), #2..#4 decode three groups, #5 starts the envelope (done above).
            switch (vc.konDelay)
            {
                case 5:
                    vc.brrAddr = readDirectory(v, false);
                    vc.brrOffset = 1;
                    vc.ringHead = 0;
                    break;
                case 4:
                case 3:
                case 2:
                    decodeGroup(v, 4 - vc.konDelay);
                    break;
                default:
                    break;
            }
            --vc.konDelay;
        }
        else
        {
            advance();
        }

        regs[static_cast<size_t>(v * 16 + kRegEnvx)] = static_cast<uint8_t>(vc.env >> 4);

        dryL[v] = voiceVolume(env15, static_cast<int8_t>(voiceReg(v, kRegVolL)));
        dryR[v] = voiceVolume(env15, static_cast<int8_t>(voiceReg(v, kRegVolR)));
        // Mixed values are clamped to 16 bits after each addition (Anomie).
        mixL = clamp16(mixL + dryL[v]);
        mixR = clamp16(mixR + dryR[v]);
        if ((eon & bit) != 0)
        {
            echoMixL = clamp16(echoMixL + dryL[v]);
            echoMixR = clamp16(echoMixR + dryR[v]);
        }
    }

    // ----- echo (research "Per-sample echo processing") ---------------------------------------
    Echo& ec = echoState;
    if (ec.index == 0)
        ec.length = echoLengthForEdl(regs[kRegEdl]);   // EDL is applied only when the index wraps
    const uint16_t addr = static_cast<uint16_t>(regs[kRegEsa] * 0x100 + ec.index * 4);
    auto readWord = [&](int offset) noexcept {
        const uint16_t a = static_cast<uint16_t>(addr + offset);
        const uint16_t b = static_cast<uint16_t>(a + 1);
        return static_cast<int16_t>(aram[a] | (aram[b] << 8));
    };
    std::copy(ec.historyL + 1, ec.historyL + 8, ec.historyL);
    std::copy(ec.historyR + 1, ec.historyR + 8, ec.historyR);
    ec.historyL[7] = static_cast<int16_t>(readWord(0) >> 1);
    ec.historyR[7] = static_cast<int16_t>(readWord(2) >> 1);

    int8_t taps[8];
    for (int k = 0; k < 8; ++k)
        taps[k] = static_cast<int8_t>(regs[static_cast<size_t>(k * 16 + kRegFir)]);
    const int32_t firL = firFilter(ec.historyL, taps);
    const int32_t firR = firFilter(ec.historyR, taps);

    const auto mvolL = static_cast<int8_t>(regs[kRegMvolL]);
    const auto mvolR = static_cast<int8_t>(regs[kRegMvolR]);
    const int32_t outL = clamp16(volumeProduct(mixL, mvolL) + volumeProduct(firL, static_cast<int8_t>(regs[kRegEvolL])));
    const int32_t outR = clamp16(volumeProduct(mixR, mvolR) + volumeProduct(firR, static_cast<int8_t>(regs[kRegEvolR])));

    const auto efb = static_cast<int8_t>(regs[kRegEfb]);
    const int32_t echoInL = clamp16(echoMixL + volumeProduct(firL, efb)) & ~1;
    const int32_t echoInR = clamp16(echoMixR + volumeProduct(firR, efb)) & ~1;
    if ((flg & kFlgEchoWriteDisable) == 0)
    {
        auto writeWord = [&](int offset, int32_t value) noexcept {
            const uint16_t a = static_cast<uint16_t>(addr + offset);
            aram[a] = static_cast<uint8_t>(value & 0xFF);
            aram[static_cast<uint16_t>(a + 1)] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        writeWord(0, echoInL);
        writeWord(2, echoInR);
    }
    if (++ec.index >= ec.length)
        ec.index = 0;

    // ----- output stage: mute (FLG.6) then the post-amp inversion (Ambiguity 9) --------------
    const bool mute = (flg & kFlgMute) != 0;
    auto finish = [mute](int32_t v) noexcept { return static_cast<int16_t>(~(mute ? 0 : v)); };
    out.mainL = finish(outL);
    out.mainR = finish(outR);
    for (int v = 0; v < kNumVoices; ++v)
    {
        out.voiceL[v] = finish(volumeProduct(dryL[v], mvolL));
        out.voiceR[v] = finish(volumeProduct(dryR[v], mvolR));
    }

    // ----- global counter, then noise (cycles 29 and 30) -------------------------------------
    counter = counter == 0 ? kCounterWrap : counter - 1;
    if (rateFires(flg & kFlgNoiseMask, counter))
        noise = noiseStep(noise);
    ++sampleIndex;
}

} // namespace chipdsp::snes
