#include "chipdsp/genesis/GenesisEngine.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace chipdsp
{

using namespace genesis;

namespace
{
    const char* const kClockLabels[] = { "NTSC", "PAL" };
    const char* const kRevisionLabels[] = { "YM2612 (discrete, ladder effect)", "YM3438 / ASIC" };
    const char* const kOffOnLabels[] = { "Off", "On" };
    // Frequencies of the hardware dividers, NTSC and PAL (research "LFO", ref 30). The
    // descriptors are static, so each label names both clocks.
    const char* const kLfoLabels[] = { "3.853 Hz (PAL 3.818)", "5.405 Hz (PAL 5.355)", "5.861 Hz (PAL 5.808)",
                                       "6.211 Hz (PAL 6.155)", "6.712 Hz (PAL 6.651)", "9.458 Hz (PAL 9.372)",
                                       "52.02 Hz (PAL 51.54)", "83.23 Hz (PAL 82.47)" };
    const char* const kAlgorithmLabels[] = {
        "0: S1>S2>S3>S4", "1: (S1+S2)>S3>S4", "2: (S1+(S2>S3))>S4", "3: ((S1>S2)+S3)>S4",
        "4: S1>S2 + S3>S4", "5: S1>(S2+S3+S4)", "6: S1>S2 + S3 + S4", "7: S1+S2+S3+S4" };
    const char* const kAmsLabels[] = { "0 dB", "1.4 dB", "5.9 dB", "11.8 dB" };
    const char* const kFmsLabels[] = { "0 cents", "3.4 cents", "6.7 cents", "10 cents", "14 cents", "20 cents", "40 cents", "80 cents" };
    // "Off" (L = R = 0, the channel is not output) was added after the first release at the
    // end of the list, so the stored values 0..2 keep their meaning.
    const char* const kPanLabels[] = { "Left", "Center", "Right", "Off" };
    const char* const kMulLabels[] = { "x0.5", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
                                       "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15" };
    const char* const kDtLabels[] = { "0", "+1", "+2", "+3", "-0", "-1", "-2", "-3" };
    const char* const kRsLabels[] = { "KC/8", "KC/4", "KC/2", "KC/1" };
    const char* const kSsgLabels[] = { "Off", "8: saw down", "9: down, hold low", "A: triangle down", "B: down, hold high",
                                       "C: saw up", "D: up, hold high", "E: triangle up", "F: up, hold low" };
    const char* const kNoiseModeLabels[] = { "Periodic", "White" };
    const char* const kNoiseRateLabels[] = { "Clock/512", "Clock/1024", "Clock/2048", "Tone 3" };

    using E = GenesisEngine;

#define RCV_GEN_OP(n, tl, ar, dr, sr, rr, sl, mul)                                                                          \
    { E::opParam(n - 1, E::OpTl), "op" #n "_tl", "Op " #n " Total Level", "Operator " #n, 0, 127, tl, true, "", nullptr },   \
    { E::opParam(n - 1, E::OpAr), "op" #n "_ar", "Op " #n " Attack Rate", "Operator " #n, 0, 31, ar, true, "", nullptr },    \
    { E::opParam(n - 1, E::OpDr), "op" #n "_dr", "Op " #n " Decay Rate", "Operator " #n, 0, 31, dr, true, "", nullptr },     \
    { E::opParam(n - 1, E::OpSr), "op" #n "_sr", "Op " #n " Sustain Rate", "Operator " #n, 0, 31, sr, true, "", nullptr },   \
    { E::opParam(n - 1, E::OpRr), "op" #n "_rr", "Op " #n " Release Rate", "Operator " #n, 0, 15, rr, true, "", nullptr },   \
    { E::opParam(n - 1, E::OpSl), "op" #n "_sl", "Op " #n " Sustain Level", "Operator " #n, 0, 15, sl, true, "", nullptr },  \
    { E::opParam(n - 1, E::OpMul), "op" #n "_mul", "Op " #n " Multiplier", "Operator " #n, 0, 15, mul, true, "", kMulLabels }, \
    { E::opParam(n - 1, E::OpDt), "op" #n "_dt", "Op " #n " Detune", "Operator " #n, 0, 7, 0, true, "", kDtLabels },         \
    { E::opParam(n - 1, E::OpRs), "op" #n "_rs", "Op " #n " Rate Scaling", "Operator " #n, 0, 3, 0, true, "", kRsLabels },   \
    { E::opParam(n - 1, E::OpAm), "op" #n "_am", "Op " #n " AM Enable", "Operator " #n, 0, 1, 0, true, "", kOffOnLabels },   \
    { E::opParam(n - 1, E::OpSsg), "op" #n "_ssg", "Op " #n " SSG-EG", "Operator " #n, 0, 8, 0, true, "", kSsgLabels }

    // Ordered by id (checked by a test). The default patch is algorithm 4 (two 2-op stacks).
    constexpr ParamDesc kDescs[] = {
        { E::Clock, "clock", "Clock", "Global", 0, 1, 0, true, "", kClockLabels },
        { E::ChipRevision, "chip_revision", "Chip Revision", "Global", 0, 1, 0, true, "", kRevisionLabels },
        { E::Model1Lowpass, "model1_lowpass", "Model 1 Low-pass", "Global", 0, 1, 1, true, "", kOffOnLabels },
        { E::LfoEnable, "lfo_enable", "LFO Enable", "Global", 0, 1, 0, true, "", kOffOnLabels },
        { E::LfoFreq, "lfo_freq", "LFO Frequency", "Global", 0, 7, 0, true, "", kLfoLabels },

        { E::Algorithm, "algorithm", "Algorithm", "FM Patch", 0, 7, 4, true, "", kAlgorithmLabels },
        { E::Feedback, "feedback", "Feedback", "FM Patch", 0, 7, 5, true, "", nullptr },
        { E::Ams, "ams", "AM Sensitivity", "FM Patch", 0, 3, 0, true, "", kAmsLabels },
        { E::Fms, "fms", "FM Sensitivity", "FM Patch", 0, 7, 0, true, "", kFmsLabels },
        { E::Transpose, "transpose", "Transpose", "FM Patch", -24, 24, 0, true, "st", nullptr },
        { E::FineTune, "fine_tune", "Fine Tune", "FM Patch", -100, 100, 0, true, "cents", nullptr },
        { E::VibratoRate, "vibrato_rate", "Vibrato Rate", "FM Patch", 0, 15, 0, true, "frames", nullptr },
        { E::VibratoDepth, "vibrato_depth", "Vibrato Depth", "FM Patch", 0, 64, 0, true, "fnum", nullptr },
        { E::VibratoDelay, "vibrato_delay", "Vibrato Delay", "FM Patch", 0, 60, 0, true, "frames", nullptr },
        { E::UnisonDetune, "unison_detune", "Unison Detune", "FM Patch", 0, 15, 0, true, "fnum", nullptr },
        { E::Fm1Pan, "fm1_pan", "FM 1 Pan", "FM Patch", 0, 3, 1, true, "", kPanLabels },
        { E::Fm2Pan, "fm2_pan", "FM 2 Pan", "FM Patch", 0, 3, 1, true, "", kPanLabels },
        { E::Fm3Pan, "fm3_pan", "FM 3 Pan", "FM Patch", 0, 3, 1, true, "", kPanLabels },
        { E::Fm4Pan, "fm4_pan", "FM 4 Pan", "FM Patch", 0, 3, 1, true, "", kPanLabels },
        { E::Fm5Pan, "fm5_pan", "FM 5 Pan", "FM Patch", 0, 3, 1, true, "", kPanLabels },
        { E::Fm6Pan, "fm6_pan", "FM 6 Pan", "FM Patch", 0, 3, 1, true, "", kPanLabels },
        { E::VelocityDepth, "velocity_depth", "Velocity Depth", "FM Patch", 0, 127, 16, true, "TL", nullptr },

        RCV_GEN_OP(1, 30, 31, 8, 2, 7, 2, 1),
        RCV_GEN_OP(2, 0, 31, 5, 2, 7, 2, 1),
        RCV_GEN_OP(3, 36, 31, 10, 2, 7, 4, 2),
        RCV_GEN_OP(4, 6, 31, 5, 2, 7, 2, 1),

        { E::DacEnable, "dac_enable", "DAC Enable", "DAC", 0, 1, 0, true, "", kOffOnLabels },
        { E::DacSample, "dac_sample", "DAC Sample", "DAC", 0, 15, 0, true, "", nullptr },
        { E::DacRate, "dac_rate", "DAC Rate", "DAC", 4000, 32000, 16000, true, "Hz", nullptr },
        { E::DacKeyed, "dac_keyed", "DAC Keyed", "DAC", 0, 1, 0, true, "", kOffOnLabels },
        { E::DacLoop, "dac_loop", "DAC Loop", "DAC", 0, 1, 0, true, "", kOffOnLabels },
        { E::DacVolume, "dac_volume", "DAC Volume", "DAC", 0, 127, 127, true, "", nullptr },

        { E::Psg1Att, "psg1_att", "PSG 1 Attenuation", "PSG", 0, 15, 0, true, "x2 dB", nullptr },
        { E::Psg2Att, "psg2_att", "PSG 2 Attenuation", "PSG", 0, 15, 0, true, "x2 dB", nullptr },
        { E::Psg3Att, "psg3_att", "PSG 3 Attenuation", "PSG", 0, 15, 0, true, "x2 dB", nullptr },
        { E::PsgnAtt, "psgn_att", "PSG Noise Attenuation", "PSG", 0, 15, 0, true, "x2 dB", nullptr },
        { E::PsgNoiseMode, "psg_noise_mode", "Noise Mode", "PSG", 0, 1, 1, true, "", kNoiseModeLabels },
        { E::PsgNoiseRate, "psg_noise_rate", "Noise Rate", "PSG", 0, 3, 0, true, "", kNoiseRateLabels },
        { E::PsgSwAttack, "psg_sw_attack", "PSG Attack", "PSG", 0, 60, 0, true, "frames", nullptr },
        { E::PsgSwDecay, "psg_sw_decay", "PSG Decay", "PSG", 0, 60, 0, true, "frames", nullptr },
        { E::PsgSwSustain, "psg_sw_sustain", "PSG Sustain", "PSG", 0, 15, 15, true, "", nullptr },
        { E::PsgSwRelease, "psg_sw_release", "PSG Release", "PSG", 0, 60, 0, true, "frames", nullptr },
        { E::PsgVibratoRate, "psg_vibrato_rate", "PSG Vibrato Rate", "PSG", 0, 15, 0, true, "frames", nullptr },
        { E::PsgVibratoDepth, "psg_vibrato_depth", "PSG Vibrato Depth", "PSG", 0, 15, 0, true, "period", nullptr },
        { E::PsgUnisonDetune, "psg_unison_detune", "PSG Unison Detune", "PSG", 0, 7, 0, true, "period", nullptr },
        { E::PsgTranspose, "psg_transpose", "PSG Transpose", "PSG", -24, 24, 0, true, "st", nullptr },
    };
#undef RCV_GEN_OP

    static_assert(std::size(kDescs) == static_cast<size_t>(GenesisEngine::NumParams), "one descriptor per parameter");

    const ChannelInfo kChannelInfo[kGenesisChannels] = {
        { "FM 1", "FM1", true }, { "FM 2", "FM2", true }, { "FM 3", "FM3", true },
        { "FM 4", "FM4", true }, { "FM 5", "FM5", true }, { "FM 6", "FM6", true },
        { "PSG 1", "PSG1", true }, { "PSG 2", "PSG2", true }, { "PSG 3", "PSG3", true },
        { "PSG Noise", "PSGN", false },
    };
} // namespace

// ----- construction / lifecycle -----------------------------------------------------------------

void GenesisEngine::OutputFilters::prepare(double sampleRate) noexcept
{
    dc.prepare(kDcBlockHz, sampleRate);
    const RcLowPassCoefficients c = rcLowPass(kModel1LowPassHz, sampleRate);
    b0 = static_cast<float>(c.b0);
    b1 = static_cast<float>(c.b1);
    b2 = static_cast<float>(c.b2);
    a1 = static_cast<float>(c.a1);
    reset();
}

GenesisEngine::GenesisEngine()
{
    for (const auto& d : kDescs)
        params[static_cast<size_t>(d.id)].store(d.defaultValue, std::memory_order_relaxed);

    // Two pre-allocated banks of 16 x 64 KiB (research "DAC", ARCHITECTURE "Sample slots").
    for (auto& bank : banks)
        for (int s = 0; s < kDacSlots; ++s)
        {
            bank.data[s].assign(kDacMaxBytes, 0x80);
            bank.view[s] = { bank.data[s].data(), 0 };
        }

    drv.attach(&ym, &psg);
    drv.reset(readSettings());
}

ChannelInfo GenesisEngine::channelInfo(int channel) const noexcept
{
    return kChannelInfo[std::clamp(channel, 0, kGenesisChannels - 1)];
}

double GenesisEngine::nativeSampleRate() const noexcept
{
    return fmSampleRate(paramInt(Clock) != 0 ? ClockStandard::Pal : ClockStandard::Ntsc);
}

std::span<const ParamDesc> GenesisEngine::parameterDescriptors() const noexcept
{
    return { kDescs, std::size(kDescs) };
}

void GenesisEngine::prepare(double hostSampleRate, int maxBlockSize)
{
    hostRate = hostSampleRate;
    maxBlock = std::max(1, maxBlockSize);
    clockStd = paramInt(Clock) != 0 ? ClockStandard::Pal : ClockStandard::Ntsc;

    const double fmRate = fmSampleRate(clockStd);
    const double psgRate = psgTickRate(clockStd);
    // Flat band-limited steps (refcheck finding F2; every engine uses this kernel).
    constexpr auto kKernel = BandLimitedStepSynth::Kernel::IntegratedStep;
    fmSynthL.prepare(fmRate, hostRate, maxBlock, kKernel);
    fmSynthR.prepare(fmRate, hostRate, maxBlock, kKernel);
    psgSynth.prepare(psgRate, hostRate, maxBlock, kKernel);
    for (int c = 0; c < 6; ++c)
    {
        fmChSynthL[c].prepare(fmRate, hostRate, maxBlock, kKernel);
        fmChSynthR[c].prepare(fmRate, hostRate, maxBlock, kKernel);
    }
    for (auto& s : psgChSynth)
        s.prepare(psgRate, hostRate, maxBlock, kKernel);

    mainFilterL.prepare(hostRate);
    mainFilterR.prepare(hostRate);
    for (int c = 0; c < kGenesisChannels; ++c)
    {
        chFilterL[c].prepare(hostRate);
        chFilterR[c].prepare(hostRate);
    }
    scratch.assign(static_cast<size_t>(maxBlock), 0.0f);

    setRawOutput(raw);
    reset();
}

void GenesisEngine::reset() noexcept
{
    applyClock();
    ym.setLadderEffect(paramInt(ChipRevision) == 0);
    lowPassOn = paramInt(Model1Lowpass) != 0;
    acquireDacBank();
    drv.reset(readSettings());
    releaseDacBank();

    fmSynthL.reset();
    fmSynthR.reset();
    psgSynth.reset();
    for (int c = 0; c < 6; ++c)
    {
        fmChSynthL[c].reset();
        fmChSynthR[c].reset();
        fmChLevelL[c] = 0;
        fmChLevelR[c] = 0;
    }
    for (int i = 0; i < 4; ++i)
    {
        psgChSynth[i].reset();
        psgChLevel[i] = 0;
    }
    mainFilterL.reset();
    mainFilterR.reset();
    for (int c = 0; c < kGenesisChannels; ++c)
    {
        chFilterL[c].reset();
        chFilterR[c].reset();
    }
    perChannelLive = false;
    blockStart = nextFm = nextPsg = nextFrame = nextDac = 0.0;

    // Let the silent chip reach its idle output (operator/EG pipelines, ladder offsets), then
    // start the level trackers there. 16 FM samples = 0.3 ms of chip time, never output.
    constexpr int kSettleSamples = 16;
    for (int i = 0; i < kSettleSamples; ++i)
    {
        ym.clockSample();
        psg.clock();
    }
    syncLevelTrackers(true);
}

void GenesisEngine::syncLevelTrackers(bool mainToo) noexcept
{
    if (mainToo)
    {
        fmLevelL = ym.outputLeft();
        fmLevelR = ym.outputRight();
        psgLevel = psg.mix();
    }
    for (int c = 0; c < 6; ++c)
    {
        fmChLevelL[c] = ym.channelOutputLeft(c);
        fmChLevelR[c] = ym.channelOutputRight(c);
    }
    for (int i = 0; i < 4; ++i)
        psgChLevel[i] = psg.channelLevel(i);
}

// ----- parameters --------------------------------------------------------------------------------

void GenesisEngine::setParameter(int id, float value) noexcept
{
    if (id < 0 || id >= NumParams)
        return;
    const ParamDesc& d = kDescs[id];
    float v = std::clamp(value, d.minValue, d.maxValue);
    if (d.isInteger)
        v = std::round(v);
    params[static_cast<size_t>(id)].store(v, std::memory_order_relaxed);
}

float GenesisEngine::getParameter(int id) const noexcept
{
    return (id >= 0 && id < NumParams) ? params[static_cast<size_t>(id)].load(std::memory_order_relaxed) : 0.0f;
}

int GenesisEngine::paramInt(int id) const noexcept
{
    return static_cast<int>(std::lround(params[static_cast<size_t>(id)].load(std::memory_order_relaxed)));
}

DriverSettings GenesisEngine::readSettings() const noexcept
{
    DriverSettings s;
    s.clock = paramInt(Clock) != 0 ? ClockStandard::Pal : ClockStandard::Ntsc;
    s.lfoEnable = paramInt(LfoEnable);
    s.lfoFreq = paramInt(LfoFreq);
    s.algorithm = paramInt(Algorithm);
    s.feedback = paramInt(Feedback);
    s.ams = paramInt(Ams);
    s.fms = paramInt(Fms);
    s.transpose = paramInt(Transpose);
    s.fineTune = paramInt(FineTune);
    s.vibratoRate = paramInt(VibratoRate);
    s.vibratoDepth = paramInt(VibratoDepth);
    s.vibratoDelay = paramInt(VibratoDelay);
    s.unisonDetune = paramInt(UnisonDetune);
    for (int c = 0; c < 6; ++c)
        s.pan[c] = paramInt(Fm1Pan + c);
    s.velocityDepth = paramInt(VelocityDepth);
    for (int op = 0; op < 4; ++op)
    {
        OperatorPatch& p = s.op[op];
        p.tl = paramInt(opParam(op, OpTl));
        p.ar = paramInt(opParam(op, OpAr));
        p.dr = paramInt(opParam(op, OpDr));
        p.sr = paramInt(opParam(op, OpSr));
        p.rr = paramInt(opParam(op, OpRr));
        p.sl = paramInt(opParam(op, OpSl));
        p.mul = paramInt(opParam(op, OpMul));
        p.dt = paramInt(opParam(op, OpDt));
        p.rs = paramInt(opParam(op, OpRs));
        p.am = paramInt(opParam(op, OpAm));
        p.ssg = paramInt(opParam(op, OpSsg));
    }
    s.dacEnable = paramInt(DacEnable);
    s.dacSample = paramInt(DacSample);
    s.dacRate = paramInt(DacRate);
    s.dacKeyed = paramInt(DacKeyed);
    s.dacLoop = paramInt(DacLoop);
    s.dacVolume = paramInt(DacVolume);
    for (int i = 0; i < 4; ++i)
        s.psgAtt[i] = paramInt(Psg1Att + i);
    s.noiseMode = paramInt(PsgNoiseMode);
    s.noiseRate = paramInt(PsgNoiseRate);
    s.psgAttack = paramInt(PsgSwAttack);
    s.psgDecay = paramInt(PsgSwDecay);
    s.psgSustain = paramInt(PsgSwSustain);
    s.psgRelease = paramInt(PsgSwRelease);
    s.psgVibratoRate = paramInt(PsgVibratoRate);
    s.psgVibratoDepth = paramInt(PsgVibratoDepth);
    s.psgUnisonDetune = paramInt(PsgUnisonDetune);
    s.psgTranspose = paramInt(PsgTranspose);
    return s;
}

void GenesisEngine::refreshDriver() noexcept
{
    drv.setSettings(readSettings());
    acquireDacBank();
}

void GenesisEngine::acquireDacBank() noexcept
{
    // Publish the bank before relying on it, then re-check the index: if the loader flipped it
    // in between, take the new bank. The loader only writes the bank that is not active, and it
    // reads bankInUse after its previous flip and before writing: with sequentially consistent
    // operations it either sees this store (and waits) or this re-check sees the flip.
    int b = activeBank.load(std::memory_order_seq_cst);
    for (;;)
    {
        bankInUse.store(b, std::memory_order_seq_cst);
        const int again = activeBank.load(std::memory_order_seq_cst);
        if (again == b)
            break;
        b = again;
    }
    drv.setDacBank(banks[b].view);
}

void GenesisEngine::applyClock() noexcept
{
    clockStd = paramInt(Clock) != 0 ? ClockStandard::Pal : ClockStandard::Ntsc;
    masterPerHost = masterClock(clockStd) / hostRate;
    hostPerMaster = hostRate / masterClock(clockStd);
}

void GenesisEngine::setClockStandard(ClockStandard standard) noexcept
{
    setParameter(Clock, standard == ClockStandard::Pal ? 1.0f : 0.0f);
}

void GenesisEngine::setRawOutput(bool r) noexcept
{
    raw = r;
    fmSynthL.setRaw(r);
    fmSynthR.setRaw(r);
    psgSynth.setRaw(r);
    for (int c = 0; c < 6; ++c)
    {
        fmChSynthL[c].setRaw(r);
        fmChSynthR[c].setRaw(r);
    }
    for (auto& s : psgChSynth)
        s.setRaw(r);
}

// ----- samples (message thread) ------------------------------------------------------------------

bool GenesisEngine::loadSample(int slot, const float* mono, int numFrames, double sourceSampleRate)
{
    if (slot < 0 || slot >= kDacSlots || numFrames < 0 || (numFrames > 0 && mono == nullptr) || sourceSampleRate <= 0.0)
        return false;

    // Encoding: resample to the current dac_rate (the rate the driver streams $2A at) and
    // quantise to 8-bit unsigned. Downsampling averages the source span of each output byte.
    const double dstRate = static_cast<double>(paramInt(DacRate));
    const double step = sourceSampleRate / dstRate;
    const long outLen = static_cast<long>(std::floor(static_cast<double>(numFrames) / step));
    if (outLen > kDacMaxBytes)
        return false;

    const int current = activeBank.load(std::memory_order_seq_cst);
    const int target = current ^ 1;
    // The audio thread may still be inside a block that started before the previous load
    // flipped the index; wait until it has left that bank (at most one audio block, so the
    // message thread yields instead of sleeping for a whole scheduler tick).
    while (bankInUse.load(std::memory_order_seq_cst) == target)
        std::this_thread::yield();
    DacBank& dst = banks[target];
    const DacBank& src = banks[current];
    for (int s = 0; s < kDacSlots; ++s)
    {
        if (s == slot)
            continue;
        std::copy_n(src.data[s].begin(), src.view[s].length, dst.data[s].begin());
        dst.view[s].length = src.view[s].length;
    }

    for (long i = 0; i < outLen; ++i)
    {
        const double x0 = static_cast<double>(i) * step;
        double v = 0.0;
        if (step > 1.0)
        {
            const int a = static_cast<int>(x0);
            const int b = std::min(numFrames, static_cast<int>(x0 + step));
            for (int k = a; k < b; ++k)
                v += mono[k];
            v /= std::max(1, b - a);
        }
        else
        {
            const int a = std::min(numFrames - 1, static_cast<int>(x0));
            const int b = std::min(numFrames - 1, a + 1);
            const double f = x0 - a;
            v = mono[a] * (1.0 - f) + mono[b] * f;
        }
        const long q = std::lround(128.0 + std::clamp(v, -1.0, 1.0) * 127.0);
        dst.data[slot][static_cast<size_t>(i)] = static_cast<uint8_t>(std::clamp(q, 0L, 255L));
    }
    dst.view[slot].length = static_cast<int>(outLen);

    activeBank.store(target, std::memory_order_seq_cst);
    return true;
}

bool GenesisEngine::clearSample(int slot)
{
    if (slot < 0 || slot >= kDacSlots)
        return false;
    // Same two-bank flip as loadSample(); length 0 = empty slot.
    const int current = activeBank.load(std::memory_order_seq_cst);
    const int target = current ^ 1;
    while (bankInUse.load(std::memory_order_seq_cst) == target)
        std::this_thread::yield();
    DacBank& dst = banks[target];
    const DacBank& src = banks[current];
    for (int s = 0; s < kDacSlots; ++s)
    {
        const int length = s == slot ? 0 : src.view[s].length;
        std::copy_n(src.data[s].begin(), length, dst.data[s].begin());
        dst.view[s].length = length;
    }
    activeBank.store(target, std::memory_order_seq_cst);
    return true;
}

// ----- notes --------------------------------------------------------------------------------------

void GenesisEngine::noteOn(int channel, float midiNote, float velocity) noexcept
{
    if (channel < 0 || channel >= kGenesisChannels)
        return;
    refreshDriver();
    drv.noteOn(channel, midiNote, velocity);
    // The note takes effect at blockStart; see GenesisDriver::deferFirstTick().
    if (nextFrame - blockStart < 0.5 * static_cast<double>(masterClocksPerFrame(clockStd)))
        drv.deferFirstTick(channel);
    releaseDacBank();
}

void GenesisEngine::noteOff(int channel) noexcept
{
    if (channel < 0 || channel >= kGenesisChannels)
        return;
    refreshDriver();
    drv.noteOff(channel);
    releaseDacBank();
}

void GenesisEngine::setChannelPitch(int channel, float midiNote) noexcept
{
    drv.setPitch(channel, midiNote);
}

bool GenesisEngine::isChannelActive(int channel) const noexcept
{
    return drv.isActive(channel);
}

// ----- rendering ---------------------------------------------------------------------------------

void GenesisEngine::emitFm(double t, bool perChannel) noexcept
{
    const int l = ym.outputLeft();
    const int r = ym.outputRight();
    if (l != fmLevelL)
    {
        fmSynthL.addDelta(t, static_cast<float>(l - fmLevelL) * kOutputScale);
        fmLevelL = l;
    }
    if (r != fmLevelR)
    {
        fmSynthR.addDelta(t, static_cast<float>(r - fmLevelR) * kOutputScale);
        fmLevelR = r;
    }
    if (!perChannel)
        return;
    for (int c = 0; c < 6; ++c)
    {
        const int cl = ym.channelOutputLeft(c);
        const int cr = ym.channelOutputRight(c);
        if (cl != fmChLevelL[c])
        {
            fmChSynthL[c].addDelta(t, static_cast<float>(cl - fmChLevelL[c]) * kOutputScale);
            fmChLevelL[c] = cl;
        }
        if (cr != fmChLevelR[c])
        {
            fmChSynthR[c].addDelta(t, static_cast<float>(cr - fmChLevelR[c]) * kOutputScale);
            fmChLevelR[c] = cr;
        }
    }
}

void GenesisEngine::emitPsg(double t, bool perChannel) noexcept
{
    constexpr float kPsgScale = static_cast<float>(kPsgToFmGain) * kOutputScale;
    const int m = psg.mix();
    if (m != psgLevel)
    {
        psgSynth.addDelta(t, static_cast<float>(m - psgLevel) * kPsgScale);
        psgLevel = m;
    }
    if (!perChannel)
        return;
    for (int i = 0; i < 4; ++i)
    {
        const int v = psg.channelLevel(i);
        if (v != psgChLevel[i])
        {
            psgChSynth[i].addDelta(t, static_cast<float>(v - psgChLevel[i]) * kPsgScale);
            psgChLevel[i] = v;
        }
    }
}

void GenesisEngine::renderChunk(float* mainL, float* mainR, float* const* outsL, float* const* outsR, int n) noexcept
{
    applyClock();
    ym.setLadderEffect(paramInt(ChipRevision) == 0);
    lowPassOn = paramInt(Model1Lowpass) != 0;
    refreshDriver();

    const bool perChannel = outsL != nullptr || outsR != nullptr;
    if (perChannel && !perChannelLive)
    {
        for (int c = 0; c < 6; ++c)
        {
            fmChSynthL[c].reset();
            fmChSynthR[c].reset();
        }
        for (int i = 0; i < 4; ++i)
            psgChSynth[i].reset();
        for (int c = 0; c < kGenesisChannels; ++c)
        {
            chFilterL[c].reset();
            chFilterR[c].reset();
        }
        syncLevelTrackers(false);
    }
    perChannelLive = perChannel;

    if (drv.takeDacRestart())
        nextDac = blockStart;

    // Event loop in master clocks: driver frame, DAC writes, FM samples, PSG ticks.
    const double blockEnd = blockStart + static_cast<double>(n) * masterPerHost;
    const double framePeriod = static_cast<double>(masterClocksPerFrame(clockStd));
    for (;;)
    {
        const bool dacOn = drv.dacPlaying();
        double t = std::min(nextFrame, std::min(nextFm, nextPsg));
        if (dacOn)
            t = std::min(t, nextDac);
        if (t >= blockEnd)
            break;
        const double hostTime = (t - blockStart) * hostPerMaster;

        if (nextFrame <= t)
        {
            drv.frameTick();
            nextFrame += framePeriod;
        }
        else if (dacOn && nextDac <= t)
        {
            drv.dacWriteNext();
            nextDac += drv.dacIntervalMasterClocks();
        }
        else if (nextFm <= t)
        {
            ym.clockSample();
            emitFm(hostTime, perChannel);
            nextFm += kMasterClocksPerFmSample;
        }
        else
        {
            psg.clock();
            emitPsg(hostTime, perChannel);
            nextPsg += kMasterClocksPerPsgTick;
        }
    }
    releaseDacBank();   // no sample byte is read until the next acquireDacBank()

    // Main output: FM L/R + mono PSG, then coupling capacitor and optional Model 1 low-pass.
    std::fill(mainL, mainL + n, 0.0f);
    std::fill(mainR, mainR + n, 0.0f);
    fmSynthL.endBlock(mainL, n);
    fmSynthR.endBlock(mainR, n);
    float* tmp = scratch.data();
    psgSynth.endBlockReplace(tmp, n);
    for (int i = 0; i < n; ++i)
    {
        mainL[i] = mainFilterL.process(mainL[i] + tmp[i], lowPassOn);
        mainR[i] = mainFilterR.process(mainR[i] + tmp[i], lowPassOn);
    }

    if (perChannel)
    {
        auto finish = [&](BandLimitedStepSynth& synth, float* out, OutputFilters& filter) {
            float* dst = out != nullptr ? out : tmp;
            synth.endBlockReplace(dst, n);
            if (out != nullptr)
                for (int i = 0; i < n; ++i)
                    out[i] = filter.process(out[i], lowPassOn);
        };
        for (int c = 0; c < 6; ++c)
        {
            finish(fmChSynthL[c], outsL != nullptr ? outsL[c] : nullptr, chFilterL[c]);
            finish(fmChSynthR[c], outsR != nullptr ? outsR[c] : nullptr, chFilterR[c]);
        }
        for (int i = 0; i < 4; ++i)
        {
            const int c = 6 + i;
            float* l = outsL != nullptr ? outsL[c] : nullptr;
            float* r = outsR != nullptr ? outsR[c] : nullptr;
            psgChSynth[i].endBlockReplace(tmp, n);
            for (int k = 0; k < n; ++k)
            {
                const float x = tmp[k];
                const float yl = chFilterL[c].process(x, lowPassOn);
                const float yr = chFilterR[c].process(x, lowPassOn);
                if (l != nullptr)
                    l[k] = yl;
                if (r != nullptr)
                    r[k] = yr;
            }
        }
    }

    // Advance and rebase the time base (keeps doubles exact for integer event times).
    blockStart = blockEnd;
    if (blockStart > 1.0e9)
    {
        const double base = std::floor(blockStart);
        blockStart -= base;
        nextFm -= base;
        nextPsg -= base;
        nextFrame -= base;
        nextDac -= base;
    }
}

void GenesisEngine::renderBlock(float* mainL, float* mainR, float* const* channelOutsL, float* const* channelOutsR,
                                int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (maxBlock <= 0 || scratch.empty())
    {
        std::fill(mainL, mainL + numSamples, 0.0f);
        std::fill(mainR, mainR + numSamples, 0.0f);
        return;
    }

    int done = 0;
    while (done < numSamples)
    {
        const int n = std::min(numSamples - done, maxBlock);
        float* chL[kGenesisChannels] = {};
        float* chR[kGenesisChannels] = {};
        for (int c = 0; c < kGenesisChannels; ++c)
        {
            if (channelOutsL != nullptr && channelOutsL[c] != nullptr)
                chL[c] = channelOutsL[c] + done;
            if (channelOutsR != nullptr && channelOutsR[c] != nullptr)
                chR[c] = channelOutsR[c] + done;
        }
        renderChunk(mainL + done, mainR + done, channelOutsL != nullptr ? chL : nullptr,
                    channelOutsR != nullptr ? chR : nullptr, n);
        done += n;
    }
}

} // namespace chipdsp
