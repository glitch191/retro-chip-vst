# Reference emulators for the differential check

Product-owner decision of 2026-09-29: a mature reference emulator binary may be downloaded
and **run** to compare audio output; its source code is never read, copied or imitated.
Everything below was used as a black box: our own stimuli in, WAV out. Binaries live in
`third_party/refemu/` (git-ignored) and are never redistributed.

The harness: `tools/refcheck/make_stimuli.py` (stimuli), `chiptool regs genesis|snes|nes` (our side),
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

## NES: VGMPlay 0.40.9 (two NES APU cores) and Game Music Emu (NSF)

Run of 2026-09-30, same black-box rule: our own register writes in, WAV out, no source read.

| Item | Value |
|---|---|
| Tool 1 | VGMPlay 0.40.9 (same binary and licence notes as the Genesis section), `[NES APU]` section of `VGMPlay.ini` |
| Input 1 | VGM 1.61: header 0x84 = NES APU clock (1789773 Hz NTSC, 1662607 Hz PAL, bit 31 = FDS clear), command `0xB4 aa dd` (write dd to $4000 + aa), DMC bytes in a data block `0x67 0x66 0xC2` ("NES APU RAM write", 16-bit start address $C000). Format from the VGM specification text v1.71 (vgmrips; the wiki page https://vgmrips.net/wiki/VGM_Specification refused the fetch on 2026-09-30, the same text was read from https://raw.githubusercontent.com/vgmrips/vgmplay-legacy/master/VGMPlay/vgmspec171.txt, a format document, not program source) |
| Configuration `nsfplay` (primary) | `EmulatorType = 0x00` (NSFPlay-derived core, per the ini comment "ported from rainwarrior's NSFPlay" in `VGMPlay.txt`), `SharedOpts = 0x02` (non-linear mixer; power-on "unmute" off), `APUOpts = 0x01` (phase refresh on $4003, no duty swap), `DMCOpts = 0x03` ($4011 and periodic noise enabled; DPCM anti-click, noise randomisation, triangle mute and triangle null all off), `ChipSmplMode = 0` (native) |
| Configuration `nesmame` (second opinion) | `EmulatorType = 0x01` (MAME core), `ChipSmplMode = 0` |
| Settings (both) | as Genesis: `SampleRate = 44100`, `ResamplingMode = 0`, `FadeTime = 0`, `Volume = 1.0`, `LogSound = 1` |
| Tool 2 (third opinion, subset) | FFmpeg 9.0.2 `libgme` demuxer (Game Music Emu NSF player, same build and licence notes as the SNES section): `ffmpeg -f libgme -sample_rate 44100 -i <stim>.nsf -t <seconds> -c:a pcm_s16le <out>.wav` |
| Input 2 | NSF (NESdev wiki "NSF", https://www.nesdev.org/wiki/NSF, consulted 2026-09-30: header layout, play rate in microseconds, player initialisation of $4000-$4017 before INIT). Our own 6502 program (`NesVgm.nsf` in `make_stimuli.py`, 57 bytes) replays the VGM writes once per video frame from a table, so only the static tones and mixer stimuli (23, `NSF_SET`) are rendered this way |

Why these: VGMPlay is already in use and ships two independent NES cores; NES VGM is the only
register-log format both it and our tool read. Game Music Emu is already present (FFmpeg) and
reads NSF, which needs a 6502 program: added for the mixer question below. No other headless
NES renderer was found among the binaries already downloaded.

Known behaviour of the references (measured; details and numbers in refcheck-report.md, NES
section):

* Output: mono (L = R), not DC-blocked (the unipolar DAC level is present); compare.py gives the
  renders the same 5 Hz coupling capacitor our console_filter = 0 path has, and the 60 Hz /
  15 kHz analysis filters of the Genesis comparison. No console filter on either side. The
  NSFPlay core puts the triangle / noise / DMC group in opposite polarity to the pulses
  (harmless for levels; alignment measures the polarity per stimulus).
* Levels: NSFPlay's pulse path equals our mixer output within 0.05 dB (global gain -0.045 dB);
  MAME +3.9 dB; GME +6.4 dB.
* NSFPlay core: non-linear pulse mixer identical to the documented formula (level ratios and
  intermodulation within 0.05 dB), but no cross-channel compression in the triangle / noise /
  DMC group (the DMC level does not change the triangle or noise level), and the triangle
  2.47 dB below the formula's level relative to the pulses. Resets the pulse timer divider on a
  $4003 write (notes start one sequencer step later than documented). No immediate quarter /
  half frame clock on a $4017 write with bit 7 set (5-step events one quarter or half frame
  late). With a PAL clock it uses the PAL noise and DMC tables but the NTSC frame-sequencer
  lengths (PAL envelopes and length counters 0.897 x the documented time). Mutes on a sweep
  target above $7FF only at the next sweep clock. Envelope decay counter not 0 at power-up (the
  first note plays before the first quarter frame). Ultrasonic triangle (t < 2) is stepped and
  aliases into the audio band (-40 dBFS). Top-octave droop of the player's resampler as for the
  Genesis (+1.1 dB at 12.4 kHz on our side after the kernel correction).
* MAME core: linear mixer (no intermodulation), pitch -0.09 cents (NTSC) / -0.13 cents (PAL),
  no t < 8 or sweep-overflow mute, triangle 6.6 dB and envelopes / sweeps with other timings
  than documented, NTSC noise and DMC tables in PAL. Used only where it agrees with NSFPlay.
* Game Music Emu (NSF): linear triangle / noise / DMC group (no compression, triangle harmonics
  without the non-linear H2), pitch +0.21 cents; skips about 30 ms of leading silence (alignment
  removes it).

## What was not used

* No game, ROM, soundtrack, VGM/SPC/NSF rip or FM patch collection: every stimulus is
  generated by `tools/refcheck/make_stimuli.py` from our own register writes, BRR data, DMC
  bytes, SPC700 program and 6502 program.
* No reference source code was opened, and no reference output is committed (renders go to
  `build-reports/refcheck/`, git-ignored).
