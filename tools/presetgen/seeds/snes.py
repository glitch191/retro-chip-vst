"""SNES (S-DSP) seed patches.

Written the way an SPC700 composer of the Nintendo / Square era builds an instrument set
(docs/research/snes-sound-design.md has the references and the per-category targets):
every instrument is one BRR sample plus an entry of the driver's instrument table
(SRCN, ADSR1, ADSR2, GAIN, tuning; SnesLab "N-SPC Engine", AddmusicK `#instruments`), a
voice volume and a moderate pan, the driver vibrato, and the global echo unit with a
per-voice EON mask. Values are raw register values (docs/ENGINE_SPECS.md, SNES section;
timings from the rate table in docs/research/snes.md, 32 kHz output):

* attack A (0..14): rate 2A+1, 64 steps of +32 -> A5 384 ms, A7 160 ms, A9 64 ms, A10 40 ms,
  A11 24 ms, A12 16 ms, A14 6 ms; A15 is instant (two +1024 steps).
* decay D (0..7): rate 2D+16, exponential down to sustain level SL, (SL + 1) / 8 of full
  scale (SL5 -2.5 dB, SL4 -4.1 dB, SL3 -6 dB, SL2 -8.5 dB). From full scale to SL5 / SL3:
  D1 86 / 204 ms, D2 52 / 122 ms, D3 34 / 82 ms, D4 22 / 51 ms, D5 13 / 31 ms.
* sustain rate SR (0..31): exponential fade after decay, 0 = hold. Time to -30 dB below
  SL5: SR12 2.9 s, SR13 2.3 s, SR14 1.7 s, SR15 1.45 s, SR16 1.2 s, SR17 0.87 s,
  SR18 0.72 s, SR19 0.58 s, SR20 0.43 s, SR22 0.29 s, SR24 0.18 s.
* release_mode 0 = hardware KOFF (-8 per sample, 8 ms: what N-SPC does at the end of every
  note); 1 = driver GAIN exponential decrease at release_rate, written at note off (same
  rate table and step as SR; -40 dB from full scale in: R14 2.0 s, R15 1.7 s, R17 1.0 s,
  R19 0.68 s, R20 0.51 s, R21 0.42 s, R22 0.34 s, R23 0.25 s, R24 0.21 s, R26 0.13 s).
  All these times were simulated with the hardware step E -= ((E - 1) >> 8) + 1 and match
  the chiptool renders within 10 ms.
* GAIN linear increase is the ADSR attack curve (+32 per step, 64 steps) at any rate, so it
  is not used for swells; bent-line increase (+32 up to 0x600, then +8: 112 steps) is the
  shape ADSR cannot make and is what every "Swell" here uses (rate 15 ~280 ms, 12 ~560 ms,
  9 ~1.1 s, 7 ~1.8 s). Direct GAIN fixes E = value x 16.
* echo_delay EDL x 16 ms (buffer EDL x 2 KiB of the 64 KiB ARAM), echo_feedback EFB and
  echo_volume EVOL are signed 8-bit.
* noise_clock is the FLG noise rate (32000 / period of the same rate table: 8 -> 83 Hz,
  16 -> 500 Hz, 22 -> 2 kHz, 24 -> 3.2 kHz, 30 -> 16 kHz, 31 -> 32 kHz). A noise voice still
  steps through its BRR sample at the key's pitch, so the sample's end code still ends the
  note: sustained noise uses the looped noise sample.

Envelope families (N-SPC practice, see the sound-design notes): plucked and struck sounds
start at A15; pianos, guitars and mallets fall with D to a sustain level then fade on SR
while the key is held; bowed strings, winds, brass and organs hold at SL7/SR0 (or a small
D/SL accent) and end with a driver GAIN release so the note does not stop in 8 ms; drums
and SFX keep the hardware KOFF or a short GAIN release.

Driver vibrato (engine parameters, ENGINE_SPECS): `vibrato_rate` counts 4 ms ticks per half
cycle (0..63): rate 23 is 5.4 Hz, 21 is 5.95 Hz, 18 is 6.9 Hz, 4 is 31 Hz. `vibrato_depth`
is in pitch-register units, so its size in cents depends on the pitch register value P
(P = 0x1000 at a 32 kHz sample's root key). `vib()` takes the depth in cents at a
reference key of the seed's range and converts it with the sample's rate and root note
from assets/samples/index.json, so one profile means the same width on every sample. The
width then halves per octave above the reference key and doubles per octave below, as a
fixed register depth does on the hardware. Samples recorded with vibrato (strings
ensemble, flute, trumpet) keep driver vibrato off by default.

Pitch ceiling: the 14-bit pitch register stops at 0x3FFF, two octaves above the root of a
32 kHz sample, three above a 16 kHz one. Melodic seeds never transpose 32 kHz A3-rooted
samples up an octave; octave-up variants exist only on 16 kHz samples.

Echo (melodic instruments and pads; drums and basses stay dry with EON off): profiles use
EDL 3..6 (48..96 ms), EFB 0x28..0x60 and EVOL 0x28..0x40, the documented range of game
settings (sound-design notes), through the documented coefficient sets only: N-SPC
low-pass 5 kHz ("Low-pass soft"), the official manual low-pass ("Low-pass strong"), N-SPC
band-pass and N-SPC high-pass (docs/research/snes.md "Echo", Ambiguity 14). The largest of
their gains is +0.91 dB (x1.11), so EFB <= 0x60 (0.75) keeps EFB x max|H| < 0.84 and every
tail decays. The in-house sets (Comb, Bright, Dark) are not used by any seed. There is a
single EVOL register pair in the engine, so the echo is centred. SFX keep a few longer
echoes as effects (up to EDL 12 with EFB 0x68: 0.81 x 1.08 = 0.88, still decaying).

Levels: a sustained tone through an echo with feedback adds up coherently on some notes
(up to EVOL / (1 - EFB x |H|) above the dry level), so sustained and looped seeds carry a
lower VxVOL than plucked ones. Every preset was rendered at C3, G3, C4, G4 and C5: at
velocity 100 the loudest of these peaks stays at or below -3 dBFS and velocity 127 does
not clip; at C4 the peak is -12 dBFS or louder except for the ghost-note drum layers
(8 dB under the kit hit by design) and the shaker, whose high-frequency content the
Gaussian interpolation attenuates.

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

Recorded samples: the pianos, electric piano, strings, brass, winds, pipe organ, upright
bass and the drum kit are CC0 recordings converted by tools/samplegen/cc0_import.py
(sources in tools/samplegen/cc0_manifest.json), most melodic ones at 16 kHz, the bright
piano and the trumpet at 24 kHz, the upright bass at 11 kHz; guitars, organs other than
the pipe organ, choirs, synth pads and single-cycle waves are procedural.

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
import math
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
# fir_preset values: 0, 1, 3 and 4 are the N-SPC standard sets, 2 is the official manual's
# low-pass example; 5, 6 and 7 are in-house designs that no seed uses.
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
def _snes_index() -> tuple[dict[str, Any], ...]:
    with open(SAMPLE_INDEX, "r", encoding="utf-8") as fh:
        entries = json.load(fh)
    return tuple(entry for entry in entries if entry.get("chip") == "snes")


@lru_cache(maxsize=1)
def sample_entries() -> tuple[tuple[str, bool], ...]:
    """(sample name, is a drum-kit sample) in the order of the snes list of index.json."""
    return tuple((str(entry["name"]), entry.get("category") == "drums") for entry in _snes_index())


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


def pitch_register(sample: str, key: int) -> float:
    """Pitch register value P of `sample` played at MIDI `key` (0x1000 = stored rate 32 kHz)."""
    entry = next(e for e in _snes_index() if e["name"] == sample)
    return 0x1000 * float(entry["sample_rate"]) / 32000.0 * 2.0 ** ((key - int(entry["root_note"])) / 12.0)


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


VIB_OFF = {"vibrato_rate": 0, "vibrato_depth": 0, "vibrato_delay": 0}


def vib(sample: str, cents: float, rate: int, delay: int, ref_key: int = 64,
        transpose: int = 0) -> dict[str, int]:
    """Driver vibrato of +/- `cents` at MIDI `ref_key` (+ `transpose`) on `sample`.

    `rate` is in 4 ms ticks per half cycle, `delay` in ticks after key-on. The depth is
    stored in pitch-register units (module docstring), clamped to the engine's 1..64.
    """
    p = pitch_register(sample, ref_key + transpose)
    depth = int(round(p * (2.0 ** (cents / 1200.0) - 1.0)))
    return {"vibrato_rate": rate, "vibrato_depth": max(1, min(64, depth)), "vibrato_delay": delay}


# Vibrato rates and delays (ticks of 4 ms)
VIB_RATE_5HZ = 23   # 5.4 Hz
VIB_RATE_6HZ = 21   # 5.95 Hz
VIB_RATE_7HZ = 18   # 6.9 Hz (organ rotary shimmer)
VIB_SHORT = 30      # 120 ms: winds and brass
VIB_BOW = 40        # 160 ms: bowed strings
VIB_PAD = 50        # 200 ms: pads and choirs
VIB_DEEP = {"vibrato_rate": 4, "vibrato_depth": 64, "vibrato_delay": 0}  # 31 Hz warble for SFX


def source(noise_enable: int, noise_clock: int, pmon: int, loop_override: int) -> dict[str, int]:
    return {"noise_enable": noise_enable, "noise_clock": noise_clock, "pmon": pmon,
            "loop_override": loop_override}


def eon(mask: Sequence[int]) -> dict[str, int]:
    return {f"v{voice + 1}_echo": bit for voice, bit in enumerate(mask)}


def echo(delay: int, feedback: int, volume: int, fir: int, mask: Sequence[int] = ALL_ON) -> dict[str, int]:
    return {"echo_enable": 1, "echo_delay": delay, "echo_feedback": feedback, "echo_volume": volume,
            "fir_preset": fir, **eon(mask)}


# ----- named profiles ---------------------------------------------------------------------

ECHO_OFF = {"echo_enable": 0, "echo_delay": 0, "echo_feedback": 0, "echo_volume": 0,
            "fir_preset": FIR_PASS, **eon(ALL_OFF)}

# Melodic instruments: the base preset carries the standard echo, variants the room and the
# deep echo. All inside EDL 3..6, EFB 0x28..0x60, EVOL 0x28..0x40.
ECHO_ROOM = echo(3, 0x28, 0x28, FIR_LP_SOFT)      # 48 ms, EFB 0.31: two quick repeats, a small room
ECHO_STD = echo(4, 0x40, 0x30, FIR_LP_SOFT)       # 64 ms, EFB 0.5: the everyday SPC echo
ECHO_DEEP = echo(6, 0x58, 0x30, FIR_LP_STRONG)    # 96 ms, EFB 0.69: long darkening tail
INSTRUMENT_ECHO = [("", ECHO_STD), ("Room", ECHO_ROOM), ("Deep", ECHO_DEEP)]
INSTRUMENT_ECHO_2 = [("", ECHO_STD), ("Deep", ECHO_DEEP)]

# Pads: always wet, a little longer and louder.
PAD_ECHO = echo(5, 0x50, 0x30, FIR_LP_SOFT)       # 80 ms, EFB 0.63
PAD_DEEP = echo(6, 0x60, 0x34, FIR_LP_STRONG)     # 96 ms, EFB 0.75, the darkest documented low-pass
PAD_BAND = echo(5, 0x48, 0x30, FIR_BAND)          # 80 ms through the N-SPC 1.5-8.5 kHz band-pass
PAD_ECHOES = [("", PAD_ECHO), ("Deep", PAD_DEEP), ("Band", PAD_BAND)]
PAD_ECHOES_2 = [("", PAD_ECHO), ("Deep", PAD_DEEP)]
PAD_ECHOES_BAND = [("", PAD_ECHO), ("Band", PAD_BAND)]

# SFX only: effect echoes outside the musical range.
SFX_SHORT = echo(3, 0x20, 0x2C, FIR_LP_SOFT)       # 48 ms
SFX_ROOM = echo(4, 0x28, 0x28, FIR_BAND)           # 64 ms band-passed (N-SPC) room
SFX_LONG = echo(8, 0x54, 0x30, FIR_LP_SOFT)        # 128 ms, long low-passed tail
SFX_BRIGHT = echo(10, 0x5A, 0x2C, FIR_HIGH)        # 160 ms thin high-passed (N-SPC 3 kHz) repeats
SFX_HALL = echo(12, 0x68, 0x34, FIR_LP_SOFT)       # 192 ms, EFB 0.81: very long tail
SFX_ECHO_2 = [("off", ECHO_OFF), ("Short", SFX_SHORT)]

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
# semitones down, short and 8 dB under the kit hit (VxVOL 72 against 127).
LAYERS = [("Accent Note", {"volume": 127, "transpose": 1}),
          ("Ghost Note", {"volume": 72, "transpose": -2, **DRUM_SHORT})]
TAIL = [("", KOFF), ("Tail", exp_release(20))]

# Kit panning, drummer's view throughout: hats and crash left, ride and floor tom right.
PAN_HAT, PAN_CRASH, PAN_RIDE = -24, -24, 24
PAN_TOM_HIGH, PAN_TOM_MID, PAN_TOM_LOW = -16, 8, 24

# Instrument panning, an orchestral seating seen from the audience, kept moderate (the
# balance law attenuates the far side only): violins and high winds left, cellos, horns and
# low brass right, keys and solo leads near the centre.
PAN_VIOLINS, PAN_CELLOS = -16, 16
PAN_TRUMPET, PAN_HORN, PAN_TROMBONE, PAN_BRASS = 8, 16, 20, 12
PAN_FLUTE, PAN_CLARINET, PAN_OBOE = -12, 8, -8
PAN_GUITAR, PAN_HARP = -12, -20


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
    grand = adsr(15, 3, 5, 14, exp_release(23))
    upright = adsr(15, 2, 5, 15, exp_release(23))
    epiano = adsr(15, 4, 4, 16, exp_release(21))
    strings = adsr(9, 7, 7, 0, exp_release(19))
    trumpet = adsr(12, 5, 6, 0, exp_release(21))
    trombone = adsr(12, 4, 5, 0, exp_release(20))
    nylon = adsr(15, 3, 3, 16, exp_release(20))
    steel = adsr(15, 2, 4, 14, exp_release(18))
    flute = adsr(10, 7, 7, 0, exp_release(20))
    organ = adsr(15, 7, 7, 0, exp_release(26))
    return [
        # Piano: A15 hammer, D to a sustain level, SR is the string decay, GAIN release the damper
        seed("Grand Piano", "Instrument", "Piano", "piano_bright",
             "Steinway B grand (24 kHz, 57 ms sustain loop of 15 whole periods): A15 hammer, D3 to "
             "SL5 (-2.5 dB, 34 ms) then SR14 (-30 dB in 1.7 s) as the string decay, GAIN release 23 "
             "(-40 dB in 0.25 s) as the damper; Long uses D1 SL6 SR12 (~3 s) for ballads.",
             env=grand, mixing=mix(104, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_STD, main_volume=127,
             axes={"decay": [("", grand), ("Long", adsr(15, 1, 6, 12, exp_release(21)))],
                   "echo": INSTRUMENT_ECHO},
             tags=["piano", "keys"]),
        seed("Upright Piano", "Instrument", "Piano", "piano_soft",
             "Soft-touch upright (16 kHz, 109 ms loop): A15 D2 to SL5 (52 ms) then SR15 (-30 dB in "
             "1.45 s), damper GAIN 23; Bell uses D5 to SL3 and SR16 for a music-box like ping.",
             env=upright, mixing=mix(108, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_STD, main_volume=127,
             axes={"tone": [("", upright), ("Bell", adsr(15, 5, 3, 16, exp_release(22)))],
                   "echo": INSTRUMENT_ECHO_2},
             tags=["piano", "keys"]),
        seed("EPiano", "Instrument", "Piano", "epiano",
             "Recorded TX81Z FM piano (16 kHz loop): A15, D4 to SL4 (the tine bark, 35 ms), SR16 "
             "fade; the 5.95 Hz driver vibrato (+/-8 cents) stands in for the chorus of the real "
             "instrument.",
             env=epiano, mixing=mix(104, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_STD, main_volume=127,
             axes={"vib": [("off", VIB_OFF), ("Vib", vib("epiano", 8, VIB_RATE_6HZ, VIB_SHORT))],
                   "echo": INSTRUMENT_ECHO_2},
             tags=["epiano", "keys"]),
        seed("Vibraphone", "Instrument", "Piano", "epiano",
             "Mallet keys from the FM piano: A15 D2 SL5 SR13 (ADSR1 $AF, ADSR2 $AD: a quick drop "
             "then a 2.3 s ring), a +/-14 cents 5.4 Hz vibrato from the start as the motor, deep echo.",
             env=adsr(15, 2, 5, 13, exp_release(20)), mixing=mix(100, -8, 0, 0),
             vibrato=vib("epiano", 14, VIB_RATE_5HZ, 0, 67), voice=TONE, fx=ECHO_DEEP, main_volume=127,
             axes={"echo": [("", ECHO_DEEP), ("Std", ECHO_STD)]},
             tags=["mallet", "vibraphone", "keys"]),
        # Strings: sustain with a GAIN release
        seed("Ensemble Strings", "Instrument", "Strings", "strings_ensemble",
             "Violin and cello sections (16 kHz, vibrato in the recording): A9 (64 ms) bow, SL7 "
             "held, GAIN release 19 (0.68 s); Legato is A7 (160 ms) with release 17 (1 s); violins left.",
             env=strings, mixing=mix(100, PAN_VIOLINS, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_STD,
             main_volume=127,
             axes={"bow": [("", strings), ("Legato", adsr(7, 7, 7, 0, exp_release(17)))],
                   "echo": INSTRUMENT_ECHO},
             tags=["strings", "orchestral"]),
        seed("Cello Section", "Instrument", "Strings", "strings_ensemble",
             "The section an octave down (P 0x400 at A2: the Gaussian interpolation darkens it) as "
             "cellos and basses, right of centre; A9 held, GAIN release 19.",
             env=strings, mixing=mix(104, PAN_CELLOS, -12, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_STD,
             main_volume=127,
             axes={"echo": INSTRUMENT_ECHO},
             tags=["strings", "cello", "orchestral", "low"]),
        seed("Pizzicato", "Instrument", "Strings", "strings_pizz",
             "Violin section pizzicato: A15 D7 SL7 SR0 lets the 0.45 s one-shot carry its own pluck, "
             "GAIN release 22 on note off; an octave down gives cellos and basses.",
             env=adsr(15, 7, 7, 0, exp_release(22)), mixing=mix(108, PAN_VIOLINS, 0, 0), vibrato=VIB_OFF,
             voice=TONE, fx=ECHO_STD, main_volume=127,
             axes={"transpose": [0, -12], "echo": INSTRUMENT_ECHO},
             tags=["strings", "pizzicato", "orchestral"]),
        seed("Harp", "Instrument", "Strings", "strings_pizz",
             "Harp-like pluck from the pizzicato: A15 D2 to SL4 then SR17 lets the string ring a "
             "little past the recorded pluck, GAIN release 20, deep echo, far left like the harp of "
             "an orchestra.",
             env=adsr(15, 2, 4, 17, exp_release(20)), mixing=mix(104, PAN_HARP, 0, 0), vibrato=VIB_OFF,
             voice=TONE, fx=ECHO_DEEP, main_volume=127,
             axes={"echo": [("", ECHO_DEEP), ("Std", ECHO_STD)]},
             tags=["harp", "pluck", "orchestral"]),
        # Brass: sustain, lip accent, GAIN release; bent-line GAIN for swells
        seed("Brass Section", "Instrument", "Brass", "brass_section",
             "Trumpet, horn and trombone section: A11 (24 ms) lip attack, D5 to SL6 held, GAIN "
             "release 20; 5.4 Hz vibrato (+/-12 cents) after 120 ms; Swell is GAIN bent-line rate "
             "15 (~280 ms), the classic SPC brass crescendo.",
             env=adsr(11, 5, 6, 0, exp_release(20)), mixing=mix(100, PAN_BRASS, 0, 0),
             vibrato=vib("brass_section", 12, VIB_RATE_5HZ, VIB_SHORT, 62), voice=TONE, fx=ECHO_STD,
             main_volume=127,
             axes={"swell": [("", adsr(11, 5, 6, 0, exp_release(20))),
                             ("Swell", gain(GAIN_BENT_INC, 15, exp_release(20)))],
                   "echo": INSTRUMENT_ECHO},
             tags=["brass", "orchestral"]),
        seed("Brass Stab", "Instrument", "Brass", "brass_section",
             "Fanfare hits: A15, D1 to SL3 (204 ms, slow enough for the section's ~150 ms onset) then "
             "SR19 so the chord falls away by itself, GAIN release 24.",
             env=adsr(15, 1, 3, 19, exp_release(24)), mixing=mix(124, PAN_BRASS, 0, 0), vibrato=VIB_OFF,
             voice=TONE, fx=ECHO_STD, main_volume=127,
             axes={"echo": INSTRUMENT_ECHO},
             tags=["brass", "stab", "orchestral"]),
        seed("Solo Trumpet", "Instrument", "Brass", "trumpet",
             "Solo trumpet (24 kHz, vibrato in the recording, so driver vibrato off): A12 D5 SL6 "
             "held, GAIN release 21; Soft is A9 (64 ms) with release 19 for legato lines. No stab: "
             "the recording speaks in ~100 ms, so a fast decay would leave only its quiet onset.",
             env=trumpet, mixing=mix(100, PAN_TRUMPET, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_STD,
             main_volume=127,
             axes={"attack": [("", trumpet), ("Soft", adsr(9, 5, 6, 0, exp_release(19)))],
                   "echo": INSTRUMENT_ECHO},
             tags=["brass", "trumpet", "lead"]),
        seed("French Horn", "Instrument", "Brass", "horn",
             "Solo French horn (16 kHz loop): A10 (40 ms) soft lip attack, D5 to SL6 held, GAIN "
             "release 19; 5.4 Hz vibrato (+/-10 cents) after 160 ms; Swell uses bent-line rate 12.",
             env=adsr(10, 5, 6, 0, exp_release(19)), mixing=mix(104, PAN_HORN, 0, 0),
             vibrato=vib("horn", 10, VIB_RATE_5HZ, VIB_BOW, 60), voice=TONE, fx=ECHO_STD, main_volume=127,
             axes={"swell": [("", adsr(10, 5, 6, 0, exp_release(19))),
                             ("Swell", gain(GAIN_BENT_INC, 12, exp_release(19)))],
                   "echo": INSTRUMENT_ECHO},
             tags=["brass", "horn", "orchestral"]),
        seed("Trombone", "Instrument", "Brass", "trombone",
             "Solo tenor trombone (16 kHz loop): A12 D4 SL5 held, GAIN release 20, vibrato "
             "+/-10 cents at 5.4 Hz after 120 ms; Stab is A15 D2 SL4 SR19.",
             env=trombone, mixing=mix(104, PAN_TROMBONE, 0, 0),
             vibrato=vib("trombone", 10, VIB_RATE_5HZ, VIB_SHORT, 55), voice=TONE, fx=ECHO_STD,
             main_volume=127,
             axes={"stab": [("", trombone), ("Stab", adsr(15, 2, 4, 19, exp_release(24)))],
                   "echo": INSTRUMENT_ECHO_2},
             tags=["brass", "trombone", "orchestral"]),
        # Guitar (procedural 0.6 s one-shots)
        seed("Nylon Guitar", "Instrument", "Guitar", "guitar_nylon",
             "Nylon guitar: A15 D3 to SL3 then SR16 lets the plucked string ring for the length of "
             "the sample; Pluck (D5 SL1 SR20) is the short arpeggio variant.",
             env=nylon, mixing=mix(104, PAN_GUITAR, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_STD,
             main_volume=127,
             axes={"pluck": [("", nylon), ("Pluck", adsr(15, 5, 1, 20, exp_release(24)))],
                   "echo": INSTRUMENT_ECHO_2},
             tags=["guitar", "acoustic"]),
        seed("Steel Guitar", "Instrument", "Guitar", "guitar_steel",
             "Steel guitar: A15 D2 SL4 SR14 for a brighter, longer ring; 5.4 Hz finger vibrato "
             "(+/-10 cents) after 160 ms on held notes.",
             env=steel, mixing=mix(100, -PAN_GUITAR, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_STD,
             main_volume=127,
             axes={"vib": [("off", VIB_OFF), ("Vib", vib("guitar_steel", 10, VIB_RATE_5HZ, VIB_BOW))],
                   "echo": INSTRUMENT_ECHO_2},
             tags=["guitar", "acoustic"]),
        seed("Muted Guitar", "Instrument", "Guitar", "guitar_muted",
             "Palm-muted guitar: A15 D6 to SL1 then SR24 and hardware KOFF, a chugging rhythm part "
             "on a 0.15 s one-shot; the room echo doubles it.",
             env=adsr(15, 6, 1, 24, KOFF), mixing=mix(108, PAN_GUITAR, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_ROOM, main_volume=127,
             axes={"echo": [("", ECHO_ROOM), ("Std", ECHO_STD)]},
             tags=["guitar", "muted", "rhythm"]),
        # Woodwind: sustain, breath onset, GAIN release
        seed("Flute", "Instrument", "Woodwind", "flute",
             "Flute (16 kHz, breath and vibrato in the recording): A10 (40 ms) breath onset, SL7 "
             "held, GAIN release 20; Soft uses A7 (160 ms) and release 18 for slow lines.",
             env=flute, mixing=mix(100, PAN_FLUTE, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_STD,
             main_volume=127,
             axes={"soft": [("", flute), ("Soft", adsr(7, 7, 7, 0, exp_release(18)))],
                   "echo": INSTRUMENT_ECHO},
             tags=["woodwind", "flute", "lead"]),
        seed("Piccolo", "Instrument", "Woodwind", "flute",
             "The flute an octave up as a piccolo (16 kHz sample: the pitch ceiling is A6 played, "
             "sounding A7): A11 SL7 held, GAIN release 21, left of centre.",
             env=adsr(11, 7, 7, 0, exp_release(21)), mixing=mix(96, PAN_FLUTE, 12, 0), vibrato=VIB_OFF,
             voice=TONE, fx=ECHO_STD, main_volume=127,
             axes={"echo": INSTRUMENT_ECHO},
             tags=["woodwind", "piccolo", "lead"]),
        seed("Clarinet", "Instrument", "Woodwind", "clarinet",
             "Clarinet (recorded without vibrato, 16 kHz loop): A11 D7 SL7 held with GAIN release "
             "21; Vib adds +/-10 cents at 5.4 Hz after 120 ms.",
             env=adsr(11, 7, 7, 0, exp_release(21)), mixing=mix(88, PAN_CLARINET, 0, 0), vibrato=VIB_OFF,
             voice=TONE, fx=ECHO_STD, main_volume=127,
             axes={"vib": [("off", VIB_OFF), ("Vib", vib("clarinet", 10, VIB_RATE_5HZ, VIB_SHORT))],
                   "echo": INSTRUMENT_ECHO},
             tags=["woodwind", "clarinet", "lead"]),
        seed("Low Clarinet", "Instrument", "Woodwind", "clarinet",
             "The clarinet an octave down for the chalumeau register and bass lines under the "
             "winds: A11 SL7, GAIN release 21, right of centre.",
             env=adsr(11, 7, 7, 0, exp_release(21)), mixing=mix(96, PAN_CLARINET + 8, -12, 0),
             vibrato=VIB_OFF, voice=TONE, fx=ECHO_STD, main_volume=127,
             axes={"echo": INSTRUMENT_ECHO_2},
             tags=["woodwind", "clarinet", "low"]),
        seed("Oboe", "Instrument", "Woodwind", "oboe",
             "Oboe (recorded without vibrato, 16 kHz loop): A11 then D3 to SL5 (~33 ms, -2.4 dB) "
             "for a reed accent before the held tone; vibrato +/-10 cents at 5.4 Hz or a nervous "
             "5.95 Hz, both after 120 ms.",
             env=adsr(11, 3, 5, 0, exp_release(21)), mixing=mix(104, PAN_OBOE, 0, 0),
             vibrato=vib("oboe", 10, VIB_RATE_5HZ, VIB_SHORT, 72), voice=TONE, fx=ECHO_STD, main_volume=127,
             axes={"vib": [("", vib("oboe", 10, VIB_RATE_5HZ, VIB_SHORT, 72)),
                           ("Fast", vib("oboe", 12, VIB_RATE_6HZ, VIB_SHORT, 72))],
                   "echo": INSTRUMENT_ECHO},
             tags=["woodwind", "oboe", "lead"]),
        # Organ: gate at full level, a short GAIN release (the echo carries the tail)
        seed("Pipe Organ", "Instrument", "Organ", "organ_pipe",
             "Recorded pipe organ (open 4-foot stop, 25 ms loop): gate A14 D7 SL7, GAIN release 22 "
             "for the pipe's speech to stop; OctDown gives the 8-foot pitch; the deep echo is the nave.",
             env=adsr(14, 7, 7, 0, exp_release(22)), mixing=mix(96, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=ECHO_DEEP, main_volume=127,
             axes={"transpose": [0, -12], "echo": [("", ECHO_DEEP), ("Std", ECHO_STD)]},
             tags=["organ", "pipe", "keys"]),
        seed("Drawbar Organ", "Instrument", "Organ", "organ_full",
             "Drawbar organ: A15 D7 SL7 SR0 is a gate (full level while held), GAIN release 26 "
             "(0.13 s); Rotary adds a 6.9 Hz +/-8 cents shimmer.",
             env=organ, mixing=mix(92, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_STD, main_volume=127,
             axes={"vib": [("off", VIB_OFF), ("Rotary", vib("organ_full", 8, VIB_RATE_7HZ, 0))],
                   "echo": INSTRUMENT_ECHO_2},
             tags=["organ", "keys"]),
        seed("Perc Organ", "Instrument", "Organ", "organ_perc",
             "Percussive organ: the click is in the sample, the gate envelope holds the loop, GAIN "
             "release 26; Long uses release 19 as a hall tail.",
             env=organ, mixing=mix(96, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_STD, main_volume=127,
             axes={"long": [("", exp_release(26)), ("Long", exp_release(19))], "echo": INSTRUMENT_ECHO_2},
             tags=["organ", "keys", "percussive"]),
        seed("Chip Organ", "Instrument", "Organ", "square_loop",
             "Square-wave organ from an 11-cycle loop: gate envelope, GAIN release 26, volume 64 "
             "because a full-scale square is louder than the recorded samples.",
             env=organ, mixing=mix(64, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=ECHO_STD, main_volume=127,
             axes={"vib": [("off", VIB_OFF), ("Vib", vib("square_loop", 8, VIB_RATE_6HZ, VIB_SHORT))],
                   "echo": INSTRUMENT_ECHO_2},
             tags=["organ", "chip", "square"]),
    ]


# ----- Bass -------------------------------------------------------------------------------

def bass_seeds() -> list[Seed]:
    finger = adsr(15, 2, 5, 16, exp_release(22))
    slap = adsr(15, 4, 3, 18, exp_release(22))
    upright = adsr(15, 3, 5, 15, exp_release(22))
    synth = adsr(15, 3, 3, 20, exp_release(24))
    saw = adsr(15, 4, 5, 0, exp_release(24))
    sine = adsr(14, 7, 7, 0, KOFF)
    triangle = adsr(15, 5, 4, 0, KOFF)
    dry = ECHO_OFF  # basses stay out of the echo (EON off on every voice)
    return [
        # Sample: fixed-length recorded basses (one-shots) and looped waveforms
        seed("Upright Bass", "Bass", "Sample", "bass_upright",
             "Recorded contrabass pizzicato (E2, 11 kHz one-shot, 0.9 s at the root): A15 D3 to SL5 "
             "then SR15 follows the string, GAIN release 22 as the hand damping it; dry and centred; "
             "OctDown reaches the low E1 string.",
             env=upright, mixing=mix(116, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=dry, main_volume=127,
             axes={"transpose": [0, -12]},
             tags=["bass", "acoustic", "upright"]),
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
        seed("Synth Bass", "Bass", "Sample", "bass_synth",
             "Filtered saw synth bass, a fixed-length 0.4 s one-shot that is still loud at its end: "
             "A15 D3 SL3 SR20 fades it to about -27 dB by then so the end code does not click; "
             "Pluck for a sequenced 16th-note bass.",
             env=synth, mixing=mix(112, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=dry, main_volume=127,
             axes={"pluck": [("", synth), ("Pluck", adsr(15, 6, 1, 18, exp_release(24)))], "transpose": [0, -12]},
             tags=["bass", "synth"]),
        seed("Saw Bass", "Bass", "Sample", "saw_loop",
             "Raw looped saw one octave down or at key pitch: A15 D4 to SL5 held, GAIN release 24 "
             "(0.21 s) so notes do not click; Pluck adds SR16 for a fading note.",
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
        seed("Short Upright", "Bass", "Short", "bass_upright",
             "Staccato upright: A15 D5 to SL1 then SR22, the thump of a slapped jazz bass, gone in "
             "~0.25 s; KOFF.",
             env=adsr(15, 5, 1, 22, KOFF), mixing=mix(116, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=dry,
             main_volume=127, axes={"transpose": [0, -12]},
             tags=["bass", "acoustic", "short", "staccato"]),
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
    warm = adsr(6, 7, 7, 0, exp_release(14))
    sine = adsr(5, 7, 7, 0, exp_release(14))
    saw = adsr(6, 7, 6, 0, exp_release(15))
    slow_strings = adsr(5, 7, 7, 0, exp_release(15))
    swell_strings = gain(GAIN_BENT_INC, 7, exp_release(15))
    choir = adsr(5, 7, 7, 0, exp_release(15))
    return [
        # Echo: the pad is mostly the echo unit
        seed("Warm Pad", "Pad", "Echo", "pad_warm",
             "Warm detuned saw pad (16 kHz loop): A6 (256 ms) SL7 held, GAIN release 14 (2 s); "
             "echo on every voice is the room, VxVOL 76 and MVOL 112 leave headroom for the echo sum; Swell is "
             "GAIN bent-line increase rate 9 (~1.1 s).",
             env=warm, mixing=mix(76, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=PAD_ECHO, main_volume=112,
             axes={"echo": PAD_ECHOES_2, "swell": [("", warm), ("Swell", gain(GAIN_BENT_INC, 9, exp_release(14)))]},
             tags=["pad", "warm", "echo"]),
        seed("Glass Pad", "Pad", "Echo", "pad_glass",
             "Glassy FM pad (16 kHz loop): A7 then D1 to SL6; the N-SPC band-pass echo (Band) gives thin "
             "shimmering repeats; Vib adds +/-8 cents at 5.4 Hz after 200 ms.",
             env=adsr(7, 1, 6, 0, exp_release(14)), mixing=mix(84, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=PAD_ECHO, main_volume=112,
             axes={"echo": PAD_ECHOES_BAND,
                   "vib": [("off", VIB_OFF), ("Vib", vib("pad_glass", 8, VIB_RATE_5HZ, VIB_PAD))]},
             tags=["pad", "glass", "echo"]),
        seed("Brass Pad", "Pad", "Echo", "brass_section",
             "Brass section swelling in on GAIN bent-line rate 9 (~1.1 s), held, GAIN release 15, "
             "5.4 Hz vibrato (+/-10 cents) after 200 ms: the sustained brass chords of SPC "
             "orchestral scores.",
             env=gain(GAIN_BENT_INC, 9, exp_release(15)), mixing=mix(88, 0, 0, 0),
             vibrato=vib("brass_section", 10, VIB_RATE_5HZ, VIB_PAD, 60), voice=TONE, fx=PAD_ECHO,
             main_volume=112,
             axes={"echo": PAD_ECHOES_2},
             tags=["pad", "brass", "swell"]),
        seed("Sine Pad", "Pad", "Echo", "sine_loop",
             "Pure sine pad: A5 (384 ms) SL7; all the colour comes from the echo FIR (low-pass or "
             "band-pass); Swell is GAIN bent-line increase rate 10 (~0.9 s).",
             env=sine, mixing=mix(60, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=PAD_ECHO, main_volume=112,
             axes={"echo": PAD_ECHOES_BAND,
                   "swell": [("", sine), ("Swell", gain(GAIN_BENT_INC, 10, exp_release(14)))]},
             tags=["pad", "sine", "echo"]),
        seed("Saw Pad", "Pad", "Echo", "saw_loop",
             "Saw pad from the 11-cycle loop: A6 SL6 with 5.4 Hz vibrato (+/-12 cents after 200 ms); "
             "Swell is GAIN bent-line increase rate 9 (~1.1 s).",
             env=saw, mixing=mix(64, 0, 0, 0), vibrato=vib("saw_loop", 12, VIB_RATE_5HZ, VIB_PAD, 60),
             voice=TONE, fx=PAD_ECHO, main_volume=112,
             axes={"echo": PAD_ECHOES_2, "swell": [("", saw), ("Swell", gain(GAIN_BENT_INC, 9, exp_release(15)))]},
             tags=["pad", "saw", "echo"]),
        seed("Triangle Pad", "Pad", "Echo", "triangle_loop",
             "Soft triangle pad: A6 SL7 held; EON Split sends only voices 1/3/5/7 to the echo so, "
             "with round-robin allocation, alternate notes stay dry, a mixing trick of SPC drivers.",
             env=adsr(6, 7, 7, 0, exp_release(14)), mixing=mix(64, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=PAD_ECHO, main_volume=112,
             axes={"echo": PAD_ECHOES_2,
                   "eon": [("", eon(ALL_ON)), ("EON Split", eon(ODD_ON))]},
             tags=["pad", "triangle", "echo"]),
        # Strings: the recorded section, slow bow or swell, and the low, Gaussian-darkened section
        seed("Slow Strings", "Pad", "Strings", "strings_ensemble",
             "Slow string pad: A5 (384 ms) bow, SL7 held, GAIN release 15 (1.7 s), vibrato of the "
             "recording only; Slower uses A3 (1 s).",
             env=slow_strings, mixing=mix(88, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=PAD_ECHO,
             main_volume=112,
             axes={"slower": [("", slow_strings), ("Slower", adsr(3, 7, 7, 0, exp_release(14)))],
                   "echo": PAD_ECHOES},
             tags=["pad", "strings", "orchestral"]),
        seed("Swell Strings", "Pad", "Strings", "strings_ensemble",
             "Crescendo strings: GAIN bent-line increase rate 7 (~1.8 s, fast then slower near the "
             "top) instead of an ADSR attack, the way drivers fade strings in; Slow is rate 4 (~3.6 s).",
             env=swell_strings, mixing=mix(88, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=PAD_ECHO,
             main_volume=112,
             axes={"slow": [("", swell_strings), ("Slow", gain(GAIN_BENT_INC, 4, exp_release(14)))],
                   "echo": PAD_ECHOES_2},
             tags=["pad", "strings", "swell"]),
        seed("Low Strings", "Pad", "Strings", "strings_ensemble",
             "Cellos and basses an octave below the keys: at P 0x400 the Gaussian interpolation "
             "rounds off the top, a dark sustained floor under the harmony; A5 SL7, release 15.",
             env=adsr(5, 7, 7, 0, exp_release(15)), mixing=mix(88, 0, -12, 0), vibrato=VIB_OFF, voice=TONE,
             fx=PAD_ECHO, main_volume=112,
             axes={"echo": PAD_ECHOES},
             tags=["pad", "strings", "orchestral", "low", "octave"]),
        seed("Saw Strings", "Pad", "Strings", "saw_loop",
             "Synth strings from the raw saw: A5 SL6 with 5.4 Hz vibrato after 200 ms; OctDown is "
             "the low section, its depth halved so the width in cents stays the same.",
             env=adsr(5, 7, 6, 0, exp_release(15)), mixing=mix(64, 0, 0, 0),
             vibrato=vib("saw_loop", 12, VIB_RATE_5HZ, VIB_PAD, 60), voice=TONE, fx=PAD_ECHO, main_volume=112,
             axes={"echo": PAD_ECHOES,
                   "oct": [("", {}), ("OctDown", {"transpose": -12,
                                                   **vib("saw_loop", 12, VIB_RATE_5HZ, VIB_PAD, 60, -12)})]},
             tags=["pad", "strings", "synth"]),
        # Choir (procedural formant loops at 32 kHz)
        seed("Choir Ah", "Pad", "Choir", "choir_ah",
             "Choir 'ah': A5 (384 ms) SL7 held on the formant loop, GAIN release 15; Vib adds "
             "+/-10 cents at 5.4 Hz after 200 ms for a massed-voices wobble.",
             env=choir, mixing=mix(88, 0, 0, 0), vibrato=VIB_OFF, voice=TONE, fx=PAD_ECHO, main_volume=112,
             axes={"echo": PAD_ECHOES,
                   "vib": [("off", VIB_OFF), ("Vib", vib("choir_ah", 10, VIB_RATE_5HZ, VIB_PAD))]},
             tags=["pad", "choir", "vocal"]),
        seed("Choir Oo", "Pad", "Choir", "choir_oo",
             "Choir 'oo': A6 SL7, darker vowel; Vib is a slower-starting +/-8 cents at 5.4 Hz.",
             env=adsr(6, 7, 7, 0, exp_release(14)), mixing=mix(104, 0, 0, 0), vibrato=VIB_OFF, voice=TONE,
             fx=PAD_ECHO, main_volume=112,
             axes={"echo": PAD_ECHOES_2,
                   "vib": [("off", VIB_OFF), ("Vib", vib("choir_oo", 8, VIB_RATE_5HZ, 2 * VIB_PAD))]},
             tags=["pad", "choir", "vocal"]),
        seed("Swell Choir", "Pad", "Choir", "choir_ah",
             "Choir swelling in with GAIN bent-line increase rate 12 (~560 ms, fast then slower near "
             "the top like a breath), vibrato +/-10 cents; OctDown for male voices, depth halved.",
             env=gain(GAIN_BENT_INC, 12, exp_release(14)), mixing=mix(84, 0, 0, 0),
             vibrato=vib("choir_ah", 10, VIB_RATE_5HZ, VIB_PAD), voice=TONE, fx=PAD_ECHO, main_volume=112,
             axes={"echo": PAD_ECHOES_2,
                   "oct": [("", {}), ("OctDown", {"transpose": -12,
                                                   **vib("choir_ah", 10, VIB_RATE_5HZ, VIB_PAD, 64, -12)})]},
             tags=["pad", "choir", "vocal", "swell"]),
        seed("Dark Choir", "Pad", "Choir", "choir_oo",
             "Low 'oo' choir an octave down, A4 slow onset; EON Split keeps every other voice dry "
             "for clarity.",
             env=adsr(4, 7, 7, 0, exp_release(14)), mixing=mix(108, 0, -12, 0), vibrato=VIB_OFF, voice=TONE,
             fx=PAD_DEEP, main_volume=112,
             axes={"echo": [("", PAD_DEEP), ("Band", PAD_BAND)],
                   "eon": [("", eon(ALL_ON)), ("EON Split", eon(ODD_ON))]},
             tags=["pad", "choir", "vocal", "dark"]),
    ]


# ----- Drums ------------------------------------------------------------------------------

def drum_seeds() -> list[Seed]:
    full_short = [("", DRUM_FULL), ("Short", DRUM_SHORT)]
    full_choke = [("", DRUM_FULL), ("Choke", DRUM_CHOKE)]
    dry = ECHO_OFF  # drums stay out of the echo (EON off on every voice)

    def kit(name: str, sample: str, comment: str, pan: int, axes: Mapping[str, Sequence[Any]],
            tags: Sequence[str], volume: int = 110) -> Seed:
        return seed(name, "Drums", "Kit", sample, comment, env=DRUM_FULL, mixing=mix(volume, pan, 0, 0),
                    vibrato=VIB_OFF, voice=DRUM_SOURCE, fx=dry, main_volume=127, axes=axes,
                    tags=["drums", *tags])

    def dynamic(name: str, sample: str, comment: str, pan: int, tags: Sequence[str]) -> Seed:
        return seed(name, "Drums", "Velocity", sample, comment, env=DRUM_FULL, mixing=mix(110, pan, 0, 0),
                    vibrato=VIB_OFF, voice=DRUM_SOURCE, fx=dry, main_volume=127, axes={"note": LAYERS},
                    tags=["drums", "velocity", *tags])

    return [
        kit("Kick", "kick",
            "Muted concert bass drum, forced one-shot, A15 D7 SL7 so the sample plays untouched; "
            "pitch +/-5 semitones retunes it, Short cuts the boom with D5 to SL0.",
            0, {"transpose": [0, -5, 5], "short": full_short}, ["kick"]),
        kit("Snare", "snare",
            "Snare, dry and centred; transpose +/-4 moves the body tone, Short tightens the noise tail.",
            0, {"transpose": [0, -4, 4], "short": full_short}, ["snare"]),
        kit("Rim", "snare_rim",
            "Snare cross-stick click, short by nature; a fourth down for a deeper crack, a fifth up "
            "for a woodblock-like tick.",
            0, {"transpose": [0, -5, 7]}, ["snare", "rim"]),
        kit("Closed Hat", "hat_closed",
            "Closed hi-hat on the drummer's left; a fourth down for a darker hat, a fifth up for a "
            "smaller cymbal.",
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
            "Small shaker stroke, centred, at VxVOL 127 because the stroke is soft; a fourth down "
            "for a larger shaker.",
            0, {"transpose": [0, -5]}, ["shaker", "percussion"], volume=127),
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
                "up at VxVOL 127, the ghost two semitones down, short (SL0 SR24) at VxVOL 72.",
                0, ["kick"]),
        dynamic("Dynamic Snare", "snare",
                "Snare layers: accent a semitone up at full level, ghost note two semitones down, "
                "short and quiet.", 0, ["snare"]),
        dynamic("Dynamic Rim", "snare_rim",
                "Cross-stick layers for ballad and bossa patterns: accent and ghost clicks.", 0, ["snare", "rim"]),
        dynamic("Dynamic Hat", "hat_closed",
                "Closed hat layers for 8th/16th patterns with accents, on the kit's hat side.",
                PAN_HAT, ["hihat", "cymbal"]),
        dynamic("Dynamic Tom", "tom_mid",
                "Mid tom layers for dynamic fills, at the mid tom's kit position.", PAN_TOM_MID, ["tom"]),
        dynamic("Dynamic Timpani", "timpani",
                "Timpani layers for rolls and crescendos: the accent a semitone up at full level, "
                "ghost strokes short and soft.", 0, ["timpani", "orchestral", "percussion"]),
        dynamic("Dynamic Conga", "conga",
                "Conga layers: open accents and soft ghost slaps for Latin patterns.", PAN_TOM_MID,
                ["conga", "percussion"]),
    ]


# ----- SFX --------------------------------------------------------------------------------

def sfx_seeds() -> list[Seed]:
    looped_noise = "noise_white_loop"  # keeps the noise voice alive: the looped BRR never hits an end code
    pmon_wobble = gain(GAIN_DIRECT, 64, KOFF)
    return [
        # Noise: hardware LFSR noise (NON), pitch comes from noise_clock only
        seed("Noise Burst", "SFX", "Noise", looped_noise,
             "Hardware noise hit: A15 D4 to SL0 then SR20, noise clock sets the colour (16 = 500 Hz "
             "rumble, 24 = 3.2 kHz hiss, 30 = 16 kHz sizzle); keys do not change noise pitch.",
             env=adsr(15, 4, 0, 20, KOFF), mixing=mix(96, 0, 0, 0), vibrato=VIB_OFF, voice=noise_source(24),
             fx=ECHO_OFF, main_volume=127,
             axes={"noise_clock": [16, 24, 30], "echo": SFX_ECHO_2},
             tags=["sfx", "noise", "hit"]),
        seed("Wind", "SFX", "Noise", looped_noise,
             "Wind: A3 (1 s) fade-in of mid-rate noise (clock 14..22, 333 Hz..2 kHz), held, GAIN "
             "release 12 (3.4 s) so it dies away; optional long echo.",
             env=adsr(3, 0, 7, 0, exp_release(12)), mixing=mix(60, 0, 0, 0), vibrato=VIB_OFF, voice=noise_source(18),
             fx=ECHO_OFF, main_volume=127,
             axes={"noise_clock": [14, 18, 22], "echo": [("off", ECHO_OFF), ("Long", SFX_LONG)]},
             tags=["sfx", "noise", "ambience"]),
        seed("Explosion", "SFX", "Noise", looped_noise,
             "Explosion: instant attack, D1 to SL3 then SR14 on low noise clocks (8..16, "
             "83..500 Hz) for crunchy rumble; Hall echo for a big blast.",
             env=adsr(15, 1, 3, 14, exp_release(14)), mixing=mix(104, 0, 0, 0), vibrato=VIB_OFF,
             voice=noise_source(12), fx=ECHO_OFF, main_volume=127,
             axes={"noise_clock": [8, 12, 16], "echo": [("off", ECHO_OFF), ("Hall", SFX_HALL)]},
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
             env=adsr(15, 7, 7, 0, exp_release(16)), mixing=mix(76, 0, 0, 0), vibrato=VIB_OFF,
             voice=noise_source(20, LOOP_ONE_SHOT), fx=ECHO_OFF, main_volume=127,
             axes={"noise_clock": [20, 28], "echo": SFX_ECHO_2},
             tags=["sfx", "noise", "hit"]),
        seed("Rain", "SFX", "Noise", looped_noise,
             "Rain: quiet top-rate noise (clock 28..31) with A12 and SL7 held, through a long "
             "low-passed or a bright high-passed echo for a diffuse wash.",
             env=adsr(12, 7, 7, 0, exp_release(16)), mixing=mix(56, 0, 0, 0), vibrato=VIB_OFF, voice=noise_source(31),
             fx=SFX_LONG, main_volume=127,
             axes={"noise_clock": [28, 31], "echo": [("Long", SFX_LONG), ("Bright", SFX_BRIGHT)]},
             tags=["sfx", "noise", "ambience"]),
        # PMON: hold two notes; the pairing depends on voice allocation (module docstring)
        seed("PMON Bell", "SFX", "PMON", "sine_loop",
             "Sine pair with PMON: while two notes are held, the voice keyed first frequency-modulates "
             "the next one into an inharmonic bell (a lone note plays a plain sine); A15 D3 SL2 SR14 "
             "decays the modulator too so the timbre mellows, GAIN release 24 so a finished note does "
             "not colour the next; OctDown for low gongs.",
             env=adsr(15, 3, 2, 14, exp_release(24)), mixing=mix(100, 0, 0, 0), vibrato=VIB_OFF, voice=PMON_SOURCE,
             fx=ECHO_OFF, main_volume=127,
             axes={"echo": [("off", ECHO_OFF), ("Short", SFX_SHORT), ("Long", SFX_LONG)], "transpose": [0, -12]},
             tags=["sfx", "pmon", "bell"]),
        seed("PMON Wobble", "SFX", "PMON", "triangle_loop",
             "Triangle PMON with direct GAIN: E = value x 16 fixes the modulator level, so Light (64) "
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
             axes={"transpose": [-12, -24], "echo": SFX_ECHO_2},
             tags=["sfx", "pmon", "growl"]),
        seed("PMON Trill", "SFX", "PMON", "square_loop",
             "Square PMON pair with a 31 Hz deep or a 6.9 Hz driver vibrato on both voices: the "
             "modulation ratio trills and buzzes (the driver vibrato cannot sweep slowly enough for "
             "a siren); GAIN release 24.",
             env=adsr(12, 7, 7, 0, exp_release(24)), mixing=mix(60, 0, 0, 0), vibrato=VIB_DEEP, voice=PMON_SOURCE,
             fx=ECHO_OFF, main_volume=127,
             axes={"vib": [("VibDeep", VIB_DEEP), ("Vib7Hz", {"vibrato_rate": 18, "vibrato_depth": 32,
                                                               "vibrato_delay": 15})],
                   "echo": [("off", ECHO_OFF), ("Long", SFX_LONG)]},
             tags=["sfx", "pmon", "trill", "buzz"]),
        seed("PMON Glass", "SFX", "PMON", "pad_glass",
             "Glass pad through PMON for a detuned, metallic shimmer on held chords; the 16 kHz "
             "sample leaves room for OctUp (pitch ceiling three octaves above the root); GAIN "
             "release 24, the echo carries the tail.",
             env=adsr(8, 0, 7, 0, exp_release(24)), mixing=mix(72, 0, 0, 0), vibrato=VIB_OFF, voice=PMON_SOURCE,
             fx=SFX_LONG, main_volume=110,
             axes={"echo": [("Long", SFX_LONG), ("Hall", SFX_HALL)], "transpose": [0, 12]},
             tags=["sfx", "pmon", "glass", "echo"]),
        seed("PMON Metal", "SFX", "PMON", "organ_full",
             "Organ loop modulating organ: dense sidebands for clanging metal; A15 D5 SL3 SR16 so hits "
             "ring then fade, GAIN release 24; a fifth up or a band-passed room echo.",
             env=adsr(15, 5, 3, 16, exp_release(24)), mixing=mix(96, 0, 0, 0), vibrato=VIB_OFF, voice=PMON_SOURCE,
             fx=ECHO_OFF, main_volume=127,
             axes={"transpose": [0, 7], "echo": [("off", ECHO_OFF), ("Room", SFX_ROOM)]},
             tags=["sfx", "pmon", "metal"]),
    ]


def get_seeds() -> list[Seed]:
    return instrument_seeds() + bass_seeds() + pad_seeds() + drum_seeds() + sfx_seeds()
