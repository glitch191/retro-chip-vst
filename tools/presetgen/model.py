"""Preset model: parameter tables, seeds, presets and Cartesian expansion.

The parameter tables in tools/presetgen/params/<chip>.json carry the hardware bounds
exported by `chiptool dump-params` (hand-transcribed from docs/ENGINE_SPECS.md until the
engines exist). Every value that ends up in a preset is validated against them, so the
generator never emits a register value the hardware cannot hold.

Seed axes come in two shapes:

    {"p1_duty": [0, 1, 2, 3]}                              scalar axis: one parameter, many values
    {"vibrato": [("off", {...}), ("slow", {...})]}          variant axis: named parameter sets

`expand(seed, table)` produces one Preset per element of the Cartesian product of the
axes, in axis order (first axis slowest), with descriptor labels in the same order.
"""

from __future__ import annotations

import itertools
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Mapping, Sequence

from presetgen import naming

CHIPS = ("nes", "snes", "genesis")
CHIP_DISPLAY_NAMES = {"nes": "NES", "snes": "SNES", "genesis": "Genesis"}
PARAMS_DIR = Path(__file__).resolve().parent / "params"

# Category -> allowed subcategories (docs/PRESET_SPECS.md). An empty tuple means the
# category has no documented subcategory: any string (including "") is accepted.
CATEGORIES: dict[str, dict[str, tuple[str, ...]]] = {
    "nes": {
        "Lead": ("Pulse", "Vibrato", "Sweep", "Octave"),
        "Bass": ("Triangle", "TriangleGated", "Pulse"),
        "Drums": ("Kick", "Snare", "HiHat", "DMC"),
        "Arp": ("Triad", "Interval"),
        "SFX": ("SweepUp", "SweepDown", "Noise", "Zap"),
    },
    "snes": {
        "Instrument": ("Piano", "Strings", "Brass", "Guitar", "Woodwind", "Organ"),
        "Bass": ("Sample", "Short"),
        "Pad": ("Echo", "Strings", "Choir"),
        "Drums": ("Kit", "Velocity"),
        "SFX": ("Noise", "PMON"),
    },
    "genesis": {
        "FM Bass": ("Alg", "Feedback", "Detune"),
        "FM Keys": ("EPiano", "Bell"),
        "FM Brass": ("Stab", "Brass", "SSG"),
        "FM Pad": ("Slow", "Unison", "LFO"),
        "DAC": ("Drums",),
        "PSG Lead": ("Solo", "Doubled"),
        "PSG Bass": (),
        "PSG Drums": ("Periodic", "White"),
    },
}


class PresetError(ValueError):
    """Raised for any invalid seed, axis value or preset."""


@dataclass(frozen=True)
class ParamSpec:
    """One engine parameter in native hardware units (mirrors chipdsp::ParamDesc)."""

    key: str
    min: float
    max: float
    default: float
    is_integer: bool
    unit: str = ""
    group: str = ""
    name: str = ""
    labels: tuple[str, ...] | None = None

    @property
    def is_enumerated(self) -> bool:
        return self.labels is not None


