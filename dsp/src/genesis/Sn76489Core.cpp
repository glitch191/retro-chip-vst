#include "chipdsp/genesis/Sn76489Core.h"

#include <bit>

namespace chipdsp::genesis
{

void Sn76489Core::reset() noexcept
{
    for (int i = 0; i < 3; ++i)
    {
        period[i] = 0;
        counter[i] = 0;
        flipFlop[i] = 0;
    }
    for (auto& a : att)
        a = 15;
    noise = 0;
    noiseCounter = 0;
    noiseFlipFlop = 0;
    lfsr = kLfsrReset;
    shifts = 0;
    latchedChannel = 0;
    latchedVolume = false;
}

void Sn76489Core::writeNoise(uint8_t value) noexcept
{
    // Any write to the noise register resets the LFSR (Maxim; datasheet "shift register is cleared").
    noise = static_cast<uint8_t>(value & 7);
    lfsr = kLfsrReset;
}

void Sn76489Core::write(uint8_t value) noexcept
{
    if (value & 0x80)
    {
        latchedChannel = static_cast<uint8_t>((value >> 5) & 3);
        latchedVolume = (value & 0x10) != 0;
        const uint8_t data = static_cast<uint8_t>(value & 0x0F);
        if (latchedVolume)
            att[latchedChannel] = data;
        else if (latchedChannel < 3)
            period[latchedChannel] = static_cast<uint16_t>((period[latchedChannel] & 0x3F0) | data);
        else
            writeNoise(data);
        return;
    }

    // DATA byte: goes to the latched register; tone registers take it as the high 6 bits.
    if (latchedVolume)
        att[latchedChannel] = static_cast<uint8_t>(value & 0x0F);
    else if (latchedChannel < 3)
        period[latchedChannel] = static_cast<uint16_t>((period[latchedChannel] & 0x00F) | ((value & 0x3F) << 4));
    else
        writeNoise(value);
}

void Sn76489Core::clock() noexcept
{
    // Tone counters: decrement if non-zero; on reaching zero reload and toggle (research "Clock and counters").
    for (int i = 0; i < 3; ++i)
    {
        if (counter[i] > 0)
            --counter[i];
        if (counter[i] == 0)
        {
            counter[i] = period[i];
            flipFlop[i] ^= 1;
        }
    }

    // Noise counter: reload 0x10/0x20/0x40 or tone 3's period; the LFSR shifts on 0 -> 1
    // transitions of the noise flip-flop (research "Noise channel").
    if (noiseCounter > 0)
        --noiseCounter;
    if (noiseCounter == 0)
    {
        const int rate = noise & 3;
        noiseCounter = rate == 3 ? period[2] : kNoiseReload[rate];
        noiseFlipFlop ^= 1;
        if (noiseFlipFlop)
        {
            const bool white = (noise & 4) != 0;
            const unsigned feedback = white ? (std::popcount(static_cast<unsigned>(lfsr & kLfsrWhiteTaps)) & 1u)
                                            : (lfsr & 1u);
            lfsr = static_cast<uint16_t>((lfsr >> 1) | (feedback << 15));
            ++shifts;
        }
    }
}

} // namespace chipdsp::genesis
