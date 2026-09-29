// Period, rate and length tables against docs/research/nes.md ("Period reference table",
// "Noise channel", "DMC channel", "Length counter", "Reference values for unit tests").
// Reference arrays below are transcribed from the research document, not from the engine.

#include "chipdsp/nes/NesApu.h"
#include "chipdsp/nes/NesTables.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace chipdsp::nes;
using Catch::Approx;

namespace
{
    // research "Period reference table": MIDI 24..108.
    constexpr int16_t kRefPulseNtsc[85] = {
        3419, 3228, 3046, 2875, 2714, 2561, 2418, 2282, 2154, 2033, 1919, 1811,
        1709, 1613, 1523, 1437, 1356, 1280, 1208, 1140, 1076, 1016, 959,  905,
        854,  806,  761,  718,  678,  640,  604,  570,  538,  507,  479,  452,
        427,  403,  380,  359,  338,  319,  301,  284,  268,  253,  239,  225,
        213,  201,  189,  179,  169,  159,  150,  142,  134,  126,  119,  112,
        106,  100,  94,   89,   84,   79,   75,   70,   66,   63,   59,   56,
        52,   49,   47,   44,   41,   39,   37,   35,   33,   31,   29,   27,
        26,
    };
    constexpr int16_t kRefPulsePal[85] = {
        3176, 2998, 2830, 2671, 2521, 2379, 2246, 2120, 2001, 1888, 1782, 1682,
        1588, 1499, 1414, 1335, 1260, 1189, 1122, 1059, 1000, 944,  891,  841,
        793,  749,  707,  667,  629,  594,  561,  529,  499,  471,  445,  420,
        396,  374,  353,  333,  314,  297,  280,  264,  249,  235,  222,  209,
        198,  186,  176,  166,  157,  148,  139,  132,  124,  117,  110,  104,
        98,   93,   87,   82,   78,   73,   69,   65,   62,   58,   55,   52,
        49,   46,   43,   41,   38,   36,   34,   32,   30,   29,   27,   25,
        24,
    };
    constexpr int16_t kRefTriangleNtsc[85] = {
        1709, 1613, 1523, 1437, 1356, 1280, 1208, 1140, 1076, 1016, 959, 905,
        854,  806,  761,  718,  678,  640,  604,  570,  538,  507,  479, 452,
        427,  403,  380,  359,  338,  319,  301,  284,  268,  253,  239, 225,
        213,  201,  189,  179,  169,  159,  150,  142,  134,  126,  119, 112,
        106,  100,  94,   89,   84,   79,   75,   70,   66,   63,   59,  56,
        52,   49,   47,   44,   41,   39,   37,   35,   33,   31,   29,  27,
        26,   24,   23,   21,   20,   19,   18,   17,   16,   15,   14,  13,
        12,
    };
    constexpr int16_t kRefTrianglePal[85] = {
        1588, 1499, 1414, 1335, 1260, 1189, 1122, 1059, 1000, 944, 891, 841,
        793,  749,  707,  667,  629,  594,  561,  529,  499,  471, 445, 420,
        396,  374,  353,  333,  314,  297,  280,  264,  249,  235, 222, 209,
        198,  186,  176,  166,  157,  148,  139,  132,  124,  117, 110, 104,
        98,   93,   87,   82,   78,   73,   69,   65,   62,   58,  55,  52,
        49,   46,   43,   41,   38,   36,   34,   32,   30,   29,  27,  25,
        24,   22,   21,   20,   19,   18,   17,   16,   15,   14,  13,  12,
        11,
    };

    // NESdev "APU basics" published NTSC bytes, MIDI 33..56 (research "Period reference table").
    constexpr int kPublishedNtsc33to56[24] = { 2033, 1919, 1811, 1709, 1613, 1523, 1437, 1356, 1280, 1208, 1140, 1076,
                                               1016, 959,  905,  854,  806,  761,  718,  678,  640,  604,  570,  538 };
} // namespace

TEST_CASE("NES pulse period table NTSC matches the research table for MIDI 24..108", "[nes][tables]")
{
    for (int n = 24; n <= 108; ++n)
    {
        INFO("MIDI " << n);
        REQUIRE(pulsePeriodForNote(n, kCpuHzNtsc) == kRefPulseNtsc[n - 24]);
    }
    for (int n = 33; n <= 56; ++n)
        REQUIRE(pulsePeriodForNote(n, kCpuHzNtsc) == kPublishedNtsc33to56[n - 33]);
    // Item 18 spot values.
    REQUIRE(pulsePeriodForNote(33, kCpuHzNtsc) == 0x07F1);
    REQUIRE(pulsePeriodForNote(45, kCpuHzNtsc) == 0x03F8);
    REQUIRE(pulsePeriodForNote(57, kCpuHzNtsc) == 507);
    REQUIRE(pulsePeriodForNote(69, kCpuHzNtsc) == 253);
    REQUIRE(pulsePeriodForNote(81, kCpuHzNtsc) == 126);
    REQUIRE(pulsePeriodForNote(93, kCpuHzNtsc) == 63);
    REQUIRE(pulsePeriodForNote(105, kCpuHzNtsc) == 31);
    REQUIRE(pulsePeriodForNote(112, kCpuHzNtsc) == 20);
}

