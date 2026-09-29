#include "chipdsp/nes/Nes2A03Engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace chipdsp
{

using namespace nes;

namespace
{
    const char* const kClockLabels[] = { "NTSC", "PAL" };
    const char* const kOffOn[] = { "Off", "On" };
    const char* const kDutyLabels[] = { "12.5 %", "25 %", "50 %", "75 %" };
    const char* const kNoiseModeLabels[] = { "Long", "Short" };

    using E = Nes2A03Engine;

    // Native units and hardware bounds (ENGINE_SPECS.md "NES"). Every parameter is an integer.
    constexpr ParamDesc kDescs[] = {
        { E::Clock,          "clock",          "Clock",          "Global", 0, 1, 0, true, "", kClockLabels },
        { E::ConsoleFilter,  "console_filter", "Console Filter", "Global", 0, 1, 1, true, "", kOffOn },

        { E::P1Duty,          "p1_duty",            "Pulse 1 Duty",            "Pulse 1", 0, 3, 2, true, "", kDutyLabels },
        { E::P1Volume,        "p1_volume",          "Pulse 1 Volume",          "Pulse 1", 0, 15, 12, true, "", nullptr },
        { E::P1EnvEnable,     "p1_env_enable",      "Pulse 1 Envelope",        "Pulse 1", 0, 1, 0, true, "", kOffOn },
        { E::P1EnvLoop,       "p1_env_loop",        "Pulse 1 Envelope Loop",   "Pulse 1", 0, 1, 0, true, "", kOffOn },
        { E::P1SweepEnable,   "p1_sweep_enable",    "Pulse 1 Sweep",           "Pulse 1", 0, 1, 0, true, "", kOffOn },
        { E::P1SweepPeriod,   "p1_sweep_period",    "Pulse 1 Sweep Period",    "Pulse 1", 0, 7, 0, true, "", nullptr },
        { E::P1SweepNegate,   "p1_sweep_negate",    "Pulse 1 Sweep Negate",    "Pulse 1", 0, 1, 0, true, "", kOffOn },
        { E::P1SweepShift,    "p1_sweep_shift",     "Pulse 1 Sweep Shift",     "Pulse 1", 0, 7, 0, true, "", nullptr },
        { E::P1VibratoRate,   "p1_vibrato_rate",    "Pulse 1 Vibrato Rate",    "Pulse 1", 0, 15, 0, true, "frames", nullptr },
        { E::P1VibratoDepth,  "p1_vibrato_depth",   "Pulse 1 Vibrato Depth",   "Pulse 1", 0, 31, 0, true, "", nullptr },
        { E::P1VibratoDelay,  "p1_vibrato_delay",   "Pulse 1 Vibrato Delay",   "Pulse 1", 0, 60, 0, true, "frames", nullptr },
        { E::P1SwAttack,      "p1_sw_attack",       "Pulse 1 Attack",          "Pulse 1", 0, 30, 0, true, "frames", nullptr },
        { E::P1SwDecay,       "p1_sw_decay",        "Pulse 1 Decay",           "Pulse 1", 0, 60, 0, true, "frames", nullptr },
        { E::P1SwSustain,     "p1_sw_sustain",      "Pulse 1 Sustain",         "Pulse 1", 0, 15, 15, true, "", nullptr },
        { E::P1SwRelease,     "p1_sw_release",      "Pulse 1 Release",         "Pulse 1", 0, 60, 0, true, "frames", nullptr },
        { E::P1PitchEnvDepth, "p1_pitch_env_depth", "Pulse 1 Pitch Env Depth", "Pulse 1", -64, 64, 0, true, "", nullptr },
        { E::P1PitchEnvSpeed, "p1_pitch_env_speed", "Pulse 1 Pitch Env Speed", "Pulse 1", 0, 30, 0, true, "frames", nullptr },
        { E::P1Transpose,     "p1_transpose",       "Pulse 1 Transpose",       "Pulse 1", -24, 24, 0, true, "st", nullptr },

        { E::P2Duty,          "p2_duty",            "Pulse 2 Duty",            "Pulse 2", 0, 3, 2, true, "", kDutyLabels },
        { E::P2Volume,        "p2_volume",          "Pulse 2 Volume",          "Pulse 2", 0, 15, 12, true, "", nullptr },
        { E::P2EnvEnable,     "p2_env_enable",      "Pulse 2 Envelope",        "Pulse 2", 0, 1, 0, true, "", kOffOn },
        { E::P2EnvLoop,       "p2_env_loop",        "Pulse 2 Envelope Loop",   "Pulse 2", 0, 1, 0, true, "", kOffOn },
        { E::P2SweepEnable,   "p2_sweep_enable",    "Pulse 2 Sweep",           "Pulse 2", 0, 1, 0, true, "", kOffOn },
        { E::P2SweepPeriod,   "p2_sweep_period",    "Pulse 2 Sweep Period",    "Pulse 2", 0, 7, 0, true, "", nullptr },
        { E::P2SweepNegate,   "p2_sweep_negate",    "Pulse 2 Sweep Negate",    "Pulse 2", 0, 1, 0, true, "", kOffOn },
        { E::P2SweepShift,    "p2_sweep_shift",     "Pulse 2 Sweep Shift",     "Pulse 2", 0, 7, 0, true, "", nullptr },
        { E::P2VibratoRate,   "p2_vibrato_rate",    "Pulse 2 Vibrato Rate",    "Pulse 2", 0, 15, 0, true, "frames", nullptr },
        { E::P2VibratoDepth,  "p2_vibrato_depth",   "Pulse 2 Vibrato Depth",   "Pulse 2", 0, 31, 0, true, "", nullptr },
        { E::P2VibratoDelay,  "p2_vibrato_delay",   "Pulse 2 Vibrato Delay",   "Pulse 2", 0, 60, 0, true, "frames", nullptr },
        { E::P2SwAttack,      "p2_sw_attack",       "Pulse 2 Attack",          "Pulse 2", 0, 30, 0, true, "frames", nullptr },
        { E::P2SwDecay,       "p2_sw_decay",        "Pulse 2 Decay",           "Pulse 2", 0, 60, 0, true, "frames", nullptr },
        { E::P2SwSustain,     "p2_sw_sustain",      "Pulse 2 Sustain",         "Pulse 2", 0, 15, 15, true, "", nullptr },
        { E::P2SwRelease,     "p2_sw_release",      "Pulse 2 Release",         "Pulse 2", 0, 60, 0, true, "frames", nullptr },
        { E::P2PitchEnvDepth, "p2_pitch_env_depth", "Pulse 2 Pitch Env Depth", "Pulse 2", -64, 64, 0, true, "", nullptr },
        { E::P2PitchEnvSpeed, "p2_pitch_env_speed", "Pulse 2 Pitch Env Speed", "Pulse 2", 0, 30, 0, true, "frames", nullptr },
        { E::P2Transpose,     "p2_transpose",       "Pulse 2 Transpose",       "Pulse 2", -24, 24, 0, true, "st", nullptr },

        { E::TriLinearLength,  "tri_linear_length",   "Triangle Linear Length",   "Triangle", 0, 127, 127, true, "", nullptr },
        { E::TriGateFrames,    "tri_gate_frames",     "Triangle Gate",            "Triangle", 0, 30, 0, true, "frames", nullptr },
        { E::TriAttackFrames,  "tri_attack_frames",   "Triangle Attack",          "Triangle", 0, 8, 0, true, "frames", nullptr },
        { E::TriVibratoRate,   "tri_vibrato_rate",    "Triangle Vibrato Rate",    "Triangle", 0, 15, 0, true, "frames", nullptr },
        { E::TriVibratoDepth,  "tri_vibrato_depth",   "Triangle Vibrato Depth",   "Triangle", 0, 31, 0, true, "", nullptr },
        { E::TriVibratoDelay,  "tri_vibrato_delay",   "Triangle Vibrato Delay",   "Triangle", 0, 60, 0, true, "frames", nullptr },
        { E::TriPitchEnvDepth, "tri_pitch_env_depth", "Triangle Pitch Env Depth", "Triangle", -64, 64, 0, true, "", nullptr },
        { E::TriPitchEnvSpeed, "tri_pitch_env_speed", "Triangle Pitch Env Speed", "Triangle", 0, 30, 0, true, "frames", nullptr },
        { E::TriTranspose,     "tri_transpose",       "Triangle Transpose",       "Triangle", -24, 24, 0, true, "st", nullptr },

        { E::NzMode,          "nz_mode",            "Noise Mode",            "Noise", 0, 1, 0, true, "", kNoiseModeLabels },
        { E::NzVolume,        "nz_volume",          "Noise Volume",          "Noise", 0, 15, 12, true, "", nullptr },
        { E::NzEnvEnable,     "nz_env_enable",      "Noise Envelope",        "Noise", 0, 1, 0, true, "", kOffOn },
        { E::NzEnvLoop,       "nz_env_loop",        "Noise Envelope Loop",   "Noise", 0, 1, 0, true, "", kOffOn },
        { E::NzPeriod,        "nz_period",          "Noise Period",          "Noise", 0, 15, 8, true, "", nullptr },
        { E::NzKeyed,         "nz_keyed",           "Noise Keyed",           "Noise", 0, 1, 0, true, "", kOffOn },
        { E::NzSwAttack,      "nz_sw_attack",       "Noise Attack",          "Noise", 0, 30, 0, true, "frames", nullptr },
        { E::NzSwDecay,       "nz_sw_decay",        "Noise Decay",           "Noise", 0, 60, 0, true, "frames", nullptr },
        { E::NzSwSustain,     "nz_sw_sustain",      "Noise Sustain",         "Noise", 0, 15, 15, true, "", nullptr },
        { E::NzSwRelease,     "nz_sw_release",      "Noise Release",         "Noise", 0, 60, 0, true, "frames", nullptr },
        { E::NzPitchEnvDepth, "nz_pitch_env_depth", "Noise Pitch Env Depth", "Noise", -15, 15, 0, true, "", nullptr },
        { E::NzPitchEnvSpeed, "nz_pitch_env_speed", "Noise Pitch Env Speed", "Noise", 0, 30, 0, true, "frames", nullptr },

        { E::DmcRate,        "dmc_rate",         "DMC Rate",         "DMC", 0, 15, 15, true, "", nullptr },
        { E::DmcSample,      "dmc_sample",       "DMC Sample",       "DMC", 0, 15, 0, true, "", nullptr },
        { E::DmcLoop,        "dmc_loop",         "DMC Loop",         "DMC", 0, 1, 0, true, "", kOffOn },
        { E::DmcKeyed,       "dmc_keyed",        "DMC Keyed",        "DMC", 0, 1, 0, true, "", kOffOn },
        { E::DmcDirectLevel, "dmc_direct_level", "DMC Direct Level", "DMC", 0, 127, 64, true, "", nullptr },
    };
    static_assert(std::size(kDescs) == Nes2A03Engine::NumParams, "one descriptor per parameter id");

    const ChannelInfo kChannelInfo[kNesChannels] = {
        { "Pulse 1", "P1", true },
        { "Pulse 2", "P2", true },
        { "Triangle", "TRI", true },
        { "Noise", "NZ", false },
        { "DMC", "DMC", false },
    };
} // namespace

// ===== construction / lifecycle ==============================================================

Nes2A03Engine::Nes2A03Engine()
    : banks(std::make_unique<SampleBank[]>(kNumBanks))
{
    for (int i = 0; i < NumParams; ++i)
        params[static_cast<size_t>(i)].store(0.0f);
    for (const auto& d : kDescs)
    {
        params[static_cast<size_t>(d.id)].store(d.defaultValue);
        applyParam(d.id, static_cast<int>(d.defaultValue));
    }
}

ChannelInfo Nes2A03Engine::channelInfo(int channel) const noexcept
{
    return kChannelInfo[std::clamp(channel, 0, kNesChannels - 1)];
}

double Nes2A03Engine::nativeSampleRate() const noexcept
{
    return getParameter(Clock) >= 0.5f ? kCpuHzPal : kCpuHzNtsc;
}

std::span<const ParamDesc> Nes2A03Engine::parameterDescriptors() const noexcept
{
    return { kDescs, std::size(kDescs) };
}

void Nes2A03Engine::prepare(double hostSampleRate, int maxBlockSize)
{
    hostRate = hostSampleRate;
    maxBlock = std::max(1, maxBlockSize);
    mixer.build();
    // Both clocks downsample to any host rate, so the kernel (cutoff 0.45) is the same for NTSC
    // and PAL; the engine computes host times itself (hostSamplesPerClock), so the synths never
    // need re-preparing when the clock standard changes on the audio thread.
    mainSynth.prepare(kCpuHzNtsc, hostRate, maxBlock);
    mainStage.prepare(hostRate);
    for (int c = 0; c < kNesChannels; ++c)
    {
        channelSynths[static_cast<size_t>(c)].prepare(kCpuHzNtsc, hostRate, maxBlock);
        channelStages[static_cast<size_t>(c)].prepare(hostRate);
    }
    scratch.assign(static_cast<size_t>(maxBlock), 0.0f);
    setRawOutput(raw);
    reset();
}

void Nes2A03Engine::reset() noexcept
{
    const bool pal = getParameter(Clock) >= 0.5f;
    chip.reset();
    applyRegion(pal);
    driver.reset(chip);

    mainSynth.reset();
    mainStage.reset();
    for (int c = 0; c < kNesChannels; ++c)
    {
        channelSynths[static_cast<size_t>(c)].reset();
        channelStages[static_cast<size_t>(c)].reset();
    }
    clockTime = 0.0;
    driverPhase = 0;

    // The synths track changes relative to the power-up level (the halted triangle holds step 0
    // = 15, a DC offset), so a reset does not produce a thump.
    lastLevels = readLevels();
    if (mixer.isBuilt())
    {
        const Levels& lv = lastLevels;
        lastMix = mixer.mix(lv.p1, lv.p2, lv.tri, lv.triUltrasonic, lv.noise, lv.dmc);
        for (int c = 0; c < kNesChannels; ++c)
            lastSolo[static_cast<size_t>(c)] = soloLevel(c, lv);
    }
    channelsRendered = false;
}

void Nes2A03Engine::applyRegion(bool pal) noexcept
{
    chip.setRegion(pal);
    hostSamplesPerClock = hostRate / chip.cpuHz();
    driverPeriod = pal ? kFrameHalfCyclesPal : kFrameHalfCyclesNtsc;
    if (driverPhase >= driverPeriod)
        driverPhase = 0;
}

// ===== parameters ============================================================================

void Nes2A03Engine::setParameter(int id, float value) noexcept
{
    if (id < 0 || id >= NumParams)
        return;
    const ParamDesc& d = kDescs[id];
    const float v = std::clamp(std::round(value), d.minValue, d.maxValue);
    params[static_cast<size_t>(id)].store(v);
    applyParam(id, static_cast<int>(v));
}

void Nes2A03Engine::stageParameter(int id, float value) noexcept
{
    // Only the atomic (read by loadSample()); the driver settings follow at setParameter().
    if (id < 0 || id >= NumParams)
        return;
    const ParamDesc& d = kDescs[id];
    params[static_cast<size_t>(id)].store(std::clamp(std::round(value), d.minValue, d.maxValue));
}

float Nes2A03Engine::getParameter(int id) const noexcept
{
    return (id >= 0 && id < NumParams) ? params[static_cast<size_t>(id)].load() : 0.0f;
}

void Nes2A03Engine::setClockStandard(ClockStandard standard) noexcept
{
    setParameter(Clock, standard == ClockStandard::Pal ? 1.0f : 0.0f);
}

void Nes2A03Engine::setRawOutput(bool r) noexcept
{
    raw = r;
    mainSynth.setRaw(r);
    for (auto& s : channelSynths)
        s.setRaw(r);
}

void Nes2A03Engine::applyParam(int id, int v) noexcept
{
    if (id >= P1Duty && id < P1Duty + 2 * kPulseParamStride)
    {
        PulseSettings& p = settings.pulse[static_cast<size_t>((id - P1Duty) / kPulseParamStride)];
        switch (P1Duty + (id - P1Duty) % kPulseParamStride)
        {
            case P1Duty:          p.duty = v; break;
            case P1Volume:        p.volume = v; break;
            case P1EnvEnable:     p.envEnable = v; break;
            case P1EnvLoop:       p.envLoop = v; break;
            case P1SweepEnable:   p.sweepEnable = v; break;
            case P1SweepPeriod:   p.sweepPeriod = v; break;
            case P1SweepNegate:   p.sweepNegate = v; break;
            case P1SweepShift:    p.sweepShift = v; break;
            case P1VibratoRate:   p.vibratoRate = v; break;
            case P1VibratoDepth:  p.vibratoDepth = v; break;
            case P1VibratoDelay:  p.vibratoDelay = v; break;
            case P1SwAttack:      p.swAttack = v; break;
            case P1SwDecay:       p.swDecay = v; break;
            case P1SwSustain:     p.swSustain = v; break;
            case P1SwRelease:     p.swRelease = v; break;
            case P1PitchEnvDepth: p.pitchEnvDepth = v; break;
            case P1PitchEnvSpeed: p.pitchEnvSpeed = v; break;
            case P1Transpose:     p.transpose = v; break;
            default: break;
        }
        return;
    }

    TriangleSettings& t = settings.triangle;
    NoiseSettings& n = settings.noise;
    DmcSettings& d = settings.dmc;
    switch (id)
    {
        case TriLinearLength:  t.linearLength = v; break;
        case TriGateFrames:    t.gateFrames = v; break;
        case TriAttackFrames:  t.attackFrames = v; break;
        case TriVibratoRate:   t.vibratoRate = v; break;
        case TriVibratoDepth:  t.vibratoDepth = v; break;
        case TriVibratoDelay:  t.vibratoDelay = v; break;
        case TriPitchEnvDepth: t.pitchEnvDepth = v; break;
        case TriPitchEnvSpeed: t.pitchEnvSpeed = v; break;
        case TriTranspose:     t.transpose = v; break;

        case NzMode:          n.mode = v; break;
        case NzVolume:        n.volume = v; break;
        case NzEnvEnable:     n.envEnable = v; break;
        case NzEnvLoop:       n.envLoop = v; break;
        case NzPeriod:        n.period = v; break;
        case NzKeyed:         n.keyed = v; break;
        case NzSwAttack:      n.swAttack = v; break;
        case NzSwDecay:       n.swDecay = v; break;
        case NzSwSustain:     n.swSustain = v; break;
        case NzSwRelease:     n.swRelease = v; break;
        case NzPitchEnvDepth: n.pitchEnvDepth = v; break;
        case NzPitchEnvSpeed: n.pitchEnvSpeed = v; break;

        case DmcRate:        d.rate = v; break;
        case DmcSample:      d.sample = v; break;
        case DmcLoop:        d.loop = v; break;
        case DmcKeyed:       d.keyed = v; break;
        case DmcDirectLevel: d.directLevel = v; break;
        default: break; // Clock and ConsoleFilter are read at block start
    }
}

// ===== notes =================================================================================

void Nes2A03Engine::noteOn(int channel, float midiNote, float velocity) noexcept
{
    if (channel < 0 || channel >= kNesChannels)
        return;
    const bool pal = getParameter(Clock) >= 0.5f;
    if (pal != chip.isPal())
        applyRegion(pal);

    DmcSampleRef ref;
    if (channel == 4)
    {
        // The DMC now reads the active bank until its next note-on (a bank switch like a mapper
        // would do); the driver stops the previous sample before anything is read.
        const int bank = claimActiveBank();
        if (bank >= 0)
            ref = sampleRef(bank, std::clamp(settings.dmc.sample, 0, kNumSampleSlots - 1));
        if (ref.length == 0)
            mappedBank.store(-1); // empty slot: the driver maps nothing
    }
    driver.noteOn(chip, channel, midiNote, velocity, settings, ref);
    // The note takes effect at the current CPU cycle; see NesDriver::deferFirstTick().
    if (driverPeriod - driverPhase < driverPeriod / 2)
        driver.deferFirstTick(channel);
}

void Nes2A03Engine::noteOff(int channel) noexcept
{
    if (channel >= 0 && channel < kNesChannels)
        driver.noteOff(chip, channel, settings);
}

void Nes2A03Engine::setChannelPitch(int channel, float midiNote) noexcept
{
    driver.setPitch(channel, midiNote);
}

bool Nes2A03Engine::isChannelActive(int channel) const noexcept
{
    // ENGINE_SPECS "Conventions": active until the envelope has fully released or the channel
    // was silenced, whether or not the key is still held. Silent but about to sound (software
    // attack from 0, triangle attack delay or gate retrigger) counts as active.
    if (channel < 0 || channel >= kNesChannels)
        return false;
    if (driver.isAttackPending(channel, settings))
        return true;
    // A pending envelope start (set by $4003/$4007/$400F) reloads the decay to 15 at the next
    // quarter frame; a looping hardware envelope passes through 0 and wraps to 15.
    const auto sounding = [](uint8_t length, uint8_t volume, const Envelope& env) {
        return length > 0 && (volume > 0 || env.start || (env.loop && !env.constant));
    };
    switch (channel)
    {
        case 0: return sounding(chip.pulse1.lengthValue(), chip.pulse1.volume(), chip.pulse1.env());
        case 1: return sounding(chip.pulse2.lengthValue(), chip.pulse2.volume(), chip.pulse2.env());
        case 2: return chip.triangle.isRunning() || chip.triangle.isStarting();
        case 3: return sounding(chip.noise.lengthValue(), chip.noise.volume(), chip.noise.env());
        default: return chip.dmc.isActive();
    }
}

// ===== samples ===============================================================================

int Nes2A03Engine::paddedDmcLength(int numBytes) noexcept
{
    if (numBytes <= 0)
        return 0;
    const int l = (numBytes - 1 + 15) / 16;
    return l * 16 + 1;
}

int Nes2A03Engine::encodeDmc(const float* mono, int numFrames, double sourceSampleRate, double bitRateHz,
                             uint8_t startLevel, uint8_t* out, int maxBytes)
{
    if (numFrames <= 0)
        return 0;
    // Resampling to the DMC bit rate is a tool step (linear interpolation), not hardware.
    const double step = sourceSampleRate / bitRateHz;
    const int numBits = static_cast<int>(std::ceil(static_cast<double>(numFrames) / step));
    const int numBytes = (numBits + 7) / 8;
    if (numBytes > maxBytes)
        return -1;
    std::fill(out, out + numBytes, uint8_t { 0 });

    uint8_t level = startLevel;
    for (int i = 0; i < numBytes * 8; ++i)
    {
        bool bit;
        if (i < numBits)
        {
            const double pos = static_cast<double>(i) * step;
            const int i0 = std::min(static_cast<int>(pos), numFrames - 1);
            const int i1 = std::min(i0 + 1, numFrames - 1);
            const double frac = pos - static_cast<double>(i0);
            const double x = static_cast<double>(mono[i0]) * (1.0 - frac) + static_cast<double>(mono[i1]) * frac;
            // Full scale -1..1 maps to levels 1..127 around the centre 64.
            const double target = std::clamp(64.0 + 63.0 * x, 0.0, 127.0);
            bit = target > static_cast<double>(level);
        }
        else
        {
            bit = (i & 1) == 0; // pad the last byte with +2 / -2 alternation
        }
        level = dmcOutputStep(level, bit);
        if (bit)
            out[i / 8] = static_cast<uint8_t>(out[i / 8] | (1u << (i % 8)));
    }
    return numBytes;
}

bool Nes2A03Engine::loadSample(int slot, const float* mono, int numFrames, double sourceSampleRate)
{
    if (slot < 0 || slot >= kNumSampleSlots || numFrames < 0 || (numFrames > 0 && (mono == nullptr || sourceSampleRate <= 0.0)))
        return false;

    // Encode at the DMC rate currently selected by dmc_rate and the current clock.
    const bool pal = getParameter(Clock) >= 0.5f;
    const int rate = std::clamp(static_cast<int>(getParameter(DmcRate)), 0, 15);
    const double bitRate = (pal ? kCpuHzPal : kCpuHzNtsc) / (pal ? kDmcPeriodPal[rate] : kDmcPeriodNtsc[rate]);

    std::vector<uint8_t> encoded(kSlotCapacity, 0x55);
    const int bytes = encodeDmc(mono, numFrames, sourceSampleRate, bitRate, kDmcEncoderStartLevel, encoded.data(),
                                kDmcMaxSampleBytes);
    if (bytes < 0)
        return false;
    const int playable = paddedDmcLength(bytes); // bytes beyond the encoded data stay 0x55 (net 0)

    // Fill a bank that is neither the active one (copied from) nor the one the DMC is mapped to.
    // The audio thread only ever maps the active bank and re-checks it after publishing its
    // claim (claimActiveBank), so the target is never read while it is written.
    const int active = activeBank.load();
    const int mapped = mappedBank.load();
    int target = 0;
    while (target == active || target == mapped)
        ++target; // < kNumBanks: at most two banks are excluded

    SampleBank& dst = banks[static_cast<size_t>(target)];
    dst = banks[static_cast<size_t>(active)];
    std::memcpy(dst.data.data() + static_cast<size_t>(slot) * kSlotCapacity, encoded.data(), kSlotCapacity);
    dst.length[static_cast<size_t>(slot)] = playable;
    activeBank.store(target);
    return true;
}

bool Nes2A03Engine::clearSample(int slot)
{
    if (slot < 0 || slot >= kNumSampleSlots)
        return false;
    // Same bank rotation as loadSample(); length 0 = empty slot (the DMC note plays nothing).
    const int active = activeBank.load();
    const int mapped = mappedBank.load();
    int target = 0;
    while (target == active || target == mapped)
        ++target;

    SampleBank& dst = banks[static_cast<size_t>(target)];
    dst = banks[static_cast<size_t>(active)];
    std::memset(dst.data.data() + static_cast<size_t>(slot) * kSlotCapacity, 0x55, kSlotCapacity);
    dst.length[static_cast<size_t>(slot)] = 0;
    activeBank.store(target);
    return true;
}

int Nes2A03Engine::sampleLength(int slot) const noexcept
{
    if (slot < 0 || slot >= kNumSampleSlots)
        return 0;
    return banks[static_cast<size_t>(activeBank.load())].length[static_cast<size_t>(slot)];
}

int Nes2A03Engine::claimActiveBank() noexcept
{
    // Publish the claim, then check the bank is still active. A loadSample() that started before
    // the claim writes a bank that was not active when it started (and stays inactive until it has
    // finished); one that starts after it sees the claim. A retry needs a complete loadSample()
    // between two atomic loads, so the loop is bounded; if every attempt fails, nothing is mapped
    // and the note plays without its sample.
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const int b = activeBank.load();
        mappedBank.store(b);
        if (activeBank.load() == b)
            return b;
    }
    mappedBank.store(-1);
    return -1;
}

