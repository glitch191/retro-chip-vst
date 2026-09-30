# Sources

Public documentation used for every table and formula in `chipdsp`, consolidated from the
"Sources" sections of `docs/research/nes.md`, `snes.md`, `genesis.md` and `plugin.md`
(those sections also record the verification passes and which pages were refetched).

**No emulator source code was used.** No emulator source (GPL or otherwise: Mesen, bsnes,
Genesis Plus GX, Nuked-OPN2, MAME `fm.cpp`, blip_buf, snes_spc, Game_Music_Emu...) was
read, copied or imitated. Where a forum post or article reports what an emulator does
(for example Eke's notes on MAME, or jsgroth crediting Nuked-OPN2 for some facts), only
the prose statement was used, as one opinion among the hardware sources, and the choice
is recorded in `docs/HARDWARE_NOTES.md`. Tables were transcribed from the documents below
and re-derived with small Python scripts written for this project.

All sources were consulted on 2026-09-28 unless noted. Format: name, URL, what was taken.

## NES (2A03 / 2A07)

| # | Source | URL | Taken |
|---|---|---|---|
| 1 | NESdev wiki, "APU" | https://www.nesdev.org/wiki/APU | Register map, $4015 status, $4017 summary, glossary (APU cycle = 2 CPU cycles), power-up note |
| 2 | NESdev wiki, "APU registers" | https://www.nesdev.org/wiki/APU_registers | Bit fields of $4000-$4017, unused bits |
| 3 | NESdev wiki, "APU Pulse" | https://www.nesdev.org/wiki/APU_Pulse | Duty sequences, timer, frequency formula, mute at t < 8, $4003 side effects |
| 4 | NESdev wiki, "APU Sweep" | https://www.nesdev.org/wiki/APU_Sweep | Target period, ones'/two's complement, mute rules, half-frame procedure |
| 5 | NESdev wiki, "APU Envelope" | https://www.nesdev.org/wiki/APU_Envelope | Start flag, divider, decay level, loop, constant volume |
| 6 | NESdev wiki, "APU Length Counter" | https://www.nesdev.org/wiki/APU_Length_Counter | 32-entry length table, load/clock/halt rules |
| 7 | NESdev wiki, "APU Triangle" | https://www.nesdev.org/wiki/APU_Triangle | 32-step sequence, CPU-rate timer, linear counter |
| 8 | NESdev wiki, "APU Noise" | https://www.nesdev.org/wiki/APU_Noise | LFSR procedure (modes 0/1), power-up value, NTSC and PAL period tables |
| 9 | NESdev wiki, "APU DMC" | https://www.nesdev.org/wiki/APU_DMC | NTSC and PAL rate tables, output unit, memory reader, $4011 |
| 10 | NESdev wiki, "APU Frame Counter" | https://www.nesdev.org/wiki/APU_Frame_Counter | 4-step and 5-step tables (NTSC, PAL), $4017 write behaviour, IRQ inhibit |
| 11 | NESdev wiki, "APU Mixer" | https://www.nesdev.org/wiki/APU_Mixer | Exact non-linear mixer formula, NES-001 filter chain, Famicom 37 Hz note |
| 12 | NESdev wiki, "APU period table" | https://www.nesdev.org/wiki/APU_period_table | Generator script constants, NTSC/PAL period formula |
| 13 | NESdev wiki, "APU basics" | https://www.nesdev.org/wiki/APU_basics | Init sequence, 80 NTSC period bytes (cross-check), $4001 = $08, period-0 pop |
| 14 | NESdev wiki, "APU Misc" | https://www.nesdev.org/wiki/APU_Misc | Divider period P + 1, timer clocking |
| 15 | NESdev wiki, "CPU" | https://www.nesdev.org/wiki/CPU | 2A03 / 2A07 clocks |
| 16 | NESdev wiki, "Cycle reference chart" | https://www.nesdev.org/wiki/Cycle_reference_chart | Exact clocks, cycles per frame, 60.0988 / 50.0070 Hz frame rates |
| 17 | NESdev wiki, "CPU power up state" | https://www.nesdev.org/wiki/CPU_power_up_state | APU power-up and reset state |
| 18 | blargg, "apu_ref.txt" | https://www.nesdev.org/apu_ref.txt (original: http://www.slack.net/~ant/nes-emu/apu_ref.txt) | Cross-check of frame sequencer, noise/DMC/length tables, mixer constants, triangle halt and "half way between 7 and 8" |
| 19 | Brad Taylor, "2A03 technical reference" | https://www.nesdev.org/2A03%20technical%20reference.txt | Cross-check (frame sequencer, noise and DMC tables, sweep, envelope, "57 %" DMC note) |
| 20 | Brad Taylor, "NESSOUND.txt" | https://www.nesdev.org/NESSOUND.txt | Same cross-checks, 5-step immediate clock |
| 21 | NESdev forum, "NES audio filters" (blargg, lidnariq) | https://forums.nesdev.org/viewtopic.php?p=44255 | RC components and measured corners (90 / 442 Hz, 14 kHz) |
| 22 | NESdev forum, t=13626 (FamiTracker note table) | https://forums.nesdev.org/viewtopic.php?t=13626 | Playable range statements only (no values) |

Also read during verification: NESdev wiki "Talk:APU Frame Counter" (7456.5 statement).

## SNES (S-DSP)

| # | Source | URL | Taken |
|---|---|---|---|
| 1 | nocash, "Fullsnes" (APU DSP sections) | https://problemkaputt.de/fullsnes.htm | Register map, BRR shift/filter formulas, clamp rules, 512-entry Gaussian table (primary transcription), pitch counter and PMON, ADSR/GAIN rates, KON/KOFF/FLG/ENDX, noise LFSR, output mixer and final XOR, echo buffer and FIR |
| 2 | Anomie (updates by jwdonal), "Anomie's S-DSP Doc", rev. 1212 | https://37.muncher.se/bot/apudsp.txt (index: https://www.romhacking.net/documents/191/; older copy: http://www.gamepilgrimage.com/sites/default/files/SystemSpecs/SNES/anomie/apudsp.txt) | Clocks, voice steps S1..S9, global counter, rate and offset tables, BRR integer filters, key-on sequence, PMON code and 0x7FFF clamp, Gaussian 15-bit formula, envelope state machine, noise, echo order, FIR wrap/clamp |
| 3 | SNESdev wiki, "S-DSP registers" | https://snes.nesdev.org/wiki/S-DSP_registers | Register diagrams, KON/KOFF errata, FLG, noise frequency table, PMON, DIR format, ESA/EDL errata, GAIN modes, ENVX/OUTX |
| 4 | SNESdev wiki, "DSP envelopes" | https://snes.nesdev.org/wiki/DSP_envelopes | Envelope step formulas, manual timing tables, period and offset tables |
| 5 | SNESdev wiki, "BRR samples" | https://snes.nesdev.org/wiki/BRR_samples | Block layout, filter coefficients, loop/end semantics |
| 6 | SNESdev wiki, "S-SMP" | https://snes.nesdev.org/wiki/S-SMP | 24.576 MHz, 3.072 / 1.024 MHz, 32 kHz sample clock |
| 7 | SnesLab wiki, "S-DSP/Gaussian Filter" | https://sneslab.net/wiki/S-DSP/Gaussian_Filter | Independent Gaussian table (verification, identical) |
| 8 | SnesLab wiki, "Bit Rate Reduction" | https://sneslab.net/wiki/BRR | Header layout, shift 13-15 rule |
| 9 | SnesLab wiki, "FIR Filter" | https://sneslab.net/wiki/FIR_Filter | Tap format and order, clip/clamp rules, N-SPC standard filter bytes |
| 10 | SnesLab wiki, "S-DSP" | https://sneslab.net/wiki/S-DSP | Register address table |
| 11 | Super Famicom Development wiki, "SPC700 Reference" (official manual transcription) | https://wiki.superfamicom.org/spc700-reference | ADSR/GAIN timing tables, noise clock table, ESA/EDL and echo initialisation, pitch formula |
| 12 | romhacking.net document 191 listing | https://www.romhacking.net/documents/191/ | Identification of source 2 only |

## Sega Genesis (YM2612 + SN76489)

| # | Source | URL | Taken |
|---|---|---|---|
| 1 | Plutiedev, "YM2612 register reference" | https://plutiedev.com/ym2612-registers | Register layouts, operator order, $28 encoding, detune signs, TL 0.75 dB, fnum table, LFO/AMS/PMS tables, DAC |
| 2 | Plutiedev, "Common YM2612 operations" | https://plutiedev.com/ym2612-operations | Key-on sequence, carriers per algorithm, DAC playback |
| 3 | Kabuto, "SEGA Mega Drive / Genesis hardware notes" v1.5 | https://plutiedev.com/mirror/kabuto-hardware-notes | DAC sampling, 9th DAC bit, MD1 / MD2 filters, loud-PCM gains |
| 4 | Yamaha, "YM2608 OPNA Application Manual" (translation by Nemesis) | https://csclub.uwaterloo.ca/~pbarfuss/YM2608J_Translated.PDF | Register tables, F-number formula, key code, multiplier, detune (Table 2-6), SL (2-7), key scaling (2-8), TL (2-9), rate formula, SSG-EG, LFO/PMS/AMS, feedback levels, algorithms |
| 5 | Nemesis, "An authoritative reference on the YM2612", SpritesMind t=386 p8 | https://gendev.spritesmind.net/forum/viewtopic.php?t=386&start=105 | EG article: attenuation, rate calculation, update tables, attack/decay formulas, SSG-EG, durations; Eke's clock notes; Shiru's timing notes |
| 6 | Same thread p11 | ...&start=150 | Operator unit, sine and power tables, 14-bit output |
| 7 | Same thread p12 | ...&start=165 | EG clock = FM clock / 3, SL clamp, attack skip |
| 8 | Same thread p13 | ...&start=180 | SSG-EG in release, operator order 1-3-2-4, feedback averaging |
| 9 | Same thread p26 | ...&start=375 | AM still applied with the LFO off |
| 10 | Same thread p33 | ...&start=480 | PM increment table for fnum bit 9; Eke on LFO hold/reset |
| 11 | Same thread p37 | ...&start=550 | GManiac: operator range +-8168, DAC level quirks; TmEE: MD2 YM3438 louder |
| 12 | Same thread p55 | ...&start=810 | Sauraen: 9-bit accumulator and DAC; BUSY flag |
| 13 | Same thread p56 | ...&start=825 | Eke: ladder effect values; Sauraen: output multiplexing order |
| 14 | SpritesMind, "How DAC works ?" | http://gendev.spritesmind.net/forum/viewtopic.php?t=3049 | DAC linearity, 24x / 6x amplification, output cycles |
| 15 | SpritesMind, "PSG mixing volume?" | https://gendev.spritesmind.net/forum/viewtopic.php?t=1631 | PSG divide by 6-6.4 (-15.6 / -16.1 dB), model variation |
| 16 | SpritesMind, "help needed for megadrive audio modding" | http://gendev.spritesmind.net/forum/viewtopic.php?t=730 | Jorge Nuno: PSG/YM mixing ratios |
| 17 | SpritesMind, "YM2612 shape of envelope attack" | http://gendev.spritesmind.net/forum/viewtopic.php?t=932 | GManiac's attack measurement (73 steps); GreenLine's alternative |
| 18 | Maxim, "Sega Genesis Technical Manual - YM2612 section" | https://www.smspower.org/maxim/Documents/YM2612 | Sega register map, DAC on channel 6, RS formula, D1L, RR, LFO/AMS/FMS tables |
| 19 | Maxim, "SN76489" (SMS Power) | https://www.smspower.org/Development/SN76489 | Write protocol, Genesis mapping, power-on state, tone formula, period 0/1, noise reload, LFSR widths/taps/reset, 2 dB volume table |
| 20 | Texas Instruments, "SN76489AN" datasheet | https://map.grauw.nl/resources/sound/texas_instruments_sn76489an.pdf | f = N/32n, attenuator weights, noise feedback and shift rates, register cleared on noise write |
| 21 | Howel, "SN76489 Sound Generator Chip" | https://www.acornatom.nl/sites/atomreview/howel/parts/76489.htm | Confirmation of the datasheet bit order |
| 22 | Charles MacDonald, "Sega Genesis hardware notes" v0.8 | https://gendev.spritesmind.net/mirrors/cmd/gen-hw.txt | NTSC clocks, PSG on the VDP, YM2612 mirroring |
| 23 | jsgroth, "Emulating the YM2612" parts 1-7 (2025) | https://jsgroth.dev/blog/posts/emulating-ym2612-part-1/ (to part-7) | Secondary prose source: clocks, phase counter, detune, 12-bit EG counter, attack shift, SL 15, sine/exp construction, 13-bit magnitude, algorithm delays, 9-bit quantisation, ladder offsets, Model 1 filters, LFO dividers, AM/PM, SSG-EG logic, discrete versus ASIC console list |
| 24 | jsgroth, "Genesis & Sega CD - Audio Filtering" (2025) | https://jsgroth.dev/blog/posts/genesis-sega-cd-audio-filtering/ | Model 1 3.39 kHz / 2.84 kHz first-order filters |
| 25 | Residual Media, "Forensics: Genesis 2 with original Mega Amp" | https://residualmedia.net/forensics-genesis-2-with-original-mega-amp/ | Model 2 second-order low-pass (not modelled) |
| 26 | Sega Retro, "Sega Mega Drive/Technical specifications" | (search summary) | Refresh rates 59.92274 / 49.701459 Hz |
| 27 | RadDad772, "Genesis VDP internals, part three" | https://raddad772.github.io/2024/07/21/genesis-vdp-pt-3.html | 3420 master clocks per line |
| 28 | Wikipedia, "Yamaha YM2612" | https://en.wikipedia.org/wiki/Yamaha_YM2612 | Confirmation only (9-bit DAC, YM3438) |
| 29 | ConsoleMods, "Genesis: Audio Chip Notes" | https://consolemods.org/wiki/Genesis:Audio_Chip_Notes | Blocked; search snippet used only to confirm the console list |
| 30 | Internet Archive, "Genesis Technical Overview v1.00 (1991)" | https://archive.org/stream/Genesis_Technical_Overview_v1.00_1991_Sega_US | Consulted; nothing taken |

Notes: several SpritesMind pages and the jsgroth posts were read through search or
fetch summaries; jsgroth and ConsoleMods later returned bot-filter pages, so the LFO
divider table and AM phase (jsgroth part 6) could not be re-read (HARDWARE_NOTES,
"LFO frequencies"). jsgroth credits Nuked-OPN2 for some facts; that emulator was not opened.

## Samples (CC0 recordings)

Product-owner decision of 2026-09-29: CC0 real-instrument recordings may be converted into
the factory samples, the way 1990s SNES and Genesis composers built their sets from
sample CDs. Never samples ripped from games, ROMs or soundtracks, never FM patch
collections ripped from games. Conversion: `tools/samplegen/cc0_import.py`; every output
sample with its source files, git blob ids, licence, root note, rate, loop mode and
processing: `tools/samplegen/cc0_manifest.json`; `source` and `licence` of every sample:
`assets/samples/index.json`.

| # | Source | URL | Consulted | Taken |
|---|---|---|---|---|
| 1 | Versilian Studios, "VS Chamber Orchestra: Community Edition" (VSCO 2 CE), commit 440300901dfe9275fd84e0b7763af1f8443ae62e | https://github.com/sgossner/VSCO-2-CE | 2026-09-29 | Upright piano, violin/cello section sustains and pizzicato, trumpet, horn and trombone sustains and staccatos, flute, clarinet, oboe, contrabass pizzicato, timpani, muted concert bass drum (VSCO 1 percussion) |
| 2 | Versilian Studios, "Versilian Community Sample Library" (VCSL), commit c1ea7bcc3c7309650ab0da9d15c9cd1fbc4a4c7e | https://github.com/sgossner/VCSL | 2026-09-29 | Steinway B grand piano, TX81Z "FM Piano" recording, pipe organ, snares, cross-stick, toms, hi-hats, claps, cowbells, suspended and clash cymbals, shaker, conga, tambourine, open concert bass drum |

Licence verification: both repositories' `LICENSE` files are the Creative Commons CC0 1.0
Universal text, and the GitHub API reports `CC0-1.0` for both (checked 2026-09-29). The
VSCO 2 CE `Readme.txt` also asks, without legal force under CC0, not to sell the samples
directly and to credit Versilian Studios / Sam Gossner and Ivy Audio / Simon Dalzell: the
samples are only shipped converted inside the plugin, and the credit is kept here and in
the manifest. The TX81Z recording is a CC0 recording of a synthesizer's factory sound, not
a patch collection ripped from a game.

Octave labels: VSCO 2 CE names its strings, brass and woodwind files one octave below
scientific pitch (the violin section's lowest file is labelled G2 for G3, 196 Hz), while
its upright piano uses MIDI key numbers and VCSL uses scientific pitch. The manifest
states the real pitch of every pitched layer; `measure_pitch` confirms it by
autocorrelation (an octave-away label is an error) and the measured cents are folded into
the resampling ratio, so every root note is exact. Several recordings (violin pizzicato,
the open organ manual) correlate almost as well at half the period, which is why the
octave comes from the label and not from the measurement.

No reference emulator was downloaded or run for this work.

## Plugin layer

| Source | URL | Taken |
|---|---|---|
| MIDI 1.0 Detailed Specification, Channel Mode messages | https://midi.org/midi-1-0-detailed-specification | CC 120 / 121 / 123 act on their own channel (general knowledge, not re-read; to check) |
| JUCE 9.0.3 module sources (framework linked by the plugin) | `third_party/JUCE/modules` | API of `CaretComponent`, popup menus, keyboard focus, VBlankAttachment |

The plugin layer implements no chip behaviour, so it has no hardware sources.

## Preset QA

The perceptual thresholds in `tools/presetgen/qa.py` (about 1 dB for level and spectral
balance, 3-5 cents for pitch, 15 % for envelope times) are the usual just-noticeable
differences, taken from general psychoacoustic knowledge. No document was re-read for
them, and they were calibrated on the rendered banks (`docs/PRESET_QA.md`).
