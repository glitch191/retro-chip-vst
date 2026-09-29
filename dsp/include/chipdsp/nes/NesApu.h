#pragma once

// Chip core of the Ricoh 2A03 / 2A07 APU: five channels, frame sequencer, non-linear mixer.
// No notion of MIDI here; everything is driven by register writes ($4000-$4017) and by
// NesApu::clock(), which advances exactly one CPU cycle. Section names in comments refer to
// docs/research/nes.md.

#include "chipdsp/nes/NesTables.h"

#include <cstdint>
#include <vector>

namespace chipdsp::nes
{

// ----- shared units ------------------------------------------------------------------------

// Envelope generator (research "Envelope generator"), used by both pulses and the noise.
struct Envelope
{
    bool start = false;
    bool loop = false;       // also the length counter halt flag
    bool constant = false;   // constant volume flag
    uint8_t param = 0;       // V: constant volume or divider period
    uint8_t divider = 0;
    uint8_t decay = 0;       // decay level 15..0

    void write(uint8_t value) noexcept
    {
        loop = (value & 0x20) != 0;
        constant = (value & 0x10) != 0;
        param = static_cast<uint8_t>(value & 0x0F);
    }

    // Quarter-frame clock.
    void clockQuarter() noexcept
    {
        if (start)
        {
            start = false;
            decay = 15;
            divider = param;
            return;
        }
        if (divider == 0)
        {
            divider = param;
            if (decay > 0)
                --decay;
            else if (loop)
                decay = 15;
        }
        else
        {
            --divider;
        }
    }

    uint8_t output() const noexcept { return constant ? param : decay; }
};

// Length counter (research "Length counter").
struct LengthCounter
{
    uint8_t value = 0;
    bool halt = false;
    bool enabled = false;    // the channel's bit in $4015

    void load(uint8_t index) noexcept
    {
        if (enabled)
            value = kLengthTable[index & 0x1F];
    }
    void setEnabled(bool e) noexcept
    {
        enabled = e;
        if (!e)
            value = 0;
    }
    // Half-frame clock.
    void clockHalf() noexcept
    {
        if (value > 0 && !halt)
            --value;
    }
};

// ----- channels ----------------------------------------------------------------------------

// Pulse channel with sweep unit (research "Pulse channels", "Sweep unit").
// Pulse 1 negates with ones' complement, pulse 2 with two's complement.
class PulseChannel
{
public:
    explicit PulseChannel(bool isPulse1) noexcept : onesComplement(isPulse1) {}

    void reset() noexcept;
    void write(int reg, uint8_t value) noexcept;  // reg 0..3 = $4000..$4003 (or $4004..$4007)
    void setEnabled(bool enabled) noexcept { length.setEnabled(enabled); }

    void clockTimer() noexcept                    // once per APU cycle (every second CPU cycle)
    {
        if (timer == 0)
        {
            timer = period;
            step = static_cast<uint8_t>((step + 1) & 7);
        }
        else
        {
            --timer;
        }
    }
    void clockQuarter() noexcept { envelope.clockQuarter(); }
    void clockHalf() noexcept;

    // Sweep target period, clamped at 0 (research "Sweep unit").
    int targetPeriod() const noexcept
    {
        int change = period >> sweepShift;
        if (sweepNegate)
            change = onesComplement ? -change - 1 : -change;
        const int target = static_cast<int>(period) + change;
        return target < 0 ? 0 : target;
    }
    // Mute rules apply whether or not the sweep is enabled.
    bool isMuted() const noexcept { return period < 8 || targetPeriod() > 0x7FF; }

    // 0..15 into the mixer.
    uint8_t output() const noexcept
    {
        if (kPulseDuty[duty][step] == 0 || length.value == 0 || isMuted())
            return 0;
        return envelope.output();
    }

    uint16_t timerPeriod() const noexcept { return period; }
    uint8_t sequencerStep() const noexcept { return step; }
    uint8_t lengthValue() const noexcept { return length.value; }
    uint8_t volume() const noexcept { return envelope.output(); }
    const Envelope& env() const noexcept { return envelope; }

private:
    bool onesComplement;
    Envelope envelope;
    LengthCounter length;
    uint8_t duty = 0;
    uint16_t period = 0;     // 11-bit timer reload value t
    uint16_t timer = 0;
    uint8_t step = 0;        // index into kPulseDuty (time order)

