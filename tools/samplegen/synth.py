"""Synthesis primitives: oscillators, envelopes, noise, filters, physical and spectral models.

Everything works on plain Python lists of floats (nominal range [-1, 1]) and takes the
sample rate explicitly. All randomness goes through a ``random.Random`` instance passed
by the caller so that every sample is reproducible from its seed.

Contents:

* oscillators: naive ``osc`` (per-sample frequency), band-limited ``bl_osc`` built from
  a cached additive wavetable, ``additive`` partial synthesis with inharmonicity and
  per-partial decay, two-operator ``fm2``;
* physical: ``karplus_strong`` plucked string (fractional delay, damping, decay);
* formants: ``formant`` bank of resonant band-pass filters with vowel presets;
* envelopes: ``exp_decay``, ``adsr``, ``ramp``, ``pitch_sweep``;
* noise: ``white`` and a NES-style ``lfsr_noise``;
* filters: one-pole ``lowpass1``/``highpass1``, RBJ ``biquad`` (lp/hp/bp/notch/peak),
  Chamberlin ``svf`` with per-sample cutoff, ``chorus``;
* utilities: ``mix``, ``mul``, ``scale``, ``remove_dc``, ``normalize``, ``fade``,
  ``soft_clip``, ``resample`` (windowed sinc), ``crossfade_loop``, ``pad_to_multiple``,
  ``loop_length`` (frames for an integer number of cycles on a block boundary).
"""

import math
import zlib
from functools import lru_cache

TWO_PI = 2.0 * math.pi


# --------------------------------------------------------------------------- helpers

def seed_for(*parts):
    """Stable 32-bit seed from any sequence of strings/numbers (never ``hash()``)."""
    text = "|".join(str(p) for p in parts)
    return zlib.crc32(text.encode("utf-8")) & 0xFFFFFFFF


def frames(sr, seconds):
    return int(round(sr * seconds))


def zeros(n):
    return [0.0] * n


def midi_to_hz(note):
    return 440.0 * 2.0 ** ((note - 69) / 12.0)


def _freq_list(freq, n):
    if isinstance(freq, (int, float)):
        return None
    if len(freq) != n:
        raise ValueError("frequency list length %d != %d" % (len(freq), n))
    return freq


# ------------------------------------------------------------------------ oscillators

def osc(n, sr, freq, shape="sine", duty=0.5, phase=0.0):
    """Naive oscillator. ``freq`` is a float or a per-sample list. ``phase`` in cycles."""
    out = zeros(n)
    fl = _freq_list(freq, n)
    ph = phase % 1.0
    inv = 1.0 / sr
    sin = math.sin
    if fl is None:
        inc = freq * inv
    for i in range(n):
        if fl is not None:
            inc = fl[i] * inv
        if shape == "sine":
            out[i] = sin(TWO_PI * ph)
        elif shape == "saw":
            out[i] = 2.0 * ph - 1.0
        elif shape == "square":
            out[i] = 1.0 if ph < 0.5 else -1.0
        elif shape == "pulse":
            out[i] = 1.0 if ph < duty else -1.0
        elif shape == "triangle":
            out[i] = 4.0 * ph - 1.0 if ph < 0.5 else 3.0 - 4.0 * ph
        else:
            raise ValueError("unknown shape " + shape)
        ph += inc
        if ph >= 1.0:
            ph -= int(ph)
    return out


