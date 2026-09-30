# SNES preset sound design

Design notes for the SNES seed patches in `tools/presetgen/seeds/snes.py` (bank
`assets/presets/snes.json`, 338 presets from 89 seeds, generated on 2026-09-30). The goal
is the sound of Nintendo- and Square-era SPC700 soundtracks with the hardware limits kept:
each instrument is one BRR sample plus the envelope, volume, pan, vibrato and echo settings
that a driver's instrument table and sequence commands give it. The patches were designed
from the public references below and checked with renders through the real engine. Nobody
listened to them (see "Verification").

No sample, instrument table or patch was taken from a game, a ROM or a soundtrack. The
default instrument table of Super Mario World that AddmusicK ships (`InstrumentData.asm`)
holds values extracted from the game, so it was treated like a patch collection ripped
from a game and not read. The SMW Central forum threads that discuss instrument ADSR
values (ref 9) could not be opened (the site returns an access notice to automated
clients), so no value from them was used. No emulator or driver source code was read.

## References

All consulted on 2026-09-30.

| # | Source | URL | What was used |
|---|---|---|---|
| 1 | SnesLab wiki, "N-SPC Engine" | https://sneslab.net/wiki/N-SPC_Engine | Instrument entry = 6 bytes: SRCN (or noise clock), ADSR1, ADSR2, GAIN, pitch multiplier and fraction. Vibrato command $E3 (delay, rate, depth), tremolo $EB, echo commands $F5 (EON, EVOL L/R) and $F7 (EDL, EFB, FIR set index). This is the model of `seed()`: one sample, one envelope, a tuning, a driver vibrato, the echo unit. |
| 2 | Super Famicom Development Wiki, "Nintendo Music Format (N-SPC)" | https://wiki.superfamicom.org/nintendo-music-format-(n-spc) | Same command set: "Set zz-sized vibrato at yy speed after xx time", echo filter "index for table, 0-3" (the four N-SPC FIR sets). |
| 3 | AddmusicK readme, "Syntax" | https://thelx5.github.io/AddMusicK/readme_files/syntax_reference.html | `#instruments` lines: sample, ADSR1 (>= $80 for ADSR, otherwise GAIN), ADSR2, GAIN, tuning multiplier and fraction. Vibrato `p` with an optional delay, a rate and an extent. |
| 4 | AddmusicK readme, "Hex Commands" | https://thelx5.github.io/AddMusicK/readme_files/hex_command_reference.html | $ED custom ADSR or GAIN per channel, $FA $01 GAIN, $F4 $09 restore the instrument, echo $EF (EON, EVOL L/R) and $F1 (EDL $00-$0F, feedback, FIR choice), vibrato $DE (delay, duration, amplitude). It confirms that a driver changes envelopes and echo per song and per channel, which is what the preset axes vary. |
| 5 | Super MIDI Pak documentation | https://www.supermidipak.com/doc/ | "A common echo setting to get started": EVOL L/R 64 (0x40), EDL 5, EFB 60 (0x3C), identity FIR. And: voices stay in use up to 8 ms after note off, because the APU has no programmable release (so a longer release has to be a GAIN write, as `release_mode` 1 does). |
| 6 | SnesLab wiki, "FIR Filter" | https://sneslab.net/wiki/FIR_Filter | The four N-SPC filters (identity, 3 kHz high-pass, 5 kHz low-pass, 1.5-8.5 kHz band-pass) with their gains, and the warning that too much feedback with a positive-gain filter makes the echo grow until it "explodes". Echo profiles use only these sets plus the official manual's low-pass (docs/research/snes.md "Echo"). |
| 7 | Wikipedia, "Vibrato" | https://en.wikipedia.org/wiki/Vibrato | Wind and bowed instruments use an extent under half a semitone either side, choirs under 10 cents; a vocal vibrato of 5 to 6.5 Hz. Used for the 5.4 and 5.95 Hz rates and the +/-8 to +/-14 cents depths. |
| 8 | OC ReMix forum, "Common ADSR envelopes" | https://ocremix.org/community/topic/16752-common-adsr-envelopes/ | General envelope shapes: piano instant attack, no sustain, long decay; flute medium attack and high sustain; percussion instant attack. |
| 9 | SMW Central, "The ADSR Thread (For AddmusicK)" and "Custom Instrument Thread" | https://www.smwcentral.net/?p=viewthread&t=73866 | Not readable (access notice). Nothing used. |
| 10 | Project notes `docs/research/snes.md` | (this repository) | Rate table and envelope steps (all times in the seed docstring were simulated from them and match the renders), pitch register and ceiling, Gaussian interpolation, echo buffer size, FIR sets and gains, EFB stability. |

