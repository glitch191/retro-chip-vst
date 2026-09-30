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
| Settings | `SampleRate = 44100`, `ChipSmplMode = 0` (native chip rate), `ResamplingMode = 0`, `FadeTime = 0`, `PseudoStereo = False` |
| Configuration `nuked` (primary) | `[YM2612] EmulatorType = 1` (Nuked OPN2), `NukedType = 3` ("YM2612", discrete chip, no Mega Drive Model 1 filter simulation); `[SN76496] EmulatorType = 0` (MAME core) |
| Configuration `mame` (second opinion) | `[YM2612] EmulatorType = 0` (MAME core, "Genesis Plus GX" fixes per the ini comment); `[SN76496] EmulatorType = 1` (Maxim core) |

Why these: VGMPlay is the reference player of the VGM format, runs headless on Windows
and offers the die-shot based Nuked OPN2 core (the most accurate public YM2612 model) plus
a second, independent YM2612 core, which lets the report separate "ours differs from the
hardware" from "the references disagree". libvgm's `vgm2wav` and vgmplay-libvgm publish no
Windows binaries (checked on GitHub 2026-09-29), and Furnace cannot import VGM files.

Known behaviour of the reference (measured, not read from source):

* The Nuked OPN2 output runs fast by a factor that depends on VGMPlay's `ChipSmplRate`
  setting: +1.69 cents with `ChipSmplMode = 0` and with `ChipSmplRate = 53267`, +1.55 cents
  at 44100, +0.45 cents at 96000 (fm_sine_ref, expected 440.126 Hz from the public formula,
  measured 440.556 Hz). The MAME core and our core both give 440.126 Hz. A pitch error that
  changes with the host-side resampling rate is an integration artefact of the player, not
  chip behaviour, so compare.py measures the factor on fm_sine_ref against the formula and
  resamples the Nuked renders by it (documented in the report header). The SN76496 cores
  running in the same VGMPlay instance show no offset.
* The YM2612 output is not DC-blocked (the ladder-effect idle offset appears as a step at
  power-on); compare.py removes it identically on both sides.
* The MAME YM2612 core has no ladder effect (its odd harmonics of a pure sine are 35 dB below
  Nuked's and ours).

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
* It starts the SPC700 program from the file's register snapshot like hardware after a
  state load; our side has no SPC700 and replays the event list the generator emits, which
  gives a constant 9-sample offset (the program's start-up and KON poll); alignment removes it.

## What was not used

* No game, ROM, soundtrack, VGM/SPC rip or FM patch collection: every stimulus is generated
  by `tools/refcheck/make_stimuli.py` from our own register writes, BRR data and SPC700
  program.
* No reference source code was opened, and no reference output is committed (renders go to
  `build-reports/refcheck/`, git-ignored).