TEST_CASE("NES pulse period table PAL matches the research table for MIDI 24..108", "[nes][tables]")
{
    for (int n = 24; n <= 108; ++n)
    {
        INFO("MIDI " << n);
        REQUIRE(pulsePeriodForNote(n, kCpuHzPal) == kRefPulsePal[n - 24]);
    }
    // Item 19 spot values.
    REQUIRE(pulsePeriodForNote(33, kCpuHzPal) == 1888);
    REQUIRE(pulsePeriodForNote(45, kCpuHzPal) == 944);
    REQUIRE(pulsePeriodForNote(57, kCpuHzPal) == 471);
    REQUIRE(pulsePeriodForNote(69, kCpuHzPal) == 235);
    REQUIRE(pulsePeriodForNote(81, kCpuHzPal) == 117);
    REQUIRE(pulsePeriodForNote(93, kCpuHzPal) == 58);
    REQUIRE(pulsePeriodForNote(105, kCpuHzPal) == 29);
}

TEST_CASE("NES triangle period tables NTSC and PAL match the research tables", "[nes][tables]")
{
    for (int n = 24; n <= 108; ++n)
    {
        INFO("MIDI " << n);
        REQUIRE(trianglePeriodForNote(n, kCpuHzNtsc) == kRefTriangleNtsc[n - 24]);
        REQUIRE(trianglePeriodForNote(n, kCpuHzPal) == kRefTrianglePal[n - 24]);
    }
    // The triangle table for MIDI n equals the pulse table for MIDI n + 12.
    for (int n = 24; n <= 96; ++n)
        REQUIRE(kRefTriangleNtsc[n - 24] == kRefPulseNtsc[n + 12 - 24]);
}

TEST_CASE("NES unreachable pulse notes and rounding margins", "[nes][tables]")
{
    // Item 20: NTSC MIDI 24..32 and PAL MIDI 24..31 exceed $7FF.
    for (int n = 24; n <= 32; ++n)
        REQUIRE(pulsePeriodForNote(n, kCpuHzNtsc) > 0x7FF);
    REQUIRE(pulsePeriodForNote(33, kCpuHzNtsc) <= 0x7FF);
    for (int n = 24; n <= 31; ++n)
        REQUIRE(pulsePeriodForNote(n, kCpuHzPal) > 0x7FF);
    REQUIRE(pulsePeriodForNote(32, kCpuHzPal) == 2001);
    REQUIRE(kCpuHzPal / (16.0 * 2002.0) == Approx(51.904564).margin(1e-6));
    // Item 52: notes closest to a rounding tie.
    REQUIRE(pulsePeriodForNote(71, kCpuHzNtsc) == 225);
    REQUIRE(trianglePeriodForNote(59, kCpuHzNtsc) == 225);
    REQUIRE(pulsePeriodForNote(87, kCpuHzPal) == 82);
    REQUIRE(trianglePeriodForNote(75, kCpuHzPal) == 82);
}

namespace
{
    // CPU cycles between two consecutive sequencer wraps (step N-1 -> 0), measured on the chip
    // core, i.e. the length of one waveform period produced by timer value t.
    long long measuredCycles(bool pal, bool triangle, int t)
    {
        NesApu apu;
        apu.setRegion(pal);
        if (triangle)
        {
            apu.write(0x4015, 0x04);
            apu.write(0x4008, 0xFF); // control set: the linear counter holds once loaded
            apu.write(0x400A, static_cast<uint8_t>(t & 0xFF));
            apu.write(0x400B, static_cast<uint8_t>(0x08 | (t >> 8)));
        }
        else
        {
            apu.write(0x4002, static_cast<uint8_t>(t & 0xFF));
            apu.write(0x4003, static_cast<uint8_t>(t >> 8));
        }
        const int last = triangle ? 31 : 7;
        long long wraps[3] = { -1, -1, -1 };
        int found = 0;
        int prev = triangle ? apu.triangle.sequencerStep() : apu.pulse1.sequencerStep();
        for (long long c = 0; c < 1000000 && found < 3; ++c)
        {
            apu.clock();
            const int step = triangle ? apu.triangle.sequencerStep() : apu.pulse1.sequencerStep();
            if (prev == last && step == 0)
                wraps[found++] = c;
            prev = step;
        }
        return found == 3 ? wraps[2] - wraps[1] : -1;
    }
    double pulseHz(double cpu, int t) { return cpu / static_cast<double>(measuredCycles(cpu == kCpuHzPal, false, t)); }
    double triHz(double cpu, int t) { return cpu / static_cast<double>(measuredCycles(cpu == kCpuHzPal, true, t)); }
} // namespace

