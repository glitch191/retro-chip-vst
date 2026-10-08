"""Polyphony headroom of the SNES presets (docs/PRESET_SPECS.md, "Polyphony headroom").

The S-DSP clamps its voice mix, its echo mix and its echo buffer writes to 16 bits, and its
echo FIR adds taps 0-6 with a 16-bit wrap (docs/research/snes.md, "Echo"). A preset whose
voices are loud enough to reach full scale when several notes are held therefore distorts,
and with echo it clicks, exactly as the hardware would. Games avoid it by keeping the voice
volumes low; so do the factory presets.

`chiptool features` renders every SNES preset as four held four-note chords at velocity
127 (C3 G3 C4 E4; the cluster C4 D4 E4 F4, whose beating aligns the peaks; C6 E6 G6 C7, where
notes above the pitch limit play in phase; E2 B2 E3 G#3) and reports `chord_peak`: the largest value any
of those stages would compute without a clamp, as a fraction of full scale. Every stage is linear in the voice
volume until something saturates, so a preset above CHORD_PEAK_TARGET gets its `volume`
scaled down to reach it:

    volume' = max(1, floor(volume * CHORD_PEAK_TARGET / chord_peak))

and its level features are moved by the same ratio (20 log10(volume' / volume) dB) so that
`preset_gain` (presetgen/level.py) restores the playing level after the chip. The margin
below full scale covers other chords and notes than the measured one. The final bank is
checked by `gen_presets.py --verify-features`: every `chord_saturation` count must be 0.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Any, Mapping, Sequence

from presetgen.level import PEAK_KEYS, SILENT_DB
from presetgen.model import Preset

CHIP = "snes"
CHORD_PEAK_TARGET = 0.75     # fraction of full scale left to the measured chords (-2.5 dB)
VOLUME_KEY = "volume"        # voice volume (VxVOL before pan and velocity)
PEAK_KEY = "chord_peak"
SATURATION_KEY = "chord_saturation"


@dataclass
class HeadroomSummary:
    measured: int = 0
    scaled: int = 0
    largest_cut_db: float = 0.0
    cuts_db: list[float] | None = None

    def describe(self) -> str:
        if self.measured == 0:
            return "no chord features"
        text = f"{self.measured} presets measured, {self.scaled} voice volumes lowered"
        if self.scaled:
            cuts = sorted(self.cuts_db or [])
            median = cuts[len(cuts) // 2]
            text += f" (median {median:.1f} dB, largest {self.largest_cut_db:.1f} dB)"
        return text


def scaled_volume(volume: int, chord_peak: float) -> int:
    """Voice volume that brings the measured chord to CHORD_PEAK_TARGET (unchanged below it)."""
    if chord_peak <= CHORD_PEAK_TARGET or volume <= 0:
        return volume
    return max(1, math.floor(volume * CHORD_PEAK_TARGET / chord_peak))


def apply(presets: Sequence[Preset], features: Mapping[str, Any]) -> tuple[dict[str, Any], HeadroomSummary]:
    """Lower the voice volume of the SNES presets above the target (in place).

    Returns the features with the level entries of the changed presets moved by the volume
    ratio, for presetgen.level.assign_preset_gains, and a summary for the QA report."""
    adjusted = dict(features)
    summary = HeadroomSummary(cuts_db=[])
    for preset in presets:
        if preset.chip != CHIP:
            continue
        entry = features.get(preset.name)
        if not isinstance(entry, Mapping) or PEAK_KEY not in entry:
            continue
        summary.measured += 1
        volume = int(preset.params.get(VOLUME_KEY, 0))
        new_volume = scaled_volume(volume, float(entry[PEAK_KEY]))
        if new_volume == volume:
            continue
        preset.params[VOLUME_KEY] = new_volume
        shift_db = 20.0 * math.log10(new_volume / volume)
        moved = dict(entry)
        for key in ("held_rms_db",) + PEAK_KEYS:
            if key in moved and float(moved[key]) > SILENT_DB:
                moved[key] = float(moved[key]) + shift_db
        adjusted[preset.name] = moved
        summary.scaled += 1
        summary.cuts_db.append(-shift_db)
        summary.largest_cut_db = max(summary.largest_cut_db, -shift_db)
    return adjusted, summary


def saturating(features: Mapping[str, Any], names: Sequence[str]) -> list[tuple[str, dict[str, float]]]:
    """Presets among `names` whose chord render saturated (non-zero chord_saturation counts)."""
    found = []
    for name in names:
        entry = features.get(name)
        counts = entry.get(SATURATION_KEY) if isinstance(entry, Mapping) else None
        if isinstance(counts, Mapping) and any(float(v) > 0 for v in counts.values()):
            found.append((name, {k: float(v) for k, v in counts.items() if float(v) > 0}))
    return found
