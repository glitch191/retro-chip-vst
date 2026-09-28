"""Procedural, rights-free sample generator for retro-chip-vst.

Usage::

    python tools\\gen_samples.py [--jobs N] [--chip nes|snes|genesis] [--out DIR]
                                [--user-dir DIR]

Writes mono 16-bit PCM WAV files to ``assets/samples/<chip>/<name>.wav`` and an index
``assets/samples/index.json``: a JSON array of ``{chip, name, file, sample_rate,
root_note, loop_start, loop_end, category, description}`` sorted by chip then name
(``loop_start``/``loop_end`` are frame counts or ``null`` for one-shots). Everything is
standard-library Python; synthesis lives in ``tools/samplegen/`` (``synth`` primitives,
``drums``, ``instruments``, ``wavio``, ``pool``).

Determinism: every sample is rendered from its own ``random.Random`` seeded by a CRC of
``chip/name``; the process pool returns results in catalogue order; JSON keys are
sorted. Running the command twice produces byte-identical files.

Post-processing common to every sample: DC offset removed (mean subtraction), 4-frame
fade-in and 5 ms fade-out on one-shots, peak normalised to -1 dBFS, SNES one-shots
padded to a multiple of 16 frames (one BRR block), NES sources low-passed at 8 kHz.

Per-chip targets
----------------

* ``nes``: 33144 Hz (DMC rate index 15), 30-400 ms. The plugin turns these into 1-bit
  delta (DPCM) streams whose slew rate is limited to +/-2 counter steps per sample, so
  the sources are bold and simple: sine-based kicks/toms with pitch drops, tight
  noise snares, short hats, a 100 ms bass pluck, a synthetic orchestra hit, a formant
  voice blip and a filtered noise burst.
* ``snes``: 32000 Hz (pads 16000 Hz). Each file stays under 12 KiB of BRR (about
  21000 frames); sustained sounds carry loop points on 16-frame boundaries with a
  whole number of pitch cycles inside the loop and a cross-fade over the wrap.
* ``genesis``: 22050 Hz, 30-300 ms, delivered as 16-bit (the plugin reduces to 8-bit
  DAC data): drums, a chunky "sega hit", a formant "uh" and a noise burst.

Synthesis families (see ``samplegen/drums.py`` and ``samplegen/instruments.py``)
-------------------------------------------------------------------------------

* Kicks / toms: sine oscillator with an exponential pitch drop, exponential amplitude
  decay, a short high-passed noise click and tanh saturation (808 kick adds a 3 kHz
  roll-off and heavier drive).
* Snares / rim / clap: decaying sine shell modes plus high-passed white noise; the rim
  rings two resonant band-pass filters with a 2 ms burst; the clap shapes band-passed
  noise with four 9.5 ms bursts and a longer tail.
* Hats / crash / ride / cowbell / shaker: the classic six-square-wave metallic bed
  (205.3 .. 800 Hz) mixed with noise, high- or band-passed, exponential decays; the
  cowbell uses the 587/845 Hz square pair; the shaker is high-passed noise grains.
* Orchestra hit / sega hit: detuned band-limited saw+square chord (root, 5th, octave,
  3rd, 5th, 2 octaves), noise transient, fast ADSR, low-pass and drive; the sega hit
  adds a low FM brass layer and more saturation.
* Voice blip / voice uh: narrow pulse with a falling pitch through a vowel formant
  bank (parallel resonant band-pass filters).
* Noise: white noise through a sweeping resonant low-pass, band-passed bursts, and a
  raw white-noise loop.
* Pianos: additive synthesis, 28 partials with stiffness inharmonicity, faster decay
  of upper partials, hammer noise. E-piano: two-operator FM with an index "bark" and
  a 7x tine partial.
* Strings / brass / trumpet: detuned band-limited saws (wavetable oscillator with
  harmonics up to Nyquist), chorus, resonant state-variable low-pass sweeps and
  vibrato whose rate fits the loop length. Pizzicato is a damped Karplus-Strong.
* Guitars / basses: Karplus-Strong plucked strings (fractional delay, one-zero
  damping, decay per period) with pick brightness, mute damping and slap transient;
  bass synth is saw + sub square through a resonant low-pass sweep with ADSR.
* Winds: flute (sine plus weak harmonics, breath noise band-passed at the pitch),
  clarinet (odd harmonics), oboe (narrow pulse through a double-reed formant bank).
* Organs: sine drawbars (16' to 1'), percussive variant with decaying 2nd/3rd.
* Choirs: three detuned pulses with vibrato through "ah"/"oo" formant banks.
* Single-cycle loops: band-limited saw/square/triangle/sine at A3 with 11 cycles in a
  1600-frame loop (exactly in tune, BRR aligned).
* Pads (16 kHz): glass = 1:3 FM plus detuned sine partials, warm = four detuned saws
  through a low-pass, both with slow attacks and chorus, looped.

User samples
------------

WAV files the product owner drops into ``tools/user_samples/<chip>/`` (or
``--user-dir DIR`` with the same ``<chip>/`` layout) are imported into the set: they
are read (8/16/24/32-bit PCM, any channel count, mixed to mono), resampled to the
chip's rate with a windowed-sinc resampler, DC-removed, normalised to -1 dBFS and
written as ``assets/samples/<chip>/<name>.wav`` where ``<name>`` is the file stem
lowercased with non-alphanumerics replaced by ``_``. They appear in the index with
``category`` ``"user"`` and no loop points; a user file whose name matches a built-in
sample replaces it. Long files are not truncated, but a warning is printed when they
exceed the chip's recommended length (NES: DMC hardware limit of 32648 frames; SNES:
21000 frames).
"""