DmcSampleRef Nes2A03Engine::sampleRef(int bank, int slot) const noexcept
{
    const SampleBank& b = banks[static_cast<size_t>(bank)];
    DmcSampleRef ref;
    ref.bytes = b.data.data() + static_cast<size_t>(slot) * kSlotCapacity;
    ref.capacity = kSlotCapacity;
    ref.length = b.length[static_cast<size_t>(slot)];
    return ref;
}

// ===== rendering =============================================================================

Nes2A03Engine::Levels Nes2A03Engine::readLevels() const noexcept
{
    Levels lv;
    lv.p1 = chip.pulse1.output();
    lv.p2 = chip.pulse2.output();
    lv.triUltrasonic = chip.triangle.isUltrasonic();
    // Ultrasonic (A2): the mixer uses the constant 7.5, so the stepping sequencer value is left
    // out of the comparison; otherwise it would differ on every CPU cycle and force a mixer
    // evaluation 1.79 M times per second.
    lv.tri = lv.triUltrasonic ? uint8_t { 0 } : chip.triangle.output();
    lv.noise = chip.noise.output();
    lv.dmc = chip.dmc.output();
    return lv;
}

float Nes2A03Engine::soloLevel(int channel, const Levels& lv) const noexcept
{
    // A channel alone through the same mixer, the other four inputs at 0 (ENGINE_SPECS.md).
    switch (channel)
    {
        case 0: return mixer.pulse(lv.p1, 0);
        case 1: return mixer.pulse(lv.p2, 0);
        case 2: return lv.triUltrasonic ? mixer.tndUltrasonic(0, 0) : mixer.tnd(lv.tri, 0, 0);
        case 3: return mixer.tnd(0, lv.noise, 0);
        default: return mixer.tnd(0, 0, lv.dmc);
    }
}

