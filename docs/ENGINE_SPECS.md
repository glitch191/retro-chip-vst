# Engine specifications

Companion to `ARCHITECTURE.md`. This file fixes what each engine exposes so that the
plugin layer, the preset generator and the randomizer can rely on it. Hardware tables
and formulas come from `docs/research/<chip>.md`.

## Common structure: chip core + driver

Each engine is two layers:

1. **Chip core**: registers, counters, clocking, mixing, exactly as documented. It has
   no notion of MIDI. It is clocked at the native rate and feeds `BandLimitedStepSynth`
   instances (main L/R and one pair per hardware channel when requested).
2. **Driver**: the small "sound engine" a game would run on the CPU. It turns
   `noteOn/noteOff/setChannelPitch` and the engine parameters into register writes,
   at the rate a real driver would (NES: once per video frame, 60.0988 Hz NTSC /
   50.0070 Hz PAL; Genesis: once per video frame, 3420 master clocks x 262 / 313 lines =
   59.92274 Hz NTSC / 49.70146 Hz PAL, see docs/research/genesis.md Ambiguities 28;
   SNES: once per 4 ms tick, like typical SPC700 drivers).
   Software features games implemented in code live here and are labelled as such:
   vibrato by period rewrites, pitch envelopes for drums, software volume envelopes,
   gating. Anything the driver writes goes through the register model, so the
   quantisation of the hardware is always audible.

Conventions:

* Pitch input is a float MIDI note (69 = 440 Hz). `noteOn` sets the pitch;
  `setChannelPitch` retunes a playing channel (glide/bend) and is picked up at the
  next driver tick, like a real pitch slide.
* Velocity (0..1) scales the initial volume/level where the hardware has a volume
  register; engines document the mapping (e.g. NES 4-bit volume = round(v * 15)).
* `isChannelActive()` is true from noteOn until the hardware envelope has fully
  released (level 0) or the channel was silenced.
* Per-channel outputs are the channel alone through the real output stage
  (NES: through the non-linear mixer with the other inputs at 0; SNES: dry voice
  through VOL L/R and MVOL, no echo; Genesis: FM channel through DAC/ladder model,
  PSG channel at its mix level).
* Every parameter has a `ParamDesc` entry with native units and hardware bounds.
  Keys below are the stable `ParamDesc::key` strings.

## NES: `Nes2A03Engine` (chip core `NesApu`)

Channels: 0 Pulse 1 (`P1`), 1 Pulse 2 (`P2`), 2 Triangle (`TRI`), 3 Noise (`NZ`),
4 DMC (`DMC`). Native clock: CPU clock 1789773 Hz (NTSC) / 1662607 Hz (PAL, 2A07).
Sample slots: 16 DMC samples, each up to 4081 bytes (hardware limit 16 × 255 + 1),
encoded from PCM by a 1-bit delta encoder at the DMC rate selected by `dmc_rate`.

Parameters (key: range, default):

* Global: `clock` 0..1 (NTSC/PAL) 0; `console_filter` 0..1 (NES-001 90 Hz + 440 Hz
  high-pass, 14 kHz low-pass) 1; `frame_mode` 0..1 ($4017 bit 7: 0 = 4-step sequence,
  1 = 5-step) 0. The driver writes $4017 ($40 or $C0, IRQ inhibited) at the next driver
  frame after a change, and only then, since every write restarts the sequence; the
  5-step mode clocks envelopes, the triangle's linear counter, sweeps and length counters
  less often (four quarter frames per 5-step sequence instead of per 4-step sequence).
* Pulse 1 and Pulse 2 (prefix `p1_` / `p2_`): `duty` 0..3 (2); `volume` 0..15 (12);
  `env_enable` 0..1 (0, 1 = hardware decay envelope, volume = decay divider period);
  `env_loop` 0..1; `sweep_enable` 0..1; `sweep_period` 0..7; `sweep_negate` 0..1;
  `sweep_shift` 0..7; `vibrato_rate` 0..15 (frames per half cycle, 0 = off);
  `vibrato_depth` 0..31 (period units); `vibrato_delay` 0..60 (frames);
  `sw_attack` 0..30 (frames); `sw_decay` 0..60 (frames); `sw_sustain` 0..15;
  `sw_release` 0..60 (frames); `pitch_env_depth` -64..64 (period units at note start);
  `pitch_env_speed` 0..30 (frames to reach 0); `transpose` -24..24 (semitones).
* Triangle (prefix `tri_`): `linear_length` 0..127 (linear counter reload; 127 = hold);
  `gate_frames` 0..30 (0 = off; retrigger every N frames); `attack_frames` 0..8
  (frames the channel is muted then unmuted at note start); `vibrato_rate`,
  `vibrato_depth`, `vibrato_delay` as pulse; `pitch_env_depth`, `pitch_env_speed`;
  `transpose` -24..24 (the triangle's own period formula already sounds one octave
  below the pulse for the same period).
