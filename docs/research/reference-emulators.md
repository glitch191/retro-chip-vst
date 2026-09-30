# Reference emulators for the differential check

Product-owner decision of 2026-09-29: a mature reference emulator binary may be downloaded
and **run** to compare audio output; its source code is never read, copied or imitated.
Everything below was used as a black box: our own stimuli in, WAV out. Binaries live in
`third_party/refemu/` (git-ignored) and are never redistributed.

The harness: `tools/refcheck/make_stimuli.py` (stimuli), `chiptool regs` (our side),
`tools/refcheck/compare.py` (renders both sides and analyses them). Results:
`docs/research/refcheck-report.md` (generated) and `docs/research/refcheck-diagnosis.md`.

## Genesis: VGMPlay 0.40.9 (legacy)

| Item | Value |
|---|---|
| Tool | VGMPlay 0.40.9, Windows build (`VGMPlay.exe`, `zlib1.dll`, `VGMPlay.ini`) |
| URL | https://github.com/vgmrips/vgmplay-legacy/releases/download/0.40.9/VGMPlay_040-9.7z (release published 2018-12-25) |
| Consulted | 2026-09-29 |
| SHA-256 | archive `2edbd47b40257d6276ec92396233c9debc8bfbd492e8c07e5a23063b37a16417`, `VGMPlay.exe` `10a4b7dab92de0315b181cbdffe081f8586705bd34467592e5f425a693be88e9` |
| Licence | The GitHub repository declares no licence file (API `license: null`); the program bundles emulation cores under their own licences (MAME-derived cores, Nuked OPN2 is LGPL-2.1 per its project page). Used only as an executable, nothing is redistributed. |
| Input | VGM 1.51 (public format, https://vgmrips.net/wiki/VGM_Specification) |
| Headless use | `LogSound = 1` in `VGMPlay.ini` ("log only": renders silently to `<file>.wav` next to the VGM and exits at the end of the file). Documented in the bundled `VGMPlay.txt` / `VGMPlay.ini` comments, which are user documentation, not source. |
| Settings (both) | `SampleRate = 44100`, `ResamplingMode = 0`, `FadeTime = 0`, `PseudoStereo = False`, `Volume = 1.0` |
| Configuration `nuked` (primary) | `[YM2612] EmulatorType = 1` (Nuked OPN2), `NukedType = 3` ("YM2612", discrete chip, no Mega Drive Model 1 filter simulation); `[SN76496] EmulatorType = 0` (MAME core); `ChipSmplMode = 2`, `ChipSmplRate = 384000` (see below) |
| Configuration `mame` (second opinion) | `[YM2612] EmulatorType = 0` (MAME core, "Genesis Plus GX" fixes per the ini comment); `[SN76496] EmulatorType = 1` (Maxim core); `ChipSmplMode = 0` (native chip rate) |

Why these: VGMPlay is the reference player of the VGM format, runs headless on Windows
and offers the die-shot based Nuked OPN2 core (the most accurate public YM2612 model) plus
a second, independent YM2612 core, which lets the report separate "ours differs from the
hardware" from "the references disagree". libvgm's `vgm2wav` and vgmplay-libvgm publish no
Windows binaries (checked on GitHub 2026-09-29), and Furnace cannot import VGM files.

Known behaviour of the reference (measured, not read from source):

* The Nuked OPN2 output runs fast by about 90000 / ChipSmplRate cents: +1.69 cents with
  `ChipSmplMode = 0` (native) and with `ChipSmplRate = 53267`, +1.55 at 44100, +0.85 at
  106534, +0.56 at 159801, +0.45 at 96000..192000, +0.23 at 384000 (the highest rate the
  player accepts; 532670 and 1065340 give the same +0.23). Measured on fm_sine_ref: expected
  440.126 Hz from the public formula (the phase increment is an exact integer there), which
  the MAME core and our core both give. A pitch error that changes with the player's chip
  sample rate is an integration artefact of the player, not chip behaviour. The register
  writes themselves are timed correctly: the DAC stimulus (pitch set by the write timing)
  has no offset, and neither have the SN76496 cores in the same instance. compare.py
  therefore runs Nuked at 384 kHz, measures the remaining factor on fm_sine_ref and
  resamples the Nuked YM2612 renders by it (the DAC stimulus excepted). The resampling also
  delays the Nuked event times by t x 1.3e-4 (0.17 ms at 1.25 s), which is why release
  crossings after 1 s show sub-millisecond offsets against Nuked only.
* The YM2612 output is not DC-blocked (the ladder-effect idle offset appears as a step at
  power-on); compare.py zeroes the silent lead-in and gives the reference the 5 Hz coupling
  capacitor our output path has, so DC steps take the same path on both sides.
* The player's resampler droops at the top of the band (a square wave's harmonics fall
  below 1/k by 0.9 dB at 11 kHz at 44.1 kHz output) and treats content above the output
  Nyquist differently from our band-limited steps (the YM2612's 26.6 kHz chatter at high
  feedback). compare.py band-limits both sides to 15 kHz for levels and nulls.
