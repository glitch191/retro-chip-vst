// Genesis tables, clocks and phase generator. Reference values are the numbered items of
// docs/research/genesis.md "Reference values for unit tests" (quoted as "ref N").

#include "chipdsp/genesis/GenesisTables.h"
#include "chipdsp/genesis/Ym2612Core.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <numeric>

using namespace chipdsp;
using namespace chipdsp::genesis;
using Catch::Approx;

namespace
{
    // Set channel ch to (block, fnum) with the documented write order ($A4 then $A0).
    void setFrequency(Ym2612Core& ym, int ch, int block, int fnum)
    {
        const int bank = ch / 3;
        const int cc = ch % 3;
        ym.write(bank, static_cast<uint8_t>(0xA4 + cc), static_cast<uint8_t>((block << 3) | (fnum >> 8)));
        ym.write(bank, static_cast<uint8_t>(0xA0 + cc), static_cast<uint8_t>(fnum & 0xFF));
    }

    void writeOp(Ym2612Core& ym, int ch, int slot, int base, int value)
    {
        ym.write(ch / 3, static_cast<uint8_t>(base + kOpRegOffset[ch % 3][slot]), static_cast<uint8_t>(value));
    }
} // namespace

TEST_CASE("Genesis clocks match the documented values", "[genesis][tables]")
{
    // ref 1-6, 42-43
    CHECK(ymClock(ClockStandard::Ntsc) == Approx(7670453.5714).margin(1e-3));
    CHECK(fmSampleRate(ClockStandard::Ntsc) == Approx(53267.0387).margin(1e-3));
    CHECK(fmSampleRate(ClockStandard::Pal) == Approx(52781.1746).margin(1e-3));
    CHECK(egRate(ClockStandard::Ntsc) == Approx(17755.6796).margin(1e-3));
    CHECK(psgClock(ClockStandard::Ntsc) == Approx(3579545.0).margin(1e-3));
    CHECK(psgTickRate(ClockStandard::Ntsc) == Approx(223721.5625).margin(1e-4));
    CHECK(psgClock(ClockStandard::Pal) == Approx(3546894.9333).margin(1e-3));
    CHECK(psgTickRate(ClockStandard::Pal) == Approx(221680.9333).margin(1e-3));
    CHECK(frameRate(ClockStandard::Ntsc) == Approx(59.92274).margin(1e-5));
    CHECK(frameRate(ClockStandard::Pal) == Approx(49.70146).margin(1e-5));
    CHECK(psgTickRate(ClockStandard::Ntsc) / frameRate(ClockStandard::Ntsc) == Approx(3733.50).margin(1e-3));
    CHECK(fmSampleRate(ClockStandard::Ntsc) / frameRate(ClockStandard::Ntsc) == Approx(888.93).margin(1e-2));
}

TEST_CASE("MIDI note to block/fnum: NTSC octave table", "[genesis][tables][fnum]")
{
    // ref 10: rounded formula values (exact), Echo/plutiedev table within 2 units (Ambiguities 12)
    constexpr int kNtsc[12] = { 644, 682, 723, 766, 811, 859, 910, 965, 1022, 1083, 1147, 1215 };
    constexpr int kEcho[12] = { 644, 681, 722, 765, 810, 858, 910, 964, 1021, 1081, 1146, 1214 };
    for (int i = 0; i < 12; ++i)
    {
        const FmPitch p = fmPitchFromNote(60 + i, ClockStandard::Ntsc);
        INFO("note " << 60 + i);
        CHECK(p.block == 4);
        CHECK(p.fnum == kNtsc[i]);
        CHECK(std::abs(p.fnum - kEcho[i]) <= 2);
    }
}

TEST_CASE("MIDI note to block/fnum: recomputed PAL octave table", "[genesis][tables][fnum]")
{
    // ref 44, ref 9
    constexpr int kPal[12] = { 650, 688, 729, 773, 819, 867, 919, 973, 1031, 1093, 1158, 1226 };
    for (int i = 0; i < 12; ++i)
    {
        const FmPitch p = fmPitchFromNote(60 + i, ClockStandard::Pal);
        INFO("note " << 60 + i);
        CHECK(p.block == 4);
        CHECK(p.fnum == kPal[i]);
    }
}

