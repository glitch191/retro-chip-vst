#pragma once

// Constant tables and small pure functions of the Sega Genesis sound hardware:
// YM2612 (FM) and the SN76489-compatible PSG integrated in the VDP.
// Every value is transcribed from docs/research/genesis.md; the section named in each
// comment is the one to read for the source and the derivation.

#include "chipdsp/ChipTypes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace chipdsp::genesis
{

// ----- Clocks and rates (research "Clocks and rates") ---------------------------------------

constexpr double kMasterClockNtsc = 53693175.0;   // Hz
constexpr double kMasterClockPal = 53203424.0;    // Hz

// YM2612 clock = master / 7; one FM output sample every 144 YM clocks (prescaler 6 x 24 slots).
constexpr int kMasterClocksPerFmSample = 7 * 144;   // 1008
// SN76489 clock = master / 15; tone/noise counters tick at PSG clock / 16.
constexpr int kMasterClocksPerPsgTick = 15 * 16;    // 240
// Video frame (driver tick): 3420 master clocks per line, 262 (NTSC) or 313 (PAL) lines.
constexpr int kMasterClocksPerLine = 3420;
constexpr int kLinesNtsc = 262;
constexpr int kLinesPal = 313;

constexpr double masterClock(ClockStandard s) noexcept
{
    return s == ClockStandard::Pal ? kMasterClockPal : kMasterClockNtsc;
}
constexpr double ymClock(ClockStandard s) noexcept { return masterClock(s) / 7.0; }
constexpr double fmSampleRate(ClockStandard s) noexcept { return masterClock(s) / kMasterClocksPerFmSample; }
constexpr double egRate(ClockStandard s) noexcept { return fmSampleRate(s) / 3.0; }
constexpr double psgClock(ClockStandard s) noexcept { return masterClock(s) / 15.0; }
constexpr double psgTickRate(ClockStandard s) noexcept { return masterClock(s) / kMasterClocksPerPsgTick; }
constexpr int masterClocksPerFrame(ClockStandard s) noexcept
{
    return kMasterClocksPerLine * (s == ClockStandard::Pal ? kLinesPal : kLinesNtsc);
}
constexpr double frameRate(ClockStandard s) noexcept { return masterClock(s) / masterClocksPerFrame(s); }

// ----- Registers (research "Registers") ------------------------------------------------------

// Register offset added to $30..$90 bases: [channel within bank 0..2][operator S1..S4].
constexpr uint8_t kOpRegOffset[3][4] = {
    { 0x0, 0x8, 0x4, 0xC },
    { 0x1, 0x9, 0x5, 0xD },
    { 0x2, 0xA, 0x6, 0xE },
};

// $28 bits 2..0 -> channel 0..5; 3 and 7 are invalid and ignored.
constexpr int kKeyOnChannel[8] = { 0, 1, 2, -1, 3, 4, 5, -1 };
// Inverse, for the driver: channel 0..5 -> $28 channel code.
constexpr uint8_t kKeyOnCode[6] = { 0, 1, 2, 4, 5, 6 };

// ----- Phase generator (research "Frequency (phase generator)") ------------------------------

// Low two bits of the key code, index = fnum bits 10..7 (OPNA p.25 N4/N3 formula).
constexpr uint8_t kKeyCodeLow[16] = { 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 3, 3, 3, 3, 3, 3 };

constexpr int keyCode(int block, int fnum) noexcept
{
    return ((block & 7) << 2) | kKeyCodeLow[(fnum >> 7) & 0xF];
}

// Detune in phase-increment units, [keycode][|DT| 0..3] (OPNA Table 2-6 converted).
constexpr uint8_t kDetuneTable[32][4] = {
    { 0, 0, 1, 2 }, { 0, 0, 1, 2 }, { 0, 0, 1, 2 }, { 0, 0, 1, 2 },
    { 0, 1, 2, 2 }, { 0, 1, 2, 3 }, { 0, 1, 2, 3 }, { 0, 1, 2, 3 },
    { 0, 1, 2, 4 }, { 0, 1, 3, 4 }, { 0, 1, 3, 4 }, { 0, 1, 3, 5 },
    { 0, 2, 4, 5 }, { 0, 2, 4, 6 }, { 0, 2, 4, 6 }, { 0, 2, 5, 7 },
    { 0, 2, 5, 8 }, { 0, 3, 6, 8 }, { 0, 3, 6, 9 }, { 0, 3, 7, 10 },
    { 0, 4, 8, 11 }, { 0, 4, 8, 12 }, { 0, 4, 9, 13 }, { 0, 5, 10, 14 },
    { 0, 5, 11, 16 }, { 0, 6, 12, 17 }, { 0, 6, 13, 19 }, { 0, 7, 14, 20 },
    { 0, 8, 16, 22 }, { 0, 8, 16, 22 }, { 0, 8, 16, 22 }, { 0, 8, 16, 22 },
};

// Signed detune for register value DT 0..7 (bit 2 = subtract; 0 and 4 = none).
constexpr int detuneDelta(int kc, int dt) noexcept
{
    const int magnitude = kDetuneTable[kc & 31][dt & 3];
    return (dt & 4) ? -magnitude : magnitude;
}

// 17-bit increment from a 12-bit fnum (fnum << 1 plus the PM fraction bit) and block, with
// detune, masked to 17 bits (research: "The 17-bit mask matters").
constexpr uint32_t phaseIncrement17(int fnum12, int block, int detune) noexcept
{
    const int base = ((fnum12 & 0xFFF) << (block & 7)) >> 2;
    return static_cast<uint32_t>(base + detune) & 0x1FFFFu;
}

// MUL 0 = x0.5, MUL n = x n; the result wraps at 20 bits.
constexpr uint32_t applyMultiplier(uint32_t inc17, int mul) noexcept
{
    return (mul == 0 ? (inc17 >> 1) : inc17 * static_cast<uint32_t>(mul)) & 0xFFFFFu;
}

// ----- Envelope generator (research "Envelope generator") -------------------------------------

constexpr uint8_t kEgShift[64] = {
    11, 11, 11, 11, 10, 10, 10, 10, 9, 9, 9, 9, 8, 8, 8, 8, 7, 7, 7, 7, 6, 6, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4,
    3, 3, 3, 3, 2, 2, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

constexpr uint8_t kEgIncrement[64][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 0, 1, 0, 1 },
    { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 },
    { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 1, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 1, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 1, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 1, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 1, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 1, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 1, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 1, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 1, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 1, 0, 1, 0, 1, 0, 1 }, { 0, 1, 0, 1, 1, 1, 0, 1 }, { 0, 1, 1, 1, 0, 1, 1, 1 }, { 0, 1, 1, 1, 1, 1, 1, 1 },
    { 1, 1, 1, 1, 1, 1, 1, 1 }, { 1, 1, 1, 2, 1, 1, 1, 2 }, { 1, 2, 1, 2, 1, 2, 1, 2 }, { 1, 2, 2, 2, 1, 2, 2, 2 },
    { 2, 2, 2, 2, 2, 2, 2, 2 }, { 2, 2, 2, 4, 2, 2, 2, 4 }, { 2, 4, 2, 4, 2, 4, 2, 4 }, { 2, 4, 4, 4, 2, 4, 4, 4 },
    { 4, 4, 4, 4, 4, 4, 4, 4 }, { 4, 4, 4, 8, 4, 4, 4, 8 }, { 4, 8, 4, 8, 4, 8, 4, 8 }, { 4, 8, 8, 8, 4, 8, 8, 8 },
    { 8, 8, 8, 8, 8, 8, 8, 8 }, { 8, 8, 8, 8, 8, 8, 8, 8 }, { 8, 8, 8, 8, 8, 8, 8, 8 }, { 8, 8, 8, 8, 8, 8, 8, 8 },
};

constexpr int kEgMaxAttenuation = 0x3FF;
constexpr int kEgSilentAttenuation = 0x340;   // any total >= 13.0 in 4.6 gives output 0

// Effective rate 0..63 from a 5-bit rate R (for release pass RR * 2 + 1), the key code and RS.
constexpr int envelopeRate(int r, int keycode, int rs) noexcept
{
    if (r == 0)
        return 0;
    return std::min(63, 2 * r + (keycode >> (3 - (rs & 3))));
}

// One attack update: att + ((inc * ~att) >> 4), arithmetic shift, floored at 0.
constexpr int attackStep(int att, int inc) noexcept
{
    return std::max(0, att + ((inc * ~att) >> 4));
}

constexpr int totalLevelToAttenuation(int tl) noexcept { return (tl & 0x7F) << 3; }
constexpr int sustainLevelToAttenuation(int sl) noexcept { return (sl & 15) == 15 ? 0x3E0 : (sl & 15) << 5; }

// ----- Operator (research "Operator") ---------------------------------------------------------

// round(-log2(sin((2i + 1) / 512 * pi / 2)) * 256), 4.8 fixed point.
constexpr uint16_t kSinTable[256] = {
    0x859, 0x6C3, 0x607, 0x58B, 0x52E, 0x4E4, 0x4A6, 0x471, 0x443, 0x41A, 0x3F5, 0x3D3, 0x3B5, 0x398, 0x37E, 0x365,
    0x34E, 0x339, 0x324, 0x311, 0x2FF, 0x2ED, 0x2DC, 0x2CD, 0x2BD, 0x2AF, 0x2A0, 0x293, 0x286, 0x279, 0x26D, 0x261,
    0x256, 0x24B, 0x240, 0x236, 0x22C, 0x222, 0x218, 0x20F, 0x206, 0x1FD, 0x1F5, 0x1EC, 0x1E4, 0x1DC, 0x1D4, 0x1CD,
    0x1C5, 0x1BE, 0x1B7, 0x1B0, 0x1A9, 0x1A2, 0x19B, 0x195, 0x18F, 0x188, 0x182, 0x17C, 0x177, 0x171, 0x16B, 0x166,
    0x160, 0x15B, 0x155, 0x150, 0x14B, 0x146, 0x141, 0x13C, 0x137, 0x133, 0x12E, 0x129, 0x125, 0x121, 0x11C, 0x118,
    0x114, 0x10F, 0x10B, 0x107, 0x103, 0x0FF, 0x0FB, 0x0F8, 0x0F4, 0x0F0, 0x0EC, 0x0E9, 0x0E5, 0x0E2, 0x0DE, 0x0DB,
    0x0D7, 0x0D4, 0x0D1, 0x0CD, 0x0CA, 0x0C7, 0x0C4, 0x0C1, 0x0BE, 0x0BB, 0x0B8, 0x0B5, 0x0B2, 0x0AF, 0x0AC, 0x0A9,
    0x0A7, 0x0A4, 0x0A1, 0x09F, 0x09C, 0x099, 0x097, 0x094, 0x092, 0x08F, 0x08D, 0x08A, 0x088, 0x086, 0x083, 0x081,
    0x07F, 0x07D, 0x07A, 0x078, 0x076, 0x074, 0x072, 0x070, 0x06E, 0x06C, 0x06A, 0x068, 0x066, 0x064, 0x062, 0x060,
    0x05E, 0x05C, 0x05B, 0x059, 0x057, 0x055, 0x053, 0x052, 0x050, 0x04E, 0x04D, 0x04B, 0x04A, 0x048, 0x046, 0x045,
    0x043, 0x042, 0x040, 0x03F, 0x03E, 0x03C, 0x03B, 0x039, 0x038, 0x037, 0x035, 0x034, 0x033, 0x031, 0x030, 0x02F,
    0x02E, 0x02D, 0x02B, 0x02A, 0x029, 0x028, 0x027, 0x026, 0x025, 0x024, 0x023, 0x022, 0x021, 0x020, 0x01F, 0x01E,
    0x01D, 0x01C, 0x01B, 0x01A, 0x019, 0x018, 0x017, 0x017, 0x016, 0x015, 0x014, 0x014, 0x013, 0x012, 0x011, 0x011,
    0x010, 0x00F, 0x00F, 0x00E, 0x00D, 0x00D, 0x00C, 0x00C, 0x00B, 0x00A, 0x00A, 0x009, 0x009, 0x008, 0x008, 0x007,
    0x007, 0x007, 0x006, 0x006, 0x005, 0x005, 0x005, 0x004, 0x004, 0x004, 0x003, 0x003, 0x003, 0x002, 0x002, 0x002,
    0x002, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x001, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
};

// round(2^(-(i + 1) / 256) * 2048), 0.11 fixed point.
constexpr uint16_t kExpTable[256] = {
    0x7FA, 0x7F5, 0x7EF, 0x7EA, 0x7E4, 0x7DF, 0x7DA, 0x7D4, 0x7CF, 0x7C9, 0x7C4, 0x7BF, 0x7B9, 0x7B4, 0x7AE, 0x7A9,
    0x7A4, 0x79F, 0x799, 0x794, 0x78F, 0x78A, 0x784, 0x77F, 0x77A, 0x775, 0x770, 0x76A, 0x765, 0x760, 0x75B, 0x756,
    0x751, 0x74C, 0x747, 0x742, 0x73D, 0x738, 0x733, 0x72E, 0x729, 0x724, 0x71F, 0x71A, 0x715, 0x710, 0x70B, 0x706,
    0x702, 0x6FD, 0x6F8, 0x6F3, 0x6EE, 0x6E9, 0x6E5, 0x6E0, 0x6DB, 0x6D6, 0x6D2, 0x6CD, 0x6C8, 0x6C4, 0x6BF, 0x6BA,
    0x6B5, 0x6B1, 0x6AC, 0x6A8, 0x6A3, 0x69E, 0x69A, 0x695, 0x691, 0x68C, 0x688, 0x683, 0x67F, 0x67A, 0x676, 0x671,
    0x66D, 0x668, 0x664, 0x65F, 0x65B, 0x657, 0x652, 0x64E, 0x649, 0x645, 0x641, 0x63C, 0x638, 0x634, 0x630, 0x62B,
    0x627, 0x623, 0x61E, 0x61A, 0x616, 0x612, 0x60E, 0x609, 0x605, 0x601, 0x5FD, 0x5F9, 0x5F5, 0x5F0, 0x5EC, 0x5E8,
    0x5E4, 0x5E0, 0x5DC, 0x5D8, 0x5D4, 0x5D0, 0x5CC, 0x5C8, 0x5C4, 0x5C0, 0x5BC, 0x5B8, 0x5B4, 0x5B0, 0x5AC, 0x5A8,
    0x5A4, 0x5A0, 0x59C, 0x599, 0x595, 0x591, 0x58D, 0x589, 0x585, 0x581, 0x57E, 0x57A, 0x576, 0x572, 0x56F, 0x56B,
    0x567, 0x563, 0x560, 0x55C, 0x558, 0x554, 0x551, 0x54D, 0x549, 0x546, 0x542, 0x53E, 0x53B, 0x537, 0x534, 0x530,
    0x52C, 0x529, 0x525, 0x522, 0x51E, 0x51B, 0x517, 0x514, 0x510, 0x50C, 0x509, 0x506, 0x502, 0x4FF, 0x4FB, 0x4F8,
    0x4F4, 0x4F1, 0x4ED, 0x4EA, 0x4E7, 0x4E3, 0x4E0, 0x4DC, 0x4D9, 0x4D6, 0x4D2, 0x4CF, 0x4CC, 0x4C8, 0x4C5, 0x4C2,
    0x4BE, 0x4BB, 0x4B8, 0x4B5, 0x4B1, 0x4AE, 0x4AB, 0x4A8, 0x4A4, 0x4A1, 0x49E, 0x49B, 0x498, 0x494, 0x491, 0x48E,
    0x48B, 0x488, 0x485, 0x482, 0x47E, 0x47B, 0x478, 0x475, 0x472, 0x46F, 0x46C, 0x469, 0x466, 0x463, 0x460, 0x45D,
    0x45A, 0x457, 0x454, 0x451, 0x44E, 0x44B, 0x448, 0x445, 0x442, 0x43F, 0x43C, 0x439, 0x436, 0x433, 0x430, 0x42D,
    0x42A, 0x428, 0x425, 0x422, 0x41F, 0x41C, 0x419, 0x416, 0x414, 0x411, 0x40E, 0x40B, 0x408, 0x406, 0x403, 0x400,
};

// Operator output, signed 14-bit (-8168..+8168), from the 10-bit phase (modulation already
// added) and the 10-bit EG output (research "Operator evaluation").
constexpr int operatorOutput(int phase10, int eg10) noexcept
{
    const int p = phase10 & 0x3FF;
    const int idx = (p & 0x100) ? (~p & 0xFF) : (p & 0xFF);
    const int total = kSinTable[idx] + ((eg10 & 0x3FF) << 2);
    const int shift = total >> 8;
    const int mag = shift >= 13 ? 0 : (kExpTable[total & 0xFF] << 2) >> shift;
    return (p & 0x200) ? -mag : mag;
}

// Algorithms (research "Algorithms"): modMask[i] bit j set = S(j+1) modulates S(i+1).
struct AlgoDef
{
    uint8_t modMask[4];
    uint8_t carrierMask;
};
constexpr AlgoDef kAlgorithm[8] = {
    { { 0, 0b0001, 0b0010, 0b0100 }, 0b1000 },  // 0: S1->S2->S3->S4
    { { 0, 0, 0b0011, 0b0100 }, 0b1000 },       // 1: (S1+S2)->S3->S4
    { { 0, 0, 0b0010, 0b0101 }, 0b1000 },       // 2: S1->S4, S2->S3->S4
    { { 0, 0b0001, 0, 0b0110 }, 0b1000 },       // 3: S1->S2->S4, S3->S4
    { { 0, 0b0001, 0, 0b0100 }, 0b1010 },       // 4: S1->S2, S3->S4
    { { 0, 0b0001, 0b0001, 0b0001 }, 0b1110 },  // 5: S1->S2, S1->S3, S1->S4
    { { 0, 0b0001, 0, 0 }, 0b1110 },            // 6: S1->S2, S3, S4
    { { 0, 0, 0, 0 }, 0b1111 },                 // 7: four sines
};

// Operators are evaluated in the order S1, S3, S2, S4 with a one-stage pipeline (research
// "Evaluation order quirk"): a modulator evaluated after its target, or immediately before
// it, is seen with its previous-sample output. S1 is read by the other operators through
// its feedback history registers, which adds one more sample to every S1 path (research
// Ambiguity 41). Result, in samples: S1->S2 1, S1->S3 2, S1->S4 1, S2->S3 1, S2->S4 1,
// S3->S4 0.
constexpr int kEvalOrder[4] = { 0, 2, 1, 3 };
constexpr int kEvalPosition[4] = { 0, 2, 1, 3 };   // position of S1..S4 in kEvalOrder
constexpr int modulatorDelay(int target, int modulator) noexcept
{
    const int pipeline = kEvalPosition[target] - kEvalPosition[modulator] < 2 ? 1 : 0;
    return modulator == 0 ? pipeline + 1 : pipeline;
}

// Carrier output truncated to 9 bits and summed with clamping (research "Channel accumulation").
constexpr int carrierTo9Bit(int out14) noexcept { return out14 >> 5; }
constexpr int clampChannel9(int sum) noexcept { return std::clamp(sum, -256, 255); }

// ----- LFO (research "LFO") --------------------------------------------------------------------

constexpr uint8_t kLfoSamplesPerStep[8] = { 108, 77, 71, 67, 62, 44, 8, 5 };

// AM attenuation in 4.6 EG units for the 7-bit LFO counter and AMS 0..3.
constexpr int amAttenuation(int lfo7, int ams) noexcept
{
    const int tri6 = (lfo7 & 0x40) ? (lfo7 & 0x3F) : (0x3F - (lfo7 & 0x3F));
    const int am = tri6 << 1;
    switch (ams & 3)
    {
        case 0: return 0;
        case 1: return am >> 3;
        case 2: return am >> 1;
        default: return am;
    }
}

// [FMS][quarter index]: delta for fnum bit 10 in 12-bit (fnum << 1) units.
constexpr uint8_t kPmTable[8][8] = {
    { 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 4, 4, 4, 4 },
    { 0, 0, 0, 4, 4, 4, 8, 8 },
    { 0, 0, 4, 4, 8, 8, 12, 12 },
    { 0, 0, 4, 8, 8, 8, 12, 16 },
    { 0, 0, 8, 12, 16, 16, 20, 24 },
    { 0, 0, 16, 24, 32, 32, 40, 48 },
    { 0, 0, 32, 48, 64, 64, 80, 96 },
};

// Signed fnum modulation in 12-bit units (research "Phase (frequency) modulation").
constexpr int pmDelta(int fnum11, int fms, int lfo7) noexcept
{
    const int lfoHi = (lfo7 >> 2) & 0x1F;
    const int idx = (lfoHi & 8) ? (7 - (lfoHi & 7)) : (lfoHi & 7);
    const int m = kPmTable[fms & 7][idx];
    int delta = 0;
    for (int i = 4; i <= 10; ++i)
        if ((fnum11 >> i) & 1)
            delta += m >> (10 - i);
    return (lfoHi & 0x10) ? -delta : delta;
}

// Modulated 12-bit fnum (fnum << 1 +/- delta), masked to 12 bits.
constexpr int modulatedFnum12(int fnum11, int fms, int lfo7) noexcept
{
    return ((fnum11 << 1) + pmDelta(fnum11, fms, lfo7)) & 0xFFF;
}

// ----- DAC and output stage (research "DAC", "Output stage, ladder effect") -------------------

// Unsigned 8-bit DAC register -> 9-bit signed sample (top 8 bits of the 9-bit word).
constexpr int dacSample9(uint8_t v) noexcept { return (static_cast<int>(v) - 128) * 2; }

// Contribution of one channel to one output side, in 9-bit steps, aggregated over the four
// internal cycles of its slot. ladder = discrete YM2612 (revision 0), false = YM3438 / ASIC.
constexpr int ladderOutput(int sample9, bool enabled, bool ladder) noexcept
{
    if (!ladder)
        return enabled ? sample9 : 0;
    if (!enabled)
        return sample9 >= 0 ? 4 : -4;
    return sample9 >= 0 ? sample9 + 4 : sample9 - 3;
}

// Full scale of the summed FM output: six channels at 256 nine-bit units.
constexpr double kFmFullScaleUnits = 6.0 * 256.0;
// Engine output scale: FM full scale maps to 1.0.
constexpr float kOutputScale = static_cast<float>(1.0 / kFmFullScaleUnits);

// Console output filters (research "Console low-pass filters and levels").
constexpr double kModel1LowPassHz = 3390.0;   // Model 1 VA0-VA2 first-order RC

// First-order low-pass by the bilinear transform with prewarping: exact -3 dB at fc
// (research generator script, lpf()). y[n] = b0 x[n] + b1 x[n-1] - a1 y[n-1].
struct FirstOrderCoefficients
{
    double b0, b1, a1;
};
inline FirstOrderCoefficients firstOrderLowPass(double cutoffHz, double sampleRate) noexcept
{
    constexpr double kPi = 3.14159265358979323846;
    const double fc = std::min(cutoffHz, 0.49 * sampleRate);
    const double w = std::tan(kPi * fc / sampleRate);
    return { w / (1.0 + w), w / (1.0 + w), (w - 1.0) / (1.0 + w) };
}

// Host-rate model of the analog first-order RC low-pass (research "Implementation decisions",
// "Time base and resampling"). The bilinear form above squeezes the RC response towards the
// host Nyquist (-3.4 dB too low at 15 kHz at 48 kHz). This design keeps the RC pole exactly
// (impulse invariance, p = exp(-2 pi fc / fs)) and chooses two zeros so that |H|^2 equals the
// RC's 1 / (1 + (f / fc)^2) at DC and at 0.70 and 0.95 of min(20 kHz, 0.45 fs) (magnitude
// matching in phi = sin^2(pi f / fs), where both |B|^2 and |A|^2 are polynomials). Measured error
// up to that frequency: < 0.14 dB at 22.05-44.1 kHz, < 0.08 dB at 48 kHz, < 0.001 dB at 96 kHz.
// Only the magnitude is matched; the phase is that of this minimum-phase filter.
// y[n] = b0 x[n] + b1 x[n-1] + b2 x[n-2] - a1 y[n-1]. Falls back to the bilinear form when the
// cutoff is too close to the host Nyquist for the matching points to exist.
struct RcLowPassCoefficients
{
    double b0, b1, b2, a1;
};
inline RcLowPassCoefficients rcLowPass(double cutoffHz, double sampleRate) noexcept
{
    constexpr double kPi = 3.14159265358979323846;
    const FirstOrderCoefficients bilinear = firstOrderLowPass(cutoffHz, sampleRate);
    const RcLowPassCoefficients fallback { bilinear.b0, bilinear.b1, 0.0, bilinear.a1 };

    const double top = std::min(20000.0, 0.45 * sampleRate);
    const double f1 = 0.70 * top;
    const double f2 = 0.95 * top;
    if (cutoffHz <= 0.0 || f1 <= cutoffHz)
        return fallback;

    const double p = std::exp(-2.0 * kPi * cutoffHz / sampleRate);
    auto denominator = [p](double phi) { return (1.0 - p) * (1.0 - p) + 4.0 * p * phi; };   // |1 - p/z|^2
    auto rc = [cutoffHz](double f) { return 1.0 / (1.0 + (f / cutoffHz) * (f / cutoffHz)); };
    auto phiOf = [&](double f) {
        const double s = std::sin(kPi * f / sampleRate);
        return s * s;
    };

    // Numerator |B|^2 = B0 + B1 phi + B2 phi^2 through the three target points.
    const double B0 = denominator(0.0);
    const double x1 = phiOf(f1);
    const double x2 = phiOf(f2);
    const double r1 = (rc(f1) * denominator(x1) - B0) / x1;
    const double r2 = (rc(f2) * denominator(x2) - B0) / x2;
    const double B2 = (r2 - r1) / (x2 - x1);
    const double B1 = r1 - B2 * x1;

    // Back to b0, b1, b2: b0 + b1 + b2 = sqrt(B0), b0 - b1 + b2 = sqrt(|B|^2 at Nyquist),
    // b0 * b2 = B2 / 16 (|B|^2 = (b0+b1+b2)^2 - 4 (b0 b1 + b1 b2 + 4 b0 b2) phi + 16 b0 b2 phi^2).
    const double nyquist = B0 + B1 + B2;
    if (nyquist < 0.0)
        return fallback;
    const double sum = std::sqrt(B0);
    const double alt = std::sqrt(nyquist);
    const double outer = 0.5 * (sum + alt);   // b0 + b2
    const double disc = outer * outer - B2 / 4.0;
    if (disc < 0.0)
        return fallback;
    const double root = std::sqrt(disc);
    return { 0.5 * (outer + root), 0.5 * (sum - alt), 0.5 * (outer - root), -p };
}
// DC-blocking high-pass modelling the output coupling capacitor. Not a documented hardware
// value (see research "Implementation decisions"); it only removes DC from the ladder offsets
// and the unipolar PSG output.
constexpr double kDcBlockHz = 5.0;

// ----- SN76489 (research "SN76489 (Genesis PSG)") ----------------------------------------------

// Maxim, SMS Power: 2 dB per attenuation step, 15 = off (published table, entry 7 kept verbatim).
constexpr int16_t kPsgVolume[16] = {
    32767, 26028, 20675, 16422, 13045, 10362, 8231, 6568,
    5193, 4125, 3277, 2603, 2067, 1642, 1304, 0 };

// Noise counter reload per rate bits; 3 = use tone channel 3's period register.
constexpr uint16_t kNoiseReload[4] = { 0x10, 0x20, 0x40, 0 };

constexpr uint16_t kLfsrReset = 0x8000;       // 16-bit register, reset to the highest bit
constexpr uint16_t kLfsrWhiteTaps = 0x0009;   // bits 0 and 3 (Sega VDP PSG)

// PSG to FM level ratio (research "Output levels and mixing", Ambiguities 17): one PSG channel at
// attenuation 0 (32767) equals one FM channel at full scale (256 nine-bit units) divided by 6.4
// (TmEE, SpritesMind t=1631), i.e. -16.1 dB. Gain per PSG unit, in nine-bit FM units.
constexpr double kPsgToFmGain = 256.0 / 6.4 / 32767.0;

// ----- Driver helpers: MIDI note -> register values --------------------------------------------

struct FmPitch
{
    int block;
    int fnum;
};

// Research "Reference F-number table and MIDI mapping": one block per octave, fnum rounded
// to nearest from the OPNA formula, clamped to 11 bits.
inline FmPitch fmPitchFromNote(double midiNote, ClockStandard s) noexcept
{
    const double f = 440.0 * std::pow(2.0, (midiNote - 69.0) / 12.0);
    const int noteInt = static_cast<int>(std::floor(midiNote + 1e-9));
    const int block = std::clamp(noteInt / 12 - 1, 0, 7);
    const double exact = 144.0 * f * 1048576.0 / ymClock(s) * 2.0 / static_cast<double>(1 << block);
    const int fnum = static_cast<int>(std::clamp(std::lround(exact), 0L, 2047L));
    return { block, fnum };
}

// Frequency produced by (block, fnum) with MUL 1 and no detune.
inline double fmFrequency(int block, int fnum, ClockStandard s) noexcept
{
    return static_cast<double>(fnum) * static_cast<double>(1 << block) / 2.0 / 1048576.0 * fmSampleRate(s);
}

// Tone period = round(clock / (32 f)), 10 bits (research "Tone channels").
inline int psgPeriodFromNote(double midiNote, ClockStandard s) noexcept
{
    const double f = 440.0 * std::pow(2.0, (midiNote - 69.0) / 12.0);
    const long period = std::lround(psgClock(s) / (32.0 * f));
    return static_cast<int>(std::clamp(period, 0L, 1023L));
}

inline double psgToneFrequency(int period, ClockStandard s) noexcept
{
    return period <= 0 ? 0.0 : psgClock(s) / (32.0 * period);
}

} // namespace chipdsp::genesis
