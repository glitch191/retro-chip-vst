# Hardware notes: ambiguities and decisions

Each entry records a point where the public documentation is ambiguous, incomplete or
contradictory, the sources consulted, the choice implemented and the alternative. This
file consolidates the "Ambiguities" sections of `docs/research/nes.md` (A1..A26),
`docs/research/snes.md` (1..26), `docs/research/genesis.md` (1..41) and
`docs/research/plugin.md`, plus the decisions taken while integrating the engines in the
plugin. The research files hold the full reasoning, quotes and numbers; the reference in
brackets after each title points to the entry there. Full source list: `docs/SOURCES.md`.

Format:

```
### <Chip>: <topic>
Ambiguity: ...
Sources: ...
Decision: ...
Alternative: ...
```

"Driver" means the software layer a game would run on the console CPU (see
`docs/ENGINE_SPECS.md`); decisions about it are software behaviour, not chip behaviour.

---

## NES (2A03 / 2A07)

### NES: exact or rounded CPU clock [nes A1]
Ambiguity: the cycle chart gives 1789772.727... / 1662607.03125 Hz; period tables and the
specs use 1789773 / 1662607.
Sources: NESdev "Cycle reference chart", "APU period table".
Decision: the integers; they reproduce the NESdev 80-note NTSC table bit-exactly.
Alternative: the exact fractions (0.15 ppm, inaudible).

### NES: triangle at periods 0 and 1 [nes A2]
Ambiguity: the ultrasonic triangle's output is not specified by the wiki.
Sources: NESdev "APU Triangle", "APU basics" (period 0 pop); blargg `apu_ref.txt`
("half way between 7 and 8").
Decision: below period 2 the core feeds a constant 7.5 into the mixer (documented
average and pop, no 1.79 M steps per second).
Alternative: run the sequencer and let the resampler average it (about 30x the cost).

### NES: triangle output while halted [nes A3, resolved]
Ambiguity: "suspended" could mean output 0 or hold.
Sources: NESdev "APU Triangle"; blargg `apu_ref.txt` (stops "at whatever phase it's at").
Decision: hold the current step value.
Alternative: none left.

### NES: noise LFSR power-up value [nes A4]
Ambiguity: "loaded with 1" (APU Noise) versus "$0000, first clock shifts in a 1".
Sources: NESdev "APU Noise", "CPU power up state".
Decision: 1. Both give the same state after the first clock.
Alternative: none needed.

### NES: noise period unit, factor 2 between sources [nes A5]
Ambiguity: Brad Taylor's table is half the wiki's and headed "CPU clock cycles".
Sources: NESdev "APU Noise", blargg, Brad Taylor (2A03 reference, NESSOUND.txt).
Decision: wiki / blargg values in CPU cycles; timer reload `period / 2 - 1` APU cycles.
Alternative: Brad Taylor literally (noise one octave higher).

### NES: DMC rate index 13 (NTSC) [nes A6]
Ambiguity: 84 CPU cycles (wiki, blargg) versus 85 (Brad Taylor, $2A8 per byte).
Sources: NESdev "APU DMC", blargg, Brad Taylor.
Decision: 84 (two sources; every APU-clocked period is even).
Alternative: 85 (probably a typo).

### NES: length counter units [nes A7]
Ambiguity: wiki values are twice Brad Taylor's (half-frame clocks versus 60 Hz frames).
Sources: NESdev "APU Length Counter", Brad Taylor.
Decision: wiki values, decremented on every half-frame clock.
Alternative: none (same durations).

### NES: pulse duty phase after a $4003 restart [nes A8]
Ambiguity: the wiki shows both a bit string and a time-ordered waveform.
Sources: NESdev "APU Pulse".
Decision: time order `0 1 0 0 0 0 0 0` (12.5 %), restart at index 0.
Alternative: start on the `1` step (one-step phase shift, audible only in the restart pop).

### NES: PAL period table [nes A9]
Ambiguity: no published PAL note table.
Sources: NESdev "APU period table" (generator script), FamiTracker forum t=13626.
Decision: the NESdev script formula at 1662607 Hz (same as the engine formula).
Alternative: none needed.

### NES: output filter corners [nes A10]
Ambiguity: 90 / 440 Hz / 14 kHz (wiki) versus 93.6 / 442 Hz measured, and 15.4 kHz from
the quoted 47 kOhm / 220 pF.
Sources: NESdev "APU Mixer"; NESdev forum p=44255.
Decision: 90 Hz and 440 Hz high-pass, 14 kHz low-pass (`console_filter`).
Alternative: 93.6 / 442 Hz / 15.4 kHz (inaudible difference).

### NES: Famicom output stage [nes A11]
Ambiguity: only a 37 Hz high-pass is documented, then an RF modulator of unknown response.
Sources: NESdev "APU Mixer".
Decision: not modelled; `console_filter` is the NES-001 chain.
Alternative: a console option with the 37 Hz high-pass only.

### NES: $4017 write delay and jitter [nes A12]
Ambiguity: the reset happens 3 or 4 CPU cycles after the write depending on parity.
Sources: NESdev "APU Frame Counter".
Decision: constant 3 cycles.
Alternative: model the odd/even case (not audible).

### NES: DMC $4011 write colliding with a timer clock [nes A13]
Ambiguity: the level is "occasionally not changed properly".
Sources: NESdev "APU DMC".
Decision: not modelled; the write always applies.
Alternative: model the collision (no exact rule published).

### NES: sweep divider reload order [nes A14]
Ambiguity: when the first period update after a $4001 write happens.
Sources: NESdev "APU Sweep".
Decision: the two documented ordered steps, exactly.
Alternative: none (documented behaviour).