No public source found lists the ADSR, GAIN or echo values of individual commercial
games in a form that is not extracted from the games themselves. The per-instrument values
below are therefore designed from the mechanics (refs 1-5, 10) and from the acoustic
envelope of each instrument (refs 7, 8, and the recordings themselves), not copied.

## Principles applied

1. **One instrument = one sample + one table entry** (refs 1-3). Every seed names one BRR
   sample and sets the whole register set: ADSR or GAIN, VxVOL and pan, transpose and fine
   tune (the driver's tuning bytes), driver vibrato, noise/PMON flags and the echo unit.
2. **Envelope families.**
   * Plucked and struck (pianos, guitars, pizzicato, harp, mallets, basses, drums): A15.
     Pianos, guitars and mallets fall with D to a sustain level (SL4-SL6) and then fade on
     SR while the key is held (SR12-SR17: -30 dB in 0.9 to 2.9 s). Staccato basses decay to
     SL0/SL1 and end by themselves.
   * Sustained (strings, brass, winds, organs): SL7 SR0 or a small D/SL accent (brass D5 to
     SL6, oboe D3 to SL5), attack from the instrument (strings A9 64 ms or A7 160 ms, brass
     A10-A12, winds A7-A11, organs A14-A15), and a driver GAIN exponential release written
     at note off (organs R22-R26, 0.13-0.34 s to -40 dB; winds and brass R18-R21,
     0.4-0.85 s; strings R17-R19, 0.7-1 s). Hardware KOFF alone would stop every note in
     8 ms (ref 5).
   * Swells use GAIN bent-line increase (brass section rate 15, 0.28 s; horn rate 12,
     0.56 s; pads rate 4-12, 0.6-3.6 s), the curve ADSR cannot draw.
   * Drums keep A15 D7 SL7 so the recorded hit plays untouched; the variants shorten the tail
     (D5 SL0 SR24) or choke it (KOFF).
3. **Onset of the recordings.** The CC0 section recordings speak slowly (strings reach
   their level after about 200 ms, the brass section after 100-180 ms, the solo trumpet
   after 100 ms). A fast decay on them leaves only the quiet onset (-15 to -18 dBFS in the
   first renders), so the stab variants use D1-D2 (0.12-0.2 s) and the solo trumpet and the
   string section have no stab or spiccato preset.
4. **Pan.** Moderate, an orchestra seen from the audience, with the balance law of the driver
   (only the far side is attenuated): violins, harp, flute, piccolo, oboe and the nylon and
   muted guitars left (-8 to -20); cellos, horn, trombone, brass section, trumpet,
   clarinets and the steel guitar right (+8 to +20); pianos, organs, pads and basses in the
   centre. The drum kit keeps the drummer's-view panning of the previous bank.
5. **Echo.** On every melodic instrument and pad, off (EON 0) on drums and basses. Profiles:

   | Name | EDL | EFB | EVOL | FIR | Used by |
   |---|---|---|---|---|---|
   | Room | 3 (48 ms) | 0x28 | 0x28 | N-SPC low-pass 5 kHz | instrument variant, muted guitar |
   | (default) | 4 (64 ms) | 0x40 | 0x30 | N-SPC low-pass 5 kHz | instruments |
   | Deep | 6 (96 ms) | 0x58 | 0x30 | official manual low-pass | instrument variant, pipe organ, harp, vibraphone |
   | (default) | 5 (80 ms) | 0x50 | 0x30 | N-SPC low-pass 5 kHz | pads |
   | Deep | 6 (96 ms) | 0x60 | 0x34 | official manual low-pass | pad variant, dark choir |
   | Band | 5 (80 ms) | 0x48 | 0x30 | N-SPC band-pass | pad variant |

   Ref 5's starting point (EDL 5, EFB 0x3C, EVOL 0x40) sits inside this range. EFB x max|H|
   stays at or below 0.83 for every profile, so every tail decays (ref 6). SFX keep their
   longer effect echoes (EDL 8-12) from the previous bank.
6. **Vibrato.** 5.4 Hz (rate 23) or 5.95 Hz (rate 21), 6.9 Hz only for the organ's rotary
   shimmer, starting after 120 ms (winds, brass, electric piano), 160 ms (horn, steel
   guitar) or 200 ms (pads, choirs; 400 ms on Choir Oo); the vibraphone motor and the
   organ rotary start at once. Depth is given in cents at a reference key of the
   instrument's range and converted to pitch-register units with the sample's rate and
   root (`vib()`), so +/-10 cents means the same on a 16 kHz and a 32 kHz sample. Samples recorded with vibrato
   (string section, flute, trumpet) have driver vibrato off.
7. **Low-rate darkening for pads.** Low Strings, Cello Section and Dark Choir play their
   samples an octave down; the Gaussian interpolation, fixed relative to the sample rate,
   then rolls off the top of the spectrum, the dark string floor of SPC scores.
8. **Levels.** Sustained tones through an echo with feedback add up coherently on some
   notes, up to EVOL / (1 - EFB x |H|) above the dry level (in the worst case +5 dB for
   the default instrument echo, +7 dB for the pad echo and the instrument Deep, +9 dB for
   the pad Deep). Looped and sustained seeds therefore carry a lower VxVOL (full-scale
   single-cycle pads 60-64, most section pads 76-88) than plucked ones (100-127).

## Category targets

The category structure of `docs/PRESET_SPECS.md` is unchanged.

### Instrument (107 presets)

* **Piano (16).** Grand Piano: the loud Steinway recording, hammer then a 1.7 s string decay
  and a 0.25 s damper; Long for ballads (SR12). Upright Piano: softer and shorter, Bell for a
  music-box ping. EPiano: the TX81Z tine bark with an optional light chorus-like vibrato.
  Vibraphone: the FM piano with a motor-like vibrato from the start and a deep echo.
* **Strings (17).** Ensemble Strings (violins left, bowed A9 or Legato A7), Cello Section (an
  octave down, right), Pizzicato (the one-shot carries its own decay; an octave down for
  cellos and basses), Harp (pizzicato with a longer ring, far left, deep echo).
* **Brass (25).** Brass Section with a lip accent and delayed vibrato, Swell for the classic
  crescendo; Brass Stab for fanfare hits; Solo Trumpet (vibrato of the recording, Soft for
  legato lines); French Horn (soft attack, delayed vibrato, Swell); Trombone (Stab variant).
* **Guitar (10).** The procedural nylon, steel and muted guitars with pluck, finger-vibrato
  and room variants.
* **Woodwind (23).** Flute (breath onset, Soft), Piccolo (the flute an octave up), Clarinet
  (dry recording, optional vibrato), Low Clarinet, Oboe (reed accent, 5.4 or 5.95 Hz
  vibrato).
* **Organ (16).** Pipe Organ (4-foot recording, OctDown for 8-foot pitch, deep echo as the
  nave), Drawbar and Perc Organ (gate envelope, short GAIN release, rotary shimmer), Chip
  Organ (square loop).

### Bass (41 presets, dry)

* **Sample (26).** Upright Bass (the recorded contrabass pizzicato, new E2 take), Finger,
  Slap and Synth basses (procedural one-shots), Saw, Sine Sub and Triangle loops, each at
  key pitch and an octave down.
* **Short (15).** Staccato versions that end by themselves (SL0/SL1 with SR20-SR26), including
  a Short Upright.

### Pad (59 presets, always wet)

* **Echo (22).** Warm, Glass, Sine, Saw and Triangle pads and a Brass Pad (bent-line swell),
  with Swell, vibrato and EON Split (alternate voices dry) variants.
* **Strings (19).** Slow Strings (A5 or A3 bow), Swell Strings (bent-line GAIN), Low Strings
  (Gaussian-darkened octave down), Saw Strings.
* **Choir (18).** Choir Ah and Oo, Swell Choir (male voices an octave down), Dark Choir.

### Drums (73 presets, dry, one-shot)

* **Kit (59).** The CC0 kit (kick, snare, rim, hats, toms, clap, crash, ride, cowbell,
  shaker, timpani, conga, tambourine) with pitch, Short and Choke variants and the kit
  panning.
* **Velocity (14).** Accent (a semitone up, VxVOL 127) and ghost (two semitones down, short,
  VxVOL 72, 8 dB under the kit hit) layers for kick, snare, rim, hat, tom, timpani and
  conga.

### SFX (58 presets)

Unchanged in design from the previous bank apart from levels: hardware-noise bursts, wind,
explosions, steam, key-scaled noise hits and rain; PMON bells, wobbles, growls, trills, glass
and metal.

## Sample fixes found by the verification

The keyboard-wide pitch check (below) found three sample problems, fixed in
`tools/samplegen/cc0_manifest.json` and `tools/samplegen/cc0_import.py` (the other samples,
and every NES and Genesis sample, are byte-identical after regeneration):

1. **Pipe organ an octave sharp.** The VCSL file `Rode_Man3Open_A3.wav` is key A3 of an open
   4-foot stop: its spectrum has no energy at 220 Hz and a harmonic series on 440 Hz, so it
   sounds A4. The root note was A3, so every key played an octave high. Root and layer note
   are now 69 (sounding pitch); the Pipe Organ seed has an OctDown variant for the 8-foot
   pitch. See Ambiguity 25 in `docs/research/snes.md`.
2. **Loops that play off-key.** A loop of L frames can only contain partials at multiples of
   rate / L, so the sustained part plays at round(L / period) periods per loop whatever the
   recording did. The grand piano's 1664-frame loop held 18.14 periods of C4 (13 cents
   flat), the string section's 2560-frame loop 35.2 periods of A3 (10 cents flat), the
   flute's 4.3 cents flat. The loop search now only accepts lengths whose loop pitch is
   within 3 cents of the root (`max_detune_cents`); the new loops are exact to 0.1 cent
   (piano 15 periods in 1376 frames).