void Nes2A03Engine::renderBlock(float* mainL, float* mainR, float* const* channelOutsL, float* const* channelOutsR,
                                int numSamples) noexcept
{
    if (numSamples <= 0)
        return;
    if (!mixer.isBuilt()) // not prepared yet: silence
    {
        std::fill(mainL, mainL + numSamples, 0.0f);
        if (mainR != nullptr)
            std::fill(mainR, mainR + numSamples, 0.0f);
        return;
    }

    const bool pal = getParameter(Clock) >= 0.5f;
    if (pal != chip.isPal())
        applyRegion(pal);

    const bool wantChannels = channelOutsL != nullptr;
    if (wantChannels && !channelsRendered)
    {
        // Per-channel rendering resumes: restart those synths from the current levels (the
        // synths track changes only, so a held DC level does not produce a step).
        for (int c = 0; c < kNesChannels; ++c)
        {
            channelSynths[static_cast<size_t>(c)].reset();
            channelStages[static_cast<size_t>(c)].reset();
            lastSolo[static_cast<size_t>(c)] = soloLevel(c, lastLevels);
        }
    }
    channelsRendered = wantChannels;

    for (int offset = 0; offset < numSamples; offset += maxBlock)
        renderChunk(mainL, mainR, channelOutsL, channelOutsR, offset, std::min(maxBlock, numSamples - offset));
}