import argparse
import json
import os
import random
import re
import sys
from collections import namedtuple

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

from samplegen import drums, instruments, pool, wavio  # noqa: E402
from samplegen import synth as S  # noqa: E402

PROJECT_ROOT = os.path.dirname(TOOLS_DIR)
DEFAULT_OUT = os.path.join(PROJECT_ROOT, "assets", "samples")
DEFAULT_USER_DIR = os.path.join(TOOLS_DIR, "user_samples")

CHIP_RATES = {"nes": 33144, "snes": 32000, "genesis": 22050}
CHIP_MAX_FRAMES = {"nes": 32648, "snes": 21000, "genesis": 6615}
CHIP_MIN_FRAMES = {"nes": 994, "snes": 16, "genesis": 661}
BRR_BLOCK = 16
NES_LOWPASS_HZ = 8000.0

Spec = namedtuple("Spec", "chip name category description sample_rate root_note builder params")

BUILDERS = {
    "drums.kick": drums.kick,
    "drums.kick_808": drums.kick_808,
    "drums.snare": drums.snare,
    "drums.rim": drums.rim,
    "drums.tom": drums.tom,
    "drums.hat": drums.hat,
    "drums.crash": drums.crash,
    "drums.ride": drums.ride,
    "drums.clap": drums.clap,
    "drums.cowbell": drums.cowbell,
    "drums.shaker": drums.shaker,
    "drums.noise_burst": drums.noise_burst,
    "drums.noise_filtered": drums.noise_filtered,
    "drums.orchestra_hit": drums.orchestra_hit,
    "drums.voice": drums.voice,
    "instruments.piano": instruments.piano,
    "instruments.epiano": instruments.epiano,
    "instruments.organ": instruments.organ,
    "instruments.strings_ensemble": instruments.strings_ensemble,
    "instruments.strings_pizz": instruments.strings_pizz,
    "instruments.brass_section": instruments.brass_section,
    "instruments.trumpet": instruments.trumpet,
    "instruments.guitar": instruments.guitar,
    "instruments.flute": instruments.flute,
    "instruments.clarinet": instruments.clarinet,
    "instruments.oboe": instruments.oboe,
    "instruments.choir": instruments.choir,
    "instruments.bass_synth": instruments.bass_synth,
    "instruments.bass_finger": instruments.bass_finger,
    "instruments.bass_slap": instruments.bass_slap,
    "instruments.single_cycle": instruments.single_cycle,
    "instruments.noise_loop": instruments.noise_loop,
    "instruments.pad_glass": instruments.pad_glass,
    "instruments.pad_warm": instruments.pad_warm,
}

