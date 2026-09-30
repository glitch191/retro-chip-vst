"""SNES (S-DSP) seed patches.

Written the way an SPC700 composer builds an instrument set: every instrument is one BRR
sample plus an envelope (ADSR or GAIN), a voice volume/pan, optional driver vibrato, and
the global echo unit with a per-voice EON mask. Values are raw register values
(docs/ENGINE_SPECS.md, SNES section; timings from the rate table in docs/research/snes.md):

* attack A (0..14): rate 2A+1, 63 steps of +32 -> A4 630 ms, A6 252 ms, A9 63 ms, A12 16 ms;
  A15 is instant (two +1024 steps).
* decay D (0..7): rate 2D+16, exponential down to sustain level SL (E >> 8 == SL).
* sustain rate SR (0..31): exponential fade after decay, 0 = hold.
* release_mode 0 = hardware KOFF (-8 per sample, 8 ms); 1 = driver GAIN exponential
  decrease at release_rate (R12 ~3.5 s, R16 ~1.4 s, R20 ~0.5 s, R24 ~0.2 s to silence).
* GAIN linear increase is the ADSR attack curve (+32 per step, 64 steps) at any rate, so it
  is not used for swells; bent-line increase (+32 up to 0x600, then +8: 112 steps) is the
  shape ADSR cannot make and is what every "Swell" here uses. Direct GAIN fixes E = value x 16.
* echo_delay EDL x 16 ms (buffer EDL x 2 KiB of the 64 KiB ARAM), echo_feedback EFB and
  echo_volume EVOL are signed 8-bit.
* noise_clock is the FLG noise rate (32000 / period of the same rate table: 8 -> 83 Hz,
  16 -> 500 Hz, 22 -> 2 kHz, 24 -> 3.2 kHz, 30 -> 16 kHz, 31 -> 32 kHz). A noise voice still
  steps through its BRR sample at the key's pitch, so the sample's end code still ends the
  note: sustained noise uses the looped noise sample.

Driver vibrato (engine parameters, ENGINE_SPECS): `vibrato_rate` counts 4 ms ticks per
half cycle (0..63): rate 23 is 5.4 Hz, 18 is 6.9 Hz, 4 is 31 Hz. Profiles are named after
their real rate. `vibrato_depth` is in pitch-register units, so its size in cents depends on the
pitch register value P: depth 40 is about 17 cents at P = 0x1000 (a 32 kHz sample at its
root key) and twice that at P = 0x800 (a 16 kHz sample, or a 32 kHz sample an octave
down), which is why the low/16 kHz seeds use half the depth.

Pitch ceiling: the 14-bit pitch register stops at 0x3FFF, just under two octaves above
the root of a 32 kHz sample. Melodic seeds never transpose 32 kHz A3-rooted samples up an
octave (every key above A4 would play the same clamped pitch); the octave-up variant exists
only on a 16 kHz sample, and the small upward steps of drums and PMON Metal lower their key
ceiling by the same few semitones.

Echo FIR sets: the canonical echo profiles use only the documented coefficient sets
(Pass-through = N-SPC identity, Low-pass soft, High-pass = N-SPC 3 kHz, Band-pass = N-SPC
1.5-8.5 kHz; docs/research/snes.md "Echo", Ambiguity 14). Their maximum gain is at most
+0.91 dB (x1.11), so EFB <= 104 (0.81) keeps EFB x max|H| < 0.91 and every tail decays.
The in-house sets (Low-pass strong, Comb, Bright, Dark) are not used by any seed. There is
a single EVOL register pair in the engine, so the echo is centred: no "wide" echo.

PMON: voice x is pitch-modulated by the output of voice x-1 (voice 1 never is, voice 8 is
never a source). With the round-robin allocator the pairing depends on allocation: a note
is modulated by whatever the previously allocated voice is playing, a note that lands on
voice 1 is not modulated, and a single note after silence plays unmodulated. PMON seeds
therefore release fast (KOFF or GAIN release 24) so that a finished note does not colour
the next one; hold two notes (or use the MIDI-channel voice mode on voices 1 and 2) to
hear the modulation. PMON is ignored on noise voices, so PMON seeds keep noise off.

One-shot samples: the BRR end code sets the envelope to 0 at once, so on a one-shot the
note ends with the sample (0.6 s for the guitars at the root key, half that an octave up),
whatever the SR or release values; envelopes on one-shots are chosen to be quiet by then
so the cut does not click. The recorded pianos are an attack plus a short sustain loop, as
in SPC sets, so their SR alone shapes the decay.

Recorded samples: the pianos, strings, brass, winds, pipe organ, upright bass and the drum
kit are CC0 recordings converted by tools/samplegen/cc0_import.py (sources in
tools/samplegen/cc0_manifest.json), most melodic ones at 16 kHz (P = 0x800 at the root,
so their vibrato profiles are the half-depth `_LOW` ones), the bright piano and the
trumpet at 24 kHz (P = 0xC00).

Sample slots: `sample` = position of the sample in the snes list of
assets/samples/index.json for the first 32 entries. That list holds more than 32 names (48);
each sample beyond position 31 shares the slot of the first not-yet-shared sample of the
opposite family (drum kit vs everything else), and `seed()` checks that the two families
always differ in (loop_override, noise_enable). The preset's `samples` entry names the WAV
that the preset manager loads into the slot, and because presets on a shared slot always
differ in those registers, the parameter-only QA can never merge two presets that play
different samples.

Every seed sets all 35 engine parameters through the block helpers below (each helper
takes every value of its block explicitly); `seed()` checks the key set.
"""

from __future__ import annotations

import json
from functools import lru_cache
from pathlib import Path
from typing import Any, Mapping, Sequence

from presetgen.model import Seed, is_variant_axis

SAMPLE_INDEX = Path(__file__).resolve().parents[3] / "assets" / "samples" / "index.json"
SLOT_COUNT = 32

# Every engine parameter, in engine order (tools/presetgen/params/snes.json).
KEYS = (
    "sample", "adsr_enable", "attack", "decay", "sustain_level", "sustain_rate",
    "gain_mode", "gain_value", "release_mode", "release_rate", "volume", "pan",
    "transpose", "fine_tune", "vibrato_rate", "vibrato_depth", "vibrato_delay",
    "noise_enable", "noise_clock", "pmon", "loop_override", "echo_enable", "echo_delay",
    "echo_feedback", "echo_volume", "fir_preset", "v1_echo", "v2_echo", "v3_echo",
    "v4_echo", "v5_echo", "v6_echo", "v7_echo", "v8_echo", "main_volume",
)

# gain_mode values
GAIN_DIRECT, GAIN_LIN_DEC, GAIN_EXP_DEC, GAIN_LIN_INC, GAIN_BENT_INC = 0, 1, 2, 3, 4
# fir_preset values (0, 1, 3, 4 are documented sets; 2, 5, 6, 7 are in-house designs)
FIR_PASS, FIR_LP_SOFT, FIR_LP_STRONG, FIR_HIGH, FIR_BAND, FIR_COMB, FIR_BRIGHT, FIR_DARK = range(8)
# loop_override values
LOOP_DEFAULT, LOOP_ONE_SHOT, LOOP_FORCE = 0, 1, 2

ALL_ON = (1, 1, 1, 1, 1, 1, 1, 1)
ALL_OFF = (0, 0, 0, 0, 0, 0, 0, 0)
ODD_ON = (1, 0, 1, 0, 1, 0, 1, 0)   # voices 1, 3, 5, 7 wet: with round-robin, every other note dry

# Registers that separate drum-kit presets from all others on a shared sample slot.
FAMILY_KEYS = ("loop_override", "noise_enable")


