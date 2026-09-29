#pragma once

// BRR (bit rate reduction) sample codec of the S-DSP. See docs/research/snes.md,
// "Sample directory and BRR block format" and "BRR decoding".
//
// A block is 9 bytes: header SSSS FFLE (shift, filter, loop, end) then 16 signed 4-bit
// nibbles, high nibble first. Decoded samples are 15-bit (-16384..16383).

#include <cstdint>
#include <vector>

namespace chipdsp::snes
{

inline constexpr int kBrrBlockBytes = 9;
inline constexpr int kBrrBlockSamples = 16;

// Decoded values an encoder must stay within so the Gaussian interpolator cannot overflow
// (fullsnes, research "BRR decoding").
inline constexpr int kBrrSafeMin = -0x3FFA;
inline constexpr int kBrrSafeMax = 0x3FF8;

// Header flag bits.
inline constexpr uint8_t kBrrFlagEnd = 0x01;
inline constexpr uint8_t kBrrFlagLoop = 0x02;

inline constexpr int32_t clamp16(int32_t v) noexcept { return v > 32767 ? 32767 : (v < -32768 ? -32768 : v); }
inline constexpr int32_t clamp15(int32_t v) noexcept { return v > 16383 ? 16383 : (v < -16384 ? -16384 : v); }
inline constexpr int32_t clip15(int32_t v) noexcept { return ((v & 0x7FFF) ^ 0x4000) - 0x4000; }
inline constexpr int32_t clip16(int32_t v) noexcept { return ((v & 0xFFFF) ^ 0x8000) - 0x8000; }

// Signed nibble (-8..7) scaled by the header shift: RD = (n << S) >> 1 for S <= 12,
// (n >> 3) << 11 for the invalid shifts 13..15 (0 or -2048).
int32_t brrUnpackNibble(int nibble, int shift) noexcept;

// One decoded sample: RD plus the filter prediction from the two previous 15-bit outputs,
// clamped to 16 bits and then wrapped to 15 bits (research "Filters").
int32_t brrDecodeSample(int nibble, int shift, int filter, int32_t old, int32_t older) noexcept;

// Decoder history carried across blocks (the two most recent 15-bit outputs).
struct BrrHistory
{
    int32_t old = 0;
    int32_t older = 0;
};

// Decodes the 16 samples of one 9-byte block, updating the history.
void brrDecodeBlock(const uint8_t* block, BrrHistory& history, int16_t out[kBrrBlockSamples]) noexcept;

// How the last block of an encoded sample is flagged.
enum class BrrEndCode
{
    EndMute,   // code 1 (E=1, L=0): voice released with envelope 0 when the header loads
    EndLoop,   // code 3 (E=1, L=1): jump to the directory loop address and keep playing
};

// Encodes 15-bit PCM (values are clamped to kBrrSafeMin..kBrrSafeMax first). The input is
// padded with zeros to a whole number of blocks. For each block every filter (0..3) and
// shift (0..12) is tried with the real decoder tracking the history, and the combination
// with the smallest squared error that keeps every decoded value in the safe range wins.
// Block 0 and 'loopStartBlock' (if >= 0) are forced to filter 0, whose output does not
// depend on the history (research "Recommended encoder practice"). Message thread only.
std::vector<uint8_t> brrEncode(const int16_t* pcm15, int numSamples, int loopStartBlock, BrrEndCode endCode);

} // namespace chipdsp::snes
