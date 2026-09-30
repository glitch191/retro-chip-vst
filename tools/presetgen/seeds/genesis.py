"""Genesis (YM2612 + SN76489) seed patches.

The references, design principles and per-category target sounds behind these patches, and
the render-based verification of the bank, are in docs/research/genesis-sound-design.md.

Every seed sets every engine parameter of tools/presetgen/params/genesis.json explicitly:
the blocks below (system, FM voice, four operators, DAC, PSG) each spell out all of their
keys, and get_seeds() fails if a seed misses or duplicates one. Register semantics used in
the comments (docs/ENGINE_SPECS.md, docs/research/genesis.md):

* Operators op1..op4 are S1..S4 in the OPNA algorithm diagrams; S1 has the feedback.
  Carriers: alg 0-3 S4; alg 4 S2 + S4; alg 5-6 S2 + S3 + S4; alg 7 all four.
* TL 0..127 is attenuation in 0.75 dB steps (0 = loudest); a modulator's TL sets the
  brightness, a carrier's TL the level. velocity_depth is the TL added to the carriers at
  velocity 0, so velocity only ever changes the carriers' level.
* AR/DR/SR 0..31 and RR 0..15 are rate indexes (higher = faster, AR 31 = instant);
  SL 0..15 is the decay target in 3 dB steps (15 = -93 dB, i.e. decay to silence).
* MUL 0 = x0.5, 1..15 = x1..x15; DT 1-3 detune up, 5-7 down, 0/4 none.
* SSG-EG value 1..8 = OPNA shape 0..7 with the enable bit (1 saw, 2 decay-hold,
  3 triangle, 5 inverted saw, 6 rise-hold ...); SSG operators use AR 31 as the OPNA manual
  requires. The SSG state logic only acts once the attenuation reaches 0x200 (48 dB), and
  below that the decay runs 4x faster (docs/research/genesis.md, SSG-EG). Every SSG operator
  therefore has SL 15 (SL10 = 0x3E0 > 0x200), so its decay always crosses 0x200 and the
  shape repeats or holds; one ramp lasts (0x200 / 0x3FF) / 4 of the rate-table decay time,
  e.g. DR 16 with RS 1 at C4 (rate 36): 460.6 ms / 8 = 58 ms.
* PSG attenuation 0..15 in 2 dB steps (15 = off). Noise rate 3 clocks the noise from tone
  channel 3, which the presets keep at attenuation 15 so that only its period is used.
  The lowest PSG tone is period 0x3FF = A2 (109 Hz): tone-channel basses use psg_transpose
  +12 so that keys from A1 up land inside the chip's range.
* Noise rate 3 note mapping (driver behaviour, docs/research/genesis.md "Driver tick model"):
  a note on the noise channel writes tone 3's period. In periodic mode the period is computed
  for note + 48 semitones (16x the key frequency), so the periodic pulse (1 in 16 shifts)
  sounds at the played key, from 6.8 Hz (period 0x3FF) to 6991 Hz (period 1). White noise
  uses the note directly: the LFSR shifts at the key frequency (C4 -> 262 Hz, a rumble), so
  white-noise presets on this rate need a large psg_transpose to reach snare colours. The
  LFSR restarts from 0x8000 on every note and outputs 0 for its first 15 shifts.
* DAC (assumed driver behaviour, also not yet in docs/ENGINE_SPECS.md): the WAV is
  resampled to `dac_rate` when it is loaded into the slot, as the NES DMC encoder does at
  `dmc_rate`, so a lower rate keeps pitch and length and only coarsens the zero-order-hold
  sound. With `dac_keyed` the playback rate follows the key (note 60 = `dac_rate`).

Channel routing is part of each preset through the preset-managed global `poly_channels`
(bit n = hardware channel n): FM 1-6 for FM patches, FM6 alone for DAC drums (the DAC
replaces FM6's output), PSG tone 1-3 for tonal PSG patches and the noise channel for PSG
drums and noise basses. `perf()` writes every preset-managed global (poly_channels, arp_*,
glide_*) so that loading a Genesis preset always resets the arpeggiator and glide.

DAC slot indexes follow the order of the genesis entries in assets/samples/index.json.
"""

from __future__ import annotations

from typing import Any, Mapping, Sequence

from presetgen.model import ParamTable, PresetError, Seed

# ----- channel masks (global poly_channels, bit n = hardware channel n) ----------------------

FM_CHANNELS = 0b0000111111      # FM1..FM6
DAC_CHANNEL = 0b0000100000      # FM6 carries the DAC when dac_enable = 1
PSG_TONE_CHANNELS = 0b0111000000  # PSG tone 1..3
PSG_NOISE_CHANNEL = 0b1000000000  # PSG noise

# Arpeggiator choices (plugin/src/Parameters.cpp label order, same as tools/presetgen/seeds/nes.py).
ARP_UP, ARP_SYNC, ARP_DIV_16 = 0, 0, 3
GLIDE_ALWAYS, GLIDE_LEGATO = 0, 1


def perf(poly: int, *, glide_ms: float = 0.0, legato: bool = False) -> dict[str, Any]:
    """Every preset-managed global: channel mask, arpeggiator off, glide (off unless given)."""
    return {
        "poly_channels": poly,
        "arp_enabled": 0, "arp_pattern": ARP_UP, "arp_octaves": 1, "arp_rate_mode": ARP_SYNC,
        "arp_sync_division": ARP_DIV_16, "arp_free_rate": 8.0, "arp_gate": 50.0, "arp_hold": 0,
        "glide_time": float(glide_ms), "glide_mode": GLIDE_LEGATO if legato else GLIDE_ALWAYS,
    }


FM_GLOBALS = perf(FM_CHANNELS)
DAC_GLOBALS = perf(DAC_CHANNEL)
PSG_TONE_GLOBALS = perf(PSG_TONE_CHANNELS)
PSG_NOISE_GLOBALS = perf(PSG_NOISE_CHANNEL)

# DAC slots: genesis samples in assets/samples/index.json order.
DAC_SLOTS = ("clap", "cowbell", "crash_short", "hat_closed", "hat_open", "kick", "kick_deep",
             "noise_burst", "rim", "sega_hit", "snare", "snare_short", "tom", "voice_uh")


# ----- parameter blocks (every argument required: nothing falls back to an engine default) ---

def system(clock: int, chip_revision: int, model1_lowpass: int, lfo_enable: int, lfo_freq: int) -> dict[str, int]:
    return {"clock": clock, "chip_revision": chip_revision, "model1_lowpass": model1_lowpass,
            "lfo_enable": lfo_enable, "lfo_freq": lfo_freq}


# NTSC Model 1 (discrete YM2612 with the ladder effect, 3.39 kHz output low-pass), LFO off.
MD1 = system(0, 0, 1, 0, 0)


def md1_lfo(freq: int) -> dict[str, int]:
    """NTSC Model 1 with the global LFO running at setting `freq` (0..7)."""
    return system(0, 0, 1, 1, freq)


def voice(*, algorithm: int, feedback: int, ams: int, fms: int, transpose: int, fine_tune: int,
          vibrato: tuple[int, int, int], unison_detune: int, velocity_depth: int, pan: int) -> dict[str, int]:
    """FM channel patch; `vibrato` = (rate, depth, delay) of the driver's software vibrato."""
    params = {"algorithm": algorithm, "feedback": feedback, "ams": ams, "fms": fms,
              "transpose": transpose, "fine_tune": fine_tune,
              "vibrato_rate": vibrato[0], "vibrato_depth": vibrato[1], "vibrato_delay": vibrato[2],
              "unison_detune": unison_detune, "velocity_depth": velocity_depth}
    for channel in range(1, 7):
        params[f"fm{channel}_pan"] = pan
    return params


_OP_FIELDS = ("tl", "ar", "dr", "sr", "rr", "sl", "mul", "dt", "rs", "am", "ssg")


def operators(rows: Sequence[Sequence[int]]) -> dict[str, int]:
    """Four rows (S1..S4) of (tl, ar, dr, sr, rr, sl, mul, dt, rs, am, ssg)."""
    if len(rows) != 4:
        raise PresetError("genesis seeds: operators() needs exactly four rows")
    params: dict[str, int] = {}
    for n, row in enumerate(rows, start=1):
        if len(row) != len(_OP_FIELDS):
            raise PresetError(f"genesis seeds: operator row {n} needs {len(_OP_FIELDS)} values")
        for field_name, value in zip(_OP_FIELDS, row):
            params[f"op{n}_{field_name}"] = value
    return params


def dac(*, enable: int, sample: int, rate: int, keyed: int, loop: int, volume: int) -> dict[str, int]:
    return {"dac_enable": enable, "dac_sample": sample, "dac_rate": rate, "dac_keyed": keyed,
            "dac_loop": loop, "dac_volume": volume}


def psg(*, tone_att: int, tone3_att: int, noise_att: int, noise_mode: int, noise_rate: int,
        attack: int, decay: int, sustain: int, release: int, vibrato_rate: int, vibrato_depth: int,
        unison_detune: int, transpose: int) -> dict[str, int]:
    """PSG block; `tone_att` goes to tone 1 and 2, `tone3_att` to tone 3 (15 when it clocks the noise)."""
    return {"psg1_att": tone_att, "psg2_att": tone_att, "psg3_att": tone3_att, "psgn_att": noise_att,
            "psg_noise_mode": noise_mode, "psg_noise_rate": noise_rate,
            "psg_sw_attack": attack, "psg_sw_decay": decay, "psg_sw_sustain": sustain,
            "psg_sw_release": release, "psg_vibrato_rate": vibrato_rate,
            "psg_vibrato_depth": vibrato_depth, "psg_unison_detune": unison_detune,
            "psg_transpose": transpose}


# DAC off (FM6 stays an FM channel); slot 0, 8 kHz, full volume so re-enabling it is sane.
DAC_OFF = dac(enable=0, sample=0, rate=8000, keyed=0, loop=0, volume=127)

# PSG silenced (attenuation 15 everywhere) for FM and DAC presets.
PSG_OFF = psg(tone_att=15, tone3_att=15, noise_att=15, noise_mode=1, noise_rate=0, attack=0, decay=0,
              sustain=15, release=0, vibrato_rate=0, vibrato_depth=0, unison_detune=0, transpose=0)

# FM patch carried by PSG and DAC presets, whose poly_channels exclude FM1-FM5: a plain sine
# on S4 (alg 7, S1-S3 at TL 127) so that FM channels re-enabled by the user stay predictable.
FM_REST = {
    **voice(algorithm=7, feedback=0, ams=0, fms=0, transpose=0, fine_tune=0, vibrato=(0, 0, 0),
            unison_detune=0, velocity_depth=24, pan=1),
    **operators([
        (127, 31, 0, 0, 8, 0, 1, 0, 0, 0, 0),
        (127, 31, 0, 0, 8, 0, 1, 0, 0, 0, 0),
        (127, 31, 0, 0, 8, 0, 1, 0, 0, 0, 0),
        (0, 31, 0, 0, 8, 0, 1, 0, 0, 0, 0),
    ]),
}


# ----- override helpers for variant axes ------------------------------------------------------

def ops(field_name: str, values: Mapping[int, int]) -> dict[str, int]:
    """{operator number: value} -> {"op<n>_<field>": value}."""
    return {f"op{n}_{field_name}": value for n, value in values.items()}


def all_ops(field_name: str, value: int) -> dict[str, int]:
    return ops(field_name, {1: value, 2: value, 3: value, 4: value})


def tone_level(att: int) -> dict[str, int]:
    """Attenuation of the three PSG tone channels (the allocator may pick any of them)."""
    return {"psg1_att": att, "psg2_att": att, "psg3_att": att}


def merge(*blocks: Mapping[str, Any]) -> dict[str, Any]:
    """Union of parameter blocks; a key set twice is a seed-authoring error."""
    params: dict[str, Any] = {}
    for block in blocks:
        for key, value in block.items():
            if key in params:
                raise PresetError(f"genesis seeds: parameter {key!r} set twice")
            params[key] = value
    return params


def fm_seed(name: str, category: str, subcategory: str, comment: str, *, sys: Mapping[str, int],
            patch: Mapping[str, int], rows: Sequence[Sequence[int]], axes: dict[str, Sequence[Any]],
            tags: list[str], global_params: Mapping[str, Any] = FM_GLOBALS) -> Seed:
    return Seed(name=name, category=category, subcategory=subcategory, comment=comment,
                params=merge(sys, patch, operators(rows), DAC_OFF, PSG_OFF),
                axes=axes, tags=["fm"] + tags, global_params=dict(global_params))


