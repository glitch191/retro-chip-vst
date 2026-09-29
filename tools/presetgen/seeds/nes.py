"""NES (2A03) seed patches.

Every seed sets all 64 engine parameters (docs/ENGINE_SPECS.md, "NES") through the channel
helpers below, which take every field as a required keyword argument, and every
preset-managed global (poly_channels, arp_*, glide_*) through `perf()`. get_seeds() checks
the parameter coverage against tools/presetgen/params/nes.json and fails loudly on a gap.

Register semantics the values rely on (docs/research/nes.md):

* Notes are routed to the channels in `poly_channels` (bit 0 pulse 1, 1 pulse 2,
  2 triangle, 3 noise, 4 DMC; round-robin). Single-voice presets use one channel only.
* Pulse `volume` is the constant 4-bit volume, or, with `env_enable` = 1, the hardware
  envelope divider period: the level steps 15 -> 0 once every V + 1 quarter frames, i.e.
  in 15 * (V + 1) / 240 s (V = 1: 125 ms, V = 4: 313 ms, V = 9: 625 ms, V = 15: 1 s); with
  `env_loop` it repeats as a sawtooth of period 16 * (V + 1) / 240 s (V = 0: 67 ms).
* Bit 5 of $4000/$4004/$400C is both the envelope loop flag and the length counter halt
  flag. Constant-volume patches (env_enable 0) therefore set `env_loop` = 1, the $30 | V
  write of real drivers and of the research driver init: the loop flag has no audible effect
  on a constant volume and the halt keeps held notes from being cut by the length counter.
* Hardware-envelope patches (env_enable 1) leave the software envelope neutral
  (0 / 0 / 15 / 0): with env_enable 1 the volume bits are the divider period, and what a
  software release or the velocity mapping would do to them is not specified yet
  (ENGINE_SPECS / research Ambiguities), so the seeds do not rely on it.
* The sweep unit computes its target period even when disabled; with negate 0 and shift 0
  the target is 2 * period, which mutes every pulse note at or below about A2 (period
  >= $400). Idle sweeps therefore keep `sweep_negate` = 1, as game drivers did.
  Enabled sweeps: negate 1 raises the pitch (mutes once the period drops below 8), negate
  0 lowers it (mutes once the target passes $7FF); each update every (period + 1) half
  frames moves the period by period >> shift. With negate 0 the change is 0 once
  period >> shift is 0 (a dead zone at high notes); pulse 1's ones' complement negate
  (-(period >> shift) - 1) always moves at least one unit.
* Seeds never combine an enabled hardware sweep with a pitch envelope or vibrato: whether the
  driver's per-frame period writes replace the period the sweep has accumulated is not
  specified.
* Vibrato and pitch-envelope depths are in period units, so the same depth is wider in
  cents the higher the note (smaller period) and about twice as wide on the triangle
  (whose period is half the pulse period for the same pitch). Vibrato rate is frames per
  half cycle: 2 = 15 Hz, 3 = 10 Hz, 5 = 6 Hz, 6 = 5 Hz, 15 = 2 Hz. A negative pitch
  envelope starts sharp and falls onto the note; a positive one starts flat and rises.
  Depths are chosen so that period + depth stays >= 8 (pulse) and within $7FF over the
  range named in the seed comment: the driver rule for a start period outside the
  hardware range is not specified.
* Triangle `linear_length` counts quarter frames (240 Hz, 127 = hold): 16 = 67 ms,
  30 = 125 ms, 60 = 250 ms. `attack_frames` mutes the first frames of the note (the
  classic 1-2 frame bass "gate"). `gate_frames` retriggers the note every N frames.
* Noise `period` is the register index (0 = brightest, 15 = darkest); mode 1 is the
  short 93-step metallic sequence. Periods 0 and 1 clock the LFSR at 447 / 224 kHz, both
  flat white noise in band, so bright noise axes use 0 and 3 or higher. The pitch envelope
  offsets the index at note start (negative = brighter) and returns to `period`.
* DMC slots follow the NES entries of assets/samples/index.json in index order (0..15);
  `samples` loads the named WAV into the slot. Rate 15 is the samples' native 33.1 kHz;
  lower rates are coarser 1-bit delta streams (rate 11 = 14.0 kHz). Keyed DMC offsets the
  rate index by note - 60 (clamped 0..15), so keyed seeds use a mid base rate that leaves
  room both ways. `direct_level` 64 starts the delta counter at mid scale so the waveform
  has headroom both ways.
* Duty 3 (75 %) is the 25 % sequence negated: same magnitude spectrum as duty 1 after the
  console high-pass. It appears only where the product owner asked for all four duties
  (Square Lead).
"""

from __future__ import annotations

from presetgen.model import ParamTable, PresetError, Seed

# ----- constants ------------------------------------------------------------------------------

# poly_channels bits (docs/PLUGIN_SPECS.md, bus mapping).
P1, P2, TRI, NZ, DMC = 1, 2, 4, 8, 16

# Arpeggiator choices (plugin/src/Parameters.cpp label order).
UP, DOWN, UPDOWN, PLAYED, RANDOM = 0, 1, 2, 3, 4
SYNC, FREE = 0, 1
DIV_4, DIV_8, DIV_8T, DIV_16, DIV_16T, DIV_32 = 0, 1, 2, 3, 4, 5

# DMC slots = NES sample order in assets/samples/index.json.
DMC_SAMPLES = (
    "bass_pluck", "clap", "crash_short", "hat_closed", "hat_open", "kick_808", "kick_long",
    "kick_short", "noise_burst", "orchestra_hit_synth", "rim", "snare_noisy", "snare_tight",
    "tom_high", "tom_low", "voice_blip",
)

NO_SWEEP = (0, 0, 1, 0)   # enable, period, negate, shift: off, negate set against the low-note mute
NO_VIB = (0, 0, 0)        # rate, depth, delay
NO_PENV = (0, 0)          # depth, speed
HW_ENV_NEUTRAL = (0, 0, 15, 0)  # software envelope left neutral on hardware-envelope patches


# ----- channel blocks ---------------------------------------------------------------------------

def pulse(ch: str, *, duty, volume, env, loop, sweep, vibrato, adsr, pitch_env, transpose) -> dict:
    """All 18 parameters of pulse channel `ch` ("p1" or "p2")."""
    sweep_enable, sweep_period, sweep_negate, sweep_shift = sweep
    vib_rate, vib_depth, vib_delay = vibrato
    attack, decay, sustain, release = adsr
    penv_depth, penv_speed = pitch_env
    return {
        f"{ch}_duty": duty, f"{ch}_volume": volume, f"{ch}_env_enable": env, f"{ch}_env_loop": loop,
        f"{ch}_sweep_enable": sweep_enable, f"{ch}_sweep_period": sweep_period,
        f"{ch}_sweep_negate": sweep_negate, f"{ch}_sweep_shift": sweep_shift,
        f"{ch}_vibrato_rate": vib_rate, f"{ch}_vibrato_depth": vib_depth, f"{ch}_vibrato_delay": vib_delay,
        f"{ch}_sw_attack": attack, f"{ch}_sw_decay": decay, f"{ch}_sw_sustain": sustain,
        f"{ch}_sw_release": release,
        f"{ch}_pitch_env_depth": penv_depth, f"{ch}_pitch_env_speed": penv_speed,
        f"{ch}_transpose": transpose,
    }


def triangle(*, linear, gate, attack, vibrato, pitch_env, transpose) -> dict:
    """All 9 triangle parameters."""
    vib_rate, vib_depth, vib_delay = vibrato
    penv_depth, penv_speed = pitch_env
    return {
        "tri_linear_length": linear, "tri_gate_frames": gate, "tri_attack_frames": attack,
        "tri_vibrato_rate": vib_rate, "tri_vibrato_depth": vib_depth, "tri_vibrato_delay": vib_delay,
        "tri_pitch_env_depth": penv_depth, "tri_pitch_env_speed": penv_speed, "tri_transpose": transpose,
    }