3. **Upright bass attack 60-80 cents flat.** The VSCO contrabass A1 take bends from -60 cents
   to +20 cents in its first 250 ms (checked on the source file, before conversion). It was
   replaced by the E1 v3 take of the same collection (git blob
   1cada4277b16b2eb8b240d4a6da4bbd3aca989a0, root E2), which is within +/-5 cents after its
   first 50 ms.

## Verification

All renders by `chiptool render` through the real engine (48 kHz, 0.4 s pre-roll, note held
60 % of the render), analysed with a standard-library script (scratch tool, not committed).

* **Level.** Every preset at C3, G3, C4, G4 and C5, velocity 100 and 127. At velocity 100
  the loudest peak of each preset is at most -3.0 dBFS and velocity 127 never clips (peak
  at most -0.5 dBFS). At C4, velocity 100, the peak is between -12.0 and -3.0 dBFS for 335
  of the 338 presets. The exceptions: Dynamic Hat Ghost Note (-13.0 dBFS) and Dynamic Conga
  Ghost Note (-12.7), quiet by design, and Shaker (-12.2): the stroke is soft and its
  content is above 5 kHz, where the Gaussian interpolation attenuates it; it is already at
  VxVOL 127. No preset is silent.
* **Pitch.** Every distinct (sample, transpose) pair with echo, vibrato and PMON off, at
  C2, C3, C4, C5 and C6, by autocorrelation on an 85 ms window after the attack. On the CC0
  samples the error has a median of 2.4 cents, 90 % of the readings are within 12.8 cents
  and the largest are 15 to 18 cents: the upright bass during its attack at C2, and the
  violin pizzicato, whose decay drifts 10 to 18 cents sharp after a pluck that is in tune
  (its energy-weighted pitch is +0.5 cents). The horn plays 12 cents flat for its first
  200 ms (lip attack of the recording) and in tune on the loop. Over its whole length, the
  energy-weighted pitch of every CC0 sample is within +/-10 cents of its root. The
  procedural samples have a median of 0.4 cents; the detuned glass pad reads up to 28
  cents because its chorus has no single pitch. The pitch ceiling is reached where
  documented (a 32 kHz A3-rooted sample above A5, the basses above A4). Octave readings on
  the organs and the synth bass come from their sub-octave components (16-foot drawbar,
  sub-octave square), fifth readings on the oboe from its dominant third harmonic; neither
  is a wrong root (the spectra were checked).
* **Envelope times.** Rise, decay and release times of the dry presets at C4 match the
  simulated hardware timings of the seed docstring within 10 ms (for example strings GAIN
  release 19: 0.69 s measured, 0.68 s simulated; pads release 15: 1.70 s and 1.69 s).
* **De-duplication.** `chiptool features` + `gen_presets.py`: 338 expanded, 0 parameter and
  0 perceptual duplicates, 338 kept (target 250-350). Two runs, one with `--jobs 1`, gave
  byte-identical banks.

Limits: nobody listened to the bank. The design follows documented driver practice and the
objective checks above, which cannot judge taste (whether the echo amount or a vibrato
depth is the most musical choice). The glass pad and the choirs are procedural and were not
re-tuned.
