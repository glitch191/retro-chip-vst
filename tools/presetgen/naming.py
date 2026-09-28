"""Mechanical preset naming (docs/PRESET_SPECS.md, "Naming").

    <Chip> <Category> [<Base>] <Descriptor...>

* Chip: NES / SNES / Genesis.
* Base: the seed name with the words already present in the category removed
  ("Square Lead" in category "Lead" -> "Square"; "Lead" in "Lead" -> nothing).
* Descriptors: one per axis, in axis order.
  - scalar axis: a short word for the parameter plus the value, e.g. p1_duty=1 -> "Duty25",
    algorithm=4 -> "Alg4", echo_delay=3 -> "Echo3", transpose=12 -> "OctUp", transpose=0 -> "".
  - variant axis: "<Axis> <Label>" ("vibrato", "slow" -> "Vibrato Slow"); a label that
    already contains the axis word is used as is ("release", "LongRelease" -> "LongRelease");
    the labels "off", "none" and "" contribute nothing.
* Names are unique within a bank: a collision gets " 2", " 3", ... appended.

This module has no dependency on the rest of the package so that model.py can import it.
"""

from __future__ import annotations

import re
from typing import Iterable, Sequence

# Channel / operator prefixes that are dropped from a key before choosing its word:
# NES per-channel prefixes and the Genesis PSG prefix. Operator, FM-channel, voice and
# PSG-tone prefixes are kept (CamelCased) because they matter for reading the name.
_DROPPED_PREFIX = re.compile(r"^(p1|p2|tri|nz|dmc|psg)_")
_KEPT_PREFIX = re.compile(r"^(op[1-4]|fm[1-6]|v[1-8]|psg[123n])_")

# Words for well-known parameter suffixes; anything else is CamelCased from the key.
_WORDS = {
    "duty": "Duty",
    "algorithm": "Alg",
    "feedback": "FB",
    "echo_delay": "Echo",
    "echo_feedback": "EchoFB",
    "echo_volume": "EchoVol",
    "volume": "Vol",
    "tl": "TL",
    "att": "Att",
    "ams": "AMS",
    "fms": "FMS",
    "lfo_freq": "LFO",
    "mul": "Mul",
    "dt": "DT",
    "rs": "RS",
    "ar": "AR",
    "dr": "DR",
    "sr": "SR",
    "rr": "RR",
    "sl": "SL",
    "ssg": "SSG",
    "period": "Period",
    "mode": "Mode",
    "sample": "Sample",
    "dac_sample": "Sample",
    "fir_preset": "FIR",
    "unison_detune": "Unison",
    "attack": "Attack",
    "decay": "Decay",
    "sustain_level": "Sustain",
    "sustain_rate": "SustainRate",
    "release_rate": "Release",
    "sw_attack": "Attack",
    "sw_decay": "Decay",
    "sw_sustain": "Sustain",
    "sw_release": "Release",
    "vibrato_rate": "VibRate",
    "vibrato_depth": "VibDepth",
    "vibrato_delay": "VibDelay",
    "pitch_env_depth": "PitchEnv",
    "pitch_env_speed": "PitchSpeed",
    "noise_clock": "NoiseClock",
    "noise_mode": "Noise",
    "noise_rate": "NoiseRate",
    "sweep_period": "SweepPeriod",
    "sweep_shift": "SweepShift",
    "gate_frames": "Gate",
    "linear_length": "Linear",
    "rate": "Rate",
}

_DUTY_LABELS = {0: "Duty12", 1: "Duty25", 2: "Duty50", 3: "Duty75"}
_OMITTED_VARIANT_LABELS = ("", "off", "none")

_NON_ALNUM = re.compile(r"[^A-Za-z0-9]+")


def camel(text: str) -> str:
    """'sw_attack' -> 'SwAttack', 'op1_tl' -> 'Op1Tl'."""
    return "".join(part[:1].upper() + part[1:] for part in text.split("_") if part)


def _split_key(key: str) -> tuple[str, str]:
    """Return (kept prefix as CamelCase or '', suffix used for the word lookup)."""
    match = _KEPT_PREFIX.match(key)
    if match:
        return camel(match.group(1)), key[match.end():]
    match = _DROPPED_PREFIX.match(key)
    if match:
        return "", key[match.end():]
    return "", key


def _format_number(value: float | int) -> str:
    number = float(value)
    if number.is_integer():
        text = str(int(number))
    else:
        text = repr(round(number, 3))
    return text.replace("-", "Neg")


def _transpose_descriptor(value: int) -> str:
    if value == 0:
        return ""
    octaves, semis = divmod(abs(value), 12)
    direction = "Up" if value > 0 else "Down"
    if semis == 0:
        return f"Oct{direction}" + (str(octaves) if octaves > 1 else "")
    return f"{direction}{abs(value)}"


def value_descriptor(key: str, value: float | int) -> str:
    """Descriptor for a scalar axis value ('' when the value adds nothing to the name)."""
    prefix, suffix = _split_key(key)
    if suffix == "duty":
        return prefix + _DUTY_LABELS.get(int(value), f"Duty{_format_number(value)}")
    if suffix == "transpose":
        return prefix + _transpose_descriptor(int(round(float(value))))
    word = _WORDS.get(suffix, camel(suffix))
    return f"{prefix}{word}{_format_number(value)}"


def variant_descriptor(axis: str, label: str) -> str:
    """Descriptor for a named variant of a variant axis."""
    clean = label.strip()
    if clean.lower() in _OMITTED_VARIANT_LABELS:
        return ""
    axis_word = camel(axis)
    words = clean.split()
    titled = " ".join(word[:1].upper() + word[1:] for word in words)
    if axis_word.lower() in _NON_ALNUM.sub("", clean).lower():
        return titled
    return f"{axis_word} {titled}"


def base_name(category: str, seed_name: str) -> str:
    """Seed name without the words that the category already carries."""
    category_words = {word.lower() for word in category.split()}
    kept = [word for word in seed_name.split() if word.lower() not in category_words]
    return " ".join(kept)


def build_name(chip_name: str, category: str, seed_name: str, descriptors: Sequence[str]) -> str:
    parts = [chip_name, category]
    base = base_name(category, seed_name)
    if base:
        parts.append(base)
    parts.extend(d for d in descriptors if d)
    return " ".join(parts)


def descriptor_tags(descriptors: Iterable[str]) -> list[str]:
    """Lower-case, hyphenated tags from the descriptors ('Vibrato Slow' -> 'vibrato-slow')."""
    tags = []
    for descriptor in descriptors:
        if descriptor:
            tags.append(_NON_ALNUM.sub("-", descriptor.strip()).strip("-").lower())
    return tags


def make_unique(names: Sequence[str]) -> list[str]:
    """Append ' 2', ' 3', ... to repeated names, in order of appearance.

    The suffixed name is itself checked against every name so that "X 2" never collides
    with an existing "X 2".
    """
    taken: set[str] = set(names)
    seen: set[str] = set()
    result: list[str] = []
    for name in names:
        candidate = name
        if name in seen:
            index = 2
            candidate = f"{name} {index}"
            while candidate in taken or candidate in seen:
                index += 1
                candidate = f"{name} {index}"
        seen.add(candidate)
        taken.add(candidate)
        result.append(candidate)
    return result


def assign_unique_names(presets) -> None:
    """Rewrite preset.name in place so that names are unique within the list."""
    unique = make_unique([preset.name for preset in presets])
    for preset, name in zip(presets, unique):
        preset.name = name