def noise(*, mode, volume, env, loop, period, keyed, adsr, pitch_env) -> dict:
    """All 12 noise parameters."""
    attack, decay, sustain, release = adsr
    penv_depth, penv_speed = pitch_env
    return {
        "nz_mode": mode, "nz_volume": volume, "nz_env_enable": env, "nz_env_loop": loop,
        "nz_period": period, "nz_keyed": keyed,
        "nz_sw_attack": attack, "nz_sw_decay": decay, "nz_sw_sustain": sustain, "nz_sw_release": release,
        "nz_pitch_env_depth": penv_depth, "nz_pitch_env_speed": penv_speed,
    }


def dmc(*, rate, sample, loop, keyed, level) -> dict:
    """All 5 DMC parameters."""
    return {"dmc_rate": rate, "dmc_sample": sample, "dmc_loop": loop, "dmc_keyed": keyed,
            "dmc_direct_level": level}


def patch(*, p1: dict, p2: dict, tri: dict, nz: dict, dmc: dict) -> dict:
    """A full parameter set: NTSC clock, NES-001 output filter, the five channel blocks."""
    params = {"clock": 0, "console_filter": 1}
    for block in (p1, p2, tri, nz, dmc):
        params.update(block)
    return params


# Channels a seed does not play keep a plain, playable setting (heard only if the user adds
# them to poly_channels). Written out once, used explicitly by every seed.
P1_IDLE = pulse("p1", duty=2, volume=10, env=0, loop=1, sweep=NO_SWEEP, vibrato=NO_VIB,
                adsr=(0, 0, 15, 2), pitch_env=NO_PENV, transpose=0)
P2_IDLE = pulse("p2", duty=2, volume=10, env=0, loop=1, sweep=NO_SWEEP, vibrato=NO_VIB,
                adsr=(0, 0, 15, 2), pitch_env=NO_PENV, transpose=0)
TRI_IDLE = triangle(linear=127, gate=0, attack=0, vibrato=NO_VIB, pitch_env=NO_PENV, transpose=0)
NZ_IDLE = noise(mode=0, volume=10, env=0, loop=1, period=8, keyed=0, adsr=(0, 0, 15, 2), pitch_env=NO_PENV)
DMC_IDLE = dmc(rate=15, sample=0, loop=0, keyed=0, level=0)


def perf(poly: int, *, arp: tuple | None = None, glide_ms: float = 0.0, legato: bool = False) -> dict:
    """Every preset-managed global. `arp` = (pattern, octaves, rate_mode, division, free_hz, gate_pct)."""
    if arp is None:
        pattern, octaves, rate_mode, division, free_hz, gate = UP, 1, SYNC, DIV_16, 8.0, 50.0
        enabled = 0
    else:
        pattern, octaves, rate_mode, division, free_hz, gate = arp
        enabled = 1
    return {
        "poly_channels": poly,
        "arp_enabled": enabled, "arp_pattern": pattern, "arp_octaves": octaves,
        "arp_rate_mode": rate_mode, "arp_sync_division": division, "arp_free_rate": float(free_hz),
        "arp_gate": float(gate), "arp_hold": 0,
        "glide_time": float(glide_ms), "glide_mode": 1 if legato else 0,
    }


# ----- axis helpers -----------------------------------------------------------------------------

def vib(ch: str, rate: int, depth: int, delay: int) -> dict:
    return {f"{ch}_vibrato_rate": rate, f"{ch}_vibrato_depth": depth, f"{ch}_vibrato_delay": delay}


def sweep_on(ch: str, period: int, negate: int, shift: int) -> dict:
    return {f"{ch}_sweep_enable": 1, f"{ch}_sweep_period": period, f"{ch}_sweep_negate": negate,
            f"{ch}_sweep_shift": shift}


def env_sw(ch: str, attack: int, decay: int, sustain: int, release: int) -> dict:
    return {f"{ch}_sw_attack": attack, f"{ch}_sw_decay": decay, f"{ch}_sw_sustain": sustain,
            f"{ch}_sw_release": release}


def penv(ch: str, depth: int, speed: int) -> dict:
    return {f"{ch}_pitch_env_depth": depth, f"{ch}_pitch_env_speed": speed}


def both_pulses(**fields) -> dict:
    """The same pulse fields on p1 and p2 (two-channel presets)."""
    out = {}
    for key, value in fields.items():
        out[f"p1_{key}"] = value
        out[f"p2_{key}"] = value
    return out


FOUR_DUTIES = [0, 1, 2, 3]
REGISTERS = [-12, 0, 12]


# ----- Lead -------------------------------------------------------------------------------------