    bool sweepEnabled = false;
    uint8_t sweepPeriod = 0; // P (divider period P + 1 half frames)
    bool sweepNegate = false;
    uint8_t sweepShift = 0;
    bool sweepReload = false;
    uint8_t sweepDivider = 0;
};

// Triangle channel (research "Triangle channel", ambiguities A2, A3).
class TriangleChannel
{
public:
    void reset() noexcept;
    void write(int reg, uint8_t value) noexcept;  // reg 0..3 = $4008..$400B
    void setEnabled(bool enabled) noexcept { length.setEnabled(enabled); }

    void clockTimer() noexcept                    // once per CPU cycle
    {
        if (timer == 0)
        {
            timer = period;
            if (linear > 0 && length.value > 0)
                step = static_cast<uint8_t>((step + 1) & 31);
        }
        else
        {
            --timer;
        }
    }
    void clockQuarter() noexcept;                 // linear counter
    void clockHalf() noexcept { length.clockHalf(); }

    bool isRunning() const noexcept { return linear > 0 && length.value > 0; }
    // A $400B write set the reload flag: the linear counter loads a non-zero value at the next
    // quarter frame and the channel starts (like the envelope start flag of the pulses).
    bool isStarting() const noexcept { return reloadFlag && reloadValue > 0 && length.value > 0; }
    // Periods 0 and 1 while running: the DAC averages to 7.5 (A2); the mixer evaluates that
    // case directly. Halted: the current step is held, not forced to 0 (A3).
    bool isUltrasonic() const noexcept { return period < 2 && isRunning(); }
    uint8_t output() const noexcept { return kTriangleSequence[step]; }

    uint16_t timerPeriod() const noexcept { return period; }
    uint8_t sequencerStep() const noexcept { return step; }
    uint8_t linearValue() const noexcept { return linear; }
    uint8_t lengthValue() const noexcept { return length.value; }

private:
    LengthCounter length;
    bool control = false;    // linear counter control = length counter halt
    uint8_t reloadValue = 0;
    bool reloadFlag = false;
    uint8_t linear = 0;
    uint16_t period = 0;
    uint16_t timer = 0;
    uint8_t step = 0;
};

// Noise channel (research "Noise channel", ambiguities A4, A5).
class NoiseChannel
{
public:
    void reset() noexcept;
    void setRegion(bool pal) noexcept { periods = pal ? kNoisePeriodPal : kNoisePeriodNtsc; }
    void write(int reg, uint8_t value) noexcept;  // reg 0..3 = $400C..$400F
    void setEnabled(bool enabled) noexcept { length.setEnabled(enabled); }

    void clockTimer() noexcept                    // once per APU cycle; reload = period / 2 - 1
    {
        if (timer == 0)
        {
            timer = static_cast<uint16_t>(periods[periodIndex] / 2 - 1);
            lfsr = noiseStep(lfsr, mode);
        }
        else
        {
            --timer;
        }
    }
    void clockQuarter() noexcept { envelope.clockQuarter(); }
    void clockHalf() noexcept { length.clockHalf(); }

    uint8_t output() const noexcept
    {
        if ((lfsr & 1u) != 0 || length.value == 0)
            return 0;
        return envelope.output();
    }

    uint16_t shiftRegister() const noexcept { return lfsr; }
    uint8_t lengthValue() const noexcept { return length.value; }
    uint8_t volume() const noexcept { return envelope.output(); }
    const Envelope& env() const noexcept { return envelope; }

private:
    const uint16_t* periods = kNoisePeriodNtsc;
    Envelope envelope;
    LengthCounter length;
    bool mode = false;
    uint8_t periodIndex = 0;
    uint16_t timer = 0;
    uint16_t lfsr = 1;       // "On power-up, the shift register is loaded with the value 1"
};

// Delta modulation channel (research "DMC channel", ambiguities A13).
// Sample memory: the engine maps one sample slot at $C000 (like a mapper bank) through
// setMemory(); reads outside the mapped bytes return 0.
class DmcChannel
{
public:
    void reset() noexcept;
    void setRegion(bool pal) noexcept { periods = pal ? kDmcPeriodPal : kDmcPeriodNtsc; }
    void setMemory(const uint8_t* bytesAtC000, int size) noexcept { memory = bytesAtC000; memorySize = size; }
    void write(int reg, uint8_t value) noexcept;  // reg 0..3 = $4010..$4013
    void setEnabled(bool enabled) noexcept;       // $4015 bit 4

