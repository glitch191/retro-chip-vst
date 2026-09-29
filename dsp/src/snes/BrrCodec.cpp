#include "chipdsp/snes/BrrCodec.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace chipdsp::snes
{

int32_t brrUnpackNibble(int nibble, int shift) noexcept
{
    if (shift <= 12)
        return (nibble * (1 << shift)) >> 1;
    // Shifts 13..15: "as if shift = 12 and nibble = nibble SAR 3" (fullsnes, SnesLab).
    return (nibble >> 3) * (1 << 11);
}

int32_t brrDecodeSample(int nibble, int shift, int filter, int32_t old, int32_t older) noexcept
{
    // Exact integer filter forms (Anomie, identical in fullsnes), research "BRR decoding".
    int32_t v = brrUnpackNibble(nibble, shift);
    switch (filter)
    {
        case 1:
            v += old + ((-old) >> 4);
            break;
        case 2:
            v += old * 2 + ((-(old * 3)) >> 5) - older + (older >> 4);
            break;
        case 3:
            v += old * 2 + ((-(old * 13)) >> 6) - older + ((older * 3) >> 4);
            break;
        default:
            break;
    }
    return clip15(clamp16(v));
}

void brrDecodeBlock(const uint8_t* block, BrrHistory& history, int16_t out[kBrrBlockSamples]) noexcept
{
    const int shift = block[0] >> 4;
    const int filter = (block[0] >> 2) & 3;
    for (int i = 0; i < kBrrBlockSamples; ++i)
    {
        const uint8_t byte = block[1 + i / 2];
        const int raw = (i & 1) == 0 ? (byte >> 4) : (byte & 0x0F);
        const int nibble = raw >= 8 ? raw - 16 : raw;
        const int32_t s = brrDecodeSample(nibble, shift, filter, history.old, history.older);
        history.older = history.old;
        history.old = s;
        out[i] = static_cast<int16_t>(s);
    }
}

namespace
{
    struct BlockCandidate
    {
        int64_t error = std::numeric_limits<int64_t>::max();
        int filter = 0;
        int shift = 0;
        int nibbles[kBrrBlockSamples] = {};
    };

    // Squared-error cost of one decoded value; values outside the Gaussian-safe range are
    // priced so high that any in-range candidate wins.
    int64_t sampleCost(int32_t decoded, int32_t target) noexcept
    {
        const int64_t e = static_cast<int64_t>(decoded) - target;
        int64_t cost = e * e;
        if (decoded < kBrrSafeMin || decoded > kBrrSafeMax)
            cost += int64_t{ 1 } << 40;
        return cost;
    }

    void tryCandidate(const int32_t* target, const BrrHistory& start, int filter, int shift, BlockCandidate& best)
    {
        BlockCandidate cand;
        cand.error = 0;
        cand.filter = filter;
        cand.shift = shift;
        int32_t old = start.old;
        int32_t older = start.older;
        const double step = shift >= 1 ? static_cast<double>(1 << (shift - 1)) : 0.5;

        for (int i = 0; i < kBrrBlockSamples; ++i)
        {
            // Prediction alone (nibble 0), then the nibble that best covers the residual;
            // the neighbours are tried too because of the truncating shift and the clamps.
            const int32_t predicted = brrDecodeSample(0, shift, filter, old, older);
            const double ideal = static_cast<double>(target[i] - predicted) / step;
            const int n0 = static_cast<int>(std::lround(std::clamp(ideal, -8.0, 7.0)));

            int bestNibble = n0;
            int32_t bestValue = 0;
            int64_t bestCost = std::numeric_limits<int64_t>::max();
            for (int n = std::max(-8, n0 - 1); n <= std::min(7, n0 + 1); ++n)
            {
                const int32_t decoded = brrDecodeSample(n, shift, filter, old, older);
                const int64_t cost = sampleCost(decoded, target[i]);
                if (cost < bestCost)
                {
                    bestCost = cost;
                    bestNibble = n;
                    bestValue = decoded;
                }
            }
            cand.nibbles[i] = bestNibble;
            cand.error += bestCost;
            older = old;
            old = bestValue;
            if (cand.error >= best.error)
                return; // already worse than the best candidate
        }
        best = cand;
    }
} // namespace

std::vector<uint8_t> brrEncode(const int16_t* pcm15, int numSamples, int loopStartBlock, BrrEndCode endCode)
{
    const int numBlocks = std::max(1, (numSamples + kBrrBlockSamples - 1) / kBrrBlockSamples);
    std::vector<uint8_t> out(static_cast<size_t>(numBlocks * kBrrBlockBytes), 0);

    BrrHistory history;
    for (int b = 0; b < numBlocks; ++b)
    {
        int32_t target[kBrrBlockSamples];
        for (int i = 0; i < kBrrBlockSamples; ++i)
        {
            const int idx = b * kBrrBlockSamples + i;
            const int32_t v = idx < numSamples ? pcm15[idx] : 0;
            target[i] = std::clamp<int32_t>(v, kBrrSafeMin, kBrrSafeMax);
        }

        const bool forceFilter0 = b == 0 || b == loopStartBlock;
        BlockCandidate best;
        for (int filter = 0; filter <= (forceFilter0 ? 0 : 3); ++filter)
            for (int shift = 0; shift <= 12; ++shift)
                tryCandidate(target, history, filter, shift, best);

        uint8_t* block = &out[static_cast<size_t>(b * kBrrBlockBytes)];
        uint8_t header = static_cast<uint8_t>((best.shift << 4) | (best.filter << 2));
        if (b == numBlocks - 1)
            header |= endCode == BrrEndCode::EndLoop ? (kBrrFlagEnd | kBrrFlagLoop) : kBrrFlagEnd;
        block[0] = header;
        for (int i = 0; i < kBrrBlockSamples; i += 2)
        {
            const int hi = best.nibbles[i] & 0x0F;
            const int lo = best.nibbles[i + 1] & 0x0F;
            block[1 + i / 2] = static_cast<uint8_t>((hi << 4) | lo);
        }

        // Track the history with the real decoder, exactly as the S-DSP will see it.
        int16_t decoded[kBrrBlockSamples];
        brrDecodeBlock(block, history, decoded);
    }
    return out;
}

} // namespace chipdsp::snes