def lead_seeds() -> list[Seed]:
    seeds = [
        Seed(name="Square Lead", category="Lead", subcategory="Pulse", tags=["pulse", "lead", "melody"],
             comment="Plain pulse melody voice: constant volume 12, 10-frame decay to 11/15 so phrases "
                     "breathe; all four duties and three registers as requested. Duty75 is the 25 % "
                     "sequence negated, so it has the same magnitude spectrum as Duty25 (it differs only "
                     "in polarity); it is kept as the named hardware variant.",
             params=patch(p1=pulse("p1", duty=2, volume=12, env=0, loop=1, sweep=NO_SWEEP, vibrato=NO_VIB,
                                   adsr=(0, 10, 11, 4), pitch_env=NO_PENV, transpose=0),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"p1_duty": FOUR_DUTIES, "p1_transpose": REGISTERS}),
        Seed(name="Pluck Lead", category="Lead", subcategory="Pulse", tags=["pulse", "lead", "pluck", "hardware-envelope"],
             comment="Hardware decay envelope (volume = divider period): V 1/4/9 = 125/313/625 ms to "
                     "silence, the quantised 16-level fade of the 2A03.",
             params=patch(p1=pulse("p1", duty=1, volume=4, env=1, loop=0, sweep=NO_SWEEP, vibrato=NO_VIB,
                                   adsr=HW_ENV_NEUTRAL, pitch_env=NO_PENV, transpose=0),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"p1_duty": [0, 1, 2],
                   "decay": [("ShortDecay", {"p1_volume": 1}), ("MidDecay", {"p1_volume": 4}),
                             ("LongDecay", {"p1_volume": 9})]}),
        Seed(name="Soft Lead", category="Lead", subcategory="Pulse", tags=["pulse", "lead", "soft"],
             comment="Software swell: 5-frame attack, 20-frame decay to 10/15, for slower melodies; "
                     "release short (6 frames) or long (24 frames).",
             params=patch(p1=pulse("p1", duty=2, volume=12, env=0, loop=1, sweep=NO_SWEEP, vibrato=NO_VIB,
                                   adsr=(5, 20, 10, 6), pitch_env=NO_PENV, transpose=0),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"p1_duty": [0, 1, 2],
                   "release": [("ShortRelease", {"p1_sw_release": 6}), ("LongRelease", {"p1_sw_release": 24})]}),
        Seed(name="Blip Lead", category="Lead", subcategory="Pulse", tags=["pulse", "lead", "staccato"],
             comment="Staccato blip: full level for one frame then a 6-frame fall to 3/15, so fast "
                     "runs stay articulated even with legato input.",
             params=patch(p1=pulse("p1", duty=1, volume=13, env=0, loop=1, sweep=NO_SWEEP, vibrato=NO_VIB,
                                   adsr=(0, 6, 3, 2), pitch_env=NO_PENV, transpose=0),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"p1_duty": [0, 1, 2], "p1_transpose": [0, 12]}),
        Seed(name="Cutting Lead", category="Lead", subcategory="Vibrato", tags=["pulse", "lead", "vibrato"],
             comment="Thin duties with vibrato after a short delay (12 frames slow, 8 frames fast): rate 6 "
                     "(5 Hz) or 3 (10 Hz), depth 2 or 5 period units (about 14 or 34 cents at A4, wider "
                     "higher up).",
             params=patch(p1=pulse("p1", duty=0, volume=12, env=0, loop=1, sweep=NO_SWEEP, vibrato=(6, 2, 12),
                                   adsr=(0, 12, 12, 6), pitch_env=NO_PENV, transpose=0),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"p1_duty": [0, 1],
                   "vibrato": [("slow shallow", vib("p1", 6, 2, 12)), ("slow deep", vib("p1", 6, 5, 12)),
                               ("fast shallow", vib("p1", 3, 2, 8)), ("fast deep", vib("p1", 3, 5, 8))]}),
        Seed(name="Held Lead", category="Lead", subcategory="Vibrato", tags=["pulse", "lead", "vibrato", "delayed-vibrato"],
             comment="Long-note lead on 25 % or the common 50 % duty: straight tone, then 6 Hz vibrato "
                     "(rate 5) after 20 or 45 frames, the driver trick that gives held notes movement "
                     "without effects.",
             params=patch(p1=pulse("p1", duty=1, volume=12, env=0, loop=1, sweep=NO_SWEEP, vibrato=(5, 2, 20),
                                   adsr=(2, 16, 12, 10), pitch_env=NO_PENV, transpose=0),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"p1_duty": [1, 2],
                   "vibrato": [("delayed shallow", vib("p1", 5, 2, 20)), ("delayed deep", vib("p1", 5, 4, 20)),
                               ("late shallow", vib("p1", 5, 2, 45)), ("late deep", vib("p1", 5, 4, 45))]}),
        Seed(name="Flute Lead", category="Lead", subcategory="Vibrato", tags=["triangle", "lead", "flute", "vibrato"],
             comment="Triangle as a soft flute (no volume control on this channel); vibrato depth 1-2 "
                     "is already twice as wide as on a pulse because the triangle period is halved "
                     "(depth 1 is about 32 cents at C6).",
             params=patch(p1=P1_IDLE, p2=P2_IDLE,
                          tri=triangle(linear=127, gate=0, attack=0, vibrato=(6, 1, 15), pitch_env=NO_PENV,
                                       transpose=12),
                          nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(TRI),
             axes={"vibrato": [("slow", vib("tri", 6, 1, 15)), ("fast", vib("tri", 4, 2, 10))],
                   "tri_transpose": [0, 12]}),
        Seed(name="Bent Lead", category="Lead", subcategory="Sweep", tags=["pulse", "lead", "sweep"],
             comment="Hardware sweep on held notes: droop (period 7, shift 7: about 1-1.5 semitones per "
                     "second down at A3-A4 and none above A5, where period >> 7 = 0), fast droop "
                     "(period 4, shift 6: about 5 semitones per second at A4, none above A6) or a slow "
                     "rise (negate: pulse 1's ones' complement moves at least one unit per update, about "
                     "2 semitones per second, until the period < 8 mute).",
             params=patch(p1=pulse("p1", duty=1, volume=12, env=0, loop=1, sweep=(1, 7, 0, 7), vibrato=NO_VIB,
                                   adsr=(0, 14, 11, 6), pitch_env=NO_PENV, transpose=0),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"p1_duty": [0, 1],
                   "sweep": [("droop", sweep_on("p1", 7, 0, 7)), ("fast droop", sweep_on("p1", 4, 0, 6)),
                             ("rise", sweep_on("p1", 7, 1, 7))]}),
        Seed(name="Bend Lead", category="Lead", subcategory="Sweep", tags=["pulse", "lead", "pitch-envelope", "scoop"],
             comment="Software pitch sweep into each note: +12/+32 period units start flat and rise "
                     "(about 1-2 semitones at A4), or -16 falls in from above.",
             params=patch(p1=pulse("p1", duty=1, volume=12, env=0, loop=1, sweep=NO_SWEEP, vibrato=NO_VIB,
                                   adsr=(0, 18, 11, 5), pitch_env=(12, 4), transpose=0),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"scoop": [("short", penv("p1", 12, 4)), ("wide", penv("p1", 32, 6)),
                             ("slow", penv("p1", 32, 14)), ("above", penv("p1", -16, 6))]}),
        Seed(name="Glide Lead", category="Lead", subcategory="Sweep", tags=["pulse", "lead", "glide", "portamento"],
             comment="Legato portamento (80 ms glide, retuned once per frame so the slide is stepped "
                     "like a driver slide) plus a late 6 Hz vibrato.",
             params=patch(p1=pulse("p1", duty=2, volume=12, env=0, loop=1, sweep=NO_SWEEP, vibrato=(5, 2, 24),
                                   adsr=(0, 14, 12, 8), pitch_env=NO_PENV, transpose=0),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1, glide_ms=80.0, legato=True),
             axes={"p1_duty": [1, 2], "p1_transpose": [0, 12]}),
        Seed(name="High Lead", category="Lead", subcategory="Octave", tags=["pulse", "lead", "octave", "high", "vibrato"],
             comment="Voiced one or two octaves above the written note, with a 2-frame -6 period chirp "
                     "(a quarter tone at a sounding C5, nearly a semitone at C6) and a fast 10 Hz "
                     "vibrato of 1 period unit after 10 frames (about 16 cents at C6, 33 at C7).",
             params=patch(p1=pulse("p1", duty=1, volume=11, env=0, loop=1, sweep=NO_SWEEP, vibrato=(3, 1, 10),
                                   adsr=(0, 8, 10, 4), pitch_env=(-6, 2), transpose=12),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"p1_duty": [0, 1, 2], "p1_transpose": [12, 24]}),
        Seed(name="Low Lead", category="Lead", subcategory="Octave", tags=["pulse", "lead", "octave", "low", "vibrato"],
             comment="Voiced one or two octaves below with a 4-frame swell and a late, wide 6 Hz vibrato "
                     "(10 period units after 24 frames: about 20 cents at C3, 10 at C2). OctDown plays "
                     "from a written A2 up and OctDown2 from A3 (lowest NTSC pulse note A1, t = 2033); "
                     "the negate-set idle sweep keeps notes under A2 from the sweep mute.",
             params=patch(p1=pulse("p1", duty=2, volume=12, env=0, loop=1, sweep=NO_SWEEP, vibrato=(5, 10, 24),
                                   adsr=(4, 12, 12, 6), pitch_env=NO_PENV, transpose=-12),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"p1_duty": [1, 2], "p1_transpose": [-12, -24]}),
    ]
    return seeds


# ----- Bass -------------------------------------------------------------------------------------

def bass_seeds() -> list[Seed]:
    return [
        Seed(name="Triangle Bass", category="Bass", subcategory="Triangle", tags=["triangle", "bass", "key-shift"],
             comment="The classic NES bass: held triangle with a 1-frame attack gate that cleans the "
                     "note start; twelve key shifts (-6..+5) to sit the line in any key.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE,
                          tri=triangle(linear=127, gate=0, attack=1, vibrato=NO_VIB, pitch_env=NO_PENV, transpose=0),
                          nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(TRI),
             axes={"tri_transpose": list(range(-6, 6))}),
        Seed(name="Punch Triangle Bass", category="Bass", subcategory="Triangle", tags=["triangle", "bass", "punch"],
             comment="Triangle with a sharp-start pitch envelope (-24 over 3 frames or -64 over 4 frames: "
                     "about 1 or 3 semitones at C3, less lower down), with or without a 1-frame attack "
                     "gate. The gate stops at 1 frame because the gate and the pitch envelope start "
                     "together at note on, so a longer gate would hide the drop.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE,
                          tri=triangle(linear=127, gate=0, attack=1, vibrato=NO_VIB, pitch_env=(-24, 3), transpose=0),
                          nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(TRI),
             axes={"drop": [("light", penv("tri", -24, 3)), ("hard", penv("tri", -64, 4))],
                   "attack": [("off", {"tri_attack_frames": 0}), ("1 frame", {"tri_attack_frames": 1})]}),
        Seed(name="Warm Triangle Bass", category="Bass", subcategory="Triangle", tags=["triangle", "bass", "vibrato"],
             comment="Held triangle with a late vibrato for long bass notes: slow (rate 7 = 4.3 Hz after "
                     "30 frames) or fast (rate 4 = 7.5 Hz after 20 frames), 4 or 8 period units (about "
                     "8 / 16 cents at C2, 16 / 32 cents at C3).",
             params=patch(p1=P1_IDLE, p2=P2_IDLE,
                          tri=triangle(linear=127, gate=0, attack=1, vibrato=(7, 4, 30), pitch_env=NO_PENV, transpose=0),
                          nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(TRI),
             axes={"vibrato": [("slow", {"tri_vibrato_rate": 7, "tri_vibrato_delay": 30}),
                               ("fast", {"tri_vibrato_rate": 4, "tri_vibrato_delay": 20})],
                   "depth": [("light", {"tri_vibrato_depth": 4}), ("wide", {"tri_vibrato_depth": 8})]}),
        Seed(name="Slide Triangle Bass", category="Bass", subcategory="Triangle", tags=["triangle", "bass", "glide"],
             comment="Legato 60 ms glide between bass notes, each note scooping up from 16 or 32 "
                     "period units flat (a fraction of a semitone at C2, more higher up).",
             params=patch(p1=P1_IDLE, p2=P2_IDLE,
                          tri=triangle(linear=127, gate=0, attack=0, vibrato=NO_VIB, pitch_env=(16, 4), transpose=0),
                          nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(TRI, glide_ms=60.0, legato=True),
             axes={"scoop": [("small", penv("tri", 16, 4)), ("wide", penv("tri", 32, 6))],
                   "tri_transpose": [-12, 0]}),
        Seed(name="Staccato Triangle Bass", category="Bass", subcategory="TriangleGated", tags=["triangle", "bass", "staccato", "linear-counter"],
             comment="Linear counter cuts every note after 16/30/60 quarter frames (67/125/250 ms) "
                     "whatever the note length: tight walking bass.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE,
                          tri=triangle(linear=30, gate=0, attack=1, vibrato=NO_VIB, pitch_env=NO_PENV, transpose=0),
                          nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(TRI),
             axes={"length": [("short", {"tri_linear_length": 16}), ("medium", {"tri_linear_length": 30}),
                              ("long", {"tri_linear_length": 60})],
                   "tri_transpose": [-12, 0]}),
        Seed(name="Gated Triangle Bass", category="Bass", subcategory="TriangleGated", tags=["triangle", "bass", "gated", "stutter"],
             comment="6-quarter-frame (25 ms) linear counter retriggered every 2/3/4/6 frames: a chopped, "
                     "pulsing bass from 30 Hz buzz down to a 10 Hz stutter.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE,
                          tri=triangle(linear=6, gate=3, attack=0, vibrato=NO_VIB, pitch_env=NO_PENV, transpose=0),
                          nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(TRI),
             axes={"tri_gate_frames": [2, 3, 4, 6], "tri_transpose": [-12, 0]}),
        Seed(name="Thump Triangle Bass", category="Bass", subcategory="TriangleGated", tags=["triangle", "bass", "thump"],
             comment="Short triangle bass note (20 or 30 quarter frames = 83 / 125 ms) with a sharp-start "
                     "pitch envelope (-32 over 3 frames or -64 over 4: about 1.3 / 2.8 semitones at C3): "
                     "a bass with a kick-like front, longer than the 33 ms Triangle Thump drum.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE,
                          tri=triangle(linear=20, gate=0, attack=0, vibrato=NO_VIB, pitch_env=(-32, 3), transpose=0),
                          nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(TRI),
             axes={"length": [("short", {"tri_linear_length": 20}), ("long", {"tri_linear_length": 30})],
                   "drop": [("light", penv("tri", -32, 3)), ("hard", penv("tri", -64, 4))]}),
        Seed(name="Pulse Bass", category="Bass", subcategory="Pulse", tags=["pulse", "bass"],
             comment="Pulse bass (brighter than the triangle, has volume), sustained or plucked software "
                     "envelope, written register or OctDown. OctDown plays from a written A2 up (lowest "
                     "NTSC pulse note A1, t = 2033); the negate-set idle sweep keeps notes under A2 "
                     "from the sweep mute.",
             params=patch(p1=pulse("p1", duty=1, volume=13, env=0, loop=1, sweep=NO_SWEEP, vibrato=NO_VIB,
                                   adsr=(0, 8, 12, 4), pitch_env=NO_PENV, transpose=-12),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"p1_duty": [1, 2], "p1_transpose": [-12, 0],
                   "shape": [("sustained", env_sw("p1", 0, 8, 12, 4)), ("plucked", env_sw("p1", 0, 14, 4, 4))]}),
        Seed(name="Pluck Pulse Bass", category="Bass", subcategory="Pulse", tags=["pulse", "bass", "pluck", "hardware-envelope"],
             comment="Hardware-envelope pulse bass one octave down (labelled OctDown; play from a "
                     "written A2 up, lowest NTSC pulse note A1): V 1/3/6 = 125/250/438 ms decays, the "
                     "stepped 16-level fade of the chip. The written register is Pluck Lead.",
             params=patch(p1=pulse("p1", duty=1, volume=3, env=1, loop=0, sweep=NO_SWEEP, vibrato=NO_VIB,
                                   adsr=HW_ENV_NEUTRAL, pitch_env=NO_PENV, transpose=-12),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"p1_duty": [1, 2], "p1_transpose": [-12],
                   "decay": [("ShortDecay", {"p1_volume": 1}), ("MidDecay", {"p1_volume": 3}),
                             ("LongDecay", {"p1_volume": 6})]}),
        Seed(name="Growl Pulse Bass", category="Bass", subcategory="Pulse", tags=["pulse", "bass", "growl"],
             comment="12.5 % pulse with an audio-rate period wobble that roughens the low end: rough "
                     "(rate 1 = 30 Hz, 12 units: about 12 cents at C2, 24 at C3) or deep (rate 2 = 15 Hz, "
                     "24 units). Play from C2 up (OctDown: from a written C3): below about A#1 the deep "
                     "wobble's upper excursion would pass $7FF.",
             params=patch(p1=pulse("p1", duty=0, volume=13, env=0, loop=1, sweep=NO_SWEEP, vibrato=(1, 12, 0),
                                   adsr=(0, 10, 11, 4), pitch_env=NO_PENV, transpose=-12),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"depth": [("rough", vib("p1", 1, 12, 0)), ("deep", vib("p1", 2, 24, 0))],
                   "p1_transpose": [-12, 0]}),
    ]


# ----- Drums ------------------------------------------------------------------------------------

def dmc_drum(name: str, sample: str, subcategory: str, tags: list[str], comment: str, keyed_axis: bool = False) -> Seed:
    """A DMC drum seed for one sample of assets/samples/index.json (NES).

    Rate 15 (33.1 kHz, native) or rate 11 (14.0 kHz, clearly lo-fi). Keyed variants start from
    rate 11 at C4 so the keyboard moves the rate index both ways (4 steps up, 11 down).
    """
    if keyed_axis:
        axes: dict = {"rate": [("Rate15", {"dmc_rate": 15, "dmc_keyed": 0}),
                               ("Rate11", {"dmc_rate": 11, "dmc_keyed": 0}),
                               ("Rate11 Keyed", {"dmc_rate": 11, "dmc_keyed": 1})]}
    else:
        axes = {"dmc_rate": [15, 11]}
    return Seed(name=name, category="Drums", subcategory=subcategory, tags=["dmc", "drums", "sample"] + tags,
                comment=comment,
                params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE,
                             dmc=dmc(rate=15, sample=DMC_SAMPLES.index(sample), loop=0, keyed=0, level=64)),
                global_params=perf(DMC), samples={"dmc_sample": sample}, axes=axes)


def drum_seeds() -> list[Seed]:
    seeds = [
        Seed(name="Noise Kick", category="Drums", subcategory="Kick", tags=["noise", "drums", "kick"],
             comment="Long-mode noise that starts 8 index steps brighter (click) and settles on a dark "
                     "period 11/14 within 3 frames; tight or booming software decay.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE,
                          nz=noise(mode=0, volume=15, env=0, loop=1, period=14, keyed=0, adsr=(0, 5, 0, 3),
                                   pitch_env=(-8, 3)),
                          dmc=DMC_IDLE),
             global_params=perf(NZ),
             axes={"nz_period": [11, 14],
                   "decay": [("TightDecay", {"nz_sw_decay": 5, "nz_sw_release": 3}),
                             ("BoomDecay", {"nz_sw_decay": 12, "nz_sw_release": 6})]}),
        Seed(name="Triangle Thump", category="Drums", subcategory="Kick", tags=["triangle", "drums", "kick"],
             comment="Triangle kick: 8 quarter frames (33 ms) with the maximum -64 period drop; the "
                     "64-unit bound only gives a few semitones, so it is a thump to play around C3.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE,
                          tri=triangle(linear=8, gate=0, attack=0, vibrato=NO_VIB, pitch_env=(-64, 2), transpose=0),
                          nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(TRI),
             axes={"drop": [("fast", {"tri_pitch_env_speed": 2}), ("slow", {"tri_pitch_env_speed": 5})],
                   "tri_transpose": [-12, 0]}),
        Seed(name="Noise Snare", category="Drums", subcategory="Snare", tags=["noise", "drums", "snare"],
             comment="Long-mode noise at mid periods 4/6/8, starting 3 steps brighter for the crack; "
                     "tight (6 frames) or loose (16 frames) tail.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE,
                          nz=noise(mode=0, volume=15, env=0, loop=1, period=6, keyed=0, adsr=(0, 6, 0, 3),
                                   pitch_env=(-3, 4)),
                          dmc=DMC_IDLE),
             global_params=perf(NZ),
             axes={"nz_period": [4, 6, 8],
                   "tail": [("tight", {"nz_sw_decay": 6, "nz_sw_release": 3}),
                            ("loose", {"nz_sw_decay": 16, "nz_sw_release": 8})]}),
        Seed(name="Metal Snare", category="Drums", subcategory="Snare", tags=["noise", "drums", "snare", "metallic"],
             comment="Short-mode (93-step) noise gives a pitched, clanky snare; periods 5/7, "
                     "8- or 18-frame decay.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE,
                          nz=noise(mode=1, volume=14, env=0, loop=1, period=5, keyed=0, adsr=(0, 8, 0, 4),
                                   pitch_env=(-2, 3)),
                          dmc=DMC_IDLE),
             global_params=perf(NZ),
             axes={"nz_period": [5, 7],
                   "tail": [("tight", {"nz_sw_decay": 8, "nz_sw_release": 4}),
                            ("loose", {"nz_sw_decay": 18, "nz_sw_release": 9})]}),
        Seed(name="Rattle Clap", category="Drums", subcategory="Snare", tags=["noise", "drums", "clap", "hardware-envelope"],
             comment="Looping hardware envelope (sawtooth every 67 ms at V 0, 200 ms at V 2) makes "
                     "repeated noise bursts for as long as the note is held: a clap/rattle.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE,
                          nz=noise(mode=0, volume=0, env=1, loop=1, period=5, keyed=0, adsr=HW_ENV_NEUTRAL,
                                   pitch_env=NO_PENV),
                          dmc=DMC_IDLE),
             global_params=perf(NZ),
             axes={"nz_period": [5, 8],
                   "rate": [("fast", {"nz_volume": 0}), ("slow", {"nz_volume": 2})]}),
        Seed(name="Closed Hat", category="Drums", subcategory="HiHat", tags=["noise", "drums", "hihat", "closed"],
             comment="Long-mode periods 0/3/5 (LFSR clocked at 447 / 56 / 19 kHz: full white, slightly "
                     "darker, dull) with a 2- or 5-frame software decay: a closed hat tick.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE,
                          nz=noise(mode=0, volume=12, env=0, loop=1, period=0, keyed=0, adsr=(0, 2, 0, 1),
                                   pitch_env=NO_PENV),
                          dmc=DMC_IDLE),
             global_params=perf(NZ),
             axes={"nz_period": [0, 3, 5],
                   "decay": [("TightDecay", {"nz_sw_decay": 2, "nz_sw_release": 1}),
                             ("SoftDecay", {"nz_sw_decay": 5, "nz_sw_release": 2})]}),
        Seed(name="Open Hat", category="Drums", subcategory="HiHat", tags=["noise", "drums", "hihat", "open", "hardware-envelope"],
             comment="Hardware decay envelope V 2/5 (188/375 ms) on bright periods 0/3: the open "
                     "hat with the chip's stepped fade.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE,
                          nz=noise(mode=0, volume=2, env=1, loop=0, period=0, keyed=0, adsr=HW_ENV_NEUTRAL,
                                   pitch_env=NO_PENV),
                          dmc=DMC_IDLE),
             global_params=perf(NZ),
             axes={"nz_period": [0, 3],
                   "decay": [("MidDecay", {"nz_volume": 2}), ("LongDecay", {"nz_volume": 5})]}),
        Seed(name="Metal Hat", category="Drums", subcategory="HiHat", tags=["noise", "drums", "hihat", "metallic"],
             comment="Short-mode noise at periods 0/1 rings as a metallic, pitched hat (93-step tones "
                     "at about 4.8 / 2.4 kHz); 3- or 10-frame decay.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE,
                          nz=noise(mode=1, volume=11, env=0, loop=1, period=0, keyed=0, adsr=(0, 3, 0, 2),
                                   pitch_env=NO_PENV),
                          dmc=DMC_IDLE),
             global_params=perf(NZ),
             axes={"nz_period": [0, 1],
                   "decay": [("TightDecay", {"nz_sw_decay": 3, "nz_sw_release": 2}),
                             ("RingDecay", {"nz_sw_decay": 10, "nz_sw_release": 5})]}),
    ]
    seeds += [
        dmc_drum("DMC Kick Short", "kick_short", "DMC", ["kick"],
                 "Punchy 150-50 Hz sampled kick, the usual DMC use in NES soundtracks; native rate "
                 "15 or grittier rate 11."),
        dmc_drum("DMC Kick 808", "kick_808", "DMC", ["kick", "808"],
                 "Round 400 ms 808-style kick through the 1-bit delta stream; rate 15 or 11."),
        dmc_drum("DMC Kick Long", "kick_long", "DMC", ["kick"],
                 "Long kick with a slow pitch drop; the delta encoder smears its tail at rate 11."),
        dmc_drum("DMC Snare Tight", "snare_tight", "DMC", ["snare"],
                 "Tight sampled snare with a short noise tail; rate 15 or 11."),
        dmc_drum("DMC Snare Noisy", "snare_noisy", "DMC", ["snare"],
                 "Snare with a long noise tail, which the 1-bit encoder turns into hiss; rate 15 or 11."),
        dmc_drum("DMC Clap", "clap", "DMC", ["clap"],
                 "Sampled four-burst hand clap; rate 15 or 11."),
        dmc_drum("DMC Hat Closed", "hat_closed", "DMC", ["hihat", "closed"],
                 "Closed hat sample; its high content suffers most at the lower rate 11, a useful lo-fi option."),
        dmc_drum("DMC Hat Open", "hat_open", "DMC", ["hihat", "open"],
                 "300 ms open hat sample; rate 15 or 11."),
        dmc_drum("DMC Crash", "crash_short", "DMC", ["crash", "cymbal"],
                 "Short 400 ms crash, rare on NES because of ROM cost; rate 15 or 11."),
        dmc_drum("DMC Rim", "rim", "DMC", ["rim"],
                 "Resonant rimshot ping; rate 15 or 11."),
        dmc_drum("DMC Tom High", "tom_high", "DMC", ["tom"],
                 "170 Hz tom at rate 15 or 11; the keyed variant moves the DMC rate index with the note "
                 "(note - 60 from base rate 11, clamped 0..15) for stepped tom fills.", keyed_axis=True),
        dmc_drum("DMC Tom Low", "tom_low", "DMC", ["tom"],
                 "90 Hz tom at rate 15 or 11; the keyed variant retunes it by DMC rate steps from the "
                 "keyboard, both ways around C4.", keyed_axis=True),
    ]
    return seeds


