# Genesis preset sound design

Design notes for the Genesis seed patches in `tools/presetgen/seeds/genesis.py` (bank
`assets/presets/genesis.json`, 393 presets from 100 seeds, generated on 2026-09-30). The goal
is the sound of classic Mega Drive / Genesis music with the hardware limits kept. The patches
were designed from the public references below and checked with renders through the real
engine. Nobody listened to them (see "Verification").

No patch was copied from a game, a ROM, a soundtrack or an FM patch collection ripped from
games (TFI/DMP/VGI megapacks and similar). No emulator source code was read.

## References

All consulted on 2026-09-30.

| # | Source | URL | What was used |
|---|---|---|---|
| 1 | nesdoug, "Sega Genesis FM Instrument Basics" (2022-02-04) | https://nesdoug.com/2022/02/04/sega-genesis-fm-instrument-basics/ | Algorithm 4 as the general-purpose pair layout, algorithm 7 for organ-like additive sounds, algorithm 0 for the noisiest sounds. TL is logarithmic and the modulator level sets the harmonic content. High feedback turns noisy. A slow modulator attack gives a "wah". Detune on carriers gives chorus. Clipping identical carriers at full level shapes the wave. No follow-up article of the series was found. |
| 2 | Sega Genesis Technical Manual, YM2612 section (transcribed and corrected by Maxim, SMS Power) | https://www.smspower.org/maxim/Documents/YM2612 | Per-algorithm instrument suggestions (0: distortion guitar, bass; 1: harp; 2: bass, electric guitar, brass, piano, woods; 3: strings, folk guitar, chimes; 4: flute, bells, chorus, drums; 5: brass, organ; 6: xylophone, tom, organ, vibraphone; 7: pipe organ). "To make a note softer, only change the TL of the slots." TL 0.75 dB per step. The layout of the manual's "Grand Piano" test program: alg 2, feedback 3, carrier TL 0, modulators TL 35-45, one high-MUL modulator. Only the layout was used, not its register values. AMS/FMS depths. The manual says to leave SSG-EG at 0; the SSG presets deliberately ignore that advice, following docs/research/genesis.md "SSG-EG". |
| 3 | Plutiedev, "YM2612 register reference" | https://www.plutiedev.com/ym2612-registers | Output operators per algorithm (0-3: S4; 4: S2 S4; 5-6: S2 S3 S4; 7: all). Rate scaling "steeper at higher frequencies" to imitate acoustic instruments. SSG-EG needs AR 31. Volume is set through the output operators' TL. |
| 4 | Plutiedev, "Common YM2612 operations" | https://plutiedev.com/ym2612-operations | No per-channel volume register: loudness is the carriers' TL (the driver's velocity follows this). |
| 5 | Furnace user manual, "FM (OPN) instrument editor" | https://tildearrow.org/furnace/doc/latest/4-instrument/fm-opn.html | Parameter definitions only (AR/DR/SL/D2R/RR, MULT, DT, AM). It gives no sound-design guidance. This is the documentation page, not the program source. |
| 6 | SpritesMind forum, "How to make ym2612 instruments?" | http://gendev.spritesmind.net/forum/viewtopic.php?t=1201 | General advice only: learn the 8 algorithms and feedback, and envelope plus frequency ratios set the timbre. The patch libraries suggested in the thread were not used. |
| 7 | J. Chowning, "The Synthesis of Complex Audio Spectra by Means of Frequency Modulation", JAES 21(7), 1973 | https://ccrma.stanford.edu/sites/default/files/user/jc/fm_synthesis_paper.pdf | Brass premises: harmonic spectrum, odd and even harmonics, higher harmonics growing with intensity, a fast rise that may overshoot. Woodwinds: c:m = 3:1 (bassoon 5:1); clarinet c:m = 3:2, which gives odd harmonics, with the index inversely proportional to the amplitude. Bells: c:m = 1:1.4 (inharmonic) with the index proportional to a long exponential amplitude envelope. Drum and wood drum: the same ratio, short, with a burst of index that collapses. Read as text extracted from the PDF. |
| 8 | G. Reid, "Synth Secrets 13: More On Frequency Modulation", Sound On Sound | https://www.soundonsound.com/techniques/more-frequency-modulation | Integer ratios give harmonic spectra: 1:1 is saw-like, 1:2 gives odd harmonics only (square-like), 1:3 and 1:4 are pulse-like. Non-integer ratios give inharmonic spectra. The index sets brightness. |
| 9 | Project notes `docs/research/genesis.md` | (this repository) | Envelope durations by rate (rate 63 = 7.21 ms, 48 = 57.6 ms, 32 = 921 ms for the full range, doubling every 4 rates), key-code rate scaling, 9-bit channel clamp, DT in Hz, LFO/AMS/FMS values, output scale, PSG level, driver unison, vibrato and noise-channel behaviour. |

