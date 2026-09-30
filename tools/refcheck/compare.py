"""Differential fidelity check: our cores vs reference emulators, audio output only.

Usage::

    python tools\\refcheck\\compare.py [--stimuli DIR] [--work DIR] [--chiptool EXE]
                                      [--report docs\\research\\refcheck-report.md] [--no-render]
                                      [--only SUBSTRING]

Pipeline (run tools\\refcheck\\make_stimuli.py first):

1. Reference renders (binaries under third_party\\refemu, see docs/research/reference-emulators.md;
   we only run them and never read their source):
   * Genesis: VGMPlay 0.40.9 in silent WAV-logging mode (LogSound = 1), 44100 Hz, chip
     sample mode "native", two configurations: ``nuked`` (YM2612 = Nuked OPN2, chip type
     "YM2612" without the Model 1 filter; SN76496 = MAME core) and ``mame`` (YM2612 = MAME /
     Genesis Plus GX core; SN76496 = Maxim core).
   * SNES: FFmpeg's libgme demuxer (Game Music Emu SPC player) at 32000 Hz.
2. Our renders: ``chiptool regs`` (the same register sequences fed straight into
   Ym2612Core / Sn76489Core / SnesDsp; Genesis through the engine output path at 44100 Hz
   with the ladder effect on and the Model 1 low-pass off, SNES native 32 kHz).
3. Analysis per stimulus (mono = (L + R) / 2 unless stated):
   * Pre-conditioning, identical for both sides: subtract the first sample, then a 20 Hz
     2nd-order Butterworth high-pass (removes the reference's un-blocked ladder / PSG DC,
     which our 5 Hz coupling capacitor already removes). Documented analysis artefact.
   * Alignment: coarse lag from the onsets (first sample above 1 % of the peak), refined
     by time-domain cross-correlation over +/-64 samples on 4096 samples after the onset;
     the sign of the best correlation gives the polarity (the S-DSP inverts its output).
   * Level matching: ONE global gain constant per chip (YM2612, SN76489, S-DSP), the RMS
     ratio ref/ours of the calibration stimulus (fm_sine_ref, psg_tone_ref, snes_sine_ref)
     over its steady window, applied unchanged to every stimulus of that chip.
   * Pitch: peak of the Hann-windowed DTFT near the expected f0 (golden-section refined),
     ours vs ref in cents.
   * Level: RMS over the steady window, per channel (L, R), ours - ref in dB.
   * Harmonics H1..H10: Hann-windowed DTFT magnitude at k * f0 (each side at its own f0),
     in dB, floored at -60 dB re the reference H1; ours - ref.
   * Envelope timing (kind env): 2 ms RMS envelope; attack crossing times of -40/-20/-6/-1 dB
     re peak, decay crossings of -3/-6/-12/-20 dB after the peak, release crossings of
     -6/-20/-40 dB re the level at key-off; each duration ours vs ref in %.
   * Envelope shape: RMS dB difference of the 2 ms envelopes where the reference is within
     60 dB of its peak.
   * Spectral distance: RMS dB difference of 1/6-octave band powers (Hann 4096, hop 2048,
     active frames), bands within 70 dB of the loudest reference band.
   * Null test: 10 log10(ref energy / residual energy) after alignment and gain.
4. Writes the Markdown report (tables + every deviation beyond the thresholds: pitch 1 cent,
   harmonics 1 dB, envelope timing 3 %, per-channel level 0.5 dB).

Standard-library Python only (no numpy).
"""

from __future__ import annotations

import argparse
import cmath
import configparser
import json
import math
import os
import shutil
import struct
import subprocess
import sys
import wave

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REFEMU = os.path.join(ROOT, "third_party", "refemu")

TH_PITCH_CENTS = 1.0
TH_HARM_DB = 1.0
TH_ENV_PCT = 3.0
TH_LEVEL_DB = 0.5
TH_ENV_MIN_MS = 0.5        # envelope timing differences below this are under the measurement resolution
HARM_FLOOR_DB = -60.0
CALIBRATION = {"ym2612": "fm_sine_ref", "sn76489": "psg_tone_ref", "sdsp": "snes_sine_ref"}
GENESIS_REFS = ("nuked", "mame")
DIAG_BEGIN = "<!-- BEGIN DIAGNOSIS (hand-written, kept by compare.py) -->"
DIAG_END = "<!-- END DIAGNOSIS -->"

# ----- rendering ------------------------------------------------------------------------------

VGMPLAY_CONFIGS = {
    # name: (YM2612 EmulatorType, NukedType, SN76496 EmulatorType, ChipSmplMode, ChipSmplRate)
    # The Nuked OPN2 core runs fast by about 90000 / ChipSmplRate cents in VGMPlay (measured,
    # see reference-emulators.md), so it runs at the highest accepted chip rate (384 kHz:
    # +0.23 cents, then corrected by the measured time-base factor).
    "nuked": (1, 3, 0, 2, 384000),
    "mame": (0, 3, 1, 0, 0),
}


def find_ffmpeg() -> str:
    base = os.path.join(REFEMU, "ffmpeg")
    for d in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        exe = os.path.join(base, d, "bin", "ffmpeg.exe")
        if os.path.exists(exe):
            return exe
    raise SystemExit("ffmpeg not found under third_party/refemu/ffmpeg (see docs/research/reference-emulators.md)")