@lru_cache(maxsize=64)
def wavetable(shape, harmonics, duty=0.5, size=2048):
    """One band-limited cycle of ``shape`` with ``harmonics`` partials, peak-normalised."""
    table = [0.0] * size
    sin = math.sin
    cos = math.cos
    for k in range(1, harmonics + 1):
        if shape == "sine":
            if k > 1:
                break
            a = 1.0
            use_cos = False
        elif shape == "saw":
            a = (2.0 / (math.pi * k)) * (1.0 if k % 2 == 1 else -1.0)
            use_cos = False
        elif shape == "square":
            if k % 2 == 0:
                continue
            a = 4.0 / (math.pi * k)
            use_cos = False
        elif shape == "triangle":
            if k % 2 == 0:
                continue
            a = (8.0 / (math.pi ** 2)) / (k * k) * (1.0 if ((k - 1) // 2) % 2 == 0 else -1.0)
            use_cos = False
        elif shape == "pulse":
            a = (2.0 / (math.pi * k)) * sin(math.pi * k * duty)
            use_cos = True
        else:
            raise ValueError("unknown shape " + shape)
        if a == 0.0:
            continue
        step = TWO_PI * k / size
        if use_cos:
            for i in range(size):
                table[i] += a * cos(step * i)
        else:
            for i in range(size):
                table[i] += a * sin(step * i)
    peak = max(abs(v) for v in table) or 1.0
    return tuple(v / peak for v in table)


def table_osc(table, n, sr, freq, phase=0.0):
    """Read a wavetable with linear interpolation. ``freq`` float or per-sample list."""
    out = zeros(n)
    size = len(table)
    fl = _freq_list(freq, n)
    ph = phase % 1.0
    inv = 1.0 / sr
    if fl is None:
        inc = freq * inv
    for i in range(n):
        if fl is not None:
            inc = fl[i] * inv
        idx = ph * size
        i0 = int(idx)
        frac = idx - i0
        i1 = i0 + 1
        if i1 >= size:
            i1 -= size
        v0 = table[i0]
        out[i] = v0 + (table[i1] - v0) * frac
        ph += inc
        if ph >= 1.0:
            ph -= int(ph)
    return out


def bl_osc(shape, n, sr, freq, duty=0.5, phase=0.0, max_harmonics=256):
    """Band-limited oscillator: harmonics up to Nyquist for the highest frequency used."""
    fl = _freq_list(freq, n)
    top = max(fl) if fl is not None else freq
    harmonics = int((sr * 0.5) / max(top, 1.0))
    harmonics = max(1, min(max_harmonics, harmonics))
    table = wavetable(shape, harmonics, round(duty, 4))
    return table_osc(table, n, sr, freq, phase)


def additive(n, sr, f0, partials, inharmonicity=0.0, phase=0.0):
    """Sum of decaying sine partials.

    ``partials`` is a list of ``(ratio, amplitude, decay_tau_seconds)``; ``decay_tau``
    ``<= 0`` means no decay. Partial k sounds at ``f0 * ratio * sqrt(1 + B * ratio^2)``
    (piano-like stiffness) and partials above Nyquist are skipped.
    """
    out = zeros(n)
    sin = math.sin
    nyq = sr * 0.5
    for ratio, amp, tau in partials:
        fk = f0 * ratio * math.sqrt(1.0 + inharmonicity * ratio * ratio)
        if fk >= nyq or amp == 0.0:
            continue
        inc = TWO_PI * fk / sr
        dec = math.exp(-1.0 / (tau * sr)) if tau > 0 else 1.0
        a = amp
        ph = TWO_PI * phase
        for i in range(n):
            out[i] += a * sin(ph)
            ph += inc
            a *= dec
        # keep the phase bounded for very long renders (accuracy only)
    return out


def fm2(n, sr, freq, ratio, index, index_env=None, phase_mod=0.0):
    """Two-operator FM: sine carrier at ``freq`` modulated by a sine at ``freq * ratio``.

    ``index`` is the peak modulation index (radians); ``index_env`` is an optional
    per-sample multiplier list. ``freq`` may be a per-sample list.
    """
    out = zeros(n)
    fl = _freq_list(freq, n)
    inv = TWO_PI / sr
    sin = math.sin
    phc = 0.0
    phm = TWO_PI * phase_mod
    if fl is None:
        incc = freq * inv
        incm = incc * ratio
    for i in range(n):
        if fl is not None:
            incc = fl[i] * inv
            incm = incc * ratio
        idx = index * (index_env[i] if index_env is not None else 1.0)
        out[i] = sin(phc + idx * sin(phm))
        phc += incc
        phm += incm
        if phc > TWO_PI:
            phc -= TWO_PI
        if phm > TWO_PI:
            phm -= TWO_PI
    return out


# ------------------------------------------------------------------------------ noise

def white(n, rng, amp=1.0):
    uniform = rng.uniform
    return [uniform(-amp, amp) for _ in range(n)]


def lfsr_noise(n, sr, clock_hz, short=False, seed_state=1):
    """NES-style 15-bit LFSR noise sampled and held at ``clock_hz``; +/-1 output."""
    out = zeros(n)
    state = seed_state & 0x7FFF or 1
    tap = 6 if short else 1
    acc = 0.0
    step = clock_hz / sr
    value = 1.0 if (state & 1) == 0 else -1.0
    for i in range(n):
        acc += step
        while acc >= 1.0:
            acc -= 1.0
            feedback = (state & 1) ^ ((state >> tap) & 1)
            state = (state >> 1) | (feedback << 14)
            value = 1.0 if (state & 1) == 0 else -1.0
        out[i] = value
    return out


# ------------------------------------------------------------------------- envelopes

def exp_decay(n, sr, tau, start=1.0, floor=0.0):
    """Exponential decay from ``start`` towards ``floor`` with time constant ``tau`` s."""
    out = zeros(n)
    dec = math.exp(-1.0 / (tau * sr)) if tau > 0 else 0.0
    v = start - floor
    for i in range(n):
        out[i] = floor + v
        v *= dec
    return out


def ramp(n, start, end):
    if n <= 1:
        return [end] * n
    step = (end - start) / (n - 1)
    return [start + step * i for i in range(n)]


def adsr(n, sr, attack, decay, sustain, release, hold):
    """Linear attack, exponential decay to ``sustain``, exponential release after ``hold`` s."""
    out = zeros(n)
    a = max(1, frames(sr, attack))
    gate = min(n, max(a, frames(sr, hold)))
    dec = math.exp(-1.0 / (max(decay, 1e-4) * sr))
    rel = math.exp(-1.0 / (max(release, 1e-4) * sr))
    v = 0.0
    for i in range(n):
        if i < a:
            v = (i + 1) / a
        elif i < gate:
            v = sustain + (v - sustain) * dec
        else:
            v *= rel
        out[i] = v
    return out


def pitch_sweep(n, sr, f_start, f_end, tau):
    """Per-sample frequency list decaying exponentially from ``f_start`` to ``f_end``."""
    return exp_decay(n, sr, tau, start=f_start, floor=f_end)


# ---------------------------------------------------------------------------- filters

def lowpass1(x, sr, cutoff):
    """One-pole low-pass."""
    k = 1.0 - math.exp(-TWO_PI * cutoff / sr)
    out = zeros(len(x))
    y = 0.0
    for i, v in enumerate(x):
        y += k * (v - y)
        out[i] = y
    return out


def highpass1(x, sr, cutoff):
    """One-pole high-pass (input minus its low-passed copy)."""
    k = 1.0 - math.exp(-TWO_PI * cutoff / sr)
    out = zeros(len(x))
    y = 0.0
    for i, v in enumerate(x):
        y += k * (v - y)
        out[i] = v - y
    return out


def biquad(x, sr, kind, f0, q=0.7071, gain_db=0.0):
    """RBJ cookbook biquad: ``kind`` in lp, hp, bp (0 dB peak), notch, peak."""
    f0 = min(f0, sr * 0.49)
    w0 = TWO_PI * f0 / sr
    cw = math.cos(w0)
    sw = math.sin(w0)
    alpha = sw / (2.0 * max(q, 1e-3))
    if kind == "lp":
        b0 = (1.0 - cw) / 2.0
        b1 = 1.0 - cw
        b2 = b0
        a0 = 1.0 + alpha
        a1 = -2.0 * cw
        a2 = 1.0 - alpha
    elif kind == "hp":
        b0 = (1.0 + cw) / 2.0
        b1 = -(1.0 + cw)
        b2 = b0
        a0 = 1.0 + alpha
        a1 = -2.0 * cw
        a2 = 1.0 - alpha
    elif kind == "bp":
        b0 = alpha
        b1 = 0.0
        b2 = -alpha
        a0 = 1.0 + alpha
        a1 = -2.0 * cw
        a2 = 1.0 - alpha
    elif kind == "notch":
        b0 = 1.0
        b1 = -2.0 * cw
        b2 = 1.0
        a0 = 1.0 + alpha
        a1 = -2.0 * cw
        a2 = 1.0 - alpha
    elif kind == "peak":
        big_a = 10.0 ** (gain_db / 40.0)
        b0 = 1.0 + alpha * big_a
        b1 = -2.0 * cw
        b2 = 1.0 - alpha * big_a
        a0 = 1.0 + alpha / big_a
        a1 = -2.0 * cw
        a2 = 1.0 - alpha / big_a
    else:
        raise ValueError("unknown biquad kind " + kind)
    b0 /= a0
    b1 /= a0
    b2 /= a0
    a1 /= a0
    a2 /= a0
    out = zeros(len(x))
    z1 = 0.0
    z2 = 0.0
    for i, v in enumerate(x):
        y = b0 * v + z1
        z1 = b1 * v - a1 * y + z2
        z2 = b2 * v - a2 * y
        out[i] = y
    return out


def svf(x, sr, cutoff, resonance=0.7, mode="lp"):
    """Chamberlin state-variable filter, 2x oversampled, per-sample cutoff allowed.

    ``resonance`` ~0.5 (flat) .. 8 (strongly resonant). ``mode`` lp, hp or bp.
    """
    n = len(x)
    cl = _freq_list(cutoff, n)
    out = zeros(n)
    q = 1.0 / max(resonance, 0.5)
    limit = sr * 0.35
    low = band = 0.0
    sin = math.sin
    k = math.pi / (2.0 * sr)
    if cl is None:
        f = 2.0 * sin(k * min(cutoff, limit))
    for i in range(n):
        if cl is not None:
            f = 2.0 * sin(k * min(cl[i], limit))
        v = x[i]
        for _ in range(2):
            low += f * band
            high = v - low - q * band
            band += f * high
        if mode == "lp":
            out[i] = low
        elif mode == "hp":
            out[i] = high
        else:
            out[i] = band
    return out


def chorus(x, sr, rate_hz, depth_ms=2.0, base_ms=8.0, mix=0.5, phase=0.0):
    """Single-voice chorus: modulated delay line mixed with the dry signal."""
    n = len(x)
    out = zeros(n)
    max_delay = int((base_ms + depth_ms) * 0.001 * sr) + 2
    buf = zeros(max_delay + n)
    sin = math.sin
    for i in range(n):
        buf[i] = x[i]
        d = (base_ms + depth_ms * sin(TWO_PI * (rate_hz * i / sr + phase))) * 0.001 * sr
        pos = i - d
        j = int(math.floor(pos))
        frac = pos - j
        a = buf[j] if j >= 0 else 0.0
        b = buf[j + 1] if j + 1 >= 0 else 0.0
        wet = a + (b - a) * frac
        out[i] = x[i] * (1.0 - mix) + wet * mix
    return out


# ---------------------------------------------------------------------- string model

def karplus_strong(n, sr, freq, rng, decay=0.996, brightness=0.5, excite=None,
                   excite_lowpass=None):
    """Karplus-Strong plucked string.

    Fractional delay line of ``sr / freq`` samples fed back through a one-zero low-pass
    (``brightness`` 0 = dark .. 1 = no damping) scaled by ``decay`` per period. The
    excitation is a burst of white noise one period long unless ``excite`` is given.
    """
    length = sr / freq
    b = min(max(brightness, 0.0), 1.0)
    eff = max(2.0, length - (1.0 - b))  # compensate the filter group delay
    li = int(eff)
    frac = eff - li
    y = zeros(n)
    if excite is None:
        burst = white(li + 1, rng)
        if excite_lowpass:
            burst = lowpass1(burst, sr, excite_lowpass)
    else:
        burst = list(excite)
    prev = 0.0
    bl = len(burst)
    for i in range(n):
        j = i - li
        if j >= 1:
            d = y[j] * (1.0 - frac) + y[j - 1] * frac
        elif j == 0:
            d = y[0] * (1.0 - frac)
        else:
            d = 0.0
        f = b * d + (1.0 - b) * prev
        prev = d
        v = decay * f
        if i < bl:
            v += burst[i]
        y[i] = v
    return y


# --------------------------------------------------------------------------- formants

# (centre Hz, bandwidth Hz, gain) — generic male voice values
VOWELS = {
    "a": [(700.0, 130.0, 1.0), (1220.0, 150.0, 0.5), (2600.0, 200.0, 0.25)],
    "e": [(500.0, 100.0, 1.0), (1750.0, 130.0, 0.4), (2450.0, 160.0, 0.25)],
    "i": [(300.0, 80.0, 1.0), (2300.0, 150.0, 0.3), (3000.0, 200.0, 0.15)],
    "o": [(500.0, 100.0, 1.0), (900.0, 120.0, 0.5), (2400.0, 180.0, 0.15)],
    "u": [(330.0, 80.0, 1.0), (800.0, 100.0, 0.4), (2300.0, 180.0, 0.1)],
    "uh": [(620.0, 100.0, 1.0), (1200.0, 130.0, 0.4), (2400.0, 180.0, 0.15)],
}


def formant(x, sr, vowel):
    """Parallel bank of resonant band-pass filters. ``vowel`` is a key of VOWELS or a list."""
    bank = VOWELS[vowel] if isinstance(vowel, str) else vowel
    out = zeros(len(x))
    for fc, bw, gain in bank:
        if fc >= sr * 0.45:
            continue
        y = biquad(x, sr, "bp", fc, fc / bw)
        for i, v in enumerate(y):
            out[i] += gain * v
    return out


# -------------------------------------------------------------------------- utilities

def mix(*signals, gains=None):
    """Sum signals (shorter ones are zero-padded); optional per-signal gains."""
    n = max(len(s) for s in signals)
    out = zeros(n)
    for idx, s in enumerate(signals):
        g = gains[idx] if gains is not None else 1.0
        for i, v in enumerate(s):
            out[i] += g * v
    return out


def mul(x, env):
    return [a * b for a, b in zip(x, env)]


def scale(x, g):
    return [v * g for v in x]


def add_at(dst, src, offset, gain=1.0):
    """Add ``src`` into ``dst`` starting at ``offset`` (clipped to dst length)."""
    n = len(dst)
    for i, v in enumerate(src):
        j = offset + i
        if j >= n:
            break
        if j >= 0:
            dst[j] += gain * v
    return dst


def remove_dc(x):
    if not x:
        return x
    m = sum(x) / len(x)
    return [v - m for v in x]


def normalize(x, peak_db=-1.0):
    peak = max((abs(v) for v in x), default=0.0)
    if peak <= 0.0:
        return list(x)
    target = 10.0 ** (peak_db / 20.0)
    g = target / peak
    return [v * g for v in x]


def fade(x, fade_in, fade_out):
    """Linear fade-in over ``fade_in`` frames and fade-out over the last ``fade_out`` frames."""
    n = len(x)
    fi = min(fade_in, n)
    fo = min(fade_out, n)
    for i in range(fi):
        x[i] *= (i + 1) / (fi + 1)
    for i in range(fo):
        x[n - 1 - i] *= i / fo
    return x


def soft_clip(x, drive=1.0):
    """tanh saturation with unity small-signal gain compensation."""
    if drive <= 0.0:
        return list(x)
    inv = 1.0 / math.tanh(drive) if drive < 20 else 1.0
    return [math.tanh(v * drive) * inv for v in x]


def resample(x, sr_in, sr_out, taps=16):
    """Windowed-sinc resampling (Hann window, ``taps`` per side, anti-aliased)."""
    if sr_in == sr_out:
        return list(x)
    n_in = len(x)
    n_out = int(round(n_in * sr_out / sr_in))
    ratio = sr_in / sr_out
    fc = min(1.0, sr_out / sr_in) * 0.5  # cycles per input sample
    out = zeros(n_out)
    sin = math.sin
    cos = math.cos
    pi = math.pi
    for k in range(n_out):
        pos = k * ratio
        i0 = int(math.floor(pos))
        acc = 0.0
        wsum = 0.0
        for j in range(i0 - taps + 1, i0 + taps + 1):
            d = pos - j
            if abs(d) >= taps:
                continue
            arg = 2.0 * fc * d
            s = 1.0 if arg == 0.0 else sin(pi * arg) / (pi * arg)
            w = 0.5 * (1.0 + cos(pi * d / taps))
            h = s * w
            wsum += h
            if 0 <= j < n_in:
                acc += h * x[j]
        out[k] = acc / wsum if wsum else 0.0
    return out


def crossfade_loop(x, loop_start, loop_end, xfade):
    """Make ``[loop_start, loop_end)`` loop smoothly and truncate to ``loop_end``.

    The last ``xfade`` frames of the loop are blended into the ``xfade`` frames that
    precede ``loop_start`` so that wrapping from ``loop_end`` to ``loop_start`` is
    continuous. Requires ``loop_start >= xfade``.
    """
    xfade = min(xfade, loop_start, (loop_end - loop_start) // 2)
    y = list(x[:loop_end])
    for i in range(xfade):
        t = (i + 1) / (xfade + 1)
        pos = loop_end - xfade + i
        src = loop_start - xfade + i
        y[pos] = y[pos] * (1.0 - t) + x[src] * t
    return y


def pad_to_multiple(x, m, value=0.0):
    r = len(x) % m
    if r:
        x = list(x) + [value] * (m - r)
    return x


def loop_length(sr, freq, min_frames, block=16, max_cycles=400):
    """Return ``(frames, exact_freq)``: a loop of whole cycles aligned to ``block`` frames.

    Chooses the cycle count >= min_frames whose rounded length has the smallest pitch
    error, then returns the frequency that fits that length exactly so an oscillator
    rendered at ``exact_freq`` loops seamlessly.
    """
    best = None
    period = sr / freq
    for cycles in range(1, max_cycles + 1):
        raw = cycles * period
        if raw < min_frames:
            continue
        rounded = int(round(raw / block)) * block
        if rounded <= 0:
            continue
        exact = cycles * sr / rounded
        cents = abs(1200.0 * math.log2(exact / freq))
        if best is None or cents < best[0] - 1e-9:
            best = (cents, rounded, exact)
        if cents < 0.01:
            break
    if best is None:
        raise ValueError("no loop length found")
    return best[1], best[2]


def detune(freq, cents):
    return freq * 2.0 ** (cents / 1200.0)