def psg_seed(name: str, category: str, subcategory: str, comment: str, *, block: Mapping[str, int],
             axes: dict[str, Sequence[Any]], tags: list[str], global_params: Mapping[str, Any]) -> Seed:
    return Seed(name=name, category=category, subcategory=subcategory, comment=comment,
                params=merge(MD1, FM_REST, DAC_OFF, block),
                axes=axes, tags=["psg"] + tags, global_params=dict(global_params))


# Console output stage for bright DAC samples: Model 1 (discrete YM2612, 3.39 kHz low-pass) or
# Model 2 (YM3438 ASIC; its own filter is not modelled, docs/research/genesis.md Ambiguity 16).
CONSOLE = [("Model1", {"chip_revision": 0, "model1_lowpass": 1}),
           ("Model2", {"chip_revision": 1, "model1_lowpass": 0})]


def dac_seed(name: str, sample: str, comment: str, *, keyed: int, rates: Sequence[int],
             tags: list[str], console_axis: bool = False) -> Seed:
    slot = DAC_SLOTS.index(sample)
    labels = {8000: "8k", 11025: "11k", 16000: "16k", 22050: "22k"}
    axes: dict[str, Sequence[Any]] = {"rate": [(labels[r], {"dac_rate": r}) for r in rates]}
    if console_axis:
        axes["console"] = CONSOLE
    return Seed(name=name, category="DAC", subcategory="Drums", comment=comment,
                params=merge(MD1, FM_REST, dac(enable=1, sample=slot, rate=rates[0], keyed=keyed, loop=0,
                                               volume=127), PSG_OFF),
                axes=axes, tags=["dac", "drums"] + tags, global_params=dict(DAC_GLOBALS),
                samples={"dac_sample": sample})


# Common variant axes.
VELOCITY = [("light", {"velocity_depth": 16}), ("heavy", {"velocity_depth": 40})]
OCT_DOWN = [0, -12]
OCT_UP = [0, 12]

# Chip revision (docs/research/genesis-sound-design.md, "Console"): the default is the discrete
# YM2612 of a Model 1 (ladder effect, 3.39 kHz output low-pass); the YM3438 variant is the ASIC of
# later consoles (linear DAC; its own filter is not modelled, docs/research/genesis.md Ambiguity 16).
CHIP = [("YM2612", {"chip_revision": 0, "model1_lowpass": 1}),
        ("YM3438", {"chip_revision": 1, "model1_lowpass": 0})]


def unison(label: str, fnum: int) -> tuple[str, dict[str, int]]:
    """Driver unison `fnum` units up on a second FM channel, centred with fine_tune.

    The partner is `fnum` units higher; one fnum unit is 1.4 to 2.8 cents across an octave
    (2.0 on average), so the pair's centre sits about `fnum` cents above the key. fine_tune moves
    both channels, so fine_tune = -fnum cents puts the key in the middle of the pair.
    """
    return (label, {"unison_detune": fnum, "fine_tune": -fnum})


NO_UNISON = ("off", {"unison_detune": 0, "fine_tune": 0})

# Vibrato choices shared by brass and leads: none, the driver's delayed software vibrato
# (rate 5 frames, 6 fnum units = about 12 cents, after 20 frames = 330 ms), or the hardware LFO
# (setting 3 = 6.2 Hz, FMS 3 = 10 cents) which starts with the note.
VIB_OFF = ("off", {"vibrato_rate": 0, "vibrato_depth": 0, "vibrato_delay": 0, "lfo_enable": 0, "lfo_freq": 0,
                   "fms": 0})
VIB_DELAYED = ("delayed", {"vibrato_rate": 5, "vibrato_depth": 6, "vibrato_delay": 20, "lfo_enable": 0,
                           "lfo_freq": 0, "fms": 0})
VIB_LFO = ("LFO", {"vibrato_rate": 0, "vibrato_depth": 0, "vibrato_delay": 0, "lfo_enable": 1, "lfo_freq": 3,
                   "fms": 3})


# ----- FM Bass (algorithms 0-4) ------------------------------------------------------------------
#
# Design rules (docs/research/genesis-sound-design.md, "FM Bass"): one carrier at TL 0 (algorithms
# 0-3) or two carriers whose linear sum stays under the channel clamp (algorithm 4), modulators at
# TL 18-40, MUL 1/2/3 ratios, S1 feedback 3-7 for the saw-like buzz, AR 31 everywhere, RS 1 so high
# notes are a little shorter, RR 8-9 for a clean stop.

def _fm_bass() -> list[Seed]:
    cat = "FM Bass"
    return [
        fm_seed("Slap Bass", cat, "Alg",
                "Slap bass (alg 3). S1 (feedback 5-7) -> S2 is the sustained buzz; S3 at MUL 3 hits the S4 "
                "carrier directly and falls to silence (DR 20, SL 15: about 160 ms at C2) for the thumb pop. "
                "The pop axis moves S3's TL, the feedback axis the buzz.",
                sys=MD1, patch=voice(algorithm=3, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(30, 31, 10, 3, 9, 4, 1, 0, 1, 0, 0),
                      (36, 31, 12, 4, 9, 5, 1, 0, 1, 0, 0),
                      (22, 31, 20, 10, 9, 15, 3, 0, 1, 0, 0),
                      (0, 31, 8, 3, 9, 3, 1, 0, 1, 0, 0)],
                axes={"pop": [("soft", {"op3_tl": 32}), ("hard", {"op3_tl": 16})], "feedback": [5, 7],
                      "transpose": OCT_DOWN},
                tags=["bass", "slap"]),
        fm_seed("Pick Bass", cat, "Alg",
                "Picked bass (alg 1): S1 (feedback) and S2 both drive S3 -> S4; S2 at MUL 3 dies in about "
                "70 ms (DR 22, SL 15) for the pick click. 'muted' shortens the S3/S4 decay like a palm mute "
                "(-24 dB in about 0.3 s and a fast release), 'ringing' lets the string sustain.",
                sys=MD1, patch=voice(algorithm=1, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(30, 31, 12, 4, 9, 4, 1, 0, 1, 0, 0),
                      (34, 31, 22, 8, 9, 15, 3, 0, 1, 0, 0),
                      (28, 31, 10, 4, 9, 4, 1, 3, 1, 0, 0),
                      (0, 31, 8, 4, 9, 3, 1, 0, 1, 0, 0)],
                axes={"feedback": [3, 6],
                      "decay": [("muted", {**ops("dr", {3: 14, 4: 14}), **ops("sl", {3: 8, 4: 8}),
                                           **ops("sr", {3: 10, 4: 10}), "op4_rr": 12}),
                                ("ringing", {**ops("dr", {3: 8, 4: 5}), **ops("sl", {3: 3, 4: 2}),
                                             **ops("sr", {3: 2, 4: 2}), "op4_rr": 8})],
                      "transpose": OCT_DOWN},
                tags=["bass", "pick"]),
        fm_seed("Synth Bass", cat, "Alg",
                "Analog-style synth bass (alg 0, serial): S1 feedback 6 is a saw, S2 and S3 add more "
                "harmonics. S3, the last modulator, works like a filter envelope: 'pluck' lets it fall 27 dB "
                "(DR 14, SL 9) so the tone closes after the attack, 'held' keeps it open. The chip axis "
                "compares the Model 1 YM2612 with the YM3438.",
                sys=MD1, patch=voice(algorithm=0, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(26, 31, 6, 2, 9, 2, 1, 0, 1, 0, 0),
                      (30, 31, 8, 3, 9, 3, 1, 0, 1, 0, 0),
                      (24, 31, 12, 4, 9, 6, 1, 0, 1, 0, 0),
                      (0, 31, 4, 2, 9, 2, 1, 0, 1, 0, 0)],
                axes={"tone": [("dark", ops("tl", {2: 38, 3: 32})), ("bright", ops("tl", {2: 26, 3: 18}))],
                      "sweep": [("pluck", {"op3_dr": 14, "op3_sl": 9, "op3_sr": 4}),
                                ("held", {"op3_dr": 2, "op3_sl": 1, "op3_sr": 0})],
                      "chip": CHIP},
                tags=["bass", "synth"]),
        fm_seed("Square Bass", cat, "Alg",
                "Hollow square-like bass (alg 4): both stacks use a 1:2 carrier:modulator ratio (modulators "
                "MUL 2, carriers MUL 1), which keeps only odd harmonics. Feedback 6 on S1 (TL 24, strong enough "
                "to be heard) adds even ones to the first stack for a reedier tone. Carriers TL 8 and 6: their "
                "sum stays under the clamp.",
                sys=MD1, patch=voice(algorithm=4, feedback=0, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(24, 31, 8, 2, 9, 3, 2, 0, 1, 0, 0),
                      (8, 31, 6, 2, 9, 2, 1, 0, 1, 0, 0),
                      (28, 31, 8, 2, 9, 3, 2, 0, 1, 0, 0),
                      (6, 31, 6, 2, 9, 2, 1, 0, 1, 0, 0)],
                axes={"feedback": [0, 6], "transpose": OCT_DOWN}, tags=["bass", "square"]),
        fm_seed("Sub Bass", cat, "Alg",
                "Round sub bass (alg 2) with modulators at TL 38-56: little more than the fundamental and the "
                "second harmonic, the deep bass under busy tracks. 'held' sustains, 'pluck' falls 18 dB in "
                "about 0.5 s like a finger bass.",
                sys=MD1, patch=voice(algorithm=2, feedback=2, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=16, pan=1),
                rows=[(44, 31, 4, 1, 8, 2, 1, 0, 0, 0, 0),
                      (56, 31, 4, 1, 8, 2, 2, 0, 0, 0, 0),
                      (38, 31, 6, 2, 8, 3, 1, 0, 0, 0, 0),
                      (0, 31, 3, 1, 8, 2, 1, 0, 0, 0, 0)],
                axes={"decay": [("held", {"op4_dr": 3, "op4_sl": 2, "op4_sr": 1}),
                                ("pluck", {"op4_dr": 10, "op4_sl": 6, "op4_sr": 6})],
                      "transpose": OCT_DOWN},
                tags=["bass", "sub"]),
        fm_seed("Growl Bass", cat, "Feedback",
                "Growling bass (alg 0): S1 feedback 4, 6 or 7 into S2 at MUL 2, the buzzy bass of fast action "
                "tracks; feedback 7 turns noisy. Everything else is fixed so the axis isolates the feedback.",
                sys=MD1, patch=voice(algorithm=0, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(22, 31, 6, 2, 9, 2, 1, 0, 1, 0, 0),
                      (28, 31, 8, 3, 9, 3, 2, 0, 1, 0, 0),
                      (32, 31, 10, 3, 9, 4, 1, 3, 1, 0, 0),
                      (0, 31, 5, 2, 9, 2, 1, 0, 1, 0, 0)],
                axes={"feedback": [4, 6, 7], "transpose": OCT_DOWN}, tags=["bass", "growl"]),
        fm_seed("Dist Bass", cat, "Feedback",
                "Overdriven bass on the Sega manual's distortion-guitar algorithm (alg 0) with maximum "
                "feedback: 'medium' has S2/S3 at TL 30, 'heavy' at TL 20, a dense, clipped-sounding spectrum. "
                "S2 at DT -3 roughens it further.",
                sys=MD1, patch=voice(algorithm=0, feedback=7, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(18, 31, 4, 1, 9, 2, 1, 0, 1, 0, 0),
                      (22, 31, 4, 1, 9, 2, 1, 7, 1, 0, 0),
                      (24, 31, 6, 2, 9, 2, 2, 0, 1, 0, 0),
                      (0, 31, 4, 2, 9, 2, 1, 0, 1, 0, 0)],
                axes={"drive": [("medium", ops("tl", {2: 30, 3: 30})), ("heavy", ops("tl", {2: 20, 3: 20}))],
                      "transpose": OCT_DOWN},
                tags=["bass", "distortion"]),
        fm_seed("Wah Bass", cat, "Feedback",
                "Funk 'wah' bass (alg 3): the carrier starts at once but S3, which modulates it directly, "
                "attacks slowly (AR 19: about 20 ms, AR 14: about 110 ms at C2) and then falls 18 dB, so the "
                "tone opens and closes on every note (a slow modulator attack is the documented wah trick). "
                "Feedback 2 or 6 on S1 -> S2 sets the grit.",
                sys=MD1, patch=voice(algorithm=3, feedback=4, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(32, 31, 8, 2, 9, 3, 1, 0, 1, 0, 0),
                      (40, 31, 8, 2, 9, 3, 1, 0, 1, 0, 0),
                      (20, 18, 10, 3, 9, 6, 1, 0, 1, 0, 0),
                      (0, 31, 6, 2, 9, 2, 1, 0, 1, 0, 0)],
                axes={"swell": [("fast", {"op3_ar": 19}), ("slow", {"op3_ar": 14})], "feedback": [2, 6]},
                tags=["bass", "wah"]),
        fm_seed("Chorus Bass", cat, "Detune",
                "Two equal 2-op stacks (alg 4) with the carriers S2/S4 at DT +3/-3. DT is a fixed offset in "
                "Hz, only 0.2-0.5 Hz of beating at bass key codes ('light': a slow swell). 'wide' adds driver "
                "unison 10 fnum units up, centred with fine_tune -10 cents, which beats on every harmonic "
                "(two FM channels per note: 3-note polyphony).",
                sys=MD1, patch=voice(algorithm=4, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(30, 31, 8, 3, 8, 3, 1, 0, 1, 0, 0),
                      (8, 31, 6, 3, 8, 3, 1, 3, 1, 0, 0),
                      (32, 31, 8, 3, 8, 3, 1, 0, 1, 0, 0),
                      (8, 31, 6, 3, 8, 3, 1, 7, 1, 0, 0)],
                axes={"detune": [("light", {"unison_detune": 0, "fine_tune": 0}), unison("wide", 10)],
                      "transpose": OCT_DOWN},
                tags=["bass", "chorus"]),
        fm_seed("Fat Bass", cat, "Detune",
                "Driver unison bass (alg 2): each note takes two FM channels (3-note polyphony), the second 8 "
                "or 14 fnum units up (about 16 or 28 cents), both centred on the key with fine_tune. The "
                "patch is plain so the doubling is what you hear.",
                sys=MD1, patch=voice(algorithm=2, feedback=4, ams=0, fms=0, transpose=0, fine_tune=-8,
                                     vibrato=(0, 0, 0), unison_detune=8, velocity_depth=20, pan=1),
                rows=[(30, 31, 6, 2, 8, 3, 1, 0, 0, 0, 0),
                      (40, 31, 8, 2, 8, 3, 1, 0, 0, 0, 0),
                      (32, 31, 6, 2, 8, 3, 1, 0, 0, 0, 0),
                      (0, 31, 4, 2, 8, 2, 1, 0, 0, 0, 0)],
                axes={"detune": [unison("medium", 8), unison("wide", 14)], "transpose": OCT_DOWN},
                tags=["bass", "unison"]),
        fm_seed("Octave Bass", cat, "Detune",
                "Octave-layered bass (alg 4): the S2 carrier at MUL 2 sounds an octave above the S4 carrier; "
                "the octave axis sets how loud the upper layer is (TL 20 or 10).",
                sys=MD1, patch=voice(algorithm=4, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(34, 31, 8, 2, 8, 3, 2, 0, 1, 0, 0),
                      (12, 31, 8, 3, 8, 4, 2, 0, 1, 0, 0),
                      (30, 31, 8, 2, 8, 3, 1, 0, 1, 0, 0),
                      (4, 31, 6, 2, 8, 3, 1, 0, 1, 0, 0)],
                axes={"octave": [("soft", {"op2_tl": 20}), ("loud", {"op2_tl": 10})], "transpose": OCT_DOWN},
                tags=["bass", "octave"]),
    ]