### NES: frame counter at power-up [nes A15]
Ambiguity: $4017 = $00 at power-up with an unknown phase against the driver tick.
Sources: NESdev "CPU power up state", "APU Frame Counter".
Decision: the driver writes $40 at `prepare()` / `reset()` and the sequencer starts there.
Alternative: $C0 (5-step, no IRQ), as some games did.

### NES: lowest PAL pulse note [nes A16]
Ambiguity: PAL reaches MIDI 32 (period 2001), NTSC stops at MIDI 33.
Sources: NESdev "APU period table", "APU Pulse".
Decision: the driver refuses notes by computed period (> 2047), so PAL keeps the extra note.
Alternative: the same lowest note on both clocks.

### NES: 5-step mode half-frame steps [nes A17]
Ambiguity: which steps clock the length/sweep units after a $4017 write with bit 7 set.
Sources: NESdev "APU Frame Counter", blargg (agree); Brad Taylor (differs by one step).
Decision: wiki + blargg.
Alternative: Brad Taylor's ordering (same 96 / 192 Hz rates, shifted by one step).

### NES: DMC level against triangle/noise volume [nes A18]
Ambiguity: "57 % total volume" (Brad Taylor) versus about 43 % remaining from the formula.
Sources: NESdev "APU Mixer", blargg, Brad Taylor.
Decision: the exact mixer formula.
Alternative: none; the figure is only a qualitative cross-check.

### NES: irregular PAL DMC and noise entries [nes A19]
Ambiguity: PAL DMC index 4 = 276 and noise entries 0-2 do not scale like the rest.
Sources: NESdev "APU DMC", "APU Noise" (the only PAL source).
Decision: keep the wiki values.
Alternative: none sourced.

### NES: triangle sequencer phase at power-up [nes A20]
Ambiguity: not documented; it sets a held triangle input that changes noise/DMC loudness
through the non-linear mixer.
Sources: NESdev "CPU power up state" (silent on it).
Decision: step 0 (value 15), like the other cleared counters.
Alternative: step 16 (value 0).

### NES: DMC encoder start level [nes A21]
Ambiguity: no convention for the start level of converted samples.
Sources: NESdev "APU DMC" (counter behaviour only).
Decision: the encoder assumes 64; `dmc_direct_level` defaults to 64.
Alternative: start at 0 (every sample then opens with a ramp).

### NES: sweep register when the sweep is off [nes A22]
Ambiguity: with $4001 = $00 the sweep mutes every period >= $400 even when disabled.
Sources: NESdev "APU Sweep", "APU basics" (writes $08).
Decision: `sweep_enable = 0` writes $08; `sweep_enable = 1` writes the parameters verbatim
and the mute rules apply.
Alternative: always verbatim (low notes muted with the sweep off).

### NES: DMC sample fetch latency [nes A23]
Ambiguity: the DMA fetch takes 1-4 CPU cycles.
Sources: NESdev "APU DMC".
Decision: immediate fetch (no audible effect).
Alternative: a 4-cycle delay.

### NES: timer rewrites after a hardware-envelope note-off [nes A24]
Ambiguity: every $4003/$4007 write restarts the decay; vibrato after note-off would never
let a note end.
Sources: NESdev "APU Pulse", "APU Envelope".
Decision (driver): after note-off with `*_env_enable = 1` no more high-byte writes; pitch
changes stay on the last page.
Alternative: freeze vibrato at note-off, or keep the restarts.

### NES: DC blocker corner [nes A25]
Ambiguity: no separate coupling corner is documented.
Sources: NESdev "APU Mixer", forum p=44255.
Decision: `console_filter = 1`: 90 Hz + 440 Hz + 14 kHz only; `console_filter = 0`: 5 Hz
one-pole DC blocker (not a hardware value).
Alternative: no high-pass with the filter off (DC level left in).

### NES: $4017 IRQ inhibit timing [nes A26]
Ambiguity: whether the inhibit bit also waits for the 3-4 cycle delay.
Sources: NESdev "APU Frame Counter" (raw wikitext).
Decision: inhibit applies at the write; only the reset and mode change are delayed.
Alternative: delay the inhibit too (not audible, the driver runs with IRQ inhibited).

### NES: duty 25 % and 75 % [integration]
Ambiguity: the two duties have the same magnitude spectrum when a pulse plays alone, so
they look like duplicates.
Sources: NESdev "APU Pulse" (75 % is the inverted 25 % sequence), "APU Mixer".
Decision: both kept in the parameter range and in the presets; they differ through the
non-linear mixer when both pulses play, and in phase relationships.
Alternative: merge presets that differ only by 25 / 75 %.

### NES: output level [integration]
Ambiguity: none in the hardware; recorded because it is quiet in a DAW.
Sources: NESdev "APU Mixer".
Decision: output is hardware-relative: one pulse at volume 12 is about -33 dBFS RMS.
`master_gain` (-24..+12 dB) compensates.
Alternative: normalise each chip (not hardware-relative).

## NES and Genesis drivers

### Drivers: note-on just before a frame tick [integration]
Ambiguity: a note keyed right before the next driver frame lost its first software step
(a 2-frame PSG hat played one frame; a softer one was silent). Games handle notes inside
their frame tick, so the first step always lasts a full frame there.
Sources: none (driver behaviour); research nes.md and genesis.md "Driver tick model".
Decision: a note keyed less than half a frame before the next tick skips that tick for its
software envelope, vibrato and pitch envelope; the first step lasts 0.5-1.5 frames (one
frame on average).
Alternative: quantise note-ons to the frame grid (up to 16.7 ms latency).