    void clockTimer() noexcept                    // once per APU cycle; reload = period / 2 - 1
    {
        if (timer == 0)
        {
            timer = static_cast<uint16_t>(periods[rateIndex] / 2 - 1);
            clockOutput();
        }
        else
        {
            --timer;
        }
    }

    uint8_t output() const noexcept { return level; }
    uint16_t bytesRemainingCount() const noexcept { return bytesRemaining; }
    bool isActive() const noexcept { return bytesRemaining > 0 || bufferFull || !silence; }
    bool loopFlag() const noexcept { return loop; }    // $4010 bit 6 as last written
    bool irqFlag() const noexcept { return irq; }
    void clearIrq() noexcept { irq = false; }
    uint16_t sampleAddress() const noexcept { return address; }
    uint16_t sampleLength() const noexcept { return length; }
    uint16_t currentAddress() const noexcept { return readAddress; }

private:
    void clockOutput() noexcept;
    void fillBuffer() noexcept;
    void restart() noexcept { readAddress = address; bytesRemaining = length; }
    uint8_t read(uint16_t addr) const noexcept
    {
        const int offset = static_cast<int>(addr) - 0xC000;
        return (memory != nullptr && offset >= 0 && offset < memorySize) ? memory[offset] : 0;
    }

    const uint16_t* periods = kDmcPeriodNtsc;
    const uint8_t* memory = nullptr;
    int memorySize = 0;

    bool irqEnable = false;
    bool loop = false;
    uint8_t rateIndex = 0;
    uint16_t address = 0xC000;    // $4012: $C000 + A * 64
    uint16_t length = 1;          // $4013: L * 16 + 1
    bool irq = false;

    uint16_t readAddress = 0xC000;
    uint16_t bytesRemaining = 0;
    uint8_t sampleBuffer = 0;
    bool bufferFull = false;

    uint16_t timer = 0;
    uint8_t shift = 0;
    uint8_t bitsRemaining = 8;
    bool silence = true;
    uint8_t level = 0;            // 7-bit output level
};

// Frame counter ($4017, research "Frame counter", ambiguities A12, A15, A17).
struct FrameClock
{
    bool quarter = false;
    bool half = false;
};

class FrameSequencer
{
public:
    void setRegion(bool pal) noexcept;
    // Immediate sequencer reset with $4017 = value (power-up, engine reset, tests). With bit 7
    // set, the next clock() also delivers the immediate quarter + half clock.
    void reset(uint8_t value) noexcept;
    // CPU write to $4017: the IRQ inhibit flag applies at once (setting it clears the frame
    // interrupt flag); the timer reset and the mode change take effect 3 CPU cycles later (A12,
    // A26).
    void write(uint8_t value) noexcept
    {
        irqInhibit = (value & 0x40) != 0;
        if (irqInhibit)
            irq = false;
        pendingValue = value;
        pendingDelay = 3;
    }

    FrameClock clock() noexcept;                  // one CPU cycle

    bool isFiveStep() const noexcept { return fiveStep; }
    bool irqFlag() const noexcept { return irq; }
    void clearIrq() noexcept { irq = false; }
    uint32_t cycle() const noexcept { return cycleCount; }

private:
    void selectTable() noexcept;

    bool pal = false;
    bool fiveStep = false;
    bool irqInhibit = false;
    bool irq = false;
    bool immediateClock = false;
    bool wrapped = false;         // the 4-step sequence just wrapped: flag set again at cycle 0
    uint8_t pendingValue = 0;
    int pendingDelay = 0;
    uint32_t cycleCount = 0;
    int stepIndex = 0;
    const FrameStep* steps = kFrame4StepNtsc;
    int stepCount = 4;
    uint32_t sequenceLength = kFrame4StepLengthNtsc;
    uint32_t irqFirst = kFrameIrqFirstNtsc;
};

// ----- the APU -----------------------------------------------------------------------------

class NesApu
{
public:
    NesApu() noexcept { reset(); }

    void setRegion(bool pal) noexcept;
    bool isPal() const noexcept { return palRegion; }
    double cpuHz() const noexcept { return palRegion ? kCpuHzPal : kCpuHzNtsc; }

    // Power-up state (research "Registers"): $4000-$4013 = 0, $4015 = 0, $4017 = 0, LFSR = 1.
    void reset() noexcept;

