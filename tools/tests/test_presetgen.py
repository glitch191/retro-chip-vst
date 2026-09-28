"""Unit tests for tools/presetgen (run: python -m unittest discover -s tools/tests)."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parents[1]
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from presetgen import naming, qa  # noqa: E402
from presetgen.model import CATEGORIES, CHIPS, ParamTable, Preset, PresetError, Seed, expand, expand_all  # noqa: E402

GEN_PRESETS = TOOLS_DIR / "gen_presets.py"


def nes_lead_seed(**axes) -> Seed:
    return Seed(
        name="Lead",
        category="Lead",
        subcategory="Pulse",
        comment="Test seed.",
        params={"p1_duty": 2, "p1_volume": 12, "p1_sw_decay": 0, "p1_transpose": 0},
        axes=axes,
    )


class ParamTableTests(unittest.TestCase):
    def test_all_tables_load_and_are_consistent(self):
        for chip in CHIPS:
            table = ParamTable.load(chip)
            self.assertGreater(len(table), 10, chip)
            self.assertIn("source", table.meta)
            for spec in table.specs():
                self.assertLessEqual(spec.min, spec.default, spec.key)
                self.assertLessEqual(spec.default, spec.max, spec.key)

    def test_validation_rejects_out_of_range(self):
        table = ParamTable.load("nes")
        with self.assertRaises(PresetError):
            table.validate("p1_duty", 4)
        with self.assertRaises(PresetError):
            table.validate("p1_transpose", -25)
        with self.assertRaises(PresetError):
            table.validate("p1_duty", 1.5)  # integer parameter
        with self.assertRaises(PresetError):
            table.validate("p1_duty", True)
        with self.assertRaises(PresetError):
            table.validate("no_such_key", 0)
        self.assertEqual(table.validate("p1_volume", 12), 12)
        self.assertEqual(table.validate("p1_volume", 15.0), 15)

    def test_expand_rejects_out_of_range_axis_values(self):
        table = ParamTable.load("nes")
        with self.assertRaises(PresetError):
            expand(nes_lead_seed(p1_duty=[0, 1, 2, 3, 4]), table)
        with self.assertRaises(PresetError):
            expand(nes_lead_seed(vibrato=[("slow", {"p1_vibrato_rate": 99})]), table)

    def test_expand_rejects_unknown_category(self):
        table = ParamTable.load("nes")
        seed = nes_lead_seed()
        seed.subcategory = "Nope"
        with self.assertRaises(PresetError):
            expand(seed, table)
        self.assertIn("Pulse", CATEGORIES["nes"]["Lead"])


class ExpansionTests(unittest.TestCase):
    def test_expansion_count_is_product_of_axes(self):
        table = ParamTable.load("nes")
        seed = nes_lead_seed(
            p1_duty=[0, 1, 2, 3],
            p1_transpose=[-12, 0, 12],
            vibrato=[
                ("off", {"p1_vibrato_rate": 0, "p1_vibrato_depth": 0}),
                ("slow", {"p1_vibrato_rate": 8, "p1_vibrato_depth": 2}),
                ("fast", {"p1_vibrato_rate": 3, "p1_vibrato_depth": 3}),
            ],
        )
        presets = expand(seed, table)
        self.assertEqual(len(presets), 4 * 3 * 3)
        # Axis order: first axis slowest; descriptors follow the axis order.
        self.assertEqual(presets[0].descriptors, ["Duty12", "OctDown", ""])
        self.assertEqual(presets[1].descriptors, ["Duty12", "OctDown", "Vibrato Slow"])
        self.assertEqual(presets[-1].descriptors, ["Duty75", "OctUp", "Vibrato Fast"])
        self.assertEqual(presets[-1].params["p1_duty"], 3)
        self.assertEqual(presets[-1].params["p1_vibrato_rate"], 3)
        self.assertEqual(presets[0].params["p1_vibrato_rate"], 0)
        self.assertEqual(presets[0].chip, "nes")
        self.assertIn("duty12", presets[0].tags)

    def test_expand_without_axes_yields_one_preset(self):
        table = ParamTable.load("nes")
        self.assertEqual(len(expand(nes_lead_seed(), table)), 1)


class NamingTests(unittest.TestCase):
    def test_spec_examples(self):
        self.assertEqual(naming.build_name("NES", "Lead", "Lead", ["Duty25", "", "Vibrato Slow"]),
                         "NES Lead Duty25 Vibrato Slow")
        self.assertEqual(naming.build_name("SNES", "Pad", "Strings", ["LongRelease", "Echo3"]),
                         "SNES Pad Strings LongRelease Echo3")
        self.assertEqual(naming.build_name("Genesis", "Lead", "FM Lead", ["Alg4", "LFO Vibrato2"]),
                         "Genesis Lead FM Alg4 LFO Vibrato2")

    def test_descriptors_from_parameters(self):
        self.assertEqual(naming.value_descriptor("p1_duty", 1), "Duty25")
        self.assertEqual(naming.value_descriptor("algorithm", 4), "Alg4")
        self.assertEqual(naming.value_descriptor("echo_delay", 3), "Echo3")
        self.assertEqual(naming.value_descriptor("p1_transpose", 0), "")
        self.assertEqual(naming.value_descriptor("p1_transpose", 12), "OctUp")
        self.assertEqual(naming.value_descriptor("p1_transpose", -24), "OctDown2")
        self.assertEqual(naming.value_descriptor("p1_transpose", 7), "Up7")
        self.assertEqual(naming.value_descriptor("op2_tl", 40), "Op2TL40")
        self.assertEqual(naming.variant_descriptor("vibrato", "slow"), "Vibrato Slow")
        self.assertEqual(naming.variant_descriptor("release", "LongRelease"), "LongRelease")
        self.assertEqual(naming.variant_descriptor("lfo", "LFO Vibrato2"), "LFO Vibrato2")
        self.assertEqual(naming.variant_descriptor("vibrato", "off"), "")

    def test_full_pipeline_name(self):
        table = ParamTable.load("nes")
        seed = nes_lead_seed(p1_duty=[1], vibrato=[("slow", {"p1_vibrato_rate": 8, "p1_vibrato_depth": 2})])
        self.assertEqual(expand(seed, table)[0].name, "NES Lead Duty25 Vibrato Slow")

    def test_names_are_unique_within_a_bank(self):
        # A name that already exists elsewhere in the bank ("A 2") is never reused for a suffix.
        self.assertEqual(naming.make_unique(["A", "A", "B", "A", "A 2"]), ["A", "A 3", "B", "A 4", "A 2"])
        self.assertEqual(naming.make_unique(["A", "A", "A"]), ["A", "A 2", "A 3"])
        table = ParamTable.load("nes")
        # Two seeds producing the same descriptors collide: the second gets an index suffix.
        presets = expand_all([nes_lead_seed(p1_duty=[1]), nes_lead_seed(p1_duty=[1])], table)
        self.assertEqual([p.name for p in presets], ["NES Lead Duty25", "NES Lead Duty25 2"])


class QaTests(unittest.TestCase):
    def make(self, name, **params) -> Preset:
        base = {"p1_duty": 2, "p1_volume": 12, "p1_sw_decay": 20}
        base.update(params)
        return Preset(name=name, chip="nes", category="Lead", subcategory="Pulse", params=base)

    def test_one_step_volume_removed_and_duty_change_kept(self):
        table = ParamTable.load("nes")
        presets = [
            self.make("A"),
            self.make("B", p1_volume=11),   # 1-step volume only -> duplicate of A
            self.make("C", p1_duty=1),      # duty change -> kept
            self.make("D", p1_volume=10),   # 2 steps below A, 1 step below B (removed) -> kept
        ]
        result = qa.deduplicate(presets, table, jobs=1)
        self.assertEqual([p.name for p in result.kept], ["A", "C", "D"])
        self.assertEqual(len(result.removals), 1)
        self.assertEqual((result.removals[0].removed, result.removals[0].kept), ("B", "A"))
        self.assertEqual(result.removals[0].kind, "parameters")
        self.assertIn("p1_volume", result.removals[0].reason)

    def test_one_step_volume_with_another_change_is_kept(self):
        table = ParamTable.load("nes")
        presets = [self.make("A"), self.make("B", p1_volume=11, p1_sw_decay=21)]
        result = qa.deduplicate(presets, table, jobs=1)
        self.assertEqual(len(result.kept), 2)

    def test_time_relative_threshold(self):
        table = ParamTable.load("nes")
        presets = [self.make("A"), self.make("B", p1_sw_decay=22), self.make("C", p1_sw_decay=30)]
        result = qa.deduplicate(presets, table, jobs=1)
        self.assertEqual([p.name for p in result.kept], ["A", "C"])

    def test_lfo_depth_rule(self):
        table = ParamTable.load("genesis")

        def fm(name, **params):
            return Preset(name=name, chip="genesis", category="FM Pad", subcategory="LFO", params=params)

        presets = [fm("A", fms=0), fm("B", fms=1), fm("C", fms=6), fm("D", fms=7)]
        result = qa.deduplicate(presets, table, jobs=1)
        self.assertEqual([p.name for p in result.kept], ["A", "B", "C"])

    def test_feature_distance(self):
        table = ParamTable.load("nes")
        presets = [self.make("A"), self.make("B", p1_duty=1), self.make("C", p1_duty=0)]
        same = {"mel": [-20.0] * 40, "env": [0.5] * 20}
        other = {"mel": [-20.0] * 20 + [-60.0] * 20, "env": [0.5] * 20}
        features = {"A": same, "B": dict(same), "C": other}
        result = qa.deduplicate(presets, table, features=features, jobs=1)
        self.assertEqual([p.name for p in result.kept], ["A", "C"])
        self.assertEqual(result.removals[0].kind, "perceptual")
        self.assertEqual(qa.mel_cosine_distance([-20.0] * 40, [-20.0] * 40), 0.0)

    def test_report_sections_round_trip(self):
        table = ParamTable.load("nes")
        result = qa.deduplicate([self.make("A"), self.make("B", p1_volume=11)], table, jobs=1, seeds=1)
        section = qa.report_section(result)
        self.assertIn("## NES", section)
        self.assertIn("| B | A |", section)
        text = qa.render_report({"nes": section})
        self.assertEqual(qa.parse_report_sections(text)["nes"].strip(), section.strip())


class DeterminismTests(unittest.TestCase):
    def _bank(self, jobs):
        table = ParamTable.load("nes")
        seed = nes_lead_seed(
            p1_duty=[0, 1, 2, 3],
            p1_volume=[12, 11, 8, 4],
            p1_sw_decay=[0, 5, 6, 30],
        )
        presets = expand_all([seed], table)
        self.assertGreaterEqual(len(presets), qa.MIN_PRESETS_FOR_POOL)
        result = qa.deduplicate(presets, table, jobs=jobs, seeds=1)
        bank = sorted(result.kept, key=Preset.sort_key)
        return json.dumps([p.to_dict() for p in bank], sort_keys=True), qa.report_section(result)

    def test_library_runs_are_identical(self):
        first = self._bank(jobs=1)
        second = self._bank(jobs=2)  # exercises the process pool
        self.assertEqual(first, second)
        third = self._bank(jobs=2)
        self.assertEqual(second, third)

    def test_cli_runs_are_byte_identical(self):
        with tempfile.TemporaryDirectory() as tmp:
            outputs = []
            for run in ("a", "b"):
                out_dir = Path(tmp) / run
                report = out_dir / "PRESET_QA.md"
                proc = subprocess.run(
                    [sys.executable, str(GEN_PRESETS), "--chip", "nes", "--jobs", "1", "--out", str(out_dir),
                     "--report", str(report), "--no-target-check"],
                    capture_output=True, text=True, check=False)
                self.assertEqual(proc.returncode, 0, proc.stderr)
                outputs.append(((out_dir / "nes.json").read_bytes(), report.read_bytes()))
            self.assertEqual(outputs[0], outputs[1])
            self.assertIn(b"# Preset QA report", outputs[0][1])


if __name__ == "__main__":
    unittest.main()
