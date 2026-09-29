#include "chipdsp/nes/NesApu.h"

#include <cmath>
#include <numbers>

namespace chipdsp::nes
{

// ===== PulseChannel ==========================================================================

void PulseChannel::reset() noexcept
{
    const bool p1 = onesComplement;
    *this = PulseChannel(p1);
}

void PulseChannel::write(int reg, uint8_t value) noexcept
{
    switch (reg & 3)
    {
        case 0: // DDLC VVVV
            duty = static_cast<uint8_t>(value >> 6);
            envelope.write(value);
            length.halt = (value & 0x20) != 0;
            break;
        case 1: // EPPP NSSS, sets the sweep reload flag
            sweepEnabled = (value & 0x80) != 0;
            sweepPeriod = static_cast<uint8_t>((value >> 4) & 7);
            sweepNegate = (value & 0x08) != 0;
            sweepShift = static_cast<uint8_t>(value & 7);
            sweepReload = true;
            break;
        case 2: // timer low; the divider is not reset
            period = static_cast<uint16_t>((period & 0x700) | value);
            break;
        case 3: // LLLL LTTT: length load, sequencer restarted, envelope restarted; divider not reset
            period = static_cast<uint16_t>((period & 0x0FF) | ((value & 7) << 8));
            length.load(static_cast<uint8_t>(value >> 3));
            step = 0;
            envelope.start = true;
            break;
        default:
            break;
    }
}

void PulseChannel::clockHalf() noexcept
{
    length.clockHalf();

    // Sweep half-frame procedure, in the documented order (research "Sweep unit", A14).
    if (sweepDivider == 0 && sweepEnabled && sweepShift != 0 && !isMuted())
        period = static_cast<uint16_t>(targetPeriod());
    if (sweepDivider == 0 || sweepReload)
    {
        sweepDivider = sweepPeriod;
        sweepReload = false;
    }
    else
    {
        --sweepDivider;
    }
}

// ===== TriangleChannel =======================================================================

void TriangleChannel::reset() noexcept
{
    *this = TriangleChannel();
}

void TriangleChannel::write(int reg, uint8_t value) noexcept
{
    switch (reg & 3)
    {
        case 0: // CRRR RRRR
            control = (value & 0x80) != 0;
            length.halt = control;
            reloadValue = static_cast<uint8_t>(value & 0x7F);
            break;
        case 2:
            period = static_cast<uint16_t>((period & 0x700) | value);
            break;
        case 3: // LLLL LTTT, sets the linear counter reload flag
            period = static_cast<uint16_t>((period & 0x0FF) | ((value & 7) << 8));
            length.load(static_cast<uint8_t>(value >> 3));
            reloadFlag = true;
            break;
        default: // $4009 unused
            break;
    }
}

void TriangleChannel::clockQuarter() noexcept
{
    if (reloadFlag)
        linear = reloadValue;
    else if (linear > 0)
        --linear;
    if (!control)
        reloadFlag = false;
}

// ===== NoiseChannel ==========================================================================

void NoiseChannel::reset() noexcept
{
    const uint16_t* table = periods;
    *this = NoiseChannel();
    periods = table;
}

void NoiseChannel::write(int reg, uint8_t value) noexcept
{
    switch (reg & 3)
    {
        case 0: // --LC VVVV
            envelope.write(value);
            length.halt = (value & 0x20) != 0;
            break;
        case 2: // M--- PPPP
            mode = (value & 0x80) != 0;
            periodIndex = static_cast<uint8_t>(value & 0x0F);
            break;
        case 3: // LLLL L---: length load, envelope restarted
            length.load(static_cast<uint8_t>(value >> 3));
            envelope.start = true;
            break;
        default: // $400D unused
            break;
    }
}

// ===== DmcChannel ============================================================================

void DmcChannel::reset() noexcept
{
    const uint16_t* table = periods;
    const uint8_t* mem = memory;
    const int size = memorySize;
    *this = DmcChannel();
    periods = table;
    memory = mem;
    memorySize = size;
}

void DmcChannel::write(int reg, uint8_t value) noexcept
{
    switch (reg & 3)
    {
        case 0: // IL-- RRRR
            irqEnable = (value & 0x80) != 0;
            loop = (value & 0x40) != 0;
            rateIndex = static_cast<uint8_t>(value & 0x0F);
            if (!irqEnable)
                irq = false;
            break;
        case 1: // -DDD DDDD: direct load, immediate (the timer-collision glitch A13 is not modelled)
            level = static_cast<uint8_t>(value & 0x7F);
            break;
        case 2: // sample address = $C000 + A * 64
            address = static_cast<uint16_t>(0xC000 + value * 64);
            break;
        case 3: // sample length = L * 16 + 1
            length = static_cast<uint16_t>(value * 16 + 1);
            break;
        default:
            break;
    }
}

void DmcChannel::setEnabled(bool enabled) noexcept
{
    if (!enabled)
    {
        bytesRemaining = 0;   // silences when the buffer empties
        return;
    }
    if (bytesRemaining == 0)
        restart();
    fillBuffer();
}

void DmcChannel::fillBuffer() noexcept
{
    if (bufferFull || bytesRemaining == 0)
        return;
    sampleBuffer = read(readAddress);
    bufferFull = true;
    readAddress = readAddress == 0xFFFF ? static_cast<uint16_t>(0x8000) : static_cast<uint16_t>(readAddress + 1);
    --bytesRemaining;
    if (bytesRemaining == 0)
    {
        if (loop)
            restart();
        else if (irqEnable)
            irq = true;
    }
}

void DmcChannel::clockOutput() noexcept
{
    // Output unit, research "DMC channel": level change, shift, bits-remaining counter.
    if (!silence)
        level = dmcOutputStep(level, (shift & 1u) != 0);
    shift = static_cast<uint8_t>(shift >> 1);
    if (--bitsRemaining == 0)
    {
        bitsRemaining = 8;
        if (!bufferFull)
        {
            silence = true;
        }
        else
        {
            silence = false;
            shift = sampleBuffer;
            bufferFull = false;
            fillBuffer();
        }
    }
}

// ===== FrameSequencer ========================================================================

void FrameSequencer::setRegion(bool isPal) noexcept
{
    pal = isPal;
    selectTable();
}

void FrameSequencer::selectTable() noexcept
{
    if (fiveStep)
    {
        steps = pal ? kFrame5StepPal : kFrame5StepNtsc;
        stepCount = 5;
        sequenceLength = pal ? kFrame5StepLengthPal : kFrame5StepLengthNtsc;
    }
    else
    {
        steps = pal ? kFrame4StepPal : kFrame4StepNtsc;
        stepCount = 4;
        sequenceLength = pal ? kFrame4StepLengthPal : kFrame4StepLengthNtsc;
    }
    irqFirst = pal ? kFrameIrqFirstPal : kFrameIrqFirstNtsc;

    // Keep the position consistent after a region switch in the middle of a sequence.
    if (cycleCount >= sequenceLength)
        cycleCount = 0;
    stepIndex = 0;
    while (stepIndex < stepCount && steps[stepIndex].cpuCycle < cycleCount)
        ++stepIndex;
}

void FrameSequencer::reset(uint8_t value) noexcept
{
    fiveStep = (value & 0x80) != 0;
    irqInhibit = (value & 0x40) != 0;
    if (irqInhibit)
        irq = false;
    immediateClock = fiveStep; // "If the mode flag is set, then both quarter and half frame signals are also generated"
    wrapped = false;
    pendingDelay = 0;
    cycleCount = 0;
    selectTable();
}

FrameClock FrameSequencer::clock() noexcept
{
    if (pendingDelay > 0 && --pendingDelay == 0)
        reset(pendingValue);

    FrameClock out;
    if (immediateClock)
    {
        out.quarter = true;
        out.half = true;
        immediateClock = false;
    }

    if (stepIndex < stepCount && steps[stepIndex].cpuCycle == cycleCount)
    {
        out.quarter = out.quarter || steps[stepIndex].quarter;
        out.half = out.half || steps[stepIndex].half;
        ++stepIndex;
    }

    // 4-step mode: interrupt flag set on the last two cycles (29828, 29829 NTSC) and on cycle 0
    // of the next sequence (29830 = 0), research "Frame counter" mode 0 table.
    if (!fiveStep && !irqInhibit && (cycleCount >= irqFirst || wrapped))
        irq = true;
    wrapped = false;

    if (++cycleCount >= sequenceLength)
    {
        cycleCount = 0;
        stepIndex = 0;
        wrapped = true;
    }
    return out;
}

// ===== NesApu ================================================================================

void NesApu::setRegion(bool pal) noexcept
{
    palRegion = pal;
    noise.setRegion(pal);
    dmc.setRegion(pal);
    frame.setRegion(pal);
}

void NesApu::reset() noexcept
{
    pulse1.reset();
    pulse2.reset();
    triangle.reset();
    noise.reset();
    dmc.reset();
    frame = FrameSequencer();
    frame.reset(0x00);
    setRegion(palRegion);
    apuCycle = false;
}

void NesApu::write(uint16_t address, uint8_t value) noexcept
{
    if (address >= 0x4000 && address <= 0x4003)
        pulse1.write(address - 0x4000, value);
    else if (address >= 0x4004 && address <= 0x4007)
        pulse2.write(address - 0x4004, value);
    else if (address >= 0x4008 && address <= 0x400B)
        triangle.write(address - 0x4008, value);
    else if (address >= 0x400C && address <= 0x400F)
        noise.write(address - 0x400C, value);
    else if (address >= 0x4010 && address <= 0x4013)
        dmc.write(address - 0x4010, value);
    else if (address == 0x4015)
    {
        pulse1.setEnabled((value & 0x01) != 0);
        pulse2.setEnabled((value & 0x02) != 0);
        triangle.setEnabled((value & 0x04) != 0);
        noise.setEnabled((value & 0x08) != 0);
        dmc.clearIrq(); // "Writing to this register clears the DMC interrupt flag."
        dmc.setEnabled((value & 0x10) != 0);
    }
    else if (address == 0x4017)
    {
        frame.write(value);
    }
}

uint8_t NesApu::peekStatus() const noexcept
{
    uint8_t s = 0;
    if (pulse1.lengthValue() > 0) s |= 0x01;
    if (pulse2.lengthValue() > 0) s |= 0x02;
    if (triangle.lengthValue() > 0) s |= 0x04;
    if (noise.lengthValue() > 0) s |= 0x08;
    if (dmc.bytesRemainingCount() > 0) s |= 0x10;
    if (frame.irqFlag()) s |= 0x40;
    if (dmc.irqFlag()) s |= 0x80;
    return s;
}

uint8_t NesApu::readStatus() noexcept
{
    const uint8_t s = peekStatus();
    frame.clearIrq();
    return s;
}

// ===== NesMixer ==============================================================================

void NesMixer::build()
{
    pulseTable.assign(kPulseEntries, 0.0f);
    for (int n = 0; n < kPulseEntries; ++n)
        pulseTable[static_cast<size_t>(n)] = static_cast<float>(mixPulse(n, 0));

    tndTable.assign(kTndEntries, 0.0f);
    for (int t = 0; t < 16; ++t)
        for (int n = 0; n < 16; ++n)
            for (int d = 0; d < 128; ++d)
                tndTable[static_cast<size_t>((t * 16 + n) * 128 + d)] = static_cast<float>(mixTnd(t, n, d));
}

// ===== NesOutputStage ========================================================================

double NesOutputStage::coefficient(double cutoffHz, double sampleRate) noexcept
{
    return std::exp(-2.0 * std::numbers::pi * cutoffHz / sampleRate);
}

void NesOutputStage::prepare(double sampleRate) noexcept
{
    dcBlock.k = static_cast<float>(coefficient(kDcBlockHz, sampleRate));
    hp90.k = static_cast<float>(coefficient(kHighPass1Hz, sampleRate));
    hp440.k = static_cast<float>(coefficient(kHighPass2Hz, sampleRate));
    lp14k.k = static_cast<float>(coefficient(kLowPassHz, sampleRate));
    reset();
}

void NesOutputStage::reset() noexcept
{
    dcBlock.prevIn = dcBlock.prevOut = 0.0f;
    hp90.prevIn = hp90.prevOut = 0.0f;
    hp440.prevIn = hp440.prevOut = 0.0f;
    lp14k.state = 0.0f;
}

void NesOutputStage::process(float* buffer, int numSamples, bool consoleFilter) noexcept
{
    if (consoleFilter)
    {
        for (int i = 0; i < numSamples; ++i)
            buffer[i] = lp14k.process(hp440.process(hp90.process(buffer[i])));
    }
    else
    {
        for (int i = 0; i < numSamples; ++i)
            buffer[i] = dcBlock.process(buffer[i]);
    }
}

} // namespace chipdsp::nes
