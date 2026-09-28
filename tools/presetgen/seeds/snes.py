"""SNES (S-DSP) seed patches.

Placeholder: the seed authors fill this module next. Categories and subcategories are
listed in presetgen.model.CATEGORIES["snes"]; parameter keys and bounds in
tools/presetgen/params/snes.json. Sample-based seeds set `samples` to map the `sample`
slot parameter to a name in assets/samples/snes/.
"""

from __future__ import annotations

from presetgen.model import Seed


def get_seeds() -> list[Seed]:
    return []