# ----- sample slots -----------------------------------------------------------------------

@lru_cache(maxsize=1)
def sample_entries() -> tuple[tuple[str, bool], ...]:
    """(sample name, is a drum-kit sample) in the order of the snes list of index.json."""
    with open(SAMPLE_INDEX, "r", encoding="utf-8") as fh:
        entries = json.load(fh)
    return tuple((str(entry["name"]), entry.get("category") == "drums")
                 for entry in entries if entry.get("chip") == "snes")


@lru_cache(maxsize=1)
def sample_slots() -> dict[str, int]:
    """Sample name -> slot (see the module docstring for samples beyond position 31)."""
    entries = sample_entries()
    slots = {name: position for position, (name, _) in enumerate(entries[:SLOT_COUNT])}
    shared: set[int] = set()
    for name, is_drum in entries[SLOT_COUNT:]:
        for position, (_, other_is_drum) in enumerate(entries[:SLOT_COUNT]):
            if position not in shared and other_is_drum != is_drum:
                slots[name] = position
                shared.add(position)
                break
        else:
            raise ValueError(f"snes: no free slot of the opposite family for sample {name!r}")
    return slots


def is_drum_sample(name: str) -> bool:
    return dict(sample_entries())[name]


def is_drum_family(params: Mapping[str, int]) -> bool:
    """Drum-kit presets force one-shot playback on a plain (non-noise) voice."""
    return params["loop_override"] == LOOP_ONE_SHOT and params["noise_enable"] == 0


# ----- parameter blocks -------------------------------------------------------------------

KOFF = {"release_mode": 0, "release_rate": 0}


def exp_release(rate: int) -> dict[str, int]:
    """Driver release: GAIN exponential decrease at `rate` written at note off."""
    return {"release_mode": 1, "release_rate": rate}


def adsr(attack: int, decay: int, sustain_level: int, sustain_rate: int,
         release: Mapping[str, int]) -> dict[str, int]:
    """Hardware ADSR (ADSR1.7 = 1); the GAIN register is unused and parked at direct 127."""
    return {"adsr_enable": 1, "attack": attack, "decay": decay, "sustain_level": sustain_level,
            "sustain_rate": sustain_rate, "gain_mode": GAIN_DIRECT, "gain_value": 127, **release}


def gain(mode: int, value: int, release: Mapping[str, int]) -> dict[str, int]:
    """GAIN envelope (ADSR1.7 = 0); the ADSR fields are unused and parked at A15 D7 SL7 SR0."""
    return {"adsr_enable": 0, "attack": 15, "decay": 7, "sustain_level": 7, "sustain_rate": 0,
            "gain_mode": mode, "gain_value": value, **release}


def mix(volume: int, pan: int, transpose: int, fine_tune: int) -> dict[str, int]:
    return {"volume": volume, "pan": pan, "transpose": transpose, "fine_tune": fine_tune}


def vib(rate: int, depth: int, delay: int) -> dict[str, int]:
    """Driver vibrato: `rate` 4 ms ticks per half cycle, `depth` pitch register units."""
    return {"vibrato_rate": rate, "vibrato_depth": depth, "vibrato_delay": delay}


def source(noise_enable: int, noise_clock: int, pmon: int, loop_override: int) -> dict[str, int]:
    return {"noise_enable": noise_enable, "noise_clock": noise_clock, "pmon": pmon,
            "loop_override": loop_override}


def eon(mask: Sequence[int]) -> dict[str, int]:
    return {f"v{voice + 1}_echo": bit for voice, bit in enumerate(mask)}


def echo(delay: int, feedback: int, volume: int, fir: int, mask: Sequence[int]) -> dict[str, int]:
    return {"echo_enable": 1, "echo_delay": delay, "echo_feedback": feedback, "echo_volume": volume,
            "fir_preset": fir, **eon(mask)}


# ----- named profiles ---------------------------------------------------------------------

VIB_OFF = vib(0, 0, 0)
VIB_5HZ = vib(23, 40, 40)      # 5.4 Hz (23 ticks), ~17 cents at P 0x1000, after 160 ms
VIB_5HZ_LOW = vib(23, 20, 40)  # the same in cents for P around 0x800 (16 kHz samples, octave-down seeds)
VIB_5HZ_LOW2 = vib(23, 10, 40)  # the same in cents for P around 0x400 (16 kHz samples an octave down)
VIB_DELAYED = vib(23, 48, 125) # 5.4 Hz, ~20 cents, only on held notes (after 500 ms)
VIB_DELAYED_LOW = vib(23, 24, 125)  # the same in cents for P around 0x800 (16 kHz samples)
VIB_7HZ = vib(18, 32, 15)      # 6.9 Hz, ~13 cents shimmer, rotary-like, after 60 ms
VIB_7HZ_LOW = vib(18, 16, 15)  # the same in cents for P around 0x800 (16 kHz samples)
VIB_DEEP = vib(4, 64, 0)       # 31 Hz, maximum depth: an audible warble for SFX

ECHO_OFF = {"echo_enable": 0, "echo_delay": 0, "echo_feedback": 0, "echo_volume": 0,
            "fir_preset": FIR_PASS, **eon(ALL_OFF)}
ECHO_SLAP = echo(2, 20, 36, FIR_LP_SOFT, ALL_ON)      # 32 ms single slap
ECHO_SHORT = echo(3, 32, 44, FIR_LP_SOFT, ALL_ON)     # 48 ms, two or three repeats
ECHO_ROOM = echo(4, 40, 40, FIR_BAND, ALL_ON)         # 64 ms band-passed (N-SPC) room
ECHO_LONG = echo(8, 84, 48, FIR_LP_SOFT, ALL_ON)      # 128 ms, long low-passed tail
ECHO_BRIGHT = echo(10, 90, 44, FIR_HIGH, ALL_ON)      # 160 ms thin high-passed (N-SPC 3 kHz) repeats
ECHO_HALL = echo(12, 104, 52, FIR_LP_SOFT, ALL_ON)    # 192 ms, EFB 104: very long darkening tail

ECHO_3 = [("off", ECHO_OFF), ("Short", ECHO_SHORT), ("Long", ECHO_LONG)]
ECHO_2 = [("off", ECHO_OFF), ("Short", ECHO_SHORT)]
PAD_ECHO_3 = [("Short", ECHO_SHORT), ("Long", ECHO_LONG), ("Hall", ECHO_HALL)]

TONE = source(0, 0, 0, LOOP_DEFAULT)                    # plain sample voice
DRUM_SOURCE = source(0, 0, 0, LOOP_ONE_SHOT)            # never loops a drum hit
PMON_SOURCE = source(0, 0, 1, LOOP_DEFAULT)             # pitch modulated by the previous voice


def noise_source(clock: int, loop_override: int = LOOP_DEFAULT) -> dict[str, int]:
    return source(1, clock, 0, loop_override)


# Drum envelopes: the sample carries the hit, the envelope only decides the tail.
DRUM_FULL = adsr(15, 7, 7, 0, exp_release(16))   # whole sample; a slow release so short MIDI notes do not choke
DRUM_SHORT = adsr(15, 5, 0, 24, exp_release(22))  # D5 to SL0 then SR24: tail cut to ~0.1 s
DRUM_CHOKE = adsr(15, 7, 7, 0, KOFF)              # hardware KOFF: note off chokes in 8 ms

# Velocity layers: each layer changes the hit, not only its level (the plugin already maps
# MIDI velocity to VxVOL): an accent a semitone up at full level, a ghost note two
# semitones down, short and quiet.
LAYERS = [("Accent Note", {"volume": 127, "transpose": 1}),
          ("Ghost Note", {"volume": 56, "transpose": -2, **DRUM_SHORT})]
