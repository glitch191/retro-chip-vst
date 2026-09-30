"""Perceptual de-duplication and QA report (docs/PRESET_SPECS.md, "Perceptual QA").

Two presets are duplicates when every parameter distance is below its threshold:

* integer register parameters (the default class): identical;
* volume/level parameters (NES volume, TL, PSG attenuation, S-DSP VOL...): a difference
  of at most LEVEL_STEP_THRESHOLD steps counts as "the same" only when no other parameter
  differs at all (a 1-step volume change alone is not a new preset);
* time parameters (frames, ms, ticks, Hz and envelope rate indexes): a relative difference
  under TIME_RELATIVE_THRESHOLD is "the same";
* duty, algorithm, waveform-defining parameters: any change is significant (they are
  register parameters, so the identical rule already covers them);
* AMS / FMS / LFO depth indexes: 0 -> 1 is significant, a change between the two highest
  indexes is not.

When a features JSON produced by `chiptool features` is given (two renders per preset
through the real engine after a 0.4 s pre-roll: C4 held 1.4 s of 2 s, and C3 held 80 ms of
0.5 s), a pair is also a perceptual
duplicate when, in both renders:

* the spectral shape (40 left + 40 right log-mel bands, each vector minus its mean over the
  bands within 60 dB of the loudest band) differs by less than SHAPE_DB_THRESHOLD dB on
  average;
* the overall level differs by less than LEVEL_DB_THRESHOLD dB;
* the 10 ms RMS envelope, in dB relative to its own peak (floored at -60 dB), differs by
  less than ENVELOPE_DB_THRESHOLD dB on average;
* the per-frame pitch track differs by less than PITCH_CENTS_THRESHOLD cents on average
  over the frames voiced in both, and at most VOICING_MISMATCH_THRESHOLD of the voiced
  frames are voiced in only one of them.

The thresholds sit at or below the usual just-noticeable differences (about 1 dB for level
and spectral balance, 3-5 cents for pitch), so the pass only removes variants a listener
cannot tell apart. They were calibrated on the rendered banks (docs/PRESET_QA.md).

Rules that keep a pair apart regardless of features:

* different `global` settings other than poly_channels and preset_gain (arpeggiator, glide):
  the renders play a single note without the arpeggiator, so they cannot see those
  differences (preset_gain is derived from the renders afterwards, presetgen/level.py, and
  the level comparison below measures the chip output without it);
* different values of parameters a single-note render cannot observe (RENDER_BLIND_KEYS:
  echo enable of voices 2-8, pitch modulation between voices, pan of the other FM channels).

The greedy pass keeps the first preset of a duplicate group in expansion order (seed order,
then axis order) so that the seed author's canonical variant survives. Pair comparisons are
batched over a process pool (os.cpu_count() - 1 workers, `--jobs` overrides); the result is
independent of the worker count.
"""

from __future__ import annotations

import math
import os
import re
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Mapping, Sequence

from presetgen.model import CHIP_DISPLAY_NAMES, ParamSpec, ParamTable, Preset

# ----- thresholds ------------------------------------------------------------------------------

# Level parameters: a difference of at most this many hardware steps is inaudible on its own
# (NES 4-bit volume, YM2612 TL, SN76489 attenuation, S-DSP VOL are all fine-grained enough
# that one step alone does not make a new preset).
LEVEL_STEP_THRESHOLD = 1

# Time parameters: relative difference below which two durations/rates sound the same
# (15 % is the documented just-noticeable step for envelope times in this project).
TIME_RELATIVE_THRESHOLD = 0.15

# Feature thresholds (chiptool features, 48 kHz). Calibration on the rendered banks: distinct
# duty cycles differ by > 1.5 dB in shape, a 2-frame vs 4-frame PSG hat by > 2 dB in
# envelope, and the shallowest vibrato profiles by 2.6-5 cents, so all of them stay.
SHAPE_DB_THRESHOLD = 0.5        # mean |shape difference| over active bands, dB
LEVEL_DB_THRESHOLD = 1.0        # overall level difference, dB
# Mean |envelope difference|, dB re each envelope's peak. Kept low because a periodic
# modulation is audible well below its mean difference: YM2612 AMS 1 (1.4 dB peak-to-peak
# tremolo) only moves the mean by 0.4-0.7 dB but is at the detection threshold for 4-6 Hz AM.
ENVELOPE_DB_THRESHOLD = 0.3
PITCH_CENTS_THRESHOLD = 2.0     # mean |pitch difference| over frames voiced in both
VOICING_MISMATCH_THRESHOLD = 0.1
ACTIVE_BAND_RANGE_DB = 60.0     # bands this far below the loudest band are ignored
ENVELOPE_FLOOR_DB = -60.0
# Floor used by chiptool for log-mel energies (dB); bands at or below it are silence.
MEL_FLOOR_DB = -100.0