class ParamTable:
    """Parameter bounds for one chip, loaded from tools/presetgen/params/<chip>.json."""

    def __init__(self, chip: str, specs: Sequence[ParamSpec], meta: Mapping[str, Any] | None = None):
        self.chip = chip
        self.meta = dict(meta or {})
        self._specs: dict[str, ParamSpec] = {}
        for spec in specs:
            if spec.key in self._specs:
                raise PresetError(f"{chip}: duplicate parameter key {spec.key!r}")
            self._specs[spec.key] = spec

    @classmethod
    def load(cls, chip: str, path: Path | str | None = None) -> "ParamTable":
        if chip not in CHIPS:
            raise PresetError(f"unknown chip {chip!r} (expected one of {', '.join(CHIPS)})")
        file = Path(path) if path is not None else PARAMS_DIR / f"{chip}.json"
        with open(file, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
        specs = []
        for entry in doc["params"]:
            labels = entry.get("labels")
            specs.append(
                ParamSpec(
                    key=str(entry["key"]),
                    min=float(entry["min"]),
                    max=float(entry["max"]),
                    default=float(entry["default"]),
                    is_integer=bool(entry["isInteger"]),
                    unit=str(entry.get("unit", "")),
                    group=str(entry.get("group", "")),
                    name=str(entry.get("name", entry["key"])),
                    labels=tuple(str(label) for label in labels) if labels is not None else None,
                )
            )
        table = cls(chip, specs, doc.get("meta"))
        table.check_consistency()
        return table

    def check_consistency(self) -> None:
        """Sanity checks on the table itself (bounds, defaults, label counts)."""
        for spec in self._specs.values():
            if spec.min > spec.max:
                raise PresetError(f"{self.chip}: {spec.key} has min {spec.min} > max {spec.max}")
            if not (spec.min <= spec.default <= spec.max):
                raise PresetError(f"{self.chip}: {spec.key} default {spec.default} outside [{spec.min}, {spec.max}]")
            if spec.labels is not None:
                if not spec.is_integer:
                    raise PresetError(f"{self.chip}: {spec.key} has labels but is not an integer parameter")
                expected = int(spec.max - spec.min) + 1
                if len(spec.labels) != expected:
                    raise PresetError(
                        f"{self.chip}: {spec.key} has {len(spec.labels)} labels, expected {expected}"
                    )

    # ----- lookup -----------------------------------------------------------------------------
    def __contains__(self, key: str) -> bool:
        return key in self._specs

    def __getitem__(self, key: str) -> ParamSpec:
        try:
            return self._specs[key]
        except KeyError:
            raise PresetError(f"{self.chip}: unknown parameter {key!r}") from None

    def __len__(self) -> int:
        return len(self._specs)

    def keys(self) -> list[str]:
        """Parameter keys in engine order."""
        return list(self._specs.keys())

    def specs(self) -> list[ParamSpec]:
        return list(self._specs.values())

    def defaults(self) -> dict[str, float | int]:
        return {key: self.normalise(key, spec.default) for key, spec in self._specs.items()}

    # ----- validation -------------------------------------------------------------------------
    def normalise(self, key: str, value: Any) -> float | int:
        """Return `value` as int for integer parameters, float otherwise (no range check)."""
        spec = self[key]
        if spec.is_integer:
            return int(round(float(value)))
        return float(value)

    def validate(self, key: str, value: Any, context: str = "") -> float | int:
        """Check `value` against the bounds of `key`; return the normalised value."""
        where = f" ({context})" if context else ""
        spec = self[key]
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise PresetError(f"{self.chip}: {key} = {value!r} is not a number{where}")
        number = float(value)
        if number != number:  # NaN
            raise PresetError(f"{self.chip}: {key} is NaN{where}")
        if spec.is_integer and number != int(number):
            raise PresetError(f"{self.chip}: {key} = {value!r} must be an integer{where}")
        if not (spec.min <= number <= spec.max):
            raise PresetError(
                f"{self.chip}: {key} = {value!r} outside hardware range [{_fmt(spec.min)}, {_fmt(spec.max)}]{where}"
            )
        return int(number) if spec.is_integer else number

    def validate_params(self, params: Mapping[str, Any], context: str = "") -> dict[str, float | int]:
        """Validate every entry of `params`; return a new dict with normalised values."""
        return {key: self.validate(key, value, context) for key, value in params.items()}


def _fmt(value: float) -> str:
    return str(int(value)) if float(value).is_integer() else repr(value)


@dataclass
class Preset:
    """One preset document (docs/PLUGIN_SPECS.md, "Presets")."""

    name: str
    chip: str
    category: str
    subcategory: str
    params: dict[str, float | int] = field(default_factory=dict)
    tags: list[str] = field(default_factory=list)
    global_params: dict[str, float | int] = field(default_factory=dict)  # "global" in JSON
    samples: dict[str, str] = field(default_factory=dict)
    # Bookkeeping, not serialised: the seed this preset came from and its axis labels
    # in axis order ("" for an axis value that contributes no descriptor).
    seed_name: str = ""
    descriptors: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "chip": self.chip,
            "category": self.category,
            "subcategory": self.subcategory,
            "tags": list(self.tags),
            "params": dict(sorted(self.params.items())),
            "global": dict(sorted(self.global_params.items())),
            "samples": dict(sorted(self.samples.items())),
        }

    @classmethod
    def from_dict(cls, doc: Mapping[str, Any]) -> "Preset":
        return cls(
            name=str(doc["name"]),
            chip=str(doc["chip"]),
            category=str(doc.get("category", "")),
            subcategory=str(doc.get("subcategory", "")),
            params=dict(doc.get("params", {})),
            tags=list(doc.get("tags", [])),
            global_params=dict(doc.get("global", {})),
            samples=dict(doc.get("samples", {})),
        )

    def sort_key(self) -> tuple[str, str, str]:
        return (self.category, self.subcategory, self.name)


