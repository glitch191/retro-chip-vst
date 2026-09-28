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

When a features JSON produced by `chiptool features` is given, a pair is also a duplicate
when the cosine distance of the 40 log-mel bands is under MEL_COSINE_THRESHOLD and the mean
L1 distance of the 20-point RMS envelope is under ENVELOPE_L1_THRESHOLD.

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

# Feature-vector thresholds (chiptool features: 1 s C4 render, 48 kHz).
# Cosine distance over the 40 log-mel bands after shifting them above MEL_FLOOR_DB, so that
# the vectors are non-negative and silence maps to the zero vector. 0.02 corresponds to a
# spectral tilt change too small to hear on chip waveforms; distinct duty cycles are > 0.1.
MEL_COSINE_THRESHOLD = 0.02
# Mean absolute difference of the 20 RMS envelope points (linear amplitude, 0..1).
# 0.02 is below one 4-bit volume step at full scale (1/15 = 0.067).
ENVELOPE_L1_THRESHOLD = 0.02
# Floor used by chiptool for log-mel energies (dB); bands at or below it are silence.
MEL_FLOOR_DB = -100.0

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


def mel_cosine_distance(a: Sequence[float], b: Sequence[float]) -> float:
    """Cosine distance of two log-mel vectors shifted above MEL_FLOOR_DB (0 = identical)."""
    ua = [max(x - MEL_FLOOR_DB, 0.0) for x in a]
    ub = [max(x - MEL_FLOOR_DB, 0.0) for x in b]
    dot = sum(x * y for x, y in zip(ua, ub))
    na = math.sqrt(sum(x * x for x in ua))
    nb = math.sqrt(sum(x * x for x in ub))
    if na == 0.0 and nb == 0.0:
        return 0.0
    if na == 0.0 or nb == 0.0:
        return 1.0
    return max(0.0, 1.0 - dot / (na * nb))


def envelope_l1_distance(a: Sequence[float], b: Sequence[float]) -> float:
    """Mean absolute difference of two RMS envelopes."""
    n = min(len(a), len(b))
    if n == 0:
        return 0.0
    return sum(abs(a[i] - b[i]) for i in range(n)) / n


def compare_features(fa: Mapping[str, Any] | None, fb: Mapping[str, Any] | None) -> tuple[bool, str]:
    if fa is None or fb is None:
        return False, ""
    mel = mel_cosine_distance(fa.get("mel", ()), fb.get("mel", ()))
    env = envelope_l1_distance(fa.get("env", ()), fb.get("env", ()))
    if mel < MEL_COSINE_THRESHOLD and env < ENVELOPE_L1_THRESHOLD:
        return True, (f"mel cosine distance {mel:.4f} < {MEL_COSINE_THRESHOLD} and envelope L1 "
                      f"{env:.4f} < {ENVELOPE_L1_THRESHOLD}")
    return False, ""


# ----- worker state (process pool) ----------------------------------------------------------------

_W: dict[str, Any] = {}


def _init_worker(vectors, keys, classes, maxes, features) -> None:
    _W["vectors"] = vectors
    _W["keys"] = keys
    _W["classes"] = classes
    _W["maxes"] = maxes
    _W["features"] = features


def _compare_rows(rows: Sequence[int]) -> list[tuple[int, int, str, str]]:
    """For every row i in `rows`, every j > i that duplicates i: (j, i, kind, reason)."""
    vectors, keys, classes, maxes, features = (_W["vectors"], _W["keys"], _W["classes"], _W["maxes"],
                                                _W["features"])
    n = len(vectors)
    out: list[tuple[int, int, str, str]] = []
    for i in rows:
        vi = vectors[i]
        fi = features[i]
        for j in range(i + 1, n):
            duplicate, reason = compare_vectors(vi, vectors[j], keys, classes, maxes)
            if duplicate:
                out.append((j, i, "parameters", reason))
                continue
            if fi is not None:
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
        _init_worker(vectors, keys, classes, maxes, feature_list)
        pairs = _compare_rows(range(n))
    else:
        # Interleaved row chunks balance the triangular workload across workers.
        chunk_count = min(n, jobs * 4)
        chunks = [list(range(k, n, chunk_count)) for k in range(chunk_count)]
        pairs = []
        with ProcessPoolExecutor(max_workers=jobs, initializer=_init_worker,
                                 initargs=(vectors, keys, classes, maxes, feature_list)) as pool:
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
REPORT_INTRO = ("Generated by `tools/gen_presets.py` (perceptual de-duplication, see "
                "`docs/PRESET_SPECS.md`). Do not edit by hand.")


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