# ----- FM Keys (algorithms 2-7) --------------------------------------------------------------
#
# Electric pianos and clavs: a bright modulator that decays much faster than its carrier
# (Chowning: percussive spectra go from complex to simple), RS 1-2 so high notes decay faster.
# Bells and mallets: non-integer carrier:modulator ratios (7:2, 11:4, 4:1 ...) and DT for the
# inharmonic beating, modulators decaying with the carriers (Chowning's bell: index proportional to
# amplitude), SL 15 so every partial rings to silence.

def _fm_keys() -> list[Seed]:
    cat = "FM Keys"
    return [
        fm_seed("Tine EPiano", cat, "EPiano",
                "Tine electric piano (alg 4, the DX-style layout): S1 at MUL 14 gives the metallic tine "
                "and dies in about 160 ms (DR 17, SL 15, RS 2); S3 -> S4 is the soft body. The carriers S2/S4 "
                "at DT +3/-3 beat slowly like a chorus. The tine axis moves S1's TL, the chip axis compares "
                "YM2612 and YM3438.",
                sys=MD1, patch=voice(algorithm=4, feedback=2, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=32, pan=1),
                rows=[(40, 31, 17, 0, 8, 15, 14, 0, 2, 0, 0),
                      (10, 31, 9, 3, 7, 5, 1, 3, 1, 0, 0),
                      (38, 31, 8, 2, 7, 4, 1, 0, 1, 0, 0),
                      (6, 31, 9, 3, 7, 5, 1, 7, 1, 0, 0)],
                axes={"tine": [("soft", {"op1_tl": 52}), ("medium", {"op1_tl": 40}), ("bright", {"op1_tl": 28})],
                      "chip": CHIP},
                tags=["keys", "epiano"]),
        fm_seed("Soft EPiano", cat, "EPiano",
                "Mellow EP (alg 5): one S1 modulator feeds three carriers at MUL 1, 2 and 1 (DT -3). The "
                "carriers have AM on, so the tremolo axis (LFO + AMS) moves the level only; the tone axis "
                "sets S1's TL.",
                sys=MD1, patch=voice(algorithm=5, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=32, pan=1),
                rows=[(34, 31, 9, 2, 6, 4, 1, 0, 1, 0, 0),
                      (8, 31, 8, 3, 6, 5, 1, 0, 1, 1, 0),
                      (16, 31, 9, 3, 6, 5, 2, 3, 1, 1, 0),
                      (10, 31, 8, 3, 6, 5, 1, 7, 1, 1, 0)],
                axes={"tremolo": [("off", {"lfo_enable": 0, "lfo_freq": 0, "ams": 0}),
                                  ("slow", {"lfo_enable": 1, "lfo_freq": 1, "ams": 1}),
                                  ("deep", {"lfo_enable": 1, "lfo_freq": 3, "ams": 2})],
                      "tone": [("soft", {"op1_tl": 42}), ("bright", {"op1_tl": 28})]},
                tags=["keys", "epiano"]),
        fm_seed("Clav", cat, "EPiano",
                "Clavinet (alg 4): modulators at MUL 3 and 5 against carriers at MUL 1 and 2 give the nasal "
                "bark; short carrier decays (DR 12, SL 8, SR 8) and RR 10 give the damped pluck, RS 2 makes "
                "high notes snappier. The bite axis moves both modulator TLs, the damping axis the carrier decay.",
                sys=MD1, patch=voice(algorithm=4, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=30, pan=1),
                rows=[(30, 31, 18, 6, 10, 8, 3, 0, 2, 0, 0),
                      (6, 31, 12, 8, 10, 8, 1, 0, 2, 0, 0),
                      (36, 31, 20, 6, 10, 9, 5, 0, 2, 0, 0),
                      (10, 31, 12, 8, 10, 8, 2, 0, 2, 0, 0)],
                axes={"bite": [("soft", ops("tl", {1: 40, 3: 46})), ("medium", ops("tl", {1: 30, 3: 36})),
                               ("hard", ops("tl", {1: 20, 3: 26}))],
                      "damping": [("tight", {**ops("dr", {2: 15, 4: 15}), **ops("sr", {2: 11, 4: 11})}),
                                  ("loose", {**ops("dr", {2: 9, 4: 9}), **ops("sr", {2: 5, 4: 5})})]},
                tags=["keys", "clav"]),
        fm_seed("Wurli", cat, "EPiano",
                "Reedy EP (alg 4): S3 at MUL 7 (DT +3) decays fast for the bark on the S4 body, S1 -> S2 is "
                "a warm second stack; carriers have AM on for the optional tremolo (LFO 5.9 Hz, 1.4 dB).",
                sys=MD1, patch=voice(algorithm=4, feedback=4, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=32, pan=1),
                rows=[(30, 31, 10, 3, 6, 5, 1, 0, 1, 0, 0),
                      (10, 31, 8, 3, 6, 5, 1, 0, 1, 1, 0),
                      (40, 31, 16, 4, 6, 8, 7, 3, 1, 0, 0),
                      (6, 31, 7, 3, 6, 4, 1, 0, 1, 1, 0)],
                axes={"tremolo": [("off", {"lfo_enable": 0, "lfo_freq": 0, "ams": 0}),
                                  ("slow", {"lfo_enable": 1, "lfo_freq": 2, "ams": 1})],
                      "bark": [("soft", {"op3_tl": 48}), ("hard", {"op3_tl": 32})]},
                tags=["keys", "epiano"]),
        fm_seed("FM Piano", cat, "EPiano",
                "Acoustic-piano approximation in the layout of the Sega manual's Grand Piano example: alg 2, "
                "feedback 3, carrier at TL 0, modulators at TL 36-46 including a MUL 13 hammer partial that "
                "decays first. Every operator decays (RS 2: high notes shorter), like a struck string. The "
                "tone axis moves S1/S3, the chip axis compares YM2612 and YM3438.",
                sys=MD1, patch=voice(algorithm=2, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=32, pan=1),
                rows=[(36, 31, 6, 2, 7, 6, 1, 7, 2, 0, 0),
                      (46, 31, 12, 4, 7, 10, 13, 0, 2, 0, 0),
                      (38, 31, 7, 2, 7, 6, 3, 3, 2, 0, 0),
                      (0, 31, 7, 3, 7, 8, 1, 0, 2, 0, 0)],
                axes={"tone": [("mellow", ops("tl", {1: 44, 3: 46})), ("bright", ops("tl", {1: 30, 3: 32}))],
                      "chip": CHIP},
                tags=["keys", "piano"]),
        fm_seed("Harpsichord", cat, "EPiano",
                "Harpsichord (alg 4): carriers at MUL 1 and 2 like an 8' and a 4' choir, modulators at MUL 3 "
                "and 7 for the plucked quill; everything decays (carrier SL 10, RS 2). Feedback 1 or 5 sets "
                "the rasp.",
                sys=MD1, patch=voice(algorithm=4, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(28, 31, 14, 6, 9, 10, 3, 0, 2, 0, 0),
                      (8, 31, 10, 6, 9, 10, 1, 0, 2, 0, 0),
                      (34, 31, 16, 6, 9, 12, 7, 0, 2, 0, 0),
                      (12, 31, 10, 6, 9, 10, 2, 0, 2, 0, 0)],
                axes={"feedback": [1, 5], "transpose": OCT_UP}, tags=["keys", "harpsichord"]),
        fm_seed("Glock", cat, "Bell",
                "Glockenspiel (alg 7, four sines): partials at MUL 2, 7, 5 and 11 with DT spread, i.e. "
                "1 : 3.5 : 2.5 : 5.5 over the MUL 2 fundamental (an octave above the key, as the instrument "
                "sounds); each decays to silence, higher ones faster.",
                sys=MD1, patch=voice(algorithm=7, feedback=0, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=32, pan=1),
                rows=[(4, 31, 8, 0, 5, 15, 2, 0, 2, 0, 0),
                      (24, 31, 12, 0, 5, 15, 7, 3, 2, 0, 0),
                      (28, 31, 10, 0, 5, 15, 5, 6, 2, 0, 0),
                      (30, 31, 14, 0, 5, 15, 11, 1, 2, 0, 0)],
                axes={"transpose": OCT_UP,
                      "decay": [("short", ops("dr", {1: 14, 2: 18, 3: 16, 4: 20})),
                                ("long", ops("dr", {1: 6, 2: 9, 3: 8, 4: 11}))]},
                tags=["bell", "glock"]),
        fm_seed("Tubular Bell", cat, "Bell",
                "Tubular bell (alg 5): S1 at MUL 7 modulates carriers at MUL 2, 2 (DT +3) and 4 (DT -3); the "
                "7 : 2 = 3.5 ratio gives the inharmonic strike, which fades with the carriers (slow DR, SL 15). "
                "The strike axis moves S1's TL; alg 6 keeps the modulation on S2 only.",
                sys=MD1, patch=voice(algorithm=5, feedback=0, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=30, pan=1),
                rows=[(30, 31, 6, 0, 4, 15, 7, 0, 1, 0, 0),
                      (8, 31, 5, 0, 4, 15, 2, 0, 1, 0, 0),
                      (10, 31, 6, 0, 4, 15, 2, 3, 1, 0, 0),
                      (16, 31, 8, 0, 4, 15, 4, 7, 1, 0, 0)],
                axes={"algorithm": [5, 6], "strike": [("soft", {"op1_tl": 40}), ("hard", {"op1_tl": 22})]},
                tags=["bell", "tubular"]),
        fm_seed("Chime", cat, "Bell",
                "Chime (alg 4, which the Sega manual lists for bells): stacks at 7 : 2 (3.5) and 11 : 4 "
                "(2.75) with opposite DT; the strike axis sets both modulator TLs.",
                sys=MD1, patch=voice(algorithm=4, feedback=1, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=32, pan=1),
                rows=[(28, 31, 9, 0, 5, 15, 7, 0, 2, 0, 0),
                      (4, 31, 7, 0, 5, 15, 2, 0, 2, 0, 0),
                      (34, 31, 11, 0, 5, 15, 11, 2, 2, 0, 0),
                      (12, 31, 8, 0, 5, 15, 4, 6, 2, 0, 0)],
                axes={"strike": [("soft", ops("tl", {1: 38, 3: 44})), ("hard", ops("tl", {1: 20, 3: 26}))],
                      "transpose": OCT_UP},
                tags=["bell", "chime"]),
        fm_seed("Vibraphone", cat, "Bell",
                "Vibraphone (alg 6, the Sega manual's suggestion): S1 MUL 4 -> S2 strike plus sine carriers "
                "at MUL 1 (DT +3) and 4. The bars are tuned so the first overtone is two octaves up (1 : 4). "
                "Carriers have AM on and the LFO plays the motor tremolo, slow (3.9 Hz) or fast (6.7 Hz), at "
                "AMS 1 or 2.",
                sys=md1_lfo(0), patch=voice(algorithm=6, feedback=0, ams=1, fms=0, transpose=0, fine_tune=0,
                                            vibrato=(0, 0, 0), unison_detune=0, velocity_depth=32, pan=1),
                rows=[(38, 31, 12, 0, 5, 15, 4, 0, 1, 0, 0),
                      (8, 31, 6, 0, 5, 15, 1, 0, 1, 1, 0),
                      (10, 31, 7, 0, 5, 15, 1, 3, 1, 1, 0),
                      (20, 31, 9, 0, 5, 15, 4, 0, 1, 1, 0)],
                axes={"motor": [("slow", {"lfo_freq": 0}), ("fast", {"lfo_freq": 4})], "ams": [1, 2]},
                tags=["bell", "vibraphone", "tremolo"]),
        fm_seed("Marimba", cat, "Bell",
                "Marimba (alg 6, the Sega manual's xylophone algorithm): Chowning's wood-drum principle, a "
                "burst of modulation that collapses at once (S1 MUL 4, DR 20, SL 15) onto the S2 fundamental "
                "and a quieter S3 at the bar's tuned 4th partial; S4 at MUL 10 is the mallet click. The mallet "
                "axis moves S1's TL.",
                sys=MD1, patch=voice(algorithm=6, feedback=0, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=30, pan=1),
                rows=[(30, 31, 20, 0, 6, 15, 4, 0, 2, 0, 0),
                      (4, 31, 11, 0, 6, 15, 1, 0, 2, 0, 0),
                      (22, 31, 16, 0, 6, 15, 4, 0, 2, 0, 0),
                      (18, 31, 22, 0, 6, 15, 10, 0, 2, 0, 0)],
                axes={"mallet": [("soft", {"op1_tl": 40}), ("hard", {"op1_tl": 24})]},
                tags=["bell", "mallet", "marimba"]),
        fm_seed("Xylophone", cat, "Bell",
                "Xylophone (alg 6): like the marimba but with the 3rd partial of a xylophone bar (MUL 3 on S1 "
                "and S3), faster decays and an octave up, where the instrument sounds.",
                sys=MD1, patch=voice(algorithm=6, feedback=0, ams=0, fms=0, transpose=12, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=30, pan=1),
                rows=[(28, 31, 22, 0, 7, 15, 3, 0, 2, 0, 0),
                      (4, 31, 14, 0, 7, 15, 1, 0, 2, 0, 0),
                      (20, 31, 18, 0, 7, 15, 3, 0, 2, 0, 0),
                      (20, 31, 24, 0, 7, 15, 10, 0, 2, 0, 0)],
                axes={"mallet": [("soft", {"op1_tl": 38}), ("hard", {"op1_tl": 22})]},
                tags=["bell", "mallet", "xylophone"]),
        fm_seed("Music Box", cat, "Bell",
                "Music box (alg 7): the S1 fundamental at MUL 2 with a detuned twin on S3 (the comb's beating "
                "pair) and faint partials at MUL 5 and 9 (2.5 and 4.5 times the fundamental); short, bright "
                "tines (RS 2, SL 15) one or two octaves up.",
                sys=MD1, patch=voice(algorithm=7, feedback=0, ams=0, fms=0, transpose=12, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=28, pan=1),
                rows=[(6, 31, 10, 0, 6, 15, 2, 0, 2, 0, 0),
                      (30, 31, 14, 0, 6, 15, 5, 1, 2, 0, 0),
                      (14, 31, 11, 0, 6, 15, 2, 5, 2, 0, 0),
                      (32, 31, 16, 0, 6, 15, 9, 0, 2, 0, 0)],
                axes={"transpose": [12, 24],
                      "detune": [("light", ops("dt", {2: 1, 3: 5})), ("wide", ops("dt", {2: 3, 3: 7}))]},
                tags=["bell", "musicbox"]),
        fm_seed("Temple Bell", cat, "Bell",
                "Low bell (alg 5): S1 at MUL 7 against a MUL 0 (x0.5) carrier, the hum tone an octave below "
                "the strike, plus MUL 2 and 3 carriers detuned; feedback roughens the strike and slow DR lets "
                "it hang.",
                sys=MD1, patch=voice(algorithm=5, feedback=4, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(32, 31, 4, 0, 3, 15, 7, 0, 0, 0, 0),
                      (8, 31, 4, 0, 3, 15, 0, 0, 0, 0, 0),
                      (14, 31, 5, 0, 3, 15, 2, 3, 0, 0, 0),
                      (18, 31, 6, 0, 3, 15, 3, 5, 0, 0, 0)],
                axes={"feedback": [1, 5],
                      "decay": [("medium", ops("dr", {1: 6, 2: 6, 3: 7, 4: 8})),
                                ("long", ops("dr", {1: 3, 2: 3, 3: 4, 4: 5}))]},
                tags=["bell", "gong"]),
    ]


