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

from presetgen import headroom, level, naming, qa  # noqa: E402
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
        voiced = [9100.0] * 10
        same = {"mel": [-20.0] * 80, "env": [0.5] * 120, "pitch": voiced}
        other = {"mel": [-20.0] * 40 + [-50.0] * 40, "env": [0.5] * 120, "pitch": voiced}
        features = {"A": same, "B": dict(same), "C": other}
        result = qa.deduplicate(presets, table, features=features, jobs=1)
        self.assertEqual([p.name for p in result.kept], ["A", "C"])
        self.assertEqual(result.removals[0].kind, "perceptual")

    def test_feature_distance_components(self):
        # A pure 3 dB level offset is level, not shape.
        shape, level = qa.spectral_distance([-20.0] * 40, [-23.0] * 40)
        self.assertAlmostEqual(shape, 0.0)
        self.assertAlmostEqual(level, 3.0)
        # A 2-point envelope difference of 60 dB over 120 points is 1 dB on average.
        a = [1.0] * 120
        b = [1.0] * 118 + [0.0, 0.0]
        self.assertAlmostEqual(qa.envelope_distance(a, b), 1.0)
        # Vibrato vs no vibrato: +-4 cents alternating -> 4 cents mean; unvoiced frames ignored.
        cents, voicing = qa.pitch_distance([9100.0, 9104.0, 9096.0, 0.0], [9100.0, 9100.0, 9100.0, 0.0])
        self.assertAlmostEqual(cents, 8.0 / 3.0)
        self.assertEqual(voicing, 0.0)
        # A variant that differs in a render-blind key (SNES voice-2 echo) is never removed by
        # features, even with identical renders.
        table = ParamTable.load("snes")
        base = dict(table.defaults())
        p1 = Preset(name="A", chip="snes", category="Pad", subcategory="Echo", params=dict(base))
        p2 = Preset(name="B", chip="snes", category="Pad", subcategory="Echo", params=dict(base, v2_echo=1 - base["v2_echo"]))
        f = {"mel": [-20.0] * 80, "env": [0.5] * 120, "pitch": [9100.0] * 10}
        result = qa.deduplicate([p1, p2], table, features={"A": f, "B": dict(f)}, jobs=1)
        self.assertEqual(len(result.kept), 2)

    def test_report_sections_round_trip(self):
        table = ParamTable.load("nes")
        result = qa.deduplicate([self.make("A"), self.make("B", p1_volume=11)], table, jobs=1, seeds=1)
        section = qa.report_section(result)
        self.assertIn("## NES", section)
        self.assertIn("| B | A |", section)
        text = qa.render_report({"nes": section})
        self.assertEqual(qa.parse_report_sections(text)["nes"].strip(), section.strip())


class LevelTests(unittest.TestCase):
    def entry(self, rms, peak, peak_short=None, peak_v127=None):
        return {"held_rms_db": rms, "peak_db": peak, "peak_short_db": peak if peak_short is None else peak_short,
                "peak_v127_db": peak if peak_v127 is None else peak_v127, "peak_short_v127_db": peak}

    def test_rms_target_when_the_peak_allows_it(self):
        # A square-like tone: -33 dBFS RMS, -31 dBFS peak -> +15 dB puts it at -18 dBFS RMS.
        decision = level.preset_gain(self.entry(-33.0, -31.0))
        self.assertEqual(decision.gain_db, 15.0)
        self.assertFalse(decision.limited_by_peak)

    def test_peak_ceiling_wins_and_rounding_never_exceeds_it(self):
        # Drum hit: RMS asks for +20 dB, the peak allows only +8.8 -> rounded down to +8.5.
        decision = level.preset_gain(self.entry(-38.0, -9.8))
        self.assertEqual(decision.gain_db, 8.5)
        self.assertTrue(decision.limited_by_peak)
        # The short pass peak counts too, and so do the velocity-127 peaks (the ceiling holds at
        # full velocity while the RMS target stays at velocity 100).
        self.assertEqual(level.preset_gain(self.entry(-38.0, -20.0, -9.8)).gain_db, 8.5)
        self.assertEqual(level.preset_gain(self.entry(-38.0, -20.0, peak_v127=-9.8)).gain_db, 8.5)
        self.assertEqual(level.preset_gain(self.entry(-33.0, -31.0, peak_v127=-14.0)).gain_db, 13.0)
        # Rounding to the nearest 0.5 dB that would cross the ceiling steps down instead.
        self.assertEqual(level.preset_gain(self.entry(-30.0, -12.8)).gain_db, 11.5)   # 11.8 -> 12.0 -> 11.5

    def test_rounding_clamping_and_silence(self):
        self.assertEqual(level.preset_gain(self.entry(-30.2, -40.0)).gain_db, 12.0)   # 12.2 -> 12.0
        self.assertEqual(level.preset_gain(self.entry(-30.3, -40.0)).gain_db, 12.5)   # 12.3 -> 12.5
        self.assertEqual(level.preset_gain(self.entry(-90.0, -88.0)).gain_db, level.GAIN_MAX_DB)
        self.assertEqual(level.preset_gain(self.entry(0.0, 0.0)).gain_db, -18.0)
        silent = level.preset_gain(self.entry(-120.0, -120.0))
        self.assertTrue(silent.silent)
        self.assertEqual(silent.gain_db, 0.0)

    def test_assign_writes_global_and_skips_presets_without_features(self):
        a = Preset(name="A", chip="nes", category="Lead", subcategory="Pulse", global_params={"poly_channels": 1})
        b = Preset(name="B", chip="nes", category="Lead", subcategory="Pulse", global_params={"preset_gain": 3.0})
        # B has features without the velocity-127 peaks (an older features file): skipped.
        old = {"held_rms_db": -33.0, "peak_db": -31.0, "peak_short_db": -31.0}
        summary = level.assign_preset_gains([a, b], {"A": self.entry(-33.0, -31.0), "B": old})
        self.assertEqual(a.global_params, {"poly_channels": 1, "preset_gain": 15.0})
        self.assertNotIn("preset_gain", b.global_params)
        self.assertEqual((summary.count, summary.missing, summary.median), (1, 1, 15.0))

    def test_qa_ignores_preset_gain_between_globals(self):
        table = ParamTable.load("nes")
        params = {"p1_duty": 2, "p1_volume": 12}
        a = Preset(name="A", chip="nes", category="Lead", subcategory="Pulse", params=dict(params),
                   global_params={"poly_channels": 1, "preset_gain": 12.0})
        b = Preset(name="B", chip="nes", category="Lead", subcategory="Pulse", params=dict(params),
                   global_params={"poly_channels": 3, "preset_gain": 3.5})
        c = Preset(name="C", chip="nes", category="Lead", subcategory="Pulse", params=dict(params),
                   global_params={"poly_channels": 1, "preset_gain": 12.0, "glide_time": 50})
        result = qa.deduplicate([a, b, c], table, jobs=1)
        self.assertEqual([p.name for p in result.kept], ["A", "C"])   # B only differs in ignored globals