# ----- Arp --------------------------------------------------------------------------------------

def arp_pulse(*, duty, volume, env, adsr, vibrato=NO_VIB, ch: str = "p1") -> dict:
    """Pulse voice for an arpeggio (no sweep, no pitch envelope, written register).

    Constant volume sets the loop / length-halt bit ($30 | V); the hardware envelope decays once.
    """
    return pulse(ch, duty=duty, volume=volume, env=env, loop=1 if env == 0 else 0, sweep=NO_SWEEP,
                 vibrato=vibrato, adsr=adsr, pitch_env=NO_PENV, transpose=0)


def arp_seeds() -> list[Seed]:
    return [
        Seed(name="Frame Chord", category="Arp", subcategory="Triad",
             tags=["arp", "pulse", "chord", "triad", "major", "minor", "hold-3-notes", "famicom-chord"],
             comment="Hold a major or minor triad: free-running 30 Hz (a note every 2 frames), gate 100 %, "
                     "the Famicom 'chord on one channel' sound.",
             params=patch(p1=arp_pulse(duty=2, volume=11, env=0, adsr=(0, 20, 11, 5)),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1, arp=(UP, 1, FREE, DIV_16, 30.0, 100.0)),
             axes={"p1_duty": [0, 1, 2], "p1_transpose": [0, 12]}),
        Seed(name="Slow Chord", category="Arp", subcategory="Triad",
             tags=["arp", "pulse", "chord", "triad", "major", "minor", "hold-3-notes"],
             comment="Hold a triad: 20 Hz steps (3 frames per note) so each chord tone is heard, with "
                     "a faint late vibrato for pads.",
             params=patch(p1=arp_pulse(duty=1, volume=10, env=0, adsr=(0, 30, 9, 8), vibrato=(6, 1, 20)),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1, arp=(UP, 1, FREE, DIV_16, 20.0, 100.0)),
             axes={"p1_duty": [1, 2], "p1_transpose": [0, 12]}),
        Seed(name="Up Triad", category="Arp", subcategory="Triad",
             tags=["arp", "pulse", "triad", "major", "minor", "hold-3-notes", "tempo-sync"],
             comment="Hold a triad: tempo-synced 1/16 upward run over two octaves, gate 50 %, short "
                     "4-frame decay so each step clicks.",
             params=patch(p1=arp_pulse(duty=1, volume=12, env=0, adsr=(0, 4, 6, 2)),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1, arp=(UP, 2, SYNC, DIV_16, 8.0, 50.0)),
             axes={"p1_duty": [0, 1, 2], "p1_transpose": [0, 12]}),
        Seed(name="Down Triad", category="Arp", subcategory="Triad",
             tags=["arp", "pulse", "triad", "major", "minor", "hold-3-notes", "tempo-sync", "hardware-envelope"],
             comment="Hold a triad: falling 1/16 run over two octaves on pulse 2 (leaving pulse 1 free "
                     "for a lead) with the hardware decay envelope (V 2, 188 ms) plucking every step.",
             params=patch(p1=P1_IDLE, p2=arp_pulse(duty=2, volume=2, env=1, adsr=HW_ENV_NEUTRAL, ch="p2"),
                          tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P2, arp=(DOWN, 2, SYNC, DIV_16, 8.0, 75.0)),
             axes={"p2_duty": [1, 2], "p2_transpose": [0, 12]}),
        Seed(name="Bounce Triad", category="Arp", subcategory="Triad",
             tags=["arp", "pulse", "triad", "major", "minor", "hold-3-notes", "tempo-sync", "triplet"],
             comment="Hold a triad: up-down 1/16 triplets over two octaves, gate 60 %, 6-frame decay "
                     "to 8/15.",
             params=patch(p1=arp_pulse(duty=1, volume=12, env=0, adsr=(0, 6, 8, 3)),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1, arp=(UPDOWN, 2, SYNC, DIV_16T, 8.0, 60.0)),
             axes={"p1_duty": [1, 2], "p1_transpose": [0, 12]}),
        Seed(name="Random Triad", category="Arp", subcategory="Triad",
             tags=["arp", "pulse", "triad", "hold-3-notes", "hold-4-notes", "tempo-sync", "random"],
             comment="Hold a triad or a seventh chord: random 1/16 order over two octaves, gate 40 %, "
                     "very short blip envelope.",
             params=patch(p1=arp_pulse(duty=1, volume=12, env=0, adsr=(0, 5, 5, 1)),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1, arp=(RANDOM, 2, SYNC, DIV_16, 8.0, 40.0)),
             axes={"p1_duty": [1, 2], "p1_transpose": [0, 12]}),
        Seed(name="Triangle Triad", category="Arp", subcategory="Triad",
             tags=["arp", "triad", "triangle", "bass", "hold-3-notes", "tempo-sync"],
             comment="Hold a triad: 1/8 bass arpeggio on the triangle in played order, notes cut by "
                     "the linear counter after 40 or 20 quarter frames (167 / 83 ms).",
             params=patch(p1=P1_IDLE, p2=P2_IDLE,
                          tri=triangle(linear=40, gate=0, attack=0, vibrato=NO_VIB, pitch_env=NO_PENV, transpose=0),
                          nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(TRI, arp=(PLAYED, 1, SYNC, DIV_8, 8.0, 90.0)),
             axes={"length": [("medium", {"tri_linear_length": 40}), ("short", {"tri_linear_length": 20})],
                   "tri_transpose": [-12, 0]}),
        Seed(name="Echo Triad", category="Arp", subcategory="Triad",
             tags=["arp", "pulse", "triad", "hold-3-notes", "tempo-sync", "two-channel"],
             comment="Hold a triad: 1/16 steps alternate round-robin between pulse 1 and pulse 2, so "
                     "each step's 12- or 30-frame release rings under the next (no effects needed).",
             params=patch(p1=pulse("p1", duty=1, volume=11, env=0, loop=1, sweep=NO_SWEEP, vibrato=NO_VIB,
                                   adsr=(0, 4, 8, 12), pitch_env=NO_PENV, transpose=0),
                          p2=pulse("p2", duty=1, volume=11, env=0, loop=1, sweep=NO_SWEEP, vibrato=NO_VIB,
                                   adsr=(0, 4, 8, 12), pitch_env=NO_PENV, transpose=0),
                          tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1 | P2, arp=(UP, 1, SYNC, DIV_16, 8.0, 30.0)),
             axes={"duty": [("Duty12", both_pulses(duty=0)), ("Duty25", both_pulses(duty=1)),
                            ("Duty50", both_pulses(duty=2))],
                   "release": [("ShortRelease", both_pulses(sw_release=12)),
                               ("LongRelease", both_pulses(sw_release=30))]}),
        Seed(name="Octave Interval", category="Arp", subcategory="Interval",
             tags=["arp", "pulse", "interval", "octave", "hold-1-note", "tempo-sync"],
             comment="Hold one note: the arp alternates it with its octave (arp_octaves 2) in 1/16, "
                     "the classic octave-bounce bass/lead.",
             params=patch(p1=arp_pulse(duty=2, volume=12, env=0, adsr=(0, 8, 7, 3)),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1, arp=(UP, 2, SYNC, DIV_16, 8.0, 70.0)),
             axes={"p1_duty": [0, 1, 2], "p1_transpose": [0, 12]}),
        Seed(name="Warble Interval", category="Arp", subcategory="Interval",
             tags=["arp", "pulse", "interval", "octave", "hold-1-note", "famicom-chord"],
             comment="Hold one note: note and octave alternate at 30 Hz (every 2 frames), gate 100 %, "
                     "a buzzy octave warble.",
             params=patch(p1=arp_pulse(duty=1, volume=11, env=0, adsr=(0, 40, 10, 6)),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1, arp=(UP, 2, FREE, DIV_16, 30.0, 100.0)),
             axes={"p1_duty": [1, 2], "p1_transpose": [0, 12]}),
        Seed(name="Fifth Interval", category="Arp", subcategory="Interval",
             tags=["arp", "pulse", "interval", "fifth", "power-chord", "hold-2-notes", "tempo-sync"],
             comment="Hold root and fifth (or any two notes): up-down 1/32 trill, gate 80 %, 2-frame "
                     "decay to 10/15.",
             params=patch(p1=arp_pulse(duty=1, volume=12, env=0, adsr=(0, 2, 10, 2)),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1, arp=(UPDOWN, 1, SYNC, DIV_32, 8.0, 80.0)),
             axes={"p1_duty": [0, 1], "p1_transpose": [0, 12]}),
        Seed(name="Wide Interval", category="Arp", subcategory="Interval",
             tags=["arp", "pulse", "interval", "octave", "hold-1-note", "tempo-sync", "triplet"],
             comment="Hold one or two notes: 1/16 triplets climbing three octaves, gate 45 %, for "
                     "sparkling fills.",
             params=patch(p1=arp_pulse(duty=1, volume=11, env=0, adsr=(0, 6, 6, 4)),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1, arp=(UP, 3, SYNC, DIV_16T, 8.0, 45.0)),
             axes={"p1_duty": [1, 2], "p1_transpose": [-12, 0]}),
    ]


# ----- SFX --------------------------------------------------------------------------------------

def sfx_seeds() -> list[Seed]:
    return [
        Seed(name="Rise", category="SFX", subcategory="SweepUp", tags=["pulse", "sfx", "sweep", "rise"],
             comment="Upward hardware sweep (negate) that mutes itself once the period falls below 8: "
                     "fast (period 0, shift 2), medium (2, 3) or slow (6, 4); 30-frame decay bounds it.",
             params=patch(p1=pulse("p1", duty=2, volume=13, env=0, loop=1, sweep=(1, 2, 1, 3), vibrato=NO_VIB,
                                   adsr=(0, 30, 0, 4), pitch_env=NO_PENV, transpose=0),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"sweep": [("fast", sweep_on("p1", 0, 1, 2)), ("medium", sweep_on("p1", 2, 1, 3)),
                             ("slow", sweep_on("p1", 6, 1, 4))]}),
        Seed(name="Power Up", category="SFX", subcategory="SweepUp", tags=["pulse", "sfx", "sweep", "rise", "hardware-envelope"],
             comment="Slow upward sweep under a looping hardware envelope (7.5 Hz sawtooth at V 1): "
                     "the pulsing power-up climb, lasting as long as the note; 25 % or 50 % duty.",
             params=patch(p1=pulse("p1", duty=1, volume=1, env=1, loop=1, sweep=(1, 5, 1, 4), vibrato=NO_VIB,
                                   adsr=HW_ENV_NEUTRAL, pitch_env=NO_PENV, transpose=0),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"sweep": [("slow", sweep_on("p1", 5, 1, 4)), ("crawl", sweep_on("p1", 7, 1, 6))],
                   "p1_duty": [1, 2]}),
        Seed(name="Jump", category="SFX", subcategory="SweepUp", tags=["pulse", "sfx", "sweep", "jump"],
             comment="Short 50 % chirp rising by (period >> 5) + 1 every 2 half frames; the sound lasts "
                     "10 or 24 frames (sw_decay).",
             params=patch(p1=pulse("p1", duty=2, volume=12, env=0, loop=1, sweep=(1, 1, 1, 5), vibrato=NO_VIB,
                                   adsr=(0, 10, 0, 2), pitch_env=NO_PENV, transpose=0),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"length": [("short", {"p1_sw_decay": 10}), ("long", {"p1_sw_decay": 24})],
                   "p1_transpose": [0, 12]}),
        Seed(name="Fall", category="SFX", subcategory="SweepDown", tags=["pulse", "sfx", "sweep", "fall"],
             comment="Downward hardware sweep that mutes when the target period passes $7FF. Fast "
                     "(period 1, shift 2) reaches the mute within about 13 frames from a written C5, so it "
                     "has a single 20-frame decay; medium (3, 3) and slow (6, 4) come with a 20- or "
                     "45-frame decay.",
             params=patch(p1=pulse("p1", duty=2, volume=13, env=0, loop=1, sweep=(1, 3, 0, 3), vibrato=NO_VIB,
                                   adsr=(0, 20, 0, 4), pitch_env=NO_PENV, transpose=12),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"sweep": [("fast", {**sweep_on("p1", 1, 0, 2), "p1_sw_decay": 20}),
                             ("medium short", {**sweep_on("p1", 3, 0, 3), "p1_sw_decay": 20}),
                             ("medium long", {**sweep_on("p1", 3, 0, 3), "p1_sw_decay": 45}),
                             ("slow short", {**sweep_on("p1", 6, 0, 4), "p1_sw_decay": 20}),
                             ("slow long", {**sweep_on("p1", 6, 0, 4), "p1_sw_decay": 45})]}),
        Seed(name="Whistle Drop", category="SFX", subcategory="SweepDown", tags=["pulse", "sfx", "sweep", "fall", "whistle"],
             comment="Thin high whistle falling with the slowest useful sweep (period 7, shift 6), "
                     "long 60-frame decay and 20-frame release: a falling-bomb whistle. Dead zone: "
                     "period >> 6 is 0 below period 64 (above about A6), where the tone stays static, "
                     "so play below a written A6 (A5 with OctUp).",
             params=patch(p1=pulse("p1", duty=0, volume=12, env=0, loop=1, sweep=(1, 7, 0, 6), vibrato=NO_VIB,
                                   adsr=(0, 60, 8, 20), pitch_env=NO_PENV, transpose=12),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"p1_duty": [0, 1], "p1_transpose": [0, 12]}),
        Seed(name="Triangle Drop", category="SFX", subcategory="SweepDown", tags=["triangle", "sfx", "fall"],
             comment="The triangle has no sweep unit, so the driver's pitch envelope does it: start 64 "
                     "period units sharp (about +6 semitones at C4, +16 at C5) and fall over 8 frames "
                     "(linear counter 100 = 417 ms) or 30 frames (held, linear 127, so the 500 ms fall is "
                     "not cut). Play below about A5 (A6 with OctDown): higher up the start period would "
                     "go below 2, the ultrasonic triangle case.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE,
                          tri=triangle(linear=100, gate=0, attack=0, vibrato=NO_VIB, pitch_env=(-64, 8), transpose=0),
                          nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(TRI),
             axes={"fall": [("fast", {"tri_pitch_env_speed": 8, "tri_linear_length": 100}),
                            ("slow", {"tri_pitch_env_speed": 30, "tri_linear_length": 127})],
                   "tri_transpose": [-12, 0]}),
        Seed(name="Noise Burst", category="SFX", subcategory="Noise", tags=["noise", "sfx", "burst"],
             comment="White (long-mode) noise burst at bright/mid/dark periods 3/8/12; 10-frame or "
                     "40-frame software decay.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE,
                          nz=noise(mode=0, volume=14, env=0, loop=1, period=8, keyed=0, adsr=(0, 10, 0, 4),
                                   pitch_env=NO_PENV),
                          dmc=DMC_IDLE),
             global_params=perf(NZ),
             axes={"nz_period": [3, 8, 12],
                   "length": [("short", {"nz_sw_decay": 10, "nz_sw_release": 4}),
                              ("long", {"nz_sw_decay": 40, "nz_sw_release": 20})]}),
        Seed(name="Explosion", category="SFX", subcategory="Noise", tags=["noise", "sfx", "explosion", "hardware-envelope"],
             comment="Dark noise (period 12/14) under the hardware decay (V 8 = 0.56 s, V 15 = 1.0 s), "
                     "starting 6 steps brighter and darkening over 30 frames.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE,
                          nz=noise(mode=0, volume=8, env=1, loop=0, period=12, keyed=0, adsr=HW_ENV_NEUTRAL,
                                   pitch_env=(-6, 30)),
                          dmc=DMC_IDLE),
             global_params=perf(NZ),
             axes={"nz_period": [12, 14],
                   "decay": [("MidDecay", {"nz_volume": 8}), ("LongDecay", {"nz_volume": 15})]}),
        Seed(name="Rumble", category="SFX", subcategory="Noise", tags=["noise", "sfx", "rumble", "keyed"],
             comment="Keyed noise (note 36 + n plays period 15 - n) with a 10- or 30-frame swell and "
                     "30-frame release: long mode for wind, engines and surf, short (metallic) mode for "
                     "machine hums.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE,
                          nz=noise(mode=0, volume=12, env=0, loop=1, period=10, keyed=1, adsr=(10, 20, 10, 30),
                                   pitch_env=NO_PENV),
                          dmc=DMC_IDLE),
             global_params=perf(NZ),
             axes={"mode": [("Long", {"nz_mode": 0}), ("Metal", {"nz_mode": 1})],
                   "swell": [("fast", {"nz_sw_attack": 10}), ("slow", {"nz_sw_attack": 30})]}),
        Seed(name="Buzz Tone", category="SFX", subcategory="Noise", tags=["noise", "sfx", "metallic", "keyed"],
             comment="Short-mode noise played as pitched buzz tones from the keyboard (keyed); "
                     "short blip or held, optionally falling 4 index steps into the note.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE,
                          nz=noise(mode=1, volume=12, env=0, loop=1, period=6, keyed=1, adsr=(0, 6, 0, 3),
                                   pitch_env=NO_PENV),
                          dmc=DMC_IDLE),
             global_params=perf(NZ),
             axes={"length": [("short", env_sw("nz", 0, 6, 0, 3)), ("held", env_sw("nz", 0, 12, 8, 6))],
                   "fall": [("off", penv("nz", 0, 0)), ("in", penv("nz", -4, 12))]}),
        Seed(name="Sample Burst", category="SFX", subcategory="Noise", tags=["dmc", "sfx", "sample", "burst"],
             comment="Sampled noise burst with a falling low-pass through the DMC; rate 15/12/9 "
                     "trades fidelity for grit.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE,
                          dmc=dmc(rate=15, sample=DMC_SAMPLES.index("noise_burst"), loop=0, keyed=0, level=64)),
             global_params=perf(DMC), samples={"dmc_sample": "noise_burst"},
             axes={"dmc_rate": [15, 12, 9]}),
        Seed(name="Laser Zap", category="SFX", subcategory="Zap", tags=["pulse", "sfx", "zap", "laser"],
             comment="Starts sharp and falls onto the note in 4 or 10 frames under a 12-frame decay: a "
                     "laser. 64 period units one octave up, 24 two octaves up, so the start period stays "
                     ">= 8 up to about a written G5. Pitch envelope only: the hardware sweep is left off "
                     "because the driver's per-frame period writes would fight it (unspecified).",
             params=patch(p1=pulse("p1", duty=0, volume=13, env=0, loop=1, sweep=NO_SWEEP, vibrato=NO_VIB,
                                   adsr=(0, 12, 0, 2), pitch_env=(-64, 4), transpose=12),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"fall": [("fast", {"p1_pitch_env_speed": 4}), ("slow", {"p1_pitch_env_speed": 10})],
                   "oct": [("OctUp", {"p1_transpose": 12, "p1_pitch_env_depth": -64}),
                           ("OctUp2", {"p1_transpose": 24, "p1_pitch_env_depth": -24})]}),
        Seed(name="Siren Zap", category="SFX", subcategory="Zap", tags=["pulse", "sfx", "zap", "siren", "warble"],
             comment="Period wobble one octave up: rate 15 (2 Hz) at the maximum 31 units is a real "
                     "siren (about -4 / +6 semitones at C6); rate 2 (15 Hz), narrow or wide (12/30 units, "
                     "about 1-2.5 semitones at C5), gives alarms and ray-gun warbles.",
             params=patch(p1=pulse("p1", duty=1, volume=12, env=0, loop=1, sweep=NO_SWEEP, vibrato=(15, 31, 0),
                                   adsr=(0, 30, 6, 6), pitch_env=NO_PENV, transpose=12),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"wobble": [("2Hz deep", {"p1_vibrato_rate": 15, "p1_vibrato_depth": 31}),
                              ("15Hz narrow", {"p1_vibrato_rate": 2, "p1_vibrato_depth": 12}),
                              ("15Hz wide", {"p1_vibrato_rate": 2, "p1_vibrato_depth": 30})]}),
        Seed(name="Blip Zap", category="SFX", subcategory="Zap", tags=["pulse", "sfx", "zap", "blip"],
             comment="Tiny rising chirp: starts 32 period units flat, reaches the note in 2 frames, "
                     "gone after 4: menu cursors and pickups.",
             params=patch(p1=pulse("p1", duty=2, volume=12, env=0, loop=1, sweep=NO_SWEEP, vibrato=NO_VIB,
                                   adsr=(0, 4, 0, 1), pitch_env=(32, 2), transpose=12),
                          p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE, dmc=DMC_IDLE),
             global_params=perf(P1),
             axes={"p1_duty": [0, 2], "p1_transpose": [12, 24]}),
        Seed(name="Voice Blip", category="SFX", subcategory="Zap", tags=["dmc", "sfx", "sample", "voice", "keyed"],
             comment="Formant 'e' blip on the DMC, keyed so each note offsets the base rate index "
                     "11/9/7 by note - 60 (clamped 0..15), leaving room both ways around C4: stepped "
                     "robot chatter.",
             params=patch(p1=P1_IDLE, p2=P2_IDLE, tri=TRI_IDLE, nz=NZ_IDLE,
                          dmc=dmc(rate=11, sample=DMC_SAMPLES.index("voice_blip"), loop=0, keyed=1, level=64)),
             global_params=perf(DMC), samples={"dmc_sample": "voice_blip"},
             axes={"dmc_rate": [11, 9, 7]}),
    ]


# ----- entry point ------------------------------------------------------------------------------

def _check_complete(seeds: list[Seed]) -> None:
    """Every seed must set every engine parameter explicitly (no reliance on defaults)."""
    keys = set(ParamTable.load("nes").keys())
    for seed in seeds:
        missing = sorted(keys - set(seed.params))
        extra = sorted(set(seed.params) - keys)
        if missing or extra:
            raise PresetError(f"nes: seed {seed.name!r}: missing {missing}, unknown {extra}")
        for axis, values in seed.axes.items():
            for value in values:
                if isinstance(value, tuple) and set(value[1]) - keys:
                    raise PresetError(f"nes: seed {seed.name!r}: axis {axis!r} sets unknown keys")


def get_seeds() -> list[Seed]:
    seeds = lead_seeds() + bass_seeds() + drum_seeds() + arp_seeds() + sfx_seeds()
    _check_complete(seeds)
    return seeds