* Noise (prefix `nz_`): `mode` 0..1 (0 long / 1 short, i.e. tap bit 1 / bit 6);
  `volume` 0..15; `env_enable`, `env_loop`; `period` 0..15; `keyed` 0..1 (1 = MIDI
  note selects the period: note 36 + n -> period 15 - n, clamped); `sw_attack`,
  `sw_decay`, `sw_sustain`, `sw_release`; `pitch_env_depth` -15..15 (period index
  steps); `pitch_env_speed` 0..30.
* DMC (prefix `dmc_`): `rate` 0..15; `sample` 0..15 (slot); `loop` 0..1;
  `keyed` 0..1 (1 = MIDI note offsets the rate index by note - 60, clamped 0..15);
  `direct_level` 0..127 (written to $4011 at note on).

Required tests (`dsp/tests/test_nes_*.cpp`): period tables NTSC and PAL for pulse
and triangle against documented reference values; noise and DMC period tables NTSC
and PAL; LFSR sequence properties (long: period 32767; short: period 93 or 31 per
documented start state); mixer non-linear formula against reference values for at
least six (pulse1, pulse2, triangle, noise, dmc) combinations including the
max/min points; sweep mute rules; frame sequencer clocking counts for 4- and 5-step
modes (envelope and length clocks per second at NTSC and PAL); DMC delta counter
clamping; engine-level: rendering a note produces the expected fundamental and no
allocation in renderBlock.

## SNES: `SnesDspEngine` (chip core `SnesDsp`)

Channels: 0..7 (`V1`..`V8`, names "Voice 1".."Voice 8"). Native rate 32000 Hz.
Sample slots: 32 BRR samples sharing a 64 KiB budget minus the echo buffer
(echo buffer = EDL × 2 KiB, minimum 4 bytes); `loadSample` returns false when the
budget is exceeded. `loadSample` encodes PCM with the built-in BRR encoder (best
filter per block chosen by minimum error; loop point optional via the `SampleInfo`
API of the engine). The engine also exposes `setSampleLoop(slot, loopStartBlock)`
and a root note per slot (`setSampleRootNote(slot, midiNote)`).

Parameters (key: range, default):

* Instrument (all voices): `sample` 0..31; `adsr_enable` 0..1 (1); `attack` 0..15;
  `decay` 0..7; `sustain_level` 0..7; `sustain_rate` 0..31; `gain_mode` 0..4
  (direct, linear decrease, exponential decrease, linear increase, bent-line
  increase); `gain_value` 0..127 (direct) / 0..31 (rates, clamped); `release_mode`
  0..1 (0 = hardware KOFF, 1 = driver writes GAIN exponential decrease with
  `release_rate` 0..31); `volume` 0..127; `pan` -64..64; `transpose` -24..24;
  `fine_tune` -100..100 (cents); `vibrato_rate` 0..63 (4 ms ticks per half cycle: 23 = 5.4 Hz);
  `vibrato_depth` 0..64 (pitch register units); `vibrato_delay` 0..250 (ticks, 1 s);
  `noise_enable` 0..1; `noise_clock` 0..31; `pmon` 0..1 (voices 1..7 modulated by
  previous voice); `loop_override` 0..2 (sample default / force one-shot / force loop).
* Echo: `echo_enable` 0..1; `echo_delay` 0..15; `echo_feedback` -128..127;
  `echo_volume` -128..127; `fir_preset` 0..8 (named coefficient sets: Pass-through,
  Low-pass soft, Low-pass strong, High-pass, Band-pass, Comb, Bright, Dark; 8 = Custom);
  `fir_c0` .. `fir_c7` -128..127 (FIR0..FIR7 written as-is when `fir_preset` is Custom;
  defaults 127, 0, ..., 0 = pass-through; FIR0 weights the oldest echo sample);
  `v1_echo` .. `v8_echo` 0..1 (EON per voice); `main_volume` 0..127.