## SNES (S-DSP)

### SNES: Gaussian arithmetic domain [snes 1]
Ambiguity: fullsnes (`>> 10`, 16-bit wrap/clamp, then SAR 1) and Anomie (`>> 11`, clip15,
clamp15) differ by 1-2 LSB.
Sources: fullsnes; Anomie's S-DSP doc; SNESdev wiki.
Decision: Anomie's form.
Alternative: the fullsnes form.

### SNES: Gaussian index bits [snes 2]
Ambiguity: fullsnes mentions both bits 3..11 and 4..11.
Sources: fullsnes, Anomie.
Decision: bits 4..11 (256 fractions, 512-entry table).
Alternative: none.

### SNES: pitch step clamp with PMON [snes 3]
Ambiguity: whether the modulated step or the counter is clamped.
Sources: fullsnes, Anomie.
Decision: register masked to 14 bits, modulated step not clamped, index clamped to 0x7FFF.
Alternative: clamp the step to 0x3FFF (audibly shallower PMON).

### SNES: PMON on noise voices [snes 4]
Ambiguity: Anomie's code masks PMON with ~NON; his text says it still affects BRR speed.
Sources: Anomie, fullsnes.
Decision: no modulation on noise voices (the code).
Alternative: modulate the decode speed.

### SNES: Attack to Decay threshold [snes 5]
Ambiguity: switch at 0x7E0 after 63 steps (fullsnes) or at 0x7FF after 64 (one reading of
Anomie).
Sources: fullsnes, Anomie, SNESdev "DSP envelopes", official manual timings.
Decision: test the computed value every sample (Decay at 0x7E0; A = 15 after 2 steps).
Alternative: gate the test on the counter event.

### SNES: Decay to Sustain threshold [snes 6]
Ambiguity: `(E >> 8) == SL` (Anomie, SNESdev) versus `E <= (SL + 1) * 0x100` (fullsnes).
Sources: Anomie, SNESdev, fullsnes.
Decision: Anomie / SNESdev.
Alternative: fullsnes (differs by one step at exact boundaries).

### SNES: release at BRR end [snes 7]
Ambiguity: step -0x800 (fullsnes) versus E = 0 at once (Anomie).
Sources: fullsnes, Anomie.
Decision: E = 0 and Release when the end-without-loop header is loaded (same result).
Alternative: none needed.

### SNES: when ENDX is set [snes 8]
Ambiguity: at the start of the end block or when it completes.
Sources: fullsnes, Anomie (both readings), SNESdev "S-DSP registers".
Decision: start of the block.
Alternative: at completion (at most one block later).

### SNES: final phase inversion [snes 9]
Ambiguity: only fullsnes documents `XOR FFFFh` after the mute.
Sources: fullsnes; Anomie and SNESdev silent.
Decision: applied in the output stage, on every bus alike.
Alternative: omit it.

### SNES: bent-increase threshold value [snes 10]
Ambiguity: compare the clamped E or the pre-clamp 11-bit value.
Sources: fullsnes, Anomie.
Decision: per-voice `hiddenEnv = newValue & 0x7FF`.
Alternative: compare the clamped E.

### SNES: envelope phase after a rate change [snes 11]
Ambiguity: "the location of the 1st step may vary" (fullsnes).
Sources: Anomie (offset table), fullsnes.
Decision: `(counter + offset) % period` with one global counter.
Alternative: none.

### SNES: per-voice bus polarity [snes 12]
Ambiguity: not a hardware question (plugin buses).
Sources: none.
Decision: mirror the main bus (inversion applied, mute honoured).
Alternative: none.

### SNES: official ADSR/GAIN timing tables [snes 13]
Ambiguity: the manual's times are rounded, and the exponential ones do not measure the
time to 0.
Sources: SNESdev "DSP envelopes", Super Famicom wiki (manual transcription).
Decision: tests use the exact step/period formulas; the manual is a +/-10 % check for the
linear phases only.
Alternative: none.

### SNES: N-SPC FIR preset bytes [snes 14, resolved]
Ambiguity: the standard filter bytes were missing at first.
Sources: SnesLab "FIR Filter", fullsnes (manual example).
Decision: ship the five documented sets under their names; other `fir_preset` sets are
labelled in-house designs.
Alternative: none.

### SNES: registers at power-on [snes 15]
Ambiguity: "most registers are uninitialized".
Sources: Anomie.
Decision: `reset()` zeroes all 128 registers, FLG = 0xE0; the driver initialises the rest.
Alternative: random contents (not reproducible).