    void write(uint16_t address, uint8_t value) noexcept;
    uint8_t readStatus() noexcept;                // $4015 read (clears the frame interrupt flag)
    uint8_t peekStatus() const noexcept;          // same bits, no side effect

    void clock() noexcept                         // advance one CPU cycle
    {
        const FrameClock fc = frame.clock();
        if (fc.quarter)
        {
            pulse1.clockQuarter();
            pulse2.clockQuarter();
            triangle.clockQuarter();
            noise.clockQuarter();
        }
        if (fc.half)
        {
            pulse1.clockHalf();
            pulse2.clockHalf();
            triangle.clockHalf();
            noise.clockHalf();
        }
        triangle.clockTimer();
        if (apuCycle)
        {
            pulse1.clockTimer();
            pulse2.clockTimer();
            noise.clockTimer();
            dmc.clockTimer();
        }
        apuCycle = !apuCycle;
    }

    PulseChannel pulse1 { true };
    PulseChannel pulse2 { false };
    TriangleChannel triangle;
    NoiseChannel noise;
    DmcChannel dmc;
    FrameSequencer frame;

private:
    bool palRegion = false;
    bool apuCycle = false;        // pulse/noise/DMC timers run on every second CPU cycle
};

// ----- mixer -------------------------------------------------------------------------------

// Exact non-linear mixer (research "Mixer") through precomputed tables:
// pulse table indexed by pulse1 + pulse2 (31 entries), tnd table indexed by
// triangle 0..15, noise 0..15, dmc 0..127 (32768 entries). The ultrasonic triangle value 7.5
// (A2) is not an integer index and is evaluated directly.
class NesMixer
{
public:
    static constexpr int kPulseEntries = 31;
    static constexpr int kTndEntries = 16 * 16 * 128;

    void build();                                 // allocates; message thread (prepare)
    bool isBuilt() const noexcept { return !tndTable.empty(); }

    float pulse(int p1, int p2) const noexcept { return pulseTable[static_cast<size_t>(p1 + p2)]; }
    float tnd(int triangle, int noise, int dmc) const noexcept
    {
        return tndTable[static_cast<size_t>((triangle * 16 + noise) * 128 + dmc)];
    }
    float tndUltrasonic(int noise, int dmc) const noexcept
    {
        return static_cast<float>(mixTnd(7.5, noise, dmc));
    }
    float mix(int p1, int p2, int triangle, bool triangleUltrasonic, int noise, int dmc) const noexcept
    {
        return pulse(p1, p2) + (triangleUltrasonic ? tndUltrasonic(noise, dmc) : tnd(triangle, noise, dmc));
    }

private:
    std::vector<float> pulseTable;
    std::vector<float> tndTable;
};

// ----- output stage (not chip behaviour; research "Output stage", A10, A11) ------------------

// First-order sections with k = exp(-2 pi fc / fs):
//   high-pass y[n] = k * (y[n-1] + x[n] - x[n-1]), low-pass y[n] = (1 - k) * x[n] + k * y[n-1].
// console_filter = 1: the NES-001 chain 90 Hz HP (which is itself the output coupling
// capacitor, research "Output stage"), 440 Hz HP and 14 kHz LP, nothing else.
// console_filter = 0: only the DC blocker required by ARCHITECTURE.md (kDcBlockHz, no hardware
// source, A25) so that the unipolar DAC output is centred.
class NesOutputStage
{
public:
    static constexpr double kDcBlockHz = 5.0; // A25: not a hardware value
    static constexpr double kHighPass1Hz = 90.0;
    static constexpr double kHighPass2Hz = 440.0;
    static constexpr double kLowPassHz = 14000.0;

    static double coefficient(double cutoffHz, double sampleRate) noexcept;

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;
    void process(float* buffer, int numSamples, bool consoleFilter) noexcept;

private:
    struct HighPass
    {
        float k = 0.0f, prevIn = 0.0f, prevOut = 0.0f;
        float process(float x) noexcept
        {
            prevOut = k * (prevOut + x - prevIn);
            prevIn = x;
            return prevOut;
        }
    };
    struct LowPass
    {
        float k = 0.0f, state = 0.0f;
        float process(float x) noexcept
        {
            state = (1.0f - k) * x + k * state;
            return state;
        }
    };

    HighPass dcBlock, hp90, hp440;
    LowPass lp14k;
};

} // namespace chipdsp::nes