# ----- FM Brass (stabs, brass, leads, SSG-EG) ---------------------------------------------------
#
# Brass follows Chowning's brass premises: harmonic spectrum (MUL 1 everywhere), brightness that
# grows with loudness (modulators attack a little slower than the carrier: AR 18-22 against 26-31),
# a small overshoot after the attack (carrier SL 1). Leads (saw, pulse, guitars) live here too
# (docs/research/genesis-sound-design.md, "Category mapping").

def _fm_brass() -> list[Seed]:
    cat = "FM Brass"
    return [
        fm_seed("Brass Stab", cat, "Stab",
                "Short brass hit (alg 2, the Sega manual's brass algorithm): S1 feedback saw into S4 plus the "
                "S2 -> S3 chain. The S4 carrier decays by itself (DR 16, SL 12: -36 dB in about 170 ms at C4, "
                "then SR 10), so a held chord ends like a sampled stab.",
                sys=MD1, patch=voice(algorithm=2, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(24, 31, 10, 4, 9, 4, 1, 0, 1, 0, 0),
                      (40, 31, 8, 3, 9, 4, 1, 0, 1, 0, 0),
                      (28, 31, 9, 4, 9, 3, 1, 0, 1, 0, 0),
                      (0, 31, 16, 10, 9, 12, 1, 0, 1, 0, 0)],
                axes={"feedback": [3, 5, 7]}, tags=["brass", "stab"]),
        fm_seed("Orch Stab", cat, "Stab",
                "Orchestra-hit stab (alg 4): two stacks an octave apart (carriers MUL 1 and 2, DT +2/-2), "
                "feedback 6; the carriers decay by themselves (DR 16, SL 12, SR 10). Alg 5 turns S3 into a "
                "third, quiet carrier; the unison axis doubles each note on a second channel 6 fnum units up, "
                "centred with fine_tune.",
                sys=MD1, patch=voice(algorithm=4, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(22, 31, 9, 4, 9, 4, 1, 0, 1, 0, 0),
                      (8, 31, 16, 10, 9, 12, 1, 2, 1, 0, 0),
                      (26, 31, 16, 10, 9, 12, 1, 0, 1, 0, 0),
                      (8, 31, 16, 10, 9, 12, 2, 6, 1, 0, 0)],
                axes={"algorithm": [4, 5], "unison": [NO_UNISON, unison("wide", 6)]},
                tags=["brass", "stab", "orchestra"]),
        fm_seed("Funk Stab", cat, "Stab",
                "Tight horn blip (alg 3): S1 -> S2 (MUL 2) and S3 (DT +3) both into S4. 'short' makes the "
                "carrier fall 42 dB in about 60 ms, 'long' in about 170 ms; feedback sets the rasp.",
                sys=MD1, patch=voice(algorithm=3, feedback=4, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(26, 31, 14, 6, 10, 6, 1, 0, 1, 0, 0),
                      (34, 31, 12, 5, 10, 5, 2, 0, 1, 0, 0),
                      (30, 31, 12, 5, 10, 5, 1, 3, 1, 0, 0),
                      (0, 31, 18, 12, 10, 14, 1, 0, 1, 0, 0)],
                axes={"feedback": [3, 6],
                      "length": [("short", {"op4_dr": 20, "op4_sr": 14}), ("long", {"op4_dr": 15, "op4_sr": 8})]},
                tags=["brass", "stab", "funk"]),
        fm_seed("Power Stab", cat, "Stab",
                "Wide power stab (alg 5, which the Sega manual lists for brass and organ): S1 feedback drives "
                "carriers at MUL 1, 2 and 3 (detuned), an organ-brass hybrid that fills a chord from one "
                "note; the carriers decay in about 350 ms (DR 14, SL 12, then SR 8).",
                sys=MD1, patch=voice(algorithm=5, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(20, 31, 8, 3, 9, 3, 1, 0, 1, 0, 0),
                      (8, 31, 14, 8, 9, 12, 1, 0, 1, 0, 0),
                      (12, 31, 14, 8, 9, 12, 2, 3, 1, 0, 0),
                      (18, 31, 14, 8, 9, 12, 3, 7, 1, 0, 0)],
                axes={"feedback": [4, 6], "transpose": OCT_DOWN}, tags=["brass", "stab"]),
        fm_seed("Trumpet", cat, "Brass",
                "Solo trumpet (alg 2) on Chowning's brass premises: all ratios 1:1 (harmonic, odd and even), "
                "modulators attacking slower (AR 20-22, about 10 ms at C4) than the carrier (AR 26), so the "
                "tone brightens as it gets louder, and a 3 dB overshoot (carrier SL 1). Vibrato by the driver "
                "(delayed) or the hardware LFO.",
                sys=MD1, patch=voice(algorithm=2, feedback=4, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=28, pan=1),
                rows=[(28, 20, 6, 1, 7, 2, 1, 0, 1, 0, 0),
                      (40, 22, 6, 1, 7, 2, 1, 0, 1, 0, 0),
                      (30, 20, 6, 1, 7, 2, 1, 0, 1, 0, 0),
                      (0, 26, 5, 1, 7, 1, 1, 0, 1, 0, 0)],
                axes={"vibrato": [VIB_OFF, VIB_DELAYED, VIB_LFO], "feedback": [3, 5]},
                tags=["brass", "trumpet"]),
        fm_seed("Horn Section", cat, "Brass",
                "Horn section (alg 4): two identical stacks with carriers at DT +2/-2 read as two players. "
                "'swell' slows every attack (AR 14-16: about 60-110 ms at C4, the modulators slightly later "
                "than the carriers) for a blown entry; vibrato off or delayed.",
                sys=MD1, patch=voice(algorithm=4, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(30, 31, 5, 1, 7, 2, 1, 0, 1, 0, 0),
                      (8, 31, 4, 1, 7, 2, 1, 2, 1, 0, 0),
                      (30, 31, 5, 1, 7, 2, 1, 0, 1, 0, 0),
                      (8, 31, 4, 1, 7, 2, 1, 6, 1, 0, 0)],
                axes={"attack": [("fast", all_ops("ar", 31)), ("swell", ops("ar", {1: 14, 2: 16, 3: 14, 4: 16}))],
                      "vibrato": [VIB_OFF, VIB_DELAYED]},
                tags=["brass", "section"]),
        fm_seed("Synth Brass", cat, "Brass",
                "80s synth brass (alg 3, feedback 6): S2 and S3 into S4 with opposite DT for movement; 'slow' "
                "delays the modulators (AR 16, about 60 ms at C4) for the filter-sweep-like opening.",
                sys=MD1, patch=voice(algorithm=3, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(22, 31, 6, 2, 8, 2, 1, 0, 1, 0, 0),
                      (34, 31, 7, 2, 8, 3, 1, 1, 1, 0, 0),
                      (28, 31, 6, 2, 8, 3, 1, 5, 1, 0, 0),
                      (0, 31, 5, 2, 8, 2, 1, 0, 1, 0, 0)],
                axes={"detune": [("light", ops("dt", {2: 1, 3: 5})), ("wide", ops("dt", {2: 3, 3: 7}))],
                      "attack": [("fast", ops("ar", {1: 31, 3: 31})), ("slow", ops("ar", {1: 16, 3: 16}))]},
                tags=["brass", "synth"]),
        fm_seed("Tuba", cat, "Brass",
                "Low tuba/trombone (alg 2) an octave down: blown onsets (modulators AR 14-16, carrier AR 18) "
                "so the tone opens after the note starts; feedback 3 or 6.",
                sys=MD1, patch=voice(algorithm=2, feedback=5, ams=0, fms=0, transpose=-12, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(28, 14, 5, 1, 7, 2, 1, 0, 0, 0, 0),
                      (44, 16, 5, 1, 7, 2, 1, 0, 0, 0, 0),
                      (32, 16, 5, 1, 7, 2, 1, 0, 0, 0, 0),
                      (0, 18, 4, 1, 7, 2, 1, 0, 0, 0, 0)],
                axes={"feedback": [3, 6]}, tags=["brass", "tuba"]),
        fm_seed("Saw Lead", cat, "Brass",
                "Saw lead (alg 4): S1 feedback 6 at TL 20 makes S2 a bright saw, S3 -> S4 (DT +3) a softer "
                "second voice; sustained (SL 1). Vibrato delayed or LFO; 'wide' adds unison 6 fnum units, "
                "centred with fine_tune.",
                sys=MD1, patch=voice(algorithm=4, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(20, 31, 4, 1, 8, 2, 1, 0, 1, 0, 0),
                      (8, 31, 3, 1, 8, 1, 1, 0, 1, 0, 0),
                      (30, 31, 4, 1, 8, 2, 1, 0, 1, 0, 0),
                      (8, 31, 3, 1, 8, 1, 1, 3, 1, 0, 0)],
                axes={"vibrato": [VIB_DELAYED, VIB_LFO],
                      "detune": [("light", {"unison_detune": 0, "fine_tune": 0}), unison("wide", 6)]},
                tags=["lead", "saw"]),
        fm_seed("Pulse Lead", cat, "Brass",
                "Pulse lead (alg 4): S1 at MUL 2 over S2 (1:2, odd harmonics, square-like) and S3 at MUL 3 "
                "over S4 (1:3, a narrower pulse), carriers TL 8 each; vibrato delayed or LFO, at the key or "
                "an octave up.",
                sys=MD1, patch=voice(algorithm=4, feedback=0, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(28, 31, 4, 1, 8, 2, 2, 0, 1, 0, 0),
                      (8, 31, 3, 1, 8, 1, 1, 0, 1, 0, 0),
                      (36, 31, 4, 1, 8, 2, 3, 0, 1, 0, 0),
                      (8, 31, 3, 1, 8, 1, 1, 7, 1, 0, 0)],
                axes={"vibrato": [VIB_DELAYED, VIB_LFO], "transpose": OCT_UP}, tags=["lead", "square"]),
        fm_seed("Dist Guitar", cat, "Brass",
                "Distorted rock guitar lead on the Sega manual's distortion-guitar algorithm (alg 0), "
                "feedback 7: 'crunch' has S1/S2 at TL 26/30, 'heavy' at 16/20. The carrier decays slowly "
                "(DR 3, SL 3, SR 2) like a sustained string; vibrato off or delayed.",
                sys=MD1, patch=voice(algorithm=0, feedback=7, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(20, 31, 4, 2, 8, 3, 1, 0, 1, 0, 0),
                      (24, 31, 4, 2, 8, 3, 1, 3, 1, 0, 0),
                      (28, 31, 6, 2, 8, 3, 2, 0, 1, 0, 0),
                      (0, 31, 3, 2, 8, 3, 1, 0, 1, 0, 0)],
                axes={"drive": [("crunch", ops("tl", {1: 26, 2: 30})), ("heavy", ops("tl", {1: 16, 2: 20}))],
                      "vibrato": [VIB_OFF, ("delayed", {"vibrato_rate": 5, "vibrato_depth": 6, "vibrato_delay": 24})]},
                tags=["lead", "guitar", "distortion"]),
        fm_seed("Clean Guitar", cat, "Brass",
                "Clean electric guitar (alg 2, listed in the Sega manual for electric guitar): S2 at MUL 3 "
                "decays in about 100 ms for the pick (the pick axis sets its TL), the carrier falls 15 dB and then "
                "sustains slowly.",
                sys=MD1, patch=voice(algorithm=2, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(34, 31, 8, 3, 8, 5, 1, 0, 1, 0, 0),
                      (44, 31, 14, 6, 8, 10, 3, 0, 1, 0, 0),
                      (32, 31, 8, 3, 8, 5, 1, 0, 1, 0, 0),
                      (0, 31, 6, 3, 8, 5, 1, 0, 1, 0, 0)],
                axes={"pick": [("soft", {"op2_tl": 54}), ("hard", {"op2_tl": 34})], "transpose": OCT_DOWN},
                tags=["lead", "guitar"]),
        fm_seed("Buzz Horn", cat, "SSG",
                "SSG-EG on the S1 modulator (AR 31, DR 16, SL 15 so the decay reaches the 0x200 SSG threshold; "
                "RS 1 makes the cycle follow the key). At C4 one ramp is 58 ms: the saw restarts the "
                "brightness every 58 ms (17 Hz buzz with a hard edge), the triangle falls and rises over "
                "115 ms (8.7 Hz, smooth pulsing).",
                sys=MD1, patch=voice(algorithm=2, feedback=4, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(24, 31, 16, 0, 8, 15, 1, 0, 1, 0, 1),
                      (40, 31, 6, 2, 8, 3, 1, 0, 1, 0, 0),
                      (30, 31, 6, 2, 8, 3, 1, 0, 1, 0, 0),
                      (0, 31, 5, 2, 8, 2, 1, 0, 1, 0, 0)],
                axes={"SSG": [("saw", {"op1_ssg": 1}), ("triangle", {"op1_ssg": 3})],
                      "feedback": [4, 6]},
                tags=["brass", "ssg"]),
        fm_seed("Growl Horn", cat, "SSG",
                "SSG-EG on S3 (DR 18, SL 15, RS 1: one ramp is 29 ms at C4), the modulator of the S4 carrier "
                "(alg 4). Saw: brightness ramps down and restarts at 35 Hz; triangle: down and up at 17 Hz; "
                "inverted saw: ramps up from -48 dB and restarts at 35 Hz.",
                sys=MD1, patch=voice(algorithm=4, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(28, 31, 6, 2, 8, 3, 1, 0, 1, 0, 0),
                      (8, 31, 5, 2, 8, 3, 1, 0, 1, 0, 0),
                      (24, 31, 18, 0, 8, 15, 1, 0, 1, 0, 1),
                      (8, 31, 5, 2, 8, 3, 1, 0, 1, 0, 0)],
                axes={"SSG": [("saw", {"op3_ssg": 1}), ("triangle", {"op3_ssg": 3}), ("InvSaw", {"op3_ssg": 5})]},
                tags=["brass", "ssg", "growl"]),
        fm_seed("Sync Horn", cat, "SSG",
                "Sync-like horn (alg 3): S2 at MUL 2 runs a repeating SSG saw (SL 15, RS 1). Each restart also "
                "resets S2's phase counter, so the S2 -> S4 modulation retriggers like oscillator sync. DR 12 "
                "is a 230 ms ramp (4 Hz pulsing), DR 18 a 29 ms ramp (35 Hz growl), at C4. Faster ramps would "
                "add a restart tone that does not follow the key, so they are left out.",
                sys=MD1, patch=voice(algorithm=3, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(26, 31, 6, 2, 8, 3, 1, 0, 1, 0, 0),
                      (30, 31, 20, 0, 8, 15, 2, 0, 1, 0, 1),
                      (32, 31, 6, 2, 8, 3, 1, 0, 1, 0, 0),
                      (0, 31, 5, 2, 8, 2, 1, 0, 1, 0, 0)],
                axes={"sweep": [("slow", {"op2_dr": 12}), ("medium", {"op2_dr": 18})], "feedback": [3, 6]},
                tags=["brass", "ssg", "sync"]),
        fm_seed("Swell Horn", cat, "SSG",
                "One-shot SSG shapes on the common modulator S1 (alg 5; DR 10, SL 15, RS 1: one 460 ms ramp at "
                "C4). 'once' fades the modulation out over 460 ms and then holds S1 silent (bright onset, pure "
                "tail); 'swell' starts at -48 dB, rises over 460 ms and holds at TL level (brightness swell).",
                sys=MD1, patch=voice(algorithm=5, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(24, 31, 10, 0, 8, 15, 1, 0, 1, 0, 2),
                      (8, 31, 5, 2, 8, 3, 1, 0, 1, 0, 0),
                      (16, 31, 5, 2, 8, 3, 2, 3, 1, 0, 0),
                      (12, 31, 5, 2, 8, 3, 1, 7, 1, 0, 0)],
                axes={"SSG": [("once", {"op1_ssg": 2}), ("swell", {"op1_ssg": 6})]},
                tags=["brass", "ssg"]),
    ]