TEST_CASE("MIDI note to block/fnum across the range and clamping", "[genesis][tables][fnum]")
{
    // research "Reference F-number table and MIDI mapping", sample results
    struct Case { int note, block, fnum; double hz; };
    const Case cases[] = {
        { 12, 0, 644, 16.357 }, { 24, 1, 644, -1 }, { 48, 3, 644, -1 }, { 57, 3, 1083, 220.063 },
        { 60, 4, 644, 261.719 }, { 69, 4, 1083, 440.126 }, { 72, 5, 644, -1 }, { 96, 7, 644, 2093.748 },
        { 107, 7, 1215, 3950.162 }, { 108, 7, 1288, -1 }, { 115, 7, 1929, -1 }, { 116, 7, 2044, 6645.375 },
        { 117, 7, 2047, 6655.129 }, { 127, 7, 2047, 6655.129 },
    };
    for (const auto& c : cases)
    {
        INFO("note " << c.note);
        const FmPitch p = fmPitchFromNote(c.note, ClockStandard::Ntsc);
        CHECK(p.block == c.block);
        CHECK(p.fnum == c.fnum);
        if (c.hz > 0)
            CHECK(fmFrequency(p.block, p.fnum, ClockStandard::Ntsc) == Approx(c.hz).margin(1e-3));
    }
    const FmPitch pal69 = fmPitchFromNote(69, ClockStandard::Pal);
    CHECK(pal69.block == 4);
    CHECK(pal69.fnum == 1093);
}

TEST_CASE("Key code from block and fnum", "[genesis][tables]")
{
    // ref 14
    CHECK(keyCode(4, 0x2A8) == 16);
    CHECK(keyCode(4, 0x400) == 18);
    CHECK(keyCode(4, 0x3C0) == 17);
    CHECK(keyCode(4, 0x4C0) == 19);
    CHECK(keyCode(7, 0x7FF) == 31);
    CHECK(keyCode(0, 0) == 0);
}

TEST_CASE("Detune table", "[genesis][tables][detune]")
{
    // ref 15
    CHECK(detuneDelta(18, 3) == 9);
    CHECK(detuneDelta(18, 7) == -9);
    CHECK(detuneDelta(31, 3) == 22);
    CHECK(detuneDelta(0, 1) == 0);
    CHECK(detuneDelta(20, 0) == 0);
    CHECK(detuneDelta(20, 4) == 0);
    const double hz = 22.0 * ymClock(ClockStandard::Ntsc) / 144.0 / 1048576.0;
    CHECK(hz == Approx(1.1176).margin(1e-4));
    // Row spot checks from the OPNA Table 2-6 conversion
    CHECK(kDetuneTable[5][3] == 3);
    CHECK(kDetuneTable[23][3] == 14);
    CHECK(kDetuneTable[27][3] == 20);
}

TEST_CASE("Detune underflow wraps at 17 bits", "[genesis][tables][detune]")
{
    // ref 16: masking function alone
    CHECK(phaseIncrement17(20, 0, -22) == 0x1FFEFu);   // base (20 << 0) >> 2 = 5

    // register-reachable cases through the core (block 0, MUL 1)
    struct Case { int fnum, dt; uint32_t inc; };
    const Case cases[] = { { 1, 7, 0x1FFFEu }, { 3, 6, 0u }, { 3, 7, 0x1FFFFu } };
    for (const auto& c : cases)
    {
        Ym2612Core ym;
        setFrequency(ym, 0, 0, c.fnum);
        writeOp(ym, 0, 0, 0x30, (c.dt << 4) | 1);
        INFO("fnum " << c.fnum << " dt " << c.dt);
        CHECK(ym.operatorPhaseIncrement(0, 0) == c.inc);
    }
}