* The MAME YM2612 core has no ladder effect (its odd harmonics of a pure sine are 35 dB below
  Nuked's and ours, and quiet notes are 2 to 43 dB quieter than on Nuked), and its attack
  curve is slower at the start (fm_ar08..fm_ar20: -20 dB reached 1.5 to 2 times earlier
  than Nuked and us). Where MAME and Nuked disagree, Nuked is taken as the reference.
* The Maxim SN76489 core outputs white noise 6 dB below the MAME core (and below ours) and
  runs tone-3-driven noise at period + 1 (psg_noise_p3: 107.56 Hz instead of 109.24 Hz; the
  MAME core and ours follow the datasheet rate clock / (32 x period) / 16).

## SNES: Game Music Emu through FFmpeg 9.0.2

| Item | Value |
|---|---|
| Tool | FFmpeg 9.0.2 "full_build-shared" by gyan.dev, `libgme` demuxer (Game Music Emu SPC player, statically inside `avformat-63.dll`) |
| URL | https://www.gyan.dev/ffmpeg/builds/packages/ffmpeg-9.0.2-full_build-shared.7z (linked from https://www.gyan.dev/ffmpeg/builds/) |
| Consulted | 2026-09-29 |
| SHA-256 | archive `4d2060a8b34a940aa47d785142055bb92a63053781e55f2ace4546edd519a8f5` |
| Licence | FFmpeg build: GPL v3 (build README); Game Music Emu: LGPL-2.1 per its project page. Used only as an executable, nothing is redistributed. |
| libgme version | not reported by the build (`--enable-libgme`, version unknown) |
| Input | SPC file (v0.30 layout: header, ID666 text tag, 64 KiB RAM, 128 DSP registers, 64 extra bytes; described on SNESdev wiki "SPC file format") |
| Command | `ffmpeg -f libgme -sample_rate 32000 -i <stim>.spc -t <seconds> -c:a pcm_s16le <out>.wav` (native 32 kHz: no resampling on either side) |

Why this: a headless SPC renderer runnable on Windows without building anything. Other
SPC players checked (SNES SPC700 Player, foobar2000 with foo_gep) are GUI-only.

Known behaviour of the reference (measured):

* Output gain +2.91 dB relative to our raw S-DSP output (a player volume choice) and
  inverted polarity relative to ours (our main output applies the documented final
  inversion, research snes.md "Output mixing"). compare.py uses one gain constant and the
  polarity found by cross-correlation.
* The player clips at 16 bits after its gain: a DSP output above -2.9 dBFS clips in the
  reference only. The VOL stimuli use MVOL 0x50 to stay below.
* The player skips leading silence at track start: a stimulus whose voice stays silent for
  about 0.1 s after key-on (GAIN direct 0) had its whole reference time line moved earlier.
  The GAIN increase stimuli therefore start from direct 0x10.
* The SPC format stores neither the DSP's global rate counter nor the noise LFSR, so the
  counter phase at key-on differs between the two sides: ADSR/GAIN stimuli null at 44 to
  61 dB instead of 72 to 81 dB for static ones, and noise stimuli align per stimulus.
* It starts the SPC700 program from the file's register snapshot like hardware after a
  state load; our side has no SPC700 and replays the event list the generator emits, which
  gives a constant 9-sample offset (the program's start-up and KON poll); alignment removes it.

## What was not used

* No game, ROM, soundtrack, VGM/SPC rip or FM patch collection: every stimulus is generated
  by `tools/refcheck/make_stimuli.py` from our own register writes, BRR data and SPC700
  program.
* No reference source code was opened, and no reference output is committed (renders go to
  `build-reports/refcheck/`, git-ignored).