# ----- FM Pad (slow attacks, strings, organs, unison, LFO; algorithms 3-7) --------------------------
#
# Pads fade in (AR 7-14 on every operator: the modulators follow the carriers, so the brightness
# grows with the level) and hold (DR 0-2, SR 0). Organs are additive (alg 7, the Sega manual's pipe
# organ): sines at drawbar ratios with no decay. Wind leads use the LFO subcategory.

def _fm_pad() -> list[Seed]:
    cat = "FM Pad"
    slow_attack = [("slow", all_ops("ar", 12)), ("slower", all_ops("ar", 7))]
    return [
        fm_seed("Warm Pad", cat, "Slow",
                "Warm pad (alg 5): one soft S1 modulator over three carriers (MUL 1, 2, 1 with DT +3/-3) that "
                "fade in (AR 12 or 7) and hold (SR 0); alg 6 limits the modulation to S2 for a purer sound.",
                sys=MD1, patch=voice(algorithm=5, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(36, 10, 2, 0, 4, 2, 1, 0, 0, 0, 0),
                      (8, 10, 2, 0, 4, 1, 1, 0, 0, 0, 0),
                      (18, 9, 2, 0, 4, 1, 2, 3, 0, 0, 0),
                      (12, 11, 2, 0, 4, 1, 1, 7, 0, 0, 0)],
                axes={"attack": slow_attack, "algorithm": [5, 6]}, tags=["pad", "warm"]),
        fm_seed("Glass Pad", cat, "Slow",
                "Glassy additive pad (alg 7): sines at MUL 1, 2, 4 and 1 with DT spread and feedback 2 on S1; "
                "slow AR and the release axis (RR 5 or 3, several seconds of tail) shape the swell and tail.",
                sys=MD1, patch=voice(algorithm=7, feedback=2, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(14, 9, 3, 1, 4, 3, 1, 0, 0, 0, 0),
                      (20, 9, 4, 1, 4, 3, 2, 2, 0, 0, 0),
                      (26, 9, 5, 1, 4, 4, 4, 6, 0, 0, 0),
                      (10, 9, 3, 1, 4, 3, 1, 3, 0, 0, 0)],
                axes={"attack": [("slow", all_ops("ar", 11)), ("slower", all_ops("ar", 7))],
                      "release": [("medium", all_ops("rr", 5)), ("long", all_ops("rr", 3))]},
                tags=["pad", "glass"]),
        fm_seed("Choir Pad", cat, "Slow",
                "Vocal pad (alg 6): S1 at MUL 3 lightly modulates S2 for a formant, S3/S4 are detuned sines; a "
                "delayed driver vibrato (rate 6, depth 3, 30 frames) adds the singer's wobble. Alg 7 lets S1 "
                "sound as a faint third-harmonic partial instead.",
                sys=MD1, patch=voice(algorithm=6, feedback=0, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(6, 3, 30), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(40, 10, 2, 0, 4, 2, 3, 0, 0, 0, 0),
                      (8, 10, 1, 0, 4, 1, 1, 0, 0, 0, 0),
                      (14, 10, 1, 0, 4, 1, 1, 2, 0, 0, 0),
                      (20, 10, 1, 0, 4, 1, 2, 6, 0, 0, 0)],
                axes={"algorithm": [6, 7], "attack": slow_attack}, tags=["pad", "choir"]),
        fm_seed("Dark Pad", cat, "Slow",
                "Dark drone pad (alg 5): S1 at MUL 0 (x0.5) adds sub-octave sidebands to carriers at MUL 1, 1 "
                "and 0; the S4 carrier at x0.5 puts the fundamental an octave under the key. 'rough' raises "
                "S1 from TL 50 to 32.",
                sys=MD1, patch=voice(algorithm=5, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(44, 8, 2, 0, 3, 2, 0, 0, 0, 0, 0),
                      (10, 8, 2, 0, 3, 1, 1, 0, 0, 0, 0),
                      (12, 8, 2, 0, 3, 1, 1, 5, 0, 0, 0),
                      (14, 8, 2, 0, 3, 1, 0, 3, 0, 0, 0)],
                axes={"tone": [("soft", {"op1_tl": 50}), ("rough", {"op1_tl": 32})], "attack": slow_attack},
                tags=["pad", "dark"]),
        fm_seed("Slow Strings", cat, "Slow",
                "String section (alg 5): S1 feedback 5 gives a bowed, saw-like source to carriers at MUL 1, 1 "
                "(DT +3) and 2 (DT -3). The modulator fades in with the carriers (AR 14 or 10), so the tone "
                "brightens as the bow digs in; the vibrato axis adds the hardware LFO at 5.4 Hz, FMS 2 "
                "(6.7 cents).",
                sys=MD1, patch=voice(algorithm=5, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(26, 14, 2, 0, 5, 2, 1, 0, 0, 0, 0),
                      (10, 14, 2, 0, 5, 1, 1, 0, 0, 0, 0),
                      (12, 14, 2, 0, 5, 1, 1, 3, 0, 0, 0),
                      (18, 14, 2, 0, 5, 1, 2, 7, 0, 0, 0)],
                axes={"attack": [("medium", all_ops("ar", 14)), ("slow", all_ops("ar", 10))],
                      "vibrato": [("off", {"lfo_enable": 0, "lfo_freq": 0, "fms": 0}),
                                  ("LFO", {"lfo_enable": 1, "lfo_freq": 1, "fms": 2})]},
                tags=["pad", "strings"]),
        fm_seed("Pipe Organ", cat, "Slow",
                "Pipe organ on the Sega manual's pipe-organ algorithm (alg 7): sines at MUL 1, 2, 4 and 3 "
                "(8', 4', 2' and 2 2/3' ranks) that speak in about 15 ms (AR 20) and hold without decay; "
                "feedback 1 adds a little reed. 'full' opens the upper ranks.",
                sys=MD1, patch=voice(algorithm=7, feedback=1, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(10, 20, 0, 0, 6, 0, 1, 0, 0, 0, 0),
                      (16, 20, 0, 0, 6, 0, 2, 0, 0, 0, 0),
                      (22, 20, 0, 0, 6, 0, 4, 0, 0, 0, 0),
                      (24, 20, 0, 0, 6, 0, 3, 0, 0, 0, 0)],
                axes={"stops": [("principal", {}), ("full", ops("tl", {2: 14, 3: 16, 4: 18}))],
                      "transpose": OCT_DOWN},
                tags=["pad", "organ"]),
        fm_seed("Super Pad", cat, "Unison",
                "Supersaw-style pad: feedback 6 on the common S1 modulator (alg 5) for a saw-like source, "
                "doubled by driver unison 3 or 10 fnum units up, centred with fine_tune.",
                sys=MD1, patch=voice(algorithm=5, feedback=6, ams=0, fms=0, transpose=0, fine_tune=-3,
                                     vibrato=(0, 0, 0), unison_detune=3, velocity_depth=12, pan=1),
                rows=[(28, 12, 2, 0, 4, 2, 1, 0, 0, 0, 0),
                      (10, 12, 2, 0, 4, 1, 1, 0, 0, 0, 0),
                      (14, 12, 2, 0, 4, 1, 1, 3, 0, 0, 0),
                      (12, 12, 2, 0, 4, 1, 2, 7, 0, 0, 0)],
                axes={"detune": [unison("narrow", 3), unison("wide", 10)],
                      "attack": [("medium", all_ops("ar", 14)), ("slow", all_ops("ar", 9))]},
                tags=["pad", "unison", "saw"]),
        fm_seed("String Ensemble", cat, "Unison",
                "String ensemble (alg 6): S1 -> S2 bowed tone plus two detuned sines, unison doubling (2 or 7 "
                "fnum units, centred) for the section spread; release medium (RR 5) or long (RR 3). Unison "
                "takes two FM channels per note, so only 3 notes sound at once.",
                sys=MD1, patch=voice(algorithm=6, feedback=4, ams=0, fms=0, transpose=0, fine_tune=-2,
                                     vibrato=(0, 0, 0), unison_detune=2, velocity_depth=12, pan=1),
                rows=[(30, 11, 2, 0, 5, 2, 1, 0, 0, 0, 0),
                      (8, 11, 2, 0, 5, 1, 1, 0, 0, 0, 0),
                      (16, 11, 2, 0, 5, 1, 2, 2, 0, 0, 0),
                      (14, 11, 2, 0, 5, 1, 1, 6, 0, 0, 0)],
                axes={"detune": [unison("narrow", 2), unison("wide", 7)],
                      "release": [("medium", all_ops("rr", 5)), ("long", all_ops("rr", 3))]},
                tags=["pad", "unison", "strings"]),
        fm_seed("Chorus Pad", cat, "Unison",
                "Additive chorus pad (alg 7): four sines at MUL 1, 1 (DT +3), 2 and 4 (DT -3) doubled by "
                "centred unison (3-note polyphony, hence the moderate RR 4 tail); the vibrato axis adds a slow "
                "delayed driver vibrato.",
                sys=MD1, patch=voice(algorithm=7, feedback=1, ams=0, fms=0, transpose=0, fine_tune=-3,
                                     vibrato=(0, 0, 0), unison_detune=3, velocity_depth=12, pan=1),
                rows=[(10, 12, 2, 0, 4, 2, 1, 0, 0, 0, 0),
                      (12, 12, 2, 0, 4, 2, 1, 3, 0, 0, 0),
                      (26, 12, 3, 0, 4, 3, 2, 0, 0, 0, 0),
                      (30, 12, 3, 0, 4, 3, 4, 7, 0, 0, 0)],
                axes={"detune": [unison("narrow", 3), unison("wide", 8)],
                      "vibrato": [("off", {"vibrato_rate": 0, "vibrato_depth": 0, "vibrato_delay": 0}),
                                  ("slow", {"vibrato_rate": 7, "vibrato_depth": 4, "vibrato_delay": 24})]},
                tags=["pad", "unison", "chorus"]),
        fm_seed("Drawbar Organ", cat, "Unison",
                "Drawbar organ (alg 7, additive): pure sines at MUL 1, 2, 3 and 4 (8', 4', 2 2/3', 2') with an "
                "instant attack, no decay and a fast release (RR 12), like a tonewheel organ key. 'mellow' "
                "favours the 8', 'bright' opens the upper drawbars; 'chorus' doubles each note 3 fnum units up "
                "(centred), the organ's chorus.",
                sys=MD1, patch=voice(algorithm=7, feedback=0, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(10, 31, 0, 0, 12, 0, 1, 0, 0, 0, 0),
                      (20, 31, 0, 0, 12, 0, 2, 0, 0, 0, 0),
                      (28, 31, 0, 0, 12, 0, 3, 0, 0, 0, 0),
                      (28, 31, 0, 0, 12, 0, 4, 0, 0, 0, 0)],
                axes={"drawbars": [("mellow", {}), ("bright", ops("tl", {1: 10, 2: 14, 3: 18, 4: 18}))],
                      "chorus": [NO_UNISON, unison("on", 3)]},
                tags=["organ", "unison"]),
        fm_seed("Vibrato Pad", cat, "LFO",
                "Hardware-LFO vibrato pad (alg 5): FMS 2 or 5 (6.7 or 20 cents) at LFO setting 1 or 5 (5.4 or "
                "9.5 Hz: a singer's vibrato or a fast flutter), applied to the whole channel as on the chip.",
                sys=md1_lfo(1), patch=voice(algorithm=5, feedback=2, ams=0, fms=2, transpose=0, fine_tune=0,
                                            vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(32, 11, 2, 0, 4, 2, 1, 0, 0, 0, 0),
                      (8, 11, 2, 0, 4, 1, 1, 0, 0, 0, 0),
                      (16, 11, 2, 0, 4, 1, 2, 3, 0, 0, 0),
                      (12, 11, 2, 0, 4, 1, 1, 7, 0, 0, 0)],
                axes={"fms": [2, 5], "lfo_freq": [1, 5]}, tags=["pad", "lfo", "vibrato"]),
        fm_seed("Tremolo Pad", cat, "LFO",
                "Tremolo pad (alg 6): the three carriers have AM on, AMS 1 (1.4 dB) or 3 (11.8 dB) at LFO "
                "setting 0 (3.9 Hz) or 4 (6.7 Hz); S1 -> S2 at MUL 2 keeps a soft reed tone.",
                sys=md1_lfo(0), patch=voice(algorithm=6, feedback=2, ams=1, fms=0, transpose=0, fine_tune=0,
                                            vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(34, 10, 2, 0, 4, 2, 2, 0, 0, 0, 0),
                      (10, 10, 2, 0, 4, 1, 1, 0, 0, 1, 0),
                      (12, 10, 2, 0, 4, 1, 1, 3, 0, 1, 0),
                      (20, 10, 2, 0, 4, 1, 3, 5, 0, 1, 0)],
                axes={"ams": [1, 3], "lfo_freq": [0, 4]}, tags=["pad", "lfo", "tremolo"]),
        fm_seed("Sweep Pad", cat, "LFO",
                "Wah-like pad: AM is set on the S1 modulator only (carriers off), so AMS 3 (11.8 dB) moves the "
                "modulation index instead of the level and the LFO (setting 0 or 4: 3.9 or 6.7 Hz) sweeps the "
                "timbre (alg 5); the tone axis centres the sweep on a soft or a bright S1 level.",
                sys=md1_lfo(0), patch=voice(algorithm=5, feedback=3, ams=3, fms=0, transpose=0, fine_tune=0,
                                            vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(26, 10, 2, 0, 4, 2, 1, 0, 0, 1, 0),
                      (8, 10, 2, 0, 4, 1, 1, 0, 0, 0, 0),
                      (16, 10, 2, 0, 4, 1, 2, 3, 0, 0, 0),
                      (12, 10, 2, 0, 4, 1, 1, 7, 0, 0, 0)],
                axes={"tone": [("soft", {"op1_tl": 34}), ("bright", {"op1_tl": 18})], "lfo_freq": [0, 4]},
                tags=["pad", "lfo", "sweep"]),
        fm_seed("Rotary Organ", cat, "LFO",
                "Organ with a rotary-speaker imitation (alg 7): drawbar sines at MUL 1, 2, 3 and 4 with AM on "
                "every carrier, the LFO at 3.9 Hz ('slow') or 6.7 Hz ('fast'), AMS 1 or 2 for the amplitude "
                "wobble and FMS 1 (3.4 cents) for a slight Doppler shift.",
                sys=md1_lfo(0), patch=voice(algorithm=7, feedback=0, ams=1, fms=1, transpose=0, fine_tune=0,
                                            vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(10, 31, 0, 0, 11, 0, 1, 0, 0, 1, 0),
                      (18, 31, 0, 0, 11, 0, 2, 0, 0, 1, 0),
                      (26, 31, 0, 0, 11, 0, 3, 0, 0, 1, 0),
                      (22, 31, 0, 0, 11, 0, 4, 0, 0, 1, 0)],
                axes={"rotor": [("slow", {"lfo_freq": 0}), ("fast", {"lfo_freq": 4})], "ams": [1, 2]},
                tags=["organ", "lfo", "rotary"]),
        fm_seed("Flute Lead", cat, "LFO",
                "Flute lead (alg 4, listed in the Sega manual for flute): nearly pure carriers (S2 MUL 1, S4 "
                "MUL 1 at DT +2) with a soft onset (AR 24), and S1 (feedback 6) giving a short breathy chiff "
                "that falls 18 dB after the attack. Vibrato from the driver (delayed) or the LFO (5.9 Hz, "
                "10 cents).",
                sys=MD1, patch=voice(algorithm=4, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(46, 24, 10, 2, 7, 6, 1, 0, 1, 0, 0),
                      (8, 24, 2, 0, 7, 1, 1, 0, 1, 0, 0),
                      (54, 24, 4, 1, 7, 2, 2, 0, 1, 0, 0),
                      (12, 24, 2, 0, 7, 1, 1, 2, 1, 0, 0)],
                axes={"vibrato": [("delayed", {"vibrato_rate": 6, "vibrato_depth": 4, "vibrato_delay": 20,
                                               "lfo_enable": 0, "lfo_freq": 0, "fms": 0}),
                                  ("LFO", {"vibrato_rate": 0, "vibrato_depth": 0, "vibrato_delay": 0,
                                           "lfo_enable": 1, "lfo_freq": 2, "fms": 3})],
                      "transpose": OCT_UP},
                tags=["lead", "flute", "lfo"]),
        fm_seed("Clarinet Lead", cat, "LFO",
                "Clarinet lead (alg 4) after Chowning's clarinet: carrier:modulator 3:2 on S2 (MUL 3) with S1 "
                "(MUL 2), and 1:2 on S4 with S3, both of which give odd harmonics only. The modulators start "
                "at full index and fall 12 dB while the carriers rise (AR 22), so the spectrum narrows as the "
                "note gets louder, as in the paper. Hardware-LFO vibrato optional.",
                sys=MD1, patch=voice(algorithm=4, feedback=0, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(30, 31, 10, 2, 7, 4, 2, 0, 1, 0, 0),
                      (8, 22, 2, 0, 7, 1, 3, 0, 1, 0, 0),
                      (36, 31, 8, 2, 7, 3, 2, 0, 1, 0, 0),
                      (10, 22, 2, 0, 7, 1, 1, 0, 1, 0, 0)],
                axes={"vibrato": [("off", {"lfo_enable": 0, "lfo_freq": 0, "fms": 0}),
                                  ("LFO", {"lfo_enable": 1, "lfo_freq": 2, "fms": 2})],
                      "transpose": OCT_DOWN},
                tags=["lead", "clarinet", "lfo"]),
    ]