TEST_CASE("Phase increment and MUL 0 (x0.5)", "[genesis][tables][phase]")
{
    // ref 13, 16b: block 4, fnum 1083 (kc 18)
    Ym2612Core ym;
    setFrequency(ym, 0, 4, 1083);
    CHECK(ym.channelKeyCode(0) == 18);
    const double fs = fmSampleRate(ClockStandard::Ntsc);

    writeOp(ym, 0, 0, 0x30, 0x00);   // DT 0 MUL 0
    CHECK(ym.operatorPhaseIncrement(0, 0) == 4332u);
    CHECK(4332.0 * fs / 1048576.0 == Approx(220.06).margin(0.01));
    writeOp(ym, 0, 0, 0x30, 0x01);   // MUL 1
    CHECK(ym.operatorPhaseIncrement(0, 0) == 8664u);
    CHECK(8664.0 * fs / 1048576.0 == Approx(440.13).margin(0.01));
    writeOp(ym, 0, 0, 0x30, 0x0F);   // MUL 15
    CHECK(ym.operatorPhaseIncrement(0, 0) == 129960u);
    writeOp(ym, 0, 0, 0x30, 0x31);   // DT 3 MUL 1
    CHECK(ym.operatorPhaseIncrement(0, 0) == 8673u);

    // The phase counter advances by the increment once per FM sample and wraps at 20 bits.
    writeOp(ym, 0, 0, 0x30, 0x01);
    ym.write(0, 0x28, 0x10);   // key on S1 of channel 1 (resets the phase)
    for (int i = 0; i < 10; ++i)
        ym.clockSample();
    CHECK(ym.operatorPhaseCounter(0, 0) == 86640u);
    CHECK(applyMultiplier(0xFFFFF, 15) == ((0xFFFFFu * 15u) & 0xFFFFFu));
}

TEST_CASE("Frequency registers apply on the low byte write", "[genesis][tables][registers]")
{
    // research "Frequency write order": $A4 alone does not change the pitch.
    Ym2612Core ym;
    setFrequency(ym, 4, 3, 500);
    CHECK(ym.channelBlock(4) == 3);
    CHECK(ym.channelFnum(4) == 500);
    ym.write(1, 0xA5, (5 << 3) | 2);
    CHECK(ym.channelBlock(4) == 3);
    CHECK(ym.channelFnum(4) == 500);
    ym.write(1, 0xA1, 0x10);
    CHECK(ym.channelBlock(4) == 5);
    CHECK(ym.channelFnum(4) == 0x210);
}

TEST_CASE("Envelope rate calculation and key scaling", "[genesis][tables][envelope]")
{
    // ref 17, 23b
    CHECK(envelopeRate(31, 31, 0) == 63);
    CHECK(envelopeRate(15, 31, 3) == 61);
    CHECK(envelopeRate(0, 31, 3) == 0);
    CHECK(envelopeRate(15 * 2 + 1, 0, 0) == 62);
    const int rks31[4] = { 3, 7, 15, 31 };
    const int rks18[4] = { 2, 4, 9, 18 };
    for (int rs = 0; rs < 4; ++rs)
    {
        CHECK(envelopeRate(1, 31, rs) == std::min(63, 2 + rks31[rs]));
        CHECK(envelopeRate(1, 18, rs) == 2 + rks18[rs]);
    }
}

TEST_CASE("Envelope update tables", "[genesis][tables][envelope]")
{
    // ref 18
    CHECK(kEgShift[2] == 11);
    CHECK(kEgShift[44] == 0);
    CHECK(kEgShift[43] == 1);
    for (int r = 0; r < 64; ++r)
        CHECK(kEgShift[r] == std::max(0, 11 - r / 4));
    for (int i = 0; i < 8; ++i)
        CHECK(kEgIncrement[63][i] == 8);
    const uint8_t r2[8] = { 0, 1, 0, 1, 0, 1, 0, 1 };
    const uint8_t r49[8] = { 1, 1, 1, 2, 1, 1, 1, 2 };
    for (int i = 0; i < 8; ++i)
    {
        CHECK(kEgIncrement[2][i] == r2[i]);
        CHECK(kEgIncrement[49][i] == r49[i]);
    }
}

