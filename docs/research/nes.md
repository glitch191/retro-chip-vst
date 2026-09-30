# NES 2A03/2A07 APU research

Self-contained specification of the Ricoh 2A03 (NTSC) / 2A07 (PAL) audio processing
unit, written for the implementer of `NesApu` / `Nes2A03Engine`. Every number comes
from public documentation (NESdev wiki, blargg's `apu_ref.txt`, Brad Taylor's 2A03
documents, the NESdev forum) or from a formula reproduced here and evaluated with the
Python script quoted in each section. No emulator source code was consulted.

Conventions used in this file:

* "CPU cycle" = one cycle of the 2A03/2A07 CPU clock (PHI2). "APU cycle" = 2 CPU cycles.
* `t` is an 11-bit timer value (0..2047 = $000..$7FF) as written to the registers.
* Tables are given as C++-ready `constexpr` arrays. Hex is kept where the source uses hex.
* `[unverified]` marks a value or statement that could not be confirmed against a
  fetched source and comes from memory; each one is also listed under "Ambiguities".
* Date consulted for all sources: 2026-09-28.

---

## Clocks and rates

Source: NESdev wiki "Cycle reference chart" (redirect target of "Clock rate"), "CPU".

| Item | NTSC (2A03) | PAL (2A07) |
|---|---|---|
| Master clock (exact) | 236250000 / 11 Hz = 21477272.727... Hz | 26601712.5 Hz |
| CPU clock = master / divider | master / 12 = 1789772.727... Hz | master / 16 = 1662607.03125 Hz |
| CPU clock, integer used by the engine | **1789773 Hz** | **1662607 Hz** |
| APU clock = CPU / 2 | 894886.36 Hz | 831303.52 Hz |
| CPU cycles per video frame | 29780.5 | 33247.5 |
| Video frame rate = CPU / cycles per frame | **60.098814 Hz** (wiki: 60.0988) | **50.006979 Hz** (wiki: 50.0070) |
| APU frame counter (4-step) rate, wiki | "60 Hz" ("every 29830 CPU cycles") | "50 Hz" |

Dendy (for completeness, not implemented): CPU = 26601712.5 / 15 = 1773447.5 Hz,
35464 CPU cycles per frame.

Verified 2026-09-28 against NESdev wiki "Cycle reference chart" (raw wikitext: 21.477272 MHz / 12,
26.601712 MHz / 16, 29780.5 / 33247.5 cycles per frame, 60.0988 / 50.0070 Hz) and recomputed
with `verify_nes.py` (all values of the table reproduced to the printed digits).

CPU vs APU cycle relation (NESdev wiki "APU", glossary, and "APU Misc"):

* "Pulse, noise, and DMC timers are clocked only on every second CPU cycle" (so their
  periods, in CPU cycles, are always even).
* The triangle timer "ticks at the rate of the CPU clock rather than the APU (CPU/2) clock".
* The frame counter is clocked "every other CPU cycle" (APU cycle); its step positions
  are given in APU cycles with `.5` fractions (see "Frame counter").

Python used for the exact values (script `gen_nes.py`, section "Clocks"):

```
NTSC_CPU_EXACT = Fraction(236250000, 11) / 12   # 19687500/11 = 1789772.727273 Hz
PAL_CPU_EXACT  = Fraction(266017125, 10) / 16   # 53203425/32 = 1662607.031250 Hz
NTSC frame rate = NTSC_CPU_EXACT / 29780.5 = 60.098814 Hz
PAL  frame rate = PAL_CPU_EXACT  / 33247.5 = 50.006979 Hz
```

Decision: the engine uses the integer clocks 1789773 / 1662607 Hz. Every NESdev period
table was generated from the same rounded values (the NESdev period-table script uses
`39375000/22 = 1789772.727` and the 80 resulting periods are identical with 1789773, see
"Period reference table"). Driver tick rates: 60.0988 Hz NTSC, 50.0070 Hz PAL
(`ENGINE_SPECS.md`).

```cpp
namespace nes {
constexpr double kCpuHzNtsc      = 1789773.0;      // 2A03, NESdev "CPU" / "Cycle reference chart"
constexpr double kCpuHzPal       = 1662607.0;      // 2A07
constexpr double kCpuHzNtscExact = 19687500.0 / 11.0;   // 1789772.727...
constexpr double kCpuHzPalExact  = 53203425.0 / 32.0;   // 1662607.03125
constexpr double kCpuCyclesPerFrameNtsc = 29780.5;
constexpr double kCpuCyclesPerFramePal  = 33247.5;
constexpr double kFrameHzNtsc = 60.098814;         // = kCpuHzNtscExact / 29780.5
constexpr double kFrameHzPal  = 50.006979;         // = kCpuHzPalExact  / 33247.5
}
```

---

## Registers

Source: NESdev wiki "APU" (register map and "Status", "Frame Counter" sections),
"APU registers", and the per-channel pages. All registers are write-only except $4015.
Bits marked `-` are unused. Writes to $4009 and $400D do nothing.

```
Address  Bits        Channel   Meaning
$4000    DDLC VVVV   Pulse 1   D duty (0-3); L envelope loop / length counter halt;
                               C constant volume; V volume (C=1) or envelope period (C=0)
$4001    EPPP NSSS   Pulse 1   E sweep enable; P sweep divider period (period = P+1 half frames);
                               N negate; S shift count
$4002    TTTT TTTT   Pulse 1   timer low 8 bits
$4003    LLLL LTTT   Pulse 1   L length counter load index (0-31); T timer high 3 bits.
                               Side effects: sequencer restarted at first value, envelope
                               start flag set, length counter loaded (if enabled)
$4004    DDLC VVVV   Pulse 2   as $4000
$4005    EPPP NSSS   Pulse 2   as $4001
$4006    TTTT TTTT   Pulse 2   as $4002
$4007    LLLL LTTT   Pulse 2   as $4003
$4008    CRRR RRRR   Triangle  C linear counter control flag AND length counter halt flag;
                               R linear counter reload value (0-127)
$4009    ---- ----   Triangle  unused
$400A    TTTT TTTT   Triangle  timer low 8 bits
$400B    LLLL LTTT   Triangle  L length counter load; T timer high 3 bits.
                               Side effect: sets the linear counter reload flag
$400C    --LC VVVV   Noise     L envelope loop / length halt; C constant volume; V volume/period
$400D    ---- ----   Noise     unused
$400E    M--- PPPP   Noise     M mode flag (0: bit-1 feedback, 32767-step; 1: bit-6 feedback,
                               93/31-step); P period index (0-15) into the noise period table
$400F    LLLL L---   Noise     L length counter load. Side effect: envelope start flag set
$4010    IL-- RRRR   DMC       I IRQ enable; L loop; R rate index (0-15) into the DMC rate table
$4011    -DDD DDDD   DMC       direct load: output level = D (0-127), takes effect immediately
$4012    AAAA AAAA   DMC       sample address = $C000 + A*64
$4013    LLLL LLLL   DMC       sample length = L*16 + 1 bytes
$4015 W  ---D NT21   all       channel enable: D DMC, N noise, T triangle, 2 pulse 2, 1 pulse 1
$4015 R  IF-D NT21   all       I DMC interrupt, F frame interrupt, D DMC bytes remaining > 0,
                               NT21 length counter > 0 for that channel
$4017 W  MI-- ----   all       M sequencer mode (0: 4-step, 1: 5-step); I IRQ inhibit
```

$4015 write semantics (NESdev wiki "APU", Status section, quoted):

* "Writing a zero to any of the channel enable bits (NT21) will silence that channel and
  halt its length counter."
* "If the DMC bit is clear, the DMC bytes remaining will be set to 0 and the DMC will
  silence when it empties."
* "If the DMC bit is set, the DMC sample will be restarted only if its bytes remaining is 0."
* "Writing to this register clears the DMC interrupt flag."
* "Power-up and reset have the effect of writing $00, silencing all channels."

$4015 read semantics: "N/T/2/1 will read as 1 if the corresponding length counter has not
been halted through either expiring or a write of 0 to the corresponding bit." "D will
read as 1 if the DMC bytes remaining is more than 0." "Reading this register clears the
frame interrupt flag (but not the DMC interrupt flag)."

Power-up state (NESdev wiki "CPU power up state", APU rows): $4017 = $00 (4-step mode,
frame IRQ enabled), $4015 = $00 (all channels disabled), $4000-$400F = $00, noise LFSR
"$0000 (all 0s, first clock shifts in a 1)" (see Ambiguities: equivalent to starting from
1), after reset "$4011 &= 1".

Engine decisions for the driver: the driver initialises like NESdev "APU basics"
(`$4000/$4004 = $30`, `$4001/$4005 = $08`, `$4008 = $80`, `$400C = $30`, `$4015 = $0F`,
`$4017 = $40`) then enables DMC in $4015 when a sample is triggered. `$4017 = $40`
selects the 4-step sequence with IRQ inhibited; the `$80`/`$C0` 5-step variant is the
alternative (see "Frame counter").

Verified 2026-09-28 against NESdev wiki "APU" ($4015 write/read sentences, $4017 `MI-- ----`),
"APU Envelope" (start flag set by $4003/$4007/$400F, not by the control registers) and
"CPU power up state" (raw: $4017 = 0 "frame IRQ enabled", $4015 = 0, $4000-$4013 = 0, noise LFSR
"$0000 (all 0s, first clock shifts in a 1)", after reset "$4011 &= 1").

```cpp
namespace nes::reg {
constexpr uint16_t kPulse1Ctrl = 0x4000, kPulse1Sweep = 0x4001, kPulse1TimerLo = 0x4002, kPulse1TimerHi = 0x4003;
constexpr uint16_t kPulse2Ctrl = 0x4004, kPulse2Sweep = 0x4005, kPulse2TimerLo = 0x4006, kPulse2TimerHi = 0x4007;
constexpr uint16_t kTriCtrl = 0x4008, kTriTimerLo = 0x400A, kTriTimerHi = 0x400B;
constexpr uint16_t kNoiseCtrl = 0x400C, kNoisePeriod = 0x400E, kNoiseLength = 0x400F;
constexpr uint16_t kDmcCtrl = 0x4010, kDmcDirect = 0x4011, kDmcAddress = 0x4012, kDmcLength = 0x4013;
constexpr uint16_t kStatus = 0x4015, kFrameCounter = 0x4017;
}
```

---

## Pulse channels (pulse 1 = $4000-$4003, pulse 2 = $4004-$4007)

Source: NESdev wiki "APU Pulse"; cross-check blargg `apu_ref.txt`, Brad Taylor
"2A03 technical reference".

Structure (wiki): envelope generator -> sweep unit -> timer -> 8-step sequencer ->
length counter gate -> mixer. The mixer receives the envelope volume (0..15) only when
all of the following hold, otherwise it receives 0:

1. the sequencer output is 1,
2. the sweep unit is not muting (target period <= $7FF, see "Sweep"),
3. the length counter is non-zero,
4. the current timer period `t >= 8` ("A period of t < 8, either set explicitly or via a
   sweep period update, silences the corresponding pulse channel").

Timer: 11 bits, `t = HHH LLLLLLLL` ($4003 bits 2-0 : $4002). It is a divider clocked
every APU cycle (every second CPU cycle). It counts t, t-1, ..., 0 and then reloads t;
the sequencer advances one step each time the divider wraps. One sequencer step
therefore lasts `2 * (t + 1)` CPU cycles and one full 8-step period lasts
`16 * (t + 1)` CPU cycles.

Frequency formula (wiki): `f_pulse = f_CPU / (16 * (t + 1))`, inverse
`t = f_CPU / (16 * f) - 1`. Range on NTSC: t = $7FF -> 54.62 Hz, t = 8 -> 12428.98 Hz
(wiki: "approximately 54.6 Hz to 12.4 kHz"); t < 8 is silent.

Duty sequences, in time order (the first value is the output right after a $4003/$4007
write restarts the sequencer). Both the wiki table's "sequence" column and its
"waveform" column agree on this time order:

```cpp
namespace nes {
// NESdev wiki "APU Pulse": Duty | waveform (time order) | duty ratio
constexpr uint8_t kPulseDuty[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0},   // 0: 12.5 %
    {0, 1, 1, 0, 0, 0, 0, 0},   // 1: 25 %
    {0, 1, 1, 1, 1, 0, 0, 0},   // 2: 50 %
    {1, 0, 0, 1, 1, 1, 1, 1},   // 3: 25 % negated (75 %)
};
}
```

Verified 2026-09-28 against NESdev wiki "APU Pulse" (raw wikitext: sequence lookup table
`0000 0001 / 0000 0011 / 0000 1111 / 1111 1100`, output waveform `0 1 0 0 0 0 0 0 / 0 1 1 0 0 0 0 0 /
0 1 1 1 1 0 0 0 / 1 0 0 1 1 1 1 1`, "8*(t+1) APU cycles, or equivalently 16*(t+1) CPU cycles",
about 12.4 kHz maximum). blargg's `apu_ref.txt` draws the four sequences graphically
(dash/underscore art) with the same 12.5 / 25 / 50 / 25 %-negated labels; it was not usable as a
digit-by-digit cross-check.

Verified 2026-09-28 (second pass) against NESdev wiki "APU Pulse" raw wikitext: both columns
refetched and identical; the page also states that the sequencer counter "counts downward
rather than upward. Thus it reads the sequence lookup table in the order 0, 7, 6, 5, 4, 3, 2,
1", which turns the lookup column into the time-order column used by `kPulseDuty`.

Register side effects (wiki "APU Pulse", quoted): on a $4003/$4007 write "The sequencer
is immediately restarted at the first value of the current sequence. The envelope is
also restarted. The period divider is not reset." Writing $4002/$4006 does not restart
the sequencer. Changing the duty in $4000/$4004 does not reset the sequencer position;
it only changes which sequence the current position indexes.

Implementation notes:

* Because the divider is not reset by $4002/$4003 writes (wiki: "The period divider is
  not reset"), a period change does not restart the current count: the divider keeps
  counting down from its current value and loads the new `t` the next time it reaches 0
  and reloads (NESdev "APU Misc" divider definition: reload from the period on reaching 0).
* Velocity mapping (`ENGINE_SPECS.md`): 4-bit volume = round(velocity * 15).

---

## Sweep unit (one per pulse channel)

Source: NESdev wiki "APU Sweep"; cross-check blargg `apu_ref.txt`, Brad Taylor.

Register `$4001` / `$4005` = `EPPP.NSSS`:

* `E` (bit 7): enabled flag.
* `PPP` (bits 6-4): "The divider's period is P + 1 half-frames".
* `N` (bit 3): negate flag. "0: add to period, sweeping toward lower frequencies.
  1: subtract from period, sweeping toward higher frequencies".
* `SSS` (bits 2-0): shift count. "If SSS is 0, then behaves like E=0" (no period updates;
  muting is still computed, see below).
* Side effect of the write: sets the sweep reload flag.

Target period computation (continuous, combinational; recomputed whenever the current
period or the sweep settings change, "whether by $400x writes or by sweep updating the
period"):

```
change = current_period >> shift_count               // 11-bit barrel shifter
if negate:
    pulse 1: change = -change - 1                    // ones' complement: "Making 20 negative produces -21"
    pulse 2: change = -change                        // two's complement: "Making 20 negative produces -20"
target = current_period + change
if target < 0: target = 0                            // "clamped to zero if this sum is negative"
```

Muting (applies "regardless of whether the sweep unit is disabled" and "regardless of
whether the sweep divider is outputting a clock signal"):

* "If the current period is less than 8, the sweep unit mutes the channel."
* "If at any time the target period is greater than $7FF, the sweep unit mutes the channel."
* Consequence quoted from the wiki: "If the negate flag is false, the shift count is
  zero, and the current period is at least $400, the target period will be large enough
  to mute the channel." (With shift 0 and negate 0 the target is 2 * period.)

Half-frame clock procedure (executed on every half-frame clock of the frame counter,
"at 120 or 96 Hz"), in this order:

1. If the divider's counter is zero AND the sweep is enabled AND the shift count is
   non-zero AND the channel is not muting: `current_period = target`. If muting, the
   period is left unchanged "but the sweep unit's divider continues to count down and
   reload the divider's period as normal".
2. If the divider's counter is zero OR the reload flag is set: the divider counter is
   set to `P` and the reload flag is cleared. Otherwise the divider counter is decremented.

Worked examples (from `gen_nes.py`, section "Sweep target examples"):

```
period=0x100 shift=1 negate=0: target 0x180 (both channels)
period=0x100 shift=1 negate=1: pulse 1 target 0x07F, pulse 2 target 0x080
period=0x3FF shift=0 negate=0: target 0x7FE, not muted
period=0x400 shift=0 negate=0: target 0x800, MUTED (even with E=0)
period=0x7FF shift=3 negate=0: target 0x8FE, MUTED
period=0x0AB shift=2 negate=1: pulse 1 target 0x080, pulse 2 target 0x081
period=0x001 shift=0 negate=1: target clamped to 0; muted because period < 8
```

Muting note for the engine: the mute is a gate on the mixer input; it does not stop
the timer or sequencer.

Verified 2026-09-28 against NESdev wiki "APU Sweep" (raw wikitext: `EPPP.NSSS`, "P + 1
half-frames", ones' complement "-c - 1" / "Making 20 negative produces -21" for pulse 1 and
two's complement "-c" / "-20" for pulse 2, clamp to zero, the two muting rules and the
$400 example, the two-step half-frame procedure in this order) and blargg `apu_ref.txt`
("The shifted value's bits are inverted, and on the second square channel, the inverted
value is incremented by 1"). The seven worked examples were recomputed with `verify_nes.py`
(all identical).

---

## Envelope generator (pulse 1, pulse 2, noise)

Source: NESdev wiki "APU Envelope" (full text obtained).

Components: start flag, divider, decay level counter. Inputs: envelope parameter `V`
($4000/$4004/$400C bits 3-0), loop flag (bit 5, shared with the length counter halt
flag), constant volume flag (bit 4).

Quarter-frame clock procedure (quoted):

* "if the start flag is clear, the divider is clocked, otherwise the start flag is
  cleared, the decay level counter is loaded with 15, and the divider's period is
  immediately reloaded."
* "When the divider is clocked while at 0, it is loaded with V and clocks the decay
  level counter. Then one of two actions occurs: If the counter is non-zero, it is
  decremented, otherwise if the loop flag is set, the decay level counter is loaded
  with 15."

So the divider period is `V + 1` quarter frames; the decay level steps 15, 14, ..., 0
once every `V + 1` quarter frames and then either stays at 0 (loop clear) or wraps to 15
(loop set).

Output: "if [the constant volume flag is] set, the envelope parameter directly sets the
volume, otherwise the decay level is the current volume. The constant volume flag has
no effect besides selecting the volume source; the decay level will still be updated
when constant volume is selected."

Start flag: set by writes to $4003, $4007 and $400F.

Timing references (NTSC, 4-step mode, quarter frame = 239.996 Hz; `gen_nes.py`):

```
V = 0 : one decay step every 4.167 ms, 15 -> 0 in 0.0625 s
V = 1 : one decay step every 8.333 ms, 15 -> 0 in 0.1250 s
V = 7 : one decay step every 33.33 ms, 15 -> 0 in 0.5000 s
V = 15: one decay step every 66.67 ms, 15 -> 0 in 1.0000 s
general: decay step rate = quarter_frame_rate / (V + 1)   (Brad Taylor: "240Hz/(N+1)")
```

Verified 2026-09-28 against NESdev wiki "APU Envelope" (raw wikitext: the quarter-frame
procedure and the constant-volume sentence quoted above; "Sets start flag" listed as the side
effect of $4003 / $4007 / $400F). Timing values recomputed with `verify_nes.py` at
239.996343 Hz: 4.166730 / 8.333460 / 33.333841 / 66.667683 ms per step, 15 -> 0 in
0.062501 / 0.125002 / 0.500008 / 1.000015 s.

---

## Length counter (pulse 1, pulse 2, triangle, noise)

Source: NESdev wiki "APU Length Counter" (table transcribed); cross-check blargg
`apu_ref.txt` (same values in hex) and Brad Taylor (same values in 60 Hz frames, i.e.
exactly half).

Load: writing $4003 / $4007 / $400B / $400F loads the length counter with entry
`L = bits 7-3` of the written value, "If the enabled flag is set" (the channel's bit in
$4015). Halt flag: $4000 bit 5, $4004 bit 5, $4008 bit 7, $400C bit 5.

Clocking (half-frame clock, quoted): "the length counter is decremented except when:
The length counter is 0, or The halt flag is set." "When the enabled bit is cleared (via
$4015), the length counter is forced to 0 and cannot be changed until enabled is set
again." A channel whose length counter is 0 outputs 0 to the mixer (pulse/noise) or stops
its sequencer (triangle).

Table as printed by the wiki (values are half-frame clocks):

```
     |  0   1   2   3   4   5   6   7    8   9   A   B   C   D   E   F
-----+----------------------------------------------------------------
00-0F  10,254, 20,  2, 40,  4, 80,  6, 160,  8, 60, 10, 14, 12, 26, 14,
10-1F  12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
```

```cpp
namespace nes {
// NESdev wiki "APU Length Counter", index = bits 7-3 of $4003/$4007/$400B/$400F.
// Units: half-frame clocks (120 Hz in NTSC 4-step mode).
constexpr uint8_t kLengthTable[32] = {
     10, 254,  20,   2,  40,   4,  80,   6, 160,   8,  60,  10,  14,  12,  26,  14,
     12,  16,  24,  18,  48,  20,  96,  22, 192,  24,  72,  26,  16,  28,  32,  30,
};
}
```

Cross-check: blargg lists the even indices as `$0A,$14,$28,$50,$A0,$3C,$0E,$1A,$0C,$18,
$30,$60,$C0,$48,$10,$20` and the odd indices as `$FE,$02,$04,...,$1E`; identical.
Brad Taylor lists `05,0A,14,28,50,1E,07,0D / 06,0C,18,30,60,24,08,10 / 7F,01..0F` in
"frames" (60 Hz): every entry is exactly half of the wiki value, consistent with the
wiki counting half frames.

Longest note: entry 1 = 254 half frames = 254 / 119.998 Hz = 2.1167 s (NTSC 4-step).

Verified 2026-09-28 against NESdev wiki "APU Length Counter" (table refetched, all 32 entries
identical) and Brad Taylor "2A03 technical reference" (length counter section refetched: bit3=0
table `05,0A,14,28,50,1E,07,0D` (bit 7 = 0) / `06,0C,18,30,60,24,08,10` (bit 7 = 1), bit3=1
table `7F,01,02,...,0F`, "conditionally clocked at a frequency of 60 Hz"; `verify_nes.py`
confirms every entry is exactly half of the wiki value).

Verified 2026-09-28 (second pass) against NESdev wiki "APU Length Counter" raw wikitext (the
two 16-value rows refetched, identical to `kLengthTable`), blargg `apu_ref.txt` (even/odd hex
columns identical) and Brad Taylor (bits 4-6 x bit 7 tables and the `7F, 01-0F` table
refetched; mapped to index = bits 7-3 they give exactly half of each wiki entry).

---

## Triangle channel ($4008, $400A, $400B)

Source: NESdev wiki "APU Triangle" (sentences transcribed); blargg `apu_ref.txt` for
the ultrasonic case; NESdev "APU basics" for the "period 0 silences with a pop" note.

* "It has no volume control; the waveform is only either cycling or suspended."
* Timer: 11 bits (`$400B` bits 2-0 : `$400A`), period `t + 1`, "the timer counts t,
  t-1, ..., 0, t, t-1, ..., clocking the waveform generator when it goes from 0 to t".
  "Unlike the pulse channels, the timer of the triangle channel ticks at the rate of the
  CPU clock rather than the APU (CPU/2) clock."
* "The sequencer is clocked by the timer as long as both the linear counter and the
  length counter are nonzero." "The triangle channel stops the sequence only when either
  the length counter or the linear counter is zero."
* Frequency: `f_tri = f_CPU / (32 * (t + 1))`, `t = f_CPU / (32 * f) - 1`. For the same
  `t` the triangle sounds one octave below a pulse channel. Maximum "fCPU/32 (about 55.9
  kHz for NTSC)" at t = 0.

32-step sequence sent to the mixer (the sequencer restarts nowhere: there is no phase
reset register; the sequence simply continues from wherever it stopped):

```cpp
namespace nes {
// NESdev wiki "APU Triangle" (blargg: "F E D C B A 9 8 7 6 5 4 3 2 1 0 0 1 2 ... F")
constexpr uint8_t kTriangleSequence[32] = {
    15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
};
}
```

Linear counter ($4008 = `CRRR RRRR`): reload value `R` (0..127), control flag `C`
(also the length counter halt flag), plus an internal reload flag.

Quarter-frame clock procedure (quoted, in order):

1. "If the linear counter reload flag is set the counter is reloaded with the counter
   reload value, otherwise, if the counter is non-zero, it is decremented."
2. "If the counter control flag is clear, the linear counter reload flag is cleared."

"Writing to [$400B] sets linear counter reload flag too." "Note that the counter reload
flag is not cleared unless the control flag is also clear, so when both are already set a
value written to $4008 will be reloaded at the next linear counter clock." With `C = 1`
the counter is therefore reloaded on every quarter frame and never reaches 0: the
triangle plays until the driver writes `C = 0` (after which the counter counts down from
`R` and silences the channel `R` quarter frames later; `R = 127` gives 127 / 240 = 0.529 s
NTSC). This is the `linear_length` parameter of `ENGINE_SPECS.md` (127 = hold).

Halting behaviour: the wiki only says the sequence "stops"/is "suspended". blargg's
`apu_ref.txt` is explicit (Triangle Channel section, quoted): "Unlike the other waveform
channels, the triangle channel is silenced by stopping its waveform at whatever phase it's
at, rather than causing zero to be sent to its DAC." The output value therefore stays at the
current sequence step (it is not forced to 0); resuming continues from that step. (Resolved
2026-09-28; see Ambiguities A3.)

Ultrasonic periods (t = 0 and t = 1): the wiki has no sentence about them. blargg
(`apu_ref.txt`): "At the lowest two periods ($400B = 0 and $400A = 0 or 1), the
resulting frequency is so high that the DAC effectively outputs a value half way between
7 and 8." NESdev "APU basics": "Writing a raw period of 0 also silences the wave, but
produces a pop". Frequencies: t = 0 -> 55930.4 Hz (NTSC) / 51956.5 Hz (PAL);
t = 1 -> 27965.2 Hz / 25978.2 Hz. Decision: see Ambiguities A2 (recommended: for
`t < 2` output the constant 7.5 average instead of stepping the sequencer at up to
1.79 M steps/s; the alternative is to run the sequencer and let the band-limited
resampler average it).

Verified 2026-09-28 against NESdev wiki "APU Triangle" (raw wikitext: 32-step sequence,
CPU-rate timer sentence, `f = fCPU / (32 * (t + 1))`, "about 55.9 kHz", the two-step linear
counter procedure, the $400B and reload-flag sentences) and blargg `apu_ref.txt` ("half way
between 7 and 8", halting sentence above). Frequencies recomputed with `verify_nes.py`.

---

## Noise channel ($400C, $400E, $400F)

Source: NESdev wiki "APU Noise" (tables and LFSR text transcribed); cross-check blargg
`apu_ref.txt` and Brad Taylor / NESSOUND.txt.

Components: envelope generator, timer, 15-bit linear feedback shift register (LFSR),
length counter. Output to the mixer: the envelope volume when bit 0 of the shift
register is **clear** and the length counter is non-zero; otherwise 0. (Equivalently:
`out = (lfsr & 1) ? 0 : volume`.)

LFSR clocking, quoted: "Feedback is calculated as the exclusive-OR of bit 0 and one
other bit: bit 6 if Mode flag is set, otherwise bit 1. The shift register is shifted
right by one bit. Bit 14, the leftmost bit, is set to the feedback calculated earlier."
"On power-up, the shift register is loaded with the value 1." Sequence lengths: "32767
steps long when Mode flag is clear, and randomly 93 or 31 steps long otherwise. (The
particular 31- or 93-step sequence depends on where in the 32767-step sequence the shift
register was when Mode flag was set)."

```cpp
inline uint16_t noiseStep(uint16_t lfsr, bool mode) {
    const uint16_t feedback = (lfsr ^ (lfsr >> (mode ? 6 : 1))) & 1u;
    return static_cast<uint16_t>((lfsr >> 1) | (feedback << 14));
}
```

Verified with `gen_nes.py` and again with `verify_nes.py` (2026-09-28): from state 1, mode 0
has period 32767 (all non-zero states, no pre-period); mode 1 from state 1 has period 93.
The mode-1 step is a bijection on the 32768 states, so every state lies on a cycle: the
32767 non-zero states split into 352 distinct 93-step cycles (352 x 93 = 32736 states) and
one 31-step cycle (31 states). Documented start states for tests: `$0001`, `$4000` and
`$7FFF` are on 93-step cycles; `$0737` (the smallest state of the 31-step cycle, reached
after 14739 mode-0 clocks from 1) has period 31 in mode 1, and its 31 bit-0 outputs are
`1101100111000011010100100010111` (bit 0 of the state after each mode-1 clock, as in the
16-state lists below: the first digit belongs to the state after one clock, the 31st to
`$0737` itself; bit 0 of `$0737` read before any clock is 1, so the string read from the
start state is the same string rotated right by one, `1110110011100001101010010001011`). First 16 states after clocking from 1 in mode 0: `$4000,
$2000, $1000, $0800, $0400, $0200, $0100, $0080, $0040, $0020, $0010, $0008, $0004, $0002,
$4001, $6000`; the first 16 bit-0 outputs are `0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0` (so the
channel is audible, volume output, for the first 14 clocks). In mode 1 from 1 the first 16
states are `$4000, $2000, $1000, $0800, $0400, $0200, $0100, $0080, $0040, $4020, $2010,
$1008, $0804, $0402, $0201, $4100`.

Timer: "The period determines how many CPU cycles happen between shift register clocks.
These periods are all even numbers because there are 2 CPU cycles in an APU cycle."

```cpp
namespace nes {
// NESdev wiki "APU Noise", $400E bits 3-0 -> CPU cycles between LFSR clocks.
constexpr uint16_t kNoisePeriodNtsc[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };
constexpr uint16_t kNoisePeriodPal[16] = {
    4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778 };
}
```

Cross-checks: blargg lists NTSC as `$004,$008,$010,$020,$040,$060,$080,$0A0,$0CA,$0FE,
$17C,$1FC,$2FA,$3F8,$7F2,$FE4` (identical in decimal). Brad Taylor / NESSOUND list
`$002,$004,$008,$010,$020,$030,$040,$050,$065,$07F,$0BE,$0FE,$17D,$1FC,$3F9,$7F2`, i.e.
exactly half of every value (`2 * value` reproduces the wiki table for all 16 entries).
Correction 2026-09-28 (second verification pass): Brad Taylor's column is headed "CPU
clock cycles (11-bit wavelength+1)" and his timers are stated to run "at the 2A03's
internal 6502 speed (1.79 MHz)", so his table is not in APU clocks: taken literally it
disagrees with the wiki and blargg by a factor of 2 (one octave). See A5.

LFSR clock frequencies (`f_CPU / period`, `gen_nes.py`):

```
idx : NTSC Hz         PAL Hz          (93-step tone pitch NTSC = f/93)
 0  : 447443.250000   415651.750000   4811.217742
 1  : 223721.625000   207825.875000   2405.608871
 2  : 111860.812500   118757.642857   1202.804435
 3  :  55930.406250    55420.233333    601.402218
 4  :  27965.203125    27710.116667    300.701109
 5  :  18643.468750    18893.261364    200.467406
 6  :  13982.601562    14089.889831    150.350554
 7  :  11186.081250    11233.831081    120.280444
 8  :   8860.262376     8843.654255     95.271638
 9  :   7046.350394     7044.944915     75.767209
10  :   4709.928947     4696.629944     50.644397
11  :   3523.175197     3522.472458     37.883604
12  :   2348.783465     2348.314972     25.255736
13  :   1761.587598     1761.236229     18.941802
14  :    879.927729      879.686243      9.461588
15  :    439.963864      440.075966      4.730794
```

Historical note (wiki, quoted): "In the earliest revisions of the 2A03 CPU, the Mode flag
was nonexistent: the shift register always used bits 0 and 1 for feedback", and its longest
period differed from 4068 CPU cycles; not modelled.

Verified 2026-09-28 against NESdev wiki "APU Noise" (rendered page and raw wikitext: both
16-entry tables in CPU cycles identical, feedback/shift/power-up sentences, "32767 ... 93 or
31", output when bit 0 is clear) and blargg `apu_ref.txt` (NTSC hex table identical);
Brad Taylor's table refetched (`002 ... 7F2`) and checked as exactly half of every wiki value
in `verify_nes.py`. The LFSR clock-frequency table was recomputed (all 48 values identical).

Verified 2026-09-28 (second pass) against NESdev wiki "APU Noise" raw wikitext (both 16-entry
tables refetched, identical), blargg `apu_ref.txt` (NTSC hex table refetched, identical) and
Brad Taylor "2A03 technical reference" / NESSOUND.txt (table and its "CPU clock cycles" heading
refetched; factor-2 disagreement recorded in A5). LFSR periods (32767 / 93 / 31), the 352 x 93 +
1 x 31 cycle split, the first-16-state lists, the 14739-clock distance to `$0737` and the 48
clock frequencies were recomputed with an independent script (`v2.py`): all identical.

---

## DMC channel ($4010-$4013)

Source: NESdev wiki "APU DMC" (procedures quoted); cross-check blargg `apu_ref.txt`
(rate table in hex) and Brad Taylor (rate table per byte).

Components: memory reader (address counter, bytes-remaining counter, 1-byte sample
buffer), timer, output unit (8-bit shift register, bits-remaining counter, silence
flag, 7-bit output level).

Registers:

* `$4010 = IL-- RRRR`: I = IRQ enable, L = loop, R = rate index.
* `$4011 = -DDD DDDD`: "The DMC output level is set to D, an unsigned value." Takes effect
  immediately (this is how games play raw PCM and how `Super Mario Bros.` / `StarTropics`
  use it as a crude volume control for triangle and noise via the mixer non-linearity).
  Hardware glitch, not modelled: "If the timer is outputting a clock at the same time,
  the output level is occasionally not changed properly."
* `$4012`: "Sample address = %11AAAAAA.AA000000 = $C000 + (A * 64)"; range $C000..$FFC0.
* `$4013`: "Sample length = %LLLL.LLLL0001 = (L * 16) + 1 bytes"; range 1..4081 bytes.

Rate tables ("The rate determines for how many CPU cycles happen between changes in the
output level during automatic delta-encoded sample playback"):

```cpp
namespace nes {
// NESdev wiki "APU DMC", $4010 bits 3-0 -> CPU cycles per output bit.
constexpr uint16_t kDmcPeriodNtsc[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106,  84,  72,  54 };
constexpr uint16_t kDmcPeriodPal[16] = {
    398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118,  98,  78,  66,  50 };
}
```

Resulting bit rates (`f_CPU / period`, `gen_nes.py`; wiki quotes 4181.71 Hz and
33143.9 Hz for NTSC indices 0 and 15):

```
idx : NTSC Hz        PAL Hz
 0  :  4181.712617   4177.404523
 1  :  4709.928947   4696.629944
 2  :  5264.038235   5261.414557
 3  :  5593.040625   5579.218121
 4  :  6257.947552   6023.938406
 5  :  7046.350394   7044.944915
 6  :  7919.349558   7917.176190
 7  :  8363.425234   8397.005051
 8  :  9419.857895   9446.630682
 9  : 11186.081250  11233.831081
10  : 12604.035211  12595.507576
11  : 13982.601562  14089.889831
12  : 16884.650943  16965.377551
13  : 21306.821429  21315.474359
14  : 24857.958333  25191.015152
15  : 33143.944444  33252.140000
```

Cross-checks: blargg's NTSC table `$1AC,$17C,$154,$140,$11E,$0FE,$0E2,$0D6,$0BE,$0A0,
$08E,$080,$06A,$054,$048,$036` is identical. Brad Taylor's table is per *byte* (8 bits):
`$D60,$BE0,$AA0,$A00,$8F0,$7F0,$710,$6B0,$5F0,$500,$470,$400,$350,$2A8,$240,$1B0`;
divided by 8 it matches except index 13 ($2A8 / 8 = 85 vs 84) - see Ambiguities A6.

Verified 2026-09-28 against NESdev wiki "APU DMC" (both rate tables refetched, identical;
$4012/$4013 formulas; the "at least 2 / at most 125" clamp sentence; silence flag and
$8000 wrap sentences) and blargg `apu_ref.txt` (NTSC hex table identical). Brad Taylor's
per-byte table refetched: entry D is printed `2A8`, so the index-13 discrepancy is real and
stays in A6. Bit rates recomputed (all 32 values identical).

Verified 2026-09-28 (second pass) against NESdev wiki "APU DMC" raw wikitext (both tables,
$4012/$4013 formulas and the "leave the output level unchanged" clamp sentence refetched),
blargg `apu_ref.txt` and Brad Taylor (refetched; `$D60 / 8 = 428` ... `$1B0 / 8 = 54`, only
index 13 differs). All 32 bit rates recomputed independently: identical. Sanity check: the
PAL/NTSC period ratio is 0.917..0.932 for every rate index except index 4 (276 / 286 =
0.965); the wiki is the only source of the PAL table, so 276 is kept (see A19).

Output unit, on every timer clock (quoted):

1. "If the silence flag is clear, the output level changes based on bit 0 of the shift
   register. If the bit is 1, add 2; otherwise, subtract 2." Clamp rule: "subtract 2 only
   if the current level is at least 2, or add 2 only if the current level is at most 125"
   - when the change would leave 0..127 the level is left **unchanged** (no saturation to
   the bound, no wrap).
2. "The right shift register is clocked." (bit 0 is consumed first: "the channel plays
   the least significant bits of each byte before playing the most significant bits").
3. "The bits-remaining counter is decremented. If it becomes zero, a new output cycle is
   started."

New output cycle: "The bits-remaining counter is loaded with 8. If the sample buffer is
empty, then the silence flag is set; otherwise, the silence flag is cleared and the
sample buffer is emptied into the shift register." While the silence flag is set the
output level is held (it does not decay to 0).

Memory reader: whenever the sample buffer is empty and bytes remaining > 0, "The sample
buffer is filled with the next sample byte read from the current address ... The address
is incremented; if it exceeds $FFFF, it is wrapped around to $8000. The bytes remaining
counter is decremented; if it becomes zero and the loop flag is set, the sample is
restarted (see above); otherwise, if the bytes remaining counter becomes zero and the IRQ
enabled flag is set, the interrupt flag is set." Restart = address counter reloaded from
$4012 and bytes remaining from $4013. (On hardware the fetch is a DMA that stalls the
CPU; irrelevant here.)

Starting/stopping: see $4015 in "Registers" (set bit 4 with bytes remaining 0 to start;
clearing bit 4 sets bytes remaining to 0 and the DMC silences when the buffer empties).
The output level itself is never reset by $4015; it keeps its last value.

Engine notes (from `ENGINE_SPECS.md`): 16 sample slots of up to 4081 bytes each; the
1-bit delta encoder must respect the +/-2 step and the clamp rule (level stays when a step
would leave 0..127) so that decoding reproduces the encoder's internal model.

```cpp
inline void dmcOutputStep(uint8_t& level, bool bit) {
    if (bit) { if (level <= 125) level += 2; }
    else     { if (level >= 2)   level -= 2; }
}
```

---

## Frame counter ($4017)

Source: NESdev wiki "APU Frame Counter" (tables transcribed from the raw page);
cross-check blargg `apu_ref.txt` (event pattern) and Brad Taylor (quoted: "An internal
clock edge divider of 14915 off the 2A03's PHI2 line is used to get 240Hz"; counting both
edges of PHI2 is 2 x 1789773 / 14915 = 240 Hz, i.e. one quarter frame per 7457.5 CPU cycles
on average, consistent with the 29830-cycle sequence below).

$4017 = `MI-- ----`: M = 0 selects the 4-step sequence (mode 0), M = 1 the 5-step
sequence (mode 1); I = interrupt inhibit. Quarter-frame signals clock the envelopes and
the triangle linear counter; half-frame signals clock the length counters and sweep units.

Mode 0 (4-step). Positions in APU cycles from the sequencer reset; `.5` means the second
CPU cycle of that APU cycle, so the CPU-cycle column is exact:

```
Step   NTSC APU  NTSC CPU   PAL APU   PAL CPU   Quarter  Half   Frame IRQ (if inhibit clear)
1      3728.5     7457      4156.5     8313     yes      -      -
2      7456.5    14913      8313.5    16627     yes      yes    -
3     11185.5    22371     12469.5    24939     yes      -      -
4     14914      29828     16626      33252     -        -      set
      14914.5    29829     16626.5    33253     yes      yes    set
      14915 (=0) 29830     16627 (=0) 33254     -        -      set, sequencer wraps to 0
```

Sequence length: 14915 APU cycles = **29830 CPU cycles** (NTSC), 16627 APU cycles =
**33254 CPU cycles** (PAL). Wiki: "the interrupt flag is set every 29830 CPU cycles".

Mode 1 (5-step). "The frame interrupt flag is never set."

```
Step   NTSC APU  NTSC CPU   PAL APU   PAL CPU   Quarter  Half
1      3728.5     7457      4156.5     8313     yes      -
2      7456.5    14913      8313.5    16627     yes      yes
3     11185.5    22371     12469.5    24939     yes      -
4     14914.5    29829     16626.5    33253     -        -      (no clocks)
5     18640.5    37281     20782.5    41565     yes      yes
      18641 (=0) 37282     20783 (=0) 41566     -        -      sequencer wraps to 0
```

Sequence length: 18641 APU cycles = **37282 CPU cycles** (NTSC), 20783 APU cycles =
**41566 CPU cycles** (PAL).

Resulting rates (`gen_nes.py`, exact CPU clocks):

```
NTSC mode 0: sequence 59.999086 Hz, quarter frame 239.996343 Hz, half frame 119.998171 Hz
NTSC mode 1: sequence 48.006350 Hz, quarter frame 192.025399 Hz, half frame  96.012699 Hz
PAL  mode 0: sequence 49.997204 Hz, quarter frame 199.988817 Hz, half frame  99.994409 Hz
PAL  mode 1: sequence 39.999207 Hz, quarter frame 159.996827 Hz, half frame  79.998414 Hz
```

Writing $4017 (wiki): "After 3 or 4 CPU clock cycles*, the timer is reset. * If the write
occurs during an APU cycle, the effects occur 3 CPU cycles after the $4017 write cycle,
and if the write occurs between APU cycles, the effects occur 4 CPU cycles after the write
cycle." "If the mode flag is set, then both quarter frame and half frame signals are also
generated" (immediately, at that reset). With bit 7 clear only the sequence is reset
without clocking anything. Brad Taylor only partly agrees: "both these counters are
clocked once immediately after $4017.7 is written with a value of 1" refers to the linear
and envelope counters (quarter-frame) only, and NESSOUND places the length/sweep clocks at
"Count sequences 1 & 3" in both modes; this conflicts with the wiki and blargg for the
5-step mode (see A17). Corrected 2026-09-28 (second pass); the earlier wording claimed full
agreement.

Writing $4017 with bit 7 set at every video frame is how commercial drivers synchronised
the envelopes to the frame; the engine's driver runs once per video frame (60.0988 /
50.0070 Hz) but the APU counters keep their own 29830/33254-cycle period, as on hardware.

```cpp
namespace nes {
// CPU cycles at which each event fires, counted from the sequencer reset (NESdev "APU Frame Counter").
struct FrameStep { uint32_t cpuCycle; bool quarter; bool half; };
constexpr FrameStep kFrame4StepNtsc[] = { {7457,1,0}, {14913,1,1}, {22371,1,0}, {29829,1,1} };
constexpr uint32_t  kFrame4StepLengthNtsc = 29830;
constexpr FrameStep kFrame4StepPal[]  = { {8313,1,0}, {16627,1,1}, {24939,1,0}, {33253,1,1} };
constexpr uint32_t  kFrame4StepLengthPal  = 33254;
constexpr FrameStep kFrame5StepNtsc[] = { {7457,1,0}, {14913,1,1}, {22371,1,0}, {29829,0,0}, {37281,1,1} };
constexpr uint32_t  kFrame5StepLengthNtsc = 37282;
constexpr FrameStep kFrame5StepPal[]  = { {8313,1,0}, {16627,1,1}, {24939,1,0}, {33253,0,0}, {41565,1,1} };
constexpr uint32_t  kFrame5StepLengthPal  = 41566;
}
```

Verified 2026-09-28 against NESdev wiki "APU Frame Counter" (raw wikitext: mode 0 rows
3728 / 7456 / 11185 / 14914 / 14914 / 14915 NTSC and 4156 / 8313 / 12469 / 16626 / 16626 / 16627
PAL with three consecutive rows at the last step, mode 1 rows ... 14914 / 18640 / 18641 and
... 16626 / 20782 / 20783, "the interrupt flag is set every 29830 CPU cycles", "If the mode
flag is set, then both quarter frame and half frame signals are also generated", the 3-or-4
cycle write delay), the wiki Talk page ("the correct update tick is 7456.5", Ulfalizer 2013,
confirming the `.5` positions the text extraction drops), blargg `apu_ref.txt` (event
pattern `- - - f / - l - l / e e e e` and `- - - - - / l - l - - / e e e e -`, the latter
starting with the immediate clock of the $4017 write) and Brad Taylor (14915 divider). All
rates recomputed with `verify_nes.py` (identical to 6 decimals).

Verified 2026-09-28 (second pass) against the NESdev wiki "APU Frame Counter" raw wikitext.
The current wiki no longer writes `.5` fractions: it writes each position as an APU cycle
plus a half-cycle tag, e.g. `3728, PUT`, `14914, GET`, `14914, PUT`, `0 (14915), GET`. Every
step of both modes for NTSC and PAL is tagged `PUT` except the first and last rows of mode 0
step 4 and the wrap row of mode 1 (`GET`). Reading `n, GET` as CPU cycle `2n` and `n, PUT` as
`2n + 1` gives exactly the CPU-cycle columns and the `kFrame*` arrays above (all 24 positions
recomputed, identical). The 29830-cycle sentence was refetched ("slightly (0.166%) slower than
the 29780.5 CPU cycles per NTSC PPU frame").

Clock counts per second for the required tests (exact CPU clocks): NTSC 4-step gives
239.996 quarter and 119.998 half clocks per second; NTSC 5-step 192.025 / 96.013;
PAL 4-step 199.989 / 99.994; PAL 5-step 159.997 / 79.998. Over one second of emulated
time the integer counts are floor/ceil of those values depending on phase (e.g. 239 or
240 quarter clocks in NTSC mode 0).

---

## Mixer

Source: NESdev wiki "APU Mixer" (full text obtained; formula credited to blargg's
`apu_ref.txt`, which lists the same constants).

Exact formula (output in 0.0 .. 1.0; `pulse1`, `pulse2`, `triangle`, `noise` are the
channel outputs 0..15, `dmc` is the DMC output level 0..127):

```
output    = pulse_out + tnd_out
pulse_out = 95.88  / ((8128 / (pulse1 + pulse2)) + 100)
tnd_out   = 159.79 / ((1 / ((triangle / 8227) + (noise / 12241) + (dmc / 22638))) + 100)
```

Zero-division rule (quoted): "When the values for one of the groups are all zero, the
result for that group should be treated as zero rather than undefined due to the
division by 0 that otherwise results." I.e. `pulse_out = 0` when `pulse1 + pulse2 == 0`
and `tnd_out = 0` when `triangle == noise == dmc == 0`.

Output range: 0.0 (all silent) to 0.999999 (15, 15, 15, 15, 127) - see reference values.
The channels interact: with `dmc = 127` a full-scale triangle adds only 0.107057 instead
of 0.246412, i.e. 43.4 % of the standalone increment (noise: 42.7 %), which is the "crude
volume control" games exploit. Brad Taylor: "When $4011 = 7F, the triangle & noise channel
outputs operate at only 57% total volume" - read literally this is 57 % remaining, not
43 %; the formula instead gives a 57 % reduction. See A18 (added 2026-09-28, second pass;
the earlier wording presented the two figures as consistent).

Lookup-table approximation (wiki, "numerators are adjusted slightly to preserve the
normalized output range", tnd within 4 %):

```
pulse_table[n] = 95.52  / (8128.0 / n + 100)        n = 0..30, pulse_out = pulse_table[pulse1 + pulse2]
tnd_table[n]   = 163.67 / (24329.0 / n + 100)       n = 0..202, tnd_out = tnd_table[3*triangle + 2*noise + dmc]
```

Linear approximation (wiki, "results in slightly louder DMC samples"):

```
pulse_out = 0.00752 * (pulse1 + pulse2)
tnd_out   = 0.00851 * triangle + 0.00494 * noise + 0.00335 * dmc
```

Decision: the engine implements the exact formula. Suggested implementation: a
precomputed `pulseTable[31]` (index `pulse1 + pulse2`) and either a direct evaluation of
`tnd_out` on each level change or a precomputed exact table `tndTable[16][16][128]`
(32768 doubles, built in `prepare()`; the ultrasonic triangle value 7.5 of A2 is not an
integer index, so that case needs the direct evaluation). The wiki's LUT and linear variants are
approximations and are used only as comparison values in tests.

Reference values (`gen_nes.py`, exact formula; 6 decimals):

```
p1 p2  t  n   d   pulse_out  tnd_out   total      linear    lut
 0  0  0  0   0   0.000000   0.000000  0.000000   0.000000  0.000000
15  0  0  0   0   0.149377   0.000000  0.149377   0.112800  0.148816
15 15  0  0   0   0.258483   0.000000  0.258483   0.225600  0.257513
 0  0 15  0   0   0.000000   0.246412  0.246412   0.127650  0.255477
 0  0  0 15   0   0.000000   0.174431  0.174431   0.074100  0.179666
 0  0  0  0 127   0.000000   0.574264  0.574264   0.425450  0.561346
 0  0 15 15 127   0.000000   0.741516  0.741516   0.627200  0.742468
15 15 15 15 127   0.258483   0.741516  0.999999   0.852800  0.999980
 1  0  0  0   0   0.011653   0.000000  0.011653   0.007520  0.011609
 0  0  1  0   0   0.000000   0.019189  0.019189   0.008510  0.019936
 0  0  0  1   0   0.000000   0.012948  0.012948   0.004940  0.013345
 0  0  0  0   1   0.000000   0.007027  0.007027   0.003350  0.006700
 8  8  0  0   0   0.157697   0.000000  0.157697   0.120320  0.157105
 0  0  8  0   0   0.000000   0.141611  0.141611   0.068080  0.146959
 0  0  0  8   0   0.000000   0.098023  0.098023   0.039520  0.100996
 0  0  0  0  64   0.000000   0.352179  0.352179   0.214400  0.340879
15 15 15 15   0   0.258483   0.373329  0.631812   0.427350  0.643175
 0  0 15 15   0   0.000000   0.373329  0.373329   0.201750  0.385662
12 12 15 10   0   0.218571   0.333758  0.552329   0.357530  0.562833
15  0 15  0 127   0.149377   0.681321  0.830698   0.665900  0.826685
15 15  0  0 127   0.258483   0.574264  0.832747   0.651050  0.818859
 0  0 15  0 127   0.000000   0.681321  0.681321   0.553100  0.677869
 0  0  0 15 127   0.000000   0.648770  0.648770   0.499550  0.641939
 0  0 15  0  64   0.000000   0.507211  0.507211   0.342050  0.506402
```

Full pulse table from the exact formula (`pulse_out` for `pulse1 + pulse2 = 0..30`):

```
0.000000, 0.011653, 0.023026, 0.034129, 0.044972, 0.055563, 0.065912, 0.076026,
0.085914, 0.095583, 0.105039, 0.114291, 0.123345, 0.132206, 0.140882, 0.149377,
0.157697, 0.165849, 0.173836, 0.181663, 0.189336, 0.196860, 0.204237, 0.211473,
0.218571, 0.225536, 0.232371, 0.239080, 0.245666, 0.252133, 0.258483
```

Triangle alone (`tnd_out(t,0,0)`, t = 0..15):

```
0.000000, 0.019189, 0.037923, 0.056218, 0.074088, 0.091549, 0.108614, 0.125297,
0.141611, 0.157567, 0.173177, 0.188452, 0.203403, 0.218040, 0.232374, 0.246412
```

Noise alone (`tnd_out(0,n,0)`, n = 0..15):

```
0.000000, 0.012948, 0.025688, 0.038224, 0.050562, 0.062707, 0.074662, 0.086433,
0.098023, 0.109437, 0.120678, 0.131751, 0.142659, 0.153406, 0.163995, 0.174431
```

DMC alone (`tnd_out(0,0,d)`) for d = 0, 1, 2, 32, 64, 96, 125, 126, 127:
`0.000000, 0.007027, 0.013993, 0.197898, 0.352179, 0.475831, 0.568437, 0.571359, 0.574264`.

```cpp
namespace nes {
inline double mixPulse(int p1, int p2) {
    const int s = p1 + p2;
    return s == 0 ? 0.0 : 95.88 / (8128.0 / s + 100.0);
}
inline double mixTnd(int tri, int noise, int dmc) {
    const double x = tri / 8227.0 + noise / 12241.0 + dmc / 22638.0;
    return x == 0.0 ? 0.0 : 159.79 / (1.0 / x + 100.0);
}
}
```

Per-channel buses (`ENGINE_SPECS.md`): a channel alone is rendered through the same
formula with the other four inputs at 0 (e.g. triangle alone uses the "triangle alone"
column above).

Verified 2026-09-28 against NESdev wiki "APU Mixer" (all constants of the exact formula,
the LUT numerators/denominators and index formula `3 * triangle + 2 * noise + dmc`, the
linear coefficients, the zero-division sentence) and blargg `apu_ref.txt` (same constants);
Brad Taylor's "57 %" sentence refetched verbatim. The 24-row table, the 31-entry pulse
table, the triangle/noise/DMC-alone rows and the 0.107057 increment were recomputed with
`verify_nes.py` (every printed value identical). Extra value for A2: `tnd_out(7.5, 0, 0)
= 0.133499`.

Verified 2026-09-28 (second pass) against NESdev wiki "APU Mixer" raw wikitext and blargg
`apu_ref.txt` (constants 95.88 / 8128 / 100, 159.79 / 8227 / 12241 / 22638 / 100, LUT 95.52 /
8128 and 163.67 / 24329 with index `3 * triangle + 2 * noise + dmc`, linear 0.00752 / 0.00851 /
0.00494 / 0.00335 all refetched). All 24 x 5 table values, the 31-entry pulse table, the
triangle / noise / DMC-alone rows, 0.107057, 0.074507 and 0.133499 recomputed with an
independent script: identical to 6 decimals.

---

## Output stage

Source: NESdev wiki "APU Mixer" (filter list) and the NESdev forum thread p=44255
(blargg's measurements, lidnariq's component analysis).

NES-001 (front-loading NES) after the DACs, all first order:

* high-pass at **90 Hz** (forum: output-stage 150 ohm / 10 uF coupling, quoted time
  constant 1700 us, which corresponds to 93.6 Hz),
* high-pass at **440 Hz** (forum measured **442 Hz**, time constant 360 us),
* low-pass at **14 kHz** (forum: 47 kohm / 220 pF around the inverting amplifier).

An additional LC network in the output has a 243 kHz corner (forum) and is ignored.

Famicom (wiki): "The Famicom hardware instead ONLY specifies a first-order high-pass
filter at 37 Hz, followed by the unknown (and varying) properties of the RF modulator and
demodulator."

Engine decision (`ARCHITECTURE.md`): `console_filter` = NES-001 chain 90 Hz HP + 440 Hz HP
+ 14 kHz LP, applied after the resampler (the 90 Hz section is the coupling capacitor, so no
separate DC blocker runs with it; `console_filter = 0` uses a 5 Hz DC blocker, A25); the Famicom variant
(37 Hz HP only) is not exposed as a parameter (alternative: add a `console` choice).
First-order coefficients at host rates, `k = exp(-2*pi*fc/fs)` (`gen_nes.py`):

```
fs=44100: 90 Hz k=0.987259   440 Hz k=0.939235   14000 Hz k=0.136060   37 Hz k=0.994742
fs=48000: 90 Hz k=0.988288   440 Hz k=0.944031   14000 Hz k=0.159998   37 Hz k=0.995168
one-pole HP: y[n] = k * (y[n-1] + x[n] - x[n-1]);   one-pole LP: y[n] = (1-k) * x[n] + k * y[n-1]
```

Verified 2026-09-28 against NESdev wiki "APU Mixer" (90 Hz / 440 Hz / 14 kHz list, Famicom
37 Hz sentence) and the forum thread p=44255 (lidnariq: 1700 us "the very last output stage
-- the 150ohm resistor and 10uF cap"; 360 us "might correspond to the 47k ... and the
.011uF of capacitors"; 14 kHz "the 47kohm and 220pF lowpass around the
inverter-used-as-an-amplifier"; LC "654ns -> corner freq=243kHz"). Coefficients recomputed
with `verify_nes.py` (identical). Note: 1 / (2 pi 47 k 220 pF) is 15.4 kHz, not 14 kHz; the
wiki's 14 kHz figure is kept (see A10).

Polarity: the mixer formula gives a positive value for a positive DAC level; the NES
output stage is inverting (forum: "inverting amplifier") but absolute polarity is not
audible and is not modelled.

---

## Period reference table

Formula (NESdev wiki "APU Pulse" / "APU period table" script): pulse
`t = round(f_CPU / (16 * f)) - 1`, triangle `t = round(f_CPU / (32 * f)) - 1`, with
`f = 440 * 2^((midi - 69) / 12)`. The NESdev script is `period = int(round(octaveBase /
frequency)) - 1` with `ntscOctaveBase = 39375000.0 / (22 * 16 * 55.0)` and
`palOctaveBase = 266017125.0 / (10 * 16 * 16 * 55.0)`, `lowestFreq = 55 Hz`
(= MIDI 33), 80 notes, semitone ratio `2 ** (1/12)`: exactly the same formula.

Cross-check against the published NESdev NTSC table (NESdev "APU basics",
`periodTableLo/Hi`, "generated by mktables.py", 80 notes from 55 Hz = MIDI 33 to
MIDI 112). `gen_nes.py` decoded the 80 published bytes and compared them with the
formula at 1789773 Hz: **all 80 values are identical** (mismatch list empty). Published
values, 16-bit, first 24 (MIDI 33..56), for the test file:

```
MIDI 33 A1  $07F1 = 2033    MIDI 45 A2  $03F8 = 1016
MIDI 34 A#1 $077F = 1919    MIDI 46 A#2 $03BF =  959
MIDI 35 B1  $0713 = 1811    MIDI 47 B2  $0389 =  905
MIDI 36 C2  $06AD = 1709    MIDI 48 C3  $0356 =  854
MIDI 37 C#2 $064D = 1613    MIDI 49 C#3 $0326 =  806
MIDI 38 D2  $05F3 = 1523    MIDI 50 D3  $02F9 =  761
MIDI 39 D#2 $059D = 1437    MIDI 51 D#3 $02CE =  718
MIDI 40 E2  $054C = 1356    MIDI 52 E3  $02A6 =  678
MIDI 41 F2  $0500 = 1280    MIDI 53 F3  $0280 =  640
MIDI 42 F#2 $04B8 = 1208    MIDI 54 F#3 $025C =  604
MIDI 43 G2  $0474 = 1140    MIDI 55 G3  $023A =  570
MIDI 44 G#2 $0434 = 1076    MIDI 56 G#3 $021A =  538
```

and the rest of the published table (MIDI 57..112): 507, 479, 452, 427, 403, 380, 359,
338, 319, 301, 284, 268, 253, 239, 225, 213, 201, 189, 179, 169, 159, 150, 142, 134,
126, 119, 112, 106, 100, 94, 89, 84, 79, 75, 70, 66, 63, 59, 56, 52, 49, 47, 44, 41,
39, 37, 35, 33, 31, 29, 27, 26, 24, 23, 21, 20 (note the NESdev naming calls MIDI 33
"A-1"; the octave label differs from MIDI, the frequency is 55 Hz either way).

The PAL table below equals the NESdev script's PAL output for all 80 notes (checked in
`gen_nes.py`; no independent published PAL table was found, see Ambiguities A9).

Pulse periods, MIDI 24..108 (`round(cpu / (16 f)) - 1`). Values above $7FF (2047) are
not representable in 11 bits; the driver must clamp to $7FF or refuse the note:
MIDI 24..32 on NTSC (MIDI 32 = 2154 > 2047, MIDI 33 = 2033 fits) but only MIDI 24..31 on
PAL (MIDI 31 = 2120 > 2047, MIDI 32 = 2001 fits, because the PAL clock is 7 % slower and
$7FF reaches 50.74 Hz, below G#1 = 51.91 Hz). The triangle tables are always representable
for MIDI 24..108; below that range the triangle follows the same rule one octave lower
(triangle MIDI n = pulse MIDI n + 12): NTSC MIDI <= 20 and PAL MIDI <= 19 exceed $7FF.

Verified 2026-09-28: the four arrays were recomputed from the formula with `verify_nes.py`
(85 x 4 values, no mismatch); the NESdev script formula (`ntscOctaveBase`, `palOctaveBase`,
exact clocks) reproduces the same 80 values for both clocks; the 80 NTSC bytes of "APU
basics" (`periodTableLo/Hi`, refetched) decode to the NTSC array from MIDI 33 to 112.

Verified 2026-09-28 (second pass): the 80 + 80 bytes of `periodTableLo/Hi` were refetched from
both "APU basics" and "APU period table" and decoded again (80 values, identical to the formula
at 1789773 Hz); the "APU period table" script was re-run with its own `relFreqs` expression
(`(1 << (i // 12)) * semitone ** (i % 12)`, not `2 ** (i / 12)`) for both `ntscOctaveBase` and
`palOctaveBase`: identical to the NTSC and PAL arrays for all 80 notes. The four 85-entry arrays
were recomputed at both the integer and the exact clocks: no mismatch. Rounding margins for
tests (distance of `cpu / (16 f)` from a .5 tie, all others > 0.01): NTSC MIDI 71 pulse / MIDI
59 triangle 226.4924 (-> 225), PAL MIDI 87 pulse / MIDI 75 triangle 83.4972 (-> 82), NTSC MIDI
25 3228.5086 (unreachable). These are safe in double and float precision but must not be
computed with a lower-precision frequency approximation.

```cpp
namespace nes {
// Pulse timer values for MIDI notes 24..108, NTSC (1789773 Hz). Values > 0x7FF are unreachable.
constexpr int16_t kPulsePeriodNtsc[85] = {
     3419,  3228,  3046,  2875,  2714,  2561,  2418,  2282,  2154,  2033,  1919,  1811,  // MIDI 24..35
     1709,  1613,  1523,  1437,  1356,  1280,  1208,  1140,  1076,  1016,   959,   905,  // MIDI 36..47
      854,   806,   761,   718,   678,   640,   604,   570,   538,   507,   479,   452,  // MIDI 48..59
      427,   403,   380,   359,   338,   319,   301,   284,   268,   253,   239,   225,  // MIDI 60..71
      213,   201,   189,   179,   169,   159,   150,   142,   134,   126,   119,   112,  // MIDI 72..83
      106,   100,    94,    89,    84,    79,    75,    70,    66,    63,    59,    56,  // MIDI 84..95
       52,    49,    47,    44,    41,    39,    37,    35,    33,    31,    29,    27,  // MIDI 96..107
       26,                                                                              // MIDI 108
};
// Pulse timer values for MIDI notes 24..108, PAL (1662607 Hz). Values > 0x7FF are unreachable.
constexpr int16_t kPulsePeriodPal[85] = {
     3176,  2998,  2830,  2671,  2521,  2379,  2246,  2120,  2001,  1888,  1782,  1682,  // MIDI 24..35
     1588,  1499,  1414,  1335,  1260,  1189,  1122,  1059,  1000,   944,   891,   841,  // MIDI 36..47
      793,   749,   707,   667,   629,   594,   561,   529,   499,   471,   445,   420,  // MIDI 48..59
      396,   374,   353,   333,   314,   297,   280,   264,   249,   235,   222,   209,  // MIDI 60..71
      198,   186,   176,   166,   157,   148,   139,   132,   124,   117,   110,   104,  // MIDI 72..83
       98,    93,    87,    82,    78,    73,    69,    65,    62,    58,    55,    52,  // MIDI 84..95
       49,    46,    43,    41,    38,    36,    34,    32,    30,    29,    27,    25,  // MIDI 96..107
       24,                                                                              // MIDI 108
};
// Triangle timer values for MIDI notes 24..108 (round(cpu / (32 f)) - 1); all representable.
constexpr int16_t kTrianglePeriodNtsc[85] = {
     1709,  1613,  1523,  1437,  1356,  1280,  1208,  1140,  1076,  1016,   959,   905,  // MIDI 24..35
      854,   806,   761,   718,   678,   640,   604,   570,   538,   507,   479,   452,  // MIDI 36..47
      427,   403,   380,   359,   338,   319,   301,   284,   268,   253,   239,   225,  // MIDI 48..59
      213,   201,   189,   179,   169,   159,   150,   142,   134,   126,   119,   112,  // MIDI 60..71
      106,   100,    94,    89,    84,    79,    75,    70,    66,    63,    59,    56,  // MIDI 72..83
       52,    49,    47,    44,    41,    39,    37,    35,    33,    31,    29,    27,  // MIDI 84..95
       26,    24,    23,    21,    20,    19,    18,    17,    16,    15,    14,    13,  // MIDI 96..107
       12,                                                                              // MIDI 108
};
constexpr int16_t kTrianglePeriodPal[85] = {
     1588,  1499,  1414,  1335,  1260,  1189,  1122,  1059,  1000,   944,   891,   841,  // MIDI 24..35
      793,   749,   707,   667,   629,   594,   561,   529,   499,   471,   445,   420,  // MIDI 36..47
      396,   374,   353,   333,   314,   297,   280,   264,   249,   235,   222,   209,  // MIDI 48..59
      198,   186,   176,   166,   157,   148,   139,   132,   124,   117,   110,   104,  // MIDI 60..71
       98,    93,    87,    82,    78,    73,    69,    65,    62,    58,    55,    52,  // MIDI 72..83
       49,    46,    43,    41,    38,    36,    34,    32,    30,    29,    27,    25,  // MIDI 84..95
       24,    22,    21,    20,    19,    18,    17,    16,    15,    14,    13,    12,  // MIDI 96..107
       11,                                                                              // MIDI 108
};
}
```

Note: the triangle table for MIDI n equals the pulse table for MIDI n + 12 (the
triangle sounds one octave lower for the same `t`), so the engine may store only the
pulse table and index it with `note + 12` for the triangle, as NESdev "APU basics"
suggests ("read period values one octave later in the table").

Tuning error at high notes grows because `t` is small: MIDI 108 NTSC t = 26 gives
1789773 / (16 * 27) = 4142.99 Hz against 4186.01 Hz (-17.9 cents); this quantisation is
intentional (`ARCHITECTURE.md`).

Python that produced the four arrays (excerpt of `gen_nes.py`):

```python
def midi_freq(n): return 440.0 * 2.0 ** ((n - 69) / 12.0)
def pulse_period(cpu, f): return int(round(cpu / (16.0 * f))) - 1
def tri_period(cpu, f):   return int(round(cpu / (32.0 * f))) - 1
for cpu in (1789773, 1662607):
    print([pulse_period(cpu, midi_freq(n)) for n in range(24, 109)])
    print([tri_period(cpu, midi_freq(n)) for n in range(24, 109)])
```

---

## Reference values for unit tests

Each value gives its formula or source. All computed values come from `gen_nes.py`
(Python 3, double precision) and are printed with 6 decimals where fractional.

Clocks and frame rates:

1. NTSC CPU exact = 236250000/11/12 = 1789772.727273 Hz; engine constant 1789773 (NESdev "Cycle reference chart").
2. PAL CPU exact = 26601712.5/16 = 1662607.031250 Hz; engine constant 1662607 (same).
3. NTSC frame rate = 1789772.727273 / 29780.5 = 60.098814 Hz (wiki 60.0988).
4. PAL frame rate = 1662607.031250 / 33247.5 = 50.006979 Hz (wiki 50.0070).
5. NTSC 4-step frame sequence = 29830 CPU cycles -> 59.999086 Hz; quarter 239.996343 Hz; half 119.998171 Hz.
6. NTSC 5-step frame sequence = 37282 CPU cycles -> 48.006350 Hz; quarter 192.025399 Hz; half 96.012699 Hz.
7. PAL 4-step = 33254 CPU cycles -> 49.997204 Hz; quarter 199.988817 Hz; half 99.994409 Hz.
8. PAL 5-step = 41566 CPU cycles -> 39.999207 Hz; quarter 159.996827 Hz; half 79.998414 Hz.
9. NTSC 4-step event CPU cycles: 7457, 14913, 22371, 29829 (2 x APU 3728.5, 7456.5, 11185.5, 14914.5).
10. NTSC 5-step event CPU cycles: 7457, 14913, 22371, 29829 (no clock), 37281.
11. PAL 4-step event CPU cycles: 8313, 16627, 24939, 33253; PAL 5-step adds 41565.

Pulse / triangle pitch (`f = cpu / (16 (t+1))`, triangle `/ 32`):

12. NTSC t = $7FF -> 54.619537 Hz pulse, 27.309769 Hz triangle (lowest).
13. NTSC t = 8 -> 12428.979167 Hz pulse (highest unmuted); t = 7 -> 13982.601562 Hz would be muted.
14. NTSC t = 0 -> 111860.812500 Hz pulse (muted), 55930.406250 Hz triangle (ultrasonic).
15. PAL t = $7FF -> 50.738739 Hz; PAL t = 8 -> 11545.881944 Hz; PAL t = 0 triangle 51956.468750 Hz.
16. NTSC t = 253 ($0FD, MIDI 69 A4) -> 440.396900 Hz pulse (+1.56 cents); triangle t = 126 -> 440.396900 Hz.
17. NTSC t = 427 ($1AB, MIDI 60 C4) -> 261.357039 Hz pulse; PAL MIDI 60 t = 396 -> 261.745435 Hz (= 1662607 / (16 * 397)).
18. Period table cross-check: NTSC MIDI 33 = 2033 ($07F1), MIDI 45 = 1016 ($03F8), MIDI 57 = 507, MIDI 69 = 253, MIDI 81 = 126, MIDI 93 = 63, MIDI 105 = 31, MIDI 112 = 20 (NESdev "APU basics" bytes; all 80 match the formula).
19. PAL MIDI 33 = 1888, MIDI 45 = 944, MIDI 57 = 471, MIDI 69 = 235, MIDI 81 = 117, MIDI 93 = 58, MIDI 105 = 29 (NESdev PAL script formula).
20. Unreachable pulse notes (period > 2047): NTSC MIDI 24..32 (3419..2154; MIDI 33 = 2033 fits); PAL MIDI 24..31 (3176..2120; MIDI 32 = 2001 fits and is the lowest PAL pulse note, 1662607 / (16 * 2002) = 51.904564 Hz against the equal-tempered 51.913087 Hz; corrected 2026-09-28, the earlier text printed 51.912 Hz). Triangle: always representable (max 1709 NTSC / 1588 PAL at MIDI 24).

Sweep (`target = period + (negate ? (p1 ? -(period>>s)-1 : -(period>>s)) : period>>s)`):

21. period $100, shift 1, negate: pulse 1 -> $07F, pulse 2 -> $080; not negated -> $180.
22. period $400, shift 0, negate 0 -> target $800 -> muted even with E = 0 (wiki statement).
23. period $3FF, shift 0, negate 0 -> target $7FE -> not muted.
24. period $0AB, shift 2, negate: pulse 1 -> $080, pulse 2 -> $081.
25. period 7 -> muted regardless of target; period 8 -> not muted by the period rule.

Envelope / length:

26. Envelope V = 15, loop off, NTSC 4-step: 15 -> 0 in 1.000015 s (16 quarter frames per step, 15 steps); V = 0: 0.062501 s.
27. Length table: index 0 = 10, 1 = 254, 2 = 20, 3 = 2, 8 = 160, 24 = 192, 31 = 30 (wiki table); index 1 lasts 2.116699 s at 119.998 Hz.

Noise:

28. LFSR from 1, mode 0: period 32767 (visits every non-zero state); mode 1 from 1, $4000 or $7FFF: period 93; the 31-step cycle contains exactly 31 of the 32767 non-zero states; state 0 is a fixed point in both modes (the hardware power-up quirk "first clock shifts in a 1" avoids it).
29. First 16 states after clocking 1 in mode 0: $4000, $2000, $1000, $0800, $0400, $0200, $0100, $0080, $0040, $0020, $0010, $0008, $0004, $0002, $4001, $6000; bit-0 outputs 0 x 14, 1, 0. Mode 1 from 1: $4000, $2000, $1000, $0800, $0400, $0200, $0100, $0080, $0040, $4020, $2010, $1008, $0804, $0402, $0201, $4100.
29a. 31-step cycle start state: $0737 (mode 1, period 31; bit-0 outputs after each clock `1101100111000011010100100010111`, i.e. starting with the state after the first clock); $0737 is reached after 14739 mode-0 clocks from 1.
30. Noise NTSC idx 15: 4068 CPU cycles -> 439.963864 Hz LFSR clock; PAL idx 15: 3778 -> 440.075966 Hz; NTSC idx 0: 4 -> 447443.25 Hz; NTSC idx 8: 202 -> 8860.262376 Hz; PAL idx 8: 188 -> 8843.654255 Hz; PAL idx 2: 14 -> 118757.642857 Hz (PAL entry 2 is 14, not 16).
31. Noise table cross-check: Brad Taylor's `$7F2 * 2 = 4068`, `$065 * 2 = 202`, `$3F9 * 2 = 2034`; full tables: NTSC {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068}, PAL {4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778}.

DMC:

32. NTSC rate 0 = 428 cycles -> 4181.712617 Hz (wiki 4181.71); rate 15 = 54 -> 33143.944444 Hz (wiki 33143.9).
33. PAL rate 0 = 398 -> 4177.404523 Hz; rate 15 = 50 -> 33252.140000 Hz; NTSC rate 8 = 190 -> 9419.857895 Hz; PAL rate 8 = 176 -> 9446.630682 Hz; NTSC rate 13 = 84 -> 21306.821429 Hz (A6). Full tables: NTSC {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54}, PAL {398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50}.
34. Output counter: level 125 + bit 1 -> 127; 126 + bit 1 -> 126 (unchanged); 127 + bit 1 -> 127; 1 + bit 0 -> 1 (unchanged); 2 + bit 0 -> 0; 0 + bit 0 -> 0.
35. $4012 = $FF -> address $FFC0; $4013 = $FF -> 4081 bytes; $4012 = 0 -> $C000; $4013 = 0 -> 1 byte.

Mixer (exact formula; see the 24-row table for the full set):

36. (0,0,0,0,0) -> 0.000000 (zero-division rule).
37. (15,15,0,0,0) -> 0.258483; (15,0,0,0,0) -> 0.149377; (1,0,0,0,0) -> 0.011653.
38. (0,0,15,0,0) -> 0.246412; (0,0,0,15,0) -> 0.174431; (0,0,0,0,127) -> 0.574264.
39. (15,15,15,15,127) -> 0.999999 (maximum); (15,15,15,15,0) -> 0.631812.
40. (0,0,15,0,127) - (0,0,0,0,127) = 0.107057 (triangle contribution with DMC at 127, vs 0.246412 alone).
41. (12,12,15,10,0) -> 0.552329; (0,0,15,0,64) -> 0.507211; (0,0,8,0,0) -> 0.141611.
42. Linear approximation for (15,15,15,15,127) = 0.852800; LUT approximation = 0.999980.
42a. Triangle ultrasonic substitute (A2): `tnd_out(7.5, 0, 0)` = 0.133499 (between the step-7 value 0.125297 and the step-8 value 0.141611).
42b. Noise with DMC at 127: (0,0,0,15,127) - (0,0,0,0,127) = 0.074507 (vs 0.174431 alone, 42.7 %).

Output stage:

43. One-pole coefficients at 48 kHz: 90 Hz -> 0.988288, 440 Hz -> 0.944031, 14 kHz -> 0.159998 (`exp(-2 pi fc / fs)`); at 44.1 kHz: 0.987259, 0.939235, 0.136060.

Frame sequencer clock counts over one second (added 2026-09-28, second pass; required by
`ENGINE_SPECS.md` "envelope and length clocks per second"). Sequencer reset at CPU cycle 0
with bit 7 clear (no immediate clock), events counted in CPU cycles `0 .. f_CPU - 1` with the
integer clocks, using the `kFrame*` arrays:

44. NTSC 4-step, 1789773 cycles: 239 quarter-frame clocks (envelope / linear counter), 119
    half-frame clocks (length / sweep). (59 full sequences = 1759970 cycles, then the events at
    7457, 14913, 22371 of the 60th; 29829 falls after the end.)
45. NTSC 5-step, 1789773 cycles: 192 quarter, 96 half (48 full sequences = 1789536 cycles, the
    remaining 237 cycles contain no event).
46. PAL 4-step, 1662607 cycles: 199 quarter, 99 half (49 full sequences = 1629446, then 8313,
    16627, 24939).
47. PAL 5-step, 1662607 cycles: 159 quarter, 79 half (39 full sequences = 1621074, then 8313,
    16627, 24939).
48. If the 5-step mode is entered by a $4017 write with bit 7 set at cycle 0, the immediate
    quarter + half clock adds one of each: NTSC 193 / 97, PAL 160 / 80 (wiki / blargg
    behaviour, see A17).

Engine-level fundamentals (added 2026-09-28, second pass; `ENGINE_SPECS.md` "rendering a note
produces the expected fundamental"):

49. MIDI 69, pulse NTSC t = 253 -> 440.396900 Hz; pulse PAL t = 235 -> 1662607 / (16 * 236) =
    440.309057 Hz (+1.22 cents); triangle NTSC t = 126 -> 1789773 / (32 * 127) = 440.396900 Hz;
    triangle PAL t = 117 -> 1662607 / (32 * 118) = 440.309057 Hz.
50. MIDI 108 NTSC pulse t = 26 -> 4142.993056 Hz (-17.88 cents vs 4186.009045 Hz).
51. Noise `keyed` mapping (`ENGINE_SPECS.md`): note 36 -> period index 15 (NTSC 4068 cycles,
    LFSR clock 439.963864 Hz); note 51 -> index 0 (4 cycles, 447443.25 Hz); notes below 36 clamp
    to 15, above 51 clamp to 0.
52. Rounding-margin notes for the period tables: NTSC MIDI 71 pulse = 225 (ratio 226.4924),
    PAL MIDI 87 pulse = 82 (ratio 83.4972); see "Period reference table".

All 43 items (and 29a, 42a, 42b) were recomputed on 2026-09-28 with `verify_nes.py`; no value
of the original list needed correction except item 20 (PAL range). Second verification pass
(2026-09-28, independent script `v2.py` / `v3.py`): all 43 items recomputed again; item 20's
PAL frequency corrected (51.912 -> 51.904564 Hz); items 44-52 added.

---

## Ambiguities

A1. **Exact vs rounded CPU clock.** Sources: NESdev "Cycle reference chart" gives
1789772.727... / 1662607.03125 Hz; the wiki period tables and `ENGINE_SPECS.md` use
1789773 / 1662607. Decision: use the integers; the NESdev 80-note NTSC table is
reproduced bit-exactly by 1789773 (verified). Alternative: carry the exact fractions
(no audible difference, 0.15 ppm).

A2. **Triangle at periods 0 and 1 (ultrasonic).** NESdev "APU Triangle" is silent on
it (only "frequencies up to fCPU/32 (about 55.9 kHz for NTSC) are possible"). blargg
`apu_ref.txt`: the DAC "effectively outputs a value half way between 7 and 8". NESdev
"APU basics": "Writing a raw period of 0 also silences the wave, but produces a pop".
Decision: when `t < 2` the chip core outputs the constant 7.5 (into the mixer as a
fractional triangle value, i.e. `tnd_out` evaluated with `triangle = 7.5`); this
reproduces the documented average and the documented pop (DC jump from the current step
to 7.5) without generating 1.79 M level changes per second in `BandLimitedStepSynth`.
Alternative: run the sequencer normally at all periods and let the resampler average it
(strictly more faithful, roughly 30x more expensive for that channel while ultrasonic).
Note that on real hardware the averaging happens after the non-linear DAC, so the exact
DC level differs slightly from `tnd_out(7.5, ...)`; not measurable from the sources.

A3. **Triangle output while halted.** (Resolved 2026-09-28.) Wiki: the waveform is
"either cycling or suspended" and the channel "stops the sequence" when a counter is zero.
blargg `apu_ref.txt` states it explicitly: "the triangle channel is silenced by stopping
its waveform at whatever phase it's at, rather than causing zero to be sent to its DAC."
Decision: hold the current sequence value. No remaining alternative.

A4. **Noise LFSR power-up value.** NESdev "APU Noise": "On power-up, the shift register
is loaded with the value 1." NESdev "CPU power up state": "$0000 (all 0s, first clock
shifts in a 1)". Decision: initialise to 1. Both descriptions produce the same state
($4000) after the first clock and therefore the same sequence.

A5. **Noise period table: factor 2 between sources.** Wiki and blargg: CPU cycles
(4..4068). Brad Taylor ("2A03 technical reference" and NESSOUND.txt): `$002..$7F2`, exactly
half of every value, in a column headed "CPU clock cycles (11-bit wavelength+1)", with the
timers stated to run "at the 2A03's internal 6502 speed (1.79 MHz)". Corrected 2026-09-28
(second pass): an earlier version of this entry said Brad Taylor's unit was the APU clock
and that there was no disagreement; his document says CPU cycles, so taken literally his
noise is one octave higher than the wiki's. Decision: the wiki / blargg values in CPU cycles
(two later sources, and the wiki's statement that all periods are even "because there are 2
CPU cycles in an APU cycle" matches the APU-clocked timer); the timer reload value is
`period / 2 - 1` in APU cycles. Alternative: Brad Taylor's values as CPU cycles (noise one
octave higher); not used.

A6. **DMC rate index 13 (NTSC).** Wiki: 84 CPU cycles (21306.8 Hz); blargg: $054 = 84;
Brad Taylor: $2A8 = 680 clocks per byte = 85 per bit (refetched 2026-09-28: the document
really prints `D 2A8`). Decision: 84 (two sources, and 84 is even as every APU-clocked
period must be). Alternative: 85 (would be odd, so it is very likely a typo in the older
document).

A7. **Length counter units.** Wiki: half-frame clocks (10, 254, 20, ...); Brad Taylor:
60 Hz frames (05, 7F, 0A, ...), exactly half. Decision: wiki values decremented on every
half-frame clock. No real disagreement.

A8. **Pulse duty sequence encoding / phase after restart.** The wiki table lists both
a bit string (e.g. `0 0 0 0 0 0 0 1` for 12.5 %) and a time-ordered waveform (`0 1 0 0 0
0 0 0`); the two WebFetch extractions of the page agree that the time order is
`0 1 0 0 0 0 0 0` and that a $4003 write restarts "at the first value", i.e. the first
output after a restart is 0 for duties 0..2 and 1 for duty 3. Decision: use
`kPulseDuty` in time order starting at index 0 after a restart. Alternative: start at the
`1` step (would only shift the phase by one step; not audible except through the
restart pop, which is also documented: "writing to $4003 and $4007 resets the phase,
which causes a slight pop").

A9. **PAL period table.** No independently published PAL note table was found (the
FamiTracker thread t=13626 contains none; the wiki page only ships the generator
script; the "APU basics" page ships NTSC bytes only). Decision: use the values produced by
the NESdev script formula with 1662607 Hz (identical to the engine formula for all 80
notes; re-verified 2026-09-28 with the script's exact `palOctaveBase`). Alternative: none
needed; the formula is the documented one.

A10. **Output filter corner frequencies.** Wiki: 90 Hz, 440 Hz, 14 kHz. Forum thread
p=44255: 442 Hz measured, 90 Hz derived from a 1700 us time constant (93.6 Hz), and the
LC network at 243 kHz. Decision: 90 / 440 / 14000 Hz as the wiki states (and as
`ARCHITECTURE.md` already fixes). Alternative: 93.6 / 442 Hz (inaudible difference).
The thread's "filter coefficient" numbers (0.999835, 0.996039, 0.815686 at 44.1 kHz)
are quoted with an unspecified filter structure and were not used. Added 2026-09-28: the
14 kHz low-pass is attributed in the thread to "47kohm and 220pF", whose RC corner is
15.4 kHz; the thread and the wiki nevertheless both state 14 kHz (presumably measured, or
including the following stage), so 14 kHz is kept. Alternative: 15.4 kHz.

A11. **Famicom output stage.** Wiki: only a 37 Hz first-order high-pass, then an RF
modulator of unknown response. Decision: not modelled; `console_filter` is the NES-001
chain. Alternative: a `console` parameter with a 37 Hz-only option.

A12. **$4017 write delay (3 or 4 CPU cycles) and reset jitter.** Documented on the
wiki; it only matters for cycle-exact IRQ timing. Decision: apply the sequencer reset
(and the immediate quarter+half clock when bit 7 is set) 3 CPU cycles after the write,
constant. Alternative: model the odd/even distinction; not audible.

A13. **DMC $4011 write colliding with a timer clock** ("the output level is
occasionally not changed properly"). Decision: not modelled (write always applies).

A14. **Sweep update when the divider reloads.** The wiki's ordering (period update
first, then divider reload/decrement) is unambiguous; but whether the reload flag set
by a $4001 write also resets the divider *count* before the next half frame is covered
by step 2 ("If the divider's counter is zero OR the reload flag is true: set to P").
Decision: implement exactly the two ordered steps; the first period update after a
$4001 write therefore happens `P + 1` half frames after the write only if the counter was
non-zero at that clock, or immediately at that clock if the counter happened to be 0.
No alternative (this is the documented behaviour).

A15. **Frame counter at power-up.** $4017 = $00 means 4-step mode with IRQ enabled
and the sequencer starting at power-up with unknown phase relative to the first driver
tick. Decision: the driver writes $4017 = $40 at `prepare()`/`reset()` and the engine
starts the sequencer from 0 at that write. Alternative: $C0 (5-step, no IRQ) as many
first-party titles used; exposed only if a "frame counter mode" parameter is added later.

A16. **Lowest PAL pulse note.** Added 2026-09-28. With $7FF = 50.74 Hz on PAL, MIDI 32
(G#1, 51.91 Hz) is representable (t = 2001) while NTSC stops at MIDI 33 (A1, t = 2033).
Decision: the driver's clamp/refuse rule uses the computed period (> 2047) rather than a
fixed lowest MIDI note, so the PAL engine gains one extra low note. Alternative: refuse
MIDI < 33 on both clocks for identical ranges (would discard a note the 2A07 can play).

A17. **5-step mode: which steps clock the half-frame units.** Added 2026-09-28 (second
pass). NESdev wiki "APU Frame Counter": in mode 1 the half-frame clocks are at steps 2 and 5
(14913 and 37281 CPU cycles) plus the immediate clock of the $4017 write ("both quarter frame
and half frame signals are also generated"); blargg `apu_ref.txt` pattern `l - l - -` /
`e e e e -` (starting at the write) agrees. Brad Taylor / NESSOUND: "Count sequences 1 & 3
clock (update) the frequency sweep (square), and length (all channels) counters" and only the
linear and envelope counters "are clocked once immediately after $4017.7 is written with a
value of 1", i.e. the pattern after the write would be quarter / quarter+half / quarter /
quarter+half / none instead of quarter+half / quarter / quarter+half / quarter / none.
Decision: wiki + blargg (two later sources, test-ROM based). Alternative: Brad Taylor's
ordering (same 96 / 192 Hz rates, half-frame clocks shifted by one step).

A18. **DMC level vs triangle/noise volume.** Added 2026-09-28 (second pass). Brad Taylor:
"When $4011 = 7F, the triangle & noise channel outputs operate at only 57% total volume".
The wiki / blargg exact mixer formula gives, with `dmc = 127`, a full-scale triangle step of
0.107057 instead of 0.246412 (43.4 % remaining) and a full-scale noise step of 0.074507
instead of 0.174431 (42.7 % remaining), i.e. a 57 % reduction. Either Brad Taylor meant
"reduced by 57 %" (then the sources agree) or the measurements differ (57 % vs 43 %
remaining). Decision: the exact formula (wiki, blargg). Alternative: none implemented; the
figure is only a qualitative cross-check.

A19. **PAL DMC rate index 4 = 276.** Added 2026-09-28 (second pass). The PAL/NTSC period
ratio is 0.917..0.932 for the fifteen other DMC indices but 276 / 286 = 0.965 for index 4
(scaling by ~0.929 would give ~266); likewise the PAL noise entries 0-2 (4, 8, 14) are not
scaled like the rest (ratio 1.0, 1.0, 0.875 vs ~0.93). The NESdev wiki is the only fetched
source for both PAL tables (blargg and Brad Taylor give NTSC only). The wiki gives
the tables as per-index values, not as a formula, so an irregular entry is not by itself
evidence of a typo. Decision:
keep the wiki values 276 and 4, 8, 14. Alternative: none sourced.

A20. **Triangle sequencer phase at power-up.** Added 2026-09-28 (implementation). NESdev "CPU
power up state" lists the APU registers and the noise LFSR but not the triangle's 32-step
sequencer position, and no fetched source gives it. Because a halted triangle holds its step
(A3), the power-up phase sets a constant triangle input to the mixer, which changes the loudness
of noise and DMC through the non-linear `tnd_out` (e.g. noise 15 alone: 0.174431 with the
triangle held at 0, 0.373329 - 0.246412 = 0.126917 with it held at 15). Decision: step 0 (value
15), i.e. every counter cleared at power-up like the registers. Alternative: step 16 (value 0),
which would give the "channel alone" loudness until the triangle first plays. After the first
triangle note the held value is whatever phase the note stopped at, as on hardware.

A21. **DMC encoder start level.** Added 2026-09-28 (implementation). The 1-bit delta format
only encodes changes, so a converted sample is correct only if playback starts from the level
the encoder assumed. The sources document the hardware counter (+/-2, skip at the bounds, $4011
direct load) but no convention for the starting level of converted samples. Decision: the
encoder assumes 64 (mid-scale; PCM -1..1 maps to 64 + 63 x) and `dmc_direct_level` defaults to
64, so a default note plays exactly the encoded waveform. Alternative: start at 0 (the power-up
level): no $4011 write needed, but every sample then begins with a ramp of up to 32 bits to
reach the centre. Other `dmc_direct_level` values offset the whole sample and clip it at the
bounds, which is the hardware behaviour and is left audible.

A22. **Sweep register when the sweep is off.** Added 2026-09-28 (implementation). The sweep
mutes the channel whenever the target period exceeds $7FF, "regardless of whether the sweep unit
is disabled"; with `$4001 = $00` (negate 0, shift 0) the target is 2 x period, so every period
>= $400 (NTSC below about MIDI 45) is silent. NESdev "APU basics" writes `$4001 = $08` (negate
set) for this reason. Decision: with `sweep_enable = 0` the driver writes `$08` and ignores the
sweep period/negate/shift parameters; with `sweep_enable = 1` it writes the parameters as given
(the mute rules then apply, as on hardware). Alternative: always write the parameters verbatim
(low notes silently muted when the sweep is off).

A23. **DMC sample fetch latency.** Added 2026-09-28 (implementation). NESdev "APU DMC" says the
reader fills the sample buffer whenever it is empty; on hardware the fetch is a DMA that takes
1-4 CPU cycles. Decision: the fetch is immediate (same CPU cycle). Alternative: a 4-cycle delay;
it cannot change the output because the byte is only used at the next output cycle (>= 50 CPU
cycles later), except for the exact moment the IRQ flag is set.

A24. **Timer rewrites after a hardware-envelope note-off.** Added 2026-09-28 (fidelity review).
NESdev "APU Pulse" / "APU Envelope": every $4003 / $4007 write sets the envelope start flag
(decay back to 15) and reloads the length counter. A driver that keeps rewriting the period
after note-off (vibrato, pitch envelope, glide) and crosses a 256 boundary therefore restarts
the decay, and a released note with vibrato never ends. The sources describe the register
side effects only, not what a driver should do. Decision: after note-off with
`*_env_enable = 1` the driver writes no more $4003 / $4007; vibrato, pitch envelope and pitch
changes are clamped to the page of the last high bits written (low byte only), so the decay
runs out as documented in "Note off". Alternative: stop all period rewrites at note-off
(vibrato frozen during the release), or keep the restarts (a real driver bug some games have,
but it makes `isChannelActive` never become false).

A25. **DC blocker corner.** Added 2026-09-28 (fidelity review). `ARCHITECTURE.md` requires a
DC-blocking one-pole high-pass after the resampler but gives no corner; research "Output
stage" identifies the NES-001 output coupling (150 ohm / 10 uF) with the 90 Hz high-pass
itself, and no source gives a separate coupling corner. Decision: with `console_filter = 1` the
chain is exactly 90 Hz HP + 440 Hz HP + 14 kHz LP (the 90 Hz section is the coupling
capacitor, no extra section); with `console_filter = 0` a 5 Hz one-pole DC blocker
(`kDcBlockHz`, same value as the SNES and Genesis engines, not a hardware value) centres the
unipolar DAC output. Alternative: no high-pass at all with `console_filter = 0` (the output then
carries the unipolar mixer's DC level, e.g. a halted triangle's held step).

A26. **$4017 IRQ inhibit timing.** Added 2026-09-28 (fidelity review). NESdev "APU Frame
Counter" (raw wikitext refetched 2026-09-28): "Interrupt inhibit flag. If set, the frame
interrupt flag is cleared, otherwise it is unaffected." and, separately, "After 3 or 4 CPU clock
cycles*, the timer is reset." The page does not say whether the inhibit bit also waits for the
delay. Decision: the inhibit bit (and the flag clear it causes) applies at the write; only the
timer reset, the mode change and the immediate quarter/half clock of bit 7 are delayed 3 CPU
cycles (A12). Alternative: delay the inhibit too (a frame IRQ could then still be set during the
3 cycles). Not audible: the driver runs with IRQ inhibited.

A27. **Envelope decay counter at power-up.** Added 2026-09-30 (differential check,
`docs/research/refcheck-report.md`, NES section). NESdev "CPU power up state" lists the APU
registers, the frame counter and the noise LFSR but not the envelope's decay level or divider;
"APU Envelope" only says that the start flag is processed at the next quarter frame. The
decay level therefore decides what a decay-envelope note written right after power-up plays
until that quarter frame. Decision (implemented): 0, like every other counter at power-up
(consistent with A20), so such a note is silent for up to one quarter frame (4.2 ms NTSC).
Measured references: the NSFPlay core of VGMPlay 0.40.9 behaves as if the decay level started
at 15 and counted down from power-up (level 5 about 50 ms after power-up, then the documented
restart at the quarter frame; a restart after a finished decay waits for the quarter frame
like ours); the MAME core restarts the envelope at the $4003 write itself, which contradicts
the documented quarter-frame start. Alternative: 15 at power-up (the NSFPlay behaviour). Only
the first decay-envelope note of each pulse / noise channel after a power-up or engine reset
differs (at most one quarter frame of silence instead of a partly decayed level). No change
without a hardware source.

Differential check note on A20 (2026-09-30): our DMC triangle-pattern harmonics match the exact
mixer with the triangle held at 15 (H2 -31.5 dB, H4 -43.3 dB re H1; formula -31.6 / -43.3),
the NSFPlay core's match it with the triangle at 0 (-30.4 / -42.1; formula -30.4 / -42.1).
That core has no triangle / noise / DMC cross-compression at all, so this is not independent
evidence for A20's alternative; decision unchanged.

---

## Sources

All consulted on 2026-09-28. No emulator source code was opened.

1. NESdev wiki, "APU" - https://www.nesdev.org/wiki/APU - register map, $4015 status
   semantics, $4017 summary, glossary (divider, sequencer, timer, APU cycle = 2 CPU
   cycles), power-up note.
2. NESdev wiki, "APU registers" - https://www.nesdev.org/wiki/APU_registers - bit field
   mnemonics for $4000-$4017, unused bits, read-back notes.
3. NESdev wiki, "APU Pulse" - https://www.nesdev.org/wiki/APU_Pulse - duty sequences,
   timer, frequency formula, muting at t < 8, $4003 side effects.
4. NESdev wiki, "APU Sweep" - https://www.nesdev.org/wiki/APU_Sweep - EPPP.NSSS, target
   period computation, ones'/two's complement, muting rules, half-frame procedure.
5. NESdev wiki, "APU Envelope" - https://www.nesdev.org/wiki/APU_Envelope - full text:
   start flag, divider, decay level, loop, constant volume.
6. NESdev wiki, "APU Length Counter" - https://www.nesdev.org/wiki/APU_Length_Counter -
   32-entry table, load/clock/disable rules, halt flag bits.
7. NESdev wiki, "APU Triangle" - https://www.nesdev.org/wiki/APU_Triangle - 32-step
   sequence, CPU-rate timer, linear counter procedure, $400B reload flag, fCPU/32 note.
8. NESdev wiki, "APU Noise" - https://www.nesdev.org/wiki/APU_Noise - LFSR procedure,
   power-up value 1, sequence lengths, NTSC and PAL period tables in CPU cycles.
9. NESdev wiki, "APU DMC" - https://www.nesdev.org/wiki/APU_DMC - NTSC and PAL rate
   tables, register formulas, output unit and memory reader procedures, $4011 note.
10. NESdev wiki, "APU Frame Counter" - https://www.nesdev.org/wiki/APU_Frame_Counter -
    4-step and 5-step tables in APU cycles for NTSC and PAL, $4017 write behaviour,
    29830-cycle statement; raw wikitext refetched 2026-09-28 for the interrupt inhibit
    sentence (A26).
11. NESdev wiki, "APU Mixer" - https://www.nesdev.org/wiki/APU_Mixer - full text: exact
    formula, zero-division rule, lookup-table and linear approximations, NES-001 filter
    list, Famicom 37 Hz note.
12. NESdev wiki, "APU period table" - https://www.nesdev.org/wiki/APU_period_table -
    generator script constants (`ntscOctaveBase`, `palOctaveBase`, lowestFreq 55 Hz,
    80 notes, rounding), first NTSC values.
13. NESdev wiki, "APU basics" - https://www.nesdev.org/wiki/APU_basics - full text: init
    sequence, `periodTableLo/Hi` NTSC bytes (80 notes, used for the cross-check),
    "raw period = 111860.8/frequency - 1", triangle octave note, period-0 pop remark.
14. NESdev wiki, "APU Misc" - https://www.nesdev.org/wiki/APU_Misc - glossary
    definitions (divider period P+1, timers clocked every second CPU cycle except triangle).
15. NESdev wiki, "CPU" - https://www.nesdev.org/wiki/CPU - 2A03/2A07 clock table
    (1.789773 / 1.662607 / 1.773448 MHz), master clocks.
16. NESdev wiki, "Clock rate" (redirect) / "Cycle reference chart" -
    https://www.nesdev.org/wiki/Cycle_reference_chart - exact master and CPU clocks,
    cycles per frame (29780.5 / 33247.5), frame rates 60.0988 / 50.0070 Hz.
17. NESdev wiki, "CPU power up state" - https://www.nesdev.org/wiki/CPU_power_up_state -
    APU power-up/reset rows ($4017, $4015, $4000-$400F, noise LFSR, $4011 &= 1).
18. blargg, "apu_ref.txt" (NES APU reference) - https://www.nesdev.org/apu_ref.txt -
    cross-check of frame sequencer event pattern, noise and DMC tables (hex), length
    table (hex), mixer constants, triangle "half way between 7 and 8" statement, sweep
    complement difference.
19. Brad Taylor, "2A03 technical reference" -
    https://www.nesdev.org/2A03%20technical%20reference.txt - cross-check of the frame
    sequencer (14915 divider, 240 Hz), noise table (headed "CPU clock cycles", half the
    wiki values, see A5), DMC per-byte
    table, length table in frames, sweep NOT/NEG difference, envelope 240/(N+1), DMC
    influence on triangle/noise ("57% total volume").
20. Brad Taylor, "2A03 sound channel hardware documentation" (NESSOUND.txt) -
    https://www.nesdev.org/NESSOUND.txt - same cross-checks (noise table, sweep, length
    table, 5-step immediate clock, 96/192 Hz).
21. NESdev forum, thread "NES audio filters" post p=44255 (blargg / lidnariq) -
    https://forums.nesdev.org/viewtopic.php?p=44255 - RC components and measured
    corners (90 Hz / 442 Hz / 14 kHz, 243 kHz LC).
22. NESdev forum, thread t=13626 "FamiTracker's note table: 96 notes? But only 87 are
    playable" - https://forums.nesdev.org/viewtopic.php?t=13626 - playable range
    statements only (no period values; recorded for A9).

Generator script: `gen_nes.py` (scratchpad, quoted in the relevant sections); its
output was pasted verbatim into this file.

Added 2026-09-30 for the differential check (`tools/refcheck`, reference-emulators.md):

23. VGM specification v1.71 (vgmrips) - https://vgmrips.net/wiki/VGM_Specification (fetch
    refused on 2026-09-30; same text read from
    https://raw.githubusercontent.com/vgmrips/vgmplay-legacy/master/VGMPlay/vgmspec171.txt) -
    header offset 0x84 (NES APU clock, bit 31 FDS), command 0xB4 register mapping, data block
    0x67 type 0xC2 (NES APU RAM write, 16-bit start address). Format only, no program source.
24. NESdev wiki, "NSF" - https://www.nesdev.org/wiki/NSF - consulted 2026-09-30: header
    fields, play rate unit, the player's APU initialisation before INIT, INIT / PLAY call
    conventions (for the NSF stimuli played by Game Music Emu).

Adversarial verification pass, 2026-09-28: every source above was refetched (rendered page
and, for the wiki, the raw wikitext), plus NESdev wiki "Talk:APU Frame Counter" (the
7456.5 statement) and blargg's original copy of `apu_ref.txt` at
http://www.slack.net/~ant/nes-emu/apu_ref.txt (triangle halting sentence). Every numeric
table and reference value was recomputed independently with `verify_nes.py` (scratchpad).
Findings: one wrong statement (PAL unreachable range, fixed in "Period reference table",
item 20 and A16), one misquotation (Brad Taylor's 14915 sentence, fixed in "Frame counter"),
one imprecise wording (noise mode-1 cycle classes, reworded); A3 resolved; A6 confirmed as a
real source discrepancy; A10 extended. No table value needed correction.

Second adversarial verification pass, 2026-09-28: refetched the raw wikitext of "APU Noise",
"APU DMC", "APU Frame Counter", "APU Length Counter", "APU Mixer", "APU Pulse", "APU basics"
and "APU period table", plus blargg `apu_ref.txt`, Brad Taylor's "2A03 technical reference"
and NESSOUND.txt. Every `constexpr` array was parsed out of this file and diffed against
values recomputed from the formulas (independent scripts `v2.py` / `v3.py`, scratchpad); the
80 published NTSC period bytes were decoded again. Findings: no table value was wrong. Fixed:
item 20's PAL lowest-note frequency (51.912 -> 51.904564 Hz); the claim that Brad Taylor's
noise table is in APU clocks (his heading says CPU clock cycles; A5 rewritten); the claim that
Brad Taylor agrees with the wiki on the 5-step immediate clock (he does not for the half-frame
units; A17); the presentation of the "57 %" figure as consistent with the formula (A18); the
bit-0 string convention of the 31-step cycle (clarified). Added: A19 (irregular PAL DMC /
noise entries), GET/PUT reading of the current frame-counter wikitext, rounding-margin notes,
and reference items 44-52 (frame clock counts per second, engine fundamentals, noise keyed
mapping).

---

## Implementation decisions

Written 2026-09-28 with the first implementation (`dsp/include/chipdsp/nes/`, `dsp/src/nes/`,
tests `dsp/tests/test_nes_*.cpp`). New open points are listed above as A20-A26 (A24-A26 from
the fidelity review of 2026-09-28).

### Layers: chip versus driver

* **Chip behaviour** (`NesApu.h`: `PulseChannel`, `TriangleChannel`, `NoiseChannel`,
  `DmcChannel`, `FrameSequencer`, `NesMixer`): exactly the procedures of the sections above,
  driven only by register writes ($4000-$4017) and `NesApu::clock()`, which advances one CPU
  cycle. Pulse, noise and DMC timers run on every second CPU cycle with reload values
  `t` (pulse) and `period / 2 - 1` (noise, DMC tables in CPU cycles, A5); the triangle timer runs
  every CPU cycle; the frame sequencer counts CPU cycles against the `kFrame*` tables. Includes
  the sweep mute rules, the ones'/two's complement difference, length/linear counters, the
  envelope start flag, the DMC clamp rule and silence flag, the $4015 semantics (DMC restart only
  when bytes remaining is 0) and the 3-cycle $4017 delay (A12). The triangle keeps stepping at
  t < 2 but reports "ultrasonic" to the mixer, which then evaluates `tnd_out(7.5, noise, dmc)`
  directly (A2); halted, it holds its step (A3).
* **Mixer**: exact formula through tables built in `prepare()` (`NesMixer::build`): 31 pulse
  entries and 16 x 16 x 128 = 32768 tnd entries (float). Per-channel buses use the same tables
  with the other inputs at 0.
* **Output stage** (not chip behaviour, `NesOutputStage`): `console_filter = 1` is exactly the
  NES-001 chain 90 Hz HP (the coupling capacitor), 440 Hz HP, 14 kHz LP with
  `k = exp(-2 pi fc / fs)` as in "Output stage"; `console_filter = 0` is a 5 Hz one-pole DC
  blocker only (A25). Revised 2026-09-28: the DC blocker used to run in front of the console
  chain as a fourth section. The engine uses its own
  one-pole sections instead of `util/Filters.h` because that file computes `rc / (rc + dt)`
  rather than the documented `exp` coefficient (0.98835 vs 0.988288 at 90 Hz / 48 kHz).
* **Software driver** (`NesDriver.h`), everything a game's sound code would do: MIDI note to
  period conversion, per-frame period rewrites (vibrato, pitch envelope, glide), software ADSR
  written as constant volume, triangle gating/attack delay, keyed noise/DMC mapping, DMC sample
  triggering. It only ever acts through `NesApu::write()`.

### Driver tick model

* One driver tick per video frame: 29780.5 CPU cycles NTSC (60.0988 Hz), 33247.5 PAL
  (50.0070 Hz), counted in half CPU cycles (59561 / 66495) inside the CPU-cycle loop, so the
  tick lands on an exact CPU cycle and drifts against the APU frame counter (29830 / 33254
  cycles) as on hardware.
* The APU frame counter is started with $4017 = $40 (4-step, IRQ inhibited) at `reset()`,
  after the NESdev "APU basics" initialisation writes (A15).
* Note events (`noteOn`, `noteOff`) are applied when they are called, i.e. between blocks, which
  is the start of the next `renderBlock()`: the note-on register writes happen immediately and
  frame 0 of the software envelopes starts there; the following ticks stay on the frame grid.
  Alternative: defer note-on to the next tick (authentic up-to-16.7 ms jitter); not chosen for
  playability. `setChannelPitch` is picked up at the next tick, like a pitch slide.
* Parameter changes update the driver settings at once; the driver writes them at the next tick.
  `$4001` is written only at note-on or when its value changes (every write sets the sweep reload
  flag); `$4003` / `$4007` / `$400B` only at note-on or when the timer high bits change, because
  these writes restart the pulse phase and envelope (the audible "vibrato pop" of real drivers
  is therefore limited to period crossings of a 256 boundary, as in games).

### MIDI note to register conversion

* Pulse: `t = round(cpu / (16 f)) - 1`; triangle: `t = round(cpu / (32 f)) - 1`, with
  `f = 440 * 2^((note + transpose - 69) / 12)` and cpu = 1789773 / 1662607 Hz ("Period reference
  table"; tests check all 85 x 4 values). The float MIDI note (glide, bend) goes through the same
  formula, so pitch moves in 11-bit steps.
* Vibrato and pitch envelope add period units to `t` before the write; the result is clamped to
  0..$7FF (the register width). If the base period of a note is above $7FF the driver
  refuses the note instead of clamping: pulses are written with volume 0, the triangle stays
  halted ($4015 bit cleared, no $400B write). NTSC pulses stop at MIDI 33 and PAL pulses at
  MIDI 32 (A16); NTSC triangle notes at MIDI 21 (t = 2033) and PAL at MIDI 20 (t = 2001), after
  `tri_transpose`. A glide into the representable range starts the note at the next tick, a
  glide out of it silences it. (Revised 2026-09-28: the triangle used to clamp to $7FF.)
* Vibrato (`*_vibrato_rate` frames per half cycle, `*_vibrato_depth` period units,
  `*_vibrato_delay` frames): triangle LFO sampled at mid-frame,
  `offset = round(depth * tri((g + 0.5) / (2 rate)) / peak(rate))`, `g` = frames since the delay
  ended, `peak = 1` for odd rates and `1 - 1 / rate` for even rates (the largest mid-frame sample
  of the triangle), so that every rate swings exactly `+/- depth` as the parameter's unit says
  (e.g. rate 2 gives +d, +d, -d, -d; rate 4 gives d/3, d, d, d/3, ...). Revised 2026-09-28: without
  the scaling, even rates peaked at `depth * (1 - 1 / rate)` (half the depth at rate 2).
* Pitch envelope (`*_pitch_env_depth` at note start, `*_pitch_env_speed` frames to reach 0):
  `offset = depth * (speed - frame) / speed` (integer, toward zero), 0 when speed = 0. Positive
  depth = larger period = lower pitch. For the noise the offset is in period-index steps.
* Noise `nz_keyed = 1`: period index `15 - (round(note) - 36)`, clamped 0..15 (item 51);
  otherwise `nz_period`. DMC `dmc_keyed = 1`: rate index `dmc_rate + round(note) - 60`, clamped.
* Sweep: `sweep_enable = 1` writes `$4001 = 1PPP NSSS` from the parameters at note-on, and
  while it is moving the period (shift > 0) the driver no longer rewrites the timer (vibrato,
  pitch envelope and glide are then inactive on that channel). `sweep_enable = 0` writes $08
  (A22).

### Velocity mapping

* Pulse and noise with the software envelope: peak volume = `round(velocity * volume)` (4-bit),
  e.g. volume 15 -> `round(v * 15)` as in ENGINE_SPECS.md, volume 12 at velocity 0.5 -> 6.
* Hardware envelope (`*_env_enable = 1`): `volume` is the envelope divider period V and the
  decay always starts at 15, so velocity has no effect (the chip has no other volume control).
* Triangle and DMC have no volume register: velocity is ignored. The DMC level can be changed
  only through `dmc_direct_level` ($4011).

### Note off

* Pulse / noise, software envelope: enters the release stage (`sw_release` frames from the
  current volume to 0, 0 = cut). Hardware envelope: the loop flag is cleared (so a looping
  envelope decays out and stops), a non-looping one simply finishes its decay; the length
  counter runs from index 1 (254 half frames, 2.1 s) whenever the halt flag is clear, which is
  longer than the slowest decay (1 s). After this note-off the driver never writes $4003 /
  $4007 again (that would restart the decay at 15): vibrato, pitch envelope and pitch changes
  stay within the current 256-period page (A24).
* Triangle: $4015 triangle bit cleared then set again in the same tick: the length counter is
  forced to 0, the sequencer stops immediately and holds its step (A3). `tri_linear_length < 127`
  makes notes self-terminating after R quarter frames (control flag clear); 127 sets the control
  flag (hold until note off). `tri_gate_frames` rewrites $400B every N frames (reload flag,
  retrigger of the linear counter; audible only with `tri_linear_length < 127`).
  `tri_attack_frames` keeps the triangle halted for N frames before the note-on writes.
* DMC: a looping sample is stopped ($4015 bit 4 cleared: it silences when the current byte is
  done); a one-shot plays to its end. "Looping" is the loop flag the chip holds ($4010 written at
  note-on), not the current `dmc_loop` parameter, which may have changed during the note. Every
  $4015 write keeps the DMC bit at its current state (D = 1 only while bytes remain), so tone
  note-offs never restart or cut a sample.
* `isChannelActive` (ENGINE_SPECS "Conventions"), whether or not the key is held: the hardware
  still sounding (pulse/noise: length > 0 and volume > 0, an envelope start pending, or a looping
  hardware envelope; triangle running or its linear counter reload pending; DMC reader or output
  unit busy), or the driver about to raise a silent channel by itself (software attack from 0,
  triangle `attack_frames` delay, triangle `gate_frames` retrigger with the key held). A held
  key on a decayed non-looping hardware envelope, a software sustain of 0, an expired triangle
  linear counter, a refused note or a finished one-shot DMC sample is inactive. (Revised
  2026-09-28: a held gate used to count as active whatever the hardware state.)

### DMC samples

* 16 slots of 4096 bytes in three pre-allocated banks. The DMC reads only the bank that was
  active at its last note-on (like a mapper bank switch), published by the audio thread in an
  atomic `mappedBank` and kept until the next DMC note-on, so reloading the slot that is playing
  never changes the playing sample: the new data is heard from the next note-on. `loadSample()`
  (one message thread) encodes into a bank that is neither the active nor the mapped one (with
  three banks one always exists, so it never waits), copies the other slots from the active bank,
  then flips the atomic `activeBank`. At note-on the audio thread publishes its claim and checks
  that the bank is still active (at most 4 attempts, bounded; if all fail the note plays without
  sample); no lock, no allocation. (Revised 2026-09-28: two banks, a per-block claim and a 1 s
  timeout after which the loader overwrote the bank regardless.)
* Encoding happens at the rate selected by `dmc_rate` (and the clock standard) at load time; a
  later rate change plays the same bytes faster or slower, as on hardware. PCM is resampled to
  the bit rate by linear interpolation (a tool step, not hardware), then encoded by counter
  tracking from level 64 (A21): bit = 1 if the target `64 + 63 x` is above the modelled counter,
  and the model follows the hardware rule, so playback reproduces the encoder's model bit for bit
  (tested). The last partial byte and the padding up to the playable length `L * 16 + 1` are
  filled with alternating bits ($55, net change 0). Samples needing more than 4081 bytes are
  rejected (`loadSample` returns false).
* The driver maps the selected slot at $C000 (like a mapper bank switch) and writes $4012 = 0,
  $4013 = (length - 1) / 16, then restarts the channel through $4015.

### Resampling

* The APU is stepped one CPU cycle at a time in `renderBlock`; every change of the mixer output
  (and of each channel's solo level when per-channel outputs are requested) is passed to
  `BandLimitedStepSynth` at `clockIndex * hostSamplesPerClock`, with `hostSamplesPerClock =
  host rate / cpu clock` computed by the engine. The synths are prepared once at the NTSC clock:
  both clocks downsample, so the kernel (cutoff 0.45) is identical and switching NTSC/PAL on the
  audio thread needs no re-allocation. The synths track changes only; after `reset()` the
  power-up DC level (A20) is taken as the reference so a reset does not produce a thump.
* Kernel: `IntegratedStep` since 2026-09-30 (product-owner decision), as the SNES and Genesis
  engines: exact band-limited steps with a flat pass band. The earlier `ImpulseSum` kernel
  boosted the top octave by (w/2)/sin(w/2) (+0.9 dB at 11 kHz at 44.1 kHz; refcheck finding
  F2); `chiptool regs nes --kernel impulse` still renders it for that measurement.
* While the triangle is ultrasonic (A2) its stepping sequencer value is left out of the level
  comparison (the mixer uses 7.5 anyway), so the mixer is only evaluated when another channel
  changes rather than on every CPU cycle.