TAIL = [("", KOFF), ("Tail", exp_release(20))]

# Kit panning, drummer's view throughout: hats and crash left, ride and floor tom right.
PAN_HAT, PAN_CRASH, PAN_RIDE = -24, -24, 24
PAN_TOM_HIGH, PAN_TOM_MID, PAN_TOM_LOW = -16, 8, 24


# ----- seed builder -----------------------------------------------------------------------

def seed(name: str, category: str, subcategory: str, sample: str, comment: str, *,
         env: Mapping[str, int], mixing: Mapping[str, int], vibrato: Mapping[str, int],
         voice: Mapping[str, int], fx: Mapping[str, int], main_volume: int,
         axes: Mapping[str, Sequence[Any]], tags: Sequence[str]) -> Seed:
    slots = sample_slots()
    if sample not in slots:
        raise ValueError(f"snes seed {name!r}: unknown sample {sample!r}")
    params: dict[str, int] = {"sample": slots[sample]}
    for block in (env, mixing, vibrato, voice, fx):
        params.update(block)
    params["main_volume"] = main_volume
    if set(params) != set(KEYS) or len(params) != len(KEYS):
        missing = sorted(set(KEYS) - set(params))
        extra = sorted(set(params) - set(KEYS))
        raise ValueError(f"snes seed {name!r}: missing {missing}, unexpected {extra}")
    # Shared slots stay unambiguous only if the sample family is fixed by the base registers.
    if is_drum_family(params) != is_drum_sample(sample):
        raise ValueError(f"snes seed {name!r}: loop_override/noise_enable do not match the "
                         f"family of sample {sample!r}")
    for axis, values in axes.items():
        touched = ({key for _, overrides in values for key in overrides}
                   if is_variant_axis(values) else {axis})
        if touched & {"sample", *FAMILY_KEYS}:
            raise ValueError(f"snes seed {name!r}: axis {axis!r} may not change the sample or its family")
    ordered = {key: params[key] for key in KEYS}
    return Seed(name=name, category=category, subcategory=subcategory, params=ordered,
                axes=dict(axes), tags=sorted(set(tags)), samples={"sample": sample}, comment=comment)


# ----- Instrument ---------------------------------------------------------------------------