TEST_CASE("NES pulse and triangle frequencies from the timer value (measured on the chip core)", "[nes][tables]")
{
    // The chip core produces one waveform period in 16 (t + 1) CPU cycles (pulse) and
    // 32 (t + 1) (triangle), research "Pulse channels" / "Triangle channel".
    REQUIRE(measuredCycles(false, false, 253) == 16 * 254);
    REQUIRE(measuredCycles(false, true, 126) == 32 * 127);
    // Items 12-17, 49, 50.
    REQUIRE(pulseHz(kCpuHzNtsc, 0x7FF) == Approx(54.619537).margin(1e-6));
    REQUIRE(triHz(kCpuHzNtsc, 0x7FF) == Approx(27.309769).margin(1e-6));
    REQUIRE(pulseHz(kCpuHzNtsc, 8) == Approx(12428.979167).margin(1e-6));
    REQUIRE(pulseHz(kCpuHzNtsc, 7) == Approx(13982.601562).margin(1e-6));
    REQUIRE(pulseHz(kCpuHzNtsc, 0) == Approx(111860.8125).margin(1e-6));
    REQUIRE(triHz(kCpuHzNtsc, 0) == Approx(55930.40625).margin(1e-6));
    REQUIRE(pulseHz(kCpuHzPal, 0x7FF) == Approx(50.738739).margin(1e-6));
    REQUIRE(pulseHz(kCpuHzPal, 8) == Approx(11545.881944).margin(1e-6));
    REQUIRE(triHz(kCpuHzPal, 0) == Approx(51956.46875).margin(1e-6));
    REQUIRE(pulseHz(kCpuHzNtsc, 253) == Approx(440.3969).margin(1e-4));
    REQUIRE(triHz(kCpuHzNtsc, 126) == Approx(440.3969).margin(1e-4));
    REQUIRE(pulseHz(kCpuHzNtsc, 427) == Approx(261.357039).margin(1e-6));
    REQUIRE(pulseHz(kCpuHzPal, 396) == Approx(261.745435).margin(1e-6));
    REQUIRE(pulseHz(kCpuHzPal, 235) == Approx(440.309057).margin(1e-6));
    REQUIRE(triHz(kCpuHzPal, 117) == Approx(440.309057).margin(1e-6));
    REQUIRE(pulseHz(kCpuHzNtsc, 26) == Approx(4142.993056).margin(1e-6));
}

TEST_CASE("NES noise period tables NTSC and PAL", "[nes][tables]")
{
    constexpr uint16_t refNtsc[16] = { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };
    constexpr uint16_t refPal[16] = { 4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778 };
    for (int i = 0; i < 16; ++i)
    {
        REQUIRE(kNoisePeriodNtsc[i] == refNtsc[i]);
        REQUIRE(kNoisePeriodPal[i] == refPal[i]);
        REQUIRE(kNoisePeriodNtsc[i] % 2 == 0); // APU-clocked: always even
        REQUIRE(kNoisePeriodPal[i] % 2 == 0);
    }
    // blargg's hex table and Brad Taylor's halved values (items 30, 31).
    constexpr uint16_t blargg[16] = { 0x004, 0x008, 0x010, 0x020, 0x040, 0x060, 0x080, 0x0A0,
                                      0x0CA, 0x0FE, 0x17C, 0x1FC, 0x2FA, 0x3F8, 0x7F2, 0xFE4 };
    constexpr uint16_t taylor[16] = { 0x002, 0x004, 0x008, 0x010, 0x020, 0x030, 0x040, 0x050,
                                      0x065, 0x07F, 0x0BE, 0x0FE, 0x17D, 0x1FC, 0x3F9, 0x7F2 };
    for (int i = 0; i < 16; ++i)
    {
        REQUIRE(kNoisePeriodNtsc[i] == blargg[i]);
        REQUIRE(kNoisePeriodNtsc[i] == 2 * taylor[i]);
    }
    REQUIRE(kCpuHzNtsc / kNoisePeriodNtsc[15] == Approx(439.963864).margin(1e-6));
    REQUIRE(kCpuHzPal / kNoisePeriodPal[15] == Approx(440.075966).margin(1e-6));
    REQUIRE(kCpuHzNtsc / kNoisePeriodNtsc[0] == Approx(447443.25).margin(1e-6));
    REQUIRE(kCpuHzNtsc / kNoisePeriodNtsc[8] == Approx(8860.262376).margin(1e-6));
    REQUIRE(kCpuHzPal / kNoisePeriodPal[8] == Approx(8843.654255).margin(1e-6));
    REQUIRE(kCpuHzPal / kNoisePeriodPal[2] == Approx(118757.642857).margin(1e-6));
}