def vgmplay_dir(config: str) -> str:
    """A private copy of VGMPlay 0.40.9 with an ini for 'config'."""
    src = os.path.join(REFEMU, "VGMPlay_040-9")
    dst = os.path.join(REFEMU, f"vgmplay_{config}")
    if not os.path.exists(os.path.join(src, "VGMPlay.exe")):
        raise SystemExit("VGMPlay not found under third_party/refemu/VGMPlay_040-9")
    os.makedirs(dst, exist_ok=True)
    for f in ("VGMPlay.exe", "zlib1.dll"):
        shutil.copy2(os.path.join(src, f), dst)
    ym_type, nuked_type, sn_type, smpl_mode, smpl_rate = VGMPLAY_CONFIGS[config]
    lines = open(os.path.join(src, "VGMPlay.ini"), encoding="latin-1").read().split("\n")
    section = ""
    out = []
    for line in lines:
        s = line.strip()
        if s.startswith("["):
            section = s
        key = s.split("=")[0].strip() if "=" in s and not s.startswith(";") else ""
        values = {
            ("[General]", "SampleRate"): "44100", ("[General]", "LogSound"): "1",
            ("[General]", "ChipSmplMode"): str(smpl_mode), ("[General]", "ChipSmplRate"): str(smpl_rate),
            ("[General]", "FadeTime"): "0",
            ("[General]", "ResamplingMode"): "0", ("[General]", "Volume"): "1.0",
            ("[YM2612]", "EmulatorType"): f"0x{ym_type:02X}", ("[YM2612]", "NukedType"): f"0x{nuked_type:02X}",
            ("[YM2612]", "PseudoStereo"): "False",
            ("[SN76496]", "EmulatorType"): f"0x{sn_type:02X}",
        }
        if (section, key) in values:
            line = f"{key} = {values[(section, key)]}"
        out.append(line)
    with open(os.path.join(dst, "VGMPlay.ini"), "w", encoding="latin-1", newline="\r\n") as f:
        f.write("\n".join(out))
    return dst


def render_all(manifest: list[dict], stimuli: str, work: str, chiptool: str) -> None:
    ffmpeg = find_ffmpeg()
    dirs = {c: vgmplay_dir(c) for c in GENESIS_REFS}
    for m in manifest:
        name = m["name"]
        src = os.path.join(stimuli, m["file"])
        if m["chip"] == "genesis":
            for c, d in dirs.items():
                tmp_vgm = os.path.join(d, name + ".vgm")
                shutil.copy2(src, tmp_vgm)
                subprocess.run([os.path.join(d, "VGMPlay.exe"), tmp_vgm], cwd=d, stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=60)
                os.replace(os.path.join(d, name + ".wav"), os.path.join(work, f"{name}.ref_{c}.wav"))
                os.remove(tmp_vgm)
            subprocess.run([chiptool, "regs", "genesis", src, os.path.join(work, f"{name}.ours.wav")], check=True)
        else:
            subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "libgme", "-sample_rate", "32000",
                            "-i", src, "-t", f"{m['end']:.3f}", "-c:a", "pcm_s16le",
                            os.path.join(work, f"{name}.ref_gme.wav")], check=True)
            subprocess.run([chiptool, "regs", "snes", src, os.path.join(stimuli, m["events"]),
                            os.path.join(work, f"{name}.ours.wav"), "--seconds", f"{m['end']:.3f}"], check=True)


# ----- signal helpers -------------------------------------------------------------------------

def read_wav(path: str) -> tuple[int, list[float], list[float]]:
    with wave.open(path) as w:
        n, ch, rate = w.getnframes(), w.getnchannels(), w.getframerate()
        assert w.getsampwidth() == 2, path
        d = struct.unpack("<%dh" % (n * ch), w.readframes(n))
    left = [x / 32768.0 for x in d[0::ch]]
    right = [x / 32768.0 for x in d[ch - 1::ch]]
    return rate, left, right


def write_wav(path: str, rate: int, left: list[float], right: list[float]) -> None:
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        q = lambda v: max(-32768, min(32767, int(round(v * 32768.0))))
        w.writeframes(b"".join(struct.pack("<hh", q(a), q(b)) for a, b in zip(left, right)))


def precondition(x: list[float], rate: int, on: int, coupling: bool = False, lowpass: bool = False) -> list[float]:
    """Idle level (mean of on-25 ms .. on-5 ms) subtracted, everything before on-5 ms zeroed
    (the stimuli are silent there; this drops the reference's power-on DC step); with
    'coupling' the 5 Hz one-pole coupling capacitor our Genesis output path models (same
    formula as chipdsp::OnePoleHighPass), so that DC steps (ladder offsets, PSG unipolar
    levels) take the same path on both sides; then a 20 Hz 2nd-order Butterworth high-pass;
    with 'lowpass' a 15 kHz 4th-order Butterworth low-pass (Genesis only: the players'
    resamplers treat content above 15 kHz differently, e.g. the YM2612's 26.6 kHz chatter)."""
    a, b = max(0, on - int(0.025 * rate)), max(1, on - int(0.005 * rate))
    idle = sum(x[a:b]) / max(1, b - a)
    y = [0.0] * b + [v - idle for v in x[b:]]
    if coupling:
        rc = 1.0 / (2.0 * math.pi * 5.0)
        alpha = rc / (rc + 1.0 / rate)
        prev_in = prev_out = 0.0
        for i, v in enumerate(y):
            prev_out = alpha * (prev_out + v - prev_in)
            prev_in = v
            y[i] = prev_out
    y = highpass20(y, rate)
    if lowpass:
        for q in (0.5411961, 1.3065630):
            y = biquad_lowpass(y, rate, 15000.0, q)
    return y