def instrument_seeds() -> list[Seed]:
    piano = adsr(15, 3, 2, 14, exp_release(18))
    strings = adsr(9, 0, 6, 0, exp_release(16))
    brass = adsr(11, 4, 5, 0, exp_release(20))
    trumpet = adsr(12, 5, 5, 0, exp_release(22))
    nylon = adsr(15, 3, 3, 16, exp_release(20))
    muted = adsr(15, 6, 1, 24, KOFF)
    flute = adsr(10, 7, 7, 0, exp_release(20))
    organ = adsr(15, 7, 7, 0, KOFF)
    return [
        # Piano (recorded attack plus a short sustain loop: SR shapes the decay)
        seed("Grand Piano", "Instrument", "Piano", "piano_bright",
             "Bright grand piano (24 kHz recording, 70 ms sustain loop): A15 instant hammer, D3 to "
             "SL2 then SR14 is the string decay; GAIN exp release 18 as the damper on short notes.",
             env=piano, mixing=mix(104, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_OFF, main_volume=127,
             axes={"soft": [("", piano), ("Soft", adsr(12, 2, 3, 12, exp_release(16)))], "echo": ECHO_3},
             tags=["piano", "keys"]),
        seed("Soft Piano", "Instrument", "Piano", "piano_soft",
             "Soft-touch upright piano (16 kHz recording, 110 ms sustain loop): A14 (6 ms) rounds "
             "the hammer, D2 to SL3 then SR12 lets the note die away slowly.",
             env=adsr(14, 2, 3, 12, exp_release(16)), mixing=mix(108, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_OFF, main_volume=127,
             axes={"echo": [("off", ECHO_OFF), ("Short", ECHO_SHORT), ("Hall", ECHO_HALL)]},
             tags=["piano", "keys"]),
        seed("EPiano", "Instrument", "Piano", "epiano",
             "FM electric piano (recorded TX81Z, 16 kHz loop): A15 D4 SL2 SR16 for the tine bark "
             "and fade; the half-depth 5.4 Hz driver vibrato stands in for the tremolo chorus.",
             env=adsr(15, 4, 2, 16, exp_release(20)), mixing=mix(100, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_OFF, main_volume=127,
             axes={"vib": [("off", VIB_OFF), ("Vib5Hz", VIB_5HZ_LOW)], "echo": ECHO_3},
             tags=["epiano", "keys"]),
        # Strings
        seed("Ensemble Strings", "Instrument", "Strings", "strings_ensemble",
             "Playable string section: A9 (63 ms) bow, D0 to SL6 held (SR0) on the looped violin and "
             "cello sections, delayed vibrato (half depth for the 16 kHz sample) only on long notes.",
             env=strings, mixing=mix(96, 0, 0, 0), vibrato=VIB_DELAYED_LOW, voice=TONE, fx=ECHO_OFF,
             main_volume=127,
             axes={"soft": [("", strings), ("Soft", adsr(6, 0, 7, 0, exp_release(14)))], "echo": ECHO_3},
             tags=["strings", "orchestral"]),
        seed("Pizzicato", "Instrument", "Strings", "strings_pizz",
             "Violin section pizzicato: A15 D7 SL7 SR0 lets the recorded 0.45 s one-shot carry its own "
             "pluck and decay, GAIN release 22 on note off; an octave down gives cellos and basses.",
             env=adsr(15, 7, 7, 0, exp_release(22)), mixing=mix(104, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_OFF, main_volume=127,
             axes={"transpose": [0, -12], "echo": ECHO_3},
             tags=["strings", "pizzicato", "orchestral"]),
        # Brass
        seed("Brass Section", "Instrument", "Brass", "brass_section",
             "Trumpet, horn and trombone section: A11 (24 ms) lip attack, D4 to SL5 held; the Swell "
             "variant uses GAIN bent-line increase rate 15 (~280 ms), the classic SPC brass crescendo.",
             env=brass, mixing=mix(100, 0, 0, 0), vibrato=VIB_DELAYED_LOW, voice=TONE, fx=ECHO_OFF,
             main_volume=127,
             axes={"swell": [("", brass), ("Swell", gain(GAIN_BENT_INC, 15, exp_release(20)))], "echo": ECHO_3},
             tags=["brass", "orchestral"]),
        seed("Solo Trumpet", "Instrument", "Brass", "trumpet",
             "Solo trumpet (vibrato baked in the sample, so driver vibrato off): A12 D5 SL5 held; "
             "Stab is A15 D6 SL2 SR22 with KOFF for short fanfare hits.",
             env=trumpet, mixing=mix(100, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_OFF, main_volume=127,
             axes={"stab": [("", trumpet), ("Stab", adsr(15, 6, 2, 22, KOFF))], "echo": ECHO_3},
             tags=["brass", "trumpet", "lead"]),
        seed("French Horn", "Instrument", "Brass", "horn",
             "Solo French horn (16 kHz loop): A10 (40 ms) soft lip attack, D5 to SL6 held, GAIN "
             "release 18; delayed half-depth vibrato on held notes.",
             env=adsr(10, 5, 6, 0, exp_release(18)), mixing=mix(100, 0, 0, 0), vibrato=VIB_DELAYED_LOW,
             voice=TONE, fx=ECHO_OFF, main_volume=127,
             axes={"echo": ECHO_3},
             tags=["brass", "horn", "orchestral"]),
        seed("Trombone", "Instrument", "Brass", "trombone",
             "Solo tenor trombone (16 kHz loop): A12 D4 SL5 held, GAIN release 20; Stab is A15 D6 "
             "SL2 SR22 with KOFF for accents.",
             env=adsr(12, 4, 5, 0, exp_release(20)), mixing=mix(100, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_OFF, main_volume=127,
             axes={"stab": [("", adsr(12, 4, 5, 0, exp_release(20))), ("Stab", adsr(15, 6, 2, 22, KOFF))],
                   "echo": ECHO_2},
             tags=["brass", "trombone", "orchestral"]),
        # Guitar (0.6 s one-shots)
        seed("Nylon Guitar", "Instrument", "Guitar", "guitar_nylon",
             "Nylon guitar: A15 D3 to SL3 then SR16 lets the plucked string ring for the length of "
             "the sample; Pluck (D5 SL1 SR20) is the short arpeggio variant.",
             env=nylon, mixing=mix(104, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_OFF, main_volume=127,
             axes={"pluck": [("", nylon), ("Pluck", adsr(15, 5, 1, 20, exp_release(24)))], "echo": ECHO_3},
             tags=["guitar", "acoustic"]),
        seed("Steel Guitar", "Instrument", "Guitar", "guitar_steel",
             "Steel guitar: A15 D2 SL4 SR14 for a brighter, longer ring, panned slightly left; the "
             "5.4 Hz vibrato imitates finger vibrato on held notes.",
             env=adsr(15, 2, 4, 14, exp_release(18)), mixing=mix(100, -16, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_OFF, main_volume=127,
             axes={"vib": [("off", VIB_OFF), ("Vib5Hz", VIB_5HZ)], "echo": ECHO_3},
             tags=["guitar", "acoustic"]),
        seed("Muted Guitar", "Instrument", "Guitar", "guitar_muted",
             "Palm-muted guitar: A15 D6 to SL1 then SR24 and hardware KOFF, a chugging rhythm part "
             "on a 0.15 s one-shot; a 32 ms slap or a 48 ms short echo doubles it.",
             env=muted, mixing=mix(108, 16, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_OFF, main_volume=127,
             axes={"echo": [("off", ECHO_OFF), ("Slap", ECHO_SLAP), ("Short", ECHO_SHORT)]},
             tags=["guitar", "muted", "rhythm"]),
        # Woodwind
        seed("Flute", "Instrument", "Woodwind", "flute",
             "Flute (breath and vibrato in the looped sample): A10 (40 ms) breath onset, SL7 held; "
             "Soft uses A7 (158 ms) and a longer release for slow lines.",
             env=flute, mixing=mix(96, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_OFF, main_volume=127,
             axes={"soft": [("", flute), ("Soft", adsr(7, 7, 7, 0, exp_release(16)))], "echo": ECHO_3},
             tags=["woodwind", "flute", "lead"]),
        seed("Clarinet", "Instrument", "Woodwind", "clarinet",
             "Clarinet (recorded without vibrato, 16 kHz loop): A11 D7 SL7 held with GAIN release "
             "22, half-depth driver vibrato optional since the sample is dry.",
             env=adsr(11, 7, 7, 0, exp_release(22)), mixing=mix(100, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_OFF, main_volume=127,
             axes={"vib": [("off", VIB_OFF), ("Vib5Hz", VIB_5HZ_LOW)], "echo": ECHO_3},
             tags=["woodwind", "clarinet", "lead"]),
        seed("Oboe", "Instrument", "Woodwind", "oboe",
             "Oboe (recorded without vibrato, 16 kHz loop): A11 then D3 to SL5 (~33 ms, -2.4 dB) for "
             "a reed accent before the held tone; vibrato none, 5.4 Hz or a nervous 6.9 Hz (half depth).",
             env=adsr(11, 3, 5, 0, exp_release(22)), mixing=mix(96, 8, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_OFF, main_volume=127,
             axes={"vib": [("off", VIB_OFF), ("Vib5Hz", VIB_5HZ_LOW), ("Vib7Hz", VIB_7HZ_LOW)], "echo": ECHO_2},
             tags=["woodwind", "oboe", "lead"]),
        # Organ
        seed("Drawbar Organ", "Instrument", "Organ", "organ_full",
             "Drawbar organ: A15 D7 SL7 SR0 is a gate (full level while held), hardware KOFF for the "
             "key click stop; 6.9 Hz vibrato for a rotary feel.",
             env=organ, mixing=mix(92, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_OFF, main_volume=127,
             axes={"vib": [("off", VIB_OFF), ("Vib7Hz", VIB_7HZ)], "echo": ECHO_3},
             tags=["organ", "keys"]),
        seed("Perc Organ", "Instrument", "Organ", "organ_perc",
             "Percussive organ: the click is in the sample, the gate envelope holds the loop; "
             "Long swaps the KOFF release for GAIN exp 18 to imitate a church hall tail.",
             env=organ, mixing=mix(96, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_OFF, main_volume=127,
             axes={"long": [("", KOFF), ("Long", exp_release(18))], "echo": ECHO_3},
             tags=["organ", "keys", "percussive"]),
        seed("Pipe Organ", "Instrument", "Organ", "organ_pipe",
             "Recorded pipe organ (open manual, 25 ms loop): gate envelope A15 D7 SL7, GAIN exp "
             "release 18 for the church tail; the long or hall echo stands in for the nave.",
             env=adsr(15, 7, 7, 0, exp_release(18)), mixing=mix(92, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_OFF, main_volume=127,
             axes={"echo": [("off", ECHO_OFF), ("Long", ECHO_LONG), ("Hall", ECHO_HALL)]},
             tags=["organ", "pipe", "keys"]),
        seed("Chip Organ", "Instrument", "Organ", "square_loop",
             "Square-wave organ from an 11-cycle loop: gate envelope with KOFF, volume 84 because a "
             "full-scale square is louder than the recorded samples.",
             env=organ, mixing=mix(84, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_OFF, main_volume=127,
             axes={"vib": [("off", VIB_OFF), ("Vib7Hz", VIB_7HZ)], "echo": ECHO_3},
             tags=["organ", "chip", "square"]),
    ]


# ----- Bass -------------------------------------------------------------------------------

def bass_seeds() -> list[Seed]:
    finger = adsr(15, 2, 5, 16, exp_release(22))
    slap = adsr(15, 4, 3, 18, exp_release(22))
    synth = adsr(15, 3, 3, 20, exp_release(24))
    saw = adsr(15, 4, 5, 0, exp_release(24))
    sine = adsr(14, 7, 7, 0, KOFF)
    triangle = adsr(15, 5, 4, 0, KOFF)
    dry = ECHO_OFF  # basses stay out of the echo (EON off on every voice)
    return [
        # Sample: fixed-length recorded basses (one-shots) and looped waveforms
        seed("Finger Bass", "Bass", "Sample", "bass_finger",
             "Fingered bass on a 0.5 s one-shot (1 s an octave down): A15 D2 to SL5 then SR16 "
             "fades with the string so the end code cuts at about -25 dB; dry and centred.",
             env=finger, mixing=mix(116, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=dry, main_volume=127,
             axes={"pluck": [("", finger), ("Pluck", adsr(15, 5, 2, 18, exp_release(24)))], "transpose": [0, -12]},
             tags=["bass", "electric"]),
        seed("Slap Bass", "Bass", "Sample", "bass_slap",
             "Slap bass on a 0.4 s one-shot: A15 D4 SL3 SR18; Pop is D6 SL1 SR20 with hardware KOFF "
             "for staccato funk; dry like every bass.",
             env=slap, mixing=mix(112, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=dry, main_volume=127,
             axes={"pop": [("", slap), ("Pop", adsr(15, 6, 1, 20, KOFF))], "transpose": [0, -12]},
             tags=["bass", "slap"]),
        seed("Upright Bass", "Bass", "Sample", "bass_upright",
             "Recorded contrabass pizzicato, an 11 kHz one-shot (0.7 s at the root, 1.4 s an octave "
             "down): A15 D2 to SL5 then SR16 follows the string; dry and centred.",
             env=adsr(15, 2, 5, 16, exp_release(22)), mixing=mix(116, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=dry, main_volume=127,
             axes={"transpose": [0, -12]},
             tags=["bass", "acoustic", "upright"]),
        seed("Synth Bass", "Bass", "Sample", "bass_synth",
             "Filtered saw synth bass, a fixed-length 0.4 s one-shot that is still loud at its end: "
             "A15 D3 SL3 SR20 fades it to about -27 dB by then so the end code does not click; "
             "Pluck for a sequenced 16th-note bass.",
             env=synth, mixing=mix(112, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=dry, main_volume=127,
             axes={"pluck": [("", synth), ("Pluck", adsr(15, 6, 1, 18, exp_release(24)))], "transpose": [0, -12]},
             tags=["bass", "synth"]),
        seed("Saw Bass", "Bass", "Sample", "saw_loop",
             "Raw looped saw one octave down or at key pitch: A15 D4 to SL5 held, GAIN release 24 "
             "(~0.2 s) so notes do not click; Pluck adds SR16 for a fading note.",
             env=saw, mixing=mix(96, 0, -12, 0), vibrato=VIB_OFF, voice=TONE, fx=dry, main_volume=127,
             axes={"transpose": [-12, 0], "pluck": [("", saw), ("Pluck", adsr(15, 5, 2, 16, exp_release(24)))]},
             tags=["bass", "synth", "saw"]),
        seed("Sine Sub", "Bass", "Sample", "sine_loop",
             "Sine sub bass from the 11-cycle loop at key pitch or an octave down: gate envelope "
             "(A14 avoids the start click), KOFF; Soft uses A10 and a GAIN tail for legato lines.",
             env=sine, mixing=mix(124, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=dry, main_volume=127,
             axes={"transpose": [0, -12], "soft": [("", sine), ("Soft", adsr(10, 7, 7, 0, exp_release(20)))]},
             tags=["bass", "sub", "sine"]),
        seed("Triangle Bass", "Bass", "Sample", "triangle_loop",
             "NES-style triangle bass on the S-DSP: A15 D5 to SL4 held, KOFF; the Gaussian "
             "interpolation softens the corners compared with the 2A03; Soft (A10, GAIN tail) "
             "for legato lines.",
             env=triangle, mixing=mix(120, 0, -12, 0), vibrato=VIB_OFF, voice=TONE, fx=dry, main_volume=127,
             axes={"transpose": [-12, 0], "soft": [("", triangle), ("Soft", adsr(10, 5, 4, 0, exp_release(20)))]},
             tags=["bass", "chip", "triangle"]),
        # Short: staccato basses, decay to SL0/SL1 so the note ends by itself
        seed("Short Finger", "Bass", "Short", "bass_finger",
             "Staccato finger bass: A15 D6 straight to SL0 then SR24, note gone in ~0.15 s whatever "
             "the gate.",
             env=adsr(15, 6, 0, 24, KOFF), mixing=mix(116, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=dry,
             main_volume=127, axes={"transpose": [0, -12]},
             tags=["bass", "short", "staccato"]),
        seed("Short Synth", "Bass", "Short", "bass_synth",
             "Short synth bass: A15 D5 to SL1 then SR24, a punchy sequencer bass, dry.",
             env=adsr(15, 5, 1, 24, KOFF), mixing=mix(112, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=dry,
             main_volume=127, axes={"transpose": [0, -12]},
             tags=["bass", "synth", "short", "staccato"]),
        seed("Short Slap", "Bass", "Short", "bass_slap",
             "Short slap: A15 D6 to SL0 then SR26 keeps only the thumb transient.",
             env=adsr(15, 6, 0, 26, KOFF), mixing=mix(112, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=dry,
             main_volume=127, axes={"transpose": [0, -12]},
             tags=["bass", "slap", "short", "staccato"]),
        seed("Short Saw", "Bass", "Short", "saw_loop",
             "Short saw bass: the looped saw never ends by itself, so D6 to SL0 and SR22 shape a "
             "~0.2 s blip; an octave down or at key pitch.",
             env=adsr(15, 6, 0, 22, KOFF), mixing=mix(100, 0, -12, 0), vibrato=VIB_OFF, voice=TONE, fx=dry,
             main_volume=127, axes={"transpose": [-12, 0]},
             tags=["bass", "synth", "saw", "short", "staccato"]),
        seed("Short Square", "Bass", "Short", "square_loop",
             "Short square bass: D5 to SL0 then SR24 on the looped square, volume 88 for level "
             "parity with the recorded basses.",
             env=adsr(15, 5, 0, 24, KOFF), mixing=mix(88, 0, -12, 0), vibrato=VIB_OFF, voice=TONE, fx=dry,
             main_volume=127, axes={"transpose": [-12, 0]},
             tags=["bass", "chip", "square", "short", "staccato"]),
        seed("Muted Bass", "Bass", "Short", "guitar_muted",
             "Palm-muted guitar one-shot played an octave down as a thumpy bass: A15 D6 SL1 SR24, "
             "KOFF.",
             env=adsr(15, 6, 1, 24, KOFF), mixing=mix(116, 0, -12, 0), vibrato=VIB_OFF, voice=TONE, fx=dry,
             main_volume=127, axes={},
             tags=["bass", "muted", "short", "staccato"]),
        seed("Short Triangle", "Bass", "Short", "triangle_loop",
             "Short triangle bass: D6 to SL0 then SR20, a round chiptune pluck.",
             env=adsr(15, 6, 0, 20, KOFF), mixing=mix(120, 0, -12, 0), vibrato=VIB_OFF, voice=TONE, fx=dry,
             main_volume=127, axes={"transpose": [-12, 0]},
             tags=["bass", "chip", "triangle", "short", "staccato"]),
    ]


# ----- Pad --------------------------------------------------------------------------------

def pad_seeds() -> list[Seed]:
    warm = adsr(6, 0, 7, 0, exp_release(12))
    sine = adsr(5, 0, 7, 0, exp_release(12))
    saw = adsr(6, 0, 6, 0, exp_release(14))
    slow_strings = adsr(4, 0, 7, 0, exp_release(14))
    swell_strings = gain(GAIN_BENT_INC, 7, exp_release(14))
    return [
        # Echo: the pad is mostly the echo unit
        seed("Warm Pad", "Pad", "Echo", "pad_warm",
             "Warm detuned saw pad (16 kHz loop): A6 (252 ms) SL7 held, GAIN release 12 (~3.5 s); "
             "echo on every voice is the room, MVOL 110 leaves headroom for the echo sum; Swell is "
             "GAIN bent-line increase rate 8 (~1.3 s).",
             env=warm, mixing=mix(88, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_SHORT, main_volume=110,
             axes={"echo": PAD_ECHO_3, "swell": [("", warm), ("Swell", gain(GAIN_BENT_INC, 8, exp_release(12)))]},
             tags=["pad", "warm", "echo"]),
        seed("Glass Pad", "Pad", "Echo", "pad_glass",
             "Glassy FM pad (16 kHz loop): A7 then D1 to SL6; the Bright echo (N-SPC high-pass) "
             "gives thin shimmering repeats; half-depth 5.4 Hz vibrato for the 16 kHz sample.",
             env=adsr(7, 1, 6, 0, exp_release(13)), mixing=mix(88, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_SHORT, main_volume=110,
             axes={"echo": [("Short", ECHO_SHORT), ("Long", ECHO_LONG), ("Bright", ECHO_BRIGHT)],
                   "vib": [("off", VIB_OFF), ("Vib5Hz", VIB_5HZ_LOW)]},
             tags=["pad", "glass", "echo"]),
        seed("Sine Pad", "Pad", "Echo", "sine_loop",
             "Pure sine pad: A5 (378 ms) SL7; all the colour comes from the echo FIR (low-pass or "
             "high-pass); Swell is GAIN bent-line increase rate 10 (~0.9 s).",
             env=sine, mixing=mix(96, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_LONG, main_volume=110,
             axes={"echo": [("Long", ECHO_LONG), ("Bright", ECHO_BRIGHT), ("Hall", ECHO_HALL)],
                   "swell": [("", sine), ("Swell", gain(GAIN_BENT_INC, 10, exp_release(12)))]},
             tags=["pad", "sine", "echo"]),
        seed("Saw Pad", "Pad", "Echo", "saw_loop",
             "Saw pad from the 11-cycle loop: A6 SL6 with 5.4 Hz vibrato; Swell is GAIN bent-line "
             "increase rate 9 (~1.1 s).",
             env=saw, mixing=mix(80, 0, 0, 0), vibrato=VIB_5HZ, voice=TONE, fx=ECHO_SHORT, main_volume=110,
             axes={"echo": PAD_ECHO_3, "swell": [("", saw), ("Swell", gain(GAIN_BENT_INC, 9, exp_release(14)))]},
             tags=["pad", "saw", "echo"]),
        seed("Triangle Pad", "Pad", "Echo", "triangle_loop",
             "Soft triangle pad: A6 SL7 held; EON Split sends only voices 1/3/5/7 to the echo so, "
             "with round-robin allocation, alternate notes stay dry, a mixing trick of SPC drivers.",
             env=adsr(6, 0, 7, 0, exp_release(12)), mixing=mix(100, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_LONG, main_volume=110,
             axes={"echo": [("Long", ECHO_LONG), ("Hall", ECHO_HALL)],
                   "eon": [("", eon(ALL_ON)), ("EON Split", eon(ODD_ON))]},
             tags=["pad", "triangle", "echo"]),
        # Strings
        seed("Slow Strings", "Pad", "Strings", "strings_ensemble",
             "Slow string pad: A4 (630 ms) bow, SL7 held, 5.4 Hz vibrato (half depth for the 16 kHz "
             "sample), GAIN release 14 (~2 s); Slower uses A2 (1.5 s).",
             env=slow_strings, mixing=mix(92, 0, 0, 0), vibrato=VIB_5HZ_LOW, voice=TONE, fx=ECHO_SHORT,
             main_volume=110,
             axes={"slower": [("", slow_strings), ("Slower", adsr(2, 0, 7, 0, exp_release(12)))],
                   "echo": PAD_ECHO_3},
             tags=["pad", "strings", "orchestral"]),
        seed("Swell Strings", "Pad", "Strings", "strings_ensemble",
             "Crescendo strings: GAIN bent-line increase rate 7 (~1.8 s, fast then slower near the "
             "top) instead of an ADSR attack, the way drivers fade strings in; Slow is rate 4 (~3.6 s).",
             env=swell_strings, mixing=mix(92, 0, 0, 0), vibrato=VIB_DELAYED_LOW, voice=TONE, fx=ECHO_LONG,
             main_volume=110,
             axes={"slow": [("", swell_strings), ("Slow", gain(GAIN_BENT_INC, 4, exp_release(12)))],
                   "echo": [("Long", ECHO_LONG), ("Hall", ECHO_HALL)]},
             tags=["pad", "strings", "swell"]),
        seed("Saw Strings", "Pad", "Strings", "saw_loop",
             "Synth strings from the raw saw: A5 SL6 with delayed vibrato for movement; OctDown is "
             "the low section, with half the vibrato depth so the width in cents stays the same.",
             env=adsr(5, 0, 6, 0, exp_release(14)), mixing=mix(80, 0, 0, 0), vibrato=VIB_DELAYED, voice=TONE,
             fx=ECHO_SHORT, main_volume=110,
             axes={"echo": [("Short", ECHO_SHORT), ("Long", ECHO_LONG), ("Bright", ECHO_BRIGHT)],
                   "oct": [("", {}), ("OctDown", {"transpose": -12, "vibrato_depth": 24})]},
             tags=["pad", "strings", "synth"]),
        seed("Low Strings", "Pad", "Strings", "strings_ensemble",
             "Cellos and basses an octave below the keys: A5 SL7 under a long or hall echo, as a "
             "sustained floor under the harmony; vibrato at quarter depth for the 16 kHz sample an "
             "octave down.",
             env=adsr(5, 0, 7, 0, exp_release(13)), mixing=mix(96, 0, -12, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_LONG, main_volume=110,
             axes={"echo": [("Long", ECHO_LONG), ("Hall", ECHO_HALL)],
                   "vib": [("off", VIB_OFF), ("Vib5Hz", VIB_5HZ_LOW2)]},
             tags=["pad", "strings", "orchestral", "low", "octave"]),
        # Choir
        seed("Choir Ah", "Pad", "Choir", "choir_ah",
             "Choir 'ah': A5 (378 ms) SL7 held on the formant loop, GAIN release 13; 5.4 Hz vibrato "
             "for a massed-voices wobble.",
             env=adsr(5, 0, 7, 0, exp_release(13)), mixing=mix(96, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_SHORT, main_volume=110,
             axes={"echo": PAD_ECHO_3, "vib": [("off", VIB_OFF), ("Vib5Hz", VIB_5HZ)]},
             tags=["pad", "choir", "vocal"]),
        seed("Choir Oo", "Pad", "Choir", "choir_oo",
             "Choir 'oo': A6 SL7, darker vowel; delayed vibrato only blooms on long chords.",
             env=adsr(6, 0, 7, 0, exp_release(12)), mixing=mix(100, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_SHORT, main_volume=110,
             axes={"echo": PAD_ECHO_3, "vib": [("off", VIB_OFF), ("VibDelayed", VIB_DELAYED)]},
             tags=["pad", "choir", "vocal"]),
        seed("Swell Choir", "Pad", "Choir", "choir_ah",
             "Choir swelling in with GAIN bent-line increase rate 12 (~560 ms, fast then slower near "
             "the top like a breath), long echo; OctDown for male voices with half the vibrato depth.",
             env=gain(GAIN_BENT_INC, 12, exp_release(12)), mixing=mix(96, 0, 0, 0), vibrato=VIB_5HZ, voice=TONE,
             fx=ECHO_LONG, main_volume=110,
             axes={"echo": [("Long", ECHO_LONG), ("Hall", ECHO_HALL)],
                   "oct": [("", {}), ("OctDown", {"transpose": -12, "vibrato_depth": 20})]},
             tags=["pad", "choir", "vocal", "swell"]),
        seed("Dark Choir", "Pad", "Choir", "choir_oo",
             "Low 'oo' choir an octave down through a band-passed room or a low-passed hall echo, "
             "A4 slow onset; EON Split keeps every other voice dry for clarity.",
             env=adsr(4, 0, 7, 0, exp_release(12)), mixing=mix(104, 0, -12, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_ROOM, main_volume=110,
             axes={"echo": [("Room", ECHO_ROOM), ("Hall", ECHO_HALL)],
                   "eon": [("", eon(ALL_ON)), ("EON Split", eon(ODD_ON))]},
             tags=["pad", "choir", "vocal", "dark"]),
    ]


# ----- Drums ------------------------------------------------------------------------------

def drum_seeds() -> list[Seed]:
    full_short = [("", DRUM_FULL), ("Short", DRUM_SHORT)]
    full_choke = [("", DRUM_FULL), ("Choke", DRUM_CHOKE)]
    dry = ECHO_OFF  # drums stay out of the echo (EON off on every voice)

    def kit(name: str, sample: str, comment: str, pan: int, axes: Mapping[str, Sequence[Any]],
            tags: Sequence[str]) -> Seed:
        return seed(name, "Drums", "Kit", sample, comment, env=DRUM_FULL, mixing=mix(110, pan, 0, 0),
                    vibrato=VIB_OFF, voice=DRUM_SOURCE, fx=dry, main_volume=127, axes=axes,
                    tags=["drums", *tags])

    def dynamic(name: str, sample: str, comment: str, pan: int, tags: Sequence[str]) -> Seed:
        return seed(name, "Drums", "Velocity", sample, comment, env=DRUM_FULL, mixing=mix(110, pan, 0, 0),
                    vibrato=VIB_OFF, voice=DRUM_SOURCE, fx=dry, main_volume=127, axes={"note": LAYERS},
                    tags=["drums", "velocity", *tags])

    return [
        kit("Kick", "kick",
            "One-shot kick, forced one-shot, A15 D7 SL7 so the sample plays untouched; pitch +/-5 "
            "semitones retunes it, Short cuts the boom with D5 to SL0.",
            0, {"transpose": [0, -5, 5], "short": full_short}, ["kick"]),
        kit("Snare", "snare",
            "Snare, dry and centred; transpose +/-4 moves the body tone, Short tightens the noise tail.",
            0, {"transpose": [0, -4, 4], "short": full_short}, ["snare"]),
        kit("Rim", "snare_rim",
            "Snare cross-stick click, short by nature; a fourth down for a deeper crack, a fifth up "
            "for a woodblock-like tick.",
            0, {"transpose": [0, -5, 7]}, ["snare", "rim"]),
        kit("Closed Hat", "hat_closed",
            "Closed hi-hat, 90 ms at 32 kHz, on the drummer's left; a fourth down for a darker hat, "
            "a fifth up for a smaller cymbal.",
            PAN_HAT, {"transpose": [0, -5, 7]}, ["hihat", "cymbal"]),
        kit("Open Hat", "hat_open",
            "Open hi-hat on the drummer's left; Choke uses hardware KOFF (8 ms) so a note off closes "
            "the hat, three semitones down for a washier hat.",
            PAN_HAT, {"choke": full_choke, "transpose": [0, -3]}, ["hihat", "cymbal"]),
        kit("High Tom", "tom_high",
            "High tom left of centre (drummer's view); a fourth down or Short (SL0 SR24) for tight fills.",
            PAN_TOM_HIGH, {"transpose": [0, -5], "short": full_short}, ["tom"]),
        kit("Mid Tom", "tom_mid",
            "Mid tom right of centre (drummer's view, between the high and the floor tom); same pitch "
            "and decay variants as the high tom for a matched set.",
            PAN_TOM_MID, {"transpose": [0, -5], "short": full_short}, ["tom"]),
        kit("Low Tom", "tom_low",
            "Low tom on the drummer's right; a fourth down turns it into a floor tom.",
            PAN_TOM_LOW, {"transpose": [0, -5], "short": full_short}, ["tom"]),
        kit("Clap", "clap",
            "Hand clap, dry and centred; a fourth down for a bigger clap, a major third up for a "
            "tight snap.",
            0, {"transpose": [0, -5, 4]}, ["clap"]),
        kit("Crash", "crash",
            "Crash cymbal, 800 ms one-shot at 22 kHz, on the drummer's left; Choke (KOFF) grabs the "
            "cymbal on note off, pitch down for a China-like wash.",
            PAN_CRASH, {"choke": full_choke, "transpose": [0, -5]}, ["crash", "cymbal"]),
        kit("Ride", "ride",
            "Ride (light stick hit on a suspended cymbal) on the drummer's right; Choke for muted "
            "ride patterns, three semitones up for a lighter ride.",
            PAN_RIDE, {"choke": full_choke, "transpose": [0, 3]}, ["ride", "cymbal"]),
        kit("Cowbell", "cowbell",
            "Cowbell; a fifth down for an agogo-like low bell, Short to shorten the ring.",
            0, {"transpose": [0, -7], "short": full_short}, ["cowbell", "percussion"]),
        kit("Shaker", "shaker",
            "Small shaker stroke, centred; up a fourth for a smaller shaker, down a fourth for a "
            "larger one.",
            0, {"transpose": [0, 5, -5]}, ["shaker", "percussion"]),
        kit("Timpani", "timpani",
            "Orchestral timpani hit (0.9 s): keys retune it like the pedal, a fourth down for the "
            "low drum, a fourth up for the high one; the one-shot rings out untouched.",
            0, {"transpose": [0, -5, 5]}, ["timpani", "orchestral", "percussion"]),
        kit("Conga", "conga",
            "Open conga tone, a little right of centre; a fourth down for a tumba, a fourth up for "
            "a quinto.",
            PAN_TOM_MID, {"transpose": [0, -5, 5]}, ["conga", "percussion"]),
        kit("Tambourine", "tambourine",
            "Tambourine hit at 32 kHz on the drummer's right; Short (SL0 SR24) cuts the jingles for "
            "16th-note patterns.",
            PAN_RIDE, {"short": full_short}, ["tambourine", "percussion"]),
        dynamic("Dynamic Kick", "kick",
                "Kick layers the way a driver writes accents and ghost notes: the accent a semitone "
                "up at VxVOL 127, the ghost two semitones down, short (SL0 SR24) at VxVOL 56.",
                0, ["kick"]),
        dynamic("Dynamic Snare", "snare",
                "Snare layers: accent a semitone up at full level, ghost note two semitones down, "
                "short and quiet.", 0, ["snare"]),
        dynamic("Dynamic Hat", "hat_closed",
                "Closed hat layers for 8th/16th patterns with accents, on the kit's hat side.",
                PAN_HAT, ["hihat", "cymbal"]),
        dynamic("Dynamic Tom", "tom_mid",
                "Mid tom layers for dynamic fills, at the mid tom's kit position.", PAN_TOM_MID, ["tom"]),
    ]


# ----- SFX --------------------------------------------------------------------------------

def sfx_seeds() -> list[Seed]:
    looped_noise = "noise_white_loop"  # keeps the noise voice alive: the looped BRR never hits an end code
    pmon_wobble = gain(GAIN_DIRECT, 40, KOFF)
    return [
        # Noise: hardware LFSR noise (NON), pitch comes from noise_clock only
        seed("Noise Burst", "SFX", "Noise", looped_noise,
             "Hardware noise hit: A15 D4 to SL0 then SR20, noise clock sets the colour (16 = 500 Hz "
             "rumble, 24 = 3.2 kHz hiss, 30 = 16 kHz sizzle); keys do not change noise pitch.",
             env=adsr(15, 4, 0, 20, KOFF), mixing=mix(96, 0, 0, 0), vibrato=VIB_OFF, voice=noise_source(24),
             fx=ECHO_OFF, main_volume=127,
             axes={"noise_clock": [16, 24, 30], "echo": ECHO_2},
             tags=["sfx", "noise", "hit"]),
        seed("Wind", "SFX", "Noise", looped_noise,
             "Wind: A3 (1 s) fade-in of mid-rate noise (clock 14..22, 333 Hz..2 kHz), held, GAIN "
             "release 12 (~3.5 s) so it dies away; optional long echo.",
             env=adsr(3, 0, 7, 0, exp_release(12)), mixing=mix(80, 0, 0, 0), vibrato=VIB_OFF, voice=noise_source(18),
             fx=ECHO_OFF, main_volume=127,
             axes={"noise_clock": [14, 18, 22], "echo": [("off", ECHO_OFF), ("Long", ECHO_LONG)]},
             tags=["sfx", "noise", "ambience"]),
        seed("Explosion", "SFX", "Noise", looped_noise,
             "Explosion: instant attack, D1 to SL3 then SR14 on low noise clocks (8..16, "
             "83..500 Hz) for crunchy rumble; Hall echo for a big blast.",
             env=adsr(15, 1, 3, 14, exp_release(14)), mixing=mix(120, 0, 0, 0), vibrato=VIB_OFF,
             voice=noise_source(12), fx=ECHO_OFF, main_volume=127,
             axes={"noise_clock": [8, 12, 16], "echo": [("off", ECHO_OFF), ("Hall", ECHO_HALL)]},
             tags=["sfx", "noise", "explosion"]),
        seed("Steam", "SFX", "Noise", looped_noise,
             "Steam hiss rising in with GAIN bent-line increase rate 13 (~450 ms) at high noise clocks "
             "(26..31, 5.3..32 kHz); the hiss is at full level when the key is released, so Tail "
             "(GAIN release 20) against the KOFF cut is audible.",
             env=gain(GAIN_BENT_INC, 13, KOFF), mixing=mix(88, 0, 0, 0), vibrato=VIB_OFF, voice=noise_source(29),
             fx=ECHO_OFF, main_volume=127,
             axes={"noise_clock": [26, 29, 31], "release": TAIL},
             tags=["sfx", "noise", "hiss"]),
        seed("Noise Hit", "SFX", "Noise", "noise_filtered",
             "Noise voice on a one-shot 0.3 s sample: the output is LFSR noise but the voice still "
             "steps through the BRR at the key's pitch, so the end code ends the hit after 0.3 s at "
             "C4, 0.15 s at C5: a hardware way to get key-scaled noise hits.",
             env=adsr(15, 7, 7, 0, exp_release(16)), mixing=mix(100, 0, 0, 0), vibrato=VIB_OFF,
             voice=noise_source(20, LOOP_ONE_SHOT), fx=ECHO_OFF, main_volume=127,
             axes={"noise_clock": [20, 28], "echo": ECHO_2},
             tags=["sfx", "noise", "hit"]),
        seed("Rain", "SFX", "Noise", looped_noise,
             "Rain: quiet top-rate noise (clock 28..31) with A12 and SL7 held, through a long "
             "low-passed or a bright high-passed echo for a diffuse wash.",
             env=adsr(12, 7, 7, 0, exp_release(16)), mixing=mix(64, 0, 0, 0), vibrato=VIB_OFF, voice=noise_source(31),
             fx=ECHO_LONG, main_volume=127,
             axes={"noise_clock": [28, 31], "echo": [("Long", ECHO_LONG), ("Bright", ECHO_BRIGHT)]},
             tags=["sfx", "noise", "ambience"]),
        # PMON: hold two notes; the pairing depends on voice allocation (module docstring)
        seed("PMON Bell", "SFX", "PMON", "sine_loop",
             "Sine pair with PMON: while two notes are held, the voice keyed first frequency-modulates "
             "the next one into an inharmonic bell (a lone note plays a plain sine); A15 D3 SL2 SR14 "
             "decays the modulator too so the timbre mellows, GAIN release 24 so a finished note does "
             "not colour the next; OctDown for low gongs.",
             env=adsr(15, 3, 2, 14, exp_release(24)), mixing=mix(100, 0, 0, 0), vibrato=VIB_OFF, voice=PMON_SOURCE,
             fx=ECHO_OFF, main_volume=127,
             axes={"echo": ECHO_3, "transpose": [0, -12]},
             tags=["sfx", "pmon", "bell"]),
        seed("PMON Wobble", "SFX", "PMON", "triangle_loop",
             "Triangle PMON with direct GAIN: E = value x 16 fixes the modulator level, so Light (40) "
             "and Deep (127) are the modulation index; the 31 Hz driver vibrato adds a warble; KOFF.",
             env=pmon_wobble, mixing=mix(110, 0, 0, 0), vibrato=VIB_OFF, voice=PMON_SOURCE, fx=ECHO_OFF,
             main_volume=127,
             axes={"depth": [("Light", pmon_wobble), ("Deep", gain(GAIN_DIRECT, 127, KOFF))],
                   "vib": [("off", VIB_OFF), ("VibDeep", VIB_DEEP)]},
             tags=["sfx", "pmon", "wobble"]),
        seed("PMON Growl", "SFX", "PMON", "saw_loop",
             "Saw modulating saw one or two octaves down: gate envelope with KOFF, a harsh growl for "
             "engines and monsters.",
             env=adsr(15, 7, 7, 0, KOFF), mixing=mix(88, 0, -12, 0), vibrato=VIB_OFF, voice=PMON_SOURCE,
             fx=ECHO_OFF, main_volume=127,
             axes={"transpose": [-12, -24], "echo": ECHO_2},
             tags=["sfx", "pmon", "growl"]),
        seed("PMON Trill", "SFX", "PMON", "square_loop",
             "Square PMON pair with a 31 Hz deep or a 6.9 Hz driver vibrato on both voices: the "
             "modulation ratio trills and buzzes (the driver vibrato cannot sweep slowly enough for "
             "a siren); GAIN release 24.",
             env=adsr(12, 7, 7, 0, exp_release(24)), mixing=mix(84, 0, 0, 0), vibrato=VIB_DEEP, voice=PMON_SOURCE,
             fx=ECHO_OFF, main_volume=127,
             axes={"vib": [("VibDeep", VIB_DEEP), ("Vib7Hz", VIB_7HZ)],
                   "echo": [("off", ECHO_OFF), ("Long", ECHO_LONG)]},
             tags=["sfx", "pmon", "trill", "buzz"]),
        seed("PMON Glass", "SFX", "PMON", "pad_glass",
             "Glass pad through PMON for a detuned, metallic shimmer on held chords; the 16 kHz "
             "sample leaves room for OctUp (pitch ceiling three octaves above the root); GAIN "
             "release 24, the echo carries the tail.",
             env=adsr(8, 0, 7, 0, exp_release(24)), mixing=mix(92, 0, 0, 0), vibrato=VIB_OFF, voice=PMON_SOURCE,
             fx=ECHO_LONG, main_volume=110,
             axes={"echo": [("Long", ECHO_LONG), ("Hall", ECHO_HALL)], "transpose": [0, 12]},
             tags=["sfx", "pmon", "glass", "echo"]),
        seed("PMON Metal", "SFX", "PMON", "organ_full",
             "Organ loop modulating organ: dense sidebands for clanging metal; A15 D5 SL3 SR16 so hits "
             "ring then fade, GAIN release 24; a fifth up or a band-passed room echo.",
             env=adsr(15, 5, 3, 16, exp_release(24)), mixing=mix(96, 0, 0, 0), vibrato=VIB_OFF, voice=PMON_SOURCE,
             fx=ECHO_OFF, main_volume=127,
             axes={"transpose": [0, 7], "echo": [("off", ECHO_OFF), ("Room", ECHO_ROOM)]},
             tags=["sfx", "pmon", "metal"]),
    ]


def get_seeds() -> list[Seed]:
    return instrument_seeds() + bass_seeds() + pad_seeds() + drum_seeds() + sfx_seeds()