# Parameters a single-note render on the preset's first channel cannot observe; presets that
# differ in any of them are never removed by the feature rule.
RENDER_BLIND_KEYS: dict[str, tuple[str, ...]] = {
    "nes": (),
    "snes": tuple(f"v{i}_echo" for i in range(2, 9)) + ("pmon",),
    "genesis": tuple(f"fm{i}_pan" for i in range(2, 7)),
}

# Globals that never keep two presets apart: the channel mask, and the playing-level gain
# (presetgen/level.py), which is computed from the renders after the de-duplication.
IGNORED_GLOBALS = frozenset({"poly_channels", "preset_gain"})

# Presets below this count are compared inline: spawning workers would cost more than it saves.
MIN_PRESETS_FOR_POOL = 64


# ----- parameter classes --------------------------------------------------------------------------

class ParamClass(Enum):
    REGISTER = "register"    # identical required
    LEVEL = "level"          # within LEVEL_STEP_THRESHOLD if nothing else differs
    TIME = "time"            # relative difference under TIME_RELATIVE_THRESHOLD
    LFO_DEPTH = "lfo depth"  # AMS / FMS / LFO index rule


_PREFIX = re.compile(r"^(p1|p2|tri|nz|dmc|op[1-4]|fm[1-6]|v[1-8]|psg[123n]?)_")

_LEVEL_SUFFIXES = {"volume", "tl", "att", "direct_level", "main_volume", "echo_volume", "dac_volume"}
_LFO_SUFFIXES = {"ams", "fms", "lfo_freq"}
_TIME_UNITS = {"frames", "ms", "ticks", "Hz"}
# Rate indexes and delays without a time unit in the table but documented as time parameters
# (S-DSP ADSR rates, YM2612 AR/DR/SR/RR, echo delay in 16 ms steps).
_TIME_SUFFIXES = {"attack", "decay", "sustain_rate", "release_rate", "ar", "dr", "sr", "rr", "echo_delay"}


def strip_prefix(key: str) -> str:
    return _PREFIX.sub("", key, count=1)


def classify(spec: ParamSpec) -> ParamClass:
    suffix = strip_prefix(spec.key)
    if suffix in _LEVEL_SUFFIXES:
        return ParamClass.LEVEL
    if suffix in _LFO_SUFFIXES:
        return ParamClass.LFO_DEPTH
    if spec.unit in _TIME_UNITS or suffix in _TIME_SUFFIXES:
        return ParamClass.TIME
    return ParamClass.REGISTER


# ----- pair comparison ------------------------------------------------------------------------------

def _fmt(value: float | int) -> str:
    number = float(value)
    return str(int(number)) if number.is_integer() else f"{number:g}"


def compare_vectors(a: Sequence[float], b: Sequence[float], keys: Sequence[str],
                    classes: Sequence[ParamClass], maxes: Sequence[float]) -> tuple[bool, str]:
    """(is_duplicate, reason). `a`/`b` are full parameter vectors in `keys` order."""
    level_steps: list[str] = []
    other_diffs: list[str] = []
    for i, key in enumerate(keys):
        x, y = a[i], b[i]
        if x == y:
            continue
        cls = classes[i]
        if cls is ParamClass.LEVEL:
            if abs(x - y) <= LEVEL_STEP_THRESHOLD:
                level_steps.append(f"{key} {_fmt(x)} -> {_fmt(y)} (level within {LEVEL_STEP_THRESHOLD} step)")
                continue
            return False, ""
        if cls is ParamClass.TIME:
            relative = abs(x - y) / max(abs(x), abs(y))
            if relative < TIME_RELATIVE_THRESHOLD:
                other_diffs.append(
                    f"{key} {_fmt(x)} -> {_fmt(y)} (time, {relative * 100:.1f} % < {TIME_RELATIVE_THRESHOLD * 100:.0f} %)"
                )
                continue
            return False, ""
        if cls is ParamClass.LFO_DEPTH:
            top = maxes[i]
            if {x, y} == {top - 1, top}:
                other_diffs.append(f"{key} {_fmt(x)} -> {_fmt(y)} (LFO depth between the two highest indexes)")
                continue
            return False, ""
        return False, ""
    if level_steps and other_diffs:
        # A 1-step level change combined with any other change is a new preset.
        return False, ""
    diffs = level_steps + other_diffs
    return True, "; ".join(diffs) if diffs else "identical parameters"