### SNES: volume products at -128 [snes 16]
Ambiguity: `-32768 * -128 >> 7` wraps (Anomie's cast) or clamps.
Sources: Anomie, fullsnes.
Decision: clamp16.
Alternative: wrap to -32768.

### SNES: global counter clock [snes 17]
Ambiguity: SNESdev says "each S-SMP clock"; its own timings only fit the 32 kHz sample.
Sources: SNESdev "DSP envelopes", Anomie, fullsnes.
Decision: one decrement per output sample.
Alternative: none consistent with the published timings.

### SNES: low bit of the VxVOL product [snes 18]
Ambiguity: `(env * V) >> 6` (fullsnes) or `((env15 * V) >> 7) << 1` (one reading of Anomie).
Sources: fullsnes, Anomie.
Decision: fullsnes's explicit formula.
Alternative: the always-even form (1 LSB difference).

### SNES: frozen echo buffer length [snes 19]
Ambiguity: Anomie's overview says 0.96 s; every other statement gives 30720 bytes.
Sources: Anomie, fullsnes, SNESdev.
Decision: 0.24 s (7680 stereo samples).
Alternative: none.

### SNES: Gaussian quad sum at d = 2 [snes 20]
Ambiguity: an earlier note said 0x800; the table gives 0x7FF.
Sources: fullsnes, Anomie, SnesLab tables (identical).
Decision: the table is authoritative (tested).
Alternative: none.

### SNES: output of key-on sample #0 [snes 21]
Ambiguity: whether sample #0 still uses the old envelope.
Sources: Anomie (S3c order, key-on sequence).
Decision: old level on #0; #1..#5 silent; first data sample on #6.
Alternative: 0 on #0.

### SNES: Decay to Sustain at SL = 7 with a fast rate [snes 22]
Ambiguity: 0 or 1 decay step depending on the counter phase.
Sources: Anomie order of operations.
Decision: keep the documented order (store, then transitions).
Alternative: test the transition before the store.

### SNES: analog output high-pass [snes 23]
Ambiguity: no source gives the coupling corner.
Sources: none.
Decision: common 5 Hz one-pole DC blocker, labelled non-chip.
Alternative: a measured value.

### SNES: envelope apply versus update order [snes 24]
Ambiguity: the first draft updated the envelope before the multiply, against Anomie S3c.
Sources: Anomie S3b/S3c/S4 and key-on sequence.
Decision: apply stored E, then FLG.7, BRR end, KOFF, KON, then compute the new E.
Alternative: the first draft (every change one sample earlier).

### SNES: driver vibrato range [integration]
Ambiguity: none in the hardware; the first range made 8.3 Hz the slowest vibrato.
Sources: none (driver software behaviour, 4 ms tick).
Decision: `vibrato_rate` 0..63 ticks per half cycle (about 2 Hz at 63), `vibrato_delay`
0..250 ticks (1 s).
Alternative: the narrower range.

### SNES: 41 samples for 32 slots [integration]
Ambiguity: the bank ships more samples than the S-DSP directory the engine exposes.
Sources: fullsnes / SNESdev (directory, 64 KiB APU RAM).
Decision: presets always write the `sample` slot parameter; drum and melodic families
share slots; loading a preset clears the other factory samples, so the budget holds for
any preset sequence (smallest free APU RAM seen in the sweep: 29,136 bytes).
Alternative: one fixed slot per sample (would need more than 32 slots).

### SNES: root note of the pipe-organ sample [snes 25]
Ambiguity: the VCSL file `Rode_Man3Open_A3.wav` is labelled with the key played (A3), but
the open stop sounds an octave higher (no energy at 220 Hz, harmonic series on 440 Hz).
Sources: VCSL (CC0, `docs/SOURCES.md` "Samples"); spectrum of the converted sample.
Decision: a sample's root is its sounding pitch: root 69 (A4); the Pipe Organ seed offers
an OctDown variant for the 8-foot pitch.
Alternative: keep the key label as the root and transpose every organ seed by -12.

### SNES: CC0 sample loops [integration]
Ambiguity: none in the hardware; a loop of L frames can only hold partials at multiples of
rate / L, so a loop that holds a fractional number of periods plays off-key (the grand
piano's first loop was 13 cents flat, the string section 10 cents).
Sources: `docs/research/snes.md` "Samples, loops and BRR encoding",
`docs/research/snes-sound-design.md` "Sample fixes".
Decision: `tools/samplegen/cc0_import.py` accepts only loop lengths (on 16-frame BRR
boundaries) whose loop pitch is within 3 cents of the root.
Alternative: accept the nearest loop point and correct the root note (every other key of
the attack would then be off-key).

### SNES: preset instrument and echo settings [snes 26]
Ambiguity: no public document lists the ADSR, GAIN or echo values of commercial games,
except in tables extracted from the games (rejected by the product-owner rule).
Sources: SnesLab and Super Famicom Wiki N-SPC pages, AddmusicK readme, Super MIDI Pak
manual (`docs/research/snes-sound-design.md`).
Decision: envelopes designed from the envelope mechanics and each recording's acoustic
envelope (plucked and struck sounds A15, decay to a sustain level, fade while held;
sustained sounds hold, with a GAIN release written at note off); echo in EDL 3..6, EFB
0x28..0x60, EVOL 0x28..0x40 with the documented FIR sets, a range that contains the Super
MIDI Pak starting point (EDL 5, EFB 0x3C, EVOL 0x40).
Alternative: game-extracted instrument tables (not allowed).

## Genesis YM2612

### Genesis YM2612: chip revision and ladder effect [genesis 11, 27; research "Ladder effect"]
Ambiguity: the discrete YM2612 (Model 1 VA0-VA6, Model 2 VA2) has a DAC crossover gap
around 0 (the "ladder effect"); the YM3438/ASIC (Model 1 VA7, most Model 2, Model 3) does
not. The constants differ between sources, and the board gains differ (24x / 6x).
Sources: Eke and Sauraen (SpritesMind t=386 p56), jsgroth parts 1 and 5, GManiac (p37),
TmEE, nukeykt (t=3049), Kabuto notes.
Decision: `chip_revision` 0 (default) = discrete YM2612 with aggregate offsets +4 (sample
>= 0) / -3 (sample < 0), +-4 when muted by L/R, per channel and side; 1 = YM3438 without
offsets. Same digital full scale for both revisions. GManiac's level-ordering glitches are
not modelled.
Alternative: the four silence slots individually (cycle-level DAC), or a louder YM3438.

### Genesis YM2612: EG clock ratio [genesis 1]
Ambiguity: External Clock / 351 versus FM clock / 3.
Sources: Nemesis (t=386 p8, p12), jsgroth part 3.
Decision: one EG update per 3 FM samples.
Alternative: 2.4375 samples (every envelope 19 % shorter).

### Genesis YM2612: global EG counter [genesis 2]
Ambiguity: counter width and whether 0 is skipped.
Sources: Nemesis, jsgroth.
Decision: 12-bit counter, 4095 to 1.
Alternative: plain free-running counter.

### Genesis YM2612: attack formula [genesis 3]
Ambiguity: Nemesis's formula, jsgroth's shift, GreenLine's patent form.
Sources: Nemesis, jsgroth, GManiac and GreenLine (t=932).
Decision: `att += (inc * ~att) >> 4` (matches GManiac's 73-step measurement).
Alternative: Nemesis's form.

### Genesis YM2612: attenuation at key-on [genesis 4]
Ambiguity: cleared or not at key-on.
Sources: Nemesis (revised), Eke, jsgroth.
Decision: not cleared, except rates 62-63 which set 0.
Alternative: none.

### Genesis YM2612: SL = 15 [genesis 5]
Ambiguity: 1023 (brief) or 0x3E0 (93 dB).
Sources: OPNA manual Table 2-7, jsgroth.
Decision: 0x3E0.
Alternative: 0x3FF.

### Genesis YM2612: rate refresh timing [genesis 6]
Ambiguity: rate recomputed immediately, at phase start, or at next key-on.
Sources: Eke (MAME notes), Nemesis, Shiru / Alone Coder.
Decision: recompute on every register change.
Alternative: latch at phase start.

### Genesis YM2612: SSG-EG increment [genesis 7]
Ambiguity: x6 (Nemesis) or x4 below 0x200 (jsgroth).
Sources: Nemesis p8, jsgroth part 7.
Decision: x4 with the 0x200 gate.
Alternative: x6 (1.5x repetition rate).

### Genesis YM2612: EG dB range [genesis 8]
Ambiguity: 48 dB (Nemesis) or 96 dB.
Sources: Nemesis p11, jsgroth.
Decision: amplitude = 2^(-att/64) (both agree on it); 0.0941 dB per step.
Alternative: none.

### Genesis YM2612: operator sign [genesis 9]
Ambiguity: negation or ones' complement.
Sources: jsgroth, GManiac (range +-8168).
Decision: negation.
Alternative: `~mag`.

### Genesis YM2612: carrier accumulation [genesis 10]
Ambiguity: 14-bit sum then clamp, or 9-bit per carrier.
Sources: jsgroth parts 4 and 5, Sauraen.
Decision: truncate each carrier to 9 bits, sum, clamp to -256..255.
Alternative: 14-bit sum (identical for algorithms 0-3).

### Genesis YM2612: reference F-number table [genesis 12]
Ambiguity: plutiedev's "approximate" table versus the rounded formula.
Sources: plutiedev, OPNA manual.
Decision: formula rounded to nearest; tests allow 2 units against plutiedev.
Alternative: hard-code the plutiedev table.

### Genesis YM2612: LFO frequencies [genesis 13, 29]
Ambiguity: manual frequencies imply dividers one larger than the hardware table; the
divider table's only source could not be re-read.
Sources: OPNA manual, plutiedev, jsgroth part 6.
Decision: dividers 108 77 71 67 62 44 8 5.
Alternative: 109 78 72 68 63 45 9 6 from the manual.

### Genesis YM2612: PM application [genesis 14]
Ambiguity: phase or frequency modulation, carry from bit 8.
Sources: OPNA manual, jsgroth, Nemesis p33.
Decision: jsgroth's 12-bit fnum method (depths within 0.7 cents of the manual).
Alternative: none.

### Genesis YM2612: RR 15 release time [genesis 15]
Ambiguity: plutiedev's 2.4 ms counts EG updates as samples.
Sources: plutiedev.
Decision: 7.2 ms (384 samples), from the register model.
Alternative: none.

### Genesis YM2612: operator order and pipeline [genesis 22]
Ambiguity: optional detail, not disputed.
Sources: Nemesis p13, jsgroth part 4.
Decision: S1, S3, S2, S4 with the documented delayed paths (S2->S3, S2->S4 one sample;
S3->S4 none), plus the S1 history delay of [genesis 41].
Alternative: none.

### Genesis YM2612: pipeline delay of S1 as a modulator [genesis 41]
Ambiguity: the delays derived from the evaluation order (S1->S2 0, S1->S3 1, S1->S4 0) are
our reading of an excerpt; no public text states the S1 delays. Two independent reference
emulators run as black boxes (Nuked OPN2, MAME / Genesis Plus GX) agree with each other and
differ from that derivation by up to 9.5 dB on algorithms 0-3.
Sources: Nemesis p13, jsgroth part 4 (excerpt); differential check
(`docs/research/refcheck-report.md` finding F1, `docs/research/reference-emulators.md`).
Decision: every S1 path one sample later, as if S1 were read from its feedback history:
S1->S2 1, S1->S3 2, S1->S4 1 (all eight algorithms then within the check's noise of Nuked);
S1 as a carrier (algorithm 7) also reaches the accumulator one sample after S2-S4, which
both references show as a one-FM-sample offset of an S1-alone tone.
Alternative: the derivation from the order alone (the code before 2026-09-30).

### Genesis YM2612: timers and CSM [genesis 23]
Ambiguity: not needed by a driver-based instrument.
Sources: OPNA manual.
Decision: not modelled.
Alternative: model them.

### Genesis YM2612: AMS depth sign [genesis 24]
Ambiguity: "+-1.4 dB" (plutiedev) versus 0..depth.
Sources: OPNA and Sega manuals, plutiedev, jsgroth part 6.
Decision: unipolar attenuation 0..max.
Alternative: +-depth around a centre (impossible at TL 0).

### Genesis YM2612: exponential table formula [genesis 25]
Ambiguity: two formulas.
Sources: Nemesis, OPL ROM convention.
Decision: none needed, identical for all entries.
Alternative: none.

### Genesis YM2612: driver tick rate [genesis 28]
Ambiguity: the specs first used the NES frame rates.
Sources: Sega Retro specifications, RadDad772 (3420 master clocks per line).
Decision: 59.92274 Hz NTSC / 49.70146 Hz PAL.
Alternative: 60.0988 Hz (0.3 % faster software modulation).

### Genesis YM2612: AM while the LFO is disabled [genesis 31, 38]
Ambiguity: the LFO counter is frozen at 0 or at its current position.
Sources: jsgroth part 6, Nemesis (t=386 p26), Eke (p33).
Decision: AM stays applied with the counter held at 0 (up to 11.8 dB with AMS 3).
Alternative: freeze at the current position, reset on enable.

### Genesis YM2612: $A4-$A6 latch scope [genesis 32]
Ambiguity: one latch per channel or one shared.
Sources: plutiedev, Sega manual.
Decision: one per channel (identical for drivers writing pairs in order).
Alternative: one latch per bank.

### Genesis YM2612: SSG-EG inversion at key-on [genesis 33]
Ambiguity: only key-off is documented to clear the flag.
Sources: jsgroth part 7, Nemesis.
Decision: key-on clears it too.
Alternative: keep it across notes.

### Genesis YM2612: SSG-EG hold overshoot [genesis 34]
Ambiguity: attenuation can step past 0x200 and wrap in hold modes.
Sources: jsgroth part 7.
Decision: formulas implemented literally.
Alternative: clamp to 0x200.

### Genesis YM2612: key edge timing [genesis 35]
Ambiguity: an off/on pair faster than one sample may be missed on hardware.
Sources: Nemesis, jsgroth.
Decision: edge per register write.
Alternative: sample the key state per operator slot.

### Genesis DAC: software volume rounding [genesis 39]
Ambiguity: driver rounding is unspecified.
Sources: none (driver behaviour).
Decision: round to nearest, symmetric around 0x80.
Alternative: truncation.

### Genesis FM: unison retrigger [genesis 40]
Ambiguity: what a repeated note does to the unison partner.
Sources: none (driver behaviour).
Decision: the owner re-keys its own partner.
Alternative: release it and take the next free channel.

## Genesis SN76489 (PSG)

### Genesis SN76489: LFSR variant [genesis 19; research "Noise channel"]
Ambiguity: the LFSR differs between chip versions (Sega VDP PSG 16-bit taps 0x0009;
discrete SN76489 15-bit taps 0x0003; Tandy 0x0011), and the reset value is "cleared"
(TI) or 0x8000 (Maxim); Maxim says "up to 65535" but the register gives 57337.
Sources: Maxim (SMS Power "SN76489", Charles MacDonald's sampled data), TI SN76489AN
datasheet, Howel's transcription.
Decision: 16-bit register, white taps bits 0 and 3 (0x0009), periodic taps bit 0, reset
0x8000 on every noise register write, output bit 0; tests check period 57337 (white) and
16 (periodic).
Alternative: the discrete 15-bit variant (period 32767), which is not the Genesis chip.

### Genesis SN76489: PSG level against FM [genesis 17]
Ambiguity: PSG about equal to FM at the amplifier (Jorge Nuno) versus divided by 6-6.4 in
software; varies by model.
Sources: SpritesMind t=730, t=1631.
Decision: one PSG channel at full scale = FM channel full scale / 6.4 (-16.1 dB). Single
PSG presets therefore play about 16-22 dB below FM presets; `master_gain` compensates.
Alternative: near-equal levels (0 dB).

### Genesis SN76489: volume table entry 7 [genesis 18]
Ambiguity: published 6568 versus 6538 computed.
Sources: Maxim.
Decision: keep the published table.
Alternative: 6538.

### Genesis SN76489: period 0 [genesis 20, 30]
Ambiguity: Maxim says 0 and 1 output +1 on Sega chips; the TI counter model would stick.
Sources: Maxim, TI datasheet (noise Table 3 row misprint resolved by row order).
Decision: periods 0 and 1 output a constant +1.
Alternative: period 0 holds the previous level.

### Genesis SN76489: output polarity [genesis 21]
Ambiguity: 0/+1 internal versus centred.
Sources: Maxim.
Decision: 0/+1 into the DC blocker (exact periodic-noise and period-1 DC).
Alternative: -0.5/+0.5 in the mixer.

### Genesis SN76489: tone-3 noise with period 0 [genesis 36]
Ambiguity: "period 0 behaves like 1" covers tone output only.
Sources: Maxim.
Decision: counter model as is (behaves like period 1).
Alternative: none.

### Genesis PSG: noise ignores the driver vibrato [integration]
Ambiguity: none in the hardware; the preset axis "Zap ... Vibrato Fast/Slow" was inert.
Sources: none (driver design).
Decision: the noise channel is not vibrated; QA removed the duplicate presets.
Alternative: vibrate the tone-3 period when the noise uses it.

## Genesis output stage

### Genesis: Model 1 and Model 2 filters [genesis 16, 26]
Ambiguity: Model 2's second-order low-pass has no published cutoff or Q.
Sources: Kabuto, jsgroth "Audio Filtering", Residual Media.
Decision: `model1_lowpass` = first-order 3.39 kHz (VA0-VA2); the VA3-VA6 2.84 kHz variant is
documented but not exposed; Model 2 is not modelled.
Alternative: a biquad fitted to a Model 2 recording.

### All chips: resampler kernel [integration, refcheck F2]
Ambiguity: none in the hardware; the host-rate conversion must not colour the chip output.
The original kernel (sampled windowed-sinc impulses summed by a discrete running sum)
boosted the top octave by (w/2)/sin(w/2), w = 2 pi f / host rate: +0.94 dB at 11.2 kHz and
+1.48 dB at 14 kHz at 44.1 kHz, measured on PSG squares and seen against both reference
emulators.
Sources: `docs/research/refcheck-report.md` finding F2.
Decision: every engine (NES, Genesis FM and PSG, SNES) uses the `IntegratedStep` kernel
(taps = first differences of the integrated windowed sinc: exact band-limited steps, flat
pass band). The NES kept the `ImpulseSum` kernel until 2026-09-30 and was switched by a
product-owner decision; its top octave lost the boost (-0.9 dB at 11 kHz), and the NES
refcheck against NSFPlay moved by the formula. `ImpulseSum` stays only for
`chiptool regs nes --kernel impulse` (the F2 measurement).
Alternative: keep the legacy kernel on the NES (top-octave boost by the formula above).

### Genesis: output coupling capacitor [genesis 37]
Ambiguity: no source gives the high-pass.
Sources: none.
Decision: 5 Hz one-pole DC blocker (removes ladder offsets and PSG DC).
Alternative: a value measured on a Model 1.

### Genesis: state at reset [integration]
Ambiguity: the console charges its coupling capacitor at power-on; a plugin reset is not a
power-on, and the idle ladder / PSG offsets reached the DC blocker as a click.
Sources: none (output-stage modelling decision).
Decision: at reset the chip is clocked 16 FM samples and the level trackers start at the
idle output (ladder offsets, unipolar PSG levels), so no DC step at reset or when a
per-channel bus turns on. About 1.6e-5 remains after a reset.
Alternative: start from 0 and let the 5 Hz DC blocker settle (audible click).

### Genesis: output level [integration]
Ambiguity: none in the hardware.
Sources: research genesis "Console low-pass filters and levels".
Decision: hardware-relative at the chip output: one FM channel is about -26 dBFS RMS.
Each factory preset carries a `preset_gain` (see "Presets: playing level (preset_gain)")
applied after the chip; `master_gain` stays the user's control.
Alternative: normalise each chip.

## Plugin layer and presets

### Plugin: sustain pedal and the arpeggiator [plugin.md]
Ambiguity: taken literally the pedal holds every arpeggiator note-off.
Sources: docs/PLUGIN_SPECS.md.
Decision: the pedal holds played notes only; arpeggiator note-offs always apply.
Alternative: CC 64 as a second arpeggiator Hold.

### Plugin: event order at one sample offset [plugin.md]
Ambiguity: order of controls and notes at the same offset.
Sources: docs/PLUGIN_SPECS.md.
Decision: MidiBuffer order; with the arpeggiator running, controls first.
Alternative: controls always first.

### Plugin: event capacity [plugin.md]
Ambiguity: fixed engine event lists (64 notes, 256 controls).
Sources: docs/PLUGIN_SPECS.md.
Decision: a slice ends when a list is full; excess events move one sample later; nothing
is dropped (48 input notes per slice).
Alternative: coalesce controller values.

### Plugin: Channel Mode messages [plugin.md]
Ambiguity: scope of CC 120/121/123 in MIDI channel mode.
Sources: MIDI 1.0 specification (from general knowledge, not re-read; to check).
Decision: each acts on its own channel; CC 120 resets the engine when no other channel
holds a note.
Alternative: global effect.

### Plugin: chip default of poly_channels [plugin.md]
Ambiguity: one host parameter cannot hold three chip defaults.
Sources: docs/PLUGIN_SPECS.md.
Decision: 0 means "chip default mask", resolved on the audio thread at each switch. The
editor's toggles refuse an empty mask and write a mask equal to the chip default as 0.
Alternative: one mask parameter per chip.

### Plugin: second chip switch during a crossfade [plugin.md]
Ambiguity: not covered by the 20 ms crossfade rule.
Sources: docs/PLUGIN_SPECS.md.
Decision: the new switch waits for the running fade (at most 20 ms plus one block).
Alternative: restart the fade (three engines audible).

### Plugin: diagnostics frame time [plugin.md]
Ambiguity: interval between frames or work per frame.
Sources: docs/PLUGIN_SPECS.md.
Decision: work per frame (vblank callback plus paints since the previous vblank).
Alternative: the interval (already shown as the refresh rate).

### Plugin: blinking caret [plugin.md]
Ambiguity: JUCE's caret timer repaints at rest.
Sources: JUCE 9.0.3 `CaretComponent`.
Decision: a caret that does not blink.
Alternative: accept the caret repaints.

### Plugin: encoder parameters at sample load [integration]
Ambiguity: the encoders read parameters (NES `dmc_rate` / `clock`, Genesis `dac_rate`, SNES
`echo_delay` budget) that the audio thread forwards only at its next block, so a preset's
samples were encoded with the previous preset's values.
Sources: docs/ARCHITECTURE.md (engine contract).
Decision: `IChipEngine::stageParameter()` (message thread) stores the parameter atomic
before every load; NES stores the atomic only, SNES and Genesis call `setParameter()`.
Alternative: defer the load until the audio thread has applied the parameters.

### Plugin: samples of a fresh instance [integration]
Ambiguity: before any preset the SNES voices, NES DMC and Genesis DAC have no sample, so
a fresh instance is silent on them.
Sources: docs/PLUGIN_SPECS.md ("Start-up samples").
Decision: each chip's default slot receives the sample of the first factory preset that
writes it, else the chip's first sample (SNES `bass_finger`, NES `bass_pluck`, Genesis
`clap`). They are factory samples, not saved in the state.
Alternative: empty slots until a preset is applied.

### Plugin: SNES echo delay display [integration]
Ambiguity: EDL 0 is not 0 ms; the buffer is 4 bytes, one stereo sample.
Sources: fullsnes, Anomie (EDL x 2 KiB, EDL 0 = 4 bytes).
Decision: host and panel text show EDL x 16 ms; EDL 0 reads "0.03 ms".
Alternative: show the raw EDL value.

### Presets: perceptual QA method [integration]
Ambiguity: how to decide that two generated presets sound the same.
Sources: docs/PRESET_SPECS.md, `tools/presetgen/qa.py`, docs/PRESET_QA.md.
Decision: features rendered by `chiptool features` through the real engines (two passes
after a pre-roll: C4 held, C3 short; stereo log-mel on active frames, 10 ms envelope, FFT
pitch track); thresholds at or below the usual JNDs; different globals and render-blind
keys never merge presets.
Alternative: parameter rules only (kept as the first stage).

### Presets: Genesis FM level target [integration, genesis-sound-design Decision 1]
Ambiguity: a single-note peak of -12..-3 dBFS cannot be reached by one FM voice without
breaking the hardware-relative mix (six full-scale FM channels map to 1.0, so one channel
tops out near -15.6 dBFS).
Sources: `docs/research/genesis-sound-design.md`, "Genesis: output level" above.
Decision: the loudest carrier sits at TL 0-10 (with a headroom rule on multi-carrier
algorithms so the 9-bit channel sum does not clamp); the preset-managed `preset_gain`
(2026-09-30) brings the preset to the common playing level after the chip.
Alternative: a different `master_gain` default (plugin decision).

### Presets: playing level (preset_gain) [integration, plugin.md]
Ambiguity: single voices came out at about -30 dBFS RMS (Genesis PSG about -45) because the
mix is hardware-relative; "one common playing level" needs a measure of a preset's level
and a rule for sounds that decay during the hold.
Sources: product-owner decision 2026-09-30; docs/PRESET_SPECS.md ("Playing level").
Decision: a preset-managed global `preset_gain` (-24..+36 dB, default 0, no panel control)
multiplies the main and channel outputs after the chip, in the same ramp as `master_gain`,
so channel ratios and the NES mixer curve are untouched. The generator sets it from the
`chiptool features` renders: stereo RMS of the long pass at velocity 100 (C4, held 1.4 s)
over the 10 ms windows of the hold within 20 dB of the loudest window (the part where the
note sounds) to -18 dBFS; peak of either channel over the long and short passes at
velocity 100 and at velocity 127 at or below -1 dBFS (the ceiling promised to the product
owner holds at full velocity); rounded to 0.5 dB. Results on 2026-09-30: NES +7.0..+27.0 dB
(median +15.0), SNES -11.5..+9.5 (median -0.5), Genesis +2.5..+36.0 (median +12.5);
260 presets are limited by the peak ceiling (NES 90, SNES 78, Genesis 92), 17 Genesis PSG
presets stop at the +36 dB bound (3.3 to 10.8 dB below the target). The velocity-127 peak
is a median 1.6 / 2.1 / 3.6 dB above the velocity-100 one (NES / SNES / Genesis, max 2.3 /
3.3 / 6.0 dB), so peak-limited presets play their velocity-100 note that much below
-1 dBFS. Level variants ("Level Soft", ghost notes) now play at the common level unless the
peak or the bound limits them.
Alternative: RMS over the whole hold (percussive presets would all sit at the peak
ceiling), or loudness weighting (ITU-R BS.1770) instead of plain RMS.

### Presets: Genesis white noise on the tone-3 clock [integration, genesis-sound-design Decision 4]
Ambiguity: the old seed assumed that white noise on rate 3 shifts at 16 times the key
frequency; the driver adds 48 semitones only in periodic mode, so white noise shifts at the
key frequency (262 Hz at C4) and the old "Pitched Snare" was a -50 dBFS rumble.
Sources: `docs/research/genesis.md` "Driver tick model".
Decision: the driver is unchanged; the seed became "Pitched Noise" with `psg_transpose`
+24 or +12. The LFSR outputs nothing for its first 15 shifts, so at the lowest keys the
note starts late (kept: hardware behaviour).
Alternative: a driver offset for white noise too (would change the documented driver).

### Presets: categories of leads, guitars and organs [integration]
Ambiguity: `docs/PRESET_SPECS.md` has no Genesis lead or organ category.
Sources: `docs/PRESET_SPECS.md`, `docs/research/genesis-sound-design.md` "Category mapping".
Decision: taxonomy unchanged: FM leads and guitars in `FM Brass` / `Brass` (tag `lead`,
`guitar`), wind leads in `FM Pad` / `LFO`, organs in `FM Pad` (tag `organ`); the browser
search finds them by name, category and tag.
Alternative: new `FM Lead` / `FM Organ` categories (a product decision, open).