# A variant axis value: (label, {param key: value, ...}).
Variant = tuple[str, Mapping[str, Any]]


@dataclass
class Seed:
    """A curated seed patch plus its variation axes (docs/PRESET_SPECS.md)."""

    name: str
    category: str
    subcategory: str
    params: dict[str, Any]
    axes: dict[str, Sequence[Any]] = field(default_factory=dict)
    tags: list[str] = field(default_factory=list)
    global_params: dict[str, Any] = field(default_factory=dict)
    samples: dict[str, str] = field(default_factory=dict)
    comment: str = ""  # intended sound, for the seed author's own documentation


def is_variant_axis(values: Sequence[Any]) -> bool:
    """True when every value is a (label, {key: value}) pair."""
    if len(values) == 0:
        return False
    return all(
        isinstance(v, (tuple, list)) and len(v) == 2 and isinstance(v[0], str) and isinstance(v[1], Mapping)
        for v in values
    )


def validate_seed(seed: Seed, table: ParamTable) -> None:
    """Fail loudly on anything the hardware or the taxonomy cannot accept."""
    chip = table.chip
    where = f"seed {seed.name!r}"
    if not seed.name.strip():
        raise PresetError(f"{chip}: a seed has an empty name")
    categories = CATEGORIES[chip]
    if seed.category not in categories:
        raise PresetError(f"{chip}: {where}: unknown category {seed.category!r}")
    allowed = categories[seed.category]
    if allowed and seed.subcategory not in allowed:
        raise PresetError(
            f"{chip}: {where}: unknown subcategory {seed.subcategory!r} for {seed.category} "
            f"(expected one of {', '.join(allowed)})"
        )
    table.validate_params(seed.params, where)
    for axis, values in seed.axes.items():
        if len(values) == 0:
            raise PresetError(f"{chip}: {where}: axis {axis!r} is empty")
        if is_variant_axis(values):
            labels = [label for label, _ in values]
            if len(set(labels)) != len(labels):
                raise PresetError(f"{chip}: {where}: axis {axis!r} has duplicate variant labels")
            for label, overrides in values:
                table.validate_params(overrides, f"{where}, axis {axis}, variant {label!r}")
        else:
            if axis not in table:
                raise PresetError(
                    f"{chip}: {where}: axis {axis!r} is neither a parameter key nor a list of (label, params) variants"
                )
            if len(set(values)) != len(values):
                raise PresetError(f"{chip}: {where}: axis {axis!r} has duplicate values")
            for value in values:
                table.validate(axis, value, f"{where}, axis {axis}")


def expand(seed: Seed, table: ParamTable) -> list[Preset]:
    """Cartesian product of the seed's axes -> presets with provisional names.

    Names are made unique across a bank by naming.assign_unique_names().
    """
    validate_seed(seed, table)
    base_params = table.validate_params(seed.params)
    axis_names = list(seed.axes.keys())
    choices: list[list[Any]] = [list(seed.axes[axis]) for axis in axis_names]
    variant_flags = [is_variant_axis(values) for values in choices]

    presets: list[Preset] = []
    for combination in itertools.product(*choices):
        params = dict(base_params)
        descriptors: list[str] = []
        for axis, is_variant, choice in zip(axis_names, variant_flags, combination):
            if is_variant:
                label, overrides = choice
                params.update(table.validate_params(overrides))
                descriptors.append(naming.variant_descriptor(axis, label))
            else:
                params[axis] = table.validate(axis, choice)
                descriptors.append(naming.value_descriptor(axis, params[axis]))
        tags = sorted(set(seed.tags) | set(naming.descriptor_tags(descriptors)))
        presets.append(
            Preset(
                name=naming.build_name(seed_chip_name(table.chip), seed.category, seed.name, descriptors),
                chip=table.chip,
                category=seed.category,
                subcategory=seed.subcategory,
                params=params,
                tags=tags,
                global_params=dict(seed.global_params),
                samples=dict(seed.samples),
                seed_name=seed.name,
                descriptors=descriptors,
            )
        )
    return presets


def seed_chip_name(chip: str) -> str:
    return CHIP_DISPLAY_NAMES[chip]


def expand_all(seeds: Sequence[Seed], table: ParamTable) -> list[Preset]:
    """Expand every seed in order and make the names unique within the bank."""
    presets: list[Preset] = []
    for seed in seeds:
        presets.extend(expand(seed, table))
    naming.assign_unique_names(presets)
    return presets
