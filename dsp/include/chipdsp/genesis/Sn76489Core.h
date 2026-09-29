#pragma once

// Register-level model of the SN76489-compatible PSG integrated in the Genesis VDP.
// Driven by byte writes (68000 $C00011 / Z80 $7F11) and clock(), which advances one
// counter tick (PSG clock / 16 = 223.72 kHz NTSC). Section names refer to
// docs/research/genesis.md ("SN76489 (Genesis PSG)").

#include "chipdsp/genesis/GenesisTables.h"

#include <cstdint>

namespace chipdsp::genesis
{

class Sn76489Core
{
public:
    Sn76489Core() noexcept { reset(); }

    // Sega power-on state: tone/noise registers 0, attenuation 15 (silence), LFSR 0x8000.
    void reset() noexcept;

    // LATCH/DATA byte (%1cctdddd) or DATA byte (%0-DDDDDD), research "Write protocol".
    void write(uint8_t value) noexcept;

    // One counter tick.
    void clock() noexcept;

    // Output of channel 0..3 (3 = noise) as bit * kPsgVolume[attenuation], 0..32767.
    int channelLevel(int ch) const noexcept { return outputBit(ch) ? kPsgVolume[att[ch]] : 0; }
    int mix() const noexcept { return channelLevel(0) + channelLevel(1) + channelLevel(2) + channelLevel(3); }

    // ----- inspection --------------------------------------------------------------------------
    int outputBit(int ch) const noexcept
    {
        if (ch == 3)
            return lfsr & 1;
        // Research "Tone channels": period 0 and 1 output a constant +1 (Ambiguities 20).
        return period[ch] <= 1 ? 1 : flipFlop[ch];
    }
    int tonePeriod(int ch) const noexcept { return period[ch]; }
    int attenuation(int ch) const noexcept { return att[ch]; }
    int noiseControl() const noexcept { return noise; }
    uint16_t shiftRegister() const noexcept { return lfsr; }
    uint32_t noiseShiftCount() const noexcept { return shifts; }

private:
    void writeNoise(uint8_t value) noexcept;

    uint16_t period[3] = { 0, 0, 0 };
    uint16_t counter[3] = { 0, 0, 0 };
    uint8_t flipFlop[3] = { 0, 0, 0 };
    uint8_t att[4] = { 15, 15, 15, 15 };
    uint8_t noise = 0;             // -trr: bit 2 = white, bits 1-0 = rate
    uint16_t noiseCounter = 0;
    uint8_t noiseFlipFlop = 0;
    uint16_t lfsr = kLfsrReset;
    uint32_t shifts = 0;           // LFSR shifts since reset (test/inspection only)
    uint8_t latchedChannel = 0;
    bool latchedVolume = false;
};

} // namespace chipdsp::genesis