TEST_CASE("Attack step formula and update counts", "[genesis][tables][envelope]")
{
    // ref 21
    CHECK(attackStep(0x3FF, 8) == 0x1FF);
    CHECK(attackStep(0x10, 1) == 0x0E);
    CHECK(attackStep(0x3FF, 0) == 0x3FF);
    // ref 21b: GManiac hardware measurement (73 steps at the slowest pattern)
    const int expected[4][2] = { { 1, 73 }, { 2, 40 }, { 4, 21 }, { 8, 10 } };
    for (const auto& e : expected)
    {
        int att = 0x3FF;
        int steps = 0;
        while (att != 0 && steps < 1000)
        {
            att = attackStep(att, e[0]);
            ++steps;
        }
        INFO("inc " << e[0]);
        CHECK(steps == e[1]);
    }
}

TEST_CASE("Total level and sustain level scaling", "[genesis][tables][envelope]")
{
    // ref 22, 23
    CHECK(totalLevelToAttenuation(127) == 1016);
    CHECK(sustainLevelToAttenuation(8) == 0x100);
    CHECK(sustainLevelToAttenuation(15) == 0x3E0);
    CHECK(sustainLevelToAttenuation(14) == 448);
    const double stepDb = 20.0 * std::log10(2.0) / 64.0;
    CHECK(stepDb == Approx(0.09407).margin(1e-5));
    CHECK(0x40 * stepDb == Approx(6.02).margin(0.01));
    CHECK(0x3FF * stepDb == Approx(96.24).margin(0.01));
    CHECK(0x340 * stepDb == Approx(78.27).margin(0.01));
    CHECK(1016 * stepDb == Approx(95.58).margin(0.01));
}

TEST_CASE("Log-sine and exponential tables", "[genesis][tables][operator]")
{
    // ref 24, 25
    CHECK(kSinTable[0] == 0x859);
    CHECK(kSinTable[1] == 0x6C3);
    CHECK(kSinTable[128] == 0x07F);
    CHECK(kSinTable[255] == 0x000);
    CHECK(std::accumulate(std::begin(kSinTable), std::end(kSinTable), 0) == 65406);
    CHECK(kExpTable[0] == 0x7FA);
    CHECK(kExpTable[128] == 0x5A4);
    CHECK(kExpTable[255] == 0x400);
    CHECK(std::accumulate(std::begin(kExpTable), std::end(kExpTable), 0) == 377687);
}

TEST_CASE("Operator output from phase and attenuation", "[genesis][tables][operator]")
{
    // ref 26
    CHECK(operatorOutput(0x100, 0) == 8168);
    CHECK(operatorOutput(0x300, 0) == -8168);
    CHECK(operatorOutput(0x100, 0x40) == 4084);
    CHECK(operatorOutput(0x100, 0x33F) == 1);
    CHECK(operatorOutput(0x100, 0x340) == 0);
    CHECK(operatorOutput(0, 0) == 25);
    CHECK(operatorOutput(0x0AB, 0x123) == 303);
    CHECK(operatorOutput(0x2AB, 0x123) == -303);
    // Full-scale feedback shift (ref 27): two outputs of 8168 at FB 7 -> 0x3FA phase units.
    CHECK((((8168 + 8168) >> (10 - 7)) & 0x3FF) == 0x3FA);
}

TEST_CASE("9-bit carrier truncation and channel clamp", "[genesis][tables][dac]")
{
    // ref 28
    CHECK(carrierTo9Bit(8168) == 255);
    CHECK(carrierTo9Bit(-8168) == -256);
    CHECK(clampChannel9(4 * 255) == 255);
    CHECK(clampChannel9(-4 * 256) == -256);
    CHECK(clampChannel9(-3) == -3);
}

