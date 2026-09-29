# SNES S-DSP research

Self-contained specification of the Nintendo/Sony S-DSP (SNES APU sound DSP) for the
`SnesDsp` chip core. Everything below was transcribed from public documentation
(fullsnes, Anomie's S-DSP doc, SNESdev wiki, SnesLab wiki, Super Famicom Development
Wiki); no emulator source code was read. Numbers marked `[computed]` were derived with
the Python script described in the section that uses them; numbers marked
`[unverified]` come from memory and must be treated with suspicion (none remain: an
adversarial verification pass on 2026-09-28 re-fetched every source, re-derived every
table with Python and left a "Verified 2026-09-28" line under each confirmed table; the
corrections it made are listed at the end of "Ambiguities").

Notation used throughout:

* `SAR n` / `>> n`: arithmetic shift right (sign preserving, rounds toward minus infinity).
* `clamp16(v)`: saturate to -32768..+32767. `clamp15(v)`: saturate to -16384..+16383.
* `clip16(v)`: keep the low 16 bits and sign-extend (wrap). `clip15(v)`: keep the low 15
  bits and sign-extend: `((v & 0x7FFF) ^ 0x4000) - 0x4000`.
* Anomie's terminology: "clip" = bit truncation (wrap), "clamp" = range restriction.
  fullsnes uses "overflow handling" for clamping and "no overflow handling" for wrapping.
* All integer arithmetic is on values wide enough never to overflow before the explicit
  clamp/clip step (use `int32_t`).

## Clocks and rates

| Item | Value | Source |
|---|---|---|
| APU ceramic resonator | 24 576 000 Hz nominal | SNESdev S-SMP, Anomie |
| S-DSP internal clock | 3 072 000 Hz (= 24.576 MHz / 8) | Anomie, SNESdev S-SMP |
| SPC700 (S-SMP) clock | 1 024 000 Hz (= 24.576 MHz / 24) | Anomie, SNESdev S-SMP |
| Output sample rate | 32 000 Hz = 24 576 000 / 768 = 1 024 000 / 32 = 3 072 000 / 96 | Anomie, SNESdev S-SMP |
| SPC700 cycles per output sample | 32 | Anomie (sample loop cycles 0..31) |
| S-DSP clocks per output sample | 96 | Anomie |
| Real consoles | resonator observed 24 592 800 .. 24 645 600 Hz (Anomie); DAC rate 32 000 .. 32 160 Hz (SNESdev) | Anomie, SNESdev S-SMP |
| Highest reproducible tone | 16 kHz (Nyquist of 32 kHz DAC) | fullsnes |
| Pitch counter range | up to 128 kHz (pitch 0x3FFF), samples above 32 kHz are skipped, not output | fullsnes |
| KON/KOFF polling | every second sample (16 000 Hz) | fullsnes, Anomie, SNESdev |
| Echo buffer | one stereo entry (4 bytes) consumed and written per 32 kHz sample | fullsnes, Anomie |

Verified 2026-09-28 against Anomie's doc (24576000 Hz, 3.072 MHz, 1.024 MHz, 96
clocks / 32 SPC700 cycles per sample, 24592800..24645600 Hz spread), SNESdev "S-SMP"
(768 resonator cycles per sample, 32000..32160 Hz), fullsnes ("1000h = 32000Hz", 16 kHz
limit) and SnesLab "S-DSP" (16 kHz limit).

The chip core runs at exactly 32 000 Hz. Per `ARCHITECTURE.md` the 32 kHz output is
handed to `BandLimitedStepSynth` (zero-order hold at 32 kHz, cutoff at 16 kHz).

Sample loop (Anomie): one stereo sample per 32 SPC700 cycles; the 8 voices are
processed interleaved over those cycles, the echo buffer left/right samples are read in
cycles 22/23, master/echo volumes applied in cycles 26/27, the echo buffer written in
cycles 29/30, the global counter updated in cycle 29, noise updated in cycle 30 and
KON/KOFF polled in cycle 30 of every second sample. For a block-based engine the
per-sample ordering that matters is given in "Order of operations per sample" below.

## Registers

128 registers, selected through SPC700 port `$F2` (DSPADDR, 7 bits used) and accessed
through `$F3` (DSPDATA). Addresses `$80..$FF` are read-only mirrors of `$00..$7F`
(Anomie). Setting bit 7 of DSPADDR makes DSPDATA writes ineffective (SNESdev).
Reading DSPDATA returns the 128-byte register memory, which does not always reflect the
internal state (SNESdev; e.g. FLG after reset, KON after a write).

Per-voice registers (`x` = voice 0..7, address `$x0`..`$x9`):

| Addr | Name | R/W | Bits | Meaning |
|---|---|---|---|---|
| `$x0` | VxVOLL | RW | signed 8 | Left volume, -128..+127, negative inverts phase. `SL = (S * VL) >> 7` on the 16-bit sample (Anomie) = `(s15 * VL) >> 6` on the 15-bit sample (fullsnes). |
| `$x1` | VxVOLR | RW | signed 8 | Right volume, same formula. |
| `$x2` | VxPITCHL | RW | 8 | Pitch low byte. |
| `$x3` | VxPITCHH | RW | 6 used | Pitch high bits 8..13; bits 14-15 unused but read back as written. 14-bit pitch `P`, 0 = stopped, 0x1000 = 32 000 Hz native rate, 0x3FFF max. |
| `$x4` | VxSRCN | RW | 8 | Source number = index into the sample directory (entry address `DIR*0x100 + SRCN*4`). Latched at key-on and at loop; changing it while playing has no immediate effect. |
| `$x5` | VxADSR1 | RW | `EDDD AAAA` | bit 7 E: 1 = ADSR, 0 = GAIN. `DDD` decay rate (R = D*2+16). `AAAA` attack rate (R = A*2+1). |
| `$x6` | VxADSR2 | RW | `LLLR RRRR` | `LLL` sustain level (0..7). `RRRRR` sustain rate (R = value, 0 = infinite). |
| `$x7` | VxGAIN | RW | `0VVV VVVV` or `1MMR RRRR` | bit 7 = 0: direct gain, envelope = `V << 4`. bit 7 = 1: `MM` mode (0 linear decrease, 1 exponential decrease, 2 linear increase, 3 bent increase), `RRRRR` rate. |
| `$x8` | VxENVX | R | `0EEE EEEE` | Upper 7 bits of the 11-bit envelope (`E >> 4`). Writable but overwritten every sample. |
| `$x9` | VxOUTX | R | signed 8 | Upper 8 bits of the voice sample after envelope, before VxVOL. Writable but overwritten every sample. |
| `$xA`,`$xB`,`$xE` | - | RW | 8 | Unused; behave as RAM (fullsnes). |
| `$xF` | FIRx (FFCx / COEF) | RW | signed 8 | Echo FIR coefficient x (FIR0 at `$0F` is applied to the oldest sample, FIR7 at `$7F` to the newest). |

Global registers:

| Addr | Name | R/W | Bits | Meaning |
|---|---|---|---|---|
| `$0C` | MVOLL | RW | signed 8 | Left master volume, `ML = (SL * V) >> 7`. -128 overflows (`-0x8000 * -0x80`), avoid (fullsnes). |
| `$1C` | MVOLR | RW | signed 8 | Right master volume. |
| `$2C` | EVOLL | RW | signed 8 | Left echo volume applied to the FIR output before mixing into the main output. |
| `$3C` | EVOLR | RW | signed 8 | Right echo volume. |
| `$4C` | KON | RW | one bit per voice | 1 = key on (envelope = 0, state Attack, restart from the directory start address). Polled every second sample; the internal bits are cleared after the poll. |
| `$5C` | KOFF | RW | one bit per voice | 1 = key off (state Release, envelope -8 per sample). Polled every second sample; acts continuously while set. |
| `$6C` | FLG | RW | `RMEN NNNN` | bit 7 soft reset (all voices to Release with envelope 0, checked every sample), bit 6 mute (DAC output only), bit 5 ECEN: 1 = echo buffer writes disabled (reads continue), bits 0-4 noise clock rate (same 32-entry rate table as envelopes). Internal value `$E0` after power-on/reset regardless of what reads back. |
| `$7C` | ENDX | R (W = clear) | one bit per voice | Set when a BRR block with the end flag is decoded; cleared for the voice on key-on; any write clears all bits. |
| `$0D` | EFB | RW | signed 8 | Echo feedback volume applied to the FIR output before it is mixed into the echo input. |
| `$1D` | - | RW | 8 | Unused, RAM. |
| `$2D` | PMON | RW | bits 1..7 | Pitch modulation enable for voices 1..7 by the output of voice x-1. Bit 0 has no function. |
| `$3D` | NON | RW | one bit per voice | 1 = the voice outputs the noise generator instead of the interpolated BRR sample. |
| `$4D` | EON | RW | one bit per voice | 1 = the voice is also mixed into the echo input. |
| `$5D` | DIR | RW | 8 | Sample directory page: directory at `DIR * 0x100`, up to 256 four-byte entries. |
| `$6D` | ESA | RW | 8 | Echo buffer start page: buffer at `ESA * 0x100`. |
| `$7D` | EDL | RW | low 4 bits | Echo delay: buffer size `EDL * 2048` bytes = `EDL * 512` stereo samples = `EDL * 16 ms`; EDL = 0 gives a 4-byte (1 sample) buffer. |

Verified 2026-09-28 against Anomie's doc (register list `$x0..$x9`, `$0C..$7D`, `$xF`,
bit layouts `edddaaaa`, `lllrrrrr`, `EGGGGGGG`/`Emmggggg`, R = d*2+16, R = a*2+1),
fullsnes (register sections, `$xA/$xB/$xE/$1D` as RAM, EDL "0=4 bytes", MVOL -128
overflow) and SNESdev "S-DSP registers" (bit diagrams, DSPADDR bit 7 note, FLG `$E0`).

C++-ready address constants:

```cpp
// Per-voice register offsets (add voice * 0x10).
constexpr int kRegVolL = 0x0, kRegVolR = 0x1, kRegPitchL = 0x2, kRegPitchH = 0x3,
              kRegSrcn = 0x4, kRegAdsr1 = 0x5, kRegAdsr2 = 0x6, kRegGain = 0x7,
              kRegEnvx = 0x8, kRegOutx = 0x9, kRegFir = 0xF;
// Global registers.
constexpr int kRegMvolL = 0x0C, kRegMvolR = 0x1C, kRegEvolL = 0x2C, kRegEvolR = 0x3C,
              kRegKon = 0x4C, kRegKoff = 0x5C, kRegFlg = 0x6C, kRegEndx = 0x7C,
              kRegEfb = 0x0D, kRegPmon = 0x2D, kRegNon = 0x3D, kRegEon = 0x4D,
              kRegDir = 0x5D, kRegEsa = 0x6D, kRegEdl = 0x7D;
constexpr int kFlgReset = 0x80, kFlgMute = 0x40, kFlgEchoWriteDisable = 0x20, kFlgNoiseMask = 0x1F;
```

Power-on / reset state (Anomie, fullsnes, SNESdev): FLG acts as `$E0` (all voices keyed
off with envelope 0, muted, echo writes disabled, noise rate 0); ENDX = 0; VxENVX and
VxOUTX = 0; the global counter = 0; the noise LFSR = 0x4000; most other registers are
uninitialised on power-on and retain their values on reset. The engine's `reset()`
must set every register to 0 except FLG = `$E0`, then the driver programs the chip.

## Sample directory and BRR block format

Sample directory (fullsnes, Anomie, SNESdev): at `DIR * 0x100`, each entry is 4 bytes:

```
Byte 0-1  BRR start address (little-endian), used at key-on
Byte 2-3  BRR loop address  (little-endian), used when a block with the end flag is reached
```

Entry for voice x: `DIR * 0x100 + VxSRCN * 4`. DIR and SRCN are only read at key-on and
at loop time (fullsnes, Anomie: register load step S1/S2 "if necessary"). Loop points
are therefore at 9-byte block granularity (16 samples, SNESdev).

BRR block (9 bytes, fullsnes, Anomie, SNESdev, SnesLab):

```
Byte 0   header  SSSS FFLE
           bits 7-4  S = shift 0..12 valid, 13..15 special (see decoding)
           bits 3-2  F = filter 0..3
           bit  1    L = loop flag
           bit  0    E = end flag
Byte 1   sample 0 (high nibble), sample 1 (low nibble)
Byte 2   sample 2, sample 3
...
Byte 8   sample 14, sample 15
```

Nibbles are signed 4-bit two's complement, -8..+7, high nibble first.

End/loop codes (fullsnes "Loop/End flags", Anomie):

| L E | Code | Behaviour |
|---|---|---|
| 0 0 | 0 | Normal: continue with the next 9-byte block. |
| 0 1 | 1 | End + mute: jump to the loop address, set ENDX bit, enter Release with envelope = 0 immediately. |
| 1 0 | 2 | Ignored, same as code 0 (loop flag has no effect without the end flag). |
| 1 1 | 3 | End + loop: jump to the loop address, set ENDX bit, keep playing. |

Verified 2026-09-28 against fullsnes ("Loop/End flags" codes 0..3, header bit layout),
Anomie ("ssssffle", 'l' = "really don't end", 'e' = "really loop") and SNESdev "BRR
samples" (block table, "loop flag has no effect on blocks without the end flag").

Anomie: BRR decoding never stops for a voice. KOFF and FLG.7 only affect the envelope;
a block with E set always continues at the loop address (also for code 1, whose voice is
silenced by the envelope only). Only KON restarts decoding. With code 1 the voice enters
Release with envelope 0 as soon as the header is loaded: because of the 12-sample decode
look-ahead the samples of that final block are never output.

Recommended encoder practice (fullsnes, SnesLab, SNESdev): the first block of a sample
and the first block at the loop address should use filter 0 (the decoder history is
undefined there, and at key-on the history holds the last samples decoded before the
key-on, Anomie).

## BRR decoding

Per nibble `n` (signed -8..+7) with header shift `S` (fullsnes, Anomie, SnesLab):

```
if S <= 12:  RD = (n << S) >> 1          // 16-bit intermediate, arithmetic shift
else:        RD = (n >> 3) << 11         // S = 13..15: 0 for n >= 0, -2048 (0xF800) for n < 0
```

fullsnes phrases the S >= 13 case as "decode as if shift = 12 and nibble = nibble SAR 3",
which is the same expression. Shift 0 loses the low bit (n = 7 gives 3, n = 1 gives 0).

Filters. `old` = previous decoded sample S(x-1), `older` = S(x-2); both are the 15-bit
values from earlier outputs (carried across blocks and groups, separate per voice).
Nominal coefficients (SNESdev): filter 1: 15/16; filter 2: 61/32 and -15/16;
filter 3: 115/64 and -13/16. Exact integer evaluation (Anomie, identical in fullsnes):