A2 = 110.0
A3 = 220.0
A4 = 440.0
C3 = 130.81
C4 = 261.63


def _spec(chip, name, category, description, builder, root_note=60, sample_rate=None, **params):
    return Spec(chip, name, category, description, sample_rate or CHIP_RATES[chip],
                root_note, builder, params)


def catalogue():
    """Every built-in sample, in a stable order."""
    specs = []
    nes = "nes"
    specs += [
        _spec(nes, "kick_short", "drums", "Short punchy kick, 150-50 Hz sine drop", "drums.kick",
              length=0.16, f_start=160.0, f_end=55.0, sweep_tau=0.035, amp_tau=0.05, click=0.4),
        _spec(nes, "kick_long", "drums", "Long kick with a slower pitch drop", "drums.kick",
              length=0.36, f_start=140.0, f_end=45.0, sweep_tau=0.08, amp_tau=0.13, click=0.3),
        _spec(nes, "kick_808", "drums", "Round 808-style kick, 400 ms", "drums.kick_808",
              length=0.4),
        _spec(nes, "snare_tight", "drums", "Tight snare, short noise tail", "drums.snare",
              length=0.14, tone_tau=0.03, noise_tau=0.035, noise_level=0.7, tone_level=0.8),
        _spec(nes, "snare_noisy", "drums", "Noisy snare with a long noise tail", "drums.snare",
              length=0.26, tone_tau=0.04, noise_tau=0.09, noise_level=1.0, tone_level=0.5),
        _spec(nes, "tom_low", "drums", "Low tom, 90 Hz", "drums.tom", length=0.3, freq=90.0),
        _spec(nes, "tom_high", "drums", "High tom, 170 Hz", "drums.tom", length=0.22, freq=170.0),
        _spec(nes, "hat_closed", "drums", "Closed hi-hat, metallic squares plus noise",
              "drums.hat", length=0.06, open_hat=False),
        _spec(nes, "hat_open", "drums", "Open hi-hat, 300 ms decay", "drums.hat",
              length=0.3, open_hat=True),
        _spec(nes, "clap", "drums", "Hand clap, four bursts and a tail", "drums.clap",
              length=0.2),
        _spec(nes, "rim", "drums", "Rimshot, resonant ping", "drums.rim", length=0.06),
        _spec(nes, "crash_short", "drums", "Short crash cymbal, 400 ms", "drums.crash",
              length=0.4, tau=0.11),
        _spec(nes, "bass_pluck", "bass", "100 ms filtered saw bass pluck at A2",
              "instruments.bass_synth", root_note=45, freq=A2, length=0.1,
              cutoff_start=4000.0, cutoff_end=300.0, cutoff_tau=0.03, resonance=1.8,
              decay=0.06, sustain=0.2),
        _spec(nes, "orchestra_hit_synth", "hit", "Synthetic orchestra hit, C3 chord",
              "drums.orchestra_hit", length=0.35, root=C3),
        _spec(nes, "voice_blip", "voice", "Formant voice blip, vowel 'e'", "drums.voice",
              length=0.09, vowel="e"),
        _spec(nes, "noise_burst", "sfx", "Noise burst with a falling low-pass", "drums.noise_burst",
              length=0.1),
    ]
    snes = "snes"
    specs += [
        _spec(snes, "piano_soft", "keys", "Additive piano, soft touch, C4", "instruments.piano",
              root_note=60, freq=C4, length=0.6, bright=False),
        _spec(snes, "piano_bright", "keys", "Additive piano, bright with inharmonic decay, C4",
              "instruments.piano", root_note=60, freq=C4, length=0.6, bright=True),
        _spec(snes, "epiano", "keys", "FM electric piano, C4", "instruments.epiano",
              root_note=60, freq=C4, length=0.6),
        _spec(snes, "strings_ensemble", "strings", "Detuned saw ensemble with chorus, looped, A3",
              "instruments.strings_ensemble", root_note=57, freq=A3),
        _spec(snes, "strings_pizz", "strings", "Pizzicato strings, A3", "instruments.strings_pizz",
              root_note=57, freq=A3, length=0.3),
        _spec(snes, "brass_section", "brass", "Brass section, filter swell, looped, A3",
              "instruments.brass_section", root_note=57, freq=A3),
        _spec(snes, "trumpet", "brass", "Solo trumpet with vibrato, looped, A4",
              "instruments.trumpet", root_note=69, freq=A4),
        _spec(snes, "guitar_nylon", "guitar", "Nylon guitar, Karplus-Strong, A3",
              "instruments.guitar", root_note=57, freq=A3, length=0.6, kind="nylon"),
        _spec(snes, "guitar_steel", "guitar", "Steel guitar, bright Karplus-Strong, A3",
              "instruments.guitar", root_note=57, freq=A3, length=0.6, kind="steel"),
        _spec(snes, "guitar_muted", "guitar", "Palm-muted guitar, A3", "instruments.guitar",
              root_note=57, freq=A3, length=0.15, kind="muted"),
        _spec(snes, "flute", "winds", "Flute with breath noise and vibrato, looped, A4",
              "instruments.flute", root_note=69, freq=A4),
        _spec(snes, "clarinet", "winds", "Clarinet, odd harmonics, looped, A3",
              "instruments.clarinet", root_note=57, freq=A3),
        _spec(snes, "oboe", "winds", "Oboe, reed formants, looped, A4", "instruments.oboe",
              root_note=69, freq=A4),
        _spec(snes, "organ_full", "organ", "Full drawbar organ, looped, A3", "instruments.organ",
              root_note=57, freq=A3, percussive=False),
        _spec(snes, "organ_perc", "organ", "Percussive organ, looped, A3", "instruments.organ",
              root_note=57, freq=A3, percussive=True),
        _spec(snes, "choir_ah", "choir", "Choir 'ah' formants, looped, A3", "instruments.choir",
              root_note=57, freq=A3, vowel="a"),
        _spec(snes, "choir_oo", "choir", "Choir 'oo' formants, looped, A3", "instruments.choir",
              root_note=57, freq=A3, vowel="u"),
        _spec(snes, "bass_synth", "bass", "Filtered saw synth bass, A2", "instruments.bass_synth",
              root_note=45, freq=A2, length=0.4),
        _spec(snes, "bass_finger", "bass", "Fingered bass, Karplus-Strong, A2",
              "instruments.bass_finger", root_note=45, freq=A2, length=0.5),
        _spec(snes, "bass_slap", "bass", "Slap bass, bright Karplus-Strong, A2",
              "instruments.bass_slap", root_note=45, freq=A2, length=0.4),
        _spec(snes, "saw_loop", "wave", "Band-limited saw, 11-cycle loop, A3",
              "instruments.single_cycle", root_note=57, freq=A3, shape="saw"),
        _spec(snes, "square_loop", "wave", "Band-limited square, 11-cycle loop, A3",
              "instruments.single_cycle", root_note=57, freq=A3, shape="square"),
        _spec(snes, "triangle_loop", "wave", "Band-limited triangle, 11-cycle loop, A3",
              "instruments.single_cycle", root_note=57, freq=A3, shape="triangle"),
        _spec(snes, "sine_loop", "wave", "Sine, 11-cycle loop, A3", "instruments.single_cycle",
              root_note=57, freq=A3, shape="sine"),
        _spec(snes, "pad_glass", "pad", "Glassy FM pad, 16 kHz, looped, A3", "instruments.pad_glass",
              root_note=57, sample_rate=16000, freq=A3),
        _spec(snes, "pad_warm", "pad", "Warm detuned saw pad, 16 kHz, looped, A2",
              "instruments.pad_warm", root_note=45, sample_rate=16000, freq=A2),
        _spec(snes, "kick", "drums", "Kick drum", "drums.kick", length=0.35, f_start=150.0,
              f_end=50.0, sweep_tau=0.05, amp_tau=0.1, click=0.35),
        _spec(snes, "snare", "drums", "Snare drum", "drums.snare", length=0.25),
        _spec(snes, "snare_rim", "drums", "Snare rimshot", "drums.rim", length=0.08),
        _spec(snes, "hat_closed", "drums", "Closed hi-hat", "drums.hat", length=0.07, open_hat=False),
        _spec(snes, "hat_open", "drums", "Open hi-hat", "drums.hat", length=0.35, open_hat=True),
        _spec(snes, "tom_low", "drums", "Low tom, 80 Hz", "drums.tom", length=0.35, freq=80.0),
        _spec(snes, "tom_mid", "drums", "Mid tom, 120 Hz", "drums.tom", length=0.3, freq=120.0),
        _spec(snes, "tom_high", "drums", "High tom, 170 Hz", "drums.tom", length=0.25, freq=170.0),
        _spec(snes, "clap", "drums", "Hand clap", "drums.clap", length=0.22),
        _spec(snes, "cowbell", "drums", "Cowbell, 587/845 Hz squares", "drums.cowbell", length=0.25),
        _spec(snes, "crash", "drums", "Crash cymbal, 600 ms", "drums.crash", length=0.6),
        _spec(snes, "ride", "drums", "Ride cymbal with bell ping, 600 ms", "drums.ride", length=0.6),
        _spec(snes, "shaker", "drums", "Shaker, two noise grains", "drums.shaker", length=0.12),
        _spec(snes, "noise_white_loop", "noise", "White noise, 4096-frame loop",
              "instruments.noise_loop", frames=4096),
        _spec(snes, "noise_filtered", "noise", "Band-passed noise burst, 2.2 kHz",
              "drums.noise_filtered", length=0.3),
    ]
    gen = "genesis"
    specs += [
        _spec(gen, "kick", "drums", "Kick drum", "drums.kick", length=0.25, f_start=150.0,
              f_end=52.0, sweep_tau=0.04, amp_tau=0.07, click=0.4, drive=1.8),
        _spec(gen, "kick_deep", "drums", "Deep kick, 100-40 Hz", "drums.kick", length=0.3,
              f_start=100.0, f_end=40.0, sweep_tau=0.07, amp_tau=0.11, click=0.2, drive=2.2,
              lowpass=2500.0),
        _spec(gen, "snare", "drums", "Snare drum", "drums.snare", length=0.2),
        _spec(gen, "snare_short", "drums", "Short snare", "drums.snare", length=0.1,
              tone_tau=0.025, noise_tau=0.03, noise_level=0.9, tone_level=0.7),
        _spec(gen, "clap", "drums", "Hand clap", "drums.clap", length=0.18),
        _spec(gen, "hat_closed", "drums", "Closed hi-hat", "drums.hat", length=0.05, open_hat=False),
        _spec(gen, "hat_open", "drums", "Open hi-hat", "drums.hat", length=0.25, open_hat=True),
        _spec(gen, "tom", "drums", "Tom, 120 Hz", "drums.tom", length=0.22, freq=120.0),
        _spec(gen, "cowbell", "drums", "Cowbell", "drums.cowbell", length=0.2),
        _spec(gen, "rim", "drums", "Rimshot", "drums.rim", length=0.05),
        _spec(gen, "crash_short", "drums", "Short crash cymbal, 300 ms", "drums.crash",
              length=0.3, tau=0.09),
        _spec(gen, "sega_hit", "hit", "Chunky synthetic orchestra hit with FM brass, C3",
              "drums.orchestra_hit", length=0.3, root=C3, chunky=True, hold=0.05, tau=0.08),
        _spec(gen, "voice_uh", "voice", "Formant voice 'uh', falling pitch", "drums.voice",
              length=0.18, vowel="uh", f_start=140.0, f_end=105.0, sweep_tau=0.08, tau=0.07,
              breath=0.08),
        _spec(gen, "noise_burst", "sfx", "Short noise burst", "drums.noise_burst",
              length=0.08, tau=0.03),
    ]
    return specs