## Principles applied

1. **Carriers set the level, modulators set the brightness** (refs 2, 3, 4). In every seed
   the loudest carrier is at TL 0-10. Modulators sit at TL 16-56 depending on the brightness
   wanted (bass and brass about 18-40, soft sounds 40-56). Velocity only moves the carriers
   (driver rule), so it changes loudness, not timbre.
2. **Headroom under the channel clamp.** Carriers are truncated to 9 bits and summed into a
   9-bit accumulator clamped to -256..+255 (docs/research/genesis.md, "Channel accumulation and
   clamp"). With several carriers, the linear sum of their levels (10^(-0.75 TL / 20)) is kept
   at or below about 1.0 for carriers at the same MUL, which peak together, and about 1.2 for
   different MULs, whose peaks rarely coincide. Multi-carrier patches therefore use pairs like
   TL 8 + 8 or 6 + 8, and three or four carriers at TL 8-18. The only deliberate clipping source
   left is feedback 7 (Dist Bass, Dist Guitar).
3. **Ratios.** 1:1 for saw-like sounds (basses, brass, strings), 1:2 for odd harmonics (Square
   Bass, Pulse Lead, Clarinet Lead), 1:3 for a pulse-like second voice (ref 8). Bells and
   mallets need non-integer carrier:modulator ratios, but MUL is an integer. They are built as
   pairs over a MUL 2 or MUL 4 carrier (7:2 = 3.5, 11:4 = 2.75, 4:1 for tuned vibraphone and
   marimba bars), and DT supplies the true inharmonicity: DT is a fixed offset in Hz, so it
   beats.
4. **Index envelopes after Chowning (ref 7).**
   * Brass: the modulators attack slightly slower than the carrier (Trumpet AR 20-22 against
     26), so the tone brightens as it grows, and the carrier has SL 1 for a small overshoot.
   * Woodwind (Clarinet Lead): the modulators start at full index and fall 12 dB while the
     carriers rise, so the spectrum narrows as the note gets louder.
   * Bells: the modulators decay with the carriers (SL 15 everywhere), so the spectrum goes
     from dense to simple.
   * Marimba and xylophone: a modulation burst that collapses in about 50 ms (DR 20-22,
     SL 15) onto a nearly pure fundamental.
   * Plucked and struck keys: a bright modulator that dies much faster than its carrier
     (the EP tine at MUL 14, the clav, the pick click at MUL 3).
5. **Feedback** on S1 is the saw source (ref 1): 5-7 for basses and distortion, 3-6 for
   brass, 0-2 for bells and organs (pure sines). An axis varies feedback only where S1's
   index is high enough to hear it. The rendered-feature QA removed axes where it was not.
6. **Rate scaling** RS 1 on basses, brass and leads, and RS 2 on percussive keys, so high notes
   decay faster, as on acoustic instruments (refs 2, 3). RS 0 on pads and organs keeps their
   envelopes the same across the keyboard.
7. **Algorithm per family** follows the Sega manual's suggestions (ref 2) where they fit:
   * alg 0: distortion basses and guitar;
   * alg 2: trumpet, brass stabs, the FM piano, the clean guitar;
   * alg 3: slap and wah basses, synth brass;
   * alg 4: bells, flute, EPs, clavs, stacked basses;
   * alg 5: power stab, pads, the tubular bell;
   * alg 6: vibraphone, marimba, xylophone;
   * alg 7: pipe and drawbar organs, the glockenspiel, the music box.
8. **Unison is centred.** The driver's unison detunes the partner channel upwards by
   `unison_detune` fnum units (1.4-2.8 cents each across an octave), so the pair's centre sits
   about `unison_detune` cents sharp. Every unison variant sets `fine_tune = -unison_detune`.
   Averaged over the held note, the pair is now within 6 cents of the key from E1 to C6. The
   residual swings between about -5 and +2 cents because a negative fine_tune moves C notes
   into the block below, where one fnum unit is half as many cents. The previous bank's wide
   unison basses measured 16-24 cents sharp.
9. **Console.** The default is the discrete YM2612 of a Model 1: ladder effect, 3.39 kHz
   output low-pass (`chip_revision` 0, `model1_lowpass` 1). 17 presets use the YM3438/ASIC with
   the low-pass off, through the chip axis on Synth Bass, Tine EPiano and FM Piano, and the
   console axis on the bright DAC samples.
10. **Vibrato** comes either from the driver (delayed, software, 20-30 frames of delay, 3-6 fnum
    units) or from the hardware LFO (starts with the note; FMS 1-5). Tremolo uses AM on the
    carriers; the Sweep Pad puts AM on the modulator to sweep the timbre.

## Category mapping

The taxonomy of `docs/PRESET_SPECS.md` is kept unchanged. Timbres that have no category of
their own are placed as follows and are found by name and tag (the plugin browser searches
both):

* FM leads (Saw Lead, Pulse Lead) and guitars (Dist Guitar, Clean Guitar): `FM Brass` /
  `Brass`, tagged `lead` (and `guitar`).
* Wind leads (Flute Lead, Clarinet Lead): `FM Pad` / `LFO`, since their character comes from
  the vibrato. Tagged `lead`.
* Organs: `FM Pad` / `Slow` (Pipe Organ), `Unison` (Drawbar Organ) and `LFO` (Rotary Organ),
  tagged `organ`.
* Strings: `FM Pad` / `Slow` (Slow Strings) and `Unison` (String Ensemble).
* Piano, harpsichord and clav: `FM Keys` / `EPiano`. Marimba and xylophone: `FM Keys` / `Bell`.

Alternative, not taken: add `FM Lead` and `FM Organ` categories to `docs/PRESET_SPECS.md` and
`tools/presetgen/model.py`. That is a product decision (see open points).

## Target sound per category

### FM Bass (58 presets)

Punchy, in-tune bass with a fast attack and a clean stop (RR 8-9).
* Alg: Slap (MUL 3 thumb pop over a feedback buzz), Pick (MUL 3 click, muted or ringing),
  Synth (serial saw with a filter-like closing modulator, YM2612/YM3438), Square (1:2 odd
  harmonics), Sub (nearly pure, held or plucked).
* Feedback: Growl (feedback 4/6/7), Dist (the manual's distortion algorithm at feedback 7),
  Wah (slow modulator attack).
* Detune: Chorus (DT pair, or centred unison), Fat (centred unison 8/14), Octave (an upper
  octave layer).

### FM Keys (58 presets)

* EPiano: Tine EPiano (MUL 14 tine, DX-style), Soft EPiano (tremolo), Wurli (bark), Clav
  (bite and damping), FM Piano (the manual's piano layout), Harpsichord (8' + 4').
* Bell: Glock, Tubular Bell, Chime, Vibraphone (motor tremolo), Marimba, Xylophone, Music
  Box, Temple Bell. All ring to silence with inharmonic or tuned-bar partials.
  * Glock and Music Box sound one or two octaves above the key, as the instruments do (their
    fundamental is a MUL 2 partial).
  * Temple Bell has its hum tone an octave below the key.

### FM Brass (60 presets)

* Stab: short hits that decay by themselves (Brass, Orch, Funk, Power).
* Brass: Trumpet (Chowning brass, three vibrato choices), Horn Section (two detuned players,
  instant or swelled), Synth Brass (slow modulators), Tuba. Leads: Saw Lead, Pulse Lead, Dist
  Guitar, Clean Guitar.
* SSG: Buzz, Growl, Sync and Swell horns (SSG-EG envelopes on a modulator, rates following
  the key).

### FM Pad (64 presets)

* Slow: fade-in pads (Warm, Glass, Choir, Dark), Slow Strings (bowed brightening, optional
  LFO vibrato), Pipe Organ.
* Unison: Super Pad, String Ensemble, Chorus Pad, Drawbar Organ. All unison is centred;
  unison costs half the polyphony.
* LFO: Vibrato, Tremolo and Sweep pads, Rotary Organ, Flute Lead, Clarinet Lead.
* The Dark Pad's S4 carrier at x0.5 puts its fundamental an octave below the key.

### DAC (46 presets)

The CC0 drum kit (VSCO 2 CE / VCSL, `assets/samples/index.json`) at the classic 8-22 kHz
driver rates, plus the keyed tom, orchestra hit, noise burst and voice. Unchanged from the
previous bank.

### PSG Lead, PSG Bass, PSG Drums (38 + 33 + 36 presets)

SN76489 squares and noise at their hardware level: one tone at attenuation 0 is 16.1 dB below
one FM channel at full scale, so PSG presets play 16-22 dB below FM presets, as on the console.
The existing designs were kept, with these changes:
* The doubled leads' detune was narrowed to 1-3 period units (the Fat Lead keeps 5/7 as an
  effect).
* The Detuned Bass uses 3/5 units.
* "Zap" was removed: its axis varied the software vibrato, which the driver never applies to
  the noise channel, so both variants were identical.
* "Pitched Snare" was redesigned as "Pitched Noise" (see "Decisions" 4).

## Verification

Renders at 48 kHz through `chiptool render` (engine built from the current working tree) at
velocity 100 and 127 and at keys 28-96, analysed with a standard-library script
(peak, 10 ms RMS envelope, YIN pitch). The script was a scratch tool and is not committed.
Then the repository pipeline ran: candidate bank, `chiptool features`, perceptual QA.

* No preset is silent at its intended keys, and none clips: the highest peak over all renders
  is -11.4 dBFS.
* Level at C4, velocity 100, peak dBFS (min / median / max):

  | Category | Min | Median | Max |
  |---|---|---|---|
  | FM Bass | -19.8 | -18.0 | -12.3 |
  | FM Brass | -20.1 | -19.1 | -12.8 |
  | FM Keys | -24.5 | -20.8 | -19.1 |
  | FM Pad | -22.3 | -18.4 | -12.7 |
  | DAC | -23.6 | -18.6 | -17.4 |
  | PSG Lead | -47.8 | -37.8 | -31.8 |
  | PSG Bass | -45.8 | -37.8 | -31.8 |
  | PSG Drums | -46.1 | -38.0 | -37.6 |

  At velocity 127, single-channel FM presets peak between -20.1 and -13.7 dBFS (median -15.5).
  Unison presets, which use two channels, peak between -15.6 and -9.3 dBFS. Peaks can exceed
  the nominal -15.6 dBFS of one full-scale channel because the DC blocker and the output filter
  change the peak of asymmetric feedback waveforms.
* Pitch over each playing range (basses E1-C4, others C3-C6):
  * 97 % of the non-unison FM measurements are within 3 cents of the key. The worst is
    Wah Bass at E1, 13.8 cents, a low-confidence estimate while its modulator is still
    sweeping.
  * Unison presets, averaged over the held note, are within 6 cents. Single-window estimates
    near the attack read up to 10 cents.
  * Not checked: keys that fall below E0 after an octave-down transpose, and the Orch Stab
    tail after it has decayed.

  Exceptions:
  * inharmonic bells, SSG and feedback-7 presets, where pitch is not defined;
  * presets with vibrato, which the analysis measures mid-swing;
  * octave offsets by design (Glock, Music Box, Temple Bell, Dark Pad).
* PSG pitch limits kept (hardware, not fixable in a preset):
  * Tone basses play from A1 (psg_transpose +12, period 0x3FF = A2).
  * Periodic-noise basses are within 16 cents up to about C3. Above that, the tone-3 period
    for 16x the key is small and its quantisation gives errors of -18 to +48 cents at C4-C5.
  * PSG unison detunes the partner downwards and there is no PSG fine tune. Doubled leads at
    3 units measure about 25 cents flat at C6, and the Fat Lead 15-55 cents flat from C4 to C6.
* Perceptual QA: 393 expanded presets, 0 duplicates by parameters or rendered features.
  Target 300-400.
* Determinism: `chiptool features` twice and `gen_presets.py` with the default pool and with
  `--jobs 1` gave byte-identical files.

## Decisions

1. **Level target.** The requested "single note peak between -12 and -3 dBFS after the chip's
   own mix" cannot be met by a single FM voice without breaking the hardware-relative mix. The
   engine maps six full-scale FM channels to 1.0 (`kOutputScale`), so one channel tops out
   near -15.6 dBFS, and the README states that levels are hardware-relative and compensated by
   `master_gain`. Decision: the loudest carrier is at TL 0-10 and the level is kept near the
   one-channel ceiling (table above). Alternatives:
   * a preset-managed output gain (this changes the plugin contract);
   * a different `master_gain` default (a plugin decision).
2. **Categories.** The taxonomy is unchanged; leads, guitars and organs are placed as described
   in "Category mapping".
3. **Headroom rule** (principle 2) instead of TL 0 on every carrier of the multi-carrier
   algorithms, which would hit the 9-bit channel clamp.
4. **Noise on the tone-3 clock.** The previous seed docstring said white noise on rate 3 shifts
   at 16x the key. The driver (docs/research/genesis.md, "Driver tick model") adds 48 semitones
   only in periodic mode, so white noise shifts at the key frequency (262 Hz at C4). The old
   "Pitched Snare" was a -50 dBFS rumble at C4 and silent below C3. The docstring was
   corrected. The seed became "Pitched Noise" with psg_transpose +24 (keys C5-C6: 2.1-4.2 kHz
   shift rate) or +12. The LFSR still outputs nothing for its first 15 shifts after a note, so
   at the lowest keys the note starts late: at key E1 with +12 and an 8-frame decay it stays
   silent.

## Open points

* Nobody has listened to the bank. All checks are objective renders; a listening pass on real
  material is still needed, especially for the bells, guitars and the SSG presets.
* A dedicated `FM Lead` / `FM Organ` category would make those timbres easier to browse.
* The -12..-3 dBFS single-note target needs a plugin-level decision (Decision 1).