```
Filter 0: new = RD
Filter 1: new = RD + old + ((-old) >> 4)
Filter 2: new = RD + (old << 1) + ((-((old << 1) + old)) >> 5) - older + (older >> 4)
Filter 3: new = RD + (old << 1) + ((-(old + (old << 2) + (old << 3))) >> 6)
             - older + (((older << 1) + older) >> 4)
```

The term-by-term forms (`old*1 + ((-old*1) SAR 4)`, `old*2 + ((-old*3) SAR 5)`,
`old*2 + ((-old*13) SAR 6)`, `-older + ((older*1) SAR 4)`, `-older + ((older*3) SAR 4)`)
in fullsnes are the same expressions. Order of operations: evaluate every term in wide
integers, sum, then

```
new = clamp16(new)        // saturate to -32768..+32767
new = clip15(new)         // ((new & 0x7FFF) ^ 0x4000) - 0x4000 : 15-bit wrap
old, older = new, old     // history uses the 15-bit result
```

fullsnes describes the same two steps as "If new > +7FFFh then new = +7FFFh (but clipped
to +3FFFh below); if new < -8000h then new = -8000h (but clipped to ZERO below); if new is
+4000h..+7FFFh then new = -4000h..-1; if new is -8000h..-4001h then new = 0..-3FFFh".
SnesLab: the output wraps once past +0x3FFF / -0x4000 and then clips. Anomie: "Certain
games do seem to depend on these exact formulas". The resulting 15-bit `new` is what the
Gaussian interpolator sees. Encoders must keep `new` within -0x3FFA..+0x3FF8 to avoid
Gaussian overflow (fullsnes).

Verified 2026-09-28: shift formula and the S = 13..15 rule against fullsnes ("(nibble SHL
shift) SAR 1", "as if shift=12 and nibble=(nibble SAR 3)"), Anomie ("RD=(D<<shift)>>1",
"0x0000 or 0xF800") and SnesLab "BRR" ("(nibble >> 3) << 11"); the four integer filter
formulas character by character against Anomie's C forms and fullsnes's term forms;
nominal coefficients against SNESdev "BRR samples"; clamp16-then-clip15 against Anomie
("clamped to 16 bits at the end and then clipped to 15 bits") and the fullsnes quote;
all worked examples recomputed with Python from these formulas (see "Worked examples").

Decode granularity (Anomie): each voice has a 12-sample ring buffer of decoded samples,
three groups of 4. Two groups are active (8 samples), one is the reserve. BRR data is
decoded 4 samples at a time (one byte pair) whenever the interpolation index passes
0x4000: the ring turns and a new group is decoded into the freed group. Header bytes are
loaded every sample; the E/L check happens when the header of a new block is loaded.

Key-on sequence (Anomie), counting output samples from the KON poll:

```
#0  final pre-KON sample; envelope := 0, state := Attack, interpolation index := 0;
    the last pre-KON BRR decode still happens (matters if the new sample's first block
    is not filter 0)
#1  first 0x0000 output sample; start address read from the directory
#2  0x0000; first BRR group (4 samples) decoded
#3  0x0000; second group decoded
#4  0x0000; third group decoded
#5  0x0000; envelope updating begins, interpolation position still 0
#6  first real sample; the first interpolation position update happens this sample
```

So there are 5 samples of silence after key-on before the first data sample; the
interpolator starts "centred on the second sample" of the ring (SNESdev), i.e. with
index 0 the output equals `gauss[511] * s1 + gauss[255] * s0 + gauss[256] * s2 >> 11`.

## Pitch counter and pitch modulation

14-bit pitch `P` = `VxPITCHH[5:0] << 8 | VxPITCHL` (mask with 0x3FFF). Step per output
sample in 4.12 fixed point: the interpolation index advances by `P` each sample, the
integer part is `index >> 12`, the fraction `index & 0xFFF`.

Playback rate of the BRR data: `rate = 32000 * P / 4096` Hz (SNESdev: `VxPITCH * 32000 /
$1000`; Anomie: `Fout = Fin * P / 0x1000`). P = 0x1000 plays a sample at its native
32 kHz; 0x2000 one octave up; 0x0800 one octave down; 0x3FFF just under two octaves up
(fullsnes: "3FFFh = fastest", 128 kHz); 0 halts the voice.

Register value for a MIDI note relative to the sample's root note at 32 kHz:
`P = round(4096 * 2^((note - root) / 12))`, clamped to 0..0x3FFF. The 4.12 grid means
one register step is 1/4096 of the native rate (0.42 cents at P = 0x1000, 1.7 cents at
P = 0x400); the engine must not smooth it.

Pitch modulation (Anomie; SNESdev quotes the same formula; fullsnes gives an equivalent
factor form):

```
pitch = P
if (PMON bit x set) and (x > 0) and (NON bit x clear):
    pitch += ((OUTX16[x-1] >> 5) * P) >> 10        // signed arithmetic
index += pitch                                     // unsigned
if index > 0x7FFF: index = 0x7FFF
while index >= 0x4000: decode next BRR group; index -= 0x4000
```

`OUTX16[x-1]` is the previous voice's 16-bit sample after the envelope and before VxVOL
(the 15-bit envelope output shifted left by 1; Anomie: `voice[x-1].outbuffer`, SNESdev:
"the 16-bit output of the previous voice"). fullsnes writes the same computation on the
15-bit value: `Factor = (OUTX15 >> 4) + 0x400` (range 0..0x7FF, i.e. 0.00..1.99),
`Step = (P * Factor) >> 10`; both forms give identical results for every input
`[computed]`, because `P * 0x400` is an exact multiple of 1024.

The modulated step can exceed 0x3FFF (up to 0x7FEE for P = 0x3FFF and maximum positive
OUTX, fullsnes "0..256 kHz"); Anomie states the pitch variable is wide enough not to wrap
and only the interpolation index is clamped to 0x7FFF. fullsnes marks "somewhere here
STEP or the counter result is cropped to 128 kHz max" as unknown. See Ambiguities.
A voice that is not playing outputs zeros and therefore does not modulate. Voice 0 cannot
be modulated, voice 7 cannot be a source (SNESdev). VxVOL of the source voice has no
effect on modulation (set it to 0 for a silent modulator).

Anomie's pitch code (`PMON & ~NON & ~1 & (1 << x)`) disables modulation for a voice that
has NON set, while his NON text says PMON still controls BRR decode speed for noise
voices. See Ambiguities.

## Gaussian interpolation

Interpolation runs on the 4 most recent 15-bit BRR samples with the fraction
`d = (index >> 4) & 0xFF` (bits 4..11 of the interpolation index) as table index. The
512-entry table (12-bit unsigned values; transcribed from fullsnes and verified entry by
entry against SnesLab's table and Anomie's listing, all three identical `[computed]`):

```cpp
// S-DSP Gaussian interpolation table, 512 x 12-bit (fullsnes "Gauss table", SnesLab
// "S-DSP/Gaussian Filter", Anomie "gauss_coeffs"). Index 0..255 = entries 000h..0FFh,
// 256..511 = entries 100h..1FFh.
constexpr int16_t kGaussTable[512] = {
    0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000,
    0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x002,0x002,0x002,0x002,0x002,
    0x002,0x002,0x003,0x003,0x003,0x003,0x003,0x004,0x004,0x004,0x004,0x004,0x005,0x005,0x005,0x005,
    0x006,0x006,0x006,0x006,0x007,0x007,0x007,0x008,0x008,0x008,0x009,0x009,0x009,0x00A,0x00A,0x00A,
    0x00B,0x00B,0x00B,0x00C,0x00C,0x00D,0x00D,0x00E,0x00E,0x00F,0x00F,0x00F,0x010,0x010,0x011,0x011,
    0x012,0x013,0x013,0x014,0x014,0x015,0x015,0x016,0x017,0x017,0x018,0x018,0x019,0x01A,0x01B,0x01B,
    0x01C,0x01D,0x01D,0x01E,0x01F,0x020,0x020,0x021,0x022,0x023,0x024,0x024,0x025,0x026,0x027,0x028,
    0x029,0x02A,0x02B,0x02C,0x02D,0x02E,0x02F,0x030,0x031,0x032,0x033,0x034,0x035,0x036,0x037,0x038,
    0x03A,0x03B,0x03C,0x03D,0x03E,0x040,0x041,0x042,0x043,0x045,0x046,0x047,0x049,0x04A,0x04C,0x04D,
    0x04E,0x050,0x051,0x053,0x054,0x056,0x057,0x059,0x05A,0x05C,0x05E,0x05F,0x061,0x063,0x064,0x066,
    0x068,0x06A,0x06B,0x06D,0x06F,0x071,0x073,0x075,0x076,0x078,0x07A,0x07C,0x07E,0x080,0x082,0x084,
    0x086,0x089,0x08B,0x08D,0x08F,0x091,0x093,0x096,0x098,0x09A,0x09C,0x09F,0x0A1,0x0A3,0x0A6,0x0A8,
    0x0AB,0x0AD,0x0AF,0x0B2,0x0B4,0x0B7,0x0BA,0x0BC,0x0BF,0x0C1,0x0C4,0x0C7,0x0C9,0x0CC,0x0CF,0x0D2,
    0x0D4,0x0D7,0x0DA,0x0DD,0x0E0,0x0E3,0x0E6,0x0E9,0x0EC,0x0EF,0x0F2,0x0F5,0x0F8,0x0FB,0x0FE,0x101,
    0x104,0x107,0x10B,0x10E,0x111,0x114,0x118,0x11B,0x11E,0x122,0x125,0x129,0x12C,0x130,0x133,0x137,
    0x13A,0x13E,0x141,0x145,0x148,0x14C,0x150,0x153,0x157,0x15B,0x15F,0x162,0x166,0x16A,0x16E,0x172,
    0x176,0x17A,0x17D,0x181,0x185,0x189,0x18D,0x191,0x195,0x19A,0x19E,0x1A2,0x1A6,0x1AA,0x1AE,0x1B2,
    0x1B7,0x1BB,0x1BF,0x1C3,0x1C8,0x1CC,0x1D0,0x1D5,0x1D9,0x1DD,0x1E2,0x1E6,0x1EB,0x1EF,0x1F3,0x1F8,
    0x1FC,0x201,0x205,0x20A,0x20F,0x213,0x218,0x21C,0x221,0x226,0x22A,0x22F,0x233,0x238,0x23D,0x241,
    0x246,0x24B,0x250,0x254,0x259,0x25E,0x263,0x267,0x26C,0x271,0x276,0x27B,0x280,0x284,0x289,0x28E,
    0x293,0x298,0x29D,0x2A2,0x2A6,0x2AB,0x2B0,0x2B5,0x2BA,0x2BF,0x2C4,0x2C9,0x2CE,0x2D3,0x2D8,0x2DC,
    0x2E1,0x2E6,0x2EB,0x2F0,0x2F5,0x2FA,0x2FF,0x304,0x309,0x30E,0x313,0x318,0x31D,0x322,0x326,0x32B,
    0x330,0x335,0x33A,0x33F,0x344,0x349,0x34E,0x353,0x357,0x35C,0x361,0x366,0x36B,0x370,0x374,0x379,
    0x37E,0x383,0x388,0x38C,0x391,0x396,0x39B,0x39F,0x3A4,0x3A9,0x3AD,0x3B2,0x3B7,0x3BB,0x3C0,0x3C5,
    0x3C9,0x3CE,0x3D2,0x3D7,0x3DC,0x3E0,0x3E5,0x3E9,0x3ED,0x3F2,0x3F6,0x3FB,0x3FF,0x403,0x408,0x40C,
    0x410,0x415,0x419,0x41D,0x421,0x425,0x42A,0x42E,0x432,0x436,0x43A,0x43E,0x442,0x446,0x44A,0x44E,
    0x452,0x455,0x459,0x45D,0x461,0x465,0x468,0x46C,0x470,0x473,0x477,0x47A,0x47E,0x481,0x485,0x488,
    0x48C,0x48F,0x492,0x496,0x499,0x49C,0x49F,0x4A2,0x4A6,0x4A9,0x4AC,0x4AF,0x4B2,0x4B5,0x4B7,0x4BA,
    0x4BD,0x4C0,0x4C3,0x4C5,0x4C8,0x4CB,0x4CD,0x4D0,0x4D2,0x4D5,0x4D7,0x4D9,0x4DC,0x4DE,0x4E0,0x4E3,
    0x4E5,0x4E7,0x4E9,0x4EB,0x4ED,0x4EF,0x4F1,0x4F3,0x4F5,0x4F6,0x4F8,0x4FA,0x4FB,0x4FD,0x4FF,0x500,
    0x502,0x503,0x504,0x506,0x507,0x508,0x50A,0x50B,0x50C,0x50D,0x50E,0x50F,0x510,0x511,0x511,0x512,
    0x513,0x514,0x514,0x515,0x516,0x516,0x517,0x517,0x517,0x518,0x518,0x518,0x518,0x518,0x519,0x519,
};
```

Verified 2026-09-28: the 512 entries above were extracted by script from the fullsnes
HTML listing, from Anomie's `gauss_coeffs[512]` (apudsp.txt) and from the SnesLab
"S-DSP/Gaussian Filter" table (512 rows, hex and decimal columns cross-checked against
each other) and diffed entry by entry against this listing: 0 differences in all three;
sum 262146 and maximum 0x519 recomputed from each source independently. The table is
monotonically non-decreasing.
Verified 2026-09-28 (second pass) against fullsnes "4-Point Gaussian Interpolation",
Anomie apudsp.txt `gauss_coeffs[512]` and SnesLab "S-DSP/Gaussian Filter" (512 rows, hex
and decimal columns consistent): re-parsed by a new script, 0 differences, sum 262146,
quad-sum statistics (168 / 46 / 42) and the 0x801 list reproduced.

Table checksum `[computed from the transcription above]`: `kGaussTable[0] = 0x000`,
`kGaussTable[255] = 0x172`, `kGaussTable[256] = 0x176`, `kGaussTable[511] = 0x519`,
sum of all 512 entries = 262146 (0x40002), maximum 0x519 = 1305. The four coefficients
used for one output (`g[255-d] + g[511-d] + g[256+d] + g[d]`) sum to 0x7FF, 0x800 or
0x801 (fullsnes: "should sum up to 800h, but in practice 7FFh..801h"); 168 of the 256
fractions sum to exactly 0x800, 46 to 0x801 (d = 0, 1, 9, 16, 18, 20, 28, 36, 39, 40, 41,
48, 58, 67, 71, 76, 78, 88, 90, 101, 113, 118, 121, 134, 137, 142, 154, 165, 167, 177, 179,
184, 188, 197, 207, 214, 215, 216, 219, 227, 235, 237, 239, 246, 254, 255) and 42 to 0x7FF
`[computed]`. The 0x801 cases are the ones that can overflow.

Interpolation formula (Anomie; the reference for this implementation). `h[0..3]` are the
four samples oldest..newest, `d` the fraction:

```
out  = (g[255 - d] * h[0]) >> 11      // oldest
out += (g[511 - d] * h[1]) >> 11
out += (g[256 + d] * h[2]) >> 11
out  = clip15(out)                     // the first three terms wrap at 15 bits signed
out += (g[d]       * h[3]) >> 11      // newest
out  = clamp15(out)                    // the last addition saturates
```

Sample naming: with fraction 0 the coefficients are `g[255] = 0x172, g[511] = 0x519,
g[256] = 0x176, g[0] = 0`, so the output is centred on `h[1]`: `h[1]` is the sample at
the current integer position n, `h[0]` = n-1, `h[2]` = n+1, `h[3]` = n+2, and `d`
interpolates toward n+1.

fullsnes gives the same computation in the 16-bit domain: each term `>> 10` instead of
`>> 11`, wrap at 16 bits after adding the second and the third term, clamp16 after adding
the fourth term, then `SAR 1`. fullsnes counts the three additions between the four
terms, not the terms: "the 1st addition can't overflow, the 2nd can overflow when
i = 0..1Fh, the 3rd can overflow when i = 20h..FFh; the 2nd one bugs, the 3rd one is
saturated", i.e. term1 + term2 never overflows, + term3 wraps, + term4 saturates.
The two formulations differ only in where the low bit is truncated: over 200 000 random
inputs 66 % of the results differ by 1 or 2 LSB of the 15-bit output `[computed]`; the
overflow behaviour is identical. See Ambiguities; the Anomie form is recommended.

Verified 2026-09-28: Anomie's `>> 11` / `((outx & 0x7FFF) ^ 0x4000) - 0x4000` / CLAMP15
formula, `d = (interpolation_index >> 4) & 0xff` and the `255-d`, `511-d`, `256+d`, `d`
indexing copied from apudsp.txt; the fullsnes 16-bit form from its "4-Point Gaussian
Interpolation" table; both re-implemented in Python and compared over 200 000 random
inputs (66.4 % differ, never by more than 2 LSB of the 15-bit result; identical overflow
behaviour); the quad-sum statistics (168 x 0x800, 46 x 0x801, 42 x 0x7FF) and the list
of 0x801 fractions recomputed from the table.

Documented overflow (fullsnes, SnesLab, SNESdev): with three or more consecutive
`-8 << 12` samples (-0x4000) decoded with filter 0, some outputs come out as +0x3FF8
instead of -0x4000 (a loud click). Reproduced below in the worked examples.

Noise voices bypass interpolation (fullsnes, Anomie): the noise value replaces the
interpolated sample.

## Envelope (ADSR/GAIN) and the global counter

The envelope `E` is 11 bits unsigned, 0..0x7FF. It multiplies the interpolated (or noise)
15-bit sample: `S = (S * E) >> 11` (Anomie). Envelope adjustments happen only on samples
where the global counter selects the voice's current rate `R` (0..31).

Rate table: number of 32 kHz samples between two envelope steps (fullsnes, Anomie,
SNESdev "DSP Period Table"; all identical):

```cpp
// Envelope / noise rate periods in samples, index = 5-bit rate. 0 = never.
constexpr int kRatePeriod[32] = {
       0, 2048, 1536, 1280, 1024,  768,  640,  512,
     384,  320,  256,  192,  160,  128,   96,   80,
      64,   48,   40,   32,   24,   20,   16,   12,
      10,    8,    6,    5,    4,    3,    2,    1,
};
// Counter offset per rate (Anomie "counter_offsets", SNESdev "DSP Period Offset").
constexpr int kRateOffset[32] = {
       0,    0, 1040,  536,    0, 1040,  536,    0,
    1040,  536,    0, 1040,  536,    0, 1040,  536,
       0, 1040,  536,    0, 1040,  536,    0, 1040,
     536,    0, 1040,  536,    0, 1040,    0,    0,
};
```

Verified 2026-09-28: `kRatePeriod` against Anomie's `counter_rates[32]`, fullsnes's
"ADSR/Gain (and Noise) Rates" table (00h..1Fh) and SNESdev "DSP Period Table" (all
identical); `kRateOffset` against Anomie's `counter_offsets[32]` and SNESdev "DSP
Period Offset". One discrepancy between those two sources: for rate 30 SNESdev lists
536 where Anomie lists 0. Rate 30 has period 2 and 536 mod 2 = 0, so both give the same
event times; the table above keeps Anomie's 0. Rate 31 (period 1) is 0 in both.
Verified 2026-09-28 (second pass) against Anomie `counter_rates`/`counter_offsets`
(re-read from apudsp.txt), fullsnes "ADSR/Gain (and Noise) Rates" and SNESdev "DSP
envelopes" period and offset tables; every rate simulated over 61440 samples.

Every period is 1, 3 or 5 times a power of two (fullsnes). Global counter (Anomie,
SNESdev): a single counter shared by all voices and the noise generator, initialised to
0 on reset, decremented by one every sample, wrapping from 0 to 0x77FF (= 30719, so the
counter cycles through 30720 values; Anomie writes "counts from 0x77FF to zero", SNESdev
writes "$77FF (30,720)" meaning the number of states, not the hex value). SNESdev says the
counter "decrements on each S-SMP clock" and labels its period table in "S-SMP clocks";
its own timing table (attack 0 = 4100 ms = 64 x 2048 events) only works if that clock is
the 32 kHz sample clock, which is what Anomie and fullsnes state (see Ambiguity 17). An
event for rate R fires on a sample when

```
R != 0 and ((counter + kRateOffset[R]) % kRatePeriod[R]) == 0
```

30720 is a multiple of every period, so the spacing is exactly `kRatePeriod[R]` across
wraps `[computed, all 31 rates checked over two wraps]`. The offsets only shift the phase
of the three columns of the table (rates 3n+1 fire when the counter is a multiple of the
period, 3n+2 are shifted by 1040 mod period, 3n are shifted by 536 mod period); a rate
change therefore makes the first period after the change shorter or longer, as on
hardware (Anomie). Anomie also documents an equivalent mask/xor formulation by
Mednafen; the modulo form above is sufficient for this engine. The counter is updated at
cycle 29, before the noise update (cycle 30) and before the next sample's voice envelope
updates.

Envelope steps (Anomie, fullsnes, SNESdev; all agree on the arithmetic):

| Mode | Rate R | Step applied on each counter event |
|---|---|---|
| ADSR Attack, A = 0..14 | `A*2 + 1` | `E += 32` |
| ADSR Attack, A = 15 | 31 (every sample) | `E += 1024` |
| ADSR Decay | `D*2 + 16` | `E -= ((E - 1) >> 8) + 1` (SNESdev: `E -= 1; E -= E >> 8`, identical) |
| ADSR Sustain | `SR` (0 = never) | `E -= ((E - 1) >> 8) + 1` |
| Release (KOFF, FLG.7, BRR end code 1) | 31 (every sample) | `E -= 8` (fullsnes adds: or `E -= 0x800`, i.e. to 0, at BRR end code 1) |
| GAIN direct (`GAIN.7 = 0`) | none | `E = (GAIN & 0x7F) << 4` every sample |
| GAIN mode 0 linear decrease (`GAIN = 100R RRRR`) | R | `E -= 32` |
| GAIN mode 1 exponential decrease (`101R RRRR`) | R | `E -= ((E - 1) >> 8) + 1` |
| GAIN mode 2 linear increase (`110R RRRR`) | R | `E += 32` |
| GAIN mode 3 bent increase (`111R RRRR`) | R | `E += (E < 0x600) ? 32 : 8` |

Verified 2026-09-28 against Anomie (`E+=32`, `E+=1024`, `E-=((E-1)>>8)+1`, `E-=8`,
`E=%GGGGGGG0000`, `E+=(E<0x600)?32:8`), fullsnes ("Step=+32 (or +1024 when Rate=31)",
"Step=-(((Level-1) SAR 8)+1)", "Step=-8", "Level=N*16", gain modes 0..3) and SNESdev
"S-DSP registers"/"DSP envelopes" (`envelope -= 1; envelope -= envelope >> 8`, which
equals `-(((E-1)>>8)+1)` for every E in 0..0x7FF `[computed]`).

After every step `E` is clamped to 0..0x7FF (never wrapped). ADSR is "GAIN loaded with
different values at different times" (Anomie): attack = `%110aaaa1` (linear increase at
rate 2A+1) except A = 15; decay = `%1011ddd0` (exponential at rate 2D+16); sustain =
`%101rrrrr` (exponential at rate SR).

State machine:

* Key-on: `E = 0`, state = Attack (both ADSR and GAIN modes track the state).
* Attack -> Decay: Anomie: when the new value exceeds 0x7FF before clamping (a negative
  new value also triggers it, "CRITICAL NOTE"). fullsnes: "at Level >= 7E0h". Anomie's
  text computes the new value every sample ("These registers are actually used to update
  the envelope every sample. The calculated value is used as follows") and only step 1
  (store E) is gated by the counter; steps 2-4 (Decay -> Sustain, Attack -> Decay, saving
  the pre-clamp value for bent increase) are not. Read literally, the +32 attack stores
  0x7E0 after 63 counter events, and on the next sample the computed value 0x800 exceeds
  0x7FF, so the voice enters Decay with E = 0x7E0 without a 64th step: both sources then
  agree. With +1024 (A = 15, rate 31, an event every sample) E = 0x400 then 0x7FF (0x800
  clamped), Decay after 2 steps in both readings. Recommended: this reading (E = 0x7E0 at
  the start of Decay for A = 0..14). Alternative: gate the Attack -> Decay test on the
  counter event too (64 steps, E = 0x7FF). See Ambiguity 5.
* Decay -> Sustain: Anomie, SNESdev: when the upper 3 bits of E equal SL
  (`(E >> 8) == SL`). fullsnes: `Level <= (SL + 1) * 0x100`. In GAIN mode the comparison
  uses VxGAIN bits 7-5 instead of VxADSR2 bits 7-5 (both sources) and has no audible
  effect; switching to ADSR mid-note then continues in whatever phase was tracked.
* Release: entered by KOFF (polled every second sample), FLG bit 7 (checked every sample,
  also forces E = 0), or a BRR block with E = 1, L = 0 (E forced to 0 immediately).
  Release overrides GAIN and ADSR: `E -= 8` every sample until 0, where it stays until
  the next KON. From 0x7FF this takes 256 samples = 8 ms.
* Direct gain (`ADSR1.7 = 0, GAIN.7 = 0`) sets E directly every sample (fullsnes: "Rate =
  Infinite"), so it can define the starting point for another mode; the DSP reads the
  registers once per sample, so a driver must wait at least one sample (32 SPC700
  cycles) between the direct write and the mode change (fullsnes).
* Bent increase threshold: the comparison against 0x600 uses the previous new value
  before the 0..0x7FF clamp, truncated to 11 bits (Anomie "pre-clamp", fullsnes
  "clipped to 11 bits; a negative value will result in the clipped version being greater
  than 0x600"). This only differs from the clamped E when the previous step went below 0
  or above 0x7FF.
* Register race (SNESdev): the DSP reads ADSR1 in step S2 and ADSR2/GAIN in S3c of the
  same sample; a driver changing the mode bit should write ADSR2/GAIN before ADSR1.

Exact per-sample update order for one voice (Anomie step S3c then S4, executed for every
voice every sample, values latched earlier in the sample; revised 2026-09-28, Ambiguity 24):

```
S3c:
1. s15 = noise or Gaussian sample; env = (s15 * E) >> 11 with E stored on the PREVIOUS
   sample ("Apply the volume envelope. This is the value used for modulating the next
   voice's pitch"); OUTX = env >> 7 ($x9), out16 = env << 1 (PMON source)
2. if FLG.7 or KOFF.x (KOFF only on poll samples): state = Release; if FLG.7: E = 0
3. if the current block's header (loaded every sample, S3b) has E=1, L=0: state = Release, E = 0
   (not on key-on sample #1, which does no header check)
4. if internal KON.x (poll samples): E = 0, state = Attack, restart BRR, clear ENDX.x, start
   the 5-sample delay (KON comes last, so it overrides 2 and 3)
5. compute the new value per the table above (every sample, Anomie "used to update the envelope every sample")
6. if a counter event fired for the rate (always for Release, direct gain has no rate): store clamp(new, 0, 0x7FF) in E
7. every sample: Decay -> Sustain if (E >> 8) == SL; Attack -> Decay if new > 0x7FF or new < 0;
   save new & 0x7FF for the next bent-increase comparison
   (5..7 are skipped on key-on samples #0..#4)
S4:
8. pitch step, BRR decode, block end (a new end block's header sets ENDX.x, Ambiguity 8,
   except on the key-on sample: "this setting of ENDX.x will not override the clearing due
   to KON in step S3c, if both occur during the same sample", Anomie S4)
9. ENVX = E >> 4 ($x8, the value just stored)
```

Consequences `[computed]`: A = 15 outputs E = 0x400 on sample #6 and 0x7FF from #7; KOFF,
FLG.7 and a code-1 header all change the output one sample after the sample on which they
are seen; key-on sample #0 still outputs the old voice level.

ADSR timing from the exact formulas `[computed]`: with the recommended reading the attack
takes 63 steps x period (+1 sample) to reach Decay at E = 0x7E0 (2 steps for A = 15):
A = 0: 129024 samples = 4.032 s; A = 7: 5040 samples = 157.5 ms; A = 14: 189 samples =
5.9 ms; A = 15: 2 samples. (Simulated from a counter reset: Decay is entered after sample
126978 for A = 0, 5018 for A = 7, 190 for A = 14, 2 for A = 15; the first period is shorter
because of the counter phase.) The nominal time to full scale 0x800 is 64 steps: 131072
samples = 4.096 s, 5120 = 160 ms, 192 = 6 ms; these are the manual's values and the
result of the alternative reading. Exponential decrease from 0x7FF to 0 takes
695 steps (values 0x7FF, 0x7F7, 0x7EF, ... in -8 steps while E > 0x700, then -7 ... down
to -1 steps below 0x100). Decay from 0x7FF until `(E >> 8) == SL` takes 0 steps for SL = 7
(0x7FF >> 8 = 7 already matches; the Decay -> Sustain check runs every sample, so the
voice leaves Decay before its first counter event and the first exponential step is
taken in Sustain at rate SR), 32 for SL = 6, 69 for SL = 5, 112 for SL = 4, 163 for
SL = 3, 227 for SL = 2, 312 for SL = 1, 440 for SL = 0 (E = 255 after the last step; with
fullsnes's `E <= (SL+1)*0x100` test: 0, 32, 69, 111, 163, 226, 312, 439). Starting from
0x7E0 (the recommended end of an A = 0..14 attack) the `(E >> 8) == SL` counts are
SL 7: 0, 6: 29, 5: 65, 4: 108, 3: 159, 2: 223, 1: 308, 0: 436 (E after the last step
2016, 1785, 1533, 1275, 1020, 765, 510, 255), and 0x7E0 reaches 0 after 691 steps.

The official manual's approximate table (SNESdev "DSP envelopes", Super Famicom Wiki
"Table 2.2"/"Table 2.3") lists attack 0 = 4.1 s, attack 7 = 160 ms, attack 14 = 6 ms,
gain linear $9F = 2 ms and bent $FF = 3.5 ms: these agree with the nominal step counts
(131072, 5120, 192, 64 and 112 samples; the 63-step attack gives 4.03 s / 157.5 ms /
5.9 ms, also within the manual's rounding, so the table cannot settle Ambiguity 5). Its exponential entries do not describe the
time to reach 0: decay 0 = 1.2 s (exact: 440 x 64 = 28160 samples = 0.88 s to leave
Decay at SL = 0, 695 x 64 = 1.39 s to reach 0), sustain rate $1F = 18 ms (exact: 695
samples = 21.7 ms to 0), $01 = 38 s (exact: 44.5 s to 0). Every exponential entry of the
manual (decay 0..7, sustain $01..$1F, GAIN $A1..$BF) corresponds to about 576..600
exponential steps `[computed: 1.2 s / 64 = 600, 740 ms / 40 = 592, 37 ms / 2 = 592,
18 ms / 1 = 576, 38 s / 2048 = 594]`, i.e. the level falling from 0x7FF to roughly
0x5F..0x77 (E = 95 after 600 steps, 119 after 576; about -25..-27 dB), which is presumably the threshold the manual's authors
measured. Do not use the manual's exponential values as test expectations; use the step
formulas.

## Key-on, key-off, ENDX and voice timing

KON/KOFF handling (fullsnes and Anomie, identical text):

1. If FLG bit 7 or the KOFF bit for the voice is set, transition to Release. If FLG bit 7
   is set, also set the envelope to 0.
2. If the internal KON value has the voice's bit set, perform the KON actions (envelope
   0, state Attack, restart at the directory start address, even if already playing).
3. Set the internal KON value to 0.

KON and KOFF are polled every other sample (16 000 Hz); FLG bit 7 is polled every sample
for every voice. KON takes effect at the poll following the write and the internal bits
are cleared 63 cycles after they are loaded; KOFF and FLG.7 act continuously while set.
Consequences: writing KON while KOFF (or FLG.7) is set keys the voice on and off again 2
samples later, so with the 5-sample start delay nothing is output; clearing KOFF within
63 SPC700 cycles after the KON write lets the key-on proceed. Two KON writes within one
poll interval usually leave only the second effective (or the first voice is 2 samples
ahead). Setting KON and KOFF together silences a voice faster than KOFF alone (KON zeros
the envelope), with a click.

ENDX (fullsnes, Anomie, SNESdev): bit x is set when a BRR block with the end flag is
decoded for voice x ("at the START of decoding the BRR block, not at the end"), including
while the voice is in Release; cleared for voice x at key-on (whether or not the key-on
succeeds; a freshly keyed-on voice reads 0 even if its sample is a single block); any
write clears all 8 bits. The bit set by the decoder in the same sample as a key-on does
not override the clear (Anomie step S4).

For the engine's `isChannelActive()`: a voice is active from KON until its envelope has
reached 0 in Release (or it was never keyed on); BRR decoding continues regardless.

## Noise generator

One 15-bit LFSR shared by all voices (fullsnes, Anomie, SNESdev). Reset value 0x4000
(fullsnes: "initially -4000h", i.e. 0x4000 read as a signed 15-bit value). It is clocked
by the global counter at the rate selected by FLG bits 0-4 (same rate table as the
envelopes; rate 0 = never, the value stays constant), during cycle 30 of the sample loop.
Update:

```
N = (N >> 1) | (((N << 14) ^ (N << 13)) & 0x4000)          // Anomie
   = ((N >> 1) & 0x3FFF) | ((bit0(N) ^ bit1(N)) << 14)      // fullsnes
```

i.e. feedback taps at bits 0 and 1 into bit 14. Period 32767 `[computed]` (maximal for
15 bits). Output: the 15-bit value interpreted as signed (`clip15(N)`, range
-0x4000..+0x3FFF), used directly as the voice sample in place of the interpolated BRR
sample (before the envelope, then VxVOL like any voice). The first outputs after reset
are -16384, 8192, 4096, 2048, 1024, 512, 256, 128, 64, 32 `[computed]`. VxPITCH, PMON and
Gaussian interpolation do not apply to noise (fullsnes, Anomie).

Noise frequencies (SNESdev table, = 32000 / period `[computed]`): rate 1: 16 Hz (15.62),
2: 21 (20.83), 3: 25, 4: 31 (31.25), 5: 42 (41.67), 6: 50, 7: 63 (62.5), 8: 83 (83.33),
9: 100, 10: 125, 11: 167 (166.67), 12: 200, 13: 250, 14: 333 (333.33), 15: 400, 16: 500,
17: 667 (666.67), 18: 800, 19: 1000, 20: 1300 (1333.33), 21: 1600, 22: 2000, 23: 2700
(2666.67), 24: 3200, 25: 4000, 26: 5300 (5333.33), 27: 6400, 28: 8000, 29: 10700 (10666.67),
30: 16000, 31: 32000 Hz.

Verified 2026-09-28: update formula against Anomie (`N=(N>>1)|(((N<<14)^(N<<13))&0x4000)`,
reset `N=0x4000`) and fullsnes (`(Level SHR 1) AND 3FFFh OR (Bit0 XOR Bit1) SHL 14`,
"initially -4000h"); both forms give identical results for all 32767 non-zero states
`[computed]`; period 32767 and the first ten outputs recomputed; the frequency table
against SNESdev "S-DSP registers" and the Super Famicom Wiki FLG table (identical).

Caution (fullsnes, Anomie, SNESdev): the BRR decoder keeps running for a noise voice, so
a sample ending with code 1 (end without loop) puts the voice in Release with envelope 0
and silences the noise. The driver must point noise voices at a looping dummy BRR block
(the engine should ship one: a filter-0 block of zeros with header `0x03`, looping to
itself).

## Echo (ESA/EDL, buffer, FIR, EFB, EVOL, EON)

Buffer (fullsnes, Anomie, SNESdev): at `ESA * 0x100`, `EDL * 2048` bytes = `EDL * 512`
stereo entries; EDL = 0 uses 4 bytes = 1 entry (the buffer is never empty). Delay =
`EDL * 16 ms` (EDL = 0: 1 sample = 31.25 us). Maximum 30720 bytes = 7680 samples = 240 ms.
Entry layout (fullsnes):

```
Byte 0: left  sample low byte  (bit 0 unused, always 0)
Byte 1: left  sample high byte
Byte 2: right sample low byte  (bit 0 unused, always 0)
Byte 3: right sample high byte
```

i.e. two little-endian 16-bit words holding 15-bit samples left-aligned
(`seee eeee eeee eee0`, SnesLab). Addresses wrap at 0xFFFF (`addr = (ESA*0x100 +
index*4) & 0xFFFF`, fullsnes; the buffer clobbers zero page when it wraps, SNESdev).
The engine keeps a private 64 KiB ARAM image so the same wrap applies.

Per-sample echo processing (fullsnes "FIR formula", Anomie register text, SnesLab):

```
addr     = (ESA * 0x100 + index * 4) & 0xFFFF
xL[i]    = (int16) ARAM[addr + 0..1] >> 1          // read, 15-bit
xR[i]    = (int16) ARAM[addr + 2..3] >> 1
firL     = FIR(xL), firR = FIR(xR)                  // 8-tap FIR below, separate per channel, same coefficients
mainL    = clamp16(clamp16(voiceMixL * MVOLL >> 7) + (firL * EVOLL >> 7))   // main output
echoInL  = clamp16(echoMixL + (firL * EFB >> 7)) & ~1                        // feedback
if not FLG.5: ARAM[addr + 0..1] = echoInL           // write back to the same entry (cycle 29)
                ARAM[addr + 2..3] = echoInR         // (cycle 30)
index += 1
if index >= length: index = 0                        // length = EDL ? EDL * 512 : 1, EDL re-read only when index wraps to 0
```

`voiceMixL` is the sum of all 8 voices' left samples after VxVOLL, clamped to 16 bits
after each addition; `echoMixL` is the same sum restricted to voices with the EON bit
set (Anomie, fullsnes "EchoVoices"). The entry is read (cycle 22/23) before it is
overwritten (29/30), so the delay through the buffer is exactly its length in samples.
FLG.5 only suppresses the write: the index keeps advancing and the FIR keeps reading, so
a frozen buffer loops forever (Anomie, fullsnes).

EDL is applied only when the index wraps to 0 (Anomie, fullsnes): a new EDL value can
take up to 240 ms (7680 samples) to take effect and the DSP keeps writing the old buffer
size until then; ESA changes take effect within 1-2 samples. Drivers therefore set ESA/EDL
with echo writes disabled and wait 240 ms before enabling them (Super Famicom Wiki,
SNESdev). The engine driver must do the same or re-initialise its private buffer.

8-tap FIR (fullsnes, Anomie, SnesLab; `x[i]` = newest input, `x[i-7]` = oldest, 15-bit):

```
sum  = (x[i-7] * FIR0) >> 6          // oldest sample, coefficient $0F
sum += (x[i-6] * FIR1) >> 6          // $1F
sum += (x[i-5] * FIR2) >> 6          // $2F
sum += (x[i-4] * FIR3) >> 6          // $3F
sum += (x[i-3] * FIR4) >> 6          // $4F
sum += (x[i-2] * FIR5) >> 6          // $5F
sum += (x[i-1] * FIR6) >> 6          // $6F
sum  = clip16(sum)                    // taps 0-6 accumulate with 16-bit wrap ("without overflow handling")
sum  = clamp16(sum + ((x[i] * FIR7) >> 6))   // newest sample, $7F, saturating
fir  = sum & ~1                       // low bit cleared (Anomie, SnesLab)
```

(The `>> 6` on a 15-bit sample is the same as `>> 7` on the left-aligned 16-bit word.)
Coefficients are signed 8-bit 1.7 fixed point: 0x7F = +0.992, 0x40 = 0.5, 0x80 = -1.0.
The identity filter is `7F 00 00 00 00 00 00 00` (Super Famicom Wiki, SNESdev), which
passes the sample delayed by 7 taps at gain 127/128; fullsnes recommends the sum of all
eight coefficients around +0x80, positive and negative partial sums of FIR0..FIR6 within
+/-0x7F, and never -128 in any tap (multiply overflow). SnesLab: absolute sum <= 128
avoids clicks from the wrapping accumulation; not all games obey this. Note: the SnesLab
page's last code line reads `S = S + (FIR[0] * x[n] >> 6)`; the surrounding text and every
other source make clear it is FIR[7].

Verified 2026-09-28: buffer size/delay (`D<<11` bytes, `D<<9` samples, 4 bytes at D = 0,
EDL applied when the index wraps, up to 0.24 s) against Anomie's ESA/EDL text, fullsnes
"Echo Buffer Notes" and SNESdev ESA/EDL errata; entry layout against fullsnes; the FIR
formula (`>> 6` per tap, wrap over taps 0-6, clamp on tap 7, `& ~1`, FIR0 = oldest)
against Anomie's `FFCx` text, fullsnes "The FIR formula is" and SnesLab "FIR Filter"
(S-DSP Implementation, including the 18623 + 16888 example); the identity filter
against SNESdev and the Super Famicom Wiki ("127,0,0,0,0,0,0,0"); tap read cycles
22/23/24/25 against Anomie and SnesLab.

EVOL is applied to the FIR output before it is mixed into the main output (after MVOL);
EFB is applied to the FIR output before it is mixed into the echo input (Anomie,
fullsnes, SnesLab). Feedback stability: EFB = 0x7F repeats "almost infinitely",
0x40 gives a normal decaying echo, 0x00 a single repeat (fullsnes).

Named FIR presets for the plugin (`fir_preset`) are a driver-level convenience. The
following sets are documented (bytes FIR0..FIR7, SnesLab "FIR Filter" section "Getting
started" for the four N-SPC standard filters of Nintendo's driver, fullsnes "Filter
Examples" for the official manual's example; verified 2026-09-28):

| Name | FIR0..FIR7 | Source description | Tap sums `[computed]` |
|---|---|---|---|
| Identity | `7F 00 00 00 00 00 00 00` | N-SPC 1, "won't affect the sound" (Super Famicom Wiki, SNESdev) | sum 127, abs 127 |
| High-pass 3 kHz | `58 BF DB F0 FE 07 0C 0C` | N-SPC 2, max gain +0.91 dB, "most used hi-pass filter" | sum -1, abs 239, FIR0..6 positive 107 / negative -120 |
| Low-pass 5 kHz | `0C 21 2B 2B 13 FE F3 F9` | N-SPC 3, +0.65 dB (Chrono Trigger and "many games") | sum 128, abs 172, FIR0..6 positive 150 |
| Band-pass 1.5-8.5 kHz | `34 33 00 D9 E5 01 FC EB` | N-SPC 4, +0.53 dB, ripples at 11-13 and 15-16 kHz | sum 13, abs 195, FIR0..6 positive 104 / negative -70 |
| Low-pass (official manual) | `FF 08 17 24 24 17 08 FF` | fullsnes "Echo with Low-pass (bugged)" with EFB 40; SnesLab "many games", +0.27 dB | sum 132, abs 136, FIR0..6 positive 134 |

Verified 2026-09-28 (second pass) against SnesLab "FIR Filter" ("Getting started" list
and the example tables) and fullsnes "Filter Examples"; tap sums recomputed, and the
max gains recomputed from the DTFT (+0.91, +0.65, +0.53, +0.27 dB) match SnesLab.

Note that the N-SPC low-pass and the official low-pass break fullsnes's "positive sum of
FIR0..FIR6 <= +7Fh" rule (150 and 134) and SnesLab's "absolute sum <= 128" rule, which
is why fullsnes marks the official example "bugged": full-scale echo content can wrap in
the tap 0-6 accumulation. They are nevertheless what games used, so the engine ships
them as-is (fidelity first) and any additional preset designed in-house (e.g. "Low-pass
strong", "Comb", "Bright", "Dark" from ENGINE_SPECS) is labelled as such in
`PRESET_QA.md` and not attributed to hardware or to N-SPC.

## Output mixing, OUTX and ENVX

Per voice, per sample (Anomie "SOUND GENERATION", fullsnes "Output Mixer"):

```
s15    = interpolated BRR sample, or noise (15-bit, -0x4000..+0x3FFF)
env    = (s15 * E) >> 11                        // 15-bit, |env| <= 0x3FF8 for E = 0x7FF
OUTX   = env >> 7                               // $x9, signed 8 bit
ENVX   = E >> 4                                 // $x8, 0..127
out16  = env << 1                               // 16-bit value used by PMON of the next voice
vL     = (out16 * VxVOLL) >> 7  =  (env * VxVOLL) >> 6      // 16-bit, no clamp needed
vR     = (out16 * VxVOLR) >> 7
```

Low bit of the voice volume product: fullsnes writes `sample*VxVOLx SAR 6` on the 15-bit
sample (the formula above, result may be odd). Anomie's formula `(S * VL) >> 7` does not
say whether S is the 15- or the 16-bit sample, and two of his sentences ("The bit is
'recovered' after the VxVOLL/VxVOLR volume adjustment"; noise: "volume adjustment then the
left-shift to 'restore' the low bit") suggest `((env * VxVOL) >> 7) << 1` (always even).
The two differ by 1 LSB for about half of all inputs (env 16383: VOL 64 -> 16383 vs 16382,
VOL 1 -> 255 vs 254; VOL 127 and -128 give 32510 and -32766 in both). See Ambiguity 18.

VxVOL = -128 cannot overflow because the envelope has already reduced the sample to at
most `0x3FFF * 0x7FF / 0x800` (fullsnes): -16376 * -128 >> 6 = 32752.

Verified 2026-09-28 against Anomie "SOUND GENERATION" (envelope `(S*E)>>11`, VxVOL
`(S*VL)>>7` on the 16-bit sample, mix "clamping to 16 bits after each addition", MVOL
then EVOL then clamp, EON mix with EFB), fullsnes "Output Mixer" (`sample*VxVOLx SAR 6`,
`sum*MVOLx SAR 7`, `fir_out*EVOLx SAR 7`, mute, `XOR FFFFh`) and its VxVOL note
("max=sample*7FFh/800h", -128 safe).

Mix (Anomie, fullsnes):

```
mainL = 0; echoL = 0
for v in 0..7:
    mainL = clamp16(mainL + vL[v])
    if EON.v: echoL = clamp16(echoL + vL[v])
mainL = (mainL * MVOLL) >> 7                     // fullsnes: sum*MVOLx SAR 7; -128 overflows, clamp anyway
outL  = clamp16(mainL + ((firL * EVOLL) >> 7))
if FLG.6 (mute): outL = 0
outL  = outL ^ 0xFFFF                            // fullsnes only: "final phase inversion (as done by built-in post-amp)"
echoInL = clamp16(echoL + ((firL * EFB) >> 7)) & ~1
```

Same for the right channel with MVOLR/EVOLR/VxVOLR. fullsnes lists the final
`XOR FFFFh` (one's complement, i.e. `-x - 1`) as the post-amplifier's inversion; Anomie
does not mention it. See Ambiguities. Note on the volume products: Anomie writes every
volume stage as `(int16_t)((S * V) >> 7)`, a 16-bit truncation, and only says that
*mixed* values (additions) are clamped; fullsnes only says that -128 "causes multiply
overflows". The `clamp16` on the MVOL/EVOL/EFB products above is therefore an engine
decision (Ambiguities 16); it only matters for V = -128 with a full-scale negative
input. Per-channel plugin buses (ENGINE_SPECS: "dry voice
through VOL L/R and MVOL, no echo") take `vL[v] * MVOLL >> 7` with the same mute and
inversion policy as the main bus.

## Order of operations per sample

Equivalent sample-synchronous ordering for the engine (derived from Anomie's cycle table;
exact cycle interleaving is not needed at block level):

```
for each output sample:
    if sample index is even: poll KON/KOFF (steps 1-3 of "Key-on, key-off")
    for v in 0..7 (in order, because PMON reads voice v-1's fresh output):
        latch registers (VOL, PITCH, SRCN, ADSR, GAIN) for this sample
        s15 = noise if NON.v else gaussian(ring, index)
        env = (s15 * E) >> 11 with the previous sample's E; OUTX; out16 = env << 1
        apply FLG.7 / BRR-end (current header) / KOFF / KON transitions, in that order
        update the envelope if the counter event for its rate fired (or Release/direct)
        advance index by the (modulated) pitch; decode new BRR groups; handle block ends
        ENVX; vL, vR, main/echo accumulation
    echo: read buffer entry, FIR, EVOL into main, EFB into echo input, write, advance index
    global counter -= 1 (wrap 0 -> 0x77FF); noise update if its rate fired
    output (mainL, mainR) through mute and inversion
```

## Worked examples

All values below were produced by `calc.py` (Python 3) implementing exactly the formulas
of this document; the script is reproducible from the formulas alone.

BRR block, shift 8, data bytes `12 34 56 70 FE DC BA 98` (signed nibbles, high nibble
first: 1, 2, 3, 4, 5, 6, 7, 0, -1, -2, -3, -4, -5, -6, -7, -8), decoder history
`old = older = 0`. `RD = (n << 8) >> 1 = n * 128`.

```
header 0x80 (filter 0): 128, 256, 384, 512, 640, 768, 896, 0, -128, -256, -384, -512, -640, -768, -896, -1024
header 0x84 (filter 1): 128, 376, 736, 1202, 1766, 2423, 3167, 2969, 2655, 2233, 1709, 1090, 381, -411, -1282, -2226
header 0x88 (filter 2): 128, 500, 1217, 2362, 4001, 6179, 8923, 11216, 12886, 13792, 13826, 12913, 11013, 8119, 4255, -525
header 0x8C (filter 3): 128, 486, 1153, 2188, 3634, 5519, 7859, 9636, 10800, 11320, 11181, 10380, 8926, 6836, 4134, 849
```

Step-by-step for filter 1, first three samples: n = 1: RD = 128, old = 0 -> 128.
n = 2: RD = 256, old = 128, `(-128) >> 4 = -8` -> 256 + 128 - 8 = 376. n = 3: RD = 384,
old = 376, `(-376) >> 4 = -24` -> 384 + 376 - 24 = 736.

Filter 2, first three samples: n = 1 -> 128. n = 2: RD = 256, old = 128, older = 0:
`(128 << 1) = 256`, `(-(256 + 128)) >> 5 = -384 >> 5 = -12`, older terms 0 -> 256 + 256 -
12 = 500. n = 3: RD = 384, old = 500, older = 128: `1000 + ((-1500) >> 5 = -47) - 128 +
(128 >> 4 = 8)` -> 384 + 1000 - 47 - 128 + 8 = 1217.

History carry: a filter-2 block with the same data decoded right after the filter-3 block
above (old = 849, older = 4134) gives -2130, -4601, -6391, -7358, -7396, -6433, -4434,
-2423, -591, 888, 1862, 2204, 1815, 624, -1409, -4295.

Wrap and shift cases: shift 12 filter 0, nibble 7 -> 14336 (0x3800); nibble -8 -> -16384.
Shift 13 (header 0xD0), nibbles 7 and -8 -> 0 and -2048; shift 15 nibbles 1 and -1 -> 0
and -2048. Shift 0 nibbles 1 and 7 -> 0 and 3; shift 1 -> 1 and 7. Shift 12 filter 1 with
sixteen nibbles of 7 (header 0xC4, bytes `77 x 8`): 14336, -4992, 9656, -9380, 5542,
-13237, 1926, 16141, -3300, 11242, -7893, 6936, -11930, 3151, -15478, -175 (the second
value: 14336 + 14336 - 896 = 27776 -> clamp16 unchanged -> clip15 = 27776 - 32768 =
-4992). Filter 2 with sixteen 7s: 14336, -1, 894, 16040, -1, -704, 12994, -1, 2152, -14330,
-14999, -822, -5938, 3786, -5649, 17.

Gaussian interpolation, history `h = [1000, 2000, 3000, 4000]` (oldest..newest):

```
d = 0x80: coefficients g[0x7F]=0x038, g[0x17F]=0x3C5, g[0x180]=0x3C9, g[0x80]=0x03A
          terms 27 + 942 + 1419 -> 2388 (no wrap) + 113 -> 2501
d = 0x00: g[0xFF]=0x172, g[0x1FF]=0x519, g[0x100]=0x176, g[0]=0
          terms 180 + 1274 + 547 + 0 -> 2001   (fullsnes 16-bit form gives 2002)
d = 0xFF: g[0]=0, g[0x100]=0x176, g[0x1FF]=0x519, g[0xFF]=0x172
          terms 0 + 365 + 1911 + 722 -> 2998   (fullsnes form 2999)
```

Overflow case, four samples of -0x4000 (filter 0 nibble -8 at shift 12), d = 0:
terms -2960, -10440, -2992 -> -16392 = -0x4008 -> clip15 -> +0x3FF8 (16376); fourth term
0 -> clamp15 -> +16376. Same at d = 1 (+16376). At d = 2 the coefficient quad sums to
0x800: result -16376. At d = 0x80: -16384. This is the documented "+3FF8h instead of
-4000h" pop. Four samples of +0x3FFF at d = 0 give -16379 (wrap the other way), at
d = 0x80 +16380.

Single impulse: `h = [0, 0, 16383, 0]`, d = 0x40 -> `(0x293 * 16383) >> 11 = 5271`;
`h = [0, 16383, 0, 0]`, d = 0x40 -> `(0x4BA * 16383) >> 11 = 9679`.

Verified 2026-09-28: every number in this section (four BRR filters, history carry,
shift/wrap cases, the sixteen-7s blocks, the three interpolation fractions, the
-0x4000 / +0x3FFF overflow cases in both the Anomie and the fullsnes form, the two
impulses) was recomputed by an independent Python re-implementation written from the
formulas of this document and the source texts; all values matched.

## Reference values for unit tests

Each line: value, then the formula or source it comes from.

1. Sample rate 32000 Hz = 24576000 / 768 (SNESdev S-SMP, Anomie).
2. SPC700 cycles per sample = 32; S-DSP clocks per sample = 96 (Anomie).
3. `kGaussTable[0] = 0x000`, `[255] = 0x172`, `[256] = 0x176`, `[511] = 0x519` (fullsnes listing).
4. `kGaussTable` sum = 262146 (0x40002); max = 0x519 (computed from the fullsnes listing, identical to SnesLab and Anomie).
5. Spot entries: `[1] = 0x000`, `[16] = 0x001`, `[128] = 0x03A`, `[300] = 0x233`, `[400] = 0x410`, `[510] = 0x519` (fullsnes listing).
6. Coefficient quad sums `g[255-d]+g[511-d]+g[256+d]+g[d]` lie in 0x7FF..0x801; d = 0 and d = 1 give 0x801, d = 2 gives 0x800 (fullsnes note, computed).
7. Gaussian `[1000,2000,3000,4000]`, d = 0x80 -> 2501; d = 0 -> 2001; d = 0xFF -> 2998 (Anomie formula, computed).
8. Gaussian `[-16384]*4`, d = 0 -> +16376 (documented overflow, fullsnes; computed); d = 0x80 -> -16384.
9. Gaussian impulse `[0,0,16383,0]`, d = 0x40 -> 5271 = `(0x293 * 16383) >> 11`.
10. BRR `RD`: shift 12 nibble 7 -> 14336; nibble -8 -> -16384; shift 13..15 nibble >= 0 -> 0, nibble < 0 -> -2048; shift 0 nibble 7 -> 3 (fullsnes/Anomie shift formula).
11. BRR filter 1, shift 8, data `12 34 56 70 FE DC BA 98`, zero history -> 128, 376, 736, 1202, 1766, 2423, 3167, 2969, 2655, 2233, 1709, 1090, 381, -411, -1282, -2226 (Anomie filter formula, computed).
12. BRR filter 2, same block -> 128, 500, 1217, 2362, 4001, 6179, 8923, 11216, 12886, 13792, 13826, 12913, 11013, 8119, 4255, -525.
13. BRR filter 3, same block -> 128, 486, 1153, 2188, 3634, 5519, 7859, 9636, 10800, 11320, 11181, 10380, 8926, 6836, 4134, 849.
14. BRR filter 0, same block -> n * 128 for each nibble (128 .. 896, 0, -128 .. -1024).
15. BRR 15-bit wrap: filter 1, shift 12, nibbles 7, 7 -> 14336 then -4992 (27776 clip15).
16. Rate periods: rate 1 = 2048, 4 = 1024, 8 = 384, 12 = 160, 16 = 64, 20 = 24, 24 = 10, 28 = 4, 29 = 3, 30 = 2, 31 = 1, 0 = never (fullsnes/Anomie/SNESdev table).
17. Rate offsets: rates 3n+1 -> 0, 3n+2 -> 1040, 3n (n >= 1) -> 536, rate 30 -> 0, rate 31 -> 0 (Anomie, SNESdev).
18. Counter: starts at 0, wraps 0 -> 0x77FF (= 30719; 30720 states per cycle); every rate fires with constant spacing = period across wraps (Anomie; verified by simulation over 61440 samples, re-run 2026-09-28).
19. First event after reset for rate 2 at sample 1040, rate 3 at 536, rate 5 at 272, rate 9 at 216, rate 12 at 56, rate 1/4/7/... at 0 (computed from `(counter + offset) % period == 0` with counter = 0 at sample 0 and decrementing).
20. Attack +32 (recommended reading, Ambiguity 5): 63 stored steps to E = 0x7E0, Decay entered on the next sample (computed value 0x800 > 0x7FF) with E = 0x7E0; attack +1024 (A = 15): E = 0x400, then 0x7FF, Decay after 2 steps. Alternative reading: 64 steps, E = 0x7FF.
21. Attack duration (recommended reading): 63 x period: A = 0: 129024 samples = 4.032 s; A = 7: 5040 = 157.5 ms; A = 14: 189 = 5.9 ms; from a counter reset at sample 0 the Decay state is entered after sample 126978 (A = 0), 5018 (A = 7), 190 (A = 14), 2 (A = 15). Nominal 64 x period (alternative reading, and the manual's 4.1 s / 160 ms / 6 ms): 131072 / 5120 / 192 samples.
22. Exponential decrease from 0x7FF: 0x7FF, 0x7F7, 0x7EF, 0x7E7, 0x7DF, 0x7D7, 0x7CF, 0x7C7, 0x7BF, 0x7B7, 0x7AF, 0x7A7, 0x79F (step `-((E-1)>>8)-1`); 695 steps to reach 0.
23. Decay steps from 0x7FF until `(E >> 8) == SL`: SL 7: 0, 6: 32, 5: 69, 4: 112, 3: 163, 2: 227, 1: 312, 0: 440; E after the last step: 2047, 1791, 1532, 1275, 1020, 765, 510, 255 (computed; fullsnes's `E <= (SL+1)*0x100` rule gives 0, 32, 69, 111, 163, 226, 312, 439). From 0x7E0 (end of an A = 0..14 attack, recommended reading): SL 7: 0, 6: 29, 5: 65, 4: 108, 3: 159, 2: 223, 1: 308, 0: 436; E after the last step 2016, 1785, 1533, 1275, 1020, 765, 510, 255.
24. Bent increase 0 -> 0x7FF: 112 steps (48 steps of +32 to 0x600, then 64 steps of +8 to 0x7FF, last clamped); manual: rate $FF = 3.5 ms vs computed 112 samples = 3.5 ms.
25. Release: 256 samples from 0x7FF to 0 at -8 per sample = 8 ms (fullsnes/Anomie/SNESdev).
26. Direct gain 0x7F -> E = 0x7F0; 0x40 -> 0x400 (`E = G << 4`).
27. ENVX = E >> 4: E = 0x7FF -> 127, 0x400 -> 64. OUTX: s15 = 16383, E = 0x7FF -> env 16375 -> OUTX 127 (0x7F); s15 = -16384, E = 0x7FF -> env -16376 -> OUTX -128 (0x80); s15 = 1000, E = 0x100 -> env 125 -> OUTX 0.
28. Voice volume: env 16383, VOL 127 -> 32510; VOL -128 -> -32766; VOL 64 -> 16383; VOL 1 -> 255 (`(env * VOL) >> 6`, fullsnes). With the alternative `((env * VOL) >> 7) << 1` (Ambiguity 18): 32510, -32766, 16382, 254.
29. Master volume: 32767 x 127 >> 7 = 32511; -32768 x -128 >> 7 = 32768, which does not fit in 16 bits: 32767 with the recommended clamp16, -32768 with Anomie's `(int16_t)` truncation (fullsnes "-128 causes multiply overflows"; Ambiguity 16). 32767 x -128 >> 7 = -32767 fits either way.
30. Pitch register from semitone offset (root at 32 kHz): -24 -> 0x0400, -12 -> 0x0800, -7 -> 0x0AAE (2734), -1 -> 0x0F1A (3866), 0 -> 0x1000, +1 -> 0x10F4 (4340), +7 -> 0x17F9 (6137), +12 -> 0x2000, +19 -> 0x2FF2 (12274), +24 -> 16384 clamped to 0x3FFF (`round(4096 * 2^(n/12))`).
31. Playback rate: P = 0x1000 -> 32000 Hz; 0x0800 -> 16000 Hz; 0x2000 -> 64000 Hz; 0x3FFF -> 127992.19 Hz (`32000 * P / 4096`).
32. PMON: P = 0x1000, OUTX16 = 0 -> 0x1000; OUTX16 = 32766 -> 8188 (0x1FFC); OUTX16 = -32768 -> 0; OUTX16 = 16384 -> 0x1800; OUTX16 = -16384 -> 0x0800; P = 0x3FFF, OUTX16 = 32766 -> 32750 (0x7FEE); P = 0x1000, OUTX16 = 31 -> 0x1000 (Anomie formula; fullsnes factor form gives identical values).
33. Noise LFSR period 32767; first outputs from 0x4000: -16384, 8192, 4096, 2048, 1024, 512, 256, 128, 64, 32 (Anomie update formula, computed).
34. Noise rate 31 -> 32000 Hz, 30 -> 16000, 28 -> 8000, 25 -> 4000, 19 -> 1000, 12 -> 200, 1 -> 15.625 Hz (32000 / period; SNESdev table rounds to 16 Hz).
35. Echo buffer length in stereo samples: EDL 0 -> 1, 1 -> 512, 2 -> 1024, 8 -> 4096, 15 -> 7680; bytes = 4 / 2048 / 4096 / 16384 / 30720; delay 16 ms per EDL step, 240 ms max (fullsnes, Anomie, SNESdev).
36. FIR identity `7F 00 00 00 00 00 00 00`: a 15-bit input 0x0800 appears at the output 7 samples later as 4064 (0x0800 x 127 >> 6, low bit cleared); with the coefficient in FIR7 instead the same value appears with no delay; input 0x3FFF x 127 >> 6 = 32510; -0x4000 -> -32512.
37. FIR wrap vs clamp: taps `7F 7F 00 00 00 00 00 00` with all inputs 0x3FFF -> 32510 + 32510 = 65020 wraps to -516; taps `00 00 00 00 00 00 7F 7F` -> 32510 (wrapped sum of tap 6) + 32510 clamps to 32767 -> 32766 after clearing bit 0 (SnesLab: "18623 + 16888 yields 32767 instead of -30025").
38. FIR all taps 0x10 with all inputs 0x1000: 8 x (0x1000 x 16 >> 6) = 8192.
39. Echo volume: fir 0x7FFE x EVOL 0x7F >> 7 = 32510; x EFB -128 >> 7 = -32766; x 0x40 >> 7 = 16383.
40. Final inversion (fullsnes `XOR FFFFh`): 0 -> -1, 32767 -> -32768, -32768 -> 32767.
41. Key-on: 5 output samples of 0 before the first data sample (Anomie, fullsnes "5 empty samples").
42. Reset state: FLG behaves as 0xE0, ENDX = 0, global counter = 0, noise = 0x4000 (Anomie, fullsnes, SNESdev).
43. Echo feedback at maximum (identity FIR `7F 00 .. 00`, EFB = 0x7F, EON voices silent, one buffer entry holding 15-bit 0x3FFF): successive (FIR output, echo input written back) pairs per buffer pass = (32510, 32256), (32004, 31752), (31502, 31254), (31008, 30764), (30522, 30282), (30044, 29808); each pass multiplies by (127/128)^2 = 0.9844 (-0.14 dB) with truncation, so the loop decays and never grows (computed; fullsnes "7Fh would repeat the echo almost infinitely"). With EFB = 0x40: (32510, 16254), (16126, 8062), (7998, 3998), (3966, 1982) (-6 dB per pass).
44. Attack -> Decay: 63 steps of +32, E = 0x7E0 on entry, with fullsnes's ">= 0x7E0" rule and with Anomie's "new value > 0x7FF" test evaluated every sample (recommended); 64 steps, E = 0x7FF on entry, if the test is gated by the counter event (alternative) (Ambiguity 5, computed).
45. Exponential decrease thresholds: from 0x7FF, E = 0x100 after 439 steps, 255 after 440, 119 after 576, 103 after 592, 95 after 600, 15 after 680, 0 after 695 (computed; explains the manual's exponential timings, see "Envelope").
46. Counter first events after reset (counter = 0 at sample 0): rate 2 -> sample 1040, 3 -> 536, 5 -> 272 (1040 mod 768), 6 -> 536 mod 640 = 536, 9 -> 216 (536 mod 320), 12 -> 56 (536 mod 160), 30 -> 0 (536 or 0 offset, both even) (computed, Anomie/SNESdev tables).
47. fullsnes 16-bit Gaussian form results for the reference inputs of item 7: d = 0x80 -> 2501, d = 0 -> 2002, d = 0xFF -> 2999; four samples of +0x3FFF: d = 0 -> -16378, d = 0x80 -> 16382 (computed; differ from the Anomie form by at most 2 LSB, Ambiguity 1).
48. Gaussian table properties: monotonically non-decreasing (`g[i] <= g[i+1]` for i = 0..510); quad sum symmetric, `quad(d) == quad(255 - d)` for every d (the coefficient set for 255 - d is the set for d in reverse order); coefficient sets: d = 0 -> (g[255], g[511], g[256], g[0]) = (370, 1305, 374, 0); d = 0x40 -> (168, 1210, 659, 11); d = 0x80 -> (56, 965, 969, 58); d = 0xFF -> (0, 374, 1305, 370) (computed from the table, identical in all three sources).
49. All 32 rate periods = `kRatePeriod` (0 = never, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80, 64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1), each firing with constant spacing over 61440 simulated samples (Anomie, fullsnes, SNESdev; computed).
50. 16-bit output clamp: two voices at +32510 mix to 32767 (clamp after the addition, not 65020); with MVOL 127 -> 32511; adding an echo term `(32766 * 127) >> 7 = 32510` clamps to 32767; eight voices at -32766 mix to -32768 (Anomie "clamping to 16 bits after each addition", computed).
51. N-SPC FIR preset gains (max of |H| over 0..16 kHz with taps/128, computed): high-pass `58 BF DB F0 FE 07 0C 0C` +0.91 dB (DC -42.1 dB); low-pass `0C 21 2B 2B 13 FE F3 F9` +0.65 dB (DC 0.0 dB); band-pass `34 33 00 D9 E5 01 FC EB` +0.53 dB; official low-pass `FF 08 17 24 24 17 08 FF` +0.27 dB at DC; identity -0.07 dB (127/128); matching the gains printed by SnesLab.

Verified 2026-09-28: every value in items 1..42 was recomputed by an independent Python
script from the source formulas (items 1..2 and 41..42 checked against the source
texts); items 18, 23 and 29 were corrected as noted in "Ambiguities"; items 43..47 were
added for the ENGINE_SPECS checklist (feedback stability at max, attack/decay
thresholds, exponential timing, counter phase, alternative Gaussian form).

Verified 2026-09-28 (second adversarial pass): every item 1..51 recomputed by a fresh
Python script (BRR decoder, both Gaussian forms, counter simulation over 61440 samples,
envelope simulations, PMON in both forms over a 14-bit x 15-bit grid, LFSR, FIR, echo
loop, DTFT of the FIR presets); the Gaussian table re-extracted from fullsnes, Anomie's
apudsp.txt and the SnesLab table (hex and decimal columns) with 0 differences. Items 20,
21, 23, 28 and 44 were revised for Ambiguities 5 and 18; items 48..51 added (table
symmetry/monotonicity, full period table, 16-bit output clamp, preset gains).

## Ambiguities

1. Gaussian arithmetic domain. fullsnes: each term `>> 10` on 15-bit samples, wrap at
   16 bits after adding the 2nd and the 3rd term, clamp16 after adding the 4th term,
   then `SAR 1`. Anomie: each term `>> 11`, clip15 after adding the 3rd term, clamp15
   after adding the 4th. Both agree
   on the overflow behaviour and on the +0x3FF8 pop; they differ by 1-2 LSB in about two
   thirds of all inputs because fullsnes keeps one extra bit through the additions.
   Recommended: Anomie's form (`>> 11`, clip15, clamp15); it is the one the SNESdev wiki
   cites and the one used by all reference values here. Alternative: the fullsnes form.
2. Gaussian index bits. fullsnes says both "Counter.Bit11-3 are used as gaussian
   interpolation index" and "using bit4-11 of the pitch counter". Anomie:
   `(index >> 4) & 0xFF`. Recommended: bits 4..11 (256 fractions), consistent with a
   512-entry table addressed as `255-d`, `511-d`, `256+d`, `d`. Alternative: none.
3. Pitch step clamp. fullsnes leaves open whether the modulated step or the counter
   result is cropped to 128 kHz (0x3FFF). Anomie: the step is not clamped; the
   interpolation index is clamped to 0x7FFF after the addition. The task brief mentions
   "clamp to 0x3FFF": that applies to the 14-bit register (mask 0x3FFF), not to the
   modulated step. Recommended: mask the register to 14 bits, do not clamp the modulated
   step, clamp the index to 0x7FFF (Anomie). Alternative: clamp the step to 0x3FFF
   (limits PMON depth to +0 % above 0x2000 sources, audibly different).
4. PMON on noise voices. Anomie's pitch code masks PMON with `~NON` (no modulation when
   the voice is a noise voice), his NON text says PMON still controls BRR decode speed;
   fullsnes only says PMON does not affect the noise frequency. Only the moment the
   dummy BRR sample ends is affected. Recommended: follow the code (no modulation on
   noise voices). Alternative: apply modulation to the decode speed.
5. Attack -> Decay threshold (revised 2026-09-28, second pass). fullsnes: switch at
   `Level >= 0x7E0` (and clip to 0x7FF if >= 0x800). Anomie: switch when "the new value
   is greater than 0x7FF" (negative values also trigger), in a list introduced by "These
   registers are actually used to update the envelope every sample. The calculated value
   is used as follows", where only step 1 (store the clamped value) depends on the
   counter. Read literally, the new value 0x7E0 + 32 = 0x800 is computed on the sample
   after the 63rd step and triggers Decay with E = 0x7E0, exactly fullsnes's result. The
   first pass read Anomie as gating the test on the counter event (Decay at 0x7FF after
   64 steps, 31 levels higher) and the same pass already evaluated the Decay -> Sustain
   test every sample, which is inconsistent. SNESdev only says "adds 32" and "clamped to
   0-2047", which does not decide. The manual's attack times (4.1 s, 160 ms, 6 ms) fit
   64 steps slightly better (4.096 s vs 4.032 s) but are rounded nominal values (attack 1:
   2600 ms vs 2.56 s nominal). Recommended: evaluate the Attack -> Decay test every sample
   on the computed value (Decay at 0x7E0 for A = 0..14, at 0x7FF after 2 steps for
   A = 15). Alternative: gate it on the counter event (Decay at 0x7FF after 64 steps).
6. Decay -> Sustain threshold. Anomie/SNESdev: `(E >> 8) == SL`. fullsnes:
   `E <= (SL + 1) * 0x100`. They differ only when E lands exactly on `(SL+1)*0x100`
   (e.g. SL = 4: fullsnes switches at step 111 with E = 1280, Anomie at step 112 with
   E = 1275). Recommended: Anomie/SNESdev. Alternative: fullsnes.
7. Release at BRR end code 1. fullsnes lists "Step = -800h when BRR-end"; Anomie says
   the envelope goes to 0 immediately when the header is loaded. Same effect (E = 0 in
   the same sample). Recommended: set E = 0 and state = Release when the header with
   E = 1, L = 0 is loaded (the engine checks the current header at S3c of every sample,
   before KON, see Ambiguity 24).
8. When ENDX is set. Register descriptions (fullsnes, Anomie, SNESdev): "at the START of
   decoding the BRR block". Anomie's BRR section: "when the block is complete". Because
   of the 12-sample look-ahead the difference is at most one block. Recommended: set the
   bit when the header of the end block is loaded (start). Alternative: at completion.
   Re-examined 2026-09-28 (fidelity review): Anomie's S4 step text also reads "flag the
   loop address for loading next step S2 and set ENDX.x in step S7", i.e. when the end
   block has been finished, which supports the alternative. SNESdev "S-DSP registers"
   ("set when the current BRR block has the end-flag set (not at the end of the BRR
   sample)") and Anomie's own register note ("at the START of decoding the BRR block, not
   at the end") support the recommendation, and SNESdev's "if the voice is recently
   keyed-on, the ENDX bits will be clear (even if the BRR sample is a single BRR block)"
   only holds for a single-block sample if the bit is not raised by finishing that block.
   Decision unchanged (start); a driver polling ENDX sees the bit up to one block (16
   samples at P = 0x1000) earlier than with the alternative. Both readings agree that a
   bit raised on the key-on sample itself is overridden by the KON clear (Anomie S4).
9. Final phase inversion. fullsnes: `sum XOR FFFFh` after the mute ("as done by
   built-in post-amp"). Anomie, SNESdev: not mentioned. Absolute polarity is inaudible
   alone but matters when mixing with other sources. Recommended: apply the inversion in
   the chip core output stage (documented hardware behaviour; the DC blocker removes the
   -1 LSB offset). Alternative: omit it. Either way keep it out of the per-voice buses'
   relative polarity (apply the same choice to every bus).
10. Bent-increase threshold value. fullsnes "clipped to 11 bits" versus Anomie
    "pre-clamp": both mean the previous computed value truncated to 11 bits rather than
    the clamped E; a negative previous value therefore counts as > 0x600. Recommended:
    keep a per-voice `hiddenEnv = newValue & 0x7FF` for the comparison. Alternative:
    compare the clamped E (differs only after a mode switch from a decreasing mode at
    E < 32).
11. Envelope timing on rate change. Anomie: the offset table reproduces the phase of the
    first step after a rate change; fullsnes: "the location of the 1st step may vary".
    Recommended: the `(counter + offset) % period` test with a single global counter.
12. Per-voice bus polarity/inversion and mute for the plugin buses: not a hardware
    question; recommended to mirror the main bus (inversion applied, mute honoured).
13. Official ADSR/GAIN timing tables (SNESdev "DSP envelopes", Super Famicom Wiki) are
    rounded measurements (e.g. attack 1 = 2600 ms in SNESdev, 2.5 s in the Super Famicom
    Wiki copy of the manual; computed 2.56 s). The linear entries (attack, GAIN linear,
    GAIN bent) match the exact step counts within rounding; the exponential entries
    (decay, sustain, GAIN exponential) do not measure the time to 0 (695 steps) nor the
    time to the SL boundary but about 576..600 exponential steps (level down to roughly
    0x60..0x77, see "Envelope"). Recommended: test against the exact step/period
    formulas; quote the manual as a +/-10 % sanity tolerance for the linear phases only
    and not at all for the exponential ones.
14. N-SPC FIR preset bytes: resolved 2026-09-28. SnesLab's FIR page ("Getting started")
    lists the four N-SPC standard filters with their bytes, and fullsnes lists the
    official manual's low-pass example; see "Echo" for the values. Recommended: ship
    these five documented sets under their documented names and label any additional
    `fir_preset` sets as in-house designs.
15. Uninitialised registers at power-on (Anomie: "most registers are uninitialized").
    Recommended: `reset()` zeroes all 128 registers and sets FLG = 0xE0, then the
    driver initialises everything it uses (as real drivers do).
16. Volume products at -128 (MVOL, EVOL, EFB). Anomie writes the VxVOL, MVOL and EVOL
    stages as `(int16_t)((S * V) >> 7)`, i.e. a 16-bit truncation (his EFB line reads
    `E = (int16_t)(E * V)>>7`, cast before the shift, which taken literally would
    truncate the product and is presumably a typo for the same form), and reserves "clamped to 16
    bits" for the additions; fullsnes says only that -128 "causes multiply overflows
    (-8000h*-80h=-400000h)" and, for FIRx/EFB/EVOLx, "does probably cause multiply
    overflows?". The only affected case is V = -128 with a full-scale negative input
    (`-32768 * -128 >> 7 = 32768`): wrap gives -32768, clamp gives 32767. VxVOL cannot
    reach this case (fullsnes). Recommended: clamp16 (a bounded, click-free result, and
    the randomizer/presets avoid -128 anyway); alternative: wrap to -32768 as Anomie's
    cast suggests. Reference value 29 documents both results.
17. Global counter clock (added 2026-09-28, second pass). SNESdev "DSP envelopes": the
    counter "decrements on each S-SMP clock" and the period table gives "how many S-SMP
    clocks elapse per envelope operation". Anomie: "decrementing by one each sample";
    fullsnes: periods in "32000Hz sample units". At the 1.024 MHz S-SMP clock SNESdev's
    own attack 0 = 4100 ms (64 x 2048) would be 128 ms, so SNESdev's "S-SMP clock" can
    only mean the 32 kHz sample tick. Decision: one decrement per output sample.
    Alternative: none consistent with any published timing.
18. Low bit of the VxVOL product (added 2026-09-28, second pass). fullsnes: `sample *
    VxVOLx SAR 6` on the 15-bit envelope output (odd results possible). Anomie: `(S *
    VL) >> 7` with S not stated as 15- or 16-bit, plus "The bit is 'recovered' after the
    VxVOLL/VxVOLR volume adjustment" and, for noise, "volume adjustment then the
    left-shift to 'restore' the low bit", which suggests `((s15 * V) >> 7) << 1` (always
    even). The two differ by 1 LSB in about 49.5 % of random (env, VOL) pairs `[computed]`
    and never at full scale for VOL 127 / -128. Recommended: fullsnes's explicit formula
    `(env * V) >> 6` (equal to Anomie's formula if S is the 16-bit `env << 1`, which his
    "Apply the VxVOL registers (16-bit stereo sample)" overview supports). Alternative:
    `((env * V) >> 7) << 1`. Reference value 28 lists both.
19. Frozen echo buffer length (added 2026-09-28, second pass). Anomie's overview says a
    write-disabled echo buffer is "a static sample buffer up to 0.96 seconds long", while
    his EDL text, fullsnes and SNESdev give at most 30720 bytes = 7680 stereo samples =
    0.24 s. 0.96 s is 30720 divided by 32000, i.e. bytes counted as samples. Decision:
    0.24 s (7680 samples); fullsnes's ".25s" in its copy of the EDL note is a rounding of
    the same value.
20. Gaussian quad sum at d = 2 (added 2026-09-28 by the engine implementation). Reference
    value 6 and the "Overflow case" worked example say the coefficients for d = 2 sum to
    0x800, but the table of this document (identical to fullsnes, Anomie and SnesLab) gives
    `g[253] + g[509] + g[258] + g[2] = 0x16A + 0x518 + 0x17D + 0 = 0x7FF`; the 0x801 list and
    the 168 / 46 / 42 counts are consistent with the table, and d = 5 is the first 0x800
    fraction. The Gaussian result quoted for d = 2 (-16376 for four -0x4000 samples) is
    unaffected (it was computed from the table). Decision: the table is authoritative;
    `test_snes_gaussian.cpp` checks `quad(2) == 0x7FF`. Alternative: none.
21. Output of key-on sample #0 (added 2026-09-28). Anomie's sequence calls sample #0 the
    "final pre-KON sample" while also setting the envelope to 0 on it; whether that
    sample's output still uses the old envelope is not stated. First decision: the KON
    actions happen before the envelope multiply, so #0 outputs 0. Revised 2026-09-28
    (fidelity review, Ambiguity 24): Anomie's S3c applies the envelope before it handles
    KON ("After the final pre-KON sample is prepared, the envelope is set to 0"), so #0
    outputs the old voice level; the pre-KON pitch step and BRR decode still run on #0.
    Samples #1..#5 are silent as documented and the first data sample is #6. Decision: old
    level on #0. Alternative: 0 on #0 (a one-sample difference).
22. Decay -> Sustain at SL = 7 with a fast decay rate (added 2026-09-28). The "Envelope"
    section says SL = 7 takes 0 decay steps because the transition test "runs every sample,
    so the voice leaves Decay before its first counter event". With the documented
    per-sample order (store on a counter event, then evaluate the transitions) that holds
    only if the first sample in Decay has no counter event; with D = 7 (rate 30, period 2)
    an event can fall on that sample and one step is taken first (E = 0x7F7). Decision: keep
    the documented order (store, then transitions), so SL = 7 costs 0 or 1 step depending
    on the counter phase; the tests use D = 0 (period 64), where the documented 0 is exact.
    Alternative: evaluate the Decay -> Sustain test before the store.
23. Analog output high-pass (added 2026-09-28). No consulted source gives the cutoff of the
    SNES output coupling after the DAC (the post-amp inversion of Ambiguity 9 is the only
    documented property of that stage). Decision: the engine uses the project's common
    one-pole DC blocker at 5 Hz, labelled as a non-chip stage. Alternative: a measured value
    if one is found later.
24. Envelope application vs update order (added 2026-09-28, fidelity review). The first
    version of "Exact per-sample update order" and "Order of operations per sample" put
    the KON/KOFF/FLG.7 transitions and the envelope update before the `(s15 * E) >> 11`
    multiply, which contradicts the Anomie S3c text they cite (re-fetched 2026-09-28):
    "If applicable, replace the current sample with the noise sample. Apply the volume
    envelope. Check FLG bit 7 (NOT previously loaded). Check BRR header 'e' and 'l' bits to
    determine if the voice ends. Handle KOFF and KON using previously loaded values. [...]
    Update the volume envelope, using previously loaded values." The key-on sequence text
    ("#5 = Envelope updating begins. The sample output is still '0x0000', because of the
    order in which voice operations are performed") confirms that the envelope stored on a
    sample is first heard on the next one. Decision: Anomie's order (apply the stored E,
    then FLG.7, BRR end, KOFF, KON, then compute and store the new E); every envelope change
    reaches OUTX, the outputs and PMON one sample after it is stored. The BRR end check
    reads the current block's header every sample (S3b "Load the BRR header byte (every
    time)"), so a code-1 header loaded by a decode acts at the next sample's S3c, and a KON
    on that sample overrides it. Alternative: the first version's order (every envelope
    change one sample earlier; A = 15 outputs 0x7FF on #6 instead of 0x400; it also let
    a same-sample code-1 header cancel a key-on, which contradicts Anomie S3c/S4).

Corrections made by the 2026-09-28 verification pass (all sources re-fetched, tables
re-derived with Python):

* Global counter wrap value: "0x77FF (30720)" corrected to 0x77FF = 30719 (30720
  states); the period arithmetic was already right.
* Decay step count for SL = 7 corrected from 1 to 0 (also in reference value 23).
* The claim that the manual's exponential timings "agree with the exact formulas" was
  wrong (decay 0: 0.88 s exact vs 1.2 s; sustain $1F: 21.7 ms vs 18 ms); replaced by the
  analysis in "Envelope" and Ambiguity 13.
* fullsnes's "1st/2nd/3rd addition" wording was mapped to the wrong terms in the
  Gaussian section and Ambiguity 1; fixed.
* Rate-30 counter offset: SNESdev lists 536, Anomie 0; noted as equivalent (period 2).
* N-SPC FIR bytes found on SnesLab and added; Ambiguity 14 resolved.
* Volume -128 wrap/clamp added as Ambiguity 16; reference value 29 reworded.
* No numeric value of the Gaussian table, the rate tables, the BRR/Gaussian/PMON/noise/
  echo worked examples or the other reference values needed a change.

Corrections made by the second adversarial pass (2026-09-28; fullsnes.htm, Anomie's
apudsp.txt and the SnesLab pages re-downloaded and parsed by script, SNESdev pages
re-read, every table and example recomputed with Python):

* Attack -> Decay (Ambiguity 5): the first pass's reading of Anomie (transition gated
  by the counter, 64 steps, E = 0x7FF) contradicted the every-sample wording of his
  envelope list; the literal reading agrees with fullsnes (E = 0x7E0 after 63 steps).
  Recommendation changed, alternative kept; the envelope text, the per-sample update
  order and reference values 20, 21, 44 revised; decay counts from 0x7E0 added (23).
* "Anomie and SNESdev write 0x77FF (30,720)": only SNESdev writes the parenthesised
  count; Anomie writes "counts from 0x77FF to zero". SNESdev's "S-SMP clock" wording
  for the counter recorded as Ambiguity 17.
* VxVOL low-bit question (fullsnes `SAR 6` vs a possible Anomie `>> 7` then `<< 1`)
  recorded as Ambiguity 18; reference value 28 lists both results.
* Anomie's "0.96 seconds" frozen buffer recorded as Ambiguity 19 (0.24 s kept).
* Anomie's EFB formula wording noted in Ambiguity 16.
* Manual exponential-threshold level corrected from "0x60..0x77" to "0x5F..0x77"
  (E = 95 after 600 steps).
* Reference values 48..51 added for the ENGINE_SPECS checklist (Gaussian symmetry and
  monotonicity, all 32 periods, 16-bit output clamp, FIR preset gains).
* Confirmed unchanged: the 512-entry Gaussian table (0 differences against each of the
  three sources, sum 262146), kRatePeriod, kRateOffset, the BRR shift/filter formulas and
  all worked examples, the Gaussian examples in both forms, PMON (both forms identical on
  a 14-bit x 15-bit grid), noise LFSR, FIR/echo examples, N-SPC preset bytes and tap sums.

## Sources

All consulted on 2026-09-28.

1. nocash, "Fullsnes - Nocash SNES Specs", https://problemkaputt.de/fullsnes.htm,
   sections "SNES APU DSP BRR Samples", "SNES APU DSP BRR Pitch", "SNES APU DSP
   ADSR/Gain Envelope", "SNES APU DSP Volume Registers", "SNES APU DSP Control
   Registers", "SNES APU DSP Echo Registers". Taken: register map and bit layouts, BRR
   header/shift/filter formulas, clamp/clip rules, 512-entry Gaussian table (primary
   transcription), Gaussian formula (16-bit form) and overflow notes, pitch counter and
   PMON factor form, ADSR/GAIN rates and steps, rate table, KON/KOFF/FLG/ENDX text,
   noise LFSR update and reset value, output mixer including the final XOR, echo buffer
   entry layout, ESA/EDL/FIR formula and coefficient advice.
2. Anomie (with updates by jwdonal), "Anomie's S-DSP Doc", revision 1212 (2015-09-28),
   text copy fetched from https://37.muncher.se/bot/apudsp.txt (romhacking.net document
   191 https://www.romhacking.net/documents/191/ refused automated download; an older
   copy is at http://www.gamepilgrimage.com/sites/default/files/SystemSpecs/SNES/anomie/apudsp.txt).
   Taken: clocks (24.576 MHz, 3.072 MHz, 1.024 MHz, 96 clocks/sample), sample loop and
   voice steps, global counter (0x77FF wrap, reset to 0), rate and offset tables, BRR
   integer filter formulas and 12-sample ring, key-on 5-sample sequence, pitch
   modulation code and 0x7FFF index clamp, Gaussian table and 15-bit formula, envelope
   state machine details (pre-clamp bent value, negative attack trigger, GAIN bits as
   sustain level), register timing, noise generator formula, echo processing order,
   EDL/ESA application, FIR wrap/clamp and `& ~1`. Re-fetched 2026-09-28 (fidelity
   review): voice steps S3b/S3c/S4 (envelope applied before FLG.7 / BRR end / KOFF / KON /
   envelope update; ENDX set in S4 does not override the KON clear), the key-on sequence
   #0..#6 and the ENDX register note (Ambiguities 8, 21, 24).
3. SNESdev Wiki, "S-DSP registers", https://snes.nesdev.org/wiki/S-DSP_registers (raw
   wikitext). Taken: register bit diagrams, KON/KOFF errata, FLG semantics, noise
   frequency table, PMON formula and notes (voice 0/7), NON caution, DIR entry format,
   ESA/EDL errata (240 ms), FIR identity filter, ADSR register semantics (decay ends
   when upper 3 bits match SL), GAIN mode table, ENVX/OUTX definitions, ADSR1/ADSR2
   write-order race.
4. SNESdev Wiki, "DSP envelopes", https://snes.nesdev.org/wiki/DSP_envelopes (raw
   wikitext). Taken: envelope step formulas, ADSR/GAIN timing tables (manual values),
   period table and period offset table, counter description.
5. SNESdev Wiki, "BRR samples", https://snes.nesdev.org/wiki/BRR_samples (raw wikitext).
   Taken: block layout table, nominal filter coefficients 15/16, 61/32, 15/16, 115/64,
   13/16, loop/end semantics, interpolator "centred on the second sample", overflow note.
6. SNESdev Wiki, "S-SMP", https://snes.nesdev.org/wiki/S-SMP (raw wikitext). Taken:
   24.576 MHz resonator, 768 cycles per sample, 3.072 MHz, 1.024 MHz, 32 clocks per
   sample, real-console rate spread, DSPADDR/DSPDATA access.
7. SnesLab Wiki, "S-DSP/Gaussian Filter", https://sneslab.net/wiki/S-DSP/Gaussian_Filter.
   Taken: independent 512-entry table used to verify the fullsnes transcription
   (identical), "12-bit unsigned" description.
8. SnesLab Wiki, "Bit Rate Reduction", https://sneslab.net/wiki/BRR. Taken: header
   layout, shift 13-15 formula `(nibble >> 3) << 11`, wrap-once-then-clip note.
9. SnesLab Wiki, "FIR Filter", https://sneslab.net/wiki/FIR_Filter, sections "S-DSP
   Implementation" and "Getting started". Taken: 1.7 fixed-point taps, oldest-to-newest
   tap order, clip on taps 0-6 and clamp on tap 7 with the 18623 + 16888 example,
   `& 0xFFFE`, gain <= 0 dB advice, tap read cycles; on 2026-09-28 also the four N-SPC
   standard filter byte sets and the "Many games" low-pass sets.
10. SnesLab Wiki, "S-DSP", https://sneslab.net/wiki/S-DSP. Taken: register address
    table, 16 kHz limit statement.
11. Super Famicom Development Wiki, "SPC700 Reference",
    https://wiki.superfamicom.org/spc700-reference (transcription of the official APU
    manual). Taken: ADSR parameter timing table 2.2, GAIN timing table 2.3, FLG noise
    clock table, ESA/EDL description (`EDL * 16 ms`, `EDL * 2 KB`, 4 bytes at EDL = 0),
    echo initialisation procedure with the 240 ms wait, FIR identity `127,0,...,0`,
    pitch formula `HZ = 32000 * P / 2^12` (re-checked 2026-09-28).
12. Romhacking.net document listing for Anomie's S-DSP Doc,
    https://www.romhacking.net/documents/191/ (index only; used to identify the
    document, download blocked).

## Implementation decisions

Written with the `SnesDspEngine` implementation (2026-09-28). Code:
`dsp/include/chipdsp/snes/` (`SnesTables.h`, `BrrCodec.h`, `SnesDsp.h`, `SnesDriver.h`,
`SnesDspEngine.h`) and `dsp/src/snes/`. No new external source was consulted; everything
below is either taken from the sections above or is a driver/engine choice, labelled so.

### Layers

* **Chip core** (`SnesDsp`, chip behaviour only): 128-byte register file, 64 KiB APU RAM
  image, 8 voices, global counter, noise LFSR, echo unit and mixer, stepped one 32 kHz
  sample per `step()` in the order of "Order of operations per sample". Every formula is
  the recommended one of this document: Anomie's Gaussian form (Ambiguity 1), bits 4..11
  as fraction (2), unclamped PMON step with the index clamped to 0x7FFF (3), no PMON on
  noise voices (4), Attack -> Decay tested every sample on the computed value (5), Anomie
  Decay -> Sustain test (6), E = 0 at a code-1 header (7), ENDX set when an end block's
  header is loaded and never for the first header after a key-on (8), the post-amp
  inversion on every output (9, 12), `hiddenEnv = new & 0x7FF` for bent increase (10),
  `(counter + offset) % period` events (11), all registers 0 and FLG = 0xE0 on reset (15),
  clamped MVOL/EVOL/EFB products (16), one counter decrement per sample (17), `(env * VOL)
  >> 6` voice volume (18), key-on sample #0 outputs the old level (21), Anomie's S3c order
  with the stored envelope applied before it is updated and KON handled after the FLG.7 and
  BRR-end checks (24).
* **Driver** (`SnesDriver`, software behaviour): what an SPC700 sound program would do.
  It only writes registers and APU RAM, never the chip's internal state.
* **Engine** (`SnesDspEngine`, plugin glue): parameters, sample banks, BRR encoding, the
  32 kHz -> host resampling and the DC blocker.

### Driver tick model

* The chip runs at exactly 32000 Hz. Before every chip sample the engine calls
  `SnesDriver::beforeSample()`.
* Tick: every 128 chip samples (4 ms, the typical SPC700 timer tick), counted from
  `reset()`. A tick rewrites the global registers from the parameters (DIR, MVOL, EFB,
  EON, NON, PMON, FIR0..7, EVOL, FLG noise clock and echo-write bit), runs the echo
  management below, recomputes every sounding voice's pitch (note + transpose + fine
  tune + vibrato) and writes PITCHL/H when it changed, and keys off (KOFF) voices whose
  GAIN release reads ENVX = 0.
* Key events are not delayed to the next tick: `noteOn`/`noteOff` are queued and flushed
  on the first chip sample of the next `renderBlock()` (the plugin splits blocks at MIDI
  events, see `ARCHITECTURE.md`). Because KON is polled every second sample and a second
  KON write before the poll replaces the first ("Key-on, key-off"), the driver collects all
  pending key-ons into one KON write and never writes KON twice within 2 samples; a key-on
  that arrives sooner waits for the next allowed sample.
* `setChannelPitch` only updates the channel's note; the new register value is written at
  the next tick, like a pitch slide in a real driver.
* Vibrato (driver): triangle wave of +/- `vibrato_depth` pitch-register units, one half
  cycle (from one peak to the other) every `vibrato_rate` ticks (period `2 * rate` ticks =
  `8 * rate` ms), after `vibrato_delay` ticks from key-on. Sampled once per tick at cycle
  position `x = (tick + ceil(rate / 2)) mod 2 * rate`, where `x = 0` is -depth and
  `x = rate` is +depth, so both peaks are reached for every rate 1..15; the first value is 0
  for even rates and the first step above 0 (`+depth / rate`, rounded) for odd rates, then
  rising. Rate 1 alternates +depth / -depth every tick. The depth is in register units, so
  its size in cents shrinks as the note rises (as in drivers that add a fixed offset).

### MIDI note -> registers

* Pitch: `P = round(4096 * storedRate / 32000 * 2^((note + transpose + fineTune / 100 -
  root) / 12))` clamped to 0..0x3FFF, then the vibrato offset is added and clamped again.
  `root` is the slot's root note (`setSampleRootNote`, default 60, fractional allowed) and
  `storedRate` the rate of the BRR data. The 4.12 grid is never smoothed.
* SRCN: slot `s` uses directory entry `2s` (one-shot) or `2s + 1` (looped), chosen at
  key-on from the slot's loop point and `loop_override` (see "APU RAM map").
* Envelope: `ADSR1 = adsr_enable << 7 | decay << 4 | attack`, `ADSR2 = sustain_level << 5 |
  sustain_rate`; `GAIN = gain_value` (direct, 0..127) or `0x80 / 0xA0 / 0xC0 / 0xE0 |
  min(gain_value, 31)` for gain_mode 1..4. ADSR2 and GAIN are written before ADSR1
  (SNESdev race note).
* `noise_enable` sets NON for all 8 voices, `noise_clock` is FLG bits 0-4, `pmon` sets
  PMON = 0xFE (voices 1..7 modulated by voice x-1; bit 0 has no function). The noise voice
  still steps through its BRR sample, so a one-shot sample ends a noise note.
* Instrument latching (driver): the instrument part of the parameters (sample,
  loop_override, ADSR/GAIN, release mode/rate, volume, pan) is captured when `noteOn` is
  called and written at key-on; later changes affect the next note only. Pitch-related
  parameters (transpose, fine tune, vibrato) and all global/echo parameters are live
  (applied at the next tick).

### Velocity mapping

Driver: `VxVOLL = round(volume * velocity * min(64, 64 - pan) / 64)`, `VxVOLR = round(volume *
velocity * min(64, 64 + pan) / 64)`, i.e. linear velocity and a balance pan law (the far
side is attenuated linearly, the near side keeps the full value). The registers are only
written at key-on, so they stay 0..127 (never negative: no phase inversion from pan).
MVOLL = MVOLR = `main_volume`.

### noteOff -> hardware

* `release_mode` 0: the driver sets the voice's KOFF bit (the register value is kept, KOFF
  acts continuously) and clears it again just before that voice's next KON, so the key-on is
  not undone by the next poll. Hardware release: -8 per sample, 256 samples = 8 ms from full.
* `release_mode` 1 (driver release): GAIN = `0xA0 | release_rate` (exponential decrease),
  then ADSR1 bit 7 cleared (GAIN written first). Like an SPC700 program, the driver polls
  ENVX ($x8 = E >> 4) at each tick and writes KOFF once it reads 0 (E < 16), so the last
  few levels may be released at -8 per sample. `release_rate` 0 is the GAIN "never" rate:
  the note holds until it is re-triggered (hardware consequence, kept).
* A note released before its key-on reached the chip is dropped (its KON bit is removed).
* `isChannelActive()` = pending key-on, or the chip reports the voice sounding: KON latched
  or start-up running, or not (Release with E = 0); for a GAIN release (release_mode 1) the
  channel is also inactive as soon as E = 0, without waiting for the next tick's KOFF
  (engine query of the chip state, not driver behaviour). A one-shot sample that ends
  releases the voice by itself (code-1 block).

### Chip behaviour vs driver/engine behaviour

Chip (`SnesDsp`): everything in the sections above this one (BRR decode, Gaussian,
pitch/PMON, ADSR/GAIN/counter, noise, echo/FIR, mixing, saturation, mute, inversion,
KON/KOFF polling, ENDX). Driver (`SnesDriver`): 4 ms tick, key event flushing, instrument
latching, pitch from MIDI notes, vibrato, velocity/pan, GAIN release, echo buffer placement,
EDL clamping and re-initialisation, FIR presets, APU RAM map. Engine: BRR encoding,
resampling of sources above 32 kHz, the sample banks, resampling to the host rate and the DC
blocker.

### APU RAM map and budget (driver)

```
0x0000-0x01FF  reserved (the zero page and stack a real SPC700 driver would use)
0x0200-0x02FF  sample directory, DIR = 0x02, 64 entries (2 per slot)
0x0300-0x0303  echo buffer when EDL = 0 (ESA = 0x03, one 4-byte entry)
0x0304-0x030C  silent block, code 3, looping to itself (empty slots)
0x030D-0x0315  silent block, code 1 (terminator: releases the voice when loaded)
0x0400-...     BRR data, slots packed in slot order
top            echo buffer for EDL >= 1: ESA = 0x100 - 8 * EDL, 0x10000 - 2048 * EDL .. 0xFFFF
```

* Budget: `loadSample` fails when the total BRR size would exceed `0x10000 - 0x0400 -
  2048 * echo_delay` bytes (64512 bytes at EDL 0, 33792 at EDL 15). The 1 KiB reserve is a
  driver decision (ENGINE_SPECS states "64 KiB minus the echo buffer"; a real driver also
  needs its code, which is not modelled). Edits that keep the size (`setSampleLoop`,
  `setSampleRootNote`) only check the physical limit.
* If `echo_delay` is raised above what fits below the loaded samples, the driver programs
  the largest EDL whose buffer does not overlap the data (`SnesDriver::maxEchoDelay`), so the
  echo can never overwrite samples (on hardware it would).
* Echo re-initialisation (driver, "Echo" section): when the effective EDL changes the
  driver sets FLG.5 (echo writes off), writes EVOL = 0, moves ESA, writes EDL, clears the new
  buffer in APU RAM and waits 60 ticks (240 ms = 7680 samples, counted from the tick after
  the change) before enabling writes and EVOL again, so
  the old length has run out (EDL only applies when the index wraps). After `reset()` the
  chip's echo index is 0, so the initial EDL applies at once without the wait. Switching
  echo on clears the buffer first (no stale repeats); switching it off sets FLG.5 and
  EVOL = 0.

### Samples, loops and BRR encoding (engine)

* Mono float PCM is converted to the 15-bit domain (`round(x * 16383)`), clamped to the
  Gaussian-safe range -0x3FFA..+0x3FF8 by the encoder. Sources above 32 kHz are first
  decimated to 32 kHz (Blackman-windowed sinc, cutoff 0.475 x 32 kHz); sources at or below
  32 kHz are stored at their own rate (smaller in RAM, pitch register scaled by rate/32000).
* Encoder: per block, filters 0..3 and shifts 0..12 are all tried; for each sample the
  nibble nearest to the residual and its two neighbours are evaluated with the real
  decoder (`brrDecodeSample`), keeping the history exactly as the S-DSP will see it; the
  candidate with the smallest squared error wins, with decoded values outside the safe
  range priced out. Block 0 and the loop block are forced to filter 0.
* Every sample is stored with its last block flagged code 3 (end + loop). Looping is then
  decided by the directory: the looped entry's loop address is the loop block (or the
  sample start when the slot has no loop point and `loop_override` forces a loop); the
  one-shot entry's loop address is the shared code-1 terminator block, whose header
  releases the voice with E = 0. Compared with a sample whose own last block is code 1, the
  last block is heard (the voice stops when the terminator header loads, one block later).
  This lets `loop_override` switch a slot between one-shot and loop without rewriting BRR
  data in RAM. The loop end is always the sample end (hardware).