# ----- DAC drums (channel 6 PCM) ---------------------------------------------------------------

def _dac() -> list[Seed]:
    # The drums and the orchestra hit are CC0 recordings stored at 8-16 kHz (the voice and the noise
    # burst are procedural, 22050 Hz), all with root note 60, and resampled to dac_rate when loaded
    # (see the module docstring), so the rate trades zero-order-hold grain (and, below the stored
    # rate, bandwidth), not pitch.
    low = [8000, 11025, 16000]    # kicks, toms, snares, voice: the classic 8-16 kHz driver rates
    high = [11025, 16000, 22050]  # rim, cowbell, noise: keep more of their top end
    # Hats, cymbals and claps: the Model 1 3.39 kHz low-pass removes most of what separates 16 and
    # 22 kHz, so they get two rates times the console output stage instead of three rates.
    bright = [11025, 22050]
    return [
        dac_seed("Kick", "kick", "Standard DAC kick, one-shot (dac_keyed 0) so every key plays the same "
                 "hit; the rate axis trades crunch (8 kHz zero-order hold) for punch.",
                 keyed=0, rates=low, tags=["kick"]),
        dac_seed("Deep Kick", "kick_deep", "Deep kick from an open concert bass drum, one-shot; at 8 kHz "
                 "the aliasing of the zero-order hold adds the grainy thump of early Mega Drive drivers.",
                 keyed=0, rates=low, tags=["kick"]),
        dac_seed("Snare", "snare", "Full DAC snare, one-shot; lower rates darken and lengthen it like the "
                 "8 kHz snares of early titles.", keyed=0, rates=low, tags=["snare"]),
        dac_seed("Short Snare", "snare_short", "Tight snare for fast grooves, one-shot at 8-16 kHz.",
                 keyed=0, rates=low, tags=["snare"]),
        dac_seed("Clap", "clap", "Hand clap, one-shot at 11 or 22 kHz; the console axis compares the dull "
                 "Model 1 output (3.39 kHz low-pass) with the brighter Model 2.",
                 keyed=0, rates=bright, tags=["clap"], console_axis=True),
        dac_seed("Rim", "rim", "Cross-stick click, one-shot at 11-22 kHz.", keyed=0, rates=high, tags=["rim"]),
        dac_seed("Closed Hat", "hat_closed", "Closed hi-hat on the DAC (games often used PSG noise instead) "
                 "at 11 or 22 kHz; on a Model 1 the 3.39 kHz low-pass takes most of the sizzle, the Model 2 "
                 "variant keeps it.", keyed=0, rates=bright, tags=["hihat"], console_axis=True),
        dac_seed("Open Hat", "hat_open", "Open hi-hat, one-shot at 11 or 22 kHz, Model 1 or Model 2 output.",
                 keyed=0, rates=bright, tags=["hihat"], console_axis=True),
        dac_seed("Crash", "crash_short", "Short crash cymbal, one-shot at 11 or 22 kHz, Model 1 or Model 2 "
                 "output; the 8-bit DAC's quantisation noise is part of the character.", keyed=0,
                 rates=bright, tags=["cymbal"], console_axis=True),
        dac_seed("Cowbell", "cowbell", "Cowbell, one-shot at 11-22 kHz.", keyed=0, rates=high,
                 tags=["cowbell"]),
        dac_seed("Tom", "tom", "Keyed tom (dac_keyed 1: note 60 plays at dac_rate, one semitone per key) so "
                 "a single sample covers a whole tom fill.", keyed=1, rates=low, tags=["tom", "keyed"]),
        dac_seed("Noise Burst", "noise_burst", "Noise burst for risers and explosions, keyed so the pitch "
                 "(and length) follows the key.", keyed=1, rates=high, tags=["noise", "sfx", "keyed"]),
        dac_seed("Orchestra Hit", "sega_hit", "Keyed orchestra hit (C major stab of recorded brass, "
                 "strings and timpani), the stock early-90s DAC stab; play it chromatically for hit "
                 "melodies.", keyed=1, rates=low, tags=["hit", "keyed"]),
        dac_seed("Voice", "voice_uh", "Keyed formant voice 'uh' for grunts and chants; low rates give the "
                 "crunchy speech of Mega Drive titles.", keyed=1, rates=low, tags=["voice", "keyed"]),
    ]


