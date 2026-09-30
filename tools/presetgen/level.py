"""Per-preset playing level: the `preset_gain` global (docs/PRESET_SPECS.md, "Playing level").

The chips' output is hardware-relative (one NES pulse at volume 12 is about -33 dBFS RMS, a
Genesis PSG tone about -45 dBFS), so each preset carries a gain in dB that the plugin applies
after the chip output, together with master_gain. It is derived from the long pass of
`chiptool features` (C4, velocity 100, held 1.4 s), which measures the chip output without
preset_gain:

* held_rms_db: stereo RMS over the 10 ms windows of the hold within 20 dB of the loudest one
  (velocity 100: the loudness target is the playing level);
* peak_db / peak_short_db: largest |sample| of either channel in the long (C4) and short (C3)
  pass at velocity 100, peak_v127_db / peak_short_v127_db the same two renders at velocity
  127, so the ceiling holds at every velocity.

    gain = min(TARGET_RMS_DB - held_rms_db, PEAK_CEILING_DB - max(all four peaks))

rounded to GAIN_STEP_DB, lowered by one step when the rounding would put the peak above the
ceiling, and clamped to the parameter range. Arpeggiator presets are measured as a single
note like the others. A silent render (no level) gets 0 dB.
"""

from __future__ import annotations

import statistics
from dataclasses import dataclass
from typing import Any, Mapping, Sequence

from presetgen.model import Preset

PRESET_GAIN_KEY = "preset_gain"
TARGET_RMS_DB = -18.0      # held note, stereo RMS (full-scale sine = -3 dBFS)
PEAK_CEILING_DB = -1.0     # sample peak of either channel
GAIN_STEP_DB = 0.5
GAIN_MIN_DB = -24.0        # plugin parameter range (plugin/src/Parameters.cpp)
GAIN_MAX_DB = 36.0
SILENT_DB = -100.0         # held_rms_db at or below this: nothing to measure
PEAK_KEYS = ("peak_db", "peak_short_db", "peak_v127_db", "peak_short_v127_db")
REQUIRED_KEYS = ("held_rms_db",) + PEAK_KEYS


@dataclass(frozen=True)
class GainDecision:
    gain_db: float
    limited_by_peak: bool
    silent: bool = False


def preset_gain(entry: Mapping[str, Any]) -> GainDecision:
    """preset_gain (dB) from one `chiptool features` entry."""
    rms = float(entry["held_rms_db"])
    peak = max(float(entry[key]) for key in PEAK_KEYS)
    if rms <= SILENT_DB:
        return GainDecision(0.0, False, silent=True)
    for_rms = TARGET_RMS_DB - rms
    for_peak = PEAK_CEILING_DB - peak
    gain = round(min(for_rms, for_peak) / GAIN_STEP_DB) * GAIN_STEP_DB
    if peak + gain > PEAK_CEILING_DB + 1e-9:
        gain -= GAIN_STEP_DB
    gain = min(GAIN_MAX_DB, max(GAIN_MIN_DB, gain))
    return GainDecision(gain + 0.0, for_peak < for_rms)   # + 0.0: no "-0.0" in the bank


@dataclass
class GainSummary:
    count: int = 0
    minimum: float = 0.0
    median: float = 0.0
    maximum: float = 0.0
    limited_by_peak: int = 0
    silent: int = 0
    missing: int = 0     # presets without (complete) level features: no preset_gain written

    def describe(self) -> str:
        if self.count == 0:
            return "no preset_gain computed (no features)"
        text = (f"{self.count} presets, min {self.minimum:+.1f} dB, median {self.median:+.1f} dB, "
                f"max {self.maximum:+.1f} dB; {self.limited_by_peak} limited by the peak ceiling")
        if self.silent:
            text += f", {self.silent} silent (0 dB)"
        if self.missing:
            text += f", {self.missing} without features (no preset_gain)"
        return text


def assign_preset_gains(presets: Sequence[Preset], features: Mapping[str, Any] | None) -> GainSummary:
    """Write global.preset_gain into every preset that has a features entry (in place)."""
    summary = GainSummary()
    if features is None:
        return summary
    gains: list[float] = []
    for preset in presets:
        entry = features.get(preset.name)
        if not isinstance(entry, Mapping) or any(key not in entry for key in REQUIRED_KEYS):
            summary.missing += 1
            preset.global_params.pop(PRESET_GAIN_KEY, None)
            continue
        decision = preset_gain(entry)
        preset.global_params[PRESET_GAIN_KEY] = decision.gain_db
        gains.append(decision.gain_db)
        summary.limited_by_peak += int(decision.limited_by_peak)
        summary.silent += int(decision.silent)
    if gains:
        summary.count = len(gains)
        summary.minimum = min(gains)
        summary.median = statistics.median(gains)
        summary.maximum = max(gains)
    return summary

