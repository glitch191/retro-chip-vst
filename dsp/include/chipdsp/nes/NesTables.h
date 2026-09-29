#pragma once

// Hardware constants of the Ricoh 2A03 (NTSC) / 2A07 (PAL) APU.
// Every table is transcribed from docs/research/nes.md, which cites the NESdev wiki,
// blargg's apu_ref.txt and Brad Taylor's documents for each value.

#include <cmath>
#include <cstdint>

namespace chipdsp::nes
{

// ----- clocks (research "Clocks and rates", ambiguity A1) ------------------------------------
constexpr double kCpuHzNtsc = 1789773.0;              // 2A03, integer clock used by every NESdev table
constexpr double kCpuHzPal = 1662607.0;               // 2A07
constexpr double kCpuHzNtscExact = 19687500.0 / 11.0; // 236250000 / 11 / 12
constexpr double kCpuHzPalExact = 53203425.0 / 32.0;  // 26601712.5 / 16
constexpr double kCpuCyclesPerFrameNtsc = 29780.5;    // CPU cycles per video frame
constexpr double kCpuCyclesPerFramePal = 33247.5;
constexpr double kFrameHzNtsc = 60.098814;            // video frame rate = driver tick rate
constexpr double kFrameHzPal = 50.006979;

// Driver tick period in half CPU cycles (integers: 2 x 29780.5 and 2 x 33247.5).
constexpr uint32_t kFrameHalfCyclesNtsc = 59561;
constexpr uint32_t kFrameHalfCyclesPal = 66495;

// ----- pulse (research "Pulse channels") ---------------------------------------------------
// Duty sequences in time order; index 0 is the output right after a $4003/$4007 write.
constexpr uint8_t kPulseDuty[4][8] = {
    { 0, 1, 0, 0, 0, 0, 0, 0 }, // 0: 12.5 %
    { 0, 1, 1, 0, 0, 0, 0, 0 }, // 1: 25 %
    { 0, 1, 1, 1, 1, 0, 0, 0 }, // 2: 50 %
    { 1, 0, 0, 1, 1, 1, 1, 1 }, // 3: 25 % negated (75 %)
};

// ----- length counter (research "Length counter"), index = bits 7-3 of $4003/$4007/$400B/$400F
// Units: half-frame clocks.
constexpr uint8_t kLengthTable[32] = {
    10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
    12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};

// ----- triangle (research "Triangle channel") ------------------------------------------------
constexpr uint8_t kTriangleSequence[32] = {
    15, 14, 13, 12, 11, 10, 9,  8,  7,  6,  5,  4,  3,  2,  1,  0,
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
};

// ----- noise (research "Noise channel", ambiguity A5): CPU cycles between LFSR clocks ------
constexpr uint16_t kNoisePeriodNtsc[16] = { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };
constexpr uint16_t kNoisePeriodPal[16] = { 4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778 };

// ----- DMC (research "DMC channel", ambiguities A6, A19): CPU cycles per output bit ---------
constexpr uint16_t kDmcPeriodNtsc[16] = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };
constexpr uint16_t kDmcPeriodPal[16] = { 398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50 };

constexpr int kDmcMaxSampleBytes = 4081; // $4013 = $FF -> 255 * 16 + 1

// ----- frame counter (research "Frame counter"): CPU cycles from the sequencer reset -------
struct FrameStep
{
    uint32_t cpuCycle;
    bool quarter;
    bool half;
};

constexpr FrameStep kFrame4StepNtsc[] = { { 7457, true, false }, { 14913, true, true }, { 22371, true, false }, { 29829, true, true } };
constexpr uint32_t kFrame4StepLengthNtsc = 29830;
constexpr FrameStep kFrame4StepPal[] = { { 8313, true, false }, { 16627, true, true }, { 24939, true, false }, { 33253, true, true } };
constexpr uint32_t kFrame4StepLengthPal = 33254;
constexpr FrameStep kFrame5StepNtsc[] = { { 7457, true, false }, { 14913, true, true }, { 22371, true, false }, { 29829, false, false }, { 37281, true, true } };
constexpr uint32_t kFrame5StepLengthNtsc = 37282;
constexpr FrameStep kFrame5StepPal[] = { { 8313, true, false }, { 16627, true, true }, { 24939, true, false }, { 33253, false, false }, { 41565, true, true } };
constexpr uint32_t kFrame5StepLengthPal = 41566;

// 4-step mode sets the frame interrupt flag on the last CPU cycles of the sequence
// (NTSC 29828, 29829 and 0 of the next sequence; PAL 33252, 33253, 0).
constexpr uint32_t kFrameIrqFirstNtsc = 29828;
constexpr uint32_t kFrameIrqFirstPal = 33252;

// ----- mixer (research "Mixer"): exact non-linear formula -----------------------------------
// Output 0.0 .. ~1.0. Zero-division rule: a group whose inputs are all 0 contributes 0.
inline double mixPulse(double p1, double p2) noexcept
{
    const double s = p1 + p2;
    return s == 0.0 ? 0.0 : 95.88 / (8128.0 / s + 100.0);
}

inline double mixTnd(double triangle, double noise, double dmc) noexcept
{
    const double x = triangle / 8227.0 + noise / 12241.0 + dmc / 22638.0;
    return x == 0.0 ? 0.0 : 159.79 / (1.0 / x + 100.0);
}

// ----- MIDI note -> 11-bit timer value (research "Period reference table") ------------------
// pulse t = round(cpu / (16 f)) - 1, triangle t = round(cpu / (32 f)) - 1, f = 440 * 2^((n-69)/12).
// The result is not clamped: values above $7FF are not representable (research A16).
inline int pulsePeriodForNote(double midiNote, double cpuHz) noexcept
{
    const double f = 440.0 * std::pow(2.0, (midiNote - 69.0) / 12.0);
    return static_cast<int>(std::lround(cpuHz / (16.0 * f))) - 1;
}

inline int trianglePeriodForNote(double midiNote, double cpuHz) noexcept
{
    const double f = 440.0 * std::pow(2.0, (midiNote - 69.0) / 12.0);
    return static_cast<int>(std::lround(cpuHz / (32.0 * f))) - 1;
}

// ----- one-step helpers used by the channels and the DMC encoder ---------------------------
// Noise LFSR clock (research "Noise channel"): feedback = bit 0 XOR bit 1 (mode 0) or bit 6
// (mode 1), shift right, feedback into bit 14.
constexpr uint16_t noiseStep(uint16_t lfsr, bool mode) noexcept
{
    const uint16_t feedback = static_cast<uint16_t>((lfsr ^ (lfsr >> (mode ? 6 : 1))) & 1u);
    return static_cast<uint16_t>((lfsr >> 1) | (feedback << 14));
}

// DMC output unit (research "DMC channel"): +/-2, a step that would leave 0..127 is skipped.
constexpr uint8_t dmcOutputStep(uint8_t level, bool bit) noexcept
{
    if (bit)
        return level <= 125 ? static_cast<uint8_t>(level + 2) : level;
    return level >= 2 ? static_cast<uint8_t>(level - 2) : level;
}

} // namespace chipdsp::nes