def biquad_lowpass(x: list[float], rate: int, fc: float, q: float) -> list[float]:
    k = math.tan(math.pi * fc / rate)
    norm = 1.0 / (1.0 + k / q + k * k)
    b0 = k * k * norm
    b1, b2 = 2.0 * b0, b0
    a1, a2 = 2.0 * (k * k - 1.0) * norm, (1.0 - k / q + k * k) * norm
    y = [0.0] * len(x)
    z1 = z2 = 0.0
    for i, v in enumerate(x):
        o = b0 * v + z1
        z1 = b1 * v - a1 * o + z2
        z2 = b2 * v - a2 * o
        y[i] = o
    return y


def resample(x: list[float], factor: float) -> list[float]:
    """y[i] = x(i * factor), Hann-windowed sinc interpolation (16 taps each side). Used only
    for the documented reference time-base correction."""
    taps = 16
    out = [0.0] * len(x)
    for i in range(len(x)):
        t = i * factor
        c = int(math.floor(t))
        frac = t - c
        acc = 0.0
        for k in range(-taps + 1, taps + 1):
            j = c + k
            if 0 <= j < len(x):
                d = k - frac
                w = 0.5 + 0.5 * math.cos(math.pi * d / taps)
                acc += x[j] * w * (math.sin(math.pi * d) / (math.pi * d) if abs(d) > 1e-12 else 1.0)
        out[i] = acc
    return out


def highpass20(x: list[float], rate: int) -> list[float]:
    """20 Hz 2nd-order Butterworth high-pass (bilinear)."""
    k = math.tan(math.pi * 20.0 / rate)
    q = 1.0 / math.sqrt(2.0)
    norm = 1.0 / (1.0 + k / q + k * k)
    b0, b1, b2 = norm, -2.0 * norm, norm
    a1, a2 = 2.0 * (k * k - 1.0) * norm, (1.0 - k / q + k * k) * norm
    y = [0.0] * len(x)
    z1 = z2 = 0.0
    for i, v in enumerate(x):
        o = b0 * v + z1
        z1 = b1 * v - a1 * o + z2
        z2 = b2 * v - a2 * o
        y[i] = o
    return y


def onset(x: list[float], start: int) -> int:
    """First sample from 'start' above 1 % of the peak (the search starts shortly before the
    stimulus on-time so that the reference's power-on DC step is not taken for the onset)."""
    peak = max((abs(v) for v in x[start:]), default=0.0)
    if peak <= 0.0:
        return start
    th = 0.01 * peak
    for i in range(start, len(x)):
        if abs(x[i]) > th:
            return i
    return start


def align(ref: list[float], ours: list[float], search_from: int) -> tuple[int, int, float]:
    """Lag (ours index = ref index + lag), polarity (+1/-1), normalised correlation."""
    o_ref, o_ours = onset(ref, search_from), onset(ours, search_from)
    coarse = o_ours - o_ref
    start = o_ref
    n = min(4096, len(ref) - start - 1)
    best = (coarse, 1, 0.0)
    er = sum(v * v for v in ref[start:start + n]) or 1e-30
    for lag in range(coarse - 64, coarse + 65):
        s0 = start + lag
        if s0 < 0 or s0 + n > len(ours):
            continue
        seg = ours[s0:s0 + n]
        c = sum(a * b for a, b in zip(ref[start:start + n], seg))
        eo = sum(v * v for v in seg) or 1e-30
        r = c / math.sqrt(er * eo)
        if abs(r) > abs(best[2]):
            best = (lag, 1 if r >= 0 else -1, r)
    return best


def shifted(x: list[float], lag: int, length: int, gain: float) -> list[float]:
    """ours re-indexed on the reference time base: y[i] = gain * x[i + lag]."""
    out = [0.0] * length
    for i in range(length):
        j = i + lag
        if 0 <= j < len(x):
            out[i] = gain * x[j]
    return out


def rms(x: list[float]) -> float:
    return math.sqrt(sum(v * v for v in x) / len(x)) if x else 0.0


def db(v: float) -> float:
    return 20.0 * math.log10(max(v, 1e-12))


def hann(n: int) -> list[float]:
    return [0.5 - 0.5 * math.cos(2.0 * math.pi * i / (n - 1)) for i in range(n)]


def dtft_mag(xw: list[float], f: float, rate: int) -> float:
    """|X(f)| of an already windowed segment (Goertzel), normalised by the window sum later."""
    w = 2.0 * math.pi * f / rate
    c = 2.0 * math.cos(w)
    s1 = s2 = 0.0
    for v in xw:
        s0 = v + c * s1 - s2
        s2 = s1
        s1 = s0
    re = s1 - s2 * math.cos(w)
    im = s2 * math.sin(w)
    return math.hypot(re, im)


def peak_freq(xw: list[float], f_guess: float, rate: int, span: float = 0.03) -> float:
    n = len(xw)
    df = rate / n / 4.0
    lo, hi = f_guess * (1 - span), f_guess * (1 + span)
    best_f, best_m = f_guess, -1.0
    f = lo
    while f <= hi:
        m = dtft_mag(xw, f, rate)
        if m > best_m:
            best_f, best_m = f, m
        f += df
    a, b = best_f - df, best_f + df
    g = (math.sqrt(5) - 1) / 2
    c1, c2 = b - g * (b - a), a + g * (b - a)
    m1, m2 = dtft_mag(xw, c1, rate), dtft_mag(xw, c2, rate)
    for _ in range(40):
        if m1 > m2:
            b, c2, m2 = c2, c1, m1
            c1 = b - g * (b - a)
            m1 = dtft_mag(xw, c1, rate)
        else:
            a, c1, m1 = c1, c2, m2
            c2 = a + g * (b - a)
            m2 = dtft_mag(xw, c2, rate)
    return (a + b) / 2