# ----- PSG Lead ---------------------------------------------------------------------------------

def _psg_lead() -> list[Seed]:
    cat = "PSG Lead"
    vib_off = ("off", {"psg_vibrato_rate": 0, "psg_vibrato_depth": 0})
    vib_slow = ("slow", {"psg_vibrato_rate": 6, "psg_vibrato_depth": 2})
    level = [("loud", tone_level(0)), ("soft", tone_level(5))]

    def lead(att: int, attack: int, decay: int, sustain: int, release: int, unison: int) -> dict[str, int]:
        return psg(tone_att=att, tone3_att=att, noise_att=15, noise_mode=1, noise_rate=0, attack=attack,
                   decay=decay, sustain=sustain, release=release, vibrato_rate=0, vibrato_depth=0,
                   unison_detune=unison, transpose=0)

    return [
        psg_seed("Square Lead", cat, "Solo",
                 "Plain SN76489 square at full level (attenuation 0) with a 3-frame release; the vibrato axis adds "
                 "the driver's period vibrato, slow or fast, the usual partner of an FM bass line.",
                 block=lead(0, 0, 0, 15, 3, 0),
                 axes={"vibrato": [vib_off, vib_slow, ("fast", {"psg_vibrato_rate": 3, "psg_vibrato_depth": 3})],
                       "psg_transpose": OCT_UP},
                 tags=["lead", "square"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Soft Lead", cat, "Solo",
                 "Soft lead: the driver ramps the 2 dB attenuation steps in over 6 or 14 frames and releases over "
                 "12 frames; attenuation 2 leaves room for the FM section.",
                 block=lead(2, 8, 0, 15, 12, 0),
                 axes={"attack": [("medium", {"psg_sw_attack": 6}), ("slow", {"psg_sw_attack": 14})],
                       "vibrato": [vib_off, vib_slow]},
                 tags=["lead", "soft"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Pluck Lead", cat, "Solo",
                 "Plucked square: decays over 6-24 frames to sustain 5 (quantised 2 dB steps are audible, as on "
                 "the chip), for arpeggios and counter-melodies.",
                 block=lead(0, 0, 12, 5, 6, 0),
                 axes={"psg_sw_decay": [6, 12, 24]}, tags=["lead", "pluck"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Staccato Lead", cat, "Solo",
                 "Staccato blip: 4-frame decay to silence, no release; the level axis compares attenuation 0 "
                 "and 5 (10 dB) for lead or echo-line use.",
                 block=lead(0, 0, 4, 0, 0, 0),
                 axes={"level": level, "psg_transpose": OCT_UP}, tags=["lead", "staccato"],
                 global_params=PSG_TONE_GLOBALS),
        psg_seed("Bend Lead", cat, "Solo",
                 "Legato lead with a 150 ms glide (about 9 frames of period rewrites, so the slide is audibly "
                 "stepped) and an 8-frame release; the vibrato axis goes from none to a deep 7.5 Hz, 6-unit "
                 "vibrato for guitar-like bends. Overlap the notes to hear the glide.",
                 block=lead(0, 0, 0, 15, 8, 0),
                 axes={"vibrato": [vib_off, vib_slow, ("deep", {"psg_vibrato_rate": 4, "psg_vibrato_depth": 6})]},
                 tags=["lead", "glide"],
                 global_params=perf(PSG_TONE_CHANNELS, glide_ms=150.0, legato=True)),
        psg_seed("Twin Lead", cat, "Doubled",
                 "Two tone channels per note, the second detuned by 1 or 3 period units (about 8 or 24 cents at "
                 "C5): the classic PSG chorus that thickens a thin square (costs a channel per note).",
                 block=lead(0, 0, 0, 15, 4, 3),
                 axes={"psg_unison_detune": [1, 3]},
                 tags=["lead", "doubled"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Chorus Lead", cat, "Doubled",
                 "Doubled lead with a 2-frame fade-in and 8-frame release at attenuation 1; the vibrato on top of "
                 "the detuned pair (1 or 3 period units) gives a lush, moving tone.",
                 block=lead(1, 2, 0, 15, 8, 1),
                 axes={"psg_unison_detune": [1, 3], "vibrato": [vib_off, vib_slow]},
                 tags=["lead", "doubled", "chorus"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Detuned Pluck", cat, "Doubled",
                 "Detuned pair (1 or 3 period units) that decays to sustain 4 over 8 or 20 frames: a "
                 "harpsichord-like doubled pluck.",
                 block=lead(0, 0, 8, 4, 6, 1),
                 axes={"psg_unison_detune": [1, 3], "psg_sw_decay": [8, 20]},
                 tags=["lead", "doubled", "pluck"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Swell Lead", cat, "Doubled",
                 "Slow-swelling doubled lead (attack 10 or 20 frames, detune 1 or 3 period units) for held notes "
                 "over FM pads.",
                 block=lead(1, 10, 0, 15, 10, 1),
                 axes={"psg_unison_detune": [1, 3], "psg_sw_attack": [10, 20]},
                 tags=["lead", "doubled", "swell"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Fat Lead", cat, "Doubled",
                 "Widest doubling (detune 5 or 7 period units, 40-60 cents at C5, a fast rough beat) at full or "
                 "-10 dB level: the loudest PSG lead the chip can make. The partner is detuned downwards and the "
                 "PSG has no fine tune to centre the pair, so it measures 15-30 cents flat at C4-C5 and 40-55 at C6; "
                 "it is an effect, not an in-tune lead.",
                 block=lead(0, 0, 0, 15, 5, 5),
                 axes={"psg_unison_detune": [5, 7], "level": level},
                 tags=["lead", "doubled"], global_params=PSG_TONE_GLOBALS),
    ]


# ----- PSG Bass ---------------------------------------------------------------------------------

def _psg_bass() -> list[Seed]:
    cat = "PSG Bass"
    tone_levels = [("loud", tone_level(0)), ("soft", tone_level(4))]
    noise_levels = [("loud", {"psgn_att": 0}), ("soft", {"psgn_att": 4})]
    # Tone basses sound an octave above the key: the lowest tone is A2 (period 0x3FF), so keys from
    # A1 up stay inside the chip's range; "up" moves the whole line one more octave up.
    bass_range = [("", {"psg_transpose": 12}), ("up", {"psg_transpose": 24})]

    def tone_bass(att: int, attack: int, decay: int, sustain: int, release: int, unison: int) -> dict[str, int]:
        return psg(tone_att=att, tone3_att=att, noise_att=15, noise_mode=1, noise_rate=0, attack=attack,
                   decay=decay, sustain=sustain, release=release, vibrato_rate=0, vibrato_depth=0,
                   unison_detune=unison, transpose=12)

    def buzz_bass(att: int, attack: int, decay: int, sustain: int, release: int, transpose: int) -> dict[str, int]:
        # Periodic noise clocked by the silent tone 3: a 1/16-duty pulse at the key (module docstring).
        return psg(tone_att=15, tone3_att=15, noise_att=att, noise_mode=0, noise_rate=3, attack=attack,
                   decay=decay, sustain=sustain, release=release, vibrato_rate=0, vibrato_depth=0,
                   unison_detune=0, transpose=transpose)

    return [
        psg_seed("Square Bass", cat, "",
                 "Square bass on the tone channels an octave above the key (playable from A1): a 3-frame "
                 "accent that settles 12 dB lower (sustain 9) and a 2-frame release, the punchy PSG bass under "
                 "FM leads.",
                 block=tone_bass(0, 0, 3, 9, 2, 0),
                 axes={"level": tone_levels, "range": bass_range}, tags=["bass", "square"],
                 global_params=PSG_TONE_GLOBALS),
        psg_seed("Pluck Bass", cat, "",
                 "Plucked square bass (playable from A1): decays to sustain 4 (-22 dB) over 4, 8 or 16 frames.",
                 block=tone_bass(0, 0, 8, 4, 3, 0),
                 axes={"psg_sw_decay": [4, 8, 16]}, tags=["bass", "pluck"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Staccato Bass", cat, "",
                 "Very short bass notes for driving eighth-note lines (playable from A1): a 2-frame click or an "
                 "8-frame thump to silence, 1-frame release against clicks.",
                 block=tone_bass(0, 0, 2, 0, 1, 0),
                 axes={"psg_sw_decay": [2, 8], "range": bass_range}, tags=["bass", "staccato"],
                 global_params=PSG_TONE_GLOBALS),
        psg_seed("Buzz Bass", cat, "",
                 "The Mega Drive PSG-bass trick: periodic noise clocked by the silent tone 3 plays a thin "
                 "1/16-duty buzz at the key, far below the tone channels' A2 floor (any key from 6.8 Hz up); "
                 "noise level and release vary.",
                 block=buzz_bass(0, 0, 0, 15, 2, 0),
                 axes={"level": noise_levels,
                       "release": [("short", {"psg_sw_release": 2}), ("long", {"psg_sw_release": 10})]},
                 tags=["bass", "periodic", "buzz"], global_params=PSG_NOISE_GLOBALS),
        psg_seed("Periodic Pluck Bass", cat, "",
                 "Periodic-noise bass (tone 3 clock, at the key or an octave up) decaying to sustain 3 over 6 or "
                 "14 frames: a pluck deeper than any tone channel can play.",
                 block=buzz_bass(0, 0, 10, 3, 2, 0),
                 axes={"psg_sw_decay": [6, 14], "psg_transpose": OCT_UP}, tags=["bass", "periodic", "pluck"],
                 global_params=PSG_NOISE_GLOBALS),
        psg_seed("Wobble Bass", cat, "",
                 "Held square bass (playable from A1) with a deep driver period vibrato: slow (3.75 Hz, 6 period "
                 "units, about 15 cents at E3) or fast (10 Hz, 10 units), at two levels.",
                 block=tone_bass(0, 0, 0, 15, 3, 0),
                 axes={"vibrato": [("slow", {"psg_vibrato_rate": 8, "psg_vibrato_depth": 6}),
                                   ("fast", {"psg_vibrato_rate": 3, "psg_vibrato_depth": 10})],
                       "level": tone_levels},
                 tags=["bass", "vibrato"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Glide Bass", cat, "",
                 "Legato square bass (playable from A1) with a 120 ms stepped glide (one period write per frame) "
                 "and a gentle 8-frame dip to sustain 13 that accents each new note; short or long release.",
                 block=tone_bass(0, 0, 8, 13, 3, 0),
                 axes={"psg_sw_release": [2, 10]}, tags=["bass", "glide"],
                 global_params=perf(PSG_TONE_CHANNELS, glide_ms=120.0, legato=True)),
        psg_seed("Detuned Bass", cat, "",
                 "Doubled square bass (playable from A1): the second tone channel is detuned by 3 or 5 period "
                 "units (5-17 or 8-28 cents over the bass range, 1 unit would be inaudible), with either an "
                 "accent to sustain 10 or a 16-frame pluck to sustain 3.",
                 block=tone_bass(0, 0, 3, 10, 3, 3),
                 axes={"psg_unison_detune": [3, 5],
                       "decay": [("accent", {"psg_sw_decay": 3, "psg_sw_sustain": 10}),
                                 ("pluck", {"psg_sw_decay": 16, "psg_sw_sustain": 3})]},
                 tags=["bass", "doubled"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Soft Bass", cat, "",
                 "Rounder square bass (playable from A1): 3-frame attack ramp, 8-frame release, attenuation 2 "
                 "(4 dB) under an FM lead.",
                 block=tone_bass(2, 3, 0, 15, 8, 0),
                 axes={"range": bass_range}, tags=["bass", "soft"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Periodic Sub Bass", cat, "",
                 "Held periodic-noise sub bass one octave below the key (psg_transpose -12), with a 4-frame "
                 "fade-in and 8-frame release, full or -8 dB.",
                 block=buzz_bass(0, 4, 0, 15, 8, -12),
                 axes={"level": noise_levels}, tags=["bass", "periodic", "sub"], global_params=PSG_NOISE_GLOBALS),
    ]


# ----- PSG Drums (noise channel) ----------------------------------------------------------------

def _psg_drums() -> list[Seed]:
    cat = "PSG Drums"
    level = [("loud", {"psgn_att": 0}), ("soft", {"psgn_att": 4})]

    def noise(mode: int, rate: int, decay: int, vibrato_rate: int, vibrato_depth: int) -> dict[str, int]:
        return psg(tone_att=15, tone3_att=15, noise_att=0, noise_mode=mode, noise_rate=rate, attack=0,
                   decay=decay, sustain=0, release=0, vibrato_rate=vibrato_rate, vibrato_depth=vibrato_depth,
                   unison_detune=0, transpose=0)

    white, periodic = 1, 0
    return [
        psg_seed("Closed Hat", cat, "White",
                 "White noise at the fastest shift rate (clock/512) decaying to silence in 2 or 4 frames: the "
                 "standard PSG hi-hat under DAC kicks and snares.",
                 block=noise(white, 0, 3, 0, 0),
                 axes={"psg_sw_decay": [2, 4], "level": level}, tags=["drums", "hihat", "white"],
                 global_params=PSG_NOISE_GLOBALS),
        psg_seed("Open Hat", cat, "White",
                 "Open hi-hat: white noise /512 decaying over 10 or 16 frames (170-270 ms).",
                 block=noise(white, 0, 12, 0, 0),
                 axes={"psg_sw_decay": [10, 16], "level": level}, tags=["drums", "hihat", "white"],
                 global_params=PSG_NOISE_GLOBALS),
        psg_seed("Noise Snare", cat, "White",
                 "PSG snare: white noise at the /1024 or /2048 shift rate (darker) with a 6 or 12-frame decay.",
                 block=noise(white, 1, 8, 0, 0),
                 axes={"psg_sw_decay": [6, 12], "psg_noise_rate": [1, 2]}, tags=["drums", "snare", "white"],
                 global_params=PSG_NOISE_GLOBALS),
        psg_seed("Crash", cat, "White",
                 "PSG crash: white noise /512 with a 40 or 60-frame decay (0.7 to 1 second).",
                 block=noise(white, 0, 40, 0, 0),
                 axes={"psg_sw_decay": [40, 60], "level": level}, tags=["drums", "cymbal", "white"],
                 global_params=PSG_NOISE_GLOBALS),
        psg_seed("Pitched Noise", cat, "White",
                 "White noise clocked by the silent tone 3: the LFSR shifts once per tone-3 period, i.e. at the "
                 "(transposed) key frequency, so the key sets the colour. With psg_transpose +24, keys C5-C6 "
                 "shift at 2.1-4.2 kHz (a keyed snare, between the fixed /2048 and /512 rates) and C3-C4 at "
                 "0.5-1 kHz (a crunchy explosion); +12 puts the whole keyboard an octave darker. The LFSR "
                 "restarts from 0x8000 on every note and outputs nothing for its first 15 shifts, so very low "
                 "keys start late or stay silent within a short decay; 8 or 16-frame decay.",
                 block=psg(tone_att=15, tone3_att=15, noise_att=0, noise_mode=white, noise_rate=3, attack=0,
                           decay=10, sustain=0, release=0, vibrato_rate=0, vibrato_depth=0, unison_detune=0,
                           transpose=24),
                 axes={"psg_sw_decay": [8, 16], "psg_transpose": [24, 12]},
                 tags=["drums", "snare", "white", "keyed"], global_params=PSG_NOISE_GLOBALS),
        psg_seed("Kick", cat, "Periodic",
                 "PSG kick: periodic noise at /1024 or /2048 is a 218 or 109 Hz 1/16-duty pulse that dies in 6 or "
                 "12 frames; a thump for tracks without DAC drums.",
                 block=noise(periodic, 2, 8, 0, 0),
                 axes={"psg_sw_decay": [6, 12], "psg_noise_rate": [1, 2]}, tags=["drums", "kick", "periodic"],
                 global_params=PSG_NOISE_GLOBALS),
        psg_seed("Tom", cat, "Periodic",
                 "Keyed PSG tom: periodic noise clocked by tone 3 sounds at the key (C3-C4 for toms; the "
                 "octave-down variant puts them at C4-C5), 8 or 16-frame decay.",
                 block=noise(periodic, 3, 12, 0, 0),
                 axes={"psg_sw_decay": [8, 16], "psg_transpose": OCT_DOWN}, tags=["drums", "tom", "periodic"],
                 global_params=PSG_NOISE_GLOBALS),
        psg_seed("Metal Hit", cat, "Periodic",
                 "Metallic ping: periodic noise at /512 (437 Hz pulse train) with a 6 or 20-frame decay.",
                 block=noise(periodic, 0, 10, 0, 0),
                 axes={"psg_sw_decay": [6, 20], "level": level}, tags=["drums", "metallic", "periodic"],
                 global_params=PSG_NOISE_GLOBALS),
        psg_seed("Blip", cat, "Periodic",
                 "Very short periodic-noise blip (2 frames) for rim/click accents, either the fixed /512 pulse "
                 "(437 Hz) or keyed through tone 3 so that it follows the melody.",
                 block=noise(periodic, 0, 2, 0, 0),
                 axes={"psg_noise_rate": [0, 3], "level": level}, tags=["drums", "click", "periodic"],
                 global_params=PSG_NOISE_GLOBALS),
    ]


# ----- entry point ---------------------------------------------------------------------------

def get_seeds() -> list[Seed]:
    seeds = (_fm_bass() + _fm_keys() + _fm_brass() + _fm_pad() + _dac()
             + _psg_lead() + _psg_bass() + _psg_drums())
    keys = set(ParamTable.load("genesis").keys())
    for seed in seeds:
        missing = sorted(keys - set(seed.params))
        unknown = sorted(set(seed.params) - keys)
        if missing or unknown:
            raise PresetError(f"genesis seed {seed.name!r}: missing {missing}, unknown {unknown}")
    return seeds