* `setSampleLoop(slot, block)` re-encodes the slot with filter 0 at the loop block;
  `loadSample` resets the slot's loop point and keeps its root note.
* Threading (ARCHITECTURE.md): two pre-allocated banks (APU RAM image + slot table);
  `loadSample` rebuilds the bank the audio thread is not reading and flips an atomic
  `(generation << 1) | index`; at block start the audio thread claims the active bank,
  copies the directory, dummy blocks and BRR data into the chip's APU RAM when the
  generation changed, and releases the claim. The message thread only waits while the audio
  thread is copying from the bank it wants to write.

### Output stage

* The soft clip requested by the product owner is the hardware's own saturating 16-bit
  arithmetic: the voice sum, the echo sum and the final `main + echo` are clamped to
  -32768..32767 after each addition, the MVOL/EVOL/EFB products are clamped (Ambiguity 16),
  and nothing else limits the level. There is no additional soft-clipping curve.
* Main output: `~(mute ? 0 : clamp16(MVOL(mix) + EVOL(fir)))`. Per-voice buses: the voice
  alone through VxVOL, MVOL, mute and the same inversion, without echo (ENGINE_SPECS).
* Each 32 kHz output value (main L/R and, when the host asked for them, the 16 per-voice
  values) is handed to its own `BandLimitedStepSynth` as a step at the sample's host-time
  position (cutoff at 16 kHz, zero-order hold in raw mode), then the 5 Hz DC blocker
  (Ambiguity 23) removes the -1 LSB inversion offset and any DC. Per-voice work is skipped
  when the channel pointers are null.