def spectral_distance(a: Sequence[float], b: Sequence[float]) -> tuple[float, float]:
    """(shape dB, level dB) between two log-mel vectors of the same length.

    Only bands within ACTIVE_BAND_RANGE_DB of the loudest band of either vector count; each
    vector's mean over those bands is its level, and the shape is what remains.
    """
    n = min(len(a), len(b))
    if n == 0:
        return 0.0, 0.0
    top = max(max(a[:n]), max(b[:n]))
    if top <= MEL_FLOOR_DB:
        return 0.0, 0.0   # both silent
    active = [i for i in range(n) if max(a[i], b[i]) > top - ACTIVE_BAND_RANGE_DB]
    level_a = sum(a[i] for i in active) / len(active)
    level_b = sum(b[i] for i in active) / len(active)
    shape = sum(abs((a[i] - level_a) - (b[i] - level_b)) for i in active) / len(active)
    return shape, abs(level_a - level_b)


def envelope_distance(a: Sequence[float], b: Sequence[float]) -> float:
    """Mean |difference| (dB) of two RMS envelopes, each relative to its own peak."""
    n = min(len(a), len(b))
    if n == 0:
        return 0.0

    def to_db(env: Sequence[float]) -> list[float]:
        peak = max(max(env[:n]), 1e-9)
        return [max(20.0 * math.log10(max(x, 1e-9) / peak), ENVELOPE_FLOOR_DB) for x in env[:n]]

    da, db = to_db(a), to_db(b)
    return sum(abs(x - y) for x, y in zip(da, db)) / n


def pitch_distance(a: Sequence[float], b: Sequence[float]) -> tuple[float, float]:
    """(mean |cents| over frames voiced in both, fraction of voiced frames voiced in one only)."""
    both = [(x, y) for x, y in zip(a, b) if x > 0 and y > 0]
    voiced = sum(1 for x, y in zip(a, b) if x > 0 or y > 0)
    single = sum(1 for x, y in zip(a, b) if (x > 0) != (y > 0))
    cents = sum(abs(x - y) for x, y in both) / len(both) if both else 0.0
    return cents, (single / voiced if voiced else 0.0)


def feature_distances(fa: Mapping[str, Any], fb: Mapping[str, Any]) -> tuple[float, float, float, float, float]:
    """Worst case over both renders: (shape dB, level dB, envelope dB, pitch cents, voicing)."""
    worst = [0.0, 0.0, 0.0, 0.0, 0.0]
    for suffix in ("", "_short"):
        if f"mel{suffix}" not in fa or f"mel{suffix}" not in fb:
            continue
        shape, level = spectral_distance(fa[f"mel{suffix}"], fb[f"mel{suffix}"])
        env = envelope_distance(fa.get(f"env{suffix}", ()), fb.get(f"env{suffix}", ()))
        cents, voicing = pitch_distance(fa.get(f"pitch{suffix}", ()), fb.get(f"pitch{suffix}", ()))
        for i, v in enumerate((shape, level, env, cents, voicing)):
            worst[i] = max(worst[i], v)
    return worst[0], worst[1], worst[2], worst[3], worst[4]


def compare_features(fa: Mapping[str, Any] | None, fb: Mapping[str, Any] | None) -> tuple[bool, str]:
    if fa is None or fb is None:
        return False, ""
    shape, level, env, cents, voicing = feature_distances(fa, fb)
    if (shape < SHAPE_DB_THRESHOLD and level < LEVEL_DB_THRESHOLD and env < ENVELOPE_DB_THRESHOLD
            and cents < PITCH_CENTS_THRESHOLD and voicing < VOICING_MISMATCH_THRESHOLD):
        return True, (f"rendered: shape {shape:.2f} dB, level {level:.2f} dB, envelope {env:.2f} dB, "
                      f"pitch {cents:.1f} cents, voicing mismatch {voicing:.2f}")
    return False, ""


# ----- worker state (process pool) ----------------------------------------------------------------

_W: dict[str, Any] = {}