TEST_CASE("Algorithm carriers", "[genesis][tables][algorithm]")
{
    // ref 29 (bit s = operator S(s+1))
    const uint8_t expected[8] = { 0b1000, 0b1000, 0b1000, 0b1000, 0b1010, 0b1110, 0b1110, 0b1111 };
    for (int a = 0; a < 8; ++a)
        CHECK(kAlgorithm[a].carrierMask == expected[a]);
    // Pipeline-delayed modulator paths (research "Evaluation order quirk")
    CHECK(modulatorIsDelayed(2, 1));    // S2 -> S3
    CHECK(modulatorIsDelayed(2, 0));    // S1 -> S3
    CHECK(modulatorIsDelayed(3, 1));    // S2 -> S4
    CHECK_FALSE(modulatorIsDelayed(1, 0));   // S1 -> S2
    CHECK_FALSE(modulatorIsDelayed(3, 2));   // S3 -> S4
    CHECK_FALSE(modulatorIsDelayed(3, 0));   // S1 -> S4
}

TEST_CASE("DAC value and ladder offsets", "[genesis][tables][dac]")
{
    // ref 33
    CHECK(dacSample9(0x00) == -256);
    CHECK(dacSample9(0x80) == 0);
    CHECK(dacSample9(0xFF) == 254);
    CHECK(dacSample9(0x7F) == -2);
    // ref 34
    CHECK(ladderOutput(0, true, true) == 4);
    CHECK(ladderOutput(-1, true, true) == -4);
    CHECK(ladderOutput(0, true, true) - ladderOutput(-1, true, true) == 8);
    CHECK(ladderOutput(100, false, true) == 4);
    CHECK(ladderOutput(-100, false, true) == -4);
    CHECK(ladderOutput(255, true, true) == 259);
    CHECK(ladderOutput(-256, true, true) == -259);
    CHECK(ladderOutput(0, true, false) == 0);
    CHECK(ladderOutput(-1, true, false) == -1);
    CHECK(ladderOutput(100, false, false) == 0);
}

TEST_CASE("LFO divider frequencies", "[genesis][tables][lfo]")
{
    // ref 30 (table level; the measured counter is tested in test_genesis_lfo.cpp)
    const double ntsc[8] = { 3.853, 5.405, 5.861, 6.211, 6.712, 9.458, 52.019, 83.230 };
    const double pal[8] = { 3.818, 5.355, 5.808, 6.155, 6.651, 9.372, 51.544, 82.471 };
    for (int i = 0; i < 8; ++i)
    {
        CHECK(fmSampleRate(ClockStandard::Ntsc) / 128.0 / kLfoSamplesPerStep[i] == Approx(ntsc[i]).margin(1e-3));
        CHECK(fmSampleRate(ClockStandard::Pal) / 128.0 / kLfoSamplesPerStep[i] == Approx(pal[i]).margin(1e-3));
    }
}

TEST_CASE("AM depth maxima in dB", "[genesis][tables][lfo]")
{
    // ref 31
    const int expectedMax[4] = { 0, 15, 63, 126 };
    const double expectedDb[4] = { 0.0, 1.41, 5.93, 11.85 };
    const double stepDb = 20.0 * std::log10(2.0) / 64.0;
    for (int ams = 0; ams < 4; ++ams)
    {
        int mx = 0, mn = 1000;
        for (int l = 0; l < 128; ++l)
        {
            mx = std::max(mx, amAttenuation(l, ams));
            mn = std::min(mn, amAttenuation(l, ams));
        }
        CHECK(mx == expectedMax[ams]);
        CHECK(mn == 0);
        CHECK(mx * stepDb == Approx(expectedDb[ams]).margin(0.01));
    }
}

TEST_CASE("PM depth maxima in cents", "[genesis][tables][lfo]")
{
    // ref 32
    const int expected[8] = { 0, 4, 8, 12, 16, 24, 48, 96 };
    const double cents[8] = { 0, 3.38, 6.75, 10.11, 13.47, 20.17, 40.11, 79.31 };
    for (int fms = 0; fms < 8; ++fms)
    {
        int mx = 0, mn = 0;
        for (int l = 0; l < 128; ++l)
        {
            mx = std::max(mx, pmDelta(0x400, fms, l));
            mn = std::min(mn, pmDelta(0x400, fms, l));
        }
        CHECK(mx == expected[fms]);
        CHECK(mn == -expected[fms]);
        CHECK(1200.0 * std::log2((2048.0 + mx) / 2048.0) == Approx(cents[fms]).margin(0.01));
    }
    int m2a8 = 0, m7ff = 0;
    for (int l = 0; l < 128; ++l)
    {
        m2a8 = std::max(m2a8, pmDelta(0x2A8, 7, l));
        m7ff = std::max(m7ff, pmDelta(0x7FF, 7, l));
    }
    CHECK(m2a8 == 63);
    CHECK(m7ff == 190);
}

