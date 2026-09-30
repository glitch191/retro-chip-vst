"""Genesis (YM2612 + SN76489) seed patches.

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
* Noise rate 3 note mapping (driver decision these seeds rely on, not yet stated in
  docs/ENGINE_SPECS.md): a note on the noise channel writes tone 3's period for note + 48
  semitones (16x the key frequency), as games did for this trick. Periodic noise (1 pulse
  in 16 shifts) then sounds at the played key, from 6.8 Hz (period 0x3FF) to 6991 Hz
  (period 1), and white noise shifts at 16x the key frequency (C4 -> 4186 Hz).
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


# ----- FM Bass (algorithms 0-4) ------------------------------------------------------------------

def _fm_bass() -> list[Seed]:
    cat = "FM Bass"
    return [
        fm_seed("Slap Bass", cat, "Alg",
                "Slap: S1 feedback 6 plus a MUL 2 S3 with DR 18 gives the thumb pop that dies in ~100 ms over a "
                "plain S4 carrier; algorithms 0-2 change how much of the pop reaches S4.",
                sys=MD1, patch=voice(algorithm=0, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(30, 31, 14, 6, 8, 4, 1, 0, 1, 0, 0),
                      (40, 31, 12, 4, 8, 3, 1, 3, 1, 0, 0),
                      (26, 31, 18, 8, 8, 6, 2, 0, 1, 0, 0),
                      (0, 31, 5, 3, 8, 2, 1, 0, 1, 0, 0)],
                axes={"algorithm": [0, 1, 2], "transpose": OCT_DOWN}, tags=["bass", "slap"]),
        fm_seed("Finger Bass", cat, "Alg",
                "Round finger bass: low feedback 3 and modulators at TL 32-40 keep only a few harmonics; slow "
                "SR on the S4 carrier lets the note ring like a plucked string.",
                sys=MD1, patch=voice(algorithm=2, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(34, 31, 8, 2, 7, 3, 1, 0, 0, 0, 0),
                      (40, 31, 10, 3, 7, 4, 2, 0, 0, 0, 0),
                      (32, 31, 6, 2, 7, 3, 1, 7, 0, 0, 0),
                      (2, 31, 5, 3, 7, 3, 1, 0, 1, 0, 0)],
                axes={"algorithm": [1, 2, 3],
                      "release": [("short", all_ops("rr", 10)), ("long", all_ops("rr", 4))]},
                tags=["bass", "finger"]),
        fm_seed("Dual Stack Bass", cat, "Alg",
                "Two 2-op stacks (alg 4) with slightly detuned carriers S2/S4; alg 3 turns S2 into a strong "
                "modulator of S4 for a gritty variant. The decay axis sets the carriers' DR/SL/SR.",
                sys=MD1, patch=voice(algorithm=4, feedback=4, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(26, 31, 9, 3, 8, 3, 1, 0, 1, 0, 0),
                      (10, 31, 6, 3, 8, 4, 1, 1, 1, 0, 0),
                      (30, 31, 10, 4, 8, 4, 2, 0, 1, 0, 0),
                      (4, 31, 6, 3, 8, 4, 1, 5, 1, 0, 0)],
                axes={"algorithm": [3, 4],
                      "decay": [("pluck", {**ops("dr", {2: 12, 4: 12}), **ops("sl", {2: 8, 4: 8}),
                                           **ops("sr", {2: 6, 4: 6})}),
                                ("medium", {**ops("dr", {2: 6, 4: 6}), **ops("sl", {2: 4, 4: 4}),
                                            **ops("sr", {2: 3, 4: 3})}),
                                ("hold", {**ops("dr", {2: 2, 4: 2}), **ops("sl", {2: 1, 4: 1}),
                                          **ops("sr", {2: 0, 4: 0})})]},
                tags=["bass", "stack"]),
        fm_seed("Round Bass", cat, "Alg",
                "Dark sub-heavy bass: S1 at MUL 0 (x0.5) with feedback 1 and modulators at TL 34-50 add only "
                "the lowest sidebands; alg 4 adds the quiet S2 as a second, softer carrier.",
                sys=MD1, patch=voice(algorithm=0, feedback=1, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=16, pan=1),
                rows=[(45, 31, 4, 1, 6, 2, 0, 0, 0, 0, 0),
                      (50, 31, 4, 1, 6, 2, 1, 0, 0, 0, 0),
                      (34, 31, 6, 2, 6, 3, 1, 0, 0, 0, 0),
                      (0, 31, 3, 1, 6, 2, 1, 0, 0, 0, 0)],
                axes={"algorithm": [0, 2, 4]}, tags=["bass", "sub"]),
        fm_seed("Growl Bass", cat, "Feedback",
                "Serial chain (alg 0) driven by S1 feedback: 4 is buzzy, 7 is the noisy growl of Thunder Force "
                "style basses; everything else stays fixed so the axis isolates the feedback.",
                sys=MD1, patch=voice(algorithm=0, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(22, 31, 6, 2, 8, 2, 1, 0, 1, 0, 0),
                      (36, 31, 8, 3, 8, 3, 1, 0, 1, 0, 0),
                      (30, 31, 10, 3, 8, 4, 1, 3, 1, 0, 0),
                      (0, 31, 4, 2, 8, 2, 1, 0, 1, 0, 0)],
                axes={"feedback": [4, 5, 6, 7]}, tags=["bass", "growl"]),
        fm_seed("Pick Bass", cat, "Feedback",
                "Picked bass (alg 1): S2 at MUL 3 decays fast (DR 20) for the pick click, S1 feedback sets the "
                "grit; velocity only moves the S4 carrier level.",
                sys=MD1, patch=voice(algorithm=1, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(28, 31, 12, 4, 9, 4, 1, 0, 1, 0, 0),
                      (36, 31, 20, 6, 9, 8, 3, 0, 1, 0, 0),
                      (30, 31, 10, 4, 9, 4, 1, 0, 1, 0, 0),
                      (0, 31, 7, 4, 9, 3, 1, 0, 1, 0, 0)],
                axes={"feedback": [3, 5, 7], "velocity": VELOCITY}, tags=["bass", "pick"]),
        fm_seed("Saw Bass", cat, "Feedback",
                "Sustained saw-like bass: high S1 feedback approximates a sawtooth, the tone axis moves the S2/S3 "
                "modulator TLs (brighter = lower TL) and slow DR keeps it held.",
                sys=MD1, patch=voice(algorithm=0, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(20, 31, 2, 1, 8, 1, 1, 0, 0, 0, 0),
                      (30, 31, 4, 2, 8, 2, 1, 0, 0, 0, 0),
                      (30, 31, 6, 2, 8, 3, 1, 0, 0, 0, 0),
                      (0, 31, 2, 1, 8, 1, 1, 0, 0, 0, 0)],
                axes={"feedback": [5, 6, 7],
                      "tone": [("dark", ops("tl", {2: 38, 3: 40})), ("bright", ops("tl", {2: 26, 3: 22}))]},
                tags=["bass", "saw"]),
        fm_seed("Hollow Bass", cat, "Feedback",
                "Square-ish bass: the S3 (MUL 2) -> S4 (MUL 1) stack gives odd harmonics, while S1 feedback on "
                "the S1 -> S2 stack adds even ones as it rises, from hollow (0) to reedy (6).",
                sys=MD1, patch=voice(algorithm=4, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(32, 31, 8, 2, 8, 3, 1, 0, 0, 0, 0),
                      (14, 31, 5, 2, 8, 3, 1, 0, 0, 0, 0),
                      (28, 31, 7, 2, 8, 3, 2, 0, 0, 0, 0),
                      (2, 31, 5, 2, 8, 3, 1, 0, 0, 0, 0)],
                axes={"feedback": [0, 3, 6], "transpose": OCT_DOWN}, tags=["bass", "square"]),
        fm_seed("Chorus Bass", cat, "Detune",
                "Two equal 2-op stacks (alg 4) with the carriers S2/S4 at DT 3/7. DT is a fixed offset in Hz "
                "(kDetuneTable, 0.05 Hz per unit): at bass key codes it is only 0.2-0.5 Hz of beating between "
                "the carriers, a slow swell ('light'). 'wide' adds driver unison 12 fnum units (about 1.4-2.7 "
                "cents per unit, so 17-32 cents), which beats on every harmonic; it costs two FM channels per "
                "note (3-note polyphony).",
                sys=MD1, patch=voice(algorithm=4, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(30, 31, 8, 3, 8, 3, 1, 0, 1, 0, 0),
                      (8, 31, 6, 3, 8, 3, 1, 3, 1, 0, 0),
                      (32, 31, 8, 3, 8, 3, 1, 0, 1, 0, 0),
                      (8, 31, 6, 3, 8, 3, 1, 7, 1, 0, 0)],
                axes={"detune": [("light", {"unison_detune": 0}), ("wide", {"unison_detune": 12})],
                      "transpose": OCT_DOWN},
                tags=["bass", "chorus"]),
        fm_seed("Metal Bass", cat, "Detune",
                "Clangy bass (alg 4). Integer MULs always give a harmonic spectrum on this chip, so the metal "
                "colour comes from the S1 -> S2 stack: a MUL 7 or 11 modulator against a MUL 2 carrier is a "
                "3.5 or 5.5 ratio, sparse high partials with the low ones missing, whose carrier falls 24 dB in "
                "about 0.6 s (DR 12, SL 8) over the plain S3 (MUL 5) -> S4 (MUL 1) body. S1 has DT 3: its "
                "0.1-0.25 Hz offset at bass key codes (blocks 1-2) is multiplied by MUL into a 0.7-2.8 Hz "
                "roughness of the sidebands. Feedback 2 or 6 on S1 adds grit to the clang.",
                sys=MD1, patch=voice(algorithm=4, feedback=2, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=20, pan=1),
                rows=[(30, 31, 14, 4, 8, 6, 7, 3, 1, 0, 0),
                      (12, 31, 12, 4, 8, 8, 2, 0, 1, 0, 0),
                      (36, 31, 8, 3, 8, 4, 5, 0, 1, 0, 0),
                      (4, 31, 5, 2, 8, 2, 1, 0, 1, 0, 0)],
                axes={"op1_mul": [7, 11], "feedback": [2, 6]}, tags=["bass", "metallic"]),
        fm_seed("Fat Bass", cat, "Detune",
                "Driver unison: each note takes two FM channels (3-note polyphony), the second offset by 8 or 15 "
                "fnum units, i.e. 11-22 or 21-40 cents across an octave of fnum values. That is a 0.4-3 Hz beat "
                "on the fundamental of an E1-C3 note and n times faster on the n-th harmonic; offsets of 2-4 "
                "units are inaudible at bass pitches. The alg 2 patch is plain so the doubling is what you hear.",
                sys=MD1, patch=voice(algorithm=2, feedback=4, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=8, velocity_depth=20, pan=1),
                rows=[(30, 31, 6, 2, 8, 3, 1, 0, 0, 0, 0),
                      (40, 31, 8, 2, 8, 3, 1, 0, 0, 0, 0),
                      (32, 31, 6, 2, 8, 3, 1, 0, 0, 0, 0),
                      (2, 31, 4, 2, 8, 2, 1, 0, 0, 0, 0)],
                axes={"unison_detune": [8, 15], "transpose": OCT_DOWN}, tags=["bass", "unison"]),
        fm_seed("Sub Bass", cat, "Detune",
                "Octave-layered bass (alg 4): the S4 carrier at MUL 0 sounds an octave under the S2 carrier. "
                "The carriers' DT 3/7 is only a few tenths of a Hz at bass key codes, so 'single' is a clean "
                "octave; 'unison' doubles the note on a second FM channel 10 fnum units up (14-27 cents), which "
                "thickens the low end at the cost of half the polyphony.",
                sys=MD1, patch=voice(algorithm=4, feedback=2, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(36, 31, 6, 2, 8, 3, 1, 0, 0, 0, 0),
                      (10, 31, 4, 2, 8, 3, 1, 3, 0, 0, 0),
                      (40, 31, 6, 2, 8, 3, 1, 0, 0, 0, 0),
                      (4, 31, 3, 1, 8, 2, 0, 7, 0, 0, 0)],
                axes={"layer": [("single", {"unison_detune": 0}), ("unison", {"unison_detune": 10})],
                      "velocity": VELOCITY},
                tags=["bass", "sub"]),
    ]


# ----- FM Keys (algorithms 4-7) --------------------------------------------------------------

def _fm_keys() -> list[Seed]:
    cat = "FM Keys"
    return [
        fm_seed("Tine Piano", cat, "EPiano",
                "Classic FM electric piano (alg 4): S1 at MUL 14 decays to silence in ~200 ms (DR 20, SL 15) "
                "for the tine, S3 -> S4 is the body; the tine axis moves S1's TL.",
                sys=MD1, patch=voice(algorithm=4, feedback=2, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=36, pan=1),
                rows=[(40, 31, 20, 0, 8, 15, 14, 0, 2, 0, 0),
                      (12, 31, 10, 4, 6, 6, 1, 3, 1, 0, 0),
                      (36, 31, 8, 2, 6, 4, 1, 0, 1, 0, 0),
                      (4, 31, 6, 3, 6, 4, 1, 7, 1, 0, 0)],
                axes={"algorithm": [4, 5],
                      "tine": [("soft", {"op1_tl": 52}), ("medium", {"op1_tl": 40}), ("bright", {"op1_tl": 28})]},
                tags=["keys", "epiano"]),
        fm_seed("Soft EPiano", cat, "EPiano",
                "Mellow EP (alg 5): one S1 modulator feeds three carriers at MUL 1, 2 and 1 detuned; carriers "
                "have AM on so the tremolo axis (LFO + AMS) works on level only.",
                sys=MD1, patch=voice(algorithm=5, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=32, pan=1),
                rows=[(34, 31, 9, 2, 6, 4, 1, 0, 1, 0, 0),
                      (8, 31, 7, 3, 6, 4, 1, 0, 1, 1, 0),
                      (20, 31, 9, 3, 6, 5, 2, 3, 1, 1, 0),
                      (6, 31, 7, 3, 6, 4, 1, 7, 1, 1, 0)],
                axes={"tremolo": [("off", {"lfo_enable": 0, "lfo_freq": 0, "ams": 0}),
                                  ("slow", {"lfo_enable": 1, "lfo_freq": 1, "ams": 1}),
                                  ("deep", {"lfo_enable": 1, "lfo_freq": 3, "ams": 2})],
                      "velocity": VELOCITY},
                tags=["keys", "epiano"]),
        fm_seed("Clav", cat, "EPiano",
                "Funky clavinet (alg 4): modulators at MUL 3 and 5 against carriers at MUL 1 and 2 give the "
                "nasal bark, short DR/RR 10 give the damped pluck, feedback adds buzz.",
                sys=MD1, patch=voice(algorithm=4, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=30, pan=1),
                rows=[(30, 31, 18, 6, 10, 8, 3, 0, 2, 0, 0),
                      (8, 31, 12, 8, 10, 8, 1, 0, 2, 0, 0),
                      (38, 31, 20, 6, 10, 9, 5, 0, 2, 0, 0),
                      (12, 31, 12, 8, 10, 8, 2, 0, 2, 0, 0)],
                axes={"feedback": [3, 5, 7], "velocity": VELOCITY}, tags=["keys", "clav"]),
        fm_seed("Wurli", cat, "EPiano",
                "Reedy EP (alg 4): S3 at MUL 7 with DT 3 and a fast decay gives the bark on the S4 body, S1 -> S2 "
                "is a warm second stack; carriers have AM on for the optional tremolo.",
                sys=MD1, patch=voice(algorithm=4, feedback=4, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=36, pan=1),
                rows=[(30, 31, 10, 3, 6, 5, 1, 0, 1, 0, 0),
                      (10, 31, 8, 3, 6, 5, 1, 0, 1, 1, 0),
                      (44, 31, 16, 4, 6, 8, 7, 3, 1, 0, 0),
                      (6, 31, 7, 3, 6, 4, 1, 0, 1, 1, 0)],
                axes={"algorithm": [4, 5],
                      "tremolo": [("off", {"lfo_enable": 0, "lfo_freq": 0, "ams": 0}),
                                  ("slow", {"lfo_enable": 1, "lfo_freq": 2, "ams": 1})]},
                tags=["keys", "epiano"]),
        fm_seed("Toy Piano", cat, "EPiano",
                "Toy piano (alg 5): S1 at MUL 4 over carriers at MUL 2, 5 and 1 gives a slightly inharmonic, "
                "bright plink; RS 2 shortens the high notes like a real toy piano.",
                sys=MD1, patch=voice(algorithm=5, feedback=2, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=28, pan=1),
                rows=[(36, 31, 18, 6, 8, 10, 4, 0, 2, 0, 0),
                      (10, 31, 12, 6, 8, 10, 2, 0, 2, 0, 0),
                      (24, 31, 14, 6, 8, 10, 5, 2, 2, 0, 0),
                      (8, 31, 10, 5, 8, 9, 1, 0, 2, 0, 0)],
                axes={"transpose": OCT_UP, "velocity": VELOCITY}, tags=["keys", "toy"]),
        fm_seed("Stage EPiano", cat, "EPiano",
                "Punchy stage EP (alg 4, feedback 5) with MUL 3 on S3 for bite; the chip axis compares two real "
                "consoles: a Model 1 (discrete YM2612 with the ladder effect, which lifts quiet tails, plus the "
                "3.39 kHz output low-pass) and a later Model 2 (YM3438 ASIC, linear DAC; its own second-order "
                "filter is not modelled, see docs/research/genesis.md Ambiguity 16, so no low-pass).",
                sys=MD1, patch=voice(algorithm=4, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=40, pan=1),
                rows=[(26, 31, 8, 3, 7, 4, 1, 0, 2, 0, 0),
                      (10, 31, 7, 3, 7, 4, 1, 2, 1, 0, 0),
                      (34, 31, 12, 4, 7, 6, 3, 0, 2, 0, 0),
                      (4, 31, 6, 3, 7, 4, 1, 6, 1, 0, 0)],
                axes={"algorithm": [4, 5],
                      "chip": [("YM2612", {"chip_revision": 0, "model1_lowpass": 1}),
                               ("YM3438", {"chip_revision": 1, "model1_lowpass": 0})]},
                tags=["keys", "epiano"]),
        fm_seed("Glock", cat, "Bell",
                "Glockenspiel (alg 7, four sines): partials at MUL 2, 7, 5 and 11 with DT spread give ratios "
                "1 : 3.5 : 2.5 : 5.5; each decays to silence (SL 15), higher ones faster.",
                sys=MD1, patch=voice(algorithm=7, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=32, pan=1),
                rows=[(10, 31, 8, 0, 5, 15, 2, 0, 2, 0, 0),
                      (22, 31, 12, 0, 5, 15, 7, 3, 2, 0, 0),
                      (26, 31, 10, 0, 5, 15, 5, 6, 2, 0, 0),
                      (30, 31, 14, 0, 5, 15, 11, 1, 2, 0, 0)],
                axes={"transpose": OCT_UP,
                      "decay": [("short", ops("dr", {1: 14, 2: 18, 3: 16, 4: 20})),
                                ("long", ops("dr", {1: 6, 2: 9, 3: 8, 4: 11}))]},
                tags=["bell", "glock"]),
        fm_seed("Tubular Bell", cat, "Bell",
                "Tubular bell (alg 5): S1 at MUL 7 modulates carriers at MUL 2, 2 (DT 3) and 4 (DT 7), the "
                "7 : 2 = 3.5 ratio gives the inharmonic strike; the strike axis moves S1's TL.",
                sys=MD1, patch=voice(algorithm=5, feedback=0, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=30, pan=1),
                rows=[(30, 31, 6, 0, 4, 15, 7, 0, 1, 0, 0),
                      (6, 31, 5, 0, 4, 15, 2, 0, 1, 0, 0),
                      (10, 31, 6, 0, 4, 15, 2, 3, 1, 0, 0),
                      (16, 31, 8, 0, 4, 15, 4, 7, 1, 0, 0)],
                axes={"algorithm": [5, 6],
                      "strike": [("soft", {"op1_tl": 40}), ("hard", {"op1_tl": 22})]},
                tags=["bell", "tubular"]),
        fm_seed("Chime", cat, "Bell",
                "Wind chime (alg 4): stacks at 7 : 2 (3.5) and 11 : 4 (2.75) with opposite DT; alg 6 frees S3 "
                "and alg 7 frees all four operators as partials for a glassier ring.",
                sys=MD1, patch=voice(algorithm=4, feedback=1, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=32, pan=1),
                rows=[(28, 31, 9, 0, 5, 15, 7, 0, 2, 0, 0),
                      (8, 31, 7, 0, 5, 15, 2, 0, 2, 0, 0),
                      (34, 31, 11, 0, 5, 15, 11, 2, 2, 0, 0),
                      (14, 31, 8, 0, 5, 15, 4, 6, 2, 0, 0)],
                axes={"algorithm": [4, 6, 7]}, tags=["bell", "chime"]),
        fm_seed("Vibraphone", cat, "Bell",
                "Vibraphone (alg 6, the Sega manual's suggestion): S1 MUL 4 -> S2 strike plus sine carriers at "
                "MUL 1 and 4; carriers have AM on and the LFO rate/AMS depth play the motor tremolo. Deliberate "
                "exception to the non-integer bell ratios: vibraphone bars are tuned so that the first "
                "overtone is two octaves up (1 : 4), and the S3 DT 3 pair gives the only slight beating.",
                sys=md1_lfo(3), patch=voice(algorithm=6, feedback=0, ams=1, fms=0, transpose=0, fine_tune=0,
                                            vibrato=(0, 0, 0), unison_detune=0, velocity_depth=32, pan=1),
                rows=[(38, 31, 12, 0, 5, 15, 4, 0, 1, 0, 0),
                      (8, 31, 6, 0, 5, 15, 1, 0, 1, 1, 0),
                      (14, 31, 7, 0, 5, 15, 1, 3, 1, 1, 0),
                      (26, 31, 9, 0, 5, 15, 4, 0, 1, 1, 0)],
                axes={"lfo_freq": [0, 3, 5], "ams": [1, 2]}, tags=["bell", "vibraphone", "tremolo"]),
        fm_seed("Music Box", cat, "Bell",
                "Music box (alg 7): partials at MUL 4, 9, 4 and 13 with S2/S3 detune pairs; RS 2 and SL 15 make "
                "short, bright tines; the detune axis sets how out of tune the comb sounds.",
                sys=MD1, patch=voice(algorithm=7, feedback=2, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=28, pan=1),
                rows=[(12, 31, 10, 0, 6, 15, 4, 0, 2, 0, 0),
                      (28, 31, 14, 0, 6, 15, 9, 1, 2, 0, 0),
                      (14, 31, 11, 0, 6, 15, 4, 5, 2, 0, 0),
                      (34, 31, 16, 0, 6, 15, 13, 0, 2, 0, 0)],
                axes={"transpose": OCT_UP,
                      "detune": [("light", ops("dt", {2: 1, 3: 5})), ("wide", ops("dt", {2: 3, 3: 7}))]},
                tags=["bell", "musicbox"]),
        fm_seed("Temple Bell", cat, "Bell",
                "Low gong-like bell (alg 5): S1 at MUL 7 against a MUL 0 (x0.5) carrier is a 14 : 1 ratio, with "
                "MUL 2 and 3 carriers detuned; feedback roughens the strike, slow DR lets it hang.",
                sys=MD1, patch=voice(algorithm=5, feedback=4, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(32, 31, 4, 0, 3, 15, 7, 0, 0, 0, 0),
                      (8, 31, 4, 0, 3, 15, 0, 0, 0, 0, 0),
                      (14, 31, 5, 0, 3, 15, 2, 3, 0, 0, 0),
                      (18, 31, 6, 0, 3, 15, 3, 5, 0, 0, 0)],
                axes={"feedback": [2, 5],
                      "decay": [("medium", ops("dr", {1: 6, 2: 6, 3: 7, 4: 8})),
                                ("long", ops("dr", {1: 3, 2: 3, 3: 4, 4: 5}))]},
                tags=["bell", "gong"]),
    ]


# ----- FM Brass (algorithms 2-5, feedback 3-7) ---------------------------------------------------

def _fm_brass() -> list[Seed]:
    cat = "FM Brass"
    vibrato_axis = [
        ("off", {"vibrato_rate": 0, "vibrato_depth": 0, "vibrato_delay": 0, "lfo_enable": 0, "lfo_freq": 0, "fms": 0}),
        ("delayed", {"vibrato_rate": 5, "vibrato_depth": 6, "vibrato_delay": 20, "lfo_enable": 0, "lfo_freq": 0,
                     "fms": 0}),
        ("LFO", {"vibrato_rate": 0, "vibrato_depth": 0, "vibrato_delay": 0, "lfo_enable": 1, "lfo_freq": 3, "fms": 3}),
    ]
    return [
        fm_seed("Brass Stab", cat, "Stab",
                "Short brass hit (alg 2): S1 feedback saw into S4 plus the S2 -> S3 chain. The S4 carrier "
                "decays by itself: DR 16 with RS 1 is rate 36 at C4, so it falls 36 dB (SL 12) in about 170 ms "
                "and SR 10 then fades the rest, a held chord ends like a sampled stab.",
                sys=MD1, patch=voice(algorithm=2, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(24, 31, 10, 4, 9, 4, 1, 0, 1, 0, 0),
                      (40, 31, 8, 3, 9, 4, 1, 0, 1, 0, 0),
                      (28, 31, 9, 4, 9, 3, 1, 0, 1, 0, 0),
                      (2, 31, 16, 10, 9, 12, 1, 0, 1, 0, 0)],
                axes={"feedback": [3, 5, 7]}, tags=["brass", "stab"]),
        fm_seed("Orch Stab", cat, "Stab",
                "Orchestra-hit stab (alg 4): two stacks an octave apart (carriers MUL 1 and 2, DT 2/6), feedback "
                "6. S2, S3 (a carrier in alg 5) and S4 decay by themselves (DR 16, SL 12, SR 10: -36 dB in "
                "about 170 ms at C4); the unison axis doubles each note on a second channel 6 fnum units up.",
                sys=MD1, patch=voice(algorithm=4, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(22, 31, 9, 4, 9, 4, 1, 0, 1, 0, 0),
                      (6, 31, 16, 10, 9, 12, 1, 2, 1, 0, 0),
                      (26, 31, 16, 10, 9, 12, 1, 0, 1, 0, 0),
                      (10, 31, 16, 10, 9, 12, 2, 6, 1, 0, 0)],
                axes={"algorithm": [4, 5], "unison": [("off", {"unison_detune": 0}), ("wide", {"unison_detune": 6})]},
                tags=["brass", "stab", "orchestra"]),
        fm_seed("Funk Stab", cat, "Stab",
                "Tight funk horn blip (alg 3): S1 -> S2 (MUL 2) and S3 (DT 3) both into S4. The S4 carrier "
                "(DR 18, rate 40 at C4) falls 42 dB (SL 14) in about 100 ms and SR 12 fades the rest, so every "
                "note is a short blip; feedback sets the rasp, velocity the carrier level.",
                sys=MD1, patch=voice(algorithm=3, feedback=4, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(26, 31, 14, 6, 10, 6, 1, 0, 1, 0, 0),
                      (34, 31, 12, 5, 10, 5, 2, 0, 1, 0, 0),
                      (30, 31, 12, 5, 10, 5, 1, 3, 1, 0, 0),
                      (0, 31, 18, 12, 10, 14, 1, 0, 1, 0, 0)],
                axes={"feedback": [3, 5, 7], "velocity": VELOCITY}, tags=["brass", "stab", "funk"]),
        fm_seed("Power Stab", cat, "Stab",
                "Wide power stab (alg 5): S1 with maximum feedback drives carriers at MUL 1, 2 and 3 (detuned), "
                "an organ-brass hybrid that fills a chord from one note. The carriers decay a little slower "
                "than the other stabs (DR 14, rate 32 at C4: -36 dB at SL 12 in about 350 ms, then SR 8).",
                sys=MD1, patch=voice(algorithm=5, feedback=7, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(20, 31, 8, 3, 9, 3, 1, 0, 1, 0, 0),
                      (4, 31, 14, 8, 9, 12, 1, 0, 1, 0, 0),
                      (12, 31, 14, 8, 9, 12, 2, 3, 1, 0, 0),
                      (20, 31, 14, 8, 9, 12, 3, 7, 1, 0, 0)],
                axes={"feedback": [5, 7], "transpose": OCT_DOWN}, tags=["brass", "stab"]),
        fm_seed("Trumpet", cat, "Brass",
                "Solo trumpet (alg 2): modulators attack slower (AR 20-22) than the S4 carrier (AR 28), so the "
                "tone brightens after the onset like a real blat; vibrato by driver or hardware LFO.",
                sys=MD1, patch=voice(algorithm=2, feedback=4, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=28, pan=1),
                rows=[(26, 20, 4, 1, 7, 2, 1, 0, 1, 0, 0),
                      (42, 22, 5, 1, 7, 2, 1, 0, 1, 0, 0),
                      (30, 22, 5, 1, 7, 2, 1, 0, 1, 0, 0),
                      (4, 28, 3, 1, 7, 1, 1, 0, 1, 0, 0)],
                axes={"vibrato": vibrato_axis, "feedback": [3, 5]}, tags=["brass", "trumpet"]),
        fm_seed("Horn Section", cat, "Brass",
                "Horn section (alg 4): two identical stacks with carriers detuned DT 2/6 read as two players; "
                "the attack axis switches between instant and a swelled AR 14-16 entry.",
                sys=MD1, patch=voice(algorithm=4, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(30, 31, 5, 1, 7, 2, 1, 0, 1, 0, 0),
                      (8, 31, 4, 1, 7, 2, 1, 2, 1, 0, 0),
                      (30, 31, 5, 1, 7, 2, 1, 0, 1, 0, 0),
                      (8, 31, 4, 1, 7, 2, 1, 6, 1, 0, 0)],
                axes={"attack": [("fast", all_ops("ar", 31)), ("swell", ops("ar", {1: 14, 2: 16, 3: 14, 4: 16}))],
                      "algorithm": [4, 5]},
                tags=["brass", "section"]),
        fm_seed("Synth Brass", cat, "Brass",
                "80s synth brass (alg 3, feedback 6): S2 and S3 into S4 with opposite DT for movement; a slow "
                "modulator attack (AR 16) gives the filter-sweep-like opening.",
                sys=MD1, patch=voice(algorithm=3, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(22, 31, 6, 2, 8, 2, 1, 0, 1, 0, 0),
                      (34, 31, 7, 2, 8, 3, 1, 1, 1, 0, 0),
                      (28, 31, 6, 2, 8, 3, 1, 5, 1, 0, 0),
                      (2, 31, 5, 2, 8, 2, 1, 0, 1, 0, 0)],
                axes={"detune": [("light", ops("dt", {2: 1, 3: 5})), ("wide", ops("dt", {2: 3, 3: 7}))],
                      "attack": [("fast", ops("ar", {1: 31, 3: 31})), ("slow", ops("ar", {1: 16, 3: 16}))]},
                tags=["brass", "synth"]),
        fm_seed("Tuba", cat, "Brass",
                "Low tuba/trombone (alg 2) an octave down: blown onsets, modulators AR 14-16 (about 50-105 ms "
                "at C3, the tone opens after the note starts) and the S4 carrier AR 18 (about 25 ms), with "
                "moderate feedback; the release axis sets how long the notes hang (RR).",
                sys=MD1, patch=voice(algorithm=2, feedback=5, ams=0, fms=0, transpose=-12, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(28, 14, 5, 1, 7, 2, 1, 0, 0, 0, 0),
                      (44, 16, 5, 1, 7, 2, 1, 0, 0, 0, 0),
                      (32, 16, 5, 1, 7, 2, 1, 0, 0, 0, 0),
                      (2, 18, 4, 1, 7, 2, 1, 0, 0, 0, 0)],
                axes={"feedback": [3, 7], "release": [("short", all_ops("rr", 10)), ("long", all_ops("rr", 5))]},
                tags=["brass", "tuba"]),
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
                      (2, 31, 5, 2, 8, 2, 1, 0, 1, 0, 0)],
                axes={"SSG": [("saw", {"op1_ssg": 1}), ("triangle", {"op1_ssg": 3})],
                      "feedback": [4, 6]},
                tags=["brass", "ssg"]),
        fm_seed("Growl Horn", cat, "SSG",
                "SSG-EG on S3 (DR 18, SL 15, RS 1: one ramp is 29 ms at C4), the modulator of the S4 carrier "
                "(alg 4) or a carrier itself (alg 5). Saw: level ramps down and restarts at 35 Hz; triangle: "
                "down and up at 17 Hz; inverted saw: ramps up from -48 dB and restarts at 35 Hz, so S3 is loud "
                "at the end of each ramp instead of the start. Three growl rhythms at audio-adjacent rates.",
                sys=MD1, patch=voice(algorithm=4, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(28, 31, 6, 2, 8, 3, 1, 0, 1, 0, 0),
                      (8, 31, 5, 2, 8, 3, 1, 0, 1, 0, 0),
                      (24, 31, 18, 0, 8, 15, 1, 0, 1, 0, 1),
                      (4, 31, 5, 2, 8, 3, 1, 0, 1, 0, 0)],
                axes={"SSG": [("saw", {"op3_ssg": 1}), ("triangle", {"op3_ssg": 3}), ("InvSaw", {"op3_ssg": 5})],
                      "algorithm": [4, 5]},
                tags=["brass", "ssg", "growl"]),
        fm_seed("Sync Horn", cat, "SSG",
                "Sync-like horn (alg 3): S2 at MUL 2 runs a repeating SSG saw (SL 15, RS 1). Each restart also "
                "resets S2's phase counter, so the S2 -> S4 modulation retriggers like oscillator sync. The "
                "sweep axis sets the ramp with DR: 12 = 230 ms (4 Hz pulsing), 18 = 29 ms (35 Hz growl), "
                "26 = 1.8 ms (a 550 Hz restart, the sync buzz), all at C4.",
                sys=MD1, patch=voice(algorithm=3, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(26, 31, 6, 2, 8, 3, 1, 0, 1, 0, 0),
                      (30, 31, 20, 0, 8, 15, 2, 0, 1, 0, 1),
                      (32, 31, 6, 2, 8, 3, 1, 0, 1, 0, 0),
                      (2, 31, 5, 2, 8, 2, 1, 0, 1, 0, 0)],
                axes={"sweep": [("slow", {"op2_dr": 12}), ("medium", {"op2_dr": 18}), ("fast", {"op2_dr": 26})],
                      "feedback": [3, 6]},
                tags=["brass", "ssg", "sync"]),
        fm_seed("Swell Horn", cat, "SSG",
                "One-shot SSG shapes on the common modulator S1 (alg 5; DR 10, SL 15, RS 1: one 460 ms ramp at "
                "C4). 'once' fades the modulation out over 460 ms and then holds S1 silent (bright onset, pure "
                "tail); 'swell' starts at -48 dB, rises over 460 ms and holds at TL level (brightness swell).",
                sys=MD1, patch=voice(algorithm=5, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=24, pan=1),
                rows=[(24, 31, 10, 0, 8, 15, 1, 0, 1, 0, 2),
                      (6, 31, 5, 2, 8, 3, 1, 0, 1, 0, 0),
                      (16, 31, 5, 2, 8, 3, 2, 3, 1, 0, 0),
                      (10, 31, 5, 2, 8, 3, 1, 7, 1, 0, 0)],
                axes={"SSG": [("once", {"op1_ssg": 2}), ("swell", {"op1_ssg": 6})],
                      "transpose": OCT_DOWN},
                tags=["brass", "ssg"]),
    ]


# ----- FM Pad (algorithms 5-7, slow attacks, unison, LFO) -----------------------------------------

def _fm_pad() -> list[Seed]:
    cat = "FM Pad"
    slow_attack = [("slow", all_ops("ar", 12)), ("slower", all_ops("ar", 7))]
    return [
        fm_seed("Warm Pad", cat, "Slow",
                "Warm pad (alg 5): one soft S1 modulator over three carriers (MUL 1, 2, 1 with DT 3/7) that fade "
                "in (AR 12 or 7) and hold (SR 0); alg 6 limits the modulation to S2 for a purer sound.",
                sys=MD1, patch=voice(algorithm=5, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(36, 10, 2, 0, 4, 2, 1, 0, 0, 0, 0),
                      (8, 10, 2, 0, 4, 1, 1, 0, 0, 0, 0),
                      (18, 9, 2, 0, 4, 1, 2, 3, 0, 0, 0),
                      (8, 11, 2, 0, 4, 1, 1, 7, 0, 0, 0)],
                axes={"attack": slow_attack, "algorithm": [5, 6]}, tags=["pad", "warm"]),
        fm_seed("Glass Pad", cat, "Slow",
                "Glassy additive pad (alg 7): sines at MUL 1, 2, 4 and 1 with DT spread and feedback 2 on S1; "
                "slow AR and the release axis (RR 5 or 3, several seconds of tail) shape the swell and tail.",
                sys=MD1, patch=voice(algorithm=7, feedback=2, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(16, 9, 3, 1, 4, 3, 1, 0, 0, 0, 0),
                      (24, 9, 4, 1, 4, 3, 2, 2, 0, 0, 0),
                      (30, 9, 5, 1, 4, 4, 4, 6, 0, 0, 0),
                      (10, 9, 3, 1, 4, 3, 1, 3, 0, 0, 0)],
                axes={"attack": [("slow", all_ops("ar", 11)), ("slower", all_ops("ar", 7))],
                      "release": [("medium", all_ops("rr", 5)), ("long", all_ops("rr", 3))]},
                tags=["pad", "glass"]),
        fm_seed("Choir Pad", cat, "Slow",
                "Vocal pad (alg 6): S1 at MUL 3 lightly modulates S2 for a formant, S3/S4 are detuned sines; a "
                "delayed driver vibrato (rate 6, depth 3, 30 frames) adds the singer's wobble.",
                sys=MD1, patch=voice(algorithm=6, feedback=0, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(6, 3, 30), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(40, 10, 2, 0, 4, 2, 3, 0, 0, 0, 0),
                      (12, 10, 1, 0, 4, 1, 1, 0, 0, 0, 0),
                      (14, 10, 1, 0, 4, 1, 1, 2, 0, 0, 0),
                      (22, 10, 1, 0, 4, 1, 2, 6, 0, 0, 0)],
                axes={"algorithm": [6, 7], "attack": slow_attack}, tags=["pad", "choir"]),
        fm_seed("Dark Pad", cat, "Slow",
                "Dark drone pad (alg 5): S1 at MUL 0 (x0.5) with feedback adds sub sidebands to carriers at "
                "MUL 1, 1 and 0; AR 8 fade-in, SR 0 hold.",
                sys=MD1, patch=voice(algorithm=5, feedback=5, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(44, 8, 2, 0, 3, 2, 0, 0, 0, 0, 0),
                      (10, 8, 2, 0, 3, 1, 1, 0, 0, 0, 0),
                      (12, 8, 2, 0, 3, 1, 1, 5, 0, 0, 0),
                      (14, 8, 2, 0, 3, 1, 0, 3, 0, 0, 0)],
                axes={"feedback": [2, 5], "transpose": OCT_DOWN}, tags=["pad", "dark"]),
        fm_seed("Super Pad", cat, "Unison",
                "Supersaw-style pad: feedback 6 on the common S1 modulator (alg 5) for a saw-like source, doubled "
                "by driver unison (second channel offset by 3-10 fnum units).",
                sys=MD1, patch=voice(algorithm=5, feedback=6, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=6, velocity_depth=12, pan=1),
                rows=[(28, 12, 2, 0, 4, 2, 1, 0, 0, 0, 0),
                      (8, 12, 2, 0, 4, 1, 1, 0, 0, 0, 0),
                      (14, 12, 2, 0, 4, 1, 1, 3, 0, 0, 0),
                      (10, 12, 2, 0, 4, 1, 2, 7, 0, 0, 0)],
                axes={"unison_detune": [3, 6, 10],
                      "attack": [("medium", all_ops("ar", 14)), ("slow", all_ops("ar", 9))]},
                tags=["pad", "unison", "saw"]),
        fm_seed("String Pad", cat, "Unison",
                "String ensemble (alg 6): S1 -> S2 bowed tone plus two detuned sines, unison doubling for the "
                "section spread; the release axis picks a medium (RR 5) or long (RR 3) string tail. Unison "
                "takes two FM channels per note, so only 3 notes sound at once and a new chord steals channels "
                "that are still releasing; RR 2 and below would make that cut-off constant.",
                sys=MD1, patch=voice(algorithm=6, feedback=4, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=4, velocity_depth=12, pan=1),
                rows=[(30, 11, 2, 0, 5, 2, 1, 0, 0, 0, 0),
                      (8, 11, 2, 0, 5, 1, 1, 0, 0, 0, 0),
                      (16, 11, 2, 0, 5, 1, 2, 2, 0, 0, 0),
                      (14, 11, 2, 0, 5, 1, 1, 6, 0, 0, 0)],
                axes={"unison_detune": [2, 4, 7],
                      "release": [("medium", all_ops("rr", 5)), ("long", all_ops("rr", 3))]},
                tags=["pad", "unison", "strings"]),
        fm_seed("Chorus Pad", cat, "Unison",
                "Additive chorus pad (alg 7): four sines at MUL 1, 1 (DT 3), 2 and 4 (DT 7) doubled by unison "
                "(two FM channels per note: 3-note polyphony, hence the moderate RR 4 tail); the vibrato axis "
                "adds a slow delayed driver vibrato.",
                sys=MD1, patch=voice(algorithm=7, feedback=1, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=3, velocity_depth=12, pan=1),
                rows=[(14, 12, 2, 0, 4, 2, 1, 0, 0, 0, 0),
                      (16, 12, 2, 0, 4, 2, 1, 3, 0, 0, 0),
                      (26, 12, 3, 0, 4, 3, 2, 0, 0, 0, 0),
                      (30, 12, 3, 0, 4, 3, 4, 7, 0, 0, 0)],
                axes={"unison_detune": [3, 8],
                      "vibrato": [("off", {"vibrato_rate": 0, "vibrato_depth": 0, "vibrato_delay": 0}),
                                  ("slow", {"vibrato_rate": 7, "vibrato_depth": 4, "vibrato_delay": 24})]},
                tags=["pad", "unison", "chorus"]),
        fm_seed("Organ Pad", cat, "Unison",
                "Drawbar organ pad (alg 7): sines at MUL 1, 2, 4 and 3 with no decay (DR 0, SL 0), AR 16 soft "
                "entry, unison doubling; carriers have AM on for the optional tremolo.",
                sys=MD1, patch=voice(algorithm=7, feedback=3, ams=0, fms=0, transpose=0, fine_tune=0,
                                     vibrato=(0, 0, 0), unison_detune=2, velocity_depth=12, pan=1),
                rows=[(12, 16, 0, 0, 5, 0, 1, 0, 0, 1, 0),
                      (16, 16, 0, 0, 5, 0, 2, 0, 0, 1, 0),
                      (22, 16, 0, 0, 5, 0, 4, 0, 0, 1, 0),
                      (20, 16, 0, 0, 5, 0, 3, 0, 0, 1, 0)],
                axes={"unison_detune": [2, 5],
                      "tremolo": [("off", {"lfo_enable": 0, "lfo_freq": 0, "ams": 0}),
                                  ("slow", {"lfo_enable": 1, "lfo_freq": 1, "ams": 1})]},
                tags=["pad", "unison", "organ"]),
        fm_seed("Vibrato Pad", cat, "LFO",
                "Hardware-LFO vibrato pad (alg 5): FMS depth 2/4/6 (6.7/14/40 cents) at LFO setting 1 or 5 "
                "(5.4 or 9.5 Hz on the NTSC dividers: a singer's vibrato or a fast flutter), applied to the "
                "whole channel as on the chip.",
                sys=md1_lfo(1), patch=voice(algorithm=5, feedback=2, ams=0, fms=4, transpose=0, fine_tune=0,
                                            vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(32, 11, 2, 0, 4, 2, 1, 0, 0, 0, 0),
                      (8, 11, 2, 0, 4, 1, 1, 0, 0, 0, 0),
                      (16, 11, 2, 0, 4, 1, 2, 3, 0, 0, 0),
                      (10, 11, 2, 0, 4, 1, 1, 7, 0, 0, 0)],
                axes={"fms": [2, 4, 6], "lfo_freq": [1, 5]}, tags=["pad", "lfo", "vibrato"]),
        fm_seed("Tremolo Pad", cat, "LFO",
                "Tremolo pad (alg 6): the three carriers have AM on, AMS 1 (1.4 dB) or 3 (11.8 dB) at LFO "
                "setting 0 (3.9 Hz) or 4 (6.7 Hz); S1 -> S2 at MUL 1 keeps a soft reed tone.",
                sys=md1_lfo(0), patch=voice(algorithm=6, feedback=2, ams=1, fms=0, transpose=0, fine_tune=0,
                                            vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(34, 10, 2, 0, 4, 2, 2, 0, 0, 0, 0),
                      (10, 10, 2, 0, 4, 1, 1, 0, 0, 1, 0),
                      (12, 10, 2, 0, 4, 1, 1, 3, 0, 1, 0),
                      (20, 10, 2, 0, 4, 1, 3, 5, 0, 1, 0)],
                axes={"ams": [1, 3], "lfo_freq": [0, 4]}, tags=["pad", "lfo", "tremolo"]),
        fm_seed("Wobble Pad", cat, "LFO",
                "Rough LFO pad: LFO settings 5 and 6 (9.5 and 52 Hz) on either amplitude (AMS 3 on the AM "
                "carriers) or pitch (FMS 5, 20 cents); at 52 Hz the modulation turns into a gritty sideband.",
                sys=md1_lfo(5), patch=voice(algorithm=5, feedback=4, ams=3, fms=0, transpose=0, fine_tune=0,
                                            vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(30, 12, 2, 0, 4, 2, 1, 0, 0, 0, 0),
                      (10, 12, 2, 0, 4, 1, 1, 0, 0, 1, 0),
                      (14, 12, 2, 0, 4, 1, 2, 2, 0, 1, 0),
                      (12, 12, 2, 0, 4, 1, 1, 6, 0, 1, 0)],
                axes={"lfo_freq": [5, 6],
                      "mod": [("amp", {"ams": 3, "fms": 0}), ("pitch", {"ams": 0, "fms": 5})]},
                tags=["pad", "lfo"]),
        fm_seed("Sweep Pad", cat, "LFO",
                "Wah-like pad: AM is set on the S1 modulator only (carriers off), so AMS 3 (11.8 dB) moves the "
                "modulation index instead of the level and the LFO (setting 0 or 4: 3.9 or 6.7 Hz) sweeps the "
                "timbre (alg 5). AMS 1 (1.4 dB) is too small to hear on a modulator and AMS 2 vs 3 counts as "
                "the same depth in QA, so the second axis is S1's TL: the sweep centred on a soft or a bright "
                "tone.",
                sys=md1_lfo(0), patch=voice(algorithm=5, feedback=3, ams=3, fms=0, transpose=0, fine_tune=0,
                                            vibrato=(0, 0, 0), unison_detune=0, velocity_depth=12, pan=1),
                rows=[(26, 10, 2, 0, 4, 2, 1, 0, 0, 1, 0),
                      (8, 10, 2, 0, 4, 1, 1, 0, 0, 0, 0),
                      (16, 10, 2, 0, 4, 1, 2, 3, 0, 0, 0),
                      (10, 10, 2, 0, 4, 1, 1, 7, 0, 0, 0)],
                axes={"tone": [("soft", {"op1_tl": 34}), ("bright", {"op1_tl": 18})], "lfo_freq": [0, 4]},
                tags=["pad", "lfo", "sweep"]),
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
                 "the detuned pair gives a lush, moving tone.",
                 block=lead(1, 2, 0, 15, 8, 2),
                 axes={"psg_unison_detune": [2, 4], "vibrato": [vib_off, vib_slow]},
                 tags=["lead", "doubled", "chorus"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Detuned Pluck", cat, "Doubled",
                 "Detuned pair that decays to sustain 4 over 8 or 20 frames: a harpsichord-like doubled pluck.",
                 block=lead(0, 0, 8, 4, 6, 1),
                 axes={"psg_unison_detune": [1, 5], "psg_sw_decay": [8, 20]},
                 tags=["lead", "doubled", "pluck"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Swell Lead", cat, "Doubled",
                 "Slow-swelling doubled lead (attack 10 or 20 frames) for held notes over FM pads.",
                 block=lead(1, 10, 0, 15, 10, 2),
                 axes={"psg_unison_detune": [2, 6], "psg_sw_attack": [10, 20]},
                 tags=["lead", "doubled", "swell"], global_params=PSG_TONE_GLOBALS),
        psg_seed("Fat Lead", cat, "Doubled",
                 "Widest doubling (detune 5 or 7 period units, 40-60 cents at C5, a fast rough beat) at full or "
                 "-10 dB level: the loudest PSG lead the chip can make.",
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
                 "Doubled square bass (playable from A1): the second tone channel is detuned by 3 or 7 period "
                 "units (5-17 or 12-40 cents over the bass range, 1 unit would be inaudible), with either an "
                 "accent to sustain 10 or a 16-frame pluck to sustain 3.",
                 block=tone_bass(0, 0, 3, 10, 3, 3),
                 axes={"psg_unison_detune": [3, 7],
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
        psg_seed("Pitched Snare", cat, "White",
                 "White noise clocked by the silent tone 3, whose shift rate is 16x the key frequency, so the "
                 "key sets the colour: keys A3-A4 match the fixed /1024 and /512 rates (3.5-7 kHz, snare), "
                 "higher keys hiss, keys below C3 rumble like an explosion; 8 or 16-frame decay.",
                 block=noise(white, 3, 10, 0, 0),
                 axes={"psg_sw_decay": [8, 16], "psg_transpose": OCT_UP}, tags=["drums", "snare", "white", "keyed"],
                 global_params=PSG_NOISE_GLOBALS),
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
        psg_seed("Zap", cat, "Periodic",
                 "Laser zap: periodic noise on the tone 3 clock with deep driver vibrato on the tone 3 period "
                 "(12 or 8 units on a period of about 27 at C4, several semitones, wider on higher keys), fast or "
                 "slow, decaying over 10 or 20 frames.",
                 block=noise(periodic, 3, 16, 1, 12),
                 axes={"psg_sw_decay": [10, 20],
                       "vibrato": [("fast", {"psg_vibrato_rate": 1, "psg_vibrato_depth": 12}),
                                   ("slow", {"psg_vibrato_rate": 4, "psg_vibrato_depth": 8})]},
                 tags=["drums", "sfx", "periodic"], global_params=PSG_NOISE_GLOBALS),
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