def windowed(x: list[float], a: int, b: int, cap: int = 16384) -> list[float]:
    seg = x[a:b]
    if len(seg) > cap:
        mid = (len(seg) - cap) // 2
        seg = seg[mid:mid + cap]
    w = hann(len(seg))
    return [v * wv for v, wv in zip(seg, w)]


ENV_WINDOW_MS = 4.0
ENV_HOP_MS = 0.25


def envelope_db(x: list[float], rate: int) -> list[float]:
    """Sliding RMS (4 ms window, 0.25 ms hop) in dB; point i covers [i*hop, i*hop + window)."""
    n = max(1, int(rate * ENV_WINDOW_MS / 1000.0))
    hop = max(1, int(round(rate * ENV_HOP_MS / 1000.0)))
    pre = [0.0]
    for v in x:
        pre.append(pre[-1] + v * v)
    return [db(math.sqrt(max(pre[i + n] - pre[i], 0.0) / n)) for i in range(0, len(x) - n + 1, hop)]


def band_powers(x: list[float], rate: int, a: int, b: int) -> list[float]:
    """1/6-octave band powers (dB) of the Hann-4096 power spectrum averaged over [a, b)."""
    n = 4096
    w = hann(n)
    frames = []
    for s in range(a, max(a + 1, b - n), n // 2):
        seg = x[s:s + n]
        if len(seg) < n:
            break
        frames.append(seg)
    if not frames:
        return []
    energies = [sum(v * v for v in f) for f in frames]
    top = max(energies)
    frames = [f for f, e in zip(frames, energies) if e >= top * 1e-3]
    power = [0.0] * (n // 2 + 1)
    for f in frames:
        spec = fft([complex(v * wv) for v, wv in zip(f, w)])
        for k in range(n // 2 + 1):
            power[k] += abs(spec[k]) ** 2
    bands = []
    f_lo = 50.0
    while f_lo * 2 ** (1 / 6) < 0.45 * rate:
        f_hi = f_lo * 2 ** (1 / 6)
        k0, k1 = int(math.ceil(f_lo * n / rate)), int(math.floor(f_hi * n / rate))
        p = sum(power[k] for k in range(k0, k1 + 1)) if k1 >= k0 else power[int(round(f_lo * n / rate))]
        bands.append(10.0 * math.log10(max(p / len(frames), 1e-30)))
        f_lo = f_hi
    return bands


def fft(a: list[complex]) -> list[complex]:
    n = len(a)
    a = list(a)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    length = 2
    while length <= n:
        wl = cmath.exp(-2j * math.pi / length)
        half = length // 2
        tw = [wl ** k for k in range(half)]
        for i in range(0, n, length):
            for k in range(half):
                u = a[i + k]
                v = a[i + k + half] * tw[k]
                a[i + k] = u + v
                a[i + k + half] = u - v
        length <<= 1
    return a


def crossing(env: list[float], start: int, stop: int, level: float, rising: bool) -> float | None:
    """First index (fractional, linear interpolation in dB) where env crosses 'level'."""
    for i in range(max(0, start), min(stop, len(env))):
        if (env[i] >= level) if rising else (env[i] <= level):
            if i == 0 or i == start or env[i] == env[i - 1]:
                return float(i)
            return i - 1 + (level - env[i - 1]) / (env[i] - env[i - 1])
    return None


# ----- per-stimulus analysis ------------------------------------------------------------------

def analyse(m: dict, rate: int, ref: tuple[list[float], list[float]], ours: tuple[list[float], list[float]],
            gain: float | None, coupling_ref: bool = False, coupling_test: bool = False) -> dict:
    """coupling_ref / coupling_test: that side is a Genesis player render, which gets the
    coupling capacitor our output path has (15 kHz analysis low-pass on both Genesis sides)."""
    on = int(m["on"] * rate)
    lp = m["chip"] == "genesis"
    refL, refR = (precondition(c, rate, on, coupling=coupling_ref, lowpass=lp) for c in ref)
    ourL, ourR = (precondition(c, rate, on, coupling=coupling_test, lowpass=lp) for c in ours)
    off = int(m["off"] * rate)
    steady = m.get("steady")
    if steady:
        sa, sb = int(steady[0] * rate), int(steady[1] * rate)
    else:
        sa = on + int((0.1 if m["kind"] == "tone" else 0.15) * rate)
        sb = off - int(0.02 * rate)
        if sb - sa < int(0.05 * rate):
            sa, sb = on + int(0.01 * rate), off
    # Analysis channel: the side with more reference energy (a mono sum would cancel for
    # opposite-sign volumes and halve single-sided pans). Levels are reported per side.
    use_left = rms(refL[on:]) >= rms(refR[on:])
    ref_m, our_m = (refL, ourL) if use_left else (refR, ourR)
    lag, pol, corr = align(ref_m, our_m, max(0, on - int(0.02 * rate)))
    n = len(ref_m)

    oL = shifted(ourL, lag, n, pol)
    oR = shifted(ourR, lag, n, pol)
    om = oL if use_left else oR
    if gain is None:
        gain = rms(ref_m[sa:sb]) / max(rms(om[sa:sb]), 1e-12)
    oL = [v * gain for v in oL]
    oR = [v * gain for v in oR]
    om = [v * gain for v in om]

    res: dict = dict(name=m["name"], unit=m["unit"], kind=m["kind"], lag=lag, polarity=pol, corr=corr, gain=gain,
                     channel="L" if use_left else "R")
    residual = [a - b for a, b in zip(ref_m[on:], om[on:])]
    e_ref = sum(v * v for v in ref_m[on:])
    e_res = sum(v * v for v in residual)
    res["null_db"] = 10.0 * math.log10(max(e_ref, 1e-30) / max(e_res, 1e-30))

    lvl = {}
    for side, r_, o_ in (("L", refL, oL), ("R", refR, oR)):
        rr, oo = rms(r_[sa:sb]), rms(o_[sa:sb])
        lvl[side] = (db(rr), db(oo))
    res["level"] = {k: dict(ref_db=v[0], ours_db=v[1], diff_db=v[1] - v[0]) for k, v in lvl.items()}

    if m["kind"] in ("tone", "lfo_pm") and m.get("f0", 0) > 0:
        xr = windowed(ref_m, sa, sb)
        xo = windowed(om, sa, sb)
        if rms(xr) > 1e-6 and rms(xo) > 1e-6:
            f_ref = peak_freq(xr, m["f0"], rate)
            f_ours = peak_freq(xo, m["f0"], rate)
            res["f0_ref"] = f_ref
            res["f0_ours"] = f_ours
            res["pitch_cents"] = 1200.0 * math.log2(f_ours / f_ref)
            wsum = len(xr) / 2.0
            h1 = None
            harms = []
            for k in range(1, 11):
                if k * f_ref >= 0.45 * rate:
                    break
                mr = db(dtft_mag(xr, k * f_ref, rate) / wsum)
                mo = db(dtft_mag(xo, k * f_ours, rate) / wsum)
                if h1 is None:
                    h1 = mr
                floor = h1 + HARM_FLOOR_DB
                harms.append(dict(k=k, hz=k * f_ref, ref_db=mr - h1, ours_db=mo - h1,
                                  diff_db=max(mo, floor) - max(mr, floor)))
            res["harmonics"] = harms
        if m["kind"] == "lfo_pm":
            res["vibrato"] = vibrato(ref_m, om, rate, sa, sb, m["f0"])

    env_r = envelope_db(ref_m, rate)
    env_o = envelope_db(om, rate)
    peak = max(env_r)
    to_i = lambda smp: int(smp * 1000.0 / rate / ENV_HOP_MS)
    first = max(0, to_i(on - int(0.005 * rate)))
    diffs = [o - r for r, o in list(zip(env_r, env_o))[first:] if r > peak - 60.0]
    res["env_shape_db"] = math.sqrt(sum(d * d for d in diffs) / len(diffs)) if diffs else 0.0
    if m["kind"] in ("env", "ssg"):
        res["env_timing"] = env_timing(env_r, env_o, to_i(on), to_i(off))
    if m["kind"] in ("ssg", "lfo_am"):
        res["modulation"] = modulation(env_r, env_o, to_i(on + int(0.05 * rate)), to_i(off - int(0.02 * rate)))

    br = band_powers(ref_m, rate, on, min(n, off + int(0.3 * rate)))
    bo = band_powers(om, rate, on, min(n, off + int(0.3 * rate)))
    if br and bo:
        top = max(br)
        d = [o - r for r, o in zip(br, bo) if r > top - 70.0]
        res["spectral_db"] = math.sqrt(sum(v * v for v in d) / len(d)) if d else 0.0
    return res


def env_timing(env_r: list[float], env_o: list[float], i_on: int, i_off: int) -> list[dict]:
    """Crossing times in ms (envelope hop ENV_HOP_MS, interpolated)."""
    ms = ENV_HOP_MS
    out = []
    peak_r = max(env_r[i_on:i_off]) if i_off > i_on else max(env_r)
    i_peak = env_r.index(peak_r, i_on)
    anchor_r = crossing(env_r, i_on - 40, i_off, peak_r - 40.0, True)
    anchor_o = crossing(env_o, i_on - 40, i_off, peak_r - 40.0, True)
    if anchor_r is not None and anchor_o is not None:
        for lv in (-20.0, -6.0, -1.0):
            tr = crossing(env_r, int(anchor_r), i_off, peak_r + lv, True)
            to = crossing(env_o, int(anchor_o), i_off, peak_r + lv, True)
            if tr is not None and to is not None and (tr - anchor_r) * ms >= 2.0:
                out.append(dict(stage=f"attack to {lv:g} dB", ref_ms=(tr - anchor_r) * ms, ours_ms=(to - anchor_o) * ms))
    # Decay: from the reference peak position, both sides.
    for lv in (-3.0, -6.0, -12.0, -20.0):
        tr = crossing(env_r, i_peak, i_off, peak_r + lv, False)
        to = crossing(env_o, i_peak, i_off, peak_r + lv, False)
        if tr is not None and to is not None and (tr - i_peak) * ms >= 2.0:
            out.append(dict(stage=f"decay to {lv:g} dB", ref_ms=(tr - i_peak) * ms, ours_ms=(to - i_peak) * ms))
    # Release: re the level at key-off (window ending at key-off); -40 dB only when that
    # stays within 65 dB of the peak (below, the analysis filters' DC tails dominate).
    n_win = int(ENV_WINDOW_MS / ms)
    if i_off < len(env_r) - 2:
        lvl_off = env_r[max(0, i_off - n_win)]
        for lv in (-6.0, -20.0, -40.0):
            if lvl_off + lv < peak_r - 65.0:
                continue
            tr = crossing(env_r, i_off - n_win, len(env_r), lvl_off + lv, False)
            to = crossing(env_o, i_off - n_win, len(env_o), lvl_off + lv, False)
            if tr is not None and to is not None and (tr - i_off) * ms >= 2.0:
                out.append(dict(stage=f"release to {lv:g} dB", ref_ms=(tr - i_off) * ms, ours_ms=(to - i_off) * ms))
    for e in out:
        e["diff_pct"] = 100.0 * (e["ours_ms"] - e["ref_ms"]) / e["ref_ms"]
    return out


def modulation(env_r: list[float], env_o: list[float], a: int, b: int) -> dict:
    """Envelope modulation over [a, b): depth (5th..95th percentile, dB) and period (first
    autocorrelation maximum after the first zero crossing, ms; None if not periodic)."""
    def stats(env: list[float]) -> tuple[float, float | None]:
        seg = env[a:b]
        srt = sorted(seg)
        depth = srt[int(0.95 * (len(srt) - 1))] - srt[int(0.05 * (len(srt) - 1))]
        mu = sum(seg) / len(seg)
        x = [v - mu for v in seg]
        e0 = sum(v * v for v in x) or 1e-30
        max_lag = min(len(x) // 2, int(400.0 / ENV_HOP_MS))
        ac = []
        for lag in range(0, max_lag, 2):
            ac.append(sum(x[i] * x[i + lag] for i in range(0, len(x) - lag, 2)) / e0 * 2)
        period = None
        crossed = False
        for i in range(1, len(ac) - 1):
            if ac[i] < 0:
                crossed = True
            if crossed and ac[i] > 0.3 and ac[i] >= ac[i - 1] and ac[i] >= ac[i + 1]:
                # parabolic refinement
                d = ac[i - 1] - 2 * ac[i] + ac[i + 1]
                shift = 0.5 * (ac[i - 1] - ac[i + 1]) / d if abs(d) > 1e-12 else 0.0
                period = (i + shift) * 2 * ENV_HOP_MS
                break
        return depth, period
    dr, pr = stats(env_r)
    do, po = stats(env_o)
    return dict(ref_depth_db=dr, ours_depth_db=do, ref_period_ms=pr, ours_period_ms=po)


def vibrato(ref: list[float], ours: list[float], rate: int, a: int, b: int, f0: float) -> dict:
    """Frame-wise f0 (2048-sample Hann frames, hop 1024): peak-to-peak depth in cents."""
    def track(x: list[float]) -> list[float]:
        out = []
        for s in range(a, b - 2048, 1024):
            xw = windowed(x, s, s + 2048)
            out.append(1200.0 * math.log2(peak_freq(xw, f0, rate, span=0.06) / f0))
        return out
    tr, to = track(ref), track(ours)
    return dict(ref_pp_cents=max(tr) - min(tr), ours_pp_cents=max(to) - min(to),
                ref_mean_cents=sum(tr) / len(tr), ours_mean_cents=sum(to) / len(to))


# ----- deviations and report ------------------------------------------------------------------

def deviations(r: dict) -> list[str]:
    out = []
    if "pitch_cents" in r and abs(r["pitch_cents"]) > TH_PITCH_CENTS:
        out.append(f"pitch {r['pitch_cents']:+.2f} cents (ref {r['f0_ref']:.3f} Hz, ours {r['f0_ours']:.3f} Hz)")
    for side, v in r["level"].items():
        if abs(v["diff_db"]) > TH_LEVEL_DB and v["ref_db"] > -90:
            out.append(f"level {side} {v['diff_db']:+.2f} dB (ref {v['ref_db']:.1f} dBFS)")
    for h in r.get("harmonics", []):
        if abs(h["diff_db"]) > TH_HARM_DB and max(h["ref_db"], h["ours_db"]) > HARM_FLOOR_DB:
            out.append(f"H{h['k']} {h['diff_db']:+.2f} dB (ref {h['ref_db']:.1f}, ours {h['ours_db']:.1f} dB re H1)")
    for e in r.get("env_timing", []):
        if abs(e["diff_pct"]) > TH_ENV_PCT and abs(e["ours_ms"] - e["ref_ms"]) > TH_ENV_MIN_MS:
            out.append(f"{e['stage']}: {e['diff_pct']:+.1f} % (ref {e['ref_ms']:.1f} ms, ours {e['ours_ms']:.1f} ms)")
    mod = r.get("modulation")
    if mod:
        if abs(mod["ours_depth_db"] - mod["ref_depth_db"]) > TH_LEVEL_DB:
            out.append(f"envelope modulation depth {mod['ours_depth_db']:.2f} vs ref {mod['ref_depth_db']:.2f} dB")
        pr, po = mod["ref_period_ms"], mod["ours_period_ms"]
        if (pr is None) != (po is None):
            out.append(f"envelope period: ref {pr if pr is None else round(pr, 1)} ms, ours {po if po is None else round(po, 1)} ms")
        elif pr and po and abs(po - pr) / pr * 100.0 > TH_ENV_PCT:
            out.append(f"envelope period {100.0 * (po - pr) / pr:+.1f} % (ref {pr:.1f} ms, ours {po:.1f} ms)")
    v = r.get("vibrato")
    if v and abs(v["ours_pp_cents"] - v["ref_pp_cents"]) > max(TH_PITCH_CENTS, 0.05 * v["ref_pp_cents"]):
        out.append(f"vibrato depth {v['ours_pp_cents']:.1f} vs ref {v['ref_pp_cents']:.1f} cents p-p")
    return out


def summary_row(r: dict) -> str:
    worst_h = max((abs(h["diff_db"]) for h in r.get("harmonics", []) if max(h["ref_db"], h["ours_db"]) > HARM_FLOOR_DB),
                  default=None)
    worst_e = max((abs(e["diff_pct"]) for e in r.get("env_timing", []) if abs(e["ours_ms"] - e["ref_ms"]) > TH_ENV_MIN_MS),
                  default=None)
    cells = [
        r["name"],
        f"{r['lag']:+d}{'' if r['polarity'] > 0 else ' inv'}",
        f"{r['null_db']:.1f}",
        f"{r['pitch_cents']:+.2f}" if "pitch_cents" in r else "",
        f"{r['level']['L']['diff_db']:+.2f} / {r['level']['R']['diff_db']:+.2f}",
        f"{worst_h:.2f}" if worst_h is not None else "",
        f"{worst_e:.1f}" if worst_e is not None else "",
        f"{r['env_shape_db']:.2f}",
        f"{r.get('spectral_db', 0.0):.2f}",
        str(len(deviations(r))),
    ]
    return "| " + " | ".join(cells) + " |"


HEADER = ("| stimulus | lag (smp) | null (dB) | pitch (cents) | level L / R (dB) | max harm (dB) | "
          "max env timing (%) | env shape (dB) | spectral (dB) | devs |\n|---|---|---|---|---|---|---|---|---|---|")


def write_report(path: str, results: dict, manifest: list[dict], gains: dict, timebase: dict) -> None:
    desc = {m["name"]: m.get("desc", "") for m in manifest}
    # The hand-written diagnosis between the markers survives regeneration.
    diagnosis = f"{DIAG_BEGIN}\n## Diagnosis\n\n(not written yet)\n{DIAG_END}"
    if os.path.exists(path):
        old = open(path, encoding="utf-8").read()
        if DIAG_BEGIN in old and DIAG_END in old:
            diagnosis = old[old.index(DIAG_BEGIN):old.index(DIAG_END) + len(DIAG_END)]
    lines = []
    lines.append("# Reference-emulator differential check\n")
    lines.append("Tables generated by `tools/refcheck/compare.py` from the stimuli of `tools/refcheck/make_stimuli.py`; "
                 "the diagnosis section between the markers is hand-written and kept on regeneration. "
                 "Tools, versions and known reference behaviour: `docs/research/reference-emulators.md`.\n")
    lines.append(diagnosis + "\n")
    lines.append("## Generated results\n")
    lines.append("Thresholds: pitch 1 cent, harmonics 1 dB (bins within 60 dB of H1), envelope timing 3 %, "
                 "per-channel level 0.5 dB. Columns: lag = our delay in samples after alignment (inv = inverted "
                 "polarity); null = reference energy over residual energy after alignment and gain; env shape = RMS dB "
                 "difference of the 2 ms envelopes; spectral = RMS dB difference of 1/6-octave band powers.\n")
    lines.append("### Global gain constants (one per chip)\n")
    lines.append("| comparison | chip | calibration stimulus | gain ref/ours (dB) |\n|---|---|---|---|")
    for (cmp_name, unit), g in sorted(gains.items()):
        lines.append(f"| {cmp_name} | {unit} | {CALIBRATION[unit]} | {db(g):+.3f} |")
    lines.append("")
    for k, v in timebase.items():
        lines.append(f"Reference time-base correction: `{k}` YM2612 renders resampled by {v:.6f} "
                     f"({1200 * math.log2(v):+.3f} cents), measured on {CALIBRATION['ym2612']} against the "
                     "public frequency formula (VGMPlay integration artefact, see the diagnosis).\n")
    for cmp_name, res in results.items():
        lines.append(f"### {cmp_name}\n")
        for unit in ("ym2612", "sn76489", "sdsp"):
            rows = [r for r in res if r["unit"] == unit]
            if not rows:
                continue
            lines.append(f"#### {unit}\n")
            lines.append(HEADER)
            lines.extend(summary_row(r) for r in rows)
            lines.append("")
        lines.append(f"#### Deviations beyond thresholds ({cmp_name})\n")
        any_dev = False
        for r in res:
            devs = deviations(r)
            if devs:
                any_dev = True
                lines.append(f"* **{r['name']}** ({desc.get(r['name'], '')}): " + "; ".join(devs))
        if not any_dev:
            lines.append("None.")
        lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


# ----- main -----------------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--stimuli", default=os.path.join(ROOT, "build-reports", "refcheck", "stimuli"))
    ap.add_argument("--work", default=os.path.join(ROOT, "build-reports", "refcheck", "renders"))
    ap.add_argument("--chiptool", default=os.path.join(ROOT, "build", "windows-x64-release", "dsp", "tools", "chiptool.exe"))
    ap.add_argument("--report", default=os.path.join(ROOT, "docs", "research", "refcheck-report.md"))
    ap.add_argument("--json", default=os.path.join(ROOT, "build-reports", "refcheck", "results.json"))
    ap.add_argument("--no-render", action="store_true")
    ap.add_argument("--only", default="")
    args = ap.parse_args()

    manifest = json.load(open(os.path.join(args.stimuli, "manifest.json"), encoding="utf-8"))
    if args.only:
        keep = {v for v in CALIBRATION.values()}
        manifest = [m for m in manifest if args.only in m["name"] or m["name"] in keep]
    os.makedirs(args.work, exist_ok=True)
    if not args.no_render:
        render_all(manifest, os.path.abspath(args.stimuli), os.path.abspath(args.work), os.path.abspath(args.chiptool))

    comparisons = {"ours vs Nuked OPN2 + MAME SN76496 (VGMPlay)": ("ours", "ref_nuked"),
                   "ours vs MAME/GPGX YM2612 + Maxim SN76489 (VGMPlay)": ("ours", "ref_mame"),
                   "reference vs reference: MAME/GPGX + Maxim against Nuked + MAME": ("ref_mame", "ref_nuked"),
                   "ours vs Game Music Emu SPC (FFmpeg libgme)": ("ours", "ref_gme")}
    results: dict[str, list[dict]] = {}
    gains: dict = {}
    # Reference time-base correction (documented measurement artefact): VGMPlay's Nuked OPN2
    # output runs fast by a factor that depends on VGMPlay's ChipSmplRate setting, so the
    # factor is measured on fm_sine_ref against the expected frequency from the public formula
    # (fnum * 2^block / 2^21 * FM rate; the phase increment is an exact integer there) and the
    # reference is resampled by it for every YM2612 stimulus. Our output never enters this.
    timebase: dict[str, float] = {}
    cal = next((m for m in manifest if m["name"] == CALIBRATION["ym2612"]), None)
    cal_path = os.path.join(args.work, f"{CALIBRATION['ym2612']}.ref_nuked.wav")
    if cal is not None and os.path.exists(cal_path):
        rate, cl, cr = read_wav(cal_path)
        on = int(cal["on"] * rate)
        mono = [(a + b) * 0.5 for a, b in zip(precondition(cl, rate, on), precondition(cr, rate, on))]
        f_meas = peak_freq(windowed(mono, on + int(0.1 * rate), int(cal["off"] * rate) - int(0.02 * rate)), cal["f0"], rate)
        timebase["ref_nuked"] = f_meas / cal["f0"]
        print(f"Nuked time-base factor {timebase['ref_nuked']:.6f} ({1200 * math.log2(timebase['ref_nuked']):+.3f} cents)")
    resampled: dict[tuple[str, str], tuple[list[float], list[float]]] = {}
    for cmp_name, (test, refk) in comparisons.items():
        chip = "snes" if refk == "ref_gme" else "genesis"
        rows = [m for m in manifest if m["chip"] == chip]
        if not rows:
            continue
        # Calibration stimuli first.
        unit_gain: dict[str, float] = {}
        cache: dict[str, dict] = {}
        ordered = sorted(rows, key=lambda m: 0 if m["name"] in CALIBRATION.values() else 1)
        for m in ordered:
            rp = os.path.join(args.work, f"{m['name']}.{refk}.wav")
            tp = os.path.join(args.work, f"{m['name']}.{test}.wav")
            rate, rl, rr = read_wav(rp)
            # The DAC stimulus is exempt: its pitch is set by the VGM write timing, which the
            # player gets right (measured: uncorrected DAC pitch matches ours and the MAME core).
            tb = timebase.get(refk, 1.0) if m["unit"] == "ym2612" and not m["name"].startswith("fm_dac") else 1.0
            if tb != 1.0:
                key = (m["name"], refk)
                if key not in resampled:
                    # y[i] = x(i / tb): a reference running fast by tb is slowed back down.
                    # Cached on disk next to the render (16-bit), keyed by the factor.
                    cp = os.path.join(args.work, f"{m['name']}.{refk}.tb{tb:.6f}.wav")
                    if os.path.exists(cp) and os.path.getmtime(cp) >= os.path.getmtime(rp):
                        _, cl_, cr_ = read_wav(cp)
                        resampled[key] = (cl_, cr_)
                    else:
                        resampled[key] = (resample(rl, 1.0 / tb), resample(rr, 1.0 / tb))
                        write_wav(cp, rate, *resampled[key])
                rl, rr = resampled[key]
            rate2, tl, tr = read_wav(tp)
            if rate != rate2:
                raise SystemExit(f"{m['name']}: sample rates differ ({rate} vs {rate2})")
            n = min(len(rl), len(tl))
            g = unit_gain.get(m["unit"])
            r = analyse(m, rate, (rl[:n], rr[:n]), (tl[:n], tr[:n]), g,
                        coupling_ref=chip == "genesis", coupling_test=chip == "genesis" and test != "ours")
            r["timebase"] = tb
            if g is None:
                unit_gain[m["unit"]] = r["gain"]
                gains[(cmp_name, m["unit"])] = r["gain"]
            cache[m["name"]] = r
            print(f"{cmp_name[:28]:28s} {m['name']:24s} null {r['null_db']:6.1f} dB  devs {len(deviations(r))}", flush=True)
        results[cmp_name] = [cache[m["name"]] for m in rows]
    os.makedirs(os.path.dirname(args.json), exist_ok=True)
    with open(args.json, "w", encoding="utf-8") as f:
        json.dump({k: v for k, v in results.items()}, f, indent=1)
    write_report(args.report, results, manifest, gains, timebase)
    print(f"report -> {args.report}")


if __name__ == "__main__":
    sys.exit(main())
