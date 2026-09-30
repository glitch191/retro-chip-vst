# Sega Genesis YM2612 + SN76489 research

Self-contained specification for `Ym2612Core` and `Sn76489Core` (see
`docs/ENGINE_SPECS.md`). Everything below comes from public documentation, forum test
reports and datasheets listed in the Sources section (all consulted 2026-09-28). No
emulator source code was read. Where a value was computed rather than transcribed, the
formula and the Python that produced it are shown; the generator script used for every
computed table is reproduced in the section "Generator script" at the end.

Conventions:

* `S1..S4` are operators in Yamaha slot order (register order is S1, S3, S2, S4).
* `fs` is the YM2612 sample rate (master / 7 / 144), one FM sample per 24 internal
  cycles. `EG cycle` = one envelope generator update period = 3 FM samples.
* Hex values are written as in the source. Tables are C++-ready `constexpr` arrays.
* `[unverified]` marks a value taken from memory or from a single unconfirmed post.

## Clocks and rates

Master clock (Charles MacDonald gen-hw.txt gives the NTSC divisors; exact PAL and NTSC
master clock values from Eke's post on Nemesis thread page 8 and jsgroth part 1):

```cpp
constexpr double kMasterClockNtsc = 53693175.0;   // Hz
constexpr double kMasterClockPal  = 53203424.0;   // Hz
// YM2612 clock = master / 7 (the 68000 clock), FM sample rate = YM clock / 144
constexpr double kYmClockNtsc     = 53693175.0 / 7.0;        // 7670453.5714 Hz
constexpr double kYmClockPal      = 53203424.0 / 7.0;        // 7600489.1429 Hz
constexpr double kFmSampleRateNtsc = kYmClockNtsc / 144.0;   // 53267.0387 Hz
constexpr double kFmSampleRatePal  = kYmClockPal  / 144.0;   // 52781.1746 Hz
// Envelope generator: one update every 3 FM samples (72 internal cycles)
constexpr double kEgRateNtsc = kFmSampleRateNtsc / 3.0;      // 17755.6796 Hz
constexpr double kEgRatePal  = kFmSampleRatePal  / 3.0;      // 17593.7249 Hz
// SN76489 (VDP-integrated): clock = master / 15, counters decrement at clock / 16
constexpr double kPsgClockNtsc   = 53693175.0 / 15.0;        // 3579545.0000 Hz
constexpr double kPsgClockPal    = 53203424.0 / 15.0;        // 3546894.9333 Hz
constexpr double kPsgCounterNtsc = kPsgClockNtsc / 16.0;     // 223721.5625 Hz
constexpr double kPsgCounterPal  = kPsgClockPal  / 16.0;     // 221680.9333 Hz
// Video frame (driver tick): 3420 master clocks per line, 262 lines NTSC / 313 lines PAL
constexpr double kFrameRateNtsc = 53693175.0 / (3420.0 * 262.0);   // 59.92274 Hz
constexpr double kFrameRatePal  = 53203424.0 / (3420.0 * 313.0);   // 49.70146 Hz
```

Verified 2026-09-28: every value above recomputed in Python from the two master clocks.
The frame rates match the published 59.92274 Hz / 49.701459 Hz (Sega Retro technical
specifications) and the 3420 master clocks per line (RadDad772, "Genesis VDP internals,
part three"). They are not the NES rates 60.0988 / 50.0070 Hz that `docs/ENGINE_SPECS.md`
lists for the driver tick; see Ambiguities 28.

Derivation of the 144: the YM2612 prescaler divides the input clock by 6 (fixed on the
YM2612; the OPNA manual Table 2-1 lists 1/6 as the default FM prescaler) and one output
sample takes 24 internal cycles (6 channels x 4 operators, one operator per cycle);
6 x 24 = 144 (Nemesis, thread page 8; jsgroth part 1). Nemesis's original post used
7610000 Hz for PAL; Eke corrected it in the same page to the exact 53203424/7, giving
52781.17 Hz.

Timing of internal blocks (jsgroth parts 2, 3, 6):

* Phase generators: once per FM sample.
* Envelope generators: once per 3 FM samples (but the SSG-EG state logic runs every
  sample, see SSG-EG).
* LFO: a divider counted in FM samples (table in the LFO section).
* Timer A: 1 tick per FM sample; Timer B: 1 tick per 16 FM samples (not needed here).

## Registers

Two register banks: bank 0 (Z80 $4000 address / $4001 data, 68000 $A04000/$A04001) for
global registers and channels 1-3; bank 1 ($4002/$4003) for channels 4-6. Global
registers exist only in bank 0. The chip actually has a single data port; a write to
$4000 or $4002 selects which bank the next data write goes to (jsgroth part 1). Register
$30..$9E: the low two address bits select the channel (0,1,2; 3 is unused) and bits 2-3
select the operator in the order S1, S3, S2, S4 (OPNA manual Table 2-2; plutiedev).

Register map (plutiedev register reference, OPNA manual Table 2-2, Maxim's SEGA2.DOC
transcription; bit layouts transcribed):

| Reg | Bits (7..0) | Meaning |
|---|---|---|
| $22 | `0 0 0 0 LFOEN LFO2 LFO1 LFO0` | LFO enable (bit 3) and 3-bit LFO frequency |
| $24 | `TMRA9..TMRA2` | Timer A high 8 bits (not needed) |
| $25 | `0 0 0 0 0 0 TMRA1 TMRA0` | Timer A low 2 bits (not needed) |
| $26 | `TMRB7..TMRB0` | Timer B (not needed) |
| $27 | `MODE1 MODE0 RSTB RSTA ENB ENA LOADB LOADA` | Ch3 mode (00 normal, 01 special, 10 CSM) and timer control (not needed) |
| $28 | `OP4 OP3 OP2 OP1 0 CH2 CH1 CH0` | Key on/off, see below |
| $2A | `D7..D0` | DAC data, unsigned 8-bit |
| $2B | `DACEN 0 0 0 0 0 0 0` | DAC enable (bit 7): 1 = DAC replaces channel 6 |
| $30+ | `0 DT2 DT1 DT0 MUL3 MUL2 MUL1 MUL0` | Detune and multiplier |
| $40+ | `0 TL6..TL0` | Total level, 0.75 dB per step (0 = loudest) |
| $50+ | `RS1 RS0 0 AR4..AR0` | Rate scaling (key scale) and attack rate |
| $60+ | `AM 0 0 DR4..DR0` | AM enable and decay rate (D1R) |
| $70+ | `0 0 0 SR4..SR0` | Sustain rate (D2R) |
| $80+ | `SL3..SL0 RR3..RR0` | Sustain level (D1L) and release rate |
| $90+ | `0 0 0 0 EN ATT ALT HLD` | SSG-EG |
| $A0-$A2 | `F7..F0` | F-number low 8 bits (channel 1..3 / 4..6) |
| $A4-$A6 | `0 0 BLK2 BLK1 BLK0 F10 F9 F8` | Block and F-number high 3 bits |
| $A8-$AA / $AC-$AE | same layouts | Channel 3 special mode per-operator F-number (S3 = $A8/$AC, S1 = $A9/$AD, S2 = $AA/$AE, S4 = $A2/$A6) |
| $B0-$B2 | `0 0 FB2 FB1 FB0 ALG2 ALG1 ALG0` | Feedback (S1) and algorithm |
| $B4-$B6 | `L R AMS1 AMS0 0 FMS2 FMS1 FMS0` | Output enable L/R, AM sensitivity, PM sensitivity |

Per-operator register addresses (OPNA manual Table 2-2, identical in plutiedev):

```cpp
// Address offset added to the register base ($30, $40, $50, $60, $70, $80, $90).
// Index [channel 0..2][operator S1,S2,S3,S4]. Bank 1 uses the same offsets for ch 4..6.
constexpr uint8_t kOpRegOffset[3][4] = {
    {0x0, 0x8, 0x4, 0xC},   // ch1/ch4: S1=$x0 S2=$x8 S3=$x4 S4=$xC
    {0x1, 0x9, 0x5, 0xD},   // ch2/ch5
    {0x2, 0xA, 0x6, 0xE},   // ch3/ch6
};
// Equivalent decode: channel = addr & 3 (3 invalid); operator = ((addr>>3)&1) | ((addr>>1)&2)
```

Verified 2026-09-28 against plutiedev (channel 3 special-mode table: S1 $A9/$AD, S2
$AA/$AE, S3 $A8/$AC, S4 $A2/$A6; $28 upper nibble = S1..S4 in bit order 4..7; $27 layout)
and the decode above checked in Python for all 16 offsets.

Register $28 (key on/off), OPNA manual p.18 and plutiedev:

```cpp
// value written to $28: bit 4 = S1, bit 5 = S2, bit 6 = S3, bit 7 = S4 (1 = key on)
// bits 2..0: 000 ch1, 001 ch2, 010 ch3, 100 ch4, 101 ch5, 110 ch6; 011 and 111 invalid (ignored)
constexpr int kKeyOnChannel[8] = { 0, 1, 2, -1, 3, 4, 5, -1 };
```

Key on/off semantics (plutiedev, jsgroth parts 2-3): a change from off to on starts the
attack phase and resets the operator's 20-bit phase counter to 0; a change from on to off
starts the release phase; writing the same state again has no effect (key-on is never
idempotent when the state changes, key-off is idempotent except for SSG-EG inversion,
see SSG-EG). Register $28 is global (bank 0) and addresses channels 4-6 via the channel
field, not via bank 1.

Frequency write order (OPNA manual 2-4-1, plutiedev, jsgroth part 2): write $A4-$A6
(block + F-number high) first, then $A0-$A2; the chip latches the high write and applies
both together when the low byte is written. Writing $A4 alone does not change the pitch.

Other register-timing facts (Nemesis thread page 8, Shiru/Alone Coder tests): TL, MUL,
DT and feedback changes take effect immediately on a playing note; AR/DR/SR/RR and SL
are re-evaluated when a new envelope phase starts (MAME-style immediate rate refresh is
the common emulator choice; see Ambiguities).

DAC registers: $2B bit 7 = 1 routes the last value written to $2A to the channel 6
output slot instead of the FM channel 6 output. Only channel 6's L/R bits in $B6 (bank 1)
affect the DAC (Maxim's SEGA2.DOC transcription p.13; jsgroth part 1). Panning bits
should power up as 1 (jsgroth part 5: After Burner II depends on it).

Timers and CSM: registers $24-$27 timer bits and channel 3 CSM mode are not needed for
this synth. Note only: Timer A ticks once per FM sample with a 10-bit counter, Timer B
once per 16 FM samples with an 8-bit counter (jsgroth part 6); CSM keys channel 3 on and
off on every Timer A overflow.

## Frequency (phase generator)

Sources: OPNA manual 2-4 (formula, key code, Table 2-5, Table 2-6), Nemesis thread
page 11/33, jsgroth part 2 (20-bit counter, shifts, detune overflow, MUL wrap).

Formula (OPNA manual p.24, master clock phiM, block B, note frequency fnote):

```
F-Number = (144 * fnote * 2^20 / phiM) / 2^(B-1)          (B = 0 means multiply by 2)
fnote    = F-Number * 2^(B-1) / 2^20 * phiM / 144
```

Phase increment per FM sample (jsgroth part 2, consistent with the formula above):

```
inc17 = (fnum11 << block) >> 1                 // 17-bit, block 0 halves fnum
inc17 = (inc17 +/- detune[keycode][dt & 3]) & 0x1FFFF   // sign = dt bit 2 (1 = subtract)
inc20 = (mul == 0) ? (inc17 >> 1) : (inc17 * mul)      // wraps at 20 bits
phase20 = (phase20 + inc20) & 0xFFFFF
phase10 = phase20 >> 10                        // 0..1023 = 0..2*pi into the operator
```

The 17-bit mask matters: subtractive detune on a very low increment wraps to 0x1FFFF
(GEMS instruments depend on it); additive detune plus vibrato can also overflow the 17
bits (jsgroth part 2). Key-on resets `phase20` to 0.

Key code (OPNA manual p.25 "Setting of KeyCode"):

```
N4 = F11
N3 = F11 & (F10 | F9 | F8)  |  !F11 & F10 & F9 & F8
keycode5 = (block << 2) | (N4 << 1) | N3            // 0..31
```

Truth table of the low two bits (generated from the formula):

```cpp
// index = (F11<<3)|(F10<<2)|(F9<<1)|F8 ; value = keycode & 3
constexpr uint8_t kKeyCodeLow[16] = { 0,0,0,0, 0,0,0,1, 2,3,3,3, 3,3,3,3 };
```

Naming note: the key-code formula uses the OPNA manual's 1-based names, F11 = fnum bit 10
(MSB) ... F8 = fnum bit 7; the register table above uses 0-based names (F10..F8 = fnum
bits 10..8). Verified 2026-09-28 against the OPNA manual p.25 formula and Table 2-4 (the
N4/N3 columns of C#..C5 at 8 MHz: 0,0,0,0,0,0,1,1,2,2,3,3 are reproduced by the table).

Multiplier (OPNA manual Table 2-5):

```cpp
// MUL 0 = x0.5, MUL n = x n
constexpr double kMulFactor[16] = { 0.5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
```

Detune (OPNA manual Table 2-6; register field D6..D4: 0 none, 1..3 = +1E..+3E, 4 none,
5..7 = -1E..-3E; "D6 is a sign bit"). The manual lists the detune in Hz at phiM = 8 MHz;
one phase-increment unit at 8 MHz is 8e6/144/2^20 = 0.05297 Hz, so units =
round(Hz * 144 * 2^20 / 8e6) (0.053 Hz -> 1.0003). The manual's "up arrow" entries
repeat the value above; resolving them and converting gives:

```cpp
// [keycode 0..31][dt magnitude 0..3] in phase-increment units (17-bit domain)
constexpr uint8_t kDetuneTable[32][4] = {
    {0,0,1,2}, {0,0,1,2}, {0,0,1,2}, {0,0,1,2},      // block 0
    {0,1,2,2}, {0,1,2,3}, {0,1,2,3}, {0,1,2,3},      // block 1
    {0,1,2,4}, {0,1,3,4}, {0,1,3,4}, {0,1,3,5},      // block 2
    {0,2,4,5}, {0,2,4,6}, {0,2,4,6}, {0,2,5,7},      // block 3
    {0,2,5,8}, {0,3,6,8}, {0,3,6,9}, {0,3,7,10},     // block 4
    {0,4,8,11}, {0,4,8,12}, {0,4,9,13}, {0,5,10,14}, // block 5
    {0,5,11,16}, {0,6,12,17}, {0,6,13,19}, {0,7,14,20}, // block 6
    {0,8,16,22}, {0,8,16,22}, {0,8,16,22}, {0,8,16,22}, // block 7
};
// DT register 4..7: subtract kDetuneTable[kc][dt-4]; DT 0 and 4: no detune.
```

Verified 2026-09-28 against the OPNA manual Table 2-6 (text extracted from the translated
PDF; every "up arrow" resolved and every Hz value converted with Python: all 19 distinct
Hz values land within 0.035 unit of an integer, so the rounding is unambiguous).

The OPNA Hz table as printed (block, note, FD=1, FD=2, FD=3; FD=0 is always 0.000):

```
0,0-3: 0.000 0.053 0.106 | 1,0: 0.053 0.106 0.106 | 1,1-3: 0.053 0.106 0.159
2,0: 0.053 0.106 0.212 | 2,1-2: 0.053 0.159 0.212 | 2,3: 0.053 0.159 0.264
3,0: 0.106 0.212 0.264 | 3,1-2: 0.106 0.212 0.317 | 3,3: 0.106 0.264 0.370
4,0: 0.106 0.264 0.423 | 4,1: 0.159 0.317 0.423 | 4,2: 0.159 0.317 0.476 | 4,3: 0.159 0.370 0.529
5,0: 0.212 0.423 0.582 | 5,1: 0.212 0.423 0.635 | 5,2: 0.212 0.476 0.688 | 5,3: 0.264 0.529 0.741
6,0: 0.264 0.582 0.846 | 6,1: 0.317 0.635 0.899 | 6,2: 0.317 0.688 1.005 | 6,3: 0.370 0.741 1.058
7,0-3: 0.423 0.846 1.164                                                     [Unit: Hz, phiM = 8 MHz]
```

Maximum detune 22 units = 22 * 7670453.57 / 144 / 2^20 = 1.1176 Hz at NTSC (with MUL 1).

### Reference F-number table and MIDI mapping

The OPNA manual example table (Table 2-4, phiM = 8 MHz, octave C#4..C5, block 4) gives
C# 654.0, D 692.8, D# 734.0, E 777.7, F 823.9, F# 872.9, G 924.8, G# 979.8, A 1038.1,
A# 1099.8, B 1165.2, C5 1234.5. Sega's manual (Maxim's transcription p.16) lists the 8 MHz
sequence 617 653 692 733 777 823 872 924 979 1037 1099 1164 for C..B. Plutiedev's
"approximate values as used by Echo" for the real 7.67 MHz clock are:

```cpp
// plutiedev / Echo driver table (block 4 = C4..B4), NTSC 7.67 MHz, as published
constexpr uint16_t kFnumOctaveEcho[12] = { 644, 681, 722, 765, 810, 858, 910, 964, 1021, 1081, 1146, 1214 };
```

Recomputed with the formula and exact clocks (Python, A4 = 440 Hz, block 4, rounded to
nearest; exact values in comments):

```cpp
// fnum = round(144 * f * 2^20 / clock / 2^(4-1)), MIDI 60..71
constexpr uint16_t kFnumOctaveNtsc[12] = { 644, 682, 723, 766, 811, 859, 910, 965, 1022, 1083, 1147, 1215 };
//  exact: 643.771 682.052 722.609 765.577 811.101 859.331 910.430 964.567 1021.923 1082.690 1147.070 1215.278
constexpr uint16_t kFnumOctavePal[12]  = { 650, 688, 729, 773, 819, 867, 919, 973, 1031, 1093, 1158, 1226 };
//  exact: 649.697 688.330 729.261 772.625 818.567 867.242 918.811 973.446 1031.330 1092.656 1157.629 1226.465
constexpr uint16_t kFnumOctave8MHz[12] = { 617, 654, 693, 734, 778, 824, 873, 925, 980, 1038, 1100, 1165 };
```

Verified 2026-09-28: the three rounded tables and the exact values recomputed in Python
(identical); the Echo table re-read on plutiedev (identical); the 8 MHz values agree with
the OPNA manual Table 2-4 (C# 654.0 ... A 1038.1 ... C5 1234.5) and its worked example
"F-Number(A4) = 1038.1".

The Echo table differs from the rounded NTSC values by 0 in two entries (C, F#), by -1 in
nine entries and by -2 at A (1081 vs 1082.69); it is an "approximate" table by its own
description. See Ambiguities; the recommended rule is the formula with round-to-nearest.

MIDI note to (block, fnum) rule (from the formula; one block per octave, fnum stays in
644..1215 for MIDI 12..107):

```
f     = 440 * 2^((note - 69) / 12)
block = clamp(note / 12 - 1, 0, 7)            // integer division: C1 (note 24) -> block 1, C4 (60) -> block 4
fnum  = clamp(round(144 * f * 2^20 / clock * 2 / 2^block), 0, 2047)
```

Sample results (NTSC clock 7670453.57; frequency produced = fnum*2^block/2/2^20*fs):
note 12 -> (0, 644) 16.357 Hz; 24 -> (1, 644); 48 -> (3, 644); 57 -> (3, 1083) 220.063 Hz;
60 -> (4, 644) 261.719 Hz; 69 -> (4, 1083) 440.126 Hz; 72 -> (5, 644); 96 -> (7, 644)
2093.748 Hz; 107 -> (7, 1215) 3950.162 Hz; 108 -> (7, 1288); 115 -> (7, 1929); 116 ->
(7, 2044) 6645.375 Hz; 117 and above -> (7, 2047) 6655.129 Hz (the hardware maximum,
jsgroth part 2 gives 6654.7 Hz with 7.67e6). Notes 117 (A8, exact fnum 2165.4) and above
clamp to 2047. (Corrected 2026-09-28: the earlier text said 119; recomputed in Python.) PAL: 60 -> (4, 650), 69 -> (4, 1093). Frequency resolution is one fnum unit =
fs/2^20*2^(block-1): 0.0508 Hz at block 1, 3.25 Hz at block 7 (fine tune of 1 fnum at C4 =
1200*log2(645/644) = 2.7 cents). Fractional MIDI notes (glide, vibrato) go through the
same rounding, which is the audible grid required by `docs/ARCHITECTURE.md`.

Channel 3 special mode ($27 bits 7-6 != 00): each operator of channel 3 has its own
fnum/block (register table above). Not used by the driver; note only.

## Envelope generator

Sources: Nemesis thread page 8 (rate calculation, tables, update cycle, attack formula,
SSG-EG), page 11 (bit weighting), page 12 (EG clock = 1/3 FM clock, SL clamp), jsgroth
part 3 (corrections: attack formula as a shift, counter, SL 15, rate 62/63 skip), OPNA
manual 2-5 (Tables 2-7, 2-8, 2-9, rate formula).

### Attenuation scale

The EG holds a 10-bit attenuation 0..0x3FF per operator. Nominal weighting (Nemesis,
"EG attenuation output bit weighting = 96 / 2^10"):

```
bit:   9    8    7    6    5     4      3       2        1        0
dB:   48   24   12    6    3    1.5   0.75   0.375   0.1875   0.09375
```

The value is a 4.6 fixed-point base-2 exponent: amplitude = 2^(-att/64). One step is
therefore 20*log10(2)/64 = 0.09407 dB (nominal 0.09375), 0x40 = 6.02 dB, 0x3FF = 96.24
dB. Nemesis (page 11) argues the effective range is "0 to 48 dB" because 6 dB per 0x40 is
a factor of 2 and Yamaha approximated log10(2) as 0.3; both statements describe the same
hardware: use 2^(-att/64). Any total attenuation >= 0x340 (13.0 in 4.6) yields operator
output 0 (jsgroth part 4), so the usable range is about 78 dB.

TL and SL scaling (Nemesis page 8; OPNA Tables 2-7 and 2-9; jsgroth part 3):

```
TL10 = TL << 3                       // TL 0..127 -> 0..1016 (0.75 dB per TL step = 8 x 0.09375)
SL10 = (SL == 15) ? 0x3E0 : SL << 5  // SL 0..14 -> 0, 32, ..., 448 (3 dB per step); SL 15 -> 992 (93 dB, OPNA Table 2-7 note)
```

OPNA Table 2-7 weights SL bits as 24, 12, 6, 3 dB and says "when D7-D4 is entire 1 it
becomes 93 dB", i.e. 0x3E0. Sega's document says "D1L ... should be multiplied by 8 if one
wishes to compare it to TL", which is the same 32-per-step scale (8 x 0.75 dB/TL... see
Ambiguities for the 1023 vs 992 question). Verified 2026-09-28 against the OPNA manual
p.28 (Table 2-7: 24, 12, 6, 3 dB; "When D7-D4 is entire 1, it becomes 93dB") and plutiedev
("every step is 0.75dB quieter" for TL); TL 127 -> 1016 = 95.25 dB nominal / 95.58 dB
exact.

### Rate calculation

OPNA manual p.30 and Nemesis page 8:

```
R    = AR | DR | SR for those phases; for release R = RR * 2 + 1 (RR is 4-bit)
Rks  = keycode5 >> (3 - RS)          // RS 0..3; OPNA Table 2-8 is exactly this shift
rate = (R == 0) ? 0 : min(63, 2 * R + Rks)
```

OPNA Table 2-8 (Key-Scaling value of Rate), transcribed: KS=0: 0..3 (one step per two
blocks: blocks 0-1 -> 0, 2-3 -> 1, 4-5 -> 2, 6-7 -> 3); KS=1: 0..7 (one per block);
KS=2: 0..15 (one per two keycodes); KS=3: 0..31 (one per keycode). This is exactly
`keycode5 >> (3 - KS)`. Verified 2026-09-28 against the OPNA manual p.29 (text extracted
from the translated PDF) and Sega's "KC/8 .. KC/1" description. (Corrected 2026-09-28: the
earlier transcription gave KS=0 as 0/1, KS=1 as 0..3 and KS=2 as 0..7, one level off and
inconsistent with the shift formula.) Release rate can never be 0 (Nemesis: "RR is
treated as a 5-bit value with the LSB set to 1, and it's after this conversion that the
check is made"). Verified 2026-09-28 against the OPNA manual p.30 ("Rate = 2R + Rks; Rate =
0 in case of R = 0"; RR "(Set value * 2 + 1) is assumed to be R"; maximum 63) and
Nemesis page 8 (same three rules).

Rate is recomputed when a phase starts; the common emulator behaviour (MAME, per Eke on
page 8) also refreshes it immediately when AR/DR/SR/RR, RS or fnum/block change. Nemesis
could not confirm which is right; see Ambiguities.

### Global counter and update tables

Nemesis page 8, Table 1 (counter shift values) and Table 2 (attenuation increment
values), reproduced exactly (asterisks in the original mark entries that differ from
MAME):

```
Table 1: Counter shift values
11 11 11 11   0-3    (0x00-0x03)
10 10 10 10   4-7    (0x04-0x07)
9  9  9  9    8-11   (0x08-0x0B)
8  8  8  8    12-15  (0x0C-0x0F)
7  7  7  7    16-19  (0x10-0x13)
6  6  6  6    20-23  (0x14-0x17)
5  5  5  5    24-27  (0x18-0x1B)
4  4  4  4    28-31  (0x1C-0x1F)
3  3  3  3    32-35  (0x20-0x23)
2  2  2  2    36-39  (0x24-0x27)
1  1  1  1    40-43  (0x28-0x2B)
0  0  0  0    44-47  (0x2C-0x2F)
0  0  0  0    48-51  (0x30-0x33)
0  0  0  0    52-55  (0x34-0x37)
0  0  0  0    56-59  (0x38-0x3B)
0  0  0  0    60-63  (0x3C-0x3F)

Table 2: Attenuation increment values
*0,0,0,0,0,0,0,0  *0,0,0,0,0,0,0,0  *0,1,0,1,0,1,0,1  *0,1,0,1,0,1,0,1    0-3
 0,1,0,1,0,1,0,1  *0,1,0,1,0,1,0,1   0,1,1,1,0,1,1,1  *0,1,1,1,0,1,1,1    4-7
 0,1,0,1,0,1,0,1   0,1,0,1,1,1,0,1   0,1,1,1,0,1,1,1   0,1,1,1,1,1,1,1    8-11
 (rows 12-15 ... 44-47 identical to row 8-11)
 1,1,1,1,1,1,1,1   1,1,1,2,1,1,1,2   1,2,1,2,1,2,1,2   1,2,2,2,1,2,2,2    48-51
 2,2,2,2,2,2,2,2   2,2,2,4,2,2,2,4   2,4,2,4,2,4,2,4   2,4,4,4,2,4,4,4    52-55
 4,4,4,4,4,4,4,4   4,4,4,8,4,4,4,8   4,8,4,8,4,8,4,8   4,8,8,8,4,8,8,8    56-59
 8,8,8,8,8,8,8,8   8,8,8,8,8,8,8,8   8,8,8,8,8,8,8,8   8,8,8,8,8,8,8,8    60-63
```

As C++ (shift = max(0, 11 - rate/4)):

```cpp
constexpr uint8_t kEgShift[64] = {
    11,11,11,11, 10,10,10,10, 9,9,9,9, 8,8,8,8, 7,7,7,7, 6,6,6,6, 5,5,5,5, 4,4,4,4,
    3,3,3,3, 2,2,2,2, 1,1,1,1, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 };

constexpr uint8_t kEgIncrement[64][8] = {
    {0,0,0,0,0,0,0,0}, {0,0,0,0,0,0,0,0}, {0,1,0,1,0,1,0,1}, {0,1,0,1,0,1,0,1},  // 0-3
    {0,1,0,1,0,1,0,1}, {0,1,0,1,0,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,0,1,1,1},  // 4-7
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},  // 8-11
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},  // 12-15
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},  // 16-19
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},  // 20-23
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},  // 24-27
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},  // 28-31
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},  // 32-35
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},  // 36-39
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},  // 40-43
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},  // 44-47
    {1,1,1,1,1,1,1,1}, {1,1,1,2,1,1,1,2}, {1,2,1,2,1,2,1,2}, {1,2,2,2,1,2,2,2},  // 48-51
    {2,2,2,2,2,2,2,2}, {2,2,2,4,2,2,2,4}, {2,4,2,4,2,4,2,4}, {2,4,4,4,2,4,4,4},  // 52-55
    {4,4,4,4,4,4,4,4}, {4,4,4,8,4,4,4,8}, {4,8,4,8,4,8,4,8}, {4,8,8,8,4,8,8,8},  // 56-59
    {8,8,8,8,8,8,8,8}, {8,8,8,8,8,8,8,8}, {8,8,8,8,8,8,8,8}, {8,8,8,8,8,8,8,8},  // 60-63
};
```

Verified 2026-09-28 against Nemesis page 8 (re-read: rates 0, 2, 4, 5, 6, 7, 9, 11, 48,
49, 51, 53, 59 and 60-63 checked entry by entry; shift 1 for rates 40-43 and 0 for 44-47)
and `kEgShift` regenerated with `max(0, 11 - rate/4)` in Python (identical).

Update cycle (Nemesis page 8 pseudocode; counter width from jsgroth part 3):

```
every 3 FM samples:
    egCounter = egCounter + 1; if (egCounter == 4096) egCounter = 1   // 12-bit, skips 0
    for each operator:
        s = kEgShift[rate]
        if ((egCounter & ((1 << s) - 1)) == 0):
            inc = kEgIncrement[rate][(egCounter >> s) & 7]
            apply(inc)
```

Nemesis's original post says the counter is "of unknown size, at least 14 bits" and
does not skip 0; the 12-bit skip-0 detail comes from jsgroth citing later hardware work.
The skip only changes very low rates slightly (rate 0-3 update once per 4095 instead of
4096 cycles); see Ambiguities.

### Phase update formulas

Attack (attenuation decreases exponentially; Nemesis page 8 as
`att += inc * (((1024 - att) / 16) + 1)` on an inverted register; corrected form from
jsgroth part 3, verified on hardware, on the non-inverted register):

```
att = att + ((inc * ~att) >> 4)      // ~att = -(att + 1); arithmetic shift, so a non-zero
                                      // negative product always moves att towards 0
att = max(att, 0)                     // reaches 0 exactly (the >> keeps a -1 floor)
```

Decay, sustain and release (linear):

```
att = min(att + inc, 0x3FF)
```

Phase transitions (Nemesis page 8, jsgroth part 3):

* Key on: phase = attack, rate from AR. If the attack rate (2*AR + Rks) is 62 or 63 the
  attenuation is set to 0 immediately (attack skipped). Otherwise `att` continues from its
  current value (it is not reset to 0x3FF; Eke's MAME note on page 8, confirmed by
  Nemesis).
* Attack -> decay when att == 0 (checked every EG cycle, also on the first one, so a
  key-on at att 0 skips attack entirely).
* Decay -> sustain when att >= SL10 (checked immediately; SL = 0 skips decay). Nemesis
  page 12 adds the clamp `if (att >= SL10) att = SL10` when leaving decay so a fast decay
  does not overshoot a slow sustain.
* Sustain runs until key off (rate from SR; SR = 0 holds).
* Key off from any phase: phase = release, rate from RR*2+1.
* Rates 62 and 63 in the attack phase make no updates (the attenuation holds unless it is
  already 0) (jsgroth part 3, hardware-verified).

Output of the EG (before the operator): `egOut = min(att + (TL << 3) + amOffset, 0x3FF)`
with `amOffset` from the LFO AM section. This 10-bit value is converted to 4.8 by `<< 2`
in the operator.

### Documented durations for verification

The OPNA manual and Sega's manual (Maxim's transcription only gives Timer A times) have
no attack/decay time table. Reference values from the sources:

* GManiac (SpritesMind t=932, hardware measurement by phase modulation): with an attack
  increment of 1 per update the operator "reaches maximum output of 8168 by step 73"; his
  derived rule is "att += not(att) asr 4", scaled by the increment for faster rates. The
  formula below reproduces this exactly: from 0x3FF, 73 updates with inc 1 (1023, 959,
  899, 842, 789, 739, 692, 648, 607, 569, 533, 499, ...), 40 with inc 2, 21 with inc 4, 10
  with inc 8 (1023, 511, 255, 127, 63, 31, 15, 7, 3, 1, 0).

* Nemesis page 8: a rate of 2 in decay lasts "up to 10223616 samples, or 193.5 seconds on
  a PAL Mega Drive"; rate 63 "as low as 312 samples". Both use his EG ratio of 2.4375
  FM samples per EG cycle (EG clock = external clock / 351), which he later corrected to
  3 samples (page 12: "EG Clock = FM Clock / 3", 72 FM clocks per operator update).
* plutiedev: RR = 15 on all operators followed by key-off silences a channel in "up to
  128 YM2612 samples (about 2.4 ms)": 128 EG updates of +8 = 1024 steps; with 3 FM
  samples per update this is 384 samples = 7.2 ms (see Ambiguities).

Simulated with the tables above (skip-0 counter, EG cycle = 3 FM samples, NTSC fs):

```
rate  attack 0x3FF->0 (EG cycles / ms)   decay 0->0x3FF (EG cycles / ms)   decay ms if x2.4375
   2      296888    16720.7               4187138    235819.6              191603.5
   4      148444     8360.4               2093569    117909.8               95801.7
   8       74222     4180.2               1046785     58954.9               47900.9
  12       37111     2090.1                523393     29477.5               23950.5
  16       18556     1045.1                261697     14738.8               11975.3
  20        9278      522.5                130849      7369.4                5987.7
  24        4639      261.3                 65425      3684.7                2993.8
  28        2320      130.7                 32713      1842.4                1496.9
  32        1160       65.3                 16357       921.2                 748.5
  36         580       32.7                  8179       460.6                 374.3
  40         290       16.3                  4090       230.3                 187.2
  44         145        8.2                  2045       115.2                  93.6
  48          73        4.1                  1023        57.6                  46.8
  52          40        2.3                   512        28.8                  23.4
  56          21        1.2                   256        14.4                  11.7
  60          10        0.56                  128         7.2                   5.9
  63           0        0                     128         7.2                   5.9
```

Verified 2026-09-28: the whole table re-simulated in Python (counter starting at 0,
first tick = 1, skip-0 at 4096): every EG-cycle count and millisecond value identical.
Note the skip-0 counter means shift 11 updates only at counter 2048 (pattern index 1) and
shift 10 only at 1024/2048/3072 (indices 1..3); averages match the wide-counter rates.

Each rate step of 4 halves the time; rates within a group of 4 differ only by the
increment pattern (rates 48-63 differ by 1.33x/1.5x/1.75x within a group, rates 8-47 by
the 0,1 patterns). Unit tests should check the EG-cycle counts exactly and the
milliseconds with the clock chosen.

## Operator (sine, exponential, modulation, algorithms)

Sources: Nemesis thread page 11 (operator unit, sine and power tables, 14-bit output),
jsgroth part 4 (index mirroring, 13-bit magnitude, PM input bits, algorithms, feedback),
GManiac page 37 (measured range +-8168), OPNA manual 2-3 (algorithm descriptions, Table
2-3 feedback levels).

### Tables

Log-sine table, 256 quarter-wave entries, 4.8 fixed point (Nemesis page 11; jsgroth
part 4 confirms "bit-perfect match"):

```
sinTable[i] = round(-log2(sin((2*i + 1) / 512 * pi / 2)) * 256),  i = 0..255
```

Exponential (power) table, 256 entries, 0.11 fixed point, offset by one step:

```
expTable[i] = round(2^(-(i + 1) / 256) * 2048),  i = 0..255    // expTable[0] = 2042, expTable[255] = 1024
```

Equivalent "OPL-style" formulation (the form quoted in the task brief): a table of the
fractional part `expFrac[i] = round((2^(i/256) - 1) * 1024)` (0..1018) read with an inverted
index and the implicit leading one restored: `kExpTable[f] = expFrac[255 - f] + 1024`.
Because `round(x - 1024) + 1024 = round(x)`, the two formulations give identical values for
all 256 entries (checked with Python: `all(exp_n[f] == exp_o[255 - f] + 1024)` is True;
`expFrac[0] = 0`, `expFrac[255] = 1018`). Likewise the sine formula
`round(-log2(sin((i + 0.5) / 256 * pi / 2)) * 256)` is the same expression as the one above
(checked equal for all 256 entries). The implementer may use either form.

Generated values (Python, `int(x + 0.5)`):

```cpp
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
};  // min 0, max 0x859 (2137), sum 65406

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
};  // min 0x400 (1024), max 0x7FA (2042), sum 377687
```

Verified 2026-09-28: both arrays parsed from this file and diffed against a fresh Python
generation of both formulations (sine: `(2i+1)/512` and `(i+0.5)/256` forms; exp: the
`2^(-(i+1)/256)` form and the inverted OPL-style fraction table): 0 differences out of 256
in each table; sums 65406 and 377687 confirmed. The operator verification values below
were recomputed with `op_out` and match.

### Operator evaluation

Inputs: 10-bit phase from the phase generator, 10-bit modulation input, 10-bit EG output.
Output: signed 14-bit (Nemesis page 11; jsgroth part 4):

```
p      = (phase10 + modInput10) & 0x3FF          // modulation is added to the phase output, not to the counter
idx    = (p & 0x100) ? (~p & 0xFF) : (p & 0xFF)  // second quarter mirrors the first
total  = kSinTable[idx] + (egOut10 << 2)          // 5.8 fixed point: 4.8 sine + 4.6->4.8 attenuation
mag13  = (total >> 8) >= 13 ? 0 : (kExpTable[total & 0xFF] << 2) >> (total >> 8)   // 0..8168
out14  = (p & 0x200) ? -mag13 : mag13             // sign from the top phase bit
```

Nemesis's `InversePow2` is `(powTable[num & 0xFF] << 2) >> (num >> 8)`. The maximum
operator output is (2042 << 2) = 8168 and the minimum is -8168, matching GManiac's
hardware measurement "operator's output is -8168..+8168" (page 37). The sign is applied
as a negation in jsgroth's description; a one's complement (`~mag`) is what a hardware
adder would do and differs by one LSB for negative values; see Ambiguities.

Verification values (from the generated tables): phase 0x100, att 0 -> 8168; phase
0x300, att 0 -> -8168; phase 0x100, att 0x40 -> 4084; phase 0x100, att 0x340 -> 0; phase
0x100, att 0x33F -> 1; phase 0, att 0 -> 25; phase 0x0AB, att 0x123 -> 303 and phase
0x2AB, att 0x123 -> -303.

### Phase modulation input and feedback

* A modulated operator adds bits 1..10 of the modulator's 14-bit output to its phase:
  `modInput = modulatorOut14 >> 1` (masked to 10 bits by the phase addition). With two
  modulators the outputs are summed first, then shifted (jsgroth part 4: "adds bits 1-10
  of the sum"). Full-scale modulation therefore spans about +-8 pi (a 14-bit output >> 1
  = +-4084 = +-4 full phase turns).
* Operator S1 feedback (jsgroth part 4; Nemesis page 13 and Eke page 11 note that the
  chip averages the last two S1 outputs): with `FB` = register value 0..7,
  `modInput = (op1Out[n-1] + op1Out[n-2]) >> (10 - FB)` for FB != 0, and 0 for FB = 0.
  jsgroth explains the 10 as "9 - FB" for one output plus 1 more for averaging two
  outputs; the result goes straight into the 10-bit phase input (no further `>> 1`).
  OPNA Table 2-3 gives the modulation index per FB value: OFF, pi/16, pi/8, pi/4, pi/2,
  pi, 2pi, 4pi. Verified 2026-09-28: OPNA Table 2-3 re-read in the extracted text; with
  two full-scale outputs (16336) the shift gives 31, 63, 127, 255, 510, 1021, 2042 phase
  units for FB 1..7, i.e. about pi/16 .. 4pi of the 1024-unit turn (Python), matching the
  table; the "10 - FB" and "average of the last two outputs" statements confirmed in a
  search excerpt of jsgroth part 4 (page itself blocked by a bot filter).

### Algorithms

OPNA manual 2-3 (figure 2-3 descriptions) and jsgroth part 4. `S4` is always a carrier.

```cpp
// kAlgorithm[alg]: for each operator S1..S4, the list of modulators feeding it, and the carrier flag.
// Notation in comments: A->B means A modulates B; carriers are summed into the channel output.
struct AlgoDef { uint8_t modMask[4]; uint8_t carrierMask; };   // modMask[i] bit j: Sj+1 modulates Si+1
constexpr AlgoDef kAlgorithm[8] = {
    { {0, 0b0001, 0b0010, 0b0100}, 0b1000 }, // 0: S1->S2->S3->S4            carriers: S4
    { {0, 0,      0b0011, 0b0100}, 0b1000 }, // 1: (S1+S2)->S3->S4           carriers: S4
    { {0, 0,      0b0010, 0b0101}, 0b1000 }, // 2: S1->S4, S2->S3->S4        carriers: S4
    { {0, 0b0001, 0,      0b0110}, 0b1000 }, // 3: S1->S2->S4, S3->S4        carriers: S4
    { {0, 0b0001, 0,      0b0100}, 0b1010 }, // 4: S1->S2, S3->S4            carriers: S2, S4
    { {0, 0b0001, 0b0001, 0b0001}, 0b1110 }, // 5: S1->S2, S1->S3, S1->S4    carriers: S2, S3, S4
    { {0, 0b0001, 0,      0     }, 0b1110 }, // 6: S1->S2                    carriers: S2, S3, S4
    { {0, 0,      0,      0     }, 0b1111 }, // 7: none                      carriers: S1, S2, S3, S4
};
// S1 always uses its own feedback as its modulation input.
```

The OPNA text for each: 0 "four serial connection"; 1 "S3 is modulated by the synthetic
output of S2 and S1"; 2 "S4 modulated by two modulators (S1 direct, S2->S3)"; 3 "S1->S2
and S3 both into S4"; 4 "two serial connections in parallel"; 5 "common modulator S1
modulates carriers S2, S3, S4"; 6 "one two-operator FM plus two sine waves"; 7 "four
parallel sine waves". Sega's manual gives suggested uses (0 distortion guitar/bass, 1
harp/PSG-like, 2 bass/electric guitar/brass/piano, 3 strings/folk guitar/chimes, 4
flute/bells/chorus/drums, 5 brass/organ, 6 xylophone/tom/organ/vibraphone, 7 pipe
organ).

Evaluation order quirk (Nemesis page 13, jsgroth part 4): operators are evaluated
S1, S3, S2, S4 with a one-stage pipeline, so these modulator paths use the modulator's
output from the previous sample: alg 0: S2->S3; alg 1: S1->S3 and S2->S3; alg 2: S2->S3;
alg 3: S2->S4; alg 5: S1->S3. Optional to emulate; documented for fidelity.
Verified 2026-09-28 by derivation from jsgroth part 4's two rules (search excerpt:
evaluation order "1→3→2→4"; an operator evaluated immediately after its modulator cannot
see that modulator's new output): delayed pairs are S2->S3 (evaluated later) and the
consecutive pairs S1->S3, S3->S2, S2->S4; applied to the eight algorithms this gives
exactly the list above (no algorithm uses S3->S2).
Revised 2026-09-30 (Ambiguity 41, refcheck finding F1): S1 reaches the other operators
through its feedback history register, one sample later than the order alone gives. The
implemented delays, in samples, are S1->S2 1, S1->S3 2, S1->S4 1, S2->S3 1, S2->S4 1,
S3->S4 0 (`modulatorDelay()` in `GenesisTables.h`, pinned for all eight algorithms by
`test_genesis_operator.cpp` "Pipeline delays"). The S2 and S3 paths keep the derivation
above. S1 as a carrier (algorithm 7) reaches the channel accumulator from the same history
register, one sample after S2, S3 and S4 (`kS1CarrierDelay`, test "Algorithm 7: S1 reaches
the accumulator one sample after the other carriers").

### Channel accumulation and clamp

Carrier outputs are summed. jsgroth part 4: the sum "should be clamped to signed 14-bit"
(hardware clamps rather than wraps). jsgroth part 5 and Sauraen (page 55: "the top 9 bits
of the 14-bit operator output are sent to the accumulator, which adds the operators
within one voice") refine this: each carrier is truncated to 9 bits before the sum and the
accumulator is 9-bit:

```
ch9 = clamp( sum over carriers (out14 >> 5), -256, +255 )      // arithmetic shift: 8168 -> 255, -8168 -> -256
```

For single-carrier algorithms (0-3) the two descriptions are identical. Recommended:
truncate per carrier, sum, clamp to -256..255 (see Ambiguities).

## SSG-EG

Sources: Nemesis page 8 (modes, x6 claim, attack interaction), page 13 (no inversion in
release), OPNA manual 2-5-2 (AR must be 1F, shapes table), jsgroth part 7 (hardware
behaviour: 0x200 threshold, x4 increment, virtual key-on, phase reset).

Register $90+ bits: `E` (bit 3) enable, `ATT` (bit 2) attack/invert, `ALT` (bit 1)
alternate, `HLD` (bit 0) hold (Nemesis page 8). OPNA shapes (No. 0..7 = ATT,ALT,HLD bits
000..111, all with E=1): 0 repeating sawtooth (decay, restart), 1 decay once then hold at
minimum, 2 triangle (decay, rise, decay...), 3 decay once then jump to max and hold, 4
inverted sawtooth (rise, restart), 5 rise once then hold at max, 6 inverted triangle,
7 rise once then hold at minimum.

Behaviour with the register enabled (jsgroth part 7, matches Nemesis's descriptions):

* Decay/sustain/release increments are multiplied while `att < 0x200`; at or above 0x200
  the linear phases do not increase further:
  `if (att < 0x200) att = att + 4 * inc; else att unchanged`. Nemesis measured "x6";
  later hardware work says x4 (see Ambiguities). Attack is unaffected.
* Output inversion: when `E && phase != release && (ATT ^ invertFlag)` the EG output is
  `(0x200 - att) & 0x3FF` instead of `att`. Key-off applies the same inversion to the
  stored value in place and then clears the inversion flag (Nemesis page 13 / Eke: do not
  invert in the release phase).
* SSG-EG state logic runs every FM sample (3x the EG rate), before the EG update on the
  cycles where both run, and only when `att >= 0x200`:
  1. if ALT: `invertFlag = HLD ? 1 : !invertFlag`;
  2. if !ALT && !HLD: reset the phase generator counter to 0 (restart);
  3. if keyed on and !HLD: virtual key-on (phase = attack; att = 0 if rate 62/63);
  4. if HLD and phase != attack and output not inverted: att = 0x3FF;
  5. if keyed off (release): att = 0x3FF.
* The 4x rate condition: OPNA says AR must be 1F so that every restart skips the attack
  (rate 62/63). With AR < 1F the attack phase is included in every repetition and the
  inversion flag fights the attack (Nemesis page 8 describes the resulting spikes and
  random polarity; jsgroth part 7 describes the alternate-bit chaos while att is in
  0x200..0x3FF).

Envelope output range in SSG-EG mode is 0..0x200 (0..48 dB) except when holding at
0x3FF.

## LFO

Sources: OPNA manual 2-6 (register, frequencies, AMS/PMS tables), plutiedev (7.67 MHz
frequency table), Nemesis page 33 (PM increment table), jsgroth part 6 (divider table
verified on hardware, AM and PM computation).

Register $22: bit 3 enable, bits 2-0 frequency. Disabling holds the 7-bit LFO counter at
0 (AM and PM then read a constant counter value). The LFO is a 7-bit counter (128 steps
per cycle) advanced every N FM samples:

```cpp
constexpr uint8_t kLfoSamplesPerStep[8] = { 108, 77, 71, 67, 62, 44, 8, 5 };
```

The OPNA manual gives the 8 frequencies at phiM = 8 MHz: 3.98, 5.56, 6.02, 6.37, 6.88,
9.63, 48.1, 72.2 Hz. 8e6/144/128/f gives 109.05, 78.06, 72.10, 68.14, 63.09, 45.07, 9.02,
6.01, i.e. the manual's numbers are one step off the hardware dividers (jsgroth part 6:
"confirmed in actual YM2612 hardware"). Frequencies from the dividers (fs / 128 / N):

```
setting  divider  NTSC Hz  PAL Hz   (8 MHz)   plutiedev (7.67 MHz, from the manual's numbers)
0        108      3.853    3.818    4.019     3.82
1         77      5.405    5.355    5.637     5.33
2         71      5.861    5.808    6.113     5.77
3         67      6.211    6.155    6.478     6.11
4         62      6.712    6.651    7.000     6.60
5         44      9.458    9.372    9.864     9.23
6          8     52.019   51.544   54.253    46.11
7          5     83.230   82.471   86.806    69.22
```

Verified 2026-09-28: NTSC/PAL/8 MHz columns recomputed in Python (fs/128/N, identical);
OPNA values 3.98 5.56 6.02 6.37 6.88 9.63 48.1 72.2 Hz re-read in the manual (p.33);
plutiedev column re-read on plutiedev (identical, and equal to the OPNA values times
7.67/8 within 0.01 Hz). The divider table itself comes only from jsgroth part 6 (page
blocked on 2026-09-28, not re-read): single secondary source, see Ambiguities 13.
One LFO cycle = 128 * N FM samples (13824, 9856, 9088, 8576, 7936, 5632, 1024, 640).

### Amplitude modulation (tremolo)

OPNA manual: AMS 0..3 = 0, 1.4, 5.9, 11.8 dB. Applied as an attenuation added to the EG
output for operators whose AM bit ($60+ bit 7) is set (jsgroth part 6):

```
lfo7  = LFO counter (0..127)
tri6  = (lfo7 & 0x40) ? (lfo7 & 0x3F) : (0x3F - (lfo7 & 0x3F))   // 63..0..63, max attenuation at counter 0
am    = tri6 << 1                                                 // 0..126 in 4.6 units (1.6 fixed point)
amOffset = { 0, am >> 3, am >> 1, am }[AMS]                       // AMS 0..3
```

Maximum offsets: AMS 3: 0x7E = 126 steps = 11.85 dB; AMS 2: 63 steps = 5.93 dB; AMS 1:
15 steps = 1.41 dB (matches 11.8 / 5.9 / 1.4 dB). Verified 2026-09-28: the OPNA manual
p.33 AMS column (0, 1.4, 5.9, 11.8 dB) re-read; the three maxima recomputed in Python
(1.411, 5.927, 11.853 dB). The LFO phase at which the attenuation is maximal (counter 0
here) comes from jsgroth part 6 only and could not be re-read [unverified]; it does not
change depths or rates.

### Phase (frequency) modulation (vibrato)

OPNA manual: PMS 0..7 = 0, 3.4, 6.7, 10, 14, 20, 40, 80 cents (Sega's manual: "% of a
halftone", plutiedev: semitones/100). The LFO modulates the 11-bit fnum before the block
shift (jsgroth part 6: the hardware output is a 12-bit fnum with one extra fraction bit;
key code and detune keep using the unmodulated fnum). Nemesis (page 33) gives the
increment table for fnum bit 9 (`phaseModIncrementTable[8][8]`):

```
{0,0,0,0,0,0,0,0}, {0,0,0,0,1,1,1,1}, {0,0,0,1,1,1,2,2}, {0,0,1,1,2,2,3,3},
{0,0,1,2,2,2,3,4}, {0,0,2,3,4,4,5,6}, {0,0,4,6,8,8,10,12}, {0,0,8,12,16,16,20,24}
```

"the lookup table for every other bit is simply a shift of this table". jsgroth's table is
the same scaled x4 (for fnum bit 10 in 12-bit units):

```cpp
// [FMS 0..7][quarter index 0..7]: delta for fnum bit 10, in 12-bit (fnum << 1) units
constexpr uint8_t kPmTable[8][8] = {
    {0,  0,  0,  0,  0,  0,  0,  0},
    {0,  0,  0,  0,  4,  4,  4,  4},
    {0,  0,  0,  4,  4,  4,  8,  8},
    {0,  0,  4,  4,  8,  8, 12, 12},
    {0,  0,  4,  8,  8,  8, 12, 16},
    {0,  0,  8, 12, 16, 16, 20, 24},
    {0,  0, 16, 24, 32, 32, 40, 48},
    {0,  0, 32, 48, 64, 64, 80, 96},
};
```

Application (jsgroth part 6):

```
lfoHi   = lfo7 >> 2                                   // 5 bits: bit 4 = sign, bit 3 = mirror, bits 2..0 = index
idx     = (lfoHi & 8) ? (7 - (lfoHi & 7)) : (lfoHi & 7)
m       = kPmTable[FMS][idx]
delta   = sum over i = 4..10 of ((fnum >> i) & 1) * (m >> (10 - i))   // bit-by-bit, fnum bits 0..3 never contribute
fnum12  = (fnum << 1) +/- delta   (minus when lfoHi bit 4 is set), masked to 0xFFF
inc17   = (fnum12 << block) >> 2                      // replaces (fnum << block) >> 1
```

Peak depth check for fnum = 0x400 (only bit 10 set): deltas 0, 4, 8, 12, 16, 24, 48, 96
in 12-bit units = 0, 3.38, 6.75, 10.11, 13.47, 20.17, 40.11, 79.31 cents, matching the
manual's 0, 3.4, 6.7, 10, 14, 20, 40, 80. For fnum 0x2A8 (680, between C and C#; the
earlier "(A)" label was wrong, A is 1083 = 0x43B) at FMS 7 the peak delta is 63 (of 1360
twelve-bit units).

Verified 2026-09-28: Nemesis's page 33 table re-read (all 8 rows identical) with his rule
"For fnum bit 10, shift the values up by 1. For fnum bit 8, shift the values down by 1";
bit 10 in 12-bit units is therefore 2 x 2 = 4 x his bit-9 table, which is `kPmTable`;
OPNA PMS column (0, 3.4, 6.7, 10, 14, 20, 40, 80 cents) re-read p.33; plutiedev PMS
(±0.034 .. ±0.80 semitones) identical; peak cents recomputed in Python.

## DAC

* $2B bit 7 enables the DAC; $2A holds an unsigned 8-bit sample. Output = `(v - 0x80)`
  as a signed value placed in the top 8 bits of the 9-bit DAC word: `dac9 = (v - 128) << 1`
  (jsgroth part 1: `(sample - 128) << 6` on the 14-bit scale; Kabuto: the 9th DAC bit "is
  normally set to 0 when playing PCM" and is only reachable through the test register).
  Range -256..+254 in 9-bit units; 0x80 -> 0.
* The DAC value replaces channel 6's FM output at the output stage; channel 6 L/R bits in
  $B6 still apply, no other channel 6 register does (Sega manual p.13, jsgroth part 1/5).
  The ladder offsets apply to the DAC value like to any channel sample.
* The register is sampled once per FM sample (Kabuto: "samples the PCM register once
  every cycle"; nukeykt on t=3049: updated within 4 internal cycles). A sample written
  between two FM samples is held (zero-order hold at fs), which is the aliasing the
  `BandLimitedStepSynth` reproduces. The DAC register accepts linear values (Sik, t=3049).
* Level: the 8-bit DAC full scale equals a 9-bit FM channel at +-254, i.e. the same
  full scale as an FM channel.

## Output stage, ladder effect, console filters

Sources: Sauraen and Eke on thread pages 55-56, GManiac page 37, Kabuto's notes, TmEE and
nukeykt on t=3049, jsgroth part 5 (Nuked-derived offsets, confirmed on hardware by
Kabuto: "I can confirm this large ladder gap from measurements of DAC output levels"),
jsgroth audio-filtering post (Model 1 cutoffs), Jorge Nuno on t=730 (levels).

### 9-bit truncation and time multiplexing

* Each channel output (14-bit) is truncated to 9 bits (arithmetic shift right by 5,
  GManiac: "+8168 asr 5 = +255, -8168 asr 5 = -256"); the DAC is 9-bit (Sauraen: "Any
  emulator which is outputting 14-bit audio is not authentic, simply because the YM2612
  DAC is 9 bit"). Quantise carriers before summing (jsgroth part 5).
* The DAC is time-multiplexed: the 24 internal cycles of one sample are 6 channel slots of
  4 cycles (order ch1..ch6; Sauraen: "the data on the output will be for Ch 1 Op 1 ...
  channels 1-6"). On a discrete YM2612 the channel sample occupies 1 of the 4 cycles
  of its slot and the other 3 are "silence" slots (Kabuto: MD1 "outputs a short pulse for
  each voice, one voice after another"; TmEE: "YM2612 outputs for one cycle every 6
  cycles (one channel time)"). The YM3438 holds the sample for most of the slot (TmEE:
  "5 of these 6 cycles"; Kabuto: MD2 "holds the value for quite a while"), giving a
  higher signal level and better S/N; the board compensates with a lower gain (nukeykt:
  "For YM2612 amplification is ~24x, for YM3438 it's ~6x"; Kabuto measured the "loud
  PCM" test bit at about 30x on MD1 and 5x on MD2).
* Practical model (both revisions): the analog output per sample is the sum of the six
  channel values (9-bit units, after ladder offsets) with a fixed gain such that six
  channels at full scale reach the board's maximum (Kabuto: the MD1 amplifier "does not
  expect to see anything louder than all 6 FM voices being played at once at maximum
  volume"). Full scale is therefore 6 x 256 = 1536 nine-bit units.

### Ladder effect (discrete YM2612 only)

Measured facts: the DAC output is linear from -256..-1 and from 0..255 but the step
between -1 and 0 is twice a normal step (Eke page 56: "the resistor array not exactly
reaching Vcc/2 on either side"); GManiac page 37: "levels -1 and -2 of DAC are the same"
and some large levels are out of order ("+35 is higher than +36"). The silence slots
output the level of -1 or 0 according to the sign bit of the channel (Eke: "when the DAC
output is silenced, the sign bit has an impact, and the output is forced to either +1 or
-1"), also when the channel is muted by its L/R bit.

Emulation values (Eke page 56 in 2015 and jsgroth part 5, identical; units are 9-bit
steps):

```
non-negative sample:  out = sample + 1;   silence slots contribute +1 each   -> aggregate  sample + 4
negative sample:      out = sample;       silence slots contribute -1 each   -> aggregate  sample - 3
channel muted by L/R: out = +4 (sample >= 0) or -4 (sample < 0)
```

So per channel and per output side: `out = (sample >= 0) ? sample + 4 : sample - 3` when
enabled, `+-4` when disabled. The aggregate gap between -1 and 0 becomes 8 steps
("eight times what it would be if the DAC was completely linear"), which sets a volume
floor and makes quiet notes louder (After Burner II, Streets of Rage rely on it). On the
14-bit scale shift these constants left by 5. `chip_revision` = 1 (YM3438 / ASIC:
Model 1 VA7, Model 2 VA0-VA1 and VA3-VA4, Model 3; the discrete YM2612 is in Model 1
VA0-VA6 and Model 2 VA2, jsgroth part 1) uses no offsets.

### Console low-pass filters and levels

* Model 1 VA0-VA2: first-order RC low-pass, cutoff 3.39 kHz; Model 1 VA3-VA6: 2.84 kHz
  (jsgroth filtering post citing the sega-16 model guide). Model 2 (and VA7): a
  second-order filter that sounds "noisy, muffled and heavily distorted" (Kabuto); its
  cutoff is not documented in the sources consulted [unverified]. Residual Media
  ("Forensics: Genesis 2 with original Mega Amp") says the Model 2 amplifier 315-5684
  (boards VA2, VA2.3, VA3, VA4) is "a second-order low-pass filter with external
  capacitors to adjust the cutoff frequency", without giving the stock cutoff; the
  3.68 kHz / 21.16 kHz figures on that page belong to the aftermarket Mega Amp, not to
  the stock console. A ConsoleMods search snippet adds that Model 2 VA0-VA1.8 and Model 1
  VA7 use a Sallen-Key low-pass "with a too-high Q-factor" (page itself blocked, values
  not obtained). The filter applies to FM and PSG alike.
* First-order Butterworth/RC coefficients at fs (bilinear, generated):
  3390 Hz @ 53267.04 Hz: b0 = b1 = 0.1684983368, a1 = -0.6630033263 (jsgroth's scipy
  values match); 2840 Hz: b0 = b1 = 0.1446281622, a1 = -0.7107436755. At the PSG rate
  223721.56 Hz: 3390 Hz: b0 = b1 = 0.0454734564, a1 = -0.9090530873; 2840 Hz:
  b0 = b1 = 0.0383705863, a1 = -0.9232588275. A one-pole RC step form
  `y += alpha * (x - y)` with `alpha = 1 - exp(-2*pi*fc/fs)` gives 0.3295941588 (3390)
  and 0.2846590708 (2840) at the FM rate (0.0908158527 and 0.0766629642 at the PSG rate).
  Verified 2026-09-28: all coefficients recomputed in Python (identical to 10 digits); the
  3.39 kHz (VA0-VA2) and 2.84 kHz (VA3-VA6) first-order cutoffs confirmed in a search
  excerpt of jsgroth's filtering post (page blocked) and in the ConsoleArtisan/sega-16
  summaries.
* Stereo: Model 1 headphone jack only, Model 2 stereo on the AV out, Model 3 mono.

## SN76489 (Genesis PSG)

Sources: SMS Power SN76489 page by Maxim (all protocol, table and LFSR facts, Genesis
noise sampled by Charles MacDonald), TI SN76489AN datasheet (formulas, attenuator
weights, noise rates, "shift register is cleared" on noise-control writes), gen-hw.txt
(PSG on the VDP die, output mixed with the YM2612), Kabuto's notes (VDP debug bits
9-11 replace the PSG output by one channel's level), spritesmind t=1631 and t=730
(levels).

### Clock and counters

Clock = master / 15 = 3579545 Hz NTSC (3546894.93 Hz PAL). The datasheet: the 10-stage
tone counter "is decremented at a N/16 rate where N is the input clock frequency"; four
10-bit down-counters (three tones, one noise) plus an output flip-flop each. Every
counter tick: decrement if non-zero; when it reaches zero it is reloaded from its
register and the flip-flop toggles.

### Write protocol

Maxim (bit 7 set = LATCH/DATA byte `%1cctdddd`, cc = channel 0..3, t = 1 volume / 0
tone or noise, dddd = low 4 bits; bit 7 clear = DATA byte `%0-DDDDDD` whose low 6 bits
go to the high 6 bits of the latched 10-bit tone register, or whose low bits go to the
latched volume (4 bits) or noise (3 bits) register). The latched register is never
cleared by a data byte; tone registers update immediately after each byte (the low 4
bits change the pitch before the data byte arrives). Genesis mapping: 68000 $C00011
(mirrors $C00013/15/17), Z80 $7F11. Sega's integrated versions power up with tone/noise
registers at 0 and volume registers at 15 (silence).

### Tone channels

```
f = clock / (32 * period)            // datasheet f = N / 32n ; period = 10-bit register
period = round(clock / (32 * f))     // e.g. 440 Hz -> 254 (0xFE gives 440.40 Hz), C4 -> 428, A2 110 Hz -> 1017, C7 -> 53
```

Range: period 0x3FF = 109.35 Hz (MIDI A2 -10 cents, Maxim), period 1 = 111861 Hz. Period
0 and 1: "the output is a constant value of +1" (Maxim; used for PCM playback: "when the
half-wavelength (tone value) is set to 1, they output a DC offset value corresponding to
the volume level"; the note that period 0 behaves the same is Maxim's statement for the
Sega implementations; the datasheet is silent on 0). Output per channel is 0 or +1 times
the volume (the flip-flop initial state is arbitrary). Verified 2026-09-28: f = N/32n and
the N/16 counter rate re-read on the datasheet scan p.2; Maxim's "If the register value is
zero or one then the output is a constant value of +1" re-read; periods and frequencies
recomputed in Python (254 -> 440.397 Hz, 0x3FF -> 109.346 Hz = A2 -10.3 cents).

### Attenuation

Datasheet Table 1: weights A0..A3 = 16, 8, 4, 2 dB, all ones = OFF, "maximum attenuation
is 28 db". TI numbers bits MSB-first (the data-format figure labels the first bit "BIT 0"
and the pin table says "D0 (MSB)"), so A0 is the most significant attenuation bit: in the
usual LSB-first numbering of the 4-bit value, bit 0 = 2 dB, bit 1 = 4 dB, bit 2 = 8 dB,
bit 3 = 16 dB, and the value is simply 2 dB per step. (Corrected 2026-09-28: the earlier
text said "bit 3 of the register = 2 dB, bit 0 = 16 dB", which is only true in TI's
numbering.) Verified 2026-09-28 against the datasheet scan (pages 2-4 rendered and read). Maxim: 2 dB per step, ratio 10^(-0.1) = 0.79432823 per step,
volume table as published:

```cpp
// Maxim, SMS Power: 32767 * 10^(-2*att/20), att 0..14; 15 = 0
constexpr int16_t kPsgVolume[16] = {
    32767, 26028, 20675, 16422, 13045, 10362,  8231,  6568,
     5193,  4125,  3277,  2603,  2067,  1642,  1304,     0 };
```

Verified 2026-09-28: the table re-read on Maxim's SMS Power page (identical, including
6568). Recomputing round(32767 * 10^(-att/10)) gives the same values except entry 7: 6538
(published 6568, an apparent typo of 30; see Ambiguities). On the 8191 scale
(8191 * 10^(-att/10)): 8191, 6506, 5168, 4105, 3261, 2590, 2057, 1634, 1298, 1031, 819,
651, 517, 411, 326, 0.

### Noise channel

Register (3 bits, `-trr`): bit 2 = mode (1 white, 0 "periodic"), bits 1-0 = rate. Counter
reload values (Maxim) and datasheet Table 3 shift rates:

```cpp
// noise counter reload per rr; the flip-flop toggles every reload and the LFSR shifts on 0->1 transitions
constexpr uint16_t kNoiseReload[4] = { 0x10, 0x20, 0x40, 0 /* use tone 2 (channel 3) period */ };
// shift rates: clock/512 (6991.30 Hz NTSC), clock/1024 (3495.65 Hz), clock/2048 (1747.82 Hz), tone-2 rate
```

Datasheet Table 3 (NF0 is the MSB in TI numbering): NF0 NF1 = 00 N/512, 01 N/1024 (the
scan misprints this row as "0 0"), 10 N/2048, 11 "Tone Generator #3 Output". So the
register value rr = 0, 1, 2, 3 selects N/512, N/1024, N/2048, tone 3, consistent with
Maxim's reload table. (Corrected 2026-09-28: the earlier text gave 10 = N/1024 and 01 =
N/2048, bit-swapped; checked on the rendered scan page 3 and on Howel's LSB-first
transcription, which lists NF1 NF0 = 01 -> N/1024, 10 -> N/2048.) With tone 2 as the
source the LFSR shifts at the tone-2 frequency f = clock/(32*period).

LFSR (Maxim; Genesis/SMS/GG data sampled by Charles MacDonald): 16 bits on the Sega
VDP-integrated PSG. Shift right; the output to the mixer is bit 0; the input to bit 15 is
the XOR (parity) of the tapped bits. White noise taps: bits 0 and 3 (`0x0009`) on SMS 1/2,
Genesis and Game Gear; the discrete SN76489 (SG-1000, SC-3000, BBC, ColecoVision) is 15
bits with taps 0 and 1 (`0x0003`) into bit 14; Tandy 1000 taps `0x0011`. "Periodic" noise
taps bit 0 only (the register recirculates). Reset on any write to the noise register:
all bits zero except the highest bit (0x8000). Maxim's implementation:

```
sr = (sr >> 1) | ((white ? parity(sr & taps) : (sr & 1)) << 15);   output = sr & 1
```

Periods (computed by running the register from 0x8000): white 16-bit taps 0x0009 -> 57337
shifts (not the maximal 65535; Maxim says "up to 2^n - 1"); periodic -> 16 shifts (one 1
in 16, duty 1/16); 15-bit taps 0x0003 -> 32767. The periodic mode at rate rr therefore
outputs a pulse train at (shift rate)/16: 436.96 Hz at /512; with tone 2 as source the
range is 6.83 Hz (period 0x3FF) to 6991.3 Hz (period 1), "shifted 4 octaves down from the
regular tone range" (Maxim). First 48 output bits after a noise-register write, white
mode, output read before the shift: `000000000000000100000000000010010000000001000001`.
Verified 2026-09-28: taps, widths, feedback bits and the 0x8000 reset re-read on Maxim's
SMS Power page ("all bits are zero except for the highest bit"; output = the bit shifted
off the end; periodic period 16 on the 16-bit register); the datasheet's "Whenever the
noise control register is changed, the shift register is cleared" read on the scan;
periods 57337 / 16 / 32767 and the 48-bit prefix re-run in Python (identical).

Output inversion note (Maxim): some systems produce inverted output; a 16-bit LFSR with
pattern $0006 inverted equals pattern $8005. The Genesis values above were sampled with
the same polarity convention as the SMS.

### Output levels and mixing

* Each channel contributes `bit * kPsgVolume[att]` with bit in {0, 1}; the mixer sums the
  four (datasheet: an op-amp summing circuit). The real output decays towards zero
  through coupling, so Maxim recommends emulating tones as -0.5/+0.5 x volume; for
  "periodic" noise (average 1/16) 0/+1 is closer. This synth keeps 0/+1 and relies on the
  DC-blocking stage of `ARCHITECTURE.md`, which gives the same result for steady tones.
* PSG level relative to the FM output on real hardware (spritesmind t=730, Jorge Nuno):
  the raw YM2612 output is "around -27dB below PSG" and the board mixes "0.0431 for the
  PSG, 1 for the YM" (20*log10(0.0431) = -27.3 dB), i.e. the two arrive at the amplifier
  at about the same level. Emulator practice (t=1631): Nemesis uses "divide by 6"
  (-15.6 dB) on his PSG scale to match the YM2612, TmEE "~6.4" (-16.1 dB) referenced to
  hardware, and Nemesis notes the ratio "does vary based on the particular system
  model". See Ambiguities for the decision.
* The VDP debug register bits 9-11 can replace the PSG output by a single channel's level
  (Kabuto), not needed.

## Reference values for unit tests

Numeric values with the formula or source each one came from (NTSC unless stated).

Clocks:
1. YM clock NTSC = 53693175/7 = 7670453.5714 Hz (formula, gen-hw/Eke).
2. FM sample rate NTSC = 7670453.5714/144 = 53267.0387 Hz (Nemesis: FM clock/24).
3. FM sample rate PAL = 53203424/7/144 = 52781.1746 Hz (Eke page 8: 52781.17).
4. EG rate NTSC = 53267.0387/3 = 17755.6796 Hz (Nemesis page 12).
5. PSG clock NTSC = 53693175/15 = 3579545 Hz; counter rate 223721.5625 Hz (Maxim).
6. PSG clock PAL = 3546894.9333 Hz; counter rate 221680.9333 Hz.

Frequency:
7. fnum(C4, block 4, NTSC) = 144*261.6256*2^20/7670453.57/8 = 643.771 -> 644 (OPNA formula).
8. fnum(A4, block 4, NTSC) = 1082.690 -> 1083; PAL 1092.656 -> 1093; 8 MHz 1038.1 (OPNA example).
9. fnum(C4, block 4, PAL) = 649.697 -> 650.
10. Full NTSC octave: 644 682 723 766 811 859 910 965 1022 1083 1147 1215; Echo/plutiedev: 644 681 722 765 810 858 910 964 1021 1081 1146 1214.
11. Frequency of (block 4, fnum 1083) = 1083*8/2^20*53267.04 = 440.126 Hz.
12. Maximum (block 7, fnum 2047) = 6655.129 Hz (jsgroth: 6654.7 at 7.67e6).
13. Phase increment for MUL 0: inc20 = inc17 >> 1 (x0.5); MUL 15: inc17*15.
14. Key code: block 4, fnum 0x2A8 (F11..F8 = 0101) -> kc = 16; fnum 0x400 (1000) -> kc = 18; fnum 0x3C0 (0111) -> kc = 17; fnum 0x4C0 (1001) -> kc = 19 (N3/N4 formula).
15. Detune: kc 18, DT 3 -> +9; DT 7 -> -9; kc 31, DT 3 -> +22 = 1.1176 Hz; kc 0, DT 1 -> 0 (OPNA Table 2-6).
16. Detune underflow (masking function alone): inc17 = 5, detune -22 -> (5 - 22) & 0x1FFFF = 0x1FFEF (jsgroth part 2). This input is not reachable through the registers (kc 31 means block 7, where inc17 = fnum * 64); a register-reachable case: block 0, fnum 1 (inc17 = (1 << 0) >> 1 = 0, kc 0), DT 7 -> (0 - 2) & 0x1FFFF = 0x1FFFE; block 0, fnum 3 (inc17 = 1), DT 6 -> (1 - 1) = 0; block 0, fnum 3, DT 7 -> 0x1FFFF.
16b. Phase increment, block 4, fnum 1083 (kc 18), DT 0: inc17 = (1083 << 4) >> 1 = 8664; MUL 0 -> inc20 = 4332 (220.06 Hz); MUL 1 -> 8664 (440.13 Hz); MUL 15 -> 129960; with DT 3 (+9): inc17 = 8673, MUL 1 -> 8673.

Envelope:
17. rate(AR 31, RS 0, kc 31) = 62 + (31 >> 3) = 65 -> 63; rate(AR 15, RS 3, kc 31) = 30 + 31 = 61; rate(0, any) = 0; release RR 15 -> R = 31 -> 62 + Rks.
18. kEgShift[2] = 11, [44] = 0; kEgIncrement[63] = all 8; [2] = 0,1,0,1,0,1,0,1; [49] = 1,1,1,2,1,1,1,2.
19. Attack from 0x3FF at rate 63 -> 0 cycles (skipped); rate 60 -> 10 EG cycles (0.56 ms); rate 32 -> 1160 EG cycles = 65.33 ms; rate 2 -> 296888 EG cycles = 16.72 s (simulation).
20. Decay 0 -> 0x3FF: rate 63 -> 128 EG cycles = 384 FM samples = 7.21 ms; rate 48 -> 1023 cycles = 57.6 ms; rate 32 -> 16357 cycles = 921.2 ms; rate 2 -> 4187138 cycles = 235.8 s (Nemesis: 193.5 s PAL with his 2.4375 ratio).
21. Attack single step: att 0x3FF, inc 8 -> 0x3FF + ((8 * ~0x3FF) >> 4) = 0x3FF - 512 = 0x1FF; att 0x10, inc 1 -> 0x10 + ((-17) >> 4) = 0x10 - 2 = 0x0E (arithmetic shift floors: -17 >> 4 = -2; corrected 2026-09-28, the earlier value 0x0F assumed truncation towards zero).
21b. Attack update count from 0x3FF to 0: inc 1 -> 73 updates (GManiac hardware measurement, t=932), inc 2 -> 40, inc 4 -> 21, inc 8 -> 10 (formula above).
22. TL 127 -> 1016 (95.6 dB); SL 8 -> 0x100 (24 dB); SL 15 -> 0x3E0 (93 dB, OPNA Table 2-7).
23. Attenuation to amplitude: 0x40 -> 0.5 (6.02 dB); 0x3FF -> 2^-15.984 (96.24 dB); step 0.09407 dB; >= 0x340 (78.27 dB) -> 0 output.
23b. Key scaling: Rks for kc 31 = 3, 7, 15, 31 at RS 0..3; kc 18 -> 2, 4, 9, 18 (OPNA Table 2-8, `kc >> (3 - RS)`).
23c. SSG-EG (x4 rule, decay phase, from att 0): rate 63 (inc 8 -> 32 per EG cycle) reaches 0x200 after 16 EG cycles; rate 48 (inc 1 -> 4) after 128 EG cycles; att then stays >= 0x200 until the SSG-EG logic acts. Inverted output: att 0 -> 0x200, att 0x100 -> 0x100, att 0x200 -> 0; mode shapes (register value 8..F): 8 \\\\, 9 \___, A \/\/, B \-- (hold high), C ////, D /-- (hold high), E /\/\, F /___ (OPNA 2-5-2). These are derived from the formulas; the x4 factor itself is Ambiguity 7.

Operator:
24. kSinTable[0] = 0x859, [1] = 0x6C3, [128] = 0x07F, [255] = 0x000, sum 65406.
25. kExpTable[0] = 0x7FA (2042), [128] = 0x5A4, [255] = 0x400 (1024), sum 377687.
26. Operator output: phase 0x100/att 0 -> +8168; 0x300/0 -> -8168; 0x100/0x40 -> 4084; 0x100/0x33F -> 1; 0x100/0x340 -> 0; 0/0 -> 25; 0x0AB/0x123 -> 303, 0x2AB/0x123 -> -303.
27. Feedback FB 7 with the last two outputs 8168 and 8168: (16336 >> 3) & 0x3FF = 2042 & 0x3FF = 0x3FA phase units (jsgroth shift 10 - FB).
28. 9-bit truncation: 8168 >> 5 = 255, -8168 >> 5 = -256 (GManiac); carrier sum clamp -256..255.
29. Algorithm carriers: 0-3 -> {S4}; 4 -> {S2, S4}; 5, 6 -> {S2, S3, S4}; 7 -> all (OPNA).

LFO / DAC / output:
30. LFO frequencies NTSC: 3.853, 5.405, 5.861, 6.211, 6.712, 9.458, 52.019, 83.230 Hz (fs/128/N); PAL: 3.818, 5.355, 5.808, 6.155, 6.651, 9.372, 51.544, 82.471 Hz; LFO cycle in FM samples: 13824, 9856, 9088, 8576, 7936, 5632, 1024, 640.
31. AM offsets: AMS 3 max 0x7E (11.85 dB), AMS 2 max 63 (5.93 dB), AMS 1 max 15 (1.41 dB), AMS 0 none.
32. PM peak for fnum 0x400: 0, 4, 8, 12, 16, 24, 48, 96 twelve-bit units = 0, 3.38, 6.75, 10.11, 13.47, 20.17, 40.11, 79.31 cents; fnum 0x2A8, FMS 7 -> 63; fnum 0x7FF, FMS 7 -> 190.
33. DAC: 0x00 -> -256, 0x80 -> 0, 0xFF -> +254 (9-bit); 0x7F -> -2.
34. Ladder (revision 0): sample 0 -> +4, sample -1 -> -4 (aggregate gap 8 steps); muted channel -> +-4; revision 1 -> unchanged.
35. Low-pass 3390 Hz @ 53267.04: b0 = b1 = 0.1684983368, a1 = -0.6630033263 (jsgroth scipy: 0.1684983368367697 / -0.6630033263264605).

PSG:
36. Tone period 0xFE -> 440.397 Hz (Maxim: 440.4); period 0x3FF -> 109.35 Hz; period 1 -> 111860.8 Hz; PAL 0xFE -> 436.38 Hz.
37. Note -> period: A4 -> 254, C4 -> 428, A2 -> 1017, C7 -> 53 (round(clock/(32 f))).
38. Volume: att 0 -> 32767, 1 -> 26028, 6 -> 8231, 14 -> 1304, 15 -> 0 (Maxim table); ratio per step 0.79432823.
39. Noise: white period 57337 shifts from 0x8000 with taps 0x0009; periodic period 16; 15-bit/0x0003 -> 32767.
40. Noise shift rates: 6991.30, 3495.65, 1747.82 Hz; periodic tone at /512 = 436.96 Hz; tone-2 source period 1 -> 6991.3 Hz, 0x3FF -> 6.834 Hz (periodic-noise output frequency; the LFSR shift rate is 16x these).
40b. Noise rate selection: rr = 0, 1, 2 -> reload 0x10, 0x20, 0x40 (N/512, N/1024, N/2048); rr = 3 -> tone 2 (channel 3) reload value (Maxim; datasheet Table 3 with NF0 as MSB).
40c. Tone period 0 and 1 -> constant +1 output (times the volume); period 2 -> 55930.4 Hz square (Maxim).
41. First 48 white-noise output bits after reset: 000000000000000100000000000010010000000001000001.
42. PSG frame: 223721.5625 / 59.92274 = 3733.50 counter ticks per NTSC frame (the frame is 3420 * 262 = 896040 master clocks = 3733.5 * 16 * 15 exactly); PAL: 221680.9333 / 49.70146 = 4460.25 ticks (3420 * 313 master clocks). (Corrected 2026-09-28: the earlier 3722.56 used the NES frame rate 60.0988 Hz.)
43. Genesis frame rate: NTSC 53693175 / (3420 * 262) = 59.92274 Hz; PAL 53203424 / (3420 * 313) = 49.70146 Hz. FM samples per frame: NTSC 888.93 (896040 / 1008), PAL 1061.96.
44. PAL fnum octave (block 4): 650 688 729 773 819 867 919 973 1031 1093 1158 1226.

## Ambiguities

Each entry: topic; sources consulted; what each says; recommended decision; alternative.

1. EG clock ratio. Nemesis page 8: "EG Clock = External Clock / 351" (2.4375 FM samples
   per EG cycle); Nemesis page 12 and jsgroth part 3: EG clock = FM clock / 3, i.e. one
   update per 3 FM samples (72 internal cycles). Decision: 3 FM samples. Alternative:
   2.4375 (would shorten every envelope by 19%; only in the original post).
2. Global EG counter width and skip-0. Nemesis: "unknown size, at least 14 bits", no
   skip; jsgroth (from later hardware analysis): 12-bit counter that jumps from 4095 to
   1. Decision: 12-bit with skip-0 (affects only rates 0-7 by 1/4096). Alternative:
   plain free-running counter.
3. Attack formula. Nemesis: `att += inc * (((1024 - att) / 16) + 1)` applied to an
   inverted register that is cleared at key-on (he later doubted the clear); jsgroth:
   `att += (inc * ~att) >> 4`, no reset at key-on, hardware verified; GManiac (t=932,
   hardware): "att += not(att) asr 4", 73 steps at the slowest pattern, which the jsgroth
   form reproduces exactly; GreenLine (same thread, from a patent) proposed
   `att -= step + (att >> 4)`. Decision: jsgroth/GManiac form. Alternative: Nemesis's form
   (produces different step sizes near 0).
4. Attenuation at key-on. Nemesis first said the attenuation is cleared; Eke (MAME) and
   Nemesis's reply, plus jsgroth: it is not cleared, the attack continues from the current
   value, except rates 62-63 which set it to 0. Decision: not cleared.
5. SL = 15 mapping. Task brief: 15 -> 1023; OPNA Table 2-7: all ones = 93 dB = 0x3E0;
   jsgroth: 0x3E0 "highest possible multiple of 32". Decision: 0x3E0 (both >= 0x340 are
   silent, the difference is only when the sustain rate is 0 and TL is small).
   Alternative: 0x3FF.
6. Rate refresh timing. MAME (Eke page 8) recomputes the rate immediately when the rate
   registers, RS or fnum/block change; Nemesis: rate computed when a phase starts, "consider
   this suspect until I can re-test"; Shiru/Alone Coder: rate changes take effect at the
   next key-on. Decision: recompute immediately on register change (MAME behaviour, needed
   by Batman & Robin per Eke). Alternative: latch at phase start.
7. SSG-EG increment multiplier. Nemesis page 8: x6 ("MAME ... only multiplies by 4, not
   6"); jsgroth part 7 (later hardware analysis): x4 below 0x200 and no increase at or
   above 0x200. Decision: x4 with the 0x200 gate. Alternative: x6 (the shape is the same,
   the repetition rate differs by 1.5).
8. dB scale of the EG (96 vs 48 dB). Nemesis page 11 says the true range is 0-48 dB;
   jsgroth and the tables say amplitude = 2^(-att/64), 96.2 dB at 0x3FF, 0.094 dB per
   step (task brief: 0.09375). Both agree on 2^(-att/64). Decision: implement the
   base-2 formula; document 0.09375 dB nominal / 0.0941 dB exact.
9. Operator sign. jsgroth: negate the 13-bit magnitude; Nemesis's diagram (not
   reproduced) and the measured symmetric range +-8168 suggest a plain negation; a one's
   complement would give -8169 minimum. Decision: negation. Alternative: `~mag`.
10. Carrier accumulation. jsgroth part 4: sum carriers then clamp to 14 bits; jsgroth part
    5 and Sauraen: truncate each carrier to 9 bits, sum in a 9-bit accumulator. Decision:
    truncate per carrier (>> 5), sum, clamp to -256..255. Alternative: 14-bit sum, clamp,
    then >> 5 (identical for algorithms 0-3).
11. Ladder-effect constants. Eke page 56 (2015): +1 on non-negative output, +3/-3 (or
    +4/-4 when muted) by sign; jsgroth part 5: +1 and +-1 per silence slot, aggregated to
    +4/-3 (muted +-4); GManiac: levels -1 and -2 identical, some larger levels out of order
    (not modelled anywhere). Decision: +4 / -3 / +-4 aggregate model. Alternative: model
    the four slots individually at 4 internal cycles each (needed only for a
    cycle-level DAC model).
12. Reference F-number table. plutiedev/Echo: 644 681 722 765 810 858 910 964 1021 1081
    1146 1214 ("approximate"); formula with exact NTSC clock rounded: 644 682 723 766 811
    859 910 965 1022 1083 1147 1215. Decision: formula, round to nearest; tests compare
    with the plutiedev table with a tolerance of 2 fnum units and with the rounded formula
    values exactly. Alternative: hard-code the Echo table.
13. LFO frequencies. OPNA manual (8 MHz): 3.98 5.56 6.02 6.37 6.88 9.63 48.1 72.2 Hz;
    plutiedev rescales them to 7.67 MHz: 3.82 5.33 5.77 6.11 6.60 9.23 46.11 69.22 Hz; the
    divider table 108 77 71 67 62 44 8 5 (jsgroth, hardware) gives 3.85 5.41 5.86 6.21
    6.71 9.46 52.0 83.2 Hz at NTSC, i.e. the manual's numbers correspond to dividers one
    larger. Decision: divider table (the register model must produce it); tests measure
    the emulated counter and compare with fs/128/N. Alternative: the manual values.
14. PM application. OPNA calls it PM (phase modulation); jsgroth: it is a frequency
    modulation applied to fnum with one extra fraction bit; Nemesis page 33 had not tested
    whether a carry from bit 8 of the modulation is possible. Decision: jsgroth's 12-bit
    fnum method with the bit-by-bit multiplication; peak depths match the manual within
    0.7 cents.
15. Release time quoted by plutiedev ("up to 128 YM2612 samples, about 2.4 ms" for RR 15)
    counts EG updates as samples. With 3 FM samples per EG cycle it is 384 samples =
    7.2 ms. Decision: 7.2 ms (the register model gives it).
16. Model 2 low-pass filter. Kabuto: "2nd order filter"; jsgroth: "second-order low-pass
    filter that makes audio output sound much more muffled"; no cutoff value found
    [unverified]. Decision: implement `model1_lowpass` as the first-order 3.39 kHz filter
    (VA0-VA2) with 2.84 kHz (VA3-VA6) as an option; do not model the Model 2 filter.
    Alternative: a second-order filter with a guessed cutoff.
17. PSG to FM level ratio. Jorge Nuno (t=730): raw PSG about 27 dB above the YM2612, mixed
    with ratio 0.0431:1 so both reach the amplifier at about the same level; Nemesis and
    TmEE (t=1631): divide the PSG by 6 to 6.4 (-15.6 to -16.1 dB) "to roughly match" in
    software, and "it does vary based on the particular system model". The reference
    signals behind these numbers are not stated. Decision: one PSG channel at attenuation
    0 (32767 in Maxim's scale) = one FM channel at full scale (+-256 nine-bit units)
    divided by 6.4, i.e. PSG channel full scale = -16.1 dB relative to an FM channel at
    TL 0, exposed as a fixed mix constant `kPsgToFmGain = 256.0 / 6.4 / 32767.0` per PSG
    unit. Alternative: Jorge Nuno's near-equal level (0 dB).
18. Maxim's volume table entry 7. Published 6568; 32767 * 10^(-0.7) = 6538. Decision:
    keep the published table verbatim (2 dB steps are what tests check, tolerance 0.05 dB)
    and note the 0.04 dB deviation. Alternative: 6538.
19. White-noise period. Maxim: "up to 65535"; running the documented register from
    0x8000 with taps 0x0009 gives 57337 (the polynomial is not maximal). Decision: test
    for 57337; the reset value and taps are what matter.
20. PSG period 0. Maxim states period 0 and 1 both output a constant +1 on the Sega
    versions; the TI datasheet is silent on 0 and its counter model (decrement if non-zero,
    reload on reaching zero) would leave a period-0 channel stuck at its current
    flip-flop level. Decision: period 0 and 1 -> constant +1 (Maxim, Sega hardware).
    Alternative: period 0 -> hold the previous level.
21. PSG output polarity. Maxim: tones are 0/+1 internally but centred by the analog
    stage; recommends -0.5/+0.5 for tones. Decision: 0/+1 into the DC blocker
    (equivalent for steady tones, keeps the periodic-noise and period-1 DC behaviour
    exact). Alternative: -0.5/+0.5 offsets in the digital mixer.
22. Operator evaluation order/pipeline delays (Nemesis page 13, jsgroth part 4). No
    source disputes them but they are optional. Decision: implement the S1,S3,S2,S4
    order with the listed delayed paths.
23. Timers/CSM: not modelled (only notes above).
24. AMS depth sign. OPNA manual and Sega's manual: 0, 1.4, 5.9, 11.8 dB; plutiedev writes
    "+-1.4 dB", "+-5.9 dB", "+-11.8 dB". jsgroth part 6: AM is a unipolar attenuation
    (0 .. max, never a gain). Decision: unipolar attenuation 0..max (the EG can only
    attenuate); the printed dB value is the peak-to-peak swing. Alternative: +-depth around
    a centre (impossible at TL 0, rejected).
25. Exponential table formula. Nemesis: `round(2^(-(i+1)/256) * 2048)`; task brief / OPL
    ROM convention: `round((2^(i/256) - 1) * 1024)` read inverted plus 1024. Verified
    identical for all entries, no decision needed.
26. Model 2 filter cutoff. Kabuto, jsgroth, Residual Media: second-order low-pass on the
    315-5684 amplifier with external capacitors; no stock cutoff or Q found (ConsoleMods
    page blocked). Decision: do not model (see 16). Alternative: measure a Model 2 recording
    later and fit a biquad.
27. YM2612 vs YM3438 output level. nukeykt: board gain 24x vs 6x; TmEE: YM3438
    "significantly louder"; Kabuto: loud-PCM bit 30x vs 5x. Decision: same digital full
    scale for both revisions (the boards compensate); the difference is the ladder
    offsets only.
28. Driver tick rate. `docs/ENGINE_SPECS.md` says the Genesis driver runs "once per video
    frame, 60.0988 Hz NTSC / 50.0070 Hz PAL", which are the NES rates. The Genesis frame
    is 3420 master clocks x 262 lines (NTSC) / 313 lines (PAL): 59.92274 Hz / 49.70146 Hz
    (Sega Retro technical specifications: 59.92274 / 49.701459 Hz; RadDad772: 3420 master
    clocks per line). Decision: the Genesis driver ticks at 59.92274 / 49.70146 Hz
    (`frameRate()` / `masterClocksPerFrame()` in `GenesisTables.h`); ENGINE_SPECS.md
    "Common structure" was corrected on 2026-09-28 to list the Genesis rates separately.
    Alternative: keep 60.0988 Hz (0.3% faster vibrato and software envelopes than a real
    game).
29. LFO divider and AM phase provenance (verification 2026-09-28). The divider table
    108 77 71 67 62 44 8 5 and the AM triangle phase (maximum attenuation at counter 0)
    come only from jsgroth part 6, which could not be re-read (bot filter, no archive
    access). The OPNA manual gives 8 MHz frequencies whose implied dividers (109.05,
    78.06, 72.10, 68.14, 63.09, 45.07, 9.02, 6.01) are about one larger for settings 0-5
    and differ by 1.0 for settings 6-7. Decision unchanged (divider table). Alternative:
    dividers 109 78 72 68 63 45 9 6 derived from the manual.
30. Tone period 2 and up follow the datasheet formula; period 0 is covered by 20. The
    datasheet noise Table 3 misprints row 2 as "0 0"; resolved by row order and by
    Maxim's reload table (no decision needed).

Added during implementation (2026-09-28, no new source consulted; each point follows from
the sections above or is left unspecified by them):

31. AM while the LFO is disabled. "LFO" says disabling holds the counter at 0, and "Amplitude
    modulation" (jsgroth part 6, [unverified] phase) puts the maximum attenuation at counter
    0. Taken together, an operator with its AM bit set and AMS > 0 is attenuated by the full
    AMS depth (up to 11.8 dB) while the LFO is off. Decision: implement literally (the core
    only holds the counter; `amAttenuation(0, ams)` is the maximum). Alternative: AM offset 0
    while the LFO is disabled (would need a source saying the AM output is gated; the sources of
    Ambiguities 38 say it is not).
32. $A4-$A6 latch scope. The sources say the high byte is latched until the low byte is
    written, not whether there is one latch per channel or one shared latch. Decision: one
    latch per channel. Alternative: a single latch per bank (a $A5 write followed by a $A0
    write would apply channel 2's block to channel 1). Identical for drivers that always
    write each pair in order, which ours does.
33. SSG-EG inversion flag at key-on. "SSG-EG" says key-off clears the flag; nothing is said
    about key-on. Decision: key-on also clears it (each note starts with the shape's own
    polarity). Alternative: keep the flag across notes (A/E triangles could start mirrored).
34. SSG-EG hold-high overshoot. With the x4 rule the attenuation can step past 0x200 (for
    example 508 + 8 = 516 with the mixed 1/2 increment patterns of rates 49-51); in the hold
    modes B and D the output is then `(0x200 - att) & 0x3FF`, which wraps to near 0x3FF
    (near silence) instead of holding at maximum level. Decision: implement the formulas
    literally (fast rates whose increments divide 0x200 hold at maximum as expected).
    Alternative: clamp the attenuation to 0x200 when the gate is reached.
35. Key edge timing. The core applies each $28 write immediately between two FM samples, so a
    key-off immediately followed by a key-on still produces an off -> on edge (new attack,
    phase reset). On hardware the key state is sampled when each operator slot is processed;
    an off/on pair written faster than one sample (18.8 us) could be missed. Real drivers write
    a whole patch between the two (many register writes with busy waits), so the edge is
    seen. Decision: edge per write. Alternative: sample the key state per operator slot.
36. PSG noise in tone-3 mode with a tone-3 period of 0. The counter model (reload 0, toggle when
    the counter is 0) makes it behave like period 1 (shift every 2 ticks). Maxim's "period 0
    behaves like 1" statement is for tone output only. Decision: counter model as is.
37. Output coupling capacitor. No source gives the Genesis output high-pass. Decision: a 5 Hz
    one-pole DC blocker (`kDcBlockHz`), which removes the ladder offsets and the unipolar PSG
    DC and is below the audio band. Alternative: a measured value from a Model 1 recording.

Added during the fidelity review (2026-09-28):

38. AM while the LFO is disabled (continues 31). Nemesis (Sources 30): AM is still applied with
    the LFO off and the waveform is "locked", which can attenuate an AMS-3 operator strongly;
    Eke (Sources 31): the level is held while disabled and reset when the LFO is enabled; jsgroth
    part 6 (not re-readable): disabling holds the counter at 0; MAME (as reported by Nemesis):
    AM/PM reset to none on disable. All hardware-oriented sources agree that AM keeps acting, so
    a constant attenuation of up to 11.85 dB (AMS 3, AM bit set) with the LFO off is authentic.
    They differ on the frozen position: counter 0 (jsgroth) vs the position at the moment of
    disabling (Nemesis "locked", Eke "held"). Decision unchanged: counter held at 0 (identical
    for the power-on state and for any patch that never enables the LFO, which is how this engine
    is normally driven), covered by the test "AM stays applied while the LFO is disabled".
    Alternative: freeze the counter at its current value on disable and reset it to 0 on enable
    (changes only what a song hears after turning the LFO off mid-note).
39. DAC software volume rounding. "Velocity" gives `round(velocity * dac_volume) / 127` but not how
    the scaled sample is rounded. This is driver behaviour (no hardware source applies).
    Decision: round the product to nearest (`128 + round(c * gain / 127)`, symmetric around
    0x80; 127 is odd so there are no ties). Alternative: truncation towards zero (the earlier
    code), which rounds the two halves in opposite directions.
40. Unison retrigger. "Software features" says the partner follows the owner; it does not say
    what a repeated note on the owner does. Decision: the owner keeps and re-keys its own
    partner (same channel), so a retrigger never occupies extra channels. Alternative: release
    the old partner and key the next free channel (the earlier behaviour: the partner walked up
    one channel per retrigger while the old ones were still releasing).
41. Operator pipeline delay of S1 as a modulator (raised 2026-09-29, decided 2026-09-30). "Evaluation order quirk"
    derives the delayed paths from the S1, S3, S2, S4 order (jsgroth part 4 excerpt, Nemesis
    page 13): S2->S3, S1->S3 and S2->S4 use the previous sample, S1->S2, S1->S4 and S3->S4 the
    current one. The differential check against two independent reference emulators run as
    black boxes (Nuked OPN2 and the MAME / Genesis Plus GX core in VGMPlay 0.40.9, see
    `reference-emulators.md` and `refcheck-report.md`) disagrees for every path that starts at
    S1: both references agree with each other within 0.2 dB and differ from us by up to 9.5 dB
    (fm_alg0 H1) on static algorithms 0-3. Adding one sample to every S1 path (S1->S2 1,
    S1->S3 2, S1->S4 1; S2 and S3 paths unchanged) in a scratch build brings all eight
    algorithms within 0.16 dB of Nuked. Plausible mechanism: other operators read S1 from the
    same one-sample history register its feedback uses (`op1Out[n-1]`). No public text found
    yet that states it. Decision (2026-09-30): adopt the +1 sample on S1 paths, since two
    independent references agree and the derivation from the evaluation order is our own
    reading of an excerpt, not a quoted rule; implemented by `modulatorDelay()` in
    `GenesisTables.h` and `Ym2612Core::computeChannel` (S1 read from `fb1` / `fb2`). The
    rerun then showed the same one-sample delay on S1 as a carrier: against both references an
    S1-alone tone (fm_fb0) sat 0.83 host samples (= 1.00 FM sample at 44.1 kHz) earlier than
    an S4-alone tone (fm_sine_ref) relative to the reference, so S1 now also reaches the
    accumulator from its history (`kS1CarrierDelay` = 1; only algorithm 7 has S1 as a
    carrier). With it, the fractional-lag null against Nuked rose from 28.9 to 49.8 dB on
    fm_fb3 and from 44.6 to 47.7 dB on fm_alg7 (fm_fb0: 55.0 dB). Rerun
    of the differential check: largest harmonic difference against Nuked on fm_alg0..3 went
    from 9.46 / 1.33 / 4.52 / 1.24 dB to the values in `refcheck-report.md` "F1". Alternative:
    the documented derivation (S1->S2 0, S1->S3 1, S1->S4 0), to revisit if a die-level
    description (Nemesis, Sauraen) states otherwise.

## Sources

Name, URL, date consulted (all 2026-09-28), what was taken.

1. Plutiedev, "YM2612 register reference", https://plutiedev.com/ym2612-registers. Register
   bit layouts, S1/S3/S2/S4 register order, $28 channel encoding, detune sign table, TL
   0.75 dB, RR 4-bit note, write order of $A4/$A0, Echo fnum table, LFO 7.67 MHz frequency
   table, AMS/PMS tables, DAC description.
2. Plutiedev, "Common YM2612 operations", https://plutiedev.com/ym2612-operations. Key-on
   sequence, which TLs to change per algorithm, release time note (128 samples), DAC
   playback sequence.
3. Kabuto (TiTAN), "SEGA Mega Drive / Genesis hardware notes" v1.5, mirror at
   https://plutiedev.com/mirror/kabuto-hardware-notes. DAC sampling once per cycle, MD1
   pulse vs MD2 hold, 9th DAC bit, MD1 first-order vs MD2 second-order filter, loud-PCM
   gain 30x/5x, VDP debug bits for PSG channel level.
4. Yamaha, "YM2608 OPNA Application Manual", transcription and translation by Nemesis
   (30/6/2008), https://csclub.uwaterloo.ca/~pbarfuss/YM2608J_Translated.PDF. Register
   tables (Table 2-2), $28, F-number formula and Table 2-4, key code N3/N4, Table 2-5
   multiplier, Table 2-6 detune Hz table, Table 2-7 SL weights (93 dB), Table 2-8 key
   scaling, Table 2-9 TL weights, rate formula, SSG-EG rules and shapes, LFO frequency /
   PMS / AMS tables, Table 2-3 feedback levels, algorithm descriptions, prescaler 1/6.
5. Nemesis, "New Documentation: An authoritative reference on the YM2612", SpritesMind
   thread t=386 page 8, https://gendev.spritesmind.net/forum/viewtopic.php?t=386&start=105.
   Envelope generator article: clocks, 10-bit attenuation weighting, rate calculation,
   Table 1 / Table 2, update pseudocode, attack/decay formulas, ADSR transitions, SSG-EG
   bits and behaviour, durations (10223616 samples / 193.5 s, 312 samples); Eke's exact
   clock values and MAME notes; Shiru's register timing notes.
6. Same thread page 11, ...&start=150. Operator unit: 10-bit inputs, 14-bit output, sine
   and power table generation code, InversePow2, 4.6 -> 4.8 conversion, 48 dB remark;
   Eke's feedback shift notes.
7. Same thread page 12, ...&start=165 (via summary). EG clock = FM clock / 3 (72 FM
   clocks per operator), SL clamp on decay -> sustain, attack skip when (ar + ksr) >= 94
   in MAME terms.
8. Same thread page 13, ...&start=180 (via summary). SSG-EG output not inverted in the
   release phase; parallel operator evaluation (1,3,2,4); feedback averages the last two
   samples.
9. Same thread page 33, ...&start=480 (via summary). Nemesis's phaseModIncrementTable
   (8x8) for fnum bit 9.
10. Same thread page 37, ...&start=550 (via summary). GManiac: operator range -8168..+8168,
    arithmetic shift by 5, levels -1 and -2 identical, level ordering glitches; TmEE: MD2
    uses YM3438, louder output.
11. Same thread page 55, ...&start=810 (via summary). Sauraen: top 9 bits to the
    accumulator, 9-bit DAC, resistor-string explanation; Eke: BUSY flag 32 internal clocks.
12. Same thread page 56, ...&start=825 (via summary). Eke: ladder emulation (+1 offset,
    +3/-3, +4/-4 muted); Sauraen: output multiplexing order; Kabuto: confirmation of the
    large gap.
13. SpritesMind, "How DAC works ?", http://gendev.spritesmind.net/forum/viewtopic.php?t=3049
    (via summary). DAC register is linear; amplification 24x (YM2612) vs 6x (YM3438);
    1 of 6 vs 5 of 6 output cycles; DAC register updated within 4 internal cycles.
14. SpritesMind, "PSG mixing volume?", https://gendev.spritesmind.net/forum/viewtopic.php?t=1631
    (via summary). Divide-by-6 (Nemesis) and ~6.4 (TmEE), -15.56/-16.12 dB, model variation.
15. SpritesMind, "help needed for megadrive audio modding",
    http://gendev.spritesmind.net/forum/viewtopic.php?t=730 (via summary). Jorge Nuno: YM
    about -27 dB below PSG, mixing relations 0.0431 PSG / 1 YM / 0.0468 external, ~1 kOhm
    impedances.
16. Maxim, "Sega Genesis Technical Manual - YM2612 section" (SEGA2.DOC transcription),
    https://www.smspower.org/maxim/Documents/YM2612. Sega register map, $28 channel table,
    DAC statement (only channel 6 stereo bits apply), MUL/DT1 description, TL 0.75 dB, RS
    formula (KC/8 .. KC/1), D1L x8 note, RR double-and-add-one, 8 MHz fnum sequence,
    LFO/AMS/FMS tables, algorithm suggested uses, init example.
17. Maxim, "SN76489", SMS Power, https://www.smspower.org/Development/SN76489 (also
    reached through ?from=Development.PSG). Write protocol, register widths, Genesis
    mapping, power-on state, tone formula, period 0/1, tone range, noise reload values,
    LFSR widths/taps per system, reset 0x8000, periodic mode, implementation snippet,
    2 dB steps and volume table, output decay/polarity notes, PCM notes.
18. Texas Instruments, "SN76489AN" datasheet (scan),
    https://map.grauw.nl/resources/sound/texas_instruments_sn76489an.pdf (pages 2-5 read as
    images). f = N/32n, N/16 counter rate, attenuator weights 2/4/8/16 dB and OFF (28 dB
    max), noise feedback control, shift rates N/512, N/1024, N/2048, tone 3, shift register
    cleared on noise-control write, data formats, 32-clock load time.
19. Charles MacDonald, "Sega Genesis hardware notes" v0.8 (gen-hw.txt),
    https://gendev.spritesmind.net/mirrors/cmd/gen-hw.txt (via summary). NTSC clocks
    (68000/YM2612 7.67 MHz, Z80/PSG 3.58 MHz), PSG on the VDP mixed with the YM2612,
    YM2612 address mirroring.
20. jsgroth, "Emulating the YM2612" parts 1-7 (2025),
    https://jsgroth.dev/blog/posts/emulating-ym2612-part-1/ through .../part-7/. Secondary
    source written from the Nemesis thread and later hardware analysis: exact clocks,
    register decode, DAC bias and shift, discrete vs ASIC console list, 20-bit phase
    counter and shifts, key code, converted detune table, MUL wrap, 12-bit EG counter,
    attack shift formula, rate 62/63 rules, SL 15, TL << 3, sine/exp table construction,
    13-bit magnitude, PM input bits, algorithm delays, feedback shift 10 - FB, 9-bit
    quantisation, ladder offsets (+1, +4/-3, +-4), Model 1 filter cutoffs, LFO dividers,
    AM/PM computation, SSG-EG logic. (Its author credits Nuked-OPN2 for some facts; the
    emulator itself was not opened.)
21. jsgroth, "Genesis & Sega CD - Audio Filtering" (2025),
    https://jsgroth.dev/blog/posts/genesis-sega-cd-audio-filtering/. Model 1 VA0-VA2
    3.39 kHz and VA3-VA6 2.84 kHz first-order filters, scipy coefficients at 53267 Hz,
    application to PSG at 223721 Hz.
22. Wikipedia, "Yamaha YM2612", https://en.wikipedia.org/wiki/Yamaha_YM2612 (via summary).
    9-bit DAC, YM3438 reduced crossover distortion (confirmation only).
23. Internet Archive, "Genesis Technical Overview v1.00 (1991)",
    https://archive.org/stream/Genesis_Technical_Overview_v1.00_1991_Sega_US/..._djvu.txt.
    Consulted; the OCR text ends before the FM register section, nothing taken.
24. ConsoleMods, "Genesis: Audio Chip Notes", https://consolemods.org/wiki/Genesis:Audio_Chip_Notes
    (blocked; search snippet only). Ladder effect naming (HardWareMan), YM3438 in Model 1
    VA7; used only as confirmation of source 20's console list.

25. SpritesMind, "YM2612 shape of envelope attack", http://gendev.spritesmind.net/forum/viewtopic.php?t=932
    (via summary). GManiac's hardware measurement of the attack curve (73 steps to 8168,
    "att += not(att) asr 4"); GreenLine's patent-based alternative.
26. Residual Media, "Forensics: Genesis 2 with original Mega Amp",
    https://residualmedia.net/forensics-genesis-2-with-original-mega-amp/ (via summary).
    315-5684 second-order low-pass with external capacitors on Model 2 VA2-VA4; Mega Amp
    cutoffs (aftermarket, not used).
27. Howel, "SN76489 Sound Generator Chip", https://www.acornatom.nl/sites/atomreview/howel/parts/76489.htm
    (consulted 2026-09-28). LSB-first retranscription of the TI tables: noise NF1 NF0 = 00
    N/512, 01 N/1024, 10 N/2048, 11 tone 3; attenuation bit weights; "shift register is
    cleared". Used to confirm the datasheet bit order.
28. Sega Retro, "Sega Mega Drive/Technical specifications" (via search summary, consulted
    2026-09-28). Refresh rates 59.92274 Hz NTSC / 49.701459 Hz PAL.
29. RadDad772, "Genesis VDP internals, part three: basic timing, DMA, and VRAM access
    slots", https://raddad772.github.io/2024/07/21/genesis-vdp-pt-3.html (consulted
    2026-09-28). "any given scanline takes exactly 3420 master clocks" (NTSC, common modes).
30. Nemesis thread (source 5) page 26, http://gendev.spritesmind.net/forum/viewtopic.php?start=375&t=386
    (via summary, consulted 2026-09-28). Nemesis: with the LFO disabled, AM (and possibly PM)
    is still applied and "the LFO waveform itself is locked"; a frozen value can attenuate an
    AMS-3 instrument strongly (Spider-Man: Separation Anxiety intro relies on it). MAME resets
    AM/PM on disable. Used for Ambiguities 38.
31. Same thread page 33 (source 9), re-read via summary 2026-09-28 for the LFO. Eke: the
    modulation level "is held when LFO is disabled, and is reseted when LFO is enabled" (his
    emulator's behaviour, stated as matching hardware). Used for Ambiguities 38.

Verification pass 2026-09-28: the OPNA manual PDF (source 4) was re-read as extracted text
(pages 24-33: formula, Tables 2-3, 2-4, 2-6, 2-7, 2-8, rate formula, LFO/PMS/AMS); the TI
datasheet (source 18) pages were rendered and read; plutiedev (source 1), Nemesis pages 8
and 33 (sources 5, 9) and Maxim's SN76489 page (source 17) were re-fetched. jsgroth's
posts (sources 20, 21) and ConsoleMods returned bot-filter / 403 pages; only search
excerpts of them were available.

32. Reference emulators run as black boxes (2026-09-29, product-owner decision of the same
    day; no source read): VGMPlay 0.40.9 with Nuked OPN2 / MAME SN76496 and MAME-GPGX YM2612 /
    Maxim SN76489, https://github.com/vgmrips/vgmplay-legacy/releases/tag/0.40.9. Used for
    the differential check only (tools, settings and licences in `reference-emulators.md`,
    results in `refcheck-report.md`); evidence for Ambiguity 41.
33. Preset sound-design references (2026-09-30): nesdoug's FM instrument basics, the Sega
    manual's algorithm suggestions (Maxim's transcription, source list above), plutiedev,
    the Furnace OPN editor page, Chowning 1973 and Sound On Sound "Synth Secrets 13". Used
    for the seed patches only, not for chip behaviour; listed with what was taken in
    `genesis-sound-design.md`.

## Generator script

The Python used for every computed value above (run with `python gen.py`; it prints the
tables verbatim as pasted here):

```python
import math
NTSC, PAL = 53693175, 53203424
def fnum(f, clock, block):
    return 144 * f * (1 << 20) / clock * 2 / (1 << block)
# octave table
for i in range(12):
    f = 440 * 2 ** ((60 + i - 69) / 12)
    print(round(fnum(f, NTSC / 7, 4)), round(fnum(f, PAL / 7, 4)), round(fnum(f, 8e6, 4)))
# key code
def keycode(block, fn):
    f11, f10, f9, f8 = (fn >> 10) & 1, (fn >> 9) & 1, (fn >> 8) & 1, (fn >> 7) & 1
    n3 = (f11 & (f10 | f9 | f8)) | ((1 - f11) & f10 & f9 & f8)
    return (block << 2) | (f11 << 1) | n3
# detune units from OPNA Hz values
units = lambda hz: round(hz * 144 * (1 << 20) / 8e6)
# EG tables
shift = [max(0, 11 - (r >> 2)) for r in range(64)]
def eg_step_decay(a, inc): return min(0x3FF, a + inc)
def eg_step_attack(a, inc): return max(0, a + ((inc * (~a)) >> 4))
# sine / exp
sin_tab = [int(-math.log2(math.sin(((i << 1) + 1) / 512 * math.pi / 2)) * 256 + 0.5) for i in range(256)]
exp_tab = [int(2 ** (-(i + 1) / 256) * 2048 + 0.5) for i in range(256)]
def op_out(phase, att10):
    idx = (~phase & 0xFF) if (phase & 0x100) else (phase & 0xFF)
    total = sin_tab[idx] + (att10 << 2)
    mag = 0 if (total >> 8) >= 13 else (exp_tab[total & 0xFF] << 2) >> (total >> 8)
    return -mag if (phase & 0x200) else mag
# LFO
lfo_div = [108, 77, 71, 67, 62, 44, 8, 5]
lfo_hz = [NTSC / 7 / 144 / 128 / d for d in lfo_div]
# PSG
vol = [round(32767 * 10 ** (-a / 10)) for a in range(15)] + [0]
def lfsr_period(taps, width, reset):
    sr, n = reset, 0
    while True:
        fb = bin(sr & taps).count("1") & 1
        sr = (sr >> 1) | (fb << (width - 1)); n += 1
        if sr == reset: return n
# first-order low-pass (bilinear)
def lpf(fc, fs):
    w = math.tan(math.pi * fc / fs); return w / (1 + w), (w - 1) / (1 + w)
```

## Implementation decisions

How `GenesisEngine` (`dsp/include/chipdsp/genesis/`, `dsp/src/genesis/`) uses this
specification. New ambiguities met while implementing are entries 31-40 above.

### Structure

* `GenesisTables.h`: every table and pure formula of this document (`constexpr`), plus the
  driver helpers (note -> block/fnum, note -> PSG period) and the output constants.
* `Ym2612Core`: register-level YM2612. `write(bank, reg, value)` / `writePort(port, value)`
  (one address latch, one data port); `clockSample()` advances one FM sample: LFO step, SSG-EG
  logic (every sample), EG (every 3rd sample, 12-bit counter skipping 0), then the six channels
  (operators in the order S1, S3, S2, S4 with the pipeline delays of "Evaluation order quirk"
  and the S1 history delay of Ambiguity 41,
  per-carrier `>> 5`, 9-bit clamp, DAC substitution on channel 6). The output stage
  (`channelOutputLeft/Right`, `outputLeft/Right`) applies `ladderOutput()`; `setLadderEffect()`
  is the `chip_revision` switch (0 = discrete YM2612 with ladder, 1 = YM3438/ASIC, linear).
  Rates are recomputed from the registers at every EG update (Ambiguity 6). The decay ->
  sustain test ("Phase transitions") runs at the start of each EG cycle and again right after
  the decay step, with the clamp to SL10, so the attenuation is never seen above SL10 in the
  decay phase. Timers, CSM and
  channel 3 special mode are not modelled ($27 is stored, $A8-$AE are ignored).
* `Sn76489Core`: register-level PSG. `write(byte)` (latch/data protocol), `clock()` = one
  counter tick at PSG clock / 16; outputs `bit * kPsgVolume[att]` per channel (0/+1 model,
  Ambiguity 21).
* `GenesisDriver`: the software sound driver (below). `GenesisEngine`: parameters, time base,
  resampling, output stage.

### Time base and resampling

* Everything is scheduled in master clocks (53.69 / 53.20 MHz): one FM sample every
  7 x 144 = 1008, one PSG tick every 15 x 16 = 240, one driver frame every 3420 x 262 (NTSC) or
  3420 x 313 (PAL), one DAC write every master / `dac_rate`. Events at the same instant run in
  the order frame, DAC write, FM sample, PSG tick. Event times are exact integers in a double,
  rebased every ~18 s.
* After each FM sample the summed L/R output (9-bit units) is compared with the previous one
  and the change goes to a `BandLimitedStepSynth` prepared for the FM rate; after each PSG tick
  the PSG sum goes to a synth prepared for the PSG tick rate. So the FM DAC hold at 53.27 kHz
  and the PSG steps at 223.7 kHz are both band-limited exactly once. The synths use the
  `IntegratedStep` kernel (true band-limited steps, flat pass band; refcheck finding F2: the
  earlier impulse-sum kernel boosted the top octave by (w/2)/sin(w/2), +0.94 dB at 11.2 kHz
  at 44.1 kHz). The synths are prepared
  at `prepare()` for the clock selected then; a later NTSC/PAL switch only changes the time
  base (the kernel cutoff differs by 0.9 %, and cannot be rebuilt on the audio thread).
* Level scale: 6 x 256 nine-bit units = 1.0 (`kOutputScale`); a single FM channel at full scale
  peaks at 0.167. PSG units are multiplied by `kPsgToFmGain` (Ambiguity 17): one PSG channel
  at attenuation 0 = 40 nine-bit units. There is no clipping stage: the sum of all channels
  can exceed 1.0 (the real amplifier would clip; not modelled).
* Output stage after the resampler, per side: DC blocker 5 Hz (Ambiguity 37), then the Model 1
  low-pass when `model1_lowpass` = 1: the first-order RC at 3390 Hz (VA0-VA2), applied to FM
  and PSG alike. It runs at the host rate on the band-limited signal, where the exact answer
  is a digital filter whose response equals the analog RC below the host Nyquist.
  `rcLowPass()` (GenesisTables.h) keeps the RC pole exactly (impulse invariance) and places two
  zeros so that |H|^2 matches 1 / (1 + (f / fc)^2) at DC and at 0.70 and 0.95 of
  min(20 kHz, 0.45 fs): error < 0.14 dB up to that frequency at 22.05-44.1 kHz, < 0.08 dB at
  48 kHz, < 0.001 dB at 96 kHz (tested). Only the magnitude is matched (the phase is that of a
  minimum-phase digital filter and differs from the RC's in the treble). The bilinear form
  (generator `lpf()`, `firstOrderLowPass()`, kept for the native-rate reference coefficients)
  was used before; it squeezes the response towards the host Nyquist (-3.4 dB too low at
  15 kHz at 48 kHz, -4.3 dB at 44.1 kHz), so the filter sounded different at each host rate.
  Running it at the native FM rate would not fix that (the bilinear warping at 53.3 kHz is
  still -1 dB at 10 kHz) and would need the filtered output emitted as steps at every chip
  event. The shared `OnePoleLowPass` (`util/Filters.h`) was not used: its
  `alpha = dt / (RC + dt)` form puts a nominal 3390 Hz cutoff at about 2.8 kHz at 48 kHz.
  The 2.84 kHz (VA3-VA6) variant is not exposed (`model1_lowpass` is 0..1 in ENGINE_SPECS).
* Per-channel outputs: FM channel c alone through the same output stage
  (`ladderOutput(sample, L/R bit, revision)`), PSG channel alone at its mix level (mono, copied
  to both sides). They use their own synths and filters, run only while the host passes
  buffers, and sum to the main output (everything after the chips is linear; tested).

### Driver tick model (software, not chip behaviour)

* The driver ticks once per video frame: 59.92274 Hz NTSC, 49.70146 Hz PAL (Ambiguity 28;
  not the NES rates listed in ENGINE_SPECS). The first tick happens at time 0 after `reset()`.
* `noteOn` / `noteOff` are handled at once, i.e. at the start of the next rendered block, like
  a driver that executes a command as soon as it receives it (a real game would wait for the
  next frame, up to 16.7 ms of jitter, which a plugin should not add). The register sequence
  is the documented one: `$28` key off, patch registers, `$A4` then `$A0`, `$28` key on for
  the four operators (`0xF0 | channel code`). A new note on a sounding channel therefore
  re-attacks from the current attenuation (no reset, Ambiguity 4).
* Every frame the driver, for each channel in use: advances the vibrato, writes the patch
  registers whose value changed (it keeps a shadow of every register written, like most
  drivers), rewrites the frequency if block/fnum changed (glide from `setChannelPitch`,
  vibrato), and writes `$22` (LFO) and `$2B` (DAC enable) when they change. PSG: advances the
  software envelopes and writes attenuation/period bytes only on change; the noise control
  register is written at note on and when `psg_noise_mode` / `psg_noise_rate` change (each
  write resets the LFSR, which is audible and authentic).
* `setParameter` stores the value; it reaches the chips at the next frame (or the next note
  on, which re-reads all parameters). `chip_revision`, `model1_lowpass` and `clock` are
  applied at the start of the next block.

### MIDI note -> registers

* FM: `note + transpose + fine_tune / 100` goes through "MIDI note to (block, fnum) rule"
  (one block per octave, `block = clamp(note / 12 - 1, 0, 7)`, fnum rounded to nearest,
  clamped to 2047). Software vibrato and unison detune are then added in fnum units and the
  result is clamped to 0..2047 without changing the block, so the fnum grid is always audible.
  Fractional notes (glide) use the same rounding.
* PSG tone: `period = round(psg_clock / (32 f))` for `note + psg_transpose`, plus vibrato and
  unison offsets in period units, clamped to 0..1023 (periods 0/1 give the documented constant
  output, Ambiguity 20).
* PSG noise (channel 9): rates 0-2 ignore the note. With `psg_noise_rate` = 3 the note sets
  tone 3's period (the noise clock) and the driver mutes tone 3 and keeps it for itself while
  the noise note sounds (channel 8 notes are ignored meanwhile and it reports active; a tone-3
  note already sounding ends, also when the rate is switched to 3 during the noise note). In
  periodic mode the period is computed for `note + 48`, because a periodic pulse train sounds
  four octaves below the tone-3 frequency ("Noise channel"), so the played note is the heard
  fundamental; white noise uses the note directly.
* DAC (channel 5 when `dac_enable` = 1): the sample in slot `dac_sample` is streamed to `$2A`
  at `dac_rate` Hz, or `dac_rate * 2^((note - 60) / 12)` with `dac_keyed` (clamped to
  100 Hz .. the FM rate). The core samples `$2A` once per FM sample (zero-order hold, aliasing
  kept).

### Velocity

* FM: `round((1 - velocity) * velocity_depth)` TL steps (0.75 dB each) are added to the carrier
  operators of the current algorithm, clamped to 127; modulators are unchanged (so velocity
  changes loudness, not timbre).
* PSG: `round((1 - velocity) * 15)` attenuation steps (2 dB each) are added, clamped to 15.
* DAC: the driver scales the signed sample by `round(velocity * dac_volume) / 127` before
  writing `$2A` (software volume; the DAC itself has none); the scaled value is rounded to
  nearest (Ambiguity 39).

### noteOff and channel activity

* FM: `noteOff` writes `$28` with the four operator bits cleared: the hardware release phase
  at RR. `isChannelActive` stays true until no operator is keyed and every carrier
  attenuation is >= 0x340 (output exactly 0, "Attenuation scale").
* PSG: `noteOff` starts the software release (`psg_sw_release` frames; 0 = attenuation 15 at
  once). Active until the envelope reaches 0.
* DAC: one-shot samples (`dac_loop` = 0) play to their end regardless of note off (drum
  practice); looped samples stop at note off and `$2A` is set to 0x80. Active while playing.
  At the end of a one-shot sample the last byte stays in `$2A` (DC, removed by the coupling
  stage).

### Software features (driver behaviour, labelled as such)

* Vibrato (`vibrato_*` for FM, `psg_vibrato_*` for PSG): triangle, `rate` frames per half
  cycle, `depth` in fnum / period units, starting at the centre after `vibrato_delay` frames:
  `p = (frames - delay + rate / 2) mod (2 rate)`, offset = `-depth + 2 depth p / rate` for
  `p < rate`, else `depth - 2 depth (p - rate) / rate`, rounded. Rate 0 or depth 0 = off.
  The hardware LFO (`lfo_*`, `ams`, `fms`) is separate and is chip behaviour.
* PSG software envelope (`psg_sw_*`, frames): level 0..15 (15 = full). Attack 0 -> 15 over
  `attack` frames, decay 15 -> `sustain` over `decay` frames, sustain, release from the
  current level to 0 over `release` frames; 0 frames = instant. Written attenuation =
  `min(15, psgN_att + (15 - level) + velocity steps)`.
* Unison (`unison_detune`, `psg_unison_detune`): a note also keys the next free channel of the
  same kind (FM: scanning upwards from the channel and skipping channel 6 in DAC mode; PSG:
  tones only) with the same patch and `+detune` fnum units (FM, higher) or period units (PSG,
  lower). The partner follows the owner's note off and pitch; a note played directly on the
  partner detaches it; a new note on the owner re-keys the same partner (Ambiguity 40).

### Samples

* `loadSample` (message thread) resamples the PCM to the current `dac_rate` (box average over
  each output byte's source span when downsampling, linear interpolation when upsampling) and
  quantises to unsigned 8-bit `round(128 + 127 s)`. A result longer than 65536 bytes returns
  false (the slot budget of ENGINE_SPECS). Changing `dac_rate` later changes the pitch, as it
  would with a ROM sample.
* Two pre-allocated banks of 16 x 64 KiB and an atomic active index (ARCHITECTURE "Sample
  slots"): the loader copies the active bank's other slots into the inactive one, writes the
  new slot and flips the index; the audio thread reads the index at each block start and at
  note on/off. Two loads within one audio block would write the bank the audio thread is
  still reading, so the audio thread also publishes the bank it is reading (`bankInUse`,
  -1 between blocks; published then re-checked against the index, sequentially consistent
  atomics), and the loader yields while `bankInUse` equals the bank it is about to write
  (at most one audio block). The audio thread never waits.

### Real-time behaviour

`renderBlock`, `noteOn`, `noteOff`, `setChannelPitch`, `setParameter`, `reset` do not allocate
(tested with a global `operator new` counter), take no locks and do no I/O. Blocks larger than
the prepared size are split internally.

### Factory DAC samples

* 2026-09-29: the DAC drums (kick, deep kick, snares, clap, hats, tom, cowbell, rim, short
  crash) and the orchestra hit are CC0 recordings (VSCO 2 CE, VCSL; `docs/SOURCES.md`,
  "Samples (CC0 recordings)"), converted by `tools/samplegen/cc0_import.py` at 8000, 11025
  or 16000 Hz like the samples of Mega Drive games, compressed and normalised to -0.5 dBFS
  so that their decays stay above the 8-bit floor of `$2A`. The engine resamples them to
  `dac_rate` at load; above the stored rate that adds no bandwidth, only the finer
  zero-order hold. The voice "uh" and the noise burst stay procedural (22050 Hz).
