"""Tonal instrument builders (SNES BRR sources, NES bass pluck, single-cycle loops, pads).

Signature: ``builder(sr, rng, **params)`` returning either a plain list (one-shot) or a
``(samples, (loop_start, loop_end))`` tuple for sustained sounds. Loop points are
multiples of 16 frames (one BRR block) and every looped sound is rendered at a
frequency that fits a whole number of cycles inside the loop (see
``synth.loop_length``), then cross-faded so that the wrap is seamless even for
detuned or modulated layers.
"""

import math

from . import synth as S

BLOCK = 16


# ---------------------------------------------------------------------------- helpers

def _attack(n, sr, seconds):
    """Raised-cosine attack reaching 1.0 after ``seconds``; 1.0 afterwards."""
    a = max(1, S.frames(sr, seconds))
    env = [1.0] * n
    for i in range(min(a, n)):
        env[i] = 0.5 - 0.5 * math.cos(math.pi * (i + 1) / a)
    return env


def _vibrato(n, sr, freq, depth_cents, rate_hz, delay_s, ramp_s):
    """Per-sample frequency list with a delayed, ramped sinusoidal vibrato."""
    out = [freq] * n
    if depth_cents <= 0 or rate_hz <= 0:
        return out
    inv = 1.0 / sr
    sin = math.sin
    for i in range(n):
        t = i * inv
        if t <= delay_s:
            continue
        d = min(1.0, (t - delay_s) / max(ramp_s, 1e-6)) * depth_cents
        out[i] = freq * 2.0 ** (d * sin(S.TWO_PI * rate_hz * t) / 1200.0)
    return out


def _loop_plan(sr, freq, head_s, min_loop_frames):
    """Return ``(loop_start, loop_len, n, exact_freq)`` for a sustained sound."""
    loop_len, exact = S.loop_length(sr, freq, min_loop_frames, BLOCK)
    loop_start = int(math.ceil(S.frames(sr, head_s) / BLOCK)) * BLOCK
    return loop_start, loop_len, loop_start + loop_len, exact


def _finish_loop(x, loop_start, n, sr, xfade_s):
    x = S.crossfade_loop(x, loop_start, n, S.frames(sr, xfade_s))
    return x, (loop_start, n)


def _loop_rate(sr, loop_len, cycles=1):
    """Vibrato/LFO rate that completes ``cycles`` whole periods per loop."""
    return cycles * sr / loop_len


# ------------------------------------------------------------------------------ keys

def piano(sr, rng, length=0.6, freq=261.63, bright=False):
    """Additive piano: 28 partials with stiffness inharmonicity, faster decay for upper
    partials, a short low-passed hammer noise and a global decay."""
    n = S.frames(sr, length)
    rolloff = 1.0 if bright else 1.6
    tau0 = 0.45 if bright else 0.5
    inharm = 0.0007 if bright else 0.0004
    partials = []
    for k in range(1, 29):
        amp = 1.0 / (k ** rolloff)
        if k % 2 == 0:
            amp *= 0.85
        partials.append((float(k), amp, tau0 / (1.0 + 0.12 * k)))
    body = S.additive(n, sr, freq, partials, inharm, phase=0.0)
    hammer = S.lowpass1(S.white(max(8, S.frames(sr, 0.005)), rng), sr, 3000.0)
    hammer = S.mul(hammer, S.exp_decay(len(hammer), sr, 0.002))
    S.add_at(body, hammer, 0, 0.3 if bright else 0.15)
    env = S.exp_decay(n, sr, 0.25)
    att = _attack(n, sr, 0.001)
    return [b * e * a for b, e, a in zip(body, env, att)]


def epiano(sr, rng, length=0.6, freq=261.63):
    """FM electric piano: 1:1 carrier/modulator with a fast index 'bark', a 7x tine
    partial and a 5 kHz roll-off."""
    n = S.frames(sr, length)
    idx_env = S.exp_decay(n, sr, 0.06, start=1.0, floor=0.15)
    body = S.fm2(n, sr, freq, 1.0, 1.8, idx_env)
    tine = S.mul(S.osc(n, sr, freq * 7.0, "sine"), S.exp_decay(n, sr, 0.05))
    out = S.mix(body, tine, gains=(1.0, 0.12))
    out = S.mul(out, S.exp_decay(n, sr, 0.3))
    out = S.mul(out, _attack(n, sr, 0.002))
    return S.lowpass1(out, sr, 5000.0)