TEST_CASE("NES DMC rate tables NTSC and PAL", "[nes][tables]")
{
    constexpr uint16_t refNtsc[16] = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };
    constexpr uint16_t refPal[16] = { 398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50 };
    constexpr uint16_t blargg[16] = { 0x1AC, 0x17C, 0x154, 0x140, 0x11E, 0x0FE, 0x0E2, 0x0D6,
                                      0x0BE, 0x0A0, 0x08E, 0x080, 0x06A, 0x054, 0x048, 0x036 };
    for (int i = 0; i < 16; ++i)
    {
        REQUIRE(kDmcPeriodNtsc[i] == refNtsc[i]);
        REQUIRE(kDmcPeriodPal[i] == refPal[i]);
        REQUIRE(kDmcPeriodNtsc[i] == blargg[i]);
    }
    // Items 32, 33.
    REQUIRE(kCpuHzNtsc / kDmcPeriodNtsc[0] == Approx(4181.712617).margin(1e-6));
    REQUIRE(kCpuHzNtsc / kDmcPeriodNtsc[15] == Approx(33143.944444).margin(1e-6));
    REQUIRE(kCpuHzPal / kDmcPeriodPal[0] == Approx(4177.404523).margin(1e-6));
    REQUIRE(kCpuHzPal / kDmcPeriodPal[15] == Approx(33252.14).margin(1e-6));
    REQUIRE(kCpuHzNtsc / kDmcPeriodNtsc[8] == Approx(9419.857895).margin(1e-6));
    REQUIRE(kCpuHzPal / kDmcPeriodPal[8] == Approx(9446.630682).margin(1e-6));
    REQUIRE(kCpuHzNtsc / kDmcPeriodNtsc[13] == Approx(21306.821429).margin(1e-6));
}

TEST_CASE("NES length, duty and triangle sequence tables", "[nes][tables]")
{
    constexpr uint8_t ref[32] = { 10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
                                  12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30 };
    for (int i = 0; i < 32; ++i)
        REQUIRE(kLengthTable[i] == ref[i]);
    REQUIRE(254.0 / (2.0 * kCpuHzNtsc / 29830.0) == Approx(2.116699).margin(1e-6)); // item 27

    constexpr uint8_t duty[4][8] = { { 0, 1, 0, 0, 0, 0, 0, 0 }, { 0, 1, 1, 0, 0, 0, 0, 0 },
                                     { 0, 1, 1, 1, 1, 0, 0, 0 }, { 1, 0, 0, 1, 1, 1, 1, 1 } };
    for (int d = 0; d < 4; ++d)
        for (int s = 0; s < 8; ++s)
            REQUIRE(kPulseDuty[d][s] == duty[d][s]);

    for (int s = 0; s < 16; ++s)
    {
        REQUIRE(kTriangleSequence[s] == 15 - s);
        REQUIRE(kTriangleSequence[16 + s] == s);
    }
}

TEST_CASE("NES clocks and frame rates", "[nes][tables]")
{
    // Items 1-8.
    REQUIRE(kCpuHzNtscExact == Approx(1789772.727273).margin(1e-6));
    REQUIRE(kCpuHzPalExact == Approx(1662607.03125).margin(1e-9));
    REQUIRE(kCpuHzNtscExact / kCpuCyclesPerFrameNtsc == Approx(kFrameHzNtsc).margin(1e-6));
    REQUIRE(kCpuHzPalExact / kCpuCyclesPerFramePal == Approx(kFrameHzPal).margin(1e-6));
    REQUIRE(kFrameHalfCyclesNtsc == 2 * 29780.5);
    REQUIRE(kFrameHalfCyclesPal == 2 * 33247.5);
    REQUIRE(kCpuHzNtscExact / kFrame4StepLengthNtsc == Approx(59.999086).margin(1e-6));
    REQUIRE(kCpuHzNtscExact / kFrame5StepLengthNtsc == Approx(48.006350).margin(1e-6));
    REQUIRE(kCpuHzPalExact / kFrame4StepLengthPal == Approx(49.997204).margin(1e-6));
    REQUIRE(kCpuHzPalExact / kFrame5StepLengthPal == Approx(39.999207).margin(1e-6));
    REQUIRE(4.0 * kCpuHzNtscExact / kFrame4StepLengthNtsc == Approx(239.996343).margin(1e-6));
    REQUIRE(2.0 * kCpuHzNtscExact / kFrame4StepLengthNtsc == Approx(119.998171).margin(1e-6));
}
