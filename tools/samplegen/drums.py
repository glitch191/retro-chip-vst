"""Percussion and one-shot effect builders.

Every builder has the signature ``builder(sr, rng, **params) -> list[float]`` and
returns an un-normalised mono signal at ``sr`` Hz; ``gen_samples.py`` applies DC
removal, fades, normalisation and writes the file. Lengths are in seconds.
"""

import math

from . import synth as S

# Six square-wave frequencies of the classic analogue hi-hat/cymbal circuit (Hz).
METAL_HZ = (205.3, 304.4, 369.6, 522.7, 540.0, 800.0)


def _metallic(n, sr, rng, base_scale=1.0, ratios=METAL_HZ):
    """Sum of six square waves at non-harmonic frequencies (cymbal bed)."""
    out = S.zeros(n)
    for hz in ratios:
        f = hz * base_scale
        if f >= sr * 0.45:
            continue
        S.add_at(out, S.osc(n, sr, f, "square", phase=rng.random()), 0, 1.0 / len(ratios))
    return out


def _click(sr, rng, length=0.004, hp=1500.0):
    n = max(4, S.frames(sr, length))
    c = S.highpass1(S.white(n, rng), sr, hp)
    return S.mul(c, S.exp_decay(n, sr, length / 3.0))


# ----------------------------------------------------------------------------- kicks

def kick(sr, rng, length=0.3, f_start=150.0, f_end=50.0, sweep_tau=0.05, amp_tau=0.09,
         click=0.35, drive=1.6, lowpass=None):
    """Sine with an exponential pitch drop, exponential decay, noise click, tanh drive."""
    n = S.frames(sr, length)
    freqs = S.pitch_sweep(n, sr, f_start, f_end, sweep_tau)
    body = S.mul(S.osc(n, sr, freqs, "sine"), S.exp_decay(n, sr, amp_tau))
    out = S.soft_clip(body, drive)
    if click > 0:
        S.add_at(out, _click(sr, rng), 0, click)
    if lowpass:
        out = S.lowpass1(out, sr, lowpass)
    return out


def kick_808(sr, rng, length=0.4, f_start=85.0, f_end=46.0, sweep_tau=0.04, amp_tau=0.16,
             drive=2.6):
    """Long, round 808-style kick: slow sine decay, heavier saturation, 3 kHz roll-off."""
    out = kick(sr, rng, length, f_start, f_end, sweep_tau, amp_tau, click=0.15,
               drive=drive)
    return S.lowpass1(out, sr, 3000.0)


# ---------------------------------------------------------------------------- snares

def snare(sr, rng, length=0.22, tone_freqs=(185.0, 331.0), tone_tau=0.045, noise_tau=0.07,
          noise_level=0.8, tone_level=0.7, noise_hp=700.0, drive=1.3):
    """Two decaying sine modes (shell) plus high-passed white noise (snares)."""
    n = S.frames(sr, length)
    tone = S.zeros(n)
    for k, f in enumerate(tone_freqs):
        freqs = S.pitch_sweep(n, sr, f * 1.35, f, 0.012)
        S.add_at(tone, S.mul(S.osc(n, sr, freqs, "sine"), S.exp_decay(n, sr, tone_tau)),
                 0, 1.0 / (k + 1))
    noise = S.highpass1(S.white(n, rng), sr, noise_hp)
    noise = S.mul(noise, S.exp_decay(n, sr, noise_tau))
    out = S.mix(tone, noise, gains=(tone_level, noise_level))
    return S.soft_clip(out, drive)


def rim(sr, rng, length=0.06, f1=1750.0, f2=620.0, tau=0.018):
    """Rimshot: short noise burst ringing two resonant band-pass filters plus a click."""
    n = S.frames(sr, length)
    burst = S.zeros(n)
    S.add_at(burst, S.white(max(4, S.frames(sr, 0.002)), rng), 0, 1.0)
    ring = S.mix(S.biquad(burst, sr, "bp", f1, 8.0), S.biquad(burst, sr, "bp", f2, 5.0),
                 gains=(1.0, 0.7))
    out = S.mul(ring, S.exp_decay(n, sr, tau))
    S.add_at(out, _click(sr, rng, 0.002, 2500.0), 0, 0.3)
    return out


# ------------------------------------------------------------------------------ toms

