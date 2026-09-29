// BRR decoder and encoder. Reference values: docs/research/snes.md, "Worked examples" and
// "Reference values for unit tests" items 10..15.

#include "chipdsp/snes/BrrCodec.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

using namespace chipdsp::snes;

namespace
{
    std::array<int16_t, 16> decode(const std::array<uint8_t, 9>& block, BrrHistory& h)
    {
        std::array<int16_t, 16> out{};
        brrDecodeBlock(block.data(), h, out.data());
        return out;
    }

    // Data bytes 12 34 56 70 FE DC BA 98 = nibbles 1..7, 0, -1..-8.
    std::array<uint8_t, 9> referenceBlock(uint8_t header)
    {
        return { header, 0x12, 0x34, 0x56, 0x70, 0xFE, 0xDC, 0xBA, 0x98 };
    }

    using Samples = std::array<int16_t, 16>;
} // namespace

TEST_CASE("BRR: filter 0 shift 8 block decodes to nibble * 128", "[snes][brr]")
{
    BrrHistory h;
    const Samples expected = { 128, 256, 384, 512, 640, 768, 896, 0, -128, -256, -384, -512, -640, -768, -896, -1024 };
    REQUIRE(decode(referenceBlock(0x80), h) == expected);
}

TEST_CASE("BRR: filter 1 shift 8 block matches the manual computation", "[snes][brr]")
{
    BrrHistory h;
    const Samples expected = { 128, 376, 736, 1202, 1766, 2423, 3167, 2969, 2655, 2233, 1709, 1090, 381, -411, -1282, -2226 };
    REQUIRE(decode(referenceBlock(0x84), h) == expected);
}

TEST_CASE("BRR: filter 2 shift 8 block matches the manual computation", "[snes][brr]")
{
    BrrHistory h;
    const Samples expected = { 128, 500, 1217, 2362, 4001, 6179, 8923, 11216, 12886, 13792, 13826, 12913, 11013, 8119, 4255, -525 };
    REQUIRE(decode(referenceBlock(0x88), h) == expected);
}

TEST_CASE("BRR: filter 3 shift 8 block matches the manual computation", "[snes][brr]")
{
    BrrHistory h;
    const Samples expected = { 128, 486, 1153, 2188, 3634, 5519, 7859, 9636, 10800, 11320, 11181, 10380, 8926, 6836, 4134, 849 };
    REQUIRE(decode(referenceBlock(0x8C), h) == expected);
}

TEST_CASE("BRR: decoder history carries across blocks", "[snes][brr]")
{
    // Filter-2 block decoded right after the filter-3 block (old = 849, older = 4134).
    BrrHistory h;
    decode(referenceBlock(0x8C), h);
    REQUIRE(h.old == 849);
    REQUIRE(h.older == 4134);
    const Samples expected = { -2130, -4601, -6391, -7358, -7396, -6433, -4434, -2423, -591, 888, 1862, 2204, 1815, 624, -1409, -4295 };
    REQUIRE(decode(referenceBlock(0x88), h) == expected);
}

TEST_CASE("BRR: nibble scaling for valid shifts", "[snes][brr]")
{
    REQUIRE(brrUnpackNibble(7, 12) == 14336);
    REQUIRE(brrUnpackNibble(-8, 12) == -16384);
    REQUIRE(brrUnpackNibble(1, 0) == 0);
    REQUIRE(brrUnpackNibble(7, 0) == 3);
    REQUIRE(brrUnpackNibble(1, 1) == 1);
    REQUIRE(brrUnpackNibble(7, 1) == 7);
}

TEST_CASE("BRR: shifts 13..15 decode to 0 or -2048", "[snes][brr]")
{
    for (int shift = 13; shift <= 15; ++shift)
    {
        for (int n = 0; n <= 7; ++n)
            REQUIRE(brrUnpackNibble(n, shift) == 0);
        for (int n = -8; n <= -1; ++n)
            REQUIRE(brrUnpackNibble(n, shift) == -2048);
    }

    // Whole blocks: header 0xD0 (shift 13, filter 0) with nibbles 7 and -8, and header 0xF0
    // (shift 15) with nibbles 1 and -1.
    BrrHistory h;
    const std::array<uint8_t, 9> d0 = { 0xD0, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78 };
    const Samples out13 = decode(d0, h);
    for (int i = 0; i < 16; i += 2)
    {
        REQUIRE(out13[static_cast<size_t>(i)] == 0);
        REQUIRE(out13[static_cast<size_t>(i + 1)] == -2048);
    }
    const std::array<uint8_t, 9> f0 = { 0xF0, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F };
    const Samples out15 = decode(f0, h);
    for (int i = 0; i < 16; i += 2)
    {
        REQUIRE(out15[static_cast<size_t>(i)] == 0);
        REQUIRE(out15[static_cast<size_t>(i + 1)] == -2048);
    }
}

TEST_CASE("BRR: clamp16 then 15-bit wrap on overflow", "[snes][brr]")
{
    const std::array<uint8_t, 9> ones = { 0xC4, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77 };
    BrrHistory h;
    const Samples f1 = { 14336, -4992, 9656, -9380, 5542, -13237, 1926, 16141, -3300, 11242, -7893, 6936, -11930, 3151, -15478, -175 };
    REQUIRE(decode(ones, h) == f1);

    std::array<uint8_t, 9> twos = ones;
    twos[0] = 0xC8;
    BrrHistory h2;
    const Samples f2 = { 14336, -1, 894, 16040, -1, -704, 12994, -1, 2152, -14330, -14999, -822, -5938, 3786, -5649, 17 };
    REQUIRE(decode(twos, h2) == f2);
}