class HeadroomTests(unittest.TestCase):
    def entry(self, chord_peak, rms=-30.0, peak=-20.0, saturation=None):
        return {"held_rms_db": rms, "peak_db": peak, "peak_short_db": peak, "peak_v127_db": peak,
                "peak_short_v127_db": peak, "chord_peak": chord_peak,
                "chord_saturation": saturation or {"mix": 0, "fir_wrap": 0}}

    def preset(self, name, volume, chip="snes"):
        return Preset(name=name, chip=chip, category="Pad", subcategory="Strings", params={"volume": volume})

    def test_scaled_volume_reaches_the_target(self):
        self.assertEqual(headroom.scaled_volume(88, headroom.CHORD_PEAK_TARGET), 88)   # at the target: unchanged
        self.assertEqual(headroom.scaled_volume(88, 0.5), 88)
        self.assertEqual(headroom.scaled_volume(100, 2.0 * headroom.CHORD_PEAK_TARGET), 50)
        self.assertEqual(headroom.scaled_volume(88, 1.8), 36)   # floor(88 * 0.75 / 1.8) = floor(36.7)
        self.assertEqual(headroom.scaled_volume(3, 100.0), 1)   # never silenced
        self.assertEqual(headroom.scaled_volume(0, 2.0), 0)

    def test_apply_lowers_the_volume_and_moves_the_level_by_the_same_ratio(self):
        loud, quiet, other = self.preset("Loud", 100), self.preset("Quiet", 60), self.preset("Pulse", 12, chip="nes")
        features = {"Loud": self.entry(2.0 * headroom.CHORD_PEAK_TARGET), "Quiet": self.entry(0.4),
                    "Pulse": self.entry(5.0)}
        adjusted, summary = headroom.apply([loud, quiet, other], features)
        self.assertEqual(loud.params["volume"], 50)
        self.assertEqual(quiet.params["volume"], 60)
        self.assertEqual(other.params["volume"], 12)   # only SNES presets
        self.assertAlmostEqual(adjusted["Loud"]["held_rms_db"], -30.0 - 6.0206, places=3)
        self.assertAlmostEqual(adjusted["Loud"]["peak_v127_db"], -20.0 - 6.0206, places=3)
        self.assertEqual(features["Loud"]["held_rms_db"], -30.0)   # the input is not modified
        self.assertIs(adjusted["Quiet"], features["Quiet"])
        self.assertEqual((summary.measured, summary.scaled), (2, 1))
        # preset_gain restores the level: 6 dB more than before the cut.
        before = level.preset_gain(features["Loud"]).gain_db
        after = level.preset_gain(adjusted["Loud"]).gain_db
        self.assertEqual(after - before, 6.0)

    def test_saturating_lists_the_non_zero_counts(self):
        features = {"A": self.entry(0.5), "B": self.entry(0.5, saturation={"mix": 0, "fir_wrap": 12})}
        self.assertEqual(headroom.saturating(features, ["A", "B"]), [("B", {"fir_wrap": 12.0})])


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