def tom(sr, rng, length=0.3, freq=110.0, sweep=1.6, sweep_tau=0.03, drive=1.2):
    """Tom: sine with a fast pitch drop, a weak second harmonic and a short noise attack."""
    n = S.frames(sr, length)
    freqs = S.pitch_sweep(n, sr, freq * sweep, freq, sweep_tau)
    env = S.exp_decay(n, sr, length / 3.5)
    body = S.mul(S.osc(n, sr, freqs, "sine"), env)
    second = S.mul(S.osc(n, sr, [f * 2.0 for f in freqs], "sine"), S.exp_decay(n, sr, length / 8.0))
    out = S.mix(body, second, gains=(1.0, 0.25))
    S.add_at(out, _click(sr, rng, 0.006, 800.0), 0, 0.25)
    return S.soft_clip(out, drive)


# ---------------------------------------------------------------------------- cymbals

def hat(sr, rng, length=0.08, open_hat=False, tau=None, noise_level=0.35):
    """Hi-hat: six-square metallic bed plus noise, high-passed, exponential decay."""
    n = S.frames(sr, length)
    if tau is None:
        tau = length / 3.0 if open_hat else 0.02
    bed = _metallic(n, sr, rng, base_scale=1.0)
    src = S.mix(bed, S.white(n, rng), gains=(1.0, noise_level))
    hp = min(6500.0, sr * 0.28)
    src = S.biquad(src, sr, "hp", hp, 0.9)
    src = S.biquad(src, sr, "hp", hp * 0.8, 0.7)
    return S.mul(src, S.exp_decay(n, sr, tau))


def crash(sr, rng, length=0.5, tau=None, hp=3000.0):
    """Crash: detuned metallic bed and white noise, high-passed, longer decay."""
    n = S.frames(sr, length)
    if tau is None:
        tau = length / 3.0
    bed = _metallic(n, sr, rng, base_scale=1.37)
    bed2 = _metallic(n, sr, rng, base_scale=2.11)
    src = S.mix(bed, bed2, S.white(n, rng), gains=(0.6, 0.5, 1.0))
    src = S.biquad(src, sr, "hp", min(hp, sr * 0.2), 0.7)
    env = S.exp_decay(n, sr, tau)
    att = max(1, S.frames(sr, 0.002))
    for i in range(att):
        env[i] *= (i + 1) / att
    return S.mul(src, env)


def ride(sr, rng, length=0.6, tau=None, ping=3200.0):
    """Ride: metallic bed band-passed around 5 kHz plus a decaying sine 'ping'."""
    n = S.frames(sr, length)
    if tau is None:
        tau = length / 2.5
    bed = _metallic(n, sr, rng, base_scale=1.73)
    src = S.mix(bed, S.white(n, rng), gains=(1.0, 0.4))
    bp = S.biquad(src, sr, "bp", min(5000.0, sr * 0.2), 1.2)
    hp = S.biquad(src, sr, "hp", min(7000.0, sr * 0.25), 0.7)
    wash = S.mul(S.mix(bp, hp, gains=(1.0, 0.6)), S.exp_decay(n, sr, tau))
    bell = S.mul(S.osc(n, sr, min(ping, sr * 0.2), "sine"), S.exp_decay(n, sr, tau * 0.6))
    return S.mix(wash, bell, gains=(1.0, 0.35))


# ------------------------------------------------------------------- hand percussion

def clap(sr, rng, length=0.2, bursts=4, spacing=0.0095, burst_tau=0.006, tail_tau=0.06):
    """Hand clap: several short noise bursts then a longer tail, band-passed ~1.2 kHz."""
    n = S.frames(sr, length)
    env = S.zeros(n)
    for b in range(bursts):
        start = S.frames(sr, b * spacing)
        S.add_at(env, S.exp_decay(n - start, sr, burst_tau), start, 1.0)
    tail_start = S.frames(sr, bursts * spacing)
    S.add_at(env, S.exp_decay(n - tail_start, sr, tail_tau), tail_start, 0.8)
    src = S.white(n, rng)
    src = S.biquad(src, sr, "bp", 1200.0, 0.8)
    src = S.biquad(src, sr, "hp", 500.0, 0.7)
    return S.soft_clip(S.mul(src, env), 1.5)


