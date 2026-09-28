"""Preset generation framework for retro-chip-vst.

Modules:
    model   Preset / Seed dataclasses, ParamTable (hardware bounds), Cartesian expansion.
    naming  mechanical preset names built from parameters and axis labels.
    qa      parameter-distance and feature-distance de-duplication plus the QA report.

Entry point: tools/gen_presets.py. Seeds live in tools/presetgen/seeds/<chip>.py.
Standard library only (no numpy).
"""

from presetgen.model import (  # noqa: F401
    CATEGORIES,
    CHIP_DISPLAY_NAMES,
    CHIPS,
    ParamSpec,
    ParamTable,
    Preset,
    PresetError,
    Seed,
    expand,
)

__all__ = [
    "CATEGORIES",
    "CHIP_DISPLAY_NAMES",
    "CHIPS",
    "ParamSpec",
    "ParamTable",
    "Preset",
    "PresetError",
    "Seed",
    "expand",
]