def organ(sr, rng, freq=220.0, percussive=False, head_s=0.1, min_loop=3200):
    """Drawbar organ: sine partials at 16', 8', 5 1/3', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1'.
    ``percussive`` adds decaying 2nd/3rd harmonic 'percussion' before the loop."""
    if percussive:
        head_s = 0.35
    loop_start, loop_len, n, f = _loop_plan(sr, freq, head_s, min_loop)
    if percussive:
        drawbars = [(1.0, 1.0, 0.0), (2.0, 0.7, 0.0), (3.0, 0.3, 0.0),
                    (3.0, 1.0, 0.12), (2.0, 0.5, 0.12)]
    else:
        drawbars = [(0.5, 0.8, 0.0), (1.0, 1.0, 0.0), (1.5, 0.6, 0.0), (2.0, 0.8, 0.0),
                    (3.0, 0.5, 0.0), (4.0, 0.4, 0.0), (5.0, 0.2, 0.0), (6.0, 0.25, 0.0),
                    (8.0, 0.15, 0.0)]
    x = S.additive(n, sr, f, drawbars)
    click = S.mul(S.highpass1(S.white(max(8, S.frames(sr, 0.003)), rng), sr, 2000.0),
                  S.exp_decay(max(8, S.frames(sr, 0.003)), sr, 0.001))
    S.add_at(x, click, 0, 0.4)
    x = S.mul(x, _attack(n, sr, 0.006))
    return _finish_loop(x, loop_start, n, sr, 0.02)


# --------------------------------------------------------------------------- strings

def strings_ensemble(sr, rng, freq=220.0, head_s=0.22, min_loop=4800):
    """Five detuned band-limited saws, chorus, 4.5 kHz low-pass, slow attack, looped."""
    loop_start, loop_len, n, f = _loop_plan(sr, freq, head_s, min_loop)
    x = S.zeros(n)
    for cents in (-14.0, -7.0, 0.0, 6.0, 13.0):
        S.add_at(x, S.bl_osc("saw", n, sr, S.detune(f, cents), phase=rng.random()), 0, 0.2)
    x = S.chorus(x, sr, _loop_rate(sr, loop_len, 1), 1.5, 9.0, 0.4)
    x = S.lowpass1(x, sr, 4500.0)
    x = S.mul(x, _attack(n, sr, 0.14))
    return _finish_loop(x, loop_start, n, sr, 0.06)


def strings_pizz(sr, rng, freq=220.0, length=0.3):
    """Pizzicato: Karplus-Strong with a bright pick, strong damping, 3 kHz roll-off."""
    n = S.frames(sr, length)
    x = S.karplus_strong(n, sr, freq, rng, decay=0.985, brightness=0.6)
    x = S.lowpass1(x, sr, 3000.0)
    return S.mul(x, S.exp_decay(n, sr, 0.09))


def brass_section(sr, rng, freq=220.0, head_s=0.25, min_loop=4800):
    """Three detuned saws through a resonant low-pass that opens over 70 ms, fitted
    vibrato, looped."""
    loop_start, loop_len, n, f = _loop_plan(sr, freq, head_s, min_loop)
    rate = _loop_rate(sr, loop_len, 1)
    x = S.zeros(n)
    for cents in (-8.0, 0.0, 7.0):
        freqs = _vibrato(n, sr, S.detune(f, cents), 7.0, rate, 0.12, 0.08)
        S.add_at(x, S.bl_osc("saw", n, sr, freqs, phase=rng.random()), 0, 0.33)
    cutoff = [2800.0 - 2300.0 * math.exp(-i / (0.07 * sr)) for i in range(n)]
    x = S.svf(x, sr, cutoff, 1.4, "lp")
    x = S.mul(x, _attack(n, sr, 0.03))
    return _finish_loop(x, loop_start, n, sr, 0.05)


def trumpet(sr, rng, freq=440.0, head_s=0.2, min_loop=6400):
    """Pulse + saw, fast filter opening, small pitch overshoot at the attack, vibrato."""
    loop_start, loop_len, n, f = _loop_plan(sr, freq, head_s, min_loop)
    rate = _loop_rate(sr, loop_len, 1)
    freqs = _vibrato(n, sr, f, 9.0, rate, 0.1, 0.08)
    over = S.exp_decay(n, sr, 0.02, start=1.015, floor=1.0)
    freqs = [a * b for a, b in zip(freqs, over)]
    x = S.mix(S.bl_osc("pulse", n, sr, freqs, duty=0.35), S.bl_osc("saw", n, sr, freqs),
              gains=(0.7, 0.5))
    cutoff = [4200.0 - 3300.0 * math.exp(-i / (0.03 * sr)) for i in range(n)]
    x = S.svf(x, sr, cutoff, 1.2, "lp")
    x = S.mul(x, _attack(n, sr, 0.02))
    return _finish_loop(x, loop_start, n, sr, 0.04)


# ---------------------------------------------------------------------------- guitars