def cowbell(sr, rng, length=0.25, f1=587.0, f2=845.0, tau=0.07):
    """Cowbell: two square waves through a band-pass, fast decay with a short sustain."""
    n = S.frames(sr, length)
    src = S.mix(S.osc(n, sr, f1, "square"), S.osc(n, sr, f2, "square"))
    bp = S.biquad(src, sr, "bp", 1800.0, 1.0)
    src = S.mix(src, bp, gains=(0.5, 1.0))
    env = S.exp_decay(n, sr, tau, floor=0.0)
    hit = S.exp_decay(n, sr, 0.008)
    env = [min(1.0, e + 0.6 * h) for e, h in zip(env, hit)]
    return S.soft_clip(S.mul(src, env), 1.4)


def shaker(sr, rng, length=0.12, grains=((0.0, 1.0), (0.055, 0.55)), attack=0.004, tau=0.035):
    """Shaker: high-passed white noise shaped by a couple of quick grains."""
    n = S.frames(sr, length)
    env = S.zeros(n)
    a = max(1, S.frames(sr, attack))
    for start_s, level in grains:
        start = S.frames(sr, start_s)
        g = S.exp_decay(n - start, sr, tau)
        for i in range(min(a, len(g))):
            g[i] *= (i + 1) / a
        S.add_at(env, g, start, level)
    src = S.biquad(S.white(n, rng), sr, "hp", min(5000.0, sr * 0.22), 0.8)
    return S.mul(src, env)


# ------------------------------------------------------------------- noise and hits

def noise_burst(sr, rng, length=0.1, tau=0.04, f_start=9000.0, f_end=600.0, resonance=1.2):
    """White noise through a low-pass whose cutoff sweeps down, exponential decay."""
    n = S.frames(sr, length)
    cut = S.pitch_sweep(n, sr, min(f_start, sr * 0.35), f_end, length / 3.0)
    src = S.svf(S.white(n, rng), sr, cut, resonance, "lp")
    return S.mul(src, S.exp_decay(n, sr, tau))


def noise_filtered(sr, rng, length=0.3, centre=2200.0, q=2.5, tau=0.09):
    """Band-passed white noise burst (resonant 'pshh')."""
    n = S.frames(sr, length)
    src = S.biquad(S.white(n, rng), sr, "bp", centre, q)
    return S.mul(src, S.exp_decay(n, sr, tau))


def orchestra_hit(sr, rng, length=0.35, root=130.81, intervals=(0, 7, 12, 16, 19, 24),
                  hold=0.04, tau=0.09, lowpass=6000.0, drive=1.3, chunky=False):
    """Orchestra hit: detuned band-limited saw/square chord, noise transient, fast decay.

    ``chunky`` adds a low FM brass layer and more saturation (Genesis "sega hit").
    """
    n = S.frames(sr, length)
    out = S.zeros(n)
    for k, semis in enumerate(intervals):
        f = root * 2.0 ** (semis / 12.0)
        for shape, cents, g in (("saw", -6.0, 1.0), ("square", 5.0, 0.5), ("saw", 9.0, 0.7)):
            S.add_at(out, S.bl_osc(shape, n, sr, S.detune(f, cents), phase=rng.random()),
                     0, g / (1.0 + 0.15 * k))
    if chunky:
        idx_env = S.exp_decay(n, sr, 0.12, start=1.0, floor=0.3)
        brass = S.fm2(n, sr, root, 1.0, 2.2, idx_env)
        S.add_at(out, brass, 0, 2.5)
        drive *= 1.6
        lowpass = min(lowpass, 4000.0)
    noise = S.mul(S.highpass1(S.white(n, rng), sr, 1500.0), S.exp_decay(n, sr, 0.012))
    S.add_at(out, noise, 0, 2.0)
    env = S.adsr(n, sr, 0.003, tau, 0.0, tau, hold)
    out = S.mul(out, env)
    out = S.lowpass1(out, sr, min(lowpass, sr * 0.4))
    peak = max(abs(v) for v in out) or 1.0
    return S.soft_clip([v / peak for v in out], drive)


def voice(sr, rng, length=0.09, vowel="e", f_start=240.0, f_end=190.0, sweep_tau=0.05,
          attack=0.005, tau=0.04, breath=0.05):
    """Formant voice blip: narrow pulse with a falling pitch through a vowel filter bank."""
    n = S.frames(sr, length)
    freqs = S.pitch_sweep(n, sr, f_start, f_end, sweep_tau)
    src = S.bl_osc("pulse", n, sr, freqs, duty=0.2)
    if breath > 0:
        S.add_at(src, S.white(n, rng), 0, breath)
    voiced = S.formant(src, sr, vowel)
    env = S.adsr(n, sr, attack, tau, 0.0, tau, attack + tau * 0.5)
    return S.mul(voiced, env)