namespace
{
    // Round trip: every block's squared error is at most what filter 0 with the smallest
    // sufficient shift guarantees: with q = 2^(S-1) and |x| <= 7.5 q, rounding to a multiple
    // of q errs by at most q / 2 per sample (research "BRR decoding": RD = n * 2^(S-1)).
    void checkRoundTrip(const std::vector<int16_t>& pcm)
    {
        const auto brr = brrEncode(pcm.data(), static_cast<int>(pcm.size()), -1, BrrEndCode::EndMute);
        const int blocks = static_cast<int>(pcm.size() + 15) / 16;
        REQUIRE(static_cast<int>(brr.size()) == blocks * 9);

        BrrHistory h;
        double signal = 0.0, noise = 0.0;
        for (int b = 0; b < blocks; ++b)
        {
            int16_t out[16];
            brrDecodeBlock(&brr[static_cast<size_t>(b * 9)], h, out);
            int peak = 0;
            double sse = 0.0;
            for (int i = 0; i < 16; ++i)
            {
                const size_t idx = static_cast<size_t>(b * 16 + i);
                const int x = idx < pcm.size() ? pcm[idx] : 0;
                peak = std::max(peak, std::abs(x));
                const double e = static_cast<double>(out[i]) - x;
                sse += e * e;
                signal += static_cast<double>(x) * x;
                // Decoded values never leave the Gaussian-safe range.
                REQUIRE(out[i] >= kBrrSafeMin);
                REQUIRE(out[i] <= kBrrSafeMax);
            }
            int shift = 1;
            while (shift < 12 && peak > 7.5 * (1 << (shift - 1)))
                ++shift;
            const double q = static_cast<double>(1 << (shift - 1));
            REQUIRE(sse <= 16.0 * (q / 2.0) * (q / 2.0) + 1e-9);
            noise += sse;
        }
        INFO("SNR dB = " << 10.0 * std::log10(signal / std::max(noise, 1.0)));
    }
} // namespace

TEST_CASE("BRR: encode -> decode round trip on sines stays within the filter-0 bound", "[snes][brr]")
{
    for (const double freq : { 110.0, 440.0, 2500.0, 9000.0 })
    {
        std::vector<int16_t> pcm(4000);
        for (size_t i = 0; i < pcm.size(); ++i)
            pcm[i] = static_cast<int16_t>(std::lround(14000.0 * std::sin(2.0 * std::numbers::pi * freq * static_cast<double>(i) / 32000.0)));
        checkRoundTrip(pcm);

        // The prediction filters make low sines far better than the bound: > 40 dB SNR.
        if (freq <= 440.0)
        {
            const auto brr = brrEncode(pcm.data(), static_cast<int>(pcm.size()), -1, BrrEndCode::EndMute);
            BrrHistory h;
            double s = 0.0, n = 0.0;
            for (size_t b = 0; b < brr.size() / 9; ++b)
            {
                int16_t out[16];
                brrDecodeBlock(&brr[b * 9], h, out);
                for (int i = 0; i < 16; ++i)
                {
                    const size_t idx = b * 16 + static_cast<size_t>(i);
                    const double x = idx < pcm.size() ? pcm[idx] : 0.0;
                    s += x * x;
                    n += (out[i] - x) * (out[i] - x);
                }
            }
            REQUIRE(10.0 * std::log10(s / n) > 40.0);
        }
    }
}

TEST_CASE("BRR: encode -> decode round trip on noise stays within the filter-0 bound", "[snes][brr]")
{
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> dist(-14000, 14000);
    std::vector<int16_t> pcm(3001);   // not a multiple of 16: the tail is zero-padded
    for (auto& x : pcm)
        x = static_cast<int16_t>(dist(rng));
    checkRoundTrip(pcm);
}

TEST_CASE("BRR: encoder uses filter 0 on the first and loop blocks and flags the end", "[snes][brr]")
{
    std::vector<int16_t> pcm(16 * 10);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<int16_t>(std::lround(8000.0 * std::sin(2.0 * std::numbers::pi * static_cast<double>(i) / 40.0)));

    const auto looped = brrEncode(pcm.data(), static_cast<int>(pcm.size()), 4, BrrEndCode::EndLoop);
    REQUIRE(looped.size() == 90);
    REQUIRE(((looped[0] >> 2) & 3) == 0);
    REQUIRE(((looped[4 * 9] >> 2) & 3) == 0);
    for (int b = 0; b < 9; ++b)
        REQUIRE((looped[static_cast<size_t>(b * 9)] & 0x03) == 0);   // no end/loop flags before the last block
    REQUIRE((looped[9 * 9] & 0x03) == 0x03);                         // code 3: end + loop

    const auto oneShot = brrEncode(pcm.data(), static_cast<int>(pcm.size()), -1, BrrEndCode::EndMute);
    REQUIRE((oneShot[9 * 9] & 0x03) == 0x01);                        // code 1: end + mute

    // A smooth sine lets later blocks use a prediction filter (the encoder does search them).
    bool usedPrediction = false;
    for (int b = 1; b < 10; ++b)
        usedPrediction = usedPrediction || ((oneShot[static_cast<size_t>(b * 9)] >> 2) & 3) != 0;
    REQUIRE(usedPrediction);
}
