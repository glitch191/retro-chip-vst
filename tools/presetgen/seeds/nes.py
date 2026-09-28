"""NES (2A03) seed patches.

Placeholder: the seed authors fill this module next. Categories and subcategories are
listed in presetgen.model.CATEGORIES["nes"]; parameter keys and bounds in
tools/presetgen/params/nes.json.

Example of a seed:

    Seed(name="Square Lead", category="Lead", subcategory="Pulse",
         comment="Plain 50 % pulse with a short software decay, the classic melody voice.",
         params={"p1_duty": 2, "p1_volume": 12, "p1_sw_decay": 0, ...},
         axes={"p1_duty": [0, 1, 2, 3],
               "p1_transpose": [-12, 0, 12],
               "vibrato": [("off", {"p1_vibrato_rate": 0, "p1_vibrato_depth": 0}),
                           ("slow", {"p1_vibrato_rate": 8, "p1_vibrato_depth": 2, "p1_vibrato_delay": 20}),
                           ("fast", {"p1_vibrato_rate": 3, "p1_vibrato_depth": 2, "p1_vibrato_delay": 10})]})
"""

from __future__ import annotations

from presetgen.model import Seed


def get_seeds() -> list[Seed]:
    return []
