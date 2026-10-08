#!/usr/bin/env python
"""Generate the preset banks (assets/presets/<chip>.json) and the QA report.

    python tools/gen_presets.py [--chip nes|snes|genesis] [--jobs N] [--features PATH ...]
                                [--out DIR] [--report PATH] [--no-target-check]
    python tools/gen_presets.py --verify-features PATH [...] [--out DIR]

Pipeline per chip: load tools/presetgen/params/<chip>.json, load the seeds from
tools/presetgen/seeds/<chip>.py (get_seeds()), expand the Cartesian product of every
seed's axes, assign unique names, de-duplicate (parameter rules, plus feature distance
when --features is given), lower the SNES voice volumes that a held chord would drive to full
scale (--features only, presetgen/headroom.py), write each preset's global.preset_gain from its
rendered level (--features only, presetgen/level.py), sort by category / subcategory / name
and write the bank.

--verify-features checks the written banks against features rendered from them (README,
"Preset regeneration"): no SNES chord render may saturate, and every preset_gain must be
the one its rendered level gives. It writes nothing and exits non-zero on any finding.

Output is deterministic: the same inputs produce byte-identical files. The exit code is
non-zero when a bank is outside its target range (docs/PRESET_SPECS.md) unless
--no-target-check is given. Standard library only.
"""

from __future__ import annotations

import argparse
import importlib
import json
import sys
from pathlib import Path
from typing import Any, Mapping, Sequence

TOOLS_DIR = Path(__file__).resolve().parent
ROOT_DIR = TOOLS_DIR.parent
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from presetgen import headroom, level, qa  # noqa: E402
from presetgen.model import CHIP_DISPLAY_NAMES, CHIPS, ParamTable, Preset, PresetError, Seed, expand_all  # noqa: E402

DEFAULT_OUT_DIR = ROOT_DIR / "assets" / "presets"
DEFAULT_REPORT = ROOT_DIR / "docs" / "PRESET_QA.md"


def load_seeds(chip: str) -> list[Seed]:
    module = importlib.import_module(f"presetgen.seeds.{chip}")
    seeds = module.get_seeds()
    if not isinstance(seeds, list) or not all(isinstance(s, Seed) for s in seeds):
        raise PresetError(f"{chip}: get_seeds() must return a list of Seed")
    return seeds


def load_features(paths: Sequence[Path]) -> dict[str, Any] | None:
    """Merge one or more `chiptool features` files ({preset name: {mel, env}})."""
    if not paths:
        return None
    merged: dict[str, Any] = {}
    for path in paths:
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
        if not isinstance(doc, Mapping):
            raise PresetError(f"{path}: features file must be a JSON object keyed by preset name")
        merged.update(doc)
    return merged


def bank_text(presets: Sequence[Preset]) -> str:
    return json.dumps([p.to_dict() for p in presets], indent=2, sort_keys=True, ensure_ascii=True) + "\n"


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def generate_bank(chip: str, seeds: Sequence[Seed], table: ParamTable, features: Mapping[str, Any] | None,
                  jobs: int | None) -> tuple[list[Preset], qa.QaResult]:
    """Expand, de-duplicate, set preset_gain (with features) and sort. Returns (sorted bank, QA result)."""
    presets = expand_all(seeds, table)
    result = qa.deduplicate(presets, table, features, jobs, seeds=len(seeds))
    if features is not None:
        level_features = features
        if chip == headroom.CHIP:
            level_features, summary = headroom.apply(result.kept, features)
            result.headroom_summary = summary.describe()
        result.gain_summary = level.assign_preset_gains(result.kept, level_features).describe()
    bank = sorted(result.kept, key=Preset.sort_key)
    return bank, result


def merge_report(report_path: Path, new_sections: Mapping[str, str]) -> str:
    """Keep the sections of chips not processed in this run."""
    sections: dict[str, str] = {}
    if report_path.exists():
        with open(report_path, "r", encoding="utf-8") as fh:
            sections = qa.parse_report_sections(fh.read())
    sections.update(new_sections)
    return qa.render_report(sections)