def _init_worker(vectors, keys, classes, maxes, features, globals_=None, samples=None, blind=None) -> None:
    _W["vectors"] = vectors
    _W["keys"] = keys
    _W["classes"] = classes
    _W["maxes"] = maxes
    _W["features"] = features
    _W["globals"] = globals_ if globals_ is not None else [()] * len(vectors)
    _W["samples"] = samples if samples is not None else [()] * len(vectors)
    _W["blind"] = blind if blind is not None else [()] * len(vectors)


def _compare_rows(rows: Sequence[int]) -> list[tuple[int, int, str, str]]:
    """For every row i in `rows`, every j > i that duplicates i: (j, i, kind, reason).

    Presets whose `global` settings differ (arpeggiator, glide; poly_channels and preset_gain
    excluded, IGNORED_GLOBALS) are
    never duplicates: `chiptool features` renders a single note without the arpeggiator, so
    their renders cannot tell them apart. Presets that load different samples are compared
    by their rendered features only (the slot index alone does not identify the sample).
    Presets that differ in a RENDER_BLIND_KEYS parameter are compared by parameters only.
    """
    vectors, keys, classes, maxes, features = (_W["vectors"], _W["keys"], _W["classes"], _W["maxes"],
                                                _W["features"])
    globals_, samples, blind = _W["globals"], _W["samples"], _W["blind"]
    n = len(vectors)
    out: list[tuple[int, int, str, str]] = []
    for i in rows:
        vi = vectors[i]
        fi = features[i]
        for j in range(i + 1, n):
            if globals_[i] != globals_[j]:
                continue
            if samples[i] == samples[j]:
                duplicate, reason = compare_vectors(vi, vectors[j], keys, classes, maxes)
                if duplicate:
                    out.append((j, i, "parameters", reason))
                    continue
            if fi is not None and blind[i] == blind[j]:
                duplicate, reason = compare_features(fi, features[j])
                if duplicate:
                    out.append((j, i, "perceptual", reason))
    return out


# ----- de-duplication ----------------------------------------------------------------------------------

@dataclass
class Removal:
    removed: str
    kept: str
    kind: str      # "parameters" or "perceptual"
    reason: str


@dataclass
class QaResult:
    chip: str
    seeds: int
    before: int
    kept: list[Preset] = field(default_factory=list)
    removals: list[Removal] = field(default_factory=list)
    features_available: bool = False
    features_missing: int = 0  # presets without an entry in the features file
    gain_summary: str = ""     # preset_gain distribution (presetgen/level.py), set by gen_presets

    @property
    def after(self) -> int:
        return len(self.kept)

    @property
    def removed_by_parameters(self) -> int:
        return sum(1 for r in self.removals if r.kind == "parameters")

    @property
    def removed_by_features(self) -> int:
        return sum(1 for r in self.removals if r.kind == "perceptual")


def default_jobs() -> int:
    return max(1, (os.cpu_count() or 2) - 1)


def deduplicate(presets: Sequence[Preset], table: ParamTable, features: Mapping[str, Any] | None = None,
                jobs: int | None = None, seeds: int = 0) -> QaResult:
    """Greedy de-duplication in list order; returns kept presets (same order) and the removals."""
    result = QaResult(chip=table.chip, seeds=seeds, before=len(presets), features_available=features is not None)
    if not presets:
        return result

    keys = table.keys()
    specs = [table[key] for key in keys]
    classes = [classify(spec) for spec in specs]
    maxes = [spec.max for spec in specs]
    defaults = table.defaults()
    vectors = [tuple(float(p.params.get(key, defaults[key])) for key in keys) for p in presets]
    globals_ = [tuple(sorted((k, float(v)) for k, v in p.global_params.items() if k not in IGNORED_GLOBALS))
                for p in presets]
    sample_maps = [tuple(sorted(p.samples.items())) for p in presets]
    blind_keys = RENDER_BLIND_KEYS.get(table.chip, ())
    blind = [tuple(float(p.params.get(k, defaults.get(k, 0.0))) for k in blind_keys) for p in presets]

    feature_list: list[Mapping[str, Any] | None] = [None] * len(presets)
    if features is not None:
        for i, preset in enumerate(presets):
            entry = features.get(preset.name)
            if isinstance(entry, Mapping):
                feature_list[i] = entry
            else:
                result.features_missing += 1

    n = len(presets)
    jobs = default_jobs() if jobs is None else max(1, int(jobs))
    if jobs == 1 or n < MIN_PRESETS_FOR_POOL:
        _init_worker(vectors, keys, classes, maxes, feature_list, globals_, sample_maps, blind)
        pairs = _compare_rows(range(n))
    else:
        # Interleaved row chunks balance the triangular workload across workers.
        chunk_count = min(n, jobs * 4)
        chunks = [list(range(k, n, chunk_count)) for k in range(chunk_count)]
        pairs = []
        with ProcessPoolExecutor(max_workers=jobs, initializer=_init_worker,
                                 initargs=(vectors, keys, classes, maxes, feature_list, globals_,
                                           sample_maps, blind)) as pool:
            for part in pool.map(_compare_rows, chunks):
                pairs.extend(part)

    # j -> candidates (i, kind, reason) with i < j, sorted by i so that the earliest kept wins.
    candidates: dict[int, list[tuple[int, str, str]]] = {}
    for j, i, kind, reason in sorted(pairs):
        candidates.setdefault(j, []).append((i, kind, reason))

    kept_indexes: set[int] = set()
    for j in range(n):
        match = None
        for i, kind, reason in candidates.get(j, ()):
            if i in kept_indexes:
                match = (i, kind, reason)
                break
        if match is None:
            kept_indexes.add(j)
            result.kept.append(presets[j])
        else:
            i, kind, reason = match
            result.removals.append(Removal(presets[j].name, presets[i].name, kind, reason))
    return result