def guitar(sr, rng, freq=220.0, length=0.6, kind="nylon"):
    """Karplus-Strong guitar. nylon: dark pick, medium decay; steel: bright pick, long
    decay with a detuned second string; muted: heavy damping, very short."""
    n = S.frames(sr, length)
    if kind == "nylon":
        x = S.karplus_strong(n, sr, freq, rng, decay=0.9975, brightness=0.35,
                             excite_lowpass=2500.0)
        x = S.lowpass1(x, sr, 3500.0)
    elif kind == "steel":
        x = S.karplus_strong(n, sr, freq, rng, decay=0.9985, brightness=0.7)
        y = S.karplus_strong(n, sr, S.detune(freq, 3.0), rng, decay=0.998, brightness=0.6)
        x = S.mix(x, y, gains=(1.0, 0.3))
        x = S.lowpass1(x, sr, 6000.0)
    elif kind == "muted":
        x = S.karplus_strong(n, sr, freq, rng, decay=0.93, brightness=0.4)
        x = S.lowpass1(x, sr, 3000.0)
    else:
        raise ValueError(kind)
    return S.mul(x, S.exp_decay(n, sr, length * 0.6))


# --------------------------------------------------------------------------- winds

def flute(sr, rng, freq=440.0, head_s=0.2, min_loop=6400):
    """Sine with weak 2nd-4th harmonics, breath noise band-passed at the pitch plus a
    little high-passed air, 50 ms attack, fitted vibrato, looped."""
    loop_start, loop_len, n, f = _loop_plan(sr, freq, head_s, min_loop)
    rate = _loop_rate(sr, loop_len, 1)
    freqs = _vibrato(n, sr, f, 12.0, rate, 0.1, 0.1)
    x = S.zeros(n)
    for k, g in ((1.0, 1.0), (2.0, 0.35), (3.0, 0.12), (4.0, 0.05)):
        S.add_at(x, S.osc(n, sr, [v * k for v in freqs], "sine"), 0, g)
    breath = S.biquad(S.white(n, rng), sr, "bp", f, 6.0)
    air = S.biquad(S.white(n, rng), sr, "hp", 3000.0, 0.7)
    x = S.mix(x, breath, air, gains=(1.0, 0.25, 0.05))
    x = S.mul(x, _attack(n, sr, 0.05))
    return _finish_loop(x, loop_start, n, sr, 0.05)


def clarinet(sr, rng, freq=220.0, head_s=0.1, min_loop=3200):
    """Odd harmonics 1/k with a trace of even ones, 4 kHz roll-off, looped exactly."""
    loop_start, loop_len, n, f = _loop_plan(sr, freq, head_s, min_loop)
    partials = []
    for k in range(1, 41):
        amp = (1.0 / k) if k % 2 == 1 else (0.08 / k)
        partials.append((float(k), amp, 0.0))
    x = S.additive(n, sr, f, partials)
    x = S.lowpass1(x, sr, 4000.0)
    x = S.mul(x, _attack(n, sr, 0.04))
    return _finish_loop(x, loop_start, n, sr, 0.02)


def oboe(sr, rng, freq=440.0, head_s=0.2, min_loop=6400):
    """Narrow pulse through a double-reed formant bank (1.0/1.5/3 kHz), vibrato, looped."""
    loop_start, loop_len, n, f = _loop_plan(sr, freq, head_s, min_loop)
    rate = _loop_rate(sr, loop_len, 1)
    freqs = _vibrato(n, sr, f, 8.0, rate, 0.1, 0.08)
    src = S.bl_osc("pulse", n, sr, freqs, duty=0.12)
    bank = [(1000.0, 120.0, 1.0), (1500.0, 150.0, 0.7), (3000.0, 300.0, 0.3)]
    x = S.mix(S.formant(src, sr, bank), src, gains=(1.0, 0.3))
    x = S.mul(x, _attack(n, sr, 0.03))
    return _finish_loop(x, loop_start, n, sr, 0.04)


# ---------------------------------------------------------------------------- choir

def choir(sr, rng, vowel="a", freq=220.0, head_s=0.2, min_loop=6400):
    """Three detuned pulses with fitted vibrato through a vowel formant bank plus a
    little breath, 120 ms attack, looped."""
    loop_start, loop_len, n, f = _loop_plan(sr, freq, head_s, min_loop)
    rate = _loop_rate(sr, loop_len, 1)
    src = S.zeros(n)
    for cents, ph in ((-9.0, 0.1), (0.0, 0.5), (8.0, 0.8)):
        freqs = _vibrato(n, sr, S.detune(f, cents), 10.0, rate, 0.08, 0.1)
        S.add_at(src, S.bl_osc("pulse", n, sr, freqs, duty=0.4, phase=ph), 0, 0.33)
    S.add_at(src, S.white(n, rng), 0, 0.03)
    x = S.formant(src, sr, vowel)
    x = S.lowpass1(x, sr, 6000.0)
    x = S.mul(x, _attack(n, sr, 0.12))
    return _finish_loop(x, loop_start, n, sr, 0.06)