void Nes2A03Engine::renderChunk(float* mainL, float* mainR, float* const* channelOutsL, float* const* channelOutsR,
                                int offset, int n) noexcept
{
    const bool wantChannels = channelOutsL != nullptr;
    const double end = static_cast<double>(n);
    double t = clockTime;

    // One CPU cycle per iteration: driver tick on the video-frame grid, APU clock, then every
    // level change goes to the band-limited step synths at its exact host time.
    while (t < end)
    {
        driverPhase += 2;
        if (driverPhase >= driverPeriod)
        {
            driverPhase -= driverPeriod;
            driver.tick(chip, settings);
        }

        chip.clock();

        const Levels lv = readLevels();
        if (!(lv == lastLevels))
        {
            const float mix = mixer.mix(lv.p1, lv.p2, lv.tri, lv.triUltrasonic, lv.noise, lv.dmc);
            if (mix != lastMix)
            {
                mainSynth.addDelta(t, mix - lastMix);
                lastMix = mix;
            }
            if (wantChannels)
            {
                for (int c = 0; c < kNesChannels; ++c)
                {
                    const float solo = soloLevel(c, lv);
                    float& last = lastSolo[static_cast<size_t>(c)];
                    if (solo != last)
                    {
                        channelSynths[static_cast<size_t>(c)].addDelta(t, solo - last);
                        last = solo;
                    }
                }
            }
            lastLevels = lv;
        }
        t += hostSamplesPerClock;
    }
    clockTime = t - end;

    const bool consoleFilter = getParameter(ConsoleFilter) >= 0.5f;

    float* outL = mainL + offset;
    mainSynth.endBlockReplace(outL, n);
    mainStage.process(outL, n, consoleFilter);
    if (mainR != nullptr && mainR != mainL)
        std::copy(outL, outL + n, mainR + offset);

    if (!wantChannels)
        return;
    for (int c = 0; c < kNesChannels; ++c)
    {
        float* chL = channelOutsL[c];
        float* chR = channelOutsR != nullptr ? channelOutsR[c] : nullptr;
        float* dst = chL != nullptr ? chL + offset : scratch.data();
        channelSynths[static_cast<size_t>(c)].endBlockReplace(dst, n);
        channelStages[static_cast<size_t>(c)].process(dst, n, consoleFilter);
        if (chR != nullptr && chR != chL)
            std::copy(dst, dst + n, chR + offset);
    }
}

} // namespace chipdsp