* Instrument, added after the first release: `invert_left`, `invert_right` 0..1 (VOL L /
  VOL R written negative: the hardware registers are signed and a negative volume inverts
  that side's phase, the "surround" trick some games used).

Required tests (`dsp/tests/test_snes_*.cpp`): BRR decode of hand-built blocks for
all four filters against manual computation; BRR encode -> decode round trip error
bound on sines and noise; shift >= 13 behaviour; gaussian table spot values and
symmetry/sum properties; gaussian interpolation of a known sequence; ADSR/GAIN
rate table (all 32 periods) and envelope step sizes; pitch register from MIDI note
(4.12 fixed point) including PMON; echo delay length in samples per EDL; FIR
pass-through preset yields the delayed signal; feedback stability at max;
16-bit clamp on output; noise LFSR period 32767.

## Genesis: `GenesisEngine` (cores `Ym2612Core`, `Sn76489Core`)

Channels: 0..5 FM (`FM1`..`FM6`), 6..8 PSG tone (`PSG1`..`PSG3`), 9 PSG noise
(`PSGN`). Master clock 53693175 Hz (NTSC) / 53203424 Hz (PAL). YM2612 clock =
master / 7, sample rate = clock / 144 (53267 Hz NTSC). SN76489 clock = master / 15
(3579545 Hz NTSC), tone/noise counters at clock / 16. Both cores run together and
are mixed like the console (see research notes for the PSG/FM level ratio).
Sample slots: 16 DAC samples (8-bit unsigned PCM, up to 64 KiB each).

Parameters (key: range, default):

* Global: `clock` 0..1; `chip_revision` 0..1 (0 = YM2612 discrete with ladder
  effect, 1 = YM3438/ASIC without); `model1_lowpass` 0..1 (Model 1 output low-pass);
  `lfo_enable` 0..1; `lfo_freq` 0..7.
* FM patch (all FM channels): `algorithm` 0..7; `feedback` 0..7; `ams` 0..3;
  `fms` 0..7; `transpose` -24..24; `fine_tune` -100..100; `vibrato_rate` 0..15;
  `vibrato_depth` 0..64 (fnum units); `vibrato_delay` 0..60; `unison_detune` 0..15
  (0 = off; otherwise each note takes two FM channels, the second offset by this
  many fnum units); `pan` 0..3 (L, C, R, Off) per FM channel as `fm1_pan`..`fm6_pan`
  ($B4 bits L and R; Off clears both, so the channel is not output; on channel 6 this
  also mutes the DAC, which goes through channel 6's output bits).
  Per operator n = 1..4 (prefix `op1_`..`op4_`): `tl` 0..127; `ar` 0..31; `dr` 0..31;
  `sr` 0..31; `rr` 0..15; `sl` 0..15; `mul` 0..15; `dt` 0..7; `rs` 0..3; `am` 0..1;
  `ssg` 0..8 (0 = off, 1..8 = SSG-EG mode bits 0..7 with enable set).
  `velocity_depth` 0..127 (TL added to carrier operators at velocity 0).
* DAC (channel 6): `dac_enable` 0..1; `dac_sample` 0..15; `dac_rate` 4000..32000 Hz;
  `dac_keyed` 0..1 (note 60 = `dac_rate`, one semitone per note); `dac_loop` 0..1;
  `dac_volume` 0..127.
* PSG: `psg1_att`, `psg2_att`, `psg3_att`, `psgn_att` 0..15; `psg_noise_mode` 0..1
  (periodic/white); `psg_noise_rate` 0..3 (/512, /1024, /2048, tone 3);
  `psg_sw_attack`, `psg_sw_decay` 0..60 (frames); `psg_sw_sustain` 0..15;
  `psg_sw_release` 0..60; `psg_vibrato_rate` 0..15; `psg_vibrato_depth` 0..15
  (period units); `psg_unison_detune` 0..7 (0 = off; else a tone note also drives
  the next free tone channel detuned); `psg_transpose` -24..24.

Required tests (`dsp/tests/test_genesis_*.cpp`): fnum/block from MIDI note against
the documented reference table for NTSC and the recomputed PAL values; detune
table; phase increment for MUL 0 (x0.5); envelope rate table and attack/decay
timing against documented durations for several rates (tolerance documented);
SSG-EG modes shape; LFO frequencies (8) measured from the emulated LFO counter;
AMS depths in dB and FMS depths in cents; algorithm routing (which operators are
carriers); 9-bit DAC truncation and ladder offset (revision 0 vs 1);
SN76489 attenuation table (2 dB steps, 15 = silence); tone period from note;
period 0/1 behaviour; noise LFSR taps/period for white and periodic modes;
noise rate selection incl. tone-3 mode.

## Performance modules (`dsp/include/chipdsp/perf/`)

* `VoiceAllocator`: `configure(numHardwareChannels, enabledMask)`;
  `noteOn(note, velocity, sampleOffset) -> channel or -1`; `noteOff(note) -> channel`;
  policies: round-robin over enabled channels, oldest-note stealing when full;
  `midiChannelMode` passthrough (MIDI channel N -> hardware N-1 when enabled).
  Never returns a channel outside the enabled mask; never exceeds the chip's
  channel count.
* `Arpeggiator`: params `enabled`, `pattern` (Up, Down, UpDown, AsPlayed, Random),
  `octaves` 1..4, `rateMode` (Sync, Free), `syncDivision` (1/4, 1/8, 1/8T, 1/16,
  1/16T, 1/32), `freeRateHz` 0.5..50, `gatePercent` 5..100, `hold` 0/1.
  `process(TransportInfo, sampleRate, numSamples, inputEvents, outputEvents)`
  with a deterministic PRNG (`seed(uint32)`) for Random. Output events carry
  sample offsets. When the arp is off, input events pass through untouched.
* `Glide`: per channel `setTarget(note, isLegato)`, `params: timeMs 0..2000, mode
  (Always, LegatoOnly)`, `advance(numSamples, sampleRate)`, `currentNote(channel)`.
  Linear in semitones over the configured time; `LegatoOnly` glides only when the
  new note started while another note on that channel was still held.

Required tests (`dsp/tests/test_perf_*.cpp`): allocator never exceeds channel count
or leaves the mask, stealing takes the oldest; arpeggiator step timing at 120 BPM
1/16 = 125 ms, patterns produce the documented order for a held C-E-G over
1 and 2 octaves, gate length, Random is deterministic for a seed, pass-through when
disabled; glide reaches the target in the configured time, LegatoOnly jumps when
the previous note was released.
