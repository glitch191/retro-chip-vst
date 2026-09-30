#!/usr/bin/env python
"""Render the SNES / Genesis A/B listening set into build-reports/listening/.

    python tools/render_listening.py [--before-ref HEAD] [--chiptool PATH] [--out DIR]

"before" = the preset banks and samples of a git revision (read with `git show`, written
to a temporary folder); "after" = the working tree (assets/presets, assets/samples).
Both are rendered through the same, current chiptool build, so the pairs compare preset
and sample changes only, not engine changes. Each file is a short phrase played with
`chiptool render --notes` at 44.1 kHz, 16-bit stereo, named
<chip>_<category>_<preset>_{before,after}.wav, and INDEX.md lists them.

The selection below is hand-picked: 2-3 presets per category. When a preset was renamed
or replaced by the seed redesign, "before" is the closest HEAD preset of the same family.
Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
DEFAULT_CHIPTOOL = ROOT_DIR / "build" / "windows-x64-release" / "dsp" / "tools" / "chiptool.exe"
DEFAULT_OUT = ROOT_DIR / "build-reports" / "listening"
RATE = 44100

# Phrases: (notes, step seconds, gate, total seconds).
PHRASES = {
    "melody": ([60, 62, 64, 65, 67, 64, 62, 60], 0.4, 0.8, 3.8),
    "bass": ([36, 43, 40, 43, 36, 38, 43, 36], 0.4, 0.8, 3.8),
    "pad": ([60, 64, 67], 1.1, 0.9, 4.0),
    "drum": ([60] * 8, 0.4, 0.5, 3.6),
    "sfx": ([48, 60, 72], 1.2, 0.6, 4.0),
}

# (chip, category, before preset name, after preset name, phrase)
SELECTION = [
    ("snes", "Bass", "SNES Bass Upright", "SNES Bass Upright", "bass"),
    ("snes", "Bass", "SNES Bass Upright OctDown", "SNES Bass Upright OctDown", "melody"),
    ("snes", "Drums", "SNES Drums Shaker", "SNES Drums Shaker", "drum"),
    ("snes", "Drums", "SNES Drums Dynamic Snare Ghost Note", "SNES Drums Dynamic Snare Ghost Note", "drum"),
    ("snes", "Drums", "SNES Drums Dynamic Kick Ghost Note", "SNES Drums Dynamic Kick Ghost Note", "drum"),
    ("snes", "Instrument", "SNES Instrument Grand Piano", "SNES Instrument Grand Piano", "melody"),
    ("snes", "Instrument", "SNES Instrument Ensemble Strings", "SNES Instrument Ensemble Strings", "melody"),
    ("snes", "Instrument", "SNES Instrument Solo Trumpet", "SNES Instrument Solo Trumpet", "melody"),
    ("snes", "Pad", "SNES Pad Choir Ah Echo Long", "SNES Pad Choir Ah Echo Deep", "pad"),
    ("snes", "Pad", "SNES Pad Low Strings Echo Long", "SNES Pad Low Strings Echo Deep", "pad"),
    ("snes", "Pad", "SNES Pad Glass Echo Long", "SNES Pad Glass Echo Band", "pad"),
    ("snes", "SFX", "SNES SFX Explosion NoiseClock12", "SNES SFX Explosion NoiseClock12", "sfx"),
    ("snes", "SFX", "SNES SFX Wind NoiseClock18", "SNES SFX Wind NoiseClock18", "sfx"),
    ("snes", "SFX", "SNES SFX PMON Glass Echo Long", "SNES SFX PMON Glass Echo Long", "sfx"),
    ("genesis", "FM Bass", "Genesis FM Bass Growl FB4", "Genesis FM Bass Growl FB4", "bass"),
    ("genesis", "FM Bass", "Genesis FM Bass Chorus Detune Wide", "Genesis FM Bass Chorus Detune Wide", "bass"),
    ("genesis", "FM Bass", "Genesis FM Bass Pick FB3 Velocity Heavy", "Genesis FM Bass Pick FB3 Decay Ringing", "bass"),
    ("genesis", "FM Keys", "Genesis FM Keys Tine Piano Alg5 Tine Medium",
     "Genesis FM Keys Tine EPiano Tine Medium Chip YM2612", "melody"),
    ("genesis", "FM Keys", "Genesis FM Keys Clav FB3 Velocity Heavy",
     "Genesis FM Keys Clav Bite Medium Damping Tight", "melody"),
    ("genesis", "FM Keys", "Genesis FM Keys Glock Decay Long", "Genesis FM Keys Glock Decay Long", "melody"),
    ("genesis", "FM Brass", "Genesis FM Brass Trumpet FB3", "Genesis FM Brass Trumpet FB3", "melody"),
    ("genesis", "FM Brass", "Genesis FM Brass Synth Detune Light Attack Fast",
     "Genesis FM Brass Synth Detune Light Attack Fast", "melody"),
    ("genesis", "FM Brass", "Genesis FM Brass Stab FB5", "Genesis FM Brass Stab FB5", "melody"),
    ("genesis", "FM Pad", "Genesis FM Pad String Unison4 Release Long",
     "Genesis FM Pad String Ensemble Detune Narrow Release Long", "pad"),
    ("genesis", "FM Pad", "Genesis FM Pad Choir Alg6 Attack Slow", "Genesis FM Pad Choir Alg6 Attack Slow", "pad"),
    ("genesis", "FM Pad", "Genesis FM Pad Organ Unison2", "Genesis FM Pad Drawbar Organ Drawbars Bright", "pad"),
    ("genesis", "DAC", "Genesis DAC Kick Rate 11k", "Genesis DAC Kick Rate 11k", "drum"),
    ("genesis", "DAC", "Genesis DAC Snare Rate 16k", "Genesis DAC Snare Rate 16k", "drum"),
    ("genesis", "PSG Lead", "Genesis PSG Lead Chorus Unison2", "Genesis PSG Lead Chorus Unison1", "melody"),
    ("genesis", "PSG Lead", "Genesis PSG Lead Bend", "Genesis PSG Lead Bend", "melody"),
    ("genesis", "PSG Bass", "Genesis PSG Bass Detuned Unison7 Decay Pluck",
     "Genesis PSG Bass Detuned Unison5 Decay Pluck", "bass"),
    ("genesis", "PSG Bass", "Genesis PSG Bass Buzz Level Loud Release Short",
     "Genesis PSG Bass Buzz Level Loud Release Short", "bass"),
    ("genesis", "PSG Drums", "Genesis PSG Drums Pitched Snare Decay8", "Genesis PSG Drums Noise Snare Decay6 NoiseRate1", "drum"),
    ("genesis", "PSG Drums", "Genesis PSG Drums Kick Decay12 NoiseRate1", "Genesis PSG Drums Kick Decay12 NoiseRate1", "drum"),
    ("genesis", "PSG Drums", "Genesis PSG Drums Closed Hat Decay2 Level Loud",
     "Genesis PSG Drums Closed Hat Decay2 Level Loud", "drum"),
]


def slug(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_").lower()


def git_show(ref: str, path: str) -> bytes:
    return subprocess.run(["git", "show", f"{ref}:{path}"], cwd=ROOT_DIR, check=True, capture_output=True).stdout


def extract_before(ref: str, dest: Path) -> None:
    """Writes <dest>/presets/<chip>.json and <dest>/samples/ as they are at `ref`."""
    (dest / "presets").mkdir(parents=True)
    for chip in ("snes", "genesis"):
        (dest / "presets" / f"{chip}.json").write_bytes(git_show(ref, f"assets/presets/{chip}.json"))
    index_bytes = git_show(ref, "assets/samples/index.json")
    (dest / "samples").mkdir()
    (dest / "samples" / "index.json").write_bytes(index_bytes)
    for entry in json.loads(index_bytes.decode("utf-8-sig")):
        target = dest / "samples" / entry["file"]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(git_show(ref, f"assets/samples/{entry['file']}"))


def load_bank(path: Path) -> dict[str, dict]:
    return {p["name"]: p for p in json.loads(path.read_text(encoding="utf-8-sig"))}


def render(chiptool: Path, preset: dict, samples: Path, wav: Path, phrase: str, work: Path) -> None:
    notes, step, gate, seconds = PHRASES[phrase]
    preset_file = work / (wav.stem + ".json")
    preset_file.write_text(json.dumps(preset), encoding="utf-8")
    subprocess.run([str(chiptool), "render", str(preset_file), str(wav), "--rate", str(RATE),
                    "--seconds", str(seconds), "--notes", ",".join(map(str, notes)),
                    "--step", str(step), "--gate", str(gate), "--samples", str(samples)], check=True)


def levels(wav: Path) -> tuple[float, float]:
    """Peak and RMS of a 16-bit stereo WAV in dBFS (the chiptool header is 44 bytes)."""
    data = wav.read_bytes()[44:]
    values = struct.unpack(f"<{len(data) // 2}h", data)
    peak = max(abs(v) for v in values) / 32768.0
    rms = math.sqrt(sum(v * v for v in values) / len(values)) / 32768.0
    to_db = lambda x: 20.0 * math.log10(x) if x > 0 else -math.inf  # noqa: E731
    return to_db(peak), to_db(rms)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--before-ref", default="HEAD")
    parser.add_argument("--chiptool", type=Path, default=DEFAULT_CHIPTOOL)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()

    commit = subprocess.run(["git", "rev-parse", "--short", args.before_ref], cwd=ROOT_DIR, check=True,
                            capture_output=True, text=True).stdout.strip()
    args.out.mkdir(parents=True, exist_ok=True)
    for old in args.out.glob("*.wav"):
        old.unlink()

    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
        extract_before(args.before_ref, tmp_dir / "before")
        work = tmp_dir / "work"
        work.mkdir()
        sides = {
            "before": (tmp_dir / "before" / "presets", tmp_dir / "before" / "samples"),
            "after": (ROOT_DIR / "assets" / "presets", ROOT_DIR / "assets" / "samples"),
        }
        banks = {(side, chip): load_bank(presets / f"{chip}.json")
                 for side, (presets, _) in sides.items() for chip in ("snes", "genesis")}
        for chip, category, before_name, after_name, phrase in SELECTION:
            short = after_name.split(" ", 1)[1]  # without the chip prefix
            if short.startswith(category + " "):
                short = short[len(category) + 1:]
            base = f"{chip}_{slug(category)}_{slug(short)}"
            result = {"chip": chip, "category": category, "phrase": phrase}
            for side, name in (("before", before_name), ("after", after_name)):
                bank = banks[(side, chip)]
                if name not in bank and side == "before":
                    # Older revisions may predate the preset: fall back to the first preset of the
                    # same category and subcategory (else category) in that bank.
                    target = banks[("after", chip)].get(after_name, {})
                    same = [n for n, p in bank.items() if p.get("category") == target.get("category")
                            and p.get("subcategory") == target.get("subcategory")]
                    same = same or [n for n, p in bank.items() if p.get("category") == target.get("category")]
                    if same:
                        print(f"before: {name} not in {commit}, using {sorted(same)[0]}")
                        name = sorted(same)[0]
                if name not in bank:
                    raise SystemExit(f"{side}: preset not found: {name}")
                wav = args.out / f"{base}_{side}.wav"
                render(args.chiptool, bank[name], sides[side][1], wav, phrase, work)
                result[side] = (name, wav.name, hashlib.sha256(wav.read_bytes()).hexdigest(), *levels(wav))
            rows.append(result)
            print(f"{base}: {'identical' if result['before'][2] == result['after'][2] else 'differs'}")

    write_index(args.out / "INDEX.md", rows, commit)
    print(f"{2 * len(rows)} files -> {args.out}")
    return 0


def write_index(path: Path, rows: list[dict], commit: str) -> None:
    lines = [
        "# A/B listening set: SNES and Genesis",
        "",
        "Generated by `python tools/render_listening.py`. Every file is 44.1 kHz, 16-bit stereo,",
        "rendered by `chiptool render --notes` (monophonic phrase on the preset's hardware",
        "channel, velocity 100, after the 0.4 s silent pre-roll).",
        "",
        f"* **before**: preset banks and samples of git revision `{commit}` (read with `git show`).",
        "* **after**: the working tree (`assets/presets`, `assets/samples`).",
        "* Both sides use the same, current engine build: the pairs isolate the preset and sample",
        "  changes (CC0 samples, SNES and Genesis seed redesign), not the engine fixes.",
        "* When the redesign renamed or replaced a preset, **before** is the closest preset of the",
        "  same family in the old bank (both names are listed).",
        "* \"identical\" means the two WAV files are byte-identical (unchanged preset, kept as a",
        "  control).",
        "",
        "Phrases:",
        "",
    ]
    for name, (notes, step, gate, seconds) in PHRASES.items():
        lines.append(f"* `{name}`: MIDI notes {', '.join(map(str, notes))}; one note every {step} s, "
                     f"held {gate:.0%} of the step; {seconds} s in total.")
    lines += ["", "| Chip | Category | Phrase | Before preset | After preset | Files | Peak / RMS dBFS (before, after) | Result |",
              "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        bn, bf, bh, bp, br = r["before"]
        an, af, ah, ap, ar = r["after"]
        same = "identical" if bh == ah else "differs"
        lines.append(f"| {r['chip']} | {r['category']} | {r['phrase']} | {bn} | {an} | `{bf}`<br>`{af}` | "
                     f"{bp:.1f} / {br:.1f}, {ap:.1f} / {ar:.1f} | {same} |")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


if __name__ == "__main__":
    sys.exit(main())