TEST_CASE("PSG attenuation table: 2 dB steps, 15 = silence", "[genesis][tables][psg]")
{
    // ref 38 (Maxim's published table; entry 7 deviates by 0.04 dB, Ambiguities 18)
    CHECK(kPsgVolume[0] == 32767);
    CHECK(kPsgVolume[1] == 26028);
    CHECK(kPsgVolume[6] == 8231);
    CHECK(kPsgVolume[14] == 1304);
    CHECK(kPsgVolume[15] == 0);
    for (int a = 1; a < 15; ++a)
    {
        const double db = 20.0 * std::log10(static_cast<double>(kPsgVolume[a]) / kPsgVolume[a - 1]);
        INFO("step " << a);
        CHECK(db == Approx(-2.0).margin(0.05));
        const double fromTop = 20.0 * std::log10(static_cast<double>(kPsgVolume[a]) / kPsgVolume[0]);
        CHECK(fromTop == Approx(-2.0 * a).margin(0.05));
    }
}

TEST_CASE("PSG tone period from MIDI note and tone frequency", "[genesis][tables][psg]")
{
    // ref 36, 37
    CHECK(psgPeriodFromNote(69, ClockStandard::Ntsc) == 254);
    CHECK(psgPeriodFromNote(60, ClockStandard::Ntsc) == 428);
    CHECK(psgPeriodFromNote(45, ClockStandard::Ntsc) == 1017);
    CHECK(psgPeriodFromNote(96, ClockStandard::Ntsc) == 53);
    CHECK(psgPeriodFromNote(20, ClockStandard::Ntsc) == 1023);   // below the range: clamped to 10 bits
    CHECK(psgToneFrequency(0xFE, ClockStandard::Ntsc) == Approx(440.397).margin(1e-3));
    CHECK(psgToneFrequency(0x3FF, ClockStandard::Ntsc) == Approx(109.35).margin(0.01));
    CHECK(psgToneFrequency(1, ClockStandard::Ntsc) == Approx(111860.8).margin(0.1));
    CHECK(psgToneFrequency(2, ClockStandard::Ntsc) == Approx(55930.4).margin(0.1));
    CHECK(psgToneFrequency(0xFE, ClockStandard::Pal) == Approx(436.38).margin(0.01));
}

TEST_CASE("PSG to FM mix constant", "[genesis][tables][psg]")
{
    // Ambiguities 17: PSG channel full scale = FM channel full scale / 6.4 (-16.1 dB)
    const double psgFull = 32767.0 * kPsgToFmGain;
    CHECK(psgFull == Approx(40.0).margin(1e-9));
    CHECK(20.0 * std::log10(psgFull / 256.0) == Approx(-16.12).margin(0.01));
}

TEST_CASE("Model 1 low-pass coefficients", "[genesis][tables][filter]")
{
    // ref 35 and research "Console low-pass filters and levels" (bilinear, jsgroth's scipy values)
    const FirstOrderCoefficients a = firstOrderLowPass(3390.0, 53267.04);
    CHECK(a.b0 == Approx(0.1684983368).margin(1e-8));
    CHECK(a.b1 == Approx(0.1684983368).margin(1e-8));
    CHECK(a.a1 == Approx(-0.6630033263).margin(1e-8));
    const FirstOrderCoefficients b = firstOrderLowPass(2840.0, 53267.04);
    CHECK(b.b0 == Approx(0.1446281622).margin(1e-8));
    CHECK(b.a1 == Approx(-0.7107436755).margin(1e-8));
    const FirstOrderCoefficients c = firstOrderLowPass(3390.0, 223721.56);
    CHECK(c.b0 == Approx(0.0454734564).margin(1e-8));
    CHECK(c.a1 == Approx(-0.9090530873).margin(1e-8));
}