# ------------------------------------------------------------------------------ worker

def render_job(args):
    """Render one Spec to disk. Returns ``(index_entry, frames, warnings)``."""
    spec, out_dir = args
    rng = random.Random(S.seed_for(spec.chip, spec.name))
    builder = BUILDERS[spec.builder]
    result = builder(spec.sample_rate, rng, **spec.params)
    if isinstance(result, tuple):
        x, loop = result
        loop_start, loop_end = loop
    else:
        x, loop_start, loop_end = list(result), None, None
    sr = spec.sample_rate
    warnings = []
    if spec.chip == "nes":
        x = S.lowpass1(x, sr, NES_LOWPASS_HZ)
    x = S.remove_dc(x)
    if loop_start is None:
        S.fade(x, 4, max(1, min(S.frames(sr, 0.005), len(x) // 8)))
        if spec.chip == "snes":
            x = S.pad_to_multiple(x, BRR_BLOCK)
    else:
        if loop_start % BRR_BLOCK or loop_end % BRR_BLOCK or loop_end != len(x):
            raise ValueError("%s/%s: loop points not block aligned (%d, %d, len %d)"
                             % (spec.chip, spec.name, loop_start, loop_end, len(x)))
    x = S.normalize(x, -1.0)
    n = len(x)
    if n < CHIP_MIN_FRAMES[spec.chip] or n > CHIP_MAX_FRAMES[spec.chip]:
        warnings.append("%s/%s: %d frames outside the chip range %d..%d" % (
            spec.chip, spec.name, n, CHIP_MIN_FRAMES[spec.chip], CHIP_MAX_FRAMES[spec.chip]))
    rel = "%s/%s.wav" % (spec.chip, spec.name)
    wavio.write_wav(os.path.join(out_dir, spec.chip, spec.name + ".wav"), x, sr)
    entry = {
        "chip": spec.chip,
        "name": spec.name,
        "file": rel,
        "sample_rate": sr,
        "root_note": spec.root_note,
        "loop_start": loop_start,
        "loop_end": loop_end,
        "category": spec.category,
        "description": spec.description,
    }
    return entry, n, warnings


# ------------------------------------------------------------------------ user samples

def _safe_name(stem):
    name = re.sub(r"[^a-z0-9]+", "_", stem.lower()).strip("_")
    return name or "sample"


def import_user_samples(user_dir, out_dir, chips):
    """Copy/resample WAVs from ``user_dir/<chip>/*.wav``. Returns ``(entries, warnings)``."""
    entries = []
    warnings = []
    if not os.path.isdir(user_dir):
        return entries, warnings
    for chip in chips:
        chip_dir = os.path.join(user_dir, chip)
        if not os.path.isdir(chip_dir):
            continue
        for fname in sorted(os.listdir(chip_dir)):
            if not fname.lower().endswith(".wav"):
                continue
            src = os.path.join(chip_dir, fname)
            try:
                samples, in_rate = wavio.read_wav(src)
            except Exception as exc:  # wave.Error, ValueError
                warnings.append("user sample %s skipped: %s" % (src, exc))
                continue
            sr = CHIP_RATES[chip]
            x = S.resample(samples, in_rate, sr)
            x = S.remove_dc(x)
            if chip == "snes":
                x = S.pad_to_multiple(x, BRR_BLOCK)
            x = S.normalize(x, -1.0)
            name = _safe_name(os.path.splitext(fname)[0])
            if len(x) > CHIP_MAX_FRAMES[chip]:
                warnings.append("user sample %s/%s: %d frames exceeds the recommended %d"
                                % (chip, name, len(x), CHIP_MAX_FRAMES[chip]))
            wavio.write_wav(os.path.join(out_dir, chip, name + ".wav"), x, sr)
            entries.append({
                "chip": chip,
                "name": name,
                "file": "%s/%s.wav" % (chip, name),
                "sample_rate": sr,
                "root_note": 60,
                "loop_start": None,
                "loop_end": None,
                "category": "user",
                "description": "User-supplied sample from %s (resampled from %d Hz)"
                               % (fname, in_rate),
            })
    return entries, warnings


# -------------------------------------------------------------------------------- main

def verify(out_dir, entries):
    """Open every indexed WAV and check its format. Returns ``(total_bytes, problems)``."""
    problems = []
    total = 0
    for e in entries:
        path = os.path.join(out_dir, *e["file"].split("/"))
        try:
            channels, width, rate, frames = wavio.wav_info(path)
        except Exception as exc:
            problems.append("%s: cannot open (%s)" % (e["file"], exc))
            continue
        if channels != 1 or width != 2 or rate != e["sample_rate"] or frames <= 0:
            problems.append("%s: unexpected format ch=%d width=%d rate=%d frames=%d"
                            % (e["file"], channels, width, rate, frames))
        if e["loop_end"] is not None and e["loop_end"] > frames:
            problems.append("%s: loop_end %d beyond %d frames" % (e["file"], e["loop_end"], frames))
        total += os.path.getsize(path)
    return total, problems


def main(argv=None):
    parser = argparse.ArgumentParser(description="Generate procedural chip samples.")
    parser.add_argument("--jobs", type=int, default=None, help="worker processes (default: CPUs - 1)")
    parser.add_argument("--chip", choices=sorted(CHIP_RATES), default=None, help="only this chip")
    parser.add_argument("--out", default=DEFAULT_OUT, help="output directory (assets/samples)")
    parser.add_argument("--user-dir", default=DEFAULT_USER_DIR,
                        help="directory with <chip>/*.wav user samples to import")
    args = parser.parse_args(argv)

    out_dir = os.path.abspath(args.out)
    chips = [args.chip] if args.chip else sorted(CHIP_RATES)
    for chip in chips:
        os.makedirs(os.path.join(out_dir, chip), exist_ok=True)

    specs = [s for s in catalogue() if s.chip in chips]
    results = pool.run_ordered(render_job, [(s, out_dir) for s in specs], args.jobs)
    warnings = []
    entries = []
    for entry, _frames, warns in results:
        entries.append(entry)
        warnings.extend(warns)

    user_entries, user_warns = import_user_samples(os.path.abspath(args.user_dir), out_dir, chips)
    warnings.extend(user_warns)
    by_key = {(e["chip"], e["name"]): e for e in entries}
    for e in user_entries:
        by_key[(e["chip"], e["name"])] = e
    entries = sorted(by_key.values(), key=lambda e: (e["chip"], e["name"]))

    index_path = os.path.join(out_dir, "index.json")
    if not args.chip:
        merged = entries
    else:
        # keep other chips' entries when only one chip is regenerated
        merged = []
        if os.path.isfile(index_path):
            with open(index_path, "r", encoding="utf-8") as f:
                merged = [e for e in json.load(f) if e["chip"] not in chips]
        merged = sorted(merged + entries, key=lambda e: (e["chip"], e["name"]))
    with open(index_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(merged, f, indent=2, sort_keys=True)
        f.write("\n")

    total, problems = verify(out_dir, entries)
    counts = {}
    for e in entries:
        counts[e["chip"]] = counts.get(e["chip"], 0) + 1
    for chip in chips:
        print("%s: %d samples" % (chip, counts.get(chip, 0)))
    print("total: %d samples, %.1f KiB in %s" % (len(entries), total / 1024.0, out_dir))
    for w in warnings:
        print("warning: " + w)
    for p in problems:
        print("error: " + p)
    if total > 6 * 1024 * 1024:
        print("error: total size exceeds 6 MB")
        problems.append("size")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