def verify_banks(chips: Sequence[str], out_dir: Path, features: Mapping[str, Any]) -> int:
    """--verify-features: saturating SNES chords and preset_gain values that disagree with the
    rendered level. Returns the number of findings."""
    findings = 0
    for chip in chips:
        path = out_dir / f"{chip}.json"
        with open(path, "r", encoding="utf-8") as fh:
            bank = json.load(fh)
        names = [p["name"] for p in bank]
        missing = [n for n in names if n not in features]
        if missing:
            continue   # features of another chip only
        bad = headroom.saturating(features, names) if chip == headroom.CHIP else []
        for name, counts in bad:
            print(f"{CHIP_DISPLAY_NAMES[chip]}: '{name}' saturates on the held chord: {counts}")
        off = []
        for preset in bank:
            entry = features[preset["name"]]
            if any(key not in entry for key in level.REQUIRED_KEYS):
                continue
            stored = float(preset.get("global", {}).get(level.PRESET_GAIN_KEY, 0.0))
            expected = level.preset_gain(entry).gain_db
            if abs(stored - expected) > level.GAIN_STEP_DB + 1e-9:
                off.append((preset["name"], stored, expected))
        for name, stored, expected in off:
            print(f"{CHIP_DISPLAY_NAMES[chip]}: '{name}' preset_gain {stored:+.1f} dB, rendered level gives {expected:+.1f} dB")
        print(f"{CHIP_DISPLAY_NAMES[chip]}: {len(bank)} presets verified, {len(bad)} saturating chords, "
              f"{len(off)} preset_gain values more than {level.GAIN_STEP_DB} dB off")
        findings += len(bad) + len(off)
    return findings


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate the chip preset banks and the QA report.")
    parser.add_argument("--chip", choices=CHIPS, action="append",
                        help="chip to generate (default: all three; may be repeated)")
    parser.add_argument("--jobs", type=int, default=None,
                        help="worker processes for QA (default: os.cpu_count() - 1)")
    parser.add_argument("--features", type=Path, action="append", default=[],
                        help="features JSON from `chiptool features` (may be repeated)")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT_DIR, help="bank output directory")
    parser.add_argument("--report", type=Path, default=DEFAULT_REPORT, help="QA report path")
    parser.add_argument("--no-target-check", action="store_true",
                        help="do not fail when a bank is outside its target count")
    parser.add_argument("--qa", action="store_true", help="accepted for compatibility; QA always runs")
    parser.add_argument("--verify-features", type=Path, action="append", default=[],
                        help="features JSON rendered from the written banks: verify them, write nothing")
    args = parser.parse_args(argv)

    chips = args.chip or list(CHIPS)
    if args.verify_features:
        try:
            findings = verify_banks(chips, args.out, load_features(args.verify_features) or {})
        except (OSError, ValueError, KeyError) as error:
            print(f"error: {error}", file=sys.stderr)
            return 2
        return 1 if findings else 0
    try:
        features = load_features(args.features)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    sections: dict[str, str] = {}
    out_of_target: list[str] = []
    for chip in chips:
        try:
            table = ParamTable.load(chip)
            seeds = load_seeds(chip)
            bank, result = generate_bank(chip, seeds, table, features, args.jobs)
        except PresetError as error:
            print(f"error: {error}", file=sys.stderr)
            return 2
        out_path = args.out / f"{chip}.json"
        write_text(out_path, bank_text(bank))
        sections[chip] = qa.report_section(result)

        low, high = qa.TARGETS[chip]
        ok = low <= len(bank) <= high
        status = "ok" if ok else "OUT OF TARGET"
        print(f"{CHIP_DISPLAY_NAMES[chip]}: {len(seeds)} seeds -> {result.before} expanded -> "
              f"{len(bank)} presets (target {low}-{high}, {status}); "
              f"removed {result.removed_by_parameters} by parameters, {result.removed_by_features} by features; "
              f"wrote {out_path}")
        if result.headroom_summary:
            print(f"  polyphony headroom: {result.headroom_summary}")
        if result.gain_summary:
            print(f"  preset_gain: {result.gain_summary}")
        if not ok:
            out_of_target.append(chip)

    write_text(args.report, merge_report(args.report, sections))
    print(f"QA report: {args.report}")

    if out_of_target and not args.no_target_check:
        print(f"error: banks outside target range: {', '.join(out_of_target)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