# ----- report ---------------------------------------------------------------------------------------

TARGETS = {"nes": (250, 350), "snes": (250, 350), "genesis": (300, 400)}

REPORT_TITLE = "# Preset QA report"
REPORT_INTRO = ("Generated by `tools/gen_presets.py` (perceptual de-duplication and the preset_gain "
                "playing level, see `docs/PRESET_SPECS.md`). Do not edit by hand.")


def _cell(text: str) -> str:
    return text.replace("|", "\\|")


def report_section(result: QaResult) -> str:
    chip_name = CHIP_DISPLAY_NAMES[result.chip]
    low, high = TARGETS[result.chip]
    status = "within target" if low <= result.after <= high else "OUT OF TARGET"
    lines = [
        f"## {chip_name}",
        "",
        f"* Seeds: {result.seeds}",
        f"* Expanded presets: {result.before}",
        f"* Removed as parameter duplicates: {result.removed_by_parameters}",
    ]
    if result.features_available:
        missing = f" ({result.features_missing} presets without features)" if result.features_missing else ""
        lines.append(f"* Removed as perceptual duplicates: {result.removed_by_features}{missing}")
    else:
        lines.append("* Removed as perceptual duplicates: 0 (no features file; run `chiptool features` "
                     "and pass `--features`)")
    lines.append(f"* Final: {result.after} (target {low}-{high}, {status})")
    if result.gain_summary:
        lines.append(f"* preset_gain: {result.gain_summary}")
    lines.append("")
    lines.append("### Removed presets")
    lines.append("")
    if result.removals:
        lines.append("| Removed | Kept | Rule | Reason |")
        lines.append("| --- | --- | --- | --- |")
        for removal in result.removals:
            lines.append(f"| {_cell(removal.removed)} | {_cell(removal.kept)} | {removal.kind} | "
                         f"{_cell(removal.reason)} |")
    else:
        lines.append("None.")
    lines.append("")
    return "\n".join(lines)


def render_report(sections: Mapping[str, str]) -> str:
    """Full report text from per-chip section texts (keyed by chip key, ordered NES/SNES/Genesis)."""
    parts = [REPORT_TITLE, "", REPORT_INTRO, ""]
    for chip in CHIP_DISPLAY_NAMES:
        if chip in sections:
            parts.append(sections[chip].rstrip("\n"))
            parts.append("")
    return "\n".join(parts).rstrip("\n") + "\n"


def parse_report_sections(text: str) -> dict[str, str]:
    """Split an existing report into {chip key: section text} (unknown sections are dropped)."""
    by_name = {name: chip for chip, name in CHIP_DISPLAY_NAMES.items()}
    sections: dict[str, str] = {}
    current: str | None = None
    buffer: list[str] = []
    for line in text.splitlines():
        if line.startswith("## "):
            if current is not None:
                sections[current] = "\n".join(buffer).rstrip("\n") + "\n"
            current = by_name.get(line[3:].strip())
            buffer = [line]
        elif current is not None:
            buffer.append(line)
    if current is not None:
        sections[current] = "\n".join(buffer).rstrip("\n") + "\n"
    return sections