# ---------------------------------------------------------------------------- basses

def bass_synth(sr, rng, freq=110.0, length=0.4, cutoff_start=3500.0, cutoff_end=250.0,
               cutoff_tau=0.07, resonance=2.5, decay=0.3, sustain=0.4):
    """Saw plus sub-octave square through a resonant low-pass sweep, ADSR, light drive."""
    n = S.frames(sr, length)
    src = S.mix(S.bl_osc("saw", n, sr, freq), S.bl_osc("square", n, sr, freq * 0.5),
                gains=(1.0, 0.5))
    cutoff = S.pitch_sweep(n, sr, min(cutoff_start, sr * 0.35), cutoff_end, cutoff_tau)
    x = S.svf(src, sr, cutoff, resonance, "lp")
    env = S.adsr(n, sr, 0.002, decay, sustain, 0.08, max(0.0, length - 0.08))
    return S.soft_clip(S.mul(x, env), 1.2)


def bass_finger(sr, rng, freq=110.0, length=0.5):
    """Karplus-Strong with a dull pick, 1.5 kHz roll-off, light saturation."""
    n = S.frames(sr, length)
    x = S.karplus_strong(n, sr, freq, rng, decay=0.997, brightness=0.3, excite_lowpass=1200.0)
    x = S.lowpass1(x, sr, 1500.0)
    x = S.soft_clip(x, 1.3)
    return S.mul(x, S.exp_decay(n, sr, length * 0.6))


def bass_slap(sr, rng, freq=110.0, length=0.4):
    """Bright Karplus-Strong plus a high-passed slap transient, hard saturation."""
    n = S.frames(sr, length)
    x = S.karplus_strong(n, sr, freq, rng, decay=0.996, brightness=0.8)
    slap = S.mul(S.highpass1(S.white(S.frames(sr, 0.006), rng), sr, 2000.0),
                 S.exp_decay(S.frames(sr, 0.006), sr, 0.002))
    S.add_at(x, slap, 0, 0.8)
    x = S.soft_clip(x, 2.0)
    x = S.lowpass1(x, sr, 5000.0)
    return S.mul(x, S.exp_decay(n, sr, length * 0.6))


# ------------------------------------------------------------------- single cycles

def single_cycle(sr, rng, shape="saw", freq=220.0, min_loop=1600):
    """Band-limited waveform looped over a whole number of cycles (exactly in tune)."""
    loop_len, f = S.loop_length(sr, freq, min_loop, BLOCK)
    x = S.bl_osc(shape, loop_len, sr, f)
    return x, (0, loop_len)


def noise_loop(sr, rng, frames=4096):
    """White noise loop (loop over the whole file)."""
    return S.white(frames, rng), (0, frames)


# ------------------------------------------------------------------------------ pads

def pad_glass(sr, rng, freq=220.0, head_s=0.5, min_loop=4800):
    """Glassy pad: 1:3 FM plus detuned sine partials at 2x/4x/6x, 400 ms attack,
    slow chorus, looped."""
    loop_start, loop_len, n, f = _loop_plan(sr, freq, head_s, min_loop)
    x = S.fm2(n, sr, f, 3.0, 0.8)
    for ratio, cents, g in ((2.0, -5.0, 0.35), (3.98, 4.0, 0.2), (6.01, -3.0, 0.1)):
        S.add_at(x, S.osc(n, sr, S.detune(f * ratio, cents), "sine", phase=rng.random()), 0, g)
    x = S.chorus(x, sr, _loop_rate(sr, loop_len, 1), 2.0, 10.0, 0.45)
    x = S.mul(x, _attack(n, sr, 0.4))
    return _finish_loop(x, loop_start, n, sr, 0.15)


def pad_warm(sr, rng, freq=110.0, head_s=0.5, min_loop=6400):
    """Warm pad: four detuned saws, 1.5 kHz one-pole plus resonant SVF, 300 ms attack,
    chorus, looped."""
    loop_start, loop_len, n, f = _loop_plan(sr, freq, head_s, min_loop)
    x = S.zeros(n)
    for cents in (-12.0, -5.0, 4.0, 11.0):
        S.add_at(x, S.bl_osc("saw", n, sr, S.detune(f, cents), phase=rng.random()), 0, 0.25)
    x = S.lowpass1(x, sr, 1500.0)
    x = S.svf(x, sr, 1200.0, 1.0, "lp")
    x = S.chorus(x, sr, _loop_rate(sr, loop_len, 1), 2.5, 12.0, 0.4)
    x = S.mul(x, _attack(n, sr, 0.3))
    return _finish_loop(x, loop_start, n, sr, 0.15)
