"""Differential fidelity check: our cores vs reference emulators, audio output only.

Usage::

    python tools\\refcheck\\compare.py [--stimuli DIR] [--work DIR] [--chiptool EXE]
                                      [--report docs\\research\\refcheck-report.md] [--no-render]
                                      [--only SUBSTRING]

Pipeline (run tools\\refcheck\\make_stimuli.py first):

1. Reference renders (binaries under third_party\\refemu, see docs/research/reference-emulators.md;
   we only run them and never read their source):
   * Genesis: VGMPlay 0.40.9 in silent WAV-logging mode (LogSound = 1) at 44100 Hz, two
     configurations: ``nuked`` (YM2612 = Nuked OPN2, chip type "YM2612" without the Model 1
     filter, chip sample rate 384 kHz; SN76496 = MAME core) and ``mame`` (YM2612 = MAME /
     Genesis Plus GX core, native chip rate; SN76496 = Maxim core).
   * SNES: FFmpeg's libgme demuxer (Game Music Emu SPC player) at 32000 Hz.
   * NES: VGMPlay 0.40.9 with its two NES APU cores, ``nsfplay`` (NSFPlay-derived, hardware-like
     option set) and ``nesmame`` (MAME core), native chip rate, 44100 Hz; and, for the NSF subset
     (static tones and mixer segments), Game Music Emu's NSF player through FFmpeg (``gmensf``).
2. Our renders: ``chiptool regs`` (the same register sequences fed straight into
   Ym2612Core / Sn76489Core / SnesDsp / NesApu; Genesis through the engine output path at 44100 Hz
   with the ladder effect on and the Model 1 low-pass off, SNES native 32 kHz, NES through the
   Nes2A03Engine output path at 44100 Hz with console_filter = 0 (5 Hz DC blocker only), once
   with the engine's ImpulseSum step kernel (``ours``) and once with the integrated-step kernel
   (``ours_int``) so that the known kernel difference (refcheck F2) is measured separately).
3. Analysis per stimulus, on the channel with more reference energy (levels per channel):
   * Pre-conditioning (``precondition``): idle level removed and the silent lead-in zeroed;
     Genesis references get the 5 Hz coupling capacitor our output path has; then, both
     sides, a 60 Hz 4th-order Butterworth high-pass and (Genesis) a 15 kHz 4th-order
     low-pass. Documented analysis choices, identical on both sides.
   * Reference time-base correction (Nuked only, measured on fm_sine_ref against the public
     frequency formula; the DAC stimulus is exempt), see ``main``.
   * Alignment: YM2612 stimuli share the lag of fm_sine_ref; others: coarse lag from the
     onsets (first sample above 1 % of the peak), refined by time-domain cross-correlation
     over +/-64 samples on 4096 samples after the onset. The correlation sign gives the
     polarity (the S-DSP inverts its output). The analysis runs on our time base (the
     manifest times are exact there).
   * Level matching: ONE global gain constant per chip (YM2612, SN76489, S-DSP), the RMS
     ratio ref/ours of the calibration stimulus (fm_sine_ref, psg_tone_ref, snes_sine_ref)
     over its steady window, applied unchanged to every stimulus of that chip.
   * Pitch: peak of the Hann-windowed DTFT near the expected f0 (golden-section refined),
     ours vs ref in cents.
   * Level: RMS over the steady window, per channel (L, R), ours - ref in dB.
   * Harmonics H1..H10: Hann-windowed DTFT magnitude at k * f0_ref on both sides (at each
     side's own f0 when the pitch differs by more than 5 cents), in dB re the reference H1,
     floored at -60 dB; ours - ref.
   * Envelope timing (kinds env, ssg): 4 ms sliding RMS every 0.25 ms, crossings
     interpolated; attack times to -20/-6/-1 dB re peak (from the -40 dB crossing), decay
     times to -3/-6/-12/-20 dB after the peak, release times to -6/-20/-40 dB re the level
     at key-off; each duration ours vs ref in % (differences under 0.5 ms are below the
     resolution and not flagged).
   * Envelope modulation (kinds ssg, lfo_am): depth (5th..95th percentile of the dB
     envelope) and period (envelope autocorrelation).
   * Envelope shape: RMS dB difference of the envelopes where the reference is within 60 dB
     of its peak.
   * Spectral distance: RMS dB difference of 1/6-octave band powers (Hann 4096, hop 2048,
     active frames), bands within 70 dB of the loudest reference band.
   * Null test: 10 log10(ref energy / residual energy) after alignment and gain.
   * NES only (mono: one channel analysed): the 15 kHz analysis low-pass as for the Genesis;
     one gain constant from nes_pulse_ref and one time base from nes_dmc_direct ($4011 square,
     edges set by the write times alone) for the timing stimuli; multi-segment stimuli (mixer
     non-linearity): per-segment levels, levels relative to the first segment against the exact
     mixer formula, intermodulation probes; gate events (length / linear counter, envelope end,
     sweep mute, one-shot DMC) and the envelope staircase timed on the raw renders against the
     frame-sequencer schedule computed by make_stimuli.py; sweep pitch tracks against the
     documented trajectory. Threshold for these events: 1 ms plus half a period of the tone.
4. Writes the Markdown report (tables + every deviation beyond the thresholds: pitch 1 cent,
   harmonics 1 dB, envelope timing 3 %, per-channel level 0.5 dB), keeping the hand-written
   diagnosis block, and ``build-reports/refcheck/results.json``.

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
from concurrent.futures import ProcessPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
REFEMU = os.path.join(ROOT, "third_party", "refemu")

TH_PITCH_CENTS = 1.0
TH_HARM_DB = 1.0
TH_ENV_PCT = 3.0
TH_LEVEL_DB = 0.5
TH_ENV_MIN_MS = 0.5        # envelope timing differences below this are under the measurement resolution
HARM_FLOOR_DB = -60.0
CALIBRATION = {"ym2612": "fm_sine_ref", "sn76489": "psg_tone_ref", "sdsp": "snes_sine_ref", "2a03": "nes_pulse_ref"}
TH_NES_EVENT_MS = 1.0      # NES frame-sequencer events (length, linear counter, envelope steps): absolute
# Chips whose renders share one lag per comparison (measured on the calibration stimulus):
# both sides apply the same writes at the same times, while slow attacks make a per-stimulus
# correlation ambiguous by whole periods. The PSG's initial tone phase differs between cores
# and the S-DSP noise LFSR phase at key-on differs (the SPC format does not store it), so those
# align per stimulus.
GLOBAL_LAG_UNITS = {"ym2612", "sdsp"}   # sdsp: except noise stimuli (LFSR phase differs)
DIAG_BEGIN = "<!-- BEGIN DIAGNOSIS (hand-written, kept by compare.py) -->"
DIAG_END = "<!-- END DIAGNOSIS -->"

# ----- rendering ------------------------------------------------------------------------------

VGMPLAY_BASE = {
    ("[General]", "SampleRate"): "44100", ("[General]", "LogSound"): "1", ("[General]", "FadeTime"): "0",
    ("[General]", "ResamplingMode"): "0", ("[General]", "Volume"): "1.0",
}
VGMPLAY_CONFIGS = {
    # ini overrides per configuration (section, key) -> value.
    # The Nuked OPN2 core runs fast by about 90000 / ChipSmplRate cents in VGMPlay (measured,
    # see reference-emulators.md), so it runs at the highest accepted chip rate (384 kHz:
    # +0.23 cents, then corrected by the measured time-base factor).
    "nuked": {("[General]", "ChipSmplMode"): "2", ("[General]", "ChipSmplRate"): "384000",
              ("[YM2612]", "EmulatorType"): "0x01", ("[YM2612]", "NukedType"): "0x03",
              ("[YM2612]", "PseudoStereo"): "False", ("[SN76496]", "EmulatorType"): "0x00"},
    "mame": {("[General]", "ChipSmplMode"): "0", ("[General]", "ChipSmplRate"): "0",
             ("[YM2612]", "EmulatorType"): "0x00", ("[YM2612]", "NukedType"): "0x03",
             ("[YM2612]", "PseudoStereo"): "False", ("[SN76496]", "EmulatorType"): "0x01"},
    # NES APU: NSFPlay-derived core with the hardware-like option set (non-linear mixer, $4003
    # phase reset, $4011 and periodic noise enabled; no power-on unmute, no duty swap, no DPCM
    # anti-click, no noise randomisation, no triangle mute/null hacks), and the MAME core.
    # Native chip rate for both (ChipSmplMode 0); see reference-emulators.md.
    "nsfplay": {("[General]", "ChipSmplMode"): "0", ("[General]", "ChipSmplRate"): "0",
                ("[NES APU]", "EmulatorType"): "0x00", ("[NES APU]", "SharedOpts"): "0x02",
                ("[NES APU]", "APUOpts"): "0x01", ("[NES APU]", "DMCOpts"): "0x03"},
    "nesmame": {("[General]", "ChipSmplMode"): "0", ("[General]", "ChipSmplRate"): "0",
                ("[NES APU]", "EmulatorType"): "0x01"},
}
GENESIS_REFS = ("nuked", "mame")
NES_REFS = ("nsfplay", "nesmame")


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
    values = dict(VGMPLAY_BASE)
    values.update(VGMPLAY_CONFIGS[config])
    lines = open(os.path.join(src, "VGMPlay.ini"), encoding="latin-1").read().split("\n")
    section = ""
    out = []
    seen = set()
    for line in lines:
        s = line.strip()
        if s.startswith("["):
            section = s
        key = s.split("=")[0].strip() if "=" in s and not s.startswith(";") else ""
        if (section, key) in values:
            seen.add((section, key))
            line = f"{key} = {values[(section, key)]}"
        out.append(line)
    missing = set(values) - seen
    if missing:
        raise SystemExit(f"VGMPlay.ini has no key {sorted(missing)}")
    with open(os.path.join(dst, "VGMPlay.ini"), "w", encoding="latin-1", newline="\r\n") as f:
        f.write("\n".join(out))
    return dst


def render_all(manifest: list[dict], stimuli: str, work: str, chiptool: str) -> None:
    chips = {m["chip"] for m in manifest}
    ffmpeg = find_ffmpeg() if "snes" in chips or any(m.get("nsf") for m in manifest) else ""
    dirs = {c: vgmplay_dir(c) for c in (GENESIS_REFS if "genesis" in chips else ()) + (NES_REFS if "nes" in chips else ())}
    for m in manifest:
        name = m["name"]
        src = os.path.join(stimuli, m["file"])
        if m["chip"] in ("genesis", "nes"):
            for c in (GENESIS_REFS if m["chip"] == "genesis" else NES_REFS):
                d = dirs[c]
                tmp_vgm = os.path.join(d, name + ".vgm")
                shutil.copy2(src, tmp_vgm)
                subprocess.run([os.path.join(d, "VGMPlay.exe"), tmp_vgm], cwd=d, stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=60)
                os.replace(os.path.join(d, name + ".wav"), os.path.join(work, f"{name}.ref_{c}.wav"))
                os.remove(tmp_vgm)
            if m["chip"] == "genesis":
                subprocess.run([chiptool, "regs", "genesis", src, os.path.join(work, f"{name}.ours.wav")], check=True)
            else:
                # Engine output path (ImpulseSum kernel), and the integrated-step kernel of the
                # F2 fix for the separate measurement of that known output-path difference.
                subprocess.run([chiptool, "regs", "nes", src, os.path.join(work, f"{name}.ours.wav")], check=True)
                subprocess.run([chiptool, "regs", "nes", src, os.path.join(work, f"{name}.ours_int.wav"),
                                "--kernel", "integrated"], check=True)
                if m.get("nsf"):
                    # Third NES renderer: Game Music Emu's NSF player (same writes, frame-quantised).
                    subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-f", "libgme",
                                    "-sample_rate", "44100", "-i", os.path.join(stimuli, m["nsf"]), "-t", f"{m['end']:.3f}",
                                    "-c:a", "pcm_s16le", os.path.join(work, f"{name}.ref_gmensf.wav")], check=True)
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
    levels) take the same path on both sides; then a 60 Hz 4th-order Butterworth high-pass
    (removes the DC-step tails that would otherwise dominate fast releases; the lowest
    stimulus fundamental is 107 Hz, attenuated by 0.03 dB, identically on both sides);
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
    for q in (0.5411961, 1.3065630):
        y = biquad_highpass(y, rate, 60.0, q)
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


def biquad_highpass(x: list[float], rate: int, fc: float, q: float) -> list[float]:
    """2nd-order high-pass section (bilinear); two with Q 0.541/1.307 make a 4th-order Butterworth."""
    k = math.tan(math.pi * fc / rate)
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
            gain: float | None, coupling_ref: bool = False, coupling_test: bool = False,
            fixed_lag: int | None = None) -> dict:
    """coupling_ref / coupling_test: that side is a Genesis player render, which gets the
    coupling capacitor our output path has (15 kHz analysis low-pass on both Genesis sides)."""
    on = int(m["on"] * rate)
    lp = m["chip"] in ("genesis", "nes")
    if m["chip"] == "nes":
        # Mono chip: both players and ours write L = R; analyse one channel, report it twice.
        refL = refR = precondition(ref[0], rate, on, coupling=coupling_ref, lowpass=lp)
        ourL = ourR = precondition(ours[0], rate, on, coupling=coupling_test, lowpass=lp)
    else:
        refL, refR = (precondition(c, rate, on, coupling=coupling_ref, lowpass=lp) for c in ref)
        ourL, ourR = (precondition(c, rate, on, coupling=coupling_test, lowpass=lp) for c in ours)
    off = int(m["off"] * rate)
    steady = m.get("steady")
    if steady:
        sa, sb = int(steady[0] * rate), int(steady[1] * rate)
    elif m["kind"] == "gate" or m.get("gate_expected"):
        # Level window inside the gate (the note may end long before 'off').
        g0 = m.get("gate_start", m["on"])
        g1 = min(m["off"], m["gate_expected"] or m["off"])
        sa, sb = int((g0 + min(0.02, 0.2 * (g1 - g0))) * rate), int((g1 - min(0.01, 0.1 * (g1 - g0))) * rate)
        if m["kind"] in ("env", "sweep"):
            sa, sb = int((g0 + 0.005) * rate), int((g0 + 0.035) * rate)
    else:
        sa = on + int((0.1 if m["kind"] == "tone" else 0.15) * rate)
        sb = off - int(0.02 * rate)
        if sb - sa < int(0.05 * rate):
            sa, sb = on + int(0.01 * rate), off
    # Analysis channel: the side with more reference energy (a mono sum would cancel for
    # opposite-sign volumes and halve single-sided pans). Levels are reported per side.
    use_left = rms(refL[on:]) >= rms(refR[on:])
    ref_m, our_m = (refL, ourL) if use_left else (refR, ourR)
    if fixed_lag is None:
        lag, pol, corr = align(ref_m, our_m, max(0, on - int(0.02 * rate)))
    else:
        # Global lag of this chip (from its calibration stimulus); only the polarity is
        # measured here. Avoids period-ambiguous lags on slow attacks.
        lag = fixed_lag
        a0 = max(0, on)
        seg = min(len(ref_m) - a0, len(our_m) - a0 - lag, 8192)
        c = sum(ref_m[a0 + i] * our_m[a0 + i + lag] for i in range(max(0, seg)))
        e = math.sqrt(sum(v * v for v in ref_m[a0:a0 + seg]) * sum(v * v for v in our_m[a0 + lag:a0 + lag + seg])) or 1e-30
        pol, corr = (1 if c >= 0 else -1), c / e
    n = len(ref_m)

    # Everything below runs on the time base of the tested side (ours: the manifest times are
    # exact there); the reference is moved by -lag. The polarity is applied to the test side.
    refL = shifted(refL, -lag, n, 1.0)
    refR = shifted(refR, -lag, n, 1.0)
    ref_m = refL if use_left else refR
    oL = [pol * v for v in ourL[:n]] + [0.0] * max(0, n - len(ourL))
    oR = [pol * v for v in ourR[:n]] + [0.0] * max(0, n - len(ourR))
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
            f_o = f_ours if abs(res["pitch_cents"]) > 5.0 else f_ref
            for k in range(1, 11):
                if k * f_ref >= 0.45 * rate:
                    break
                mr = db(dtft_mag(xr, k * f_ref, rate) / wsum)
                mo = db(dtft_mag(xo, k * f_o, rate) / wsum)
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
    if m["kind"] in ("env", "ssg") and m["unit"] != "2a03":
        # (2A03 envelopes are staircases: fixed dB thresholds can sit on a step and flip by a
        # whole step for a 0.1 dB difference; they are timed step by step instead, env_steps.)
        res["env_timing"] = env_timing(env_r, env_o, to_i(on), to_i(off))
    if m["kind"] in ("ssg", "lfo_am"):
        res["modulation"] = modulation(env_r, env_o, to_i(on + int(0.05 * rate)), to_i(off - int(0.02 * rate)))

    if m["kind"] == "segments":
        res["segments"] = segment_levels(m, rate, ref_m, om, gain)
    if "gate_expected" in m:
        # On the raw renders (aligned, gain and polarity applied): the analysis high-pass would
        # turn the DC step at a note end into a tail of about 12 ms.
        ch = 0 if use_left else 1
        raw_r = shifted(ref[ch], -lag, n, 1.0)
        raw_o = [pol * gain * v for v in ours[ch][:n]] + [0.0] * max(0, n - len(ours[ch]))
        res["gate"] = gate_times(m, rate, raw_r, raw_o)
    if "env_steps" in m:
        ch = 0 if use_left else 1
        res["env_steps"] = env_steps(m, rate, shifted(ref[ch], -lag, n, 1.0),
                                     [pol * gain * v for v in ours[ch][:n]] + [0.0] * max(0, n - len(ours[ch])))
    if m["kind"] == "sweep":
        res["sweep"] = sweep_track(m, rate, ref_m, om)

    br = band_powers(ref_m, rate, on, min(n, off + int(0.3 * rate)))
    bo = band_powers(om, rate, on, min(n, off + int(0.3 * rate)))
    if br and bo:
        top = max(br)
        d = [o - r for r, o in zip(br, bo) if r > top - 70.0]
        res["spectral_db"] = math.sqrt(sum(v * v for v in d) / len(d)) if d else 0.0
    return res


def probe_db(x: list[float], rate: float, freqs: list[float]) -> list[float]:
    """Same measure as make_stimuli.probe_db: component amplitude (Hann DTFT, 2 |X| / sum(w))
    in dB re sqrt(2) x the AC RMS of the segment."""
    n = len(x)
    mean = sum(x) / n
    w = hann(n)
    xw = [(v - mean) * wv for v, wv in zip(x, w)]
    ref = math.sqrt(2.0 * sum((v - mean) ** 2 for v in x) / n) or 1e-30
    return [db(2.0 * dtft_mag(xw, f, rate) / sum(w) / ref) for f in freqs]


def segment_levels(m: dict, rate: int, ref: list[float], ours: list[float], gain: float) -> list[dict]:
    """Per-segment levels (RMS, or peak for mode 'peak') of both sides after the global gain,
    the formula prediction ('pred', AC RMS in mixer units, scaled by the same gain), levels
    relative to the first RMS segment (gain independent: this is what checks the non-linear
    mixer), and intermodulation probes where the stimulus lists them."""
    out = []
    first = None
    for seg in m["segments"]:
        a, b = int(seg["t0"] * rate), int(seg["t1"] * rate)
        peak = seg.get("mode") == "peak"
        f = (lambda x: max(abs(v) for v in x)) if peak else rms
        e = dict(label=seg["label"], mode="peak" if peak else "rms", ref_db=db(f(ref[a:b])), ours_db=db(f(ours[a:b])))
        e["diff_db"] = e["ours_db"] - e["ref_db"]
        e["silent"] = seg.get("pred") == 0.0        # documented silence (e.g. volume 0)
        if seg.get("pred"):
            e["pred_db"] = db(seg["pred"] * gain)
        if not peak and first is None and seg.get("pred"):
            first = e
        if seg.get("probes"):
            e["probes"] = [dict(hz=hz, ref_db=r, ours_db=o, pred_db=p) for hz, r, o, p in
                           zip(seg["probes"], probe_db(ref[a:b], rate, seg["probes"]), probe_db(ours[a:b], rate, seg["probes"]),
                               seg.get("probes_pred", [None] * len(seg["probes"])))]
        out.append(e)
    if first is not None:
        for e in out:
            if e["mode"] == "rms" and not e["silent"]:
                e["ref_rel_db"] = e["ref_db"] - first["ref_db"]
                e["ours_rel_db"] = e["ours_db"] - first["ours_db"]
                if "pred_db" in e:
                    e["pred_rel_db"] = e["pred_db"] - first["pred_db"]
    return out


def p2p_track(x: list[float], w: int) -> list[float]:
    """Peak-to-peak value of x over the trailing window [i - w + 1, i] (monotonic queues)."""
    from collections import deque
    hi, lo, out = deque(), deque(), []
    for i, v in enumerate(x):
        while hi and x[hi[-1]] <= v:
            hi.pop()
        hi.append(i)
        while lo and x[lo[-1]] >= v:
            lo.pop()
        lo.append(i)
        if hi[0] <= i - w:
            hi.popleft()
        if lo[0] <= i - w:
            lo.popleft()
        out.append(x[hi[0]] - x[lo[0]])
    return out


def gate_times(m: dict, rate: int, ref: list[float], ours: list[float]) -> dict:
    """Start and end of a note gated by the chip itself (length counter, linear counter,
    envelope reaching 0, sweep mute, one-shot DMC), from the edges of each raw render: the
    activity is the largest sample-to-sample step over a trailing window of 1.1 periods of the
    tone (edges of a square or of the triangle / DMC staircase; blind to DC steps and to slow
    DC-blocker tails, and a held level, e.g. a halted triangle, counts as silence).
    start = first edge (activity above steady - 20 dB); end = last edge before the key-off
    write that is larger than steady + gate_db (default -20 dB; envelope decays -30 dB, i.e.
    the level-1 edges count and level 0 is the end). The steady value is each
    side's median over the first 5..30 ms of the gate. Times in ms after the note-start write,
    with the documented schedule of make_stimuli ('expected') and the resolution (half a period
    of the last audible tone: its last edge precedes the clock by up to that much)."""
    t_on = m["on"]
    g0 = m.get("gate_start", t_on)
    g1 = m.get("gate_expected")
    cpu = 1662607.0 if m.get("pal") else 1789773.0
    f_start = f_end = m["f0"]
    steps = m.get("sweep_steps")
    if steps and g1 is not None:
        last = steps[-2] if len(steps) > 1 and steps[-1][0] >= g1 - 1e-9 else steps[-1]
        f_end = cpu / (16.0 * (last[1] + 1))
    w_start = max(4, int(round(1.1 * rate / max(f_start, 1.0))))
    thr_end = 10.0 ** (m.get("gate_db", -20.0) / 20.0)
    stop = m["off"] if g1 is None else g1

    def one(x: list[float]) -> tuple[float | None, float | None]:
        d = [0.0] + [abs(x[i] - x[i - 1]) for i in range(1, len(x))]
        act = p2p_track(d, w_start)
        a = int((g0 + 0.005) * rate) + w_start
        b = max(a + 1, min(int((g0 + 0.03) * rate) + w_start, int((stop - 0.001) * rate)))
        seg = sorted(act[a:b])
        steady = seg[len(seg) // 2]
        if steady <= 0.0:
            return None, None
        start = next((i for i in range(max(0, int((t_on - 0.01) * rate)), len(act)) if act[i] > 0.1 * steady), None)
        end = None
        if g1 is not None and start is not None:
            # The last edge before the key-off write (the gate is silent from its end to 'off').
            stop_i = min(len(d), int((m["off"] + 0.002) * rate))
            end = next((i for i in range(stop_i - 1, start, -1) if d[i] > thr_end * steady), None)
        conv = lambda i: None if i is None else i * 1000.0 / rate - t_on * 1000.0
        return conv(start), conv(end)

    rs, re_ = one(ref)
    os_, oe = one(ours)
    return dict(start_ref_ms=rs, start_ours_ms=os_, start_expected_ms=(g0 - t_on) * 1000.0,
                end_ref_ms=re_, end_ours_ms=oe, end_expected_ms=None if g1 is None else (g1 - t_on) * 1000.0,
                resolution_ms=500.0 / max(f_end, 1.0), approx=bool(m.get("gate_approx")))


def env_steps(m: dict, rate: int, ref: list[float], ours: list[float]) -> dict:
    """Times of the 2A03 envelope steps against the documented staircase m['env_steps']
    ([time, level], research "Envelope generator"): the square's amplitude is its AC RMS over a
    trailing window of 4 periods of the raw render (independent of DC and of where the band-limited
    edges fall between samples); a step is the crossing of the midpoint between the two levels'
    mixer amplitudes (pulse group formula, scaled to each side's own level-15 value), searched
    within +/- 45 % of the step interval around the documented time, and dated at the window
    centre. Resolution: half a period of the tone (a level change waits for the next edge)."""
    steps = m["env_steps"]
    w = max(8, int(round(4.0 * rate / m["f0"])))
    amp = lambda k: 0.0 if k == 0 else 95.88 / (8128.0 / k + 100.0)

    def ac_track(x: list[float]) -> list[float]:
        s1 = s2 = 0.0
        out = []
        for i, v in enumerate(x):
            s1 += v
            s2 += v * v
            if i >= w:
                u = x[i - w]
                s1 -= u
                s2 -= u * u
            k = min(i + 1, w)
            out.append(math.sqrt(max(s2 / k - (s1 / k) ** 2, 0.0)))
        return out

    def one(x: list[float]) -> list:
        p = ac_track(x)
        t0 = steps[0][0]
        t1 = steps[1][0] if len(steps) > 1 else t0 + 0.004
        a = int((t0 + 0.0005) * rate) + w
        b = max(a + 1, int((t1 - 0.0005) * rate))
        seg = sorted(p[a:b])
        a15 = seg[len(seg) // 2]
        out = []
        for j in range(1, len(steps)):
            (tp, lp), (t, lv) = steps[j - 1], steps[j]
            dt = t - tp
            thr = 0.5 * (amp(lp) + amp(lv)) / amp(15) * a15
            lo = max(0, int((t - 0.45 * dt) * rate) + w // 2)
            hi = min(len(p), int((t + 0.45 * dt) * rate) + w // 2)
            hit = (lambda v: v < thr) if lv < lp else (lambda v: v > thr)
            i = next((i for i in range(lo, hi) if hit(p[i])), None)
            out.append(None if i is None else (i - w / 2.0) / rate)
        return out

    r, o = one(ref), one(ours)
    doc = [st[0] for st in steps[1:]]
    diff = lambda a, b: [1000.0 * (x - y) for x, y in zip(a, b) if x is not None and y is not None]
    worst = lambda d: max(d, key=abs) if d else None
    d_rd = sorted(diff(r, doc))
    return dict(n=len(doc), found_ref=sum(x is not None for x in r), found_ours=sum(x is not None for x in o),
                ours_vs_ref_ms=worst(diff(o, r)), ours_vs_doc_ms=worst(diff(o, doc)), ref_vs_doc_ms=worst(diff(r, doc)),
                ref_median_vs_doc_ms=d_rd[len(d_rd) // 2] if d_rd else None, resolution_ms=500.0 / m["f0"])


def zc_freq(x: list[float], rate: int) -> float | None:
    """Mean frequency from the upward zero crossings of a segment (linear interpolation)."""
    cr = []
    for i in range(1, len(x)):
        if x[i - 1] < 0.0 <= x[i]:
            cr.append(i - 1 + (-x[i - 1]) / (x[i] - x[i - 1]))
    if len(cr) < 2:
        return None
    return (len(cr) - 1) * rate / (cr[-1] - cr[0])


def sweep_track(m: dict, rate: int, ref: list[float], ours: list[float]) -> dict:
    """Frame-wise pitch of a sweep (zero-crossing frequency on frames of 0.6 x the documented step
    interval, 128..1024 samples, hop an eighth of a frame), ours
    vs the reference and both vs the documented trajectory (m['sweep_steps']: [time, period];
    pulse f = cpu / (16 (t + 1))), on the frames that contain no documented period change (nor
    one in the 2 ms before them): a reference whose steps come at other times then differs by
    whole steps. Summaries: median / 90th percentile / max |cents|."""
    cpu = 1662607.0 if m.get("pal") else 1789773.0
    steps = m["sweep_steps"]
    end = m["off"] if m.get("gate_expected") is None else m["gate_expected"]
    # Frames of 0.6 x the shortest documented step interval (128..1024 samples), hop n / 8.
    gaps = [b[0] - a[0] for a, b in zip(steps[1:], steps[2:])]
    n = int(min(1024, max(128, 0.6 * min(gaps) * rate if gaps else 1024)))
    hop = max(16, n // 8)
    lo = max(rms(ref[int(m["on"] * rate):int(end * rate)]), 1e-12)
    d_ref, d_spec_o, d_spec_r = [], [], []
    for s in range(int((m["on"] + 0.005) * rate), int((end - 0.003) * rate) - n, hop):
        fr, fo = zc_freq(ref[s:s + n], rate), zc_freq(ours[s:s + n], rate)
        if rms(ref[s:s + n]) < lo * 0.01:
            continue
        t0, t1 = s / rate, (s + n) / rate
        inside = [st for st in steps if st[0] <= t0]
        # Clean frames only: no documented period change inside the frame or in the 20 ms
        # before it (the analysis filters' transient after a pitch step).
        nxt = [st for st in steps if t0 - 0.002 < st[0] < t1 and st is not steps[0]]
        if inside and not nxt:
            if fr and fo:
                d_ref.append(1200.0 * math.log2(fo / fr))
            f_spec = cpu / (16.0 * (inside[-1][1] + 1))
            if fo:
                d_spec_o.append(1200.0 * math.log2(fo / f_spec))
            if fr:
                d_spec_r.append(1200.0 * math.log2(fr / f_spec))

    def summ(d: list[float]) -> dict:
        a = sorted(abs(v) for v in d)
        if not a:
            return dict(n=0, median=None, p90=None, max=None)
        return dict(n=len(a), median=a[len(a) // 2], p90=a[int(0.9 * (len(a) - 1))], max=a[-1])

    return dict(ours_vs_ref=summ(d_ref), ours_vs_spec=summ(d_spec_o), ref_vs_spec=summ(d_spec_r), frame=n)


def env_timing(env_r: list[float], env_o: list[float], i_on: int, i_off: int) -> list[dict]:
    """Crossing times in ms (envelope hop ENV_HOP_MS, interpolated). Levels closer than 10 dB
    to the reference's residual floor (where only
    filtered DC-step and ladder residues remain) are skipped: crossings there measure the
    floor, not the envelope. The floor is the median of the last 20 ms when they start at
    least 15 ms after key-off (otherwise no floor is applied)."""
    ms = ENV_HOP_MS
    out = []
    last = int(20.0 / ms)
    floor = -240.0
    if len(env_r) - last > i_off + int(15.0 / ms):
        tail = sorted(env_r[-last:])
        floor = tail[len(tail) // 2] + 10.0
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
        if peak_r + lv < floor:
            continue
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
            if lvl_off + lv < peak_r - 65.0 or lvl_off + lv < floor:
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
        # NES envelopes step on the frame sequencer: a whole quarter frame late is under 3 % of
        # a long decay, so an absolute limit applies there as well.
        d_ms = abs(e["ours_ms"] - e["ref_ms"])
        if (abs(e["diff_pct"]) > TH_ENV_PCT and d_ms > TH_ENV_MIN_MS) or (r["unit"] == "2a03" and d_ms > TH_NES_EVENT_MS):
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
    if r["kind"] == "mute":
        lv = r["level"]["L"]
        if max(lv["ref_db"], lv["ours_db"]) > -60.0 and abs(lv["diff_db"]) > 6.0:
            out.append(f"mute: ref {lv['ref_db']:.1f} dBFS, ours {lv['ours_db']:.1f} dBFS")
    for e in r.get("segments", []):
        if e.get("silent"):
            if max(e["ref_db"], e["ours_db"]) > -60.0 and abs(e["diff_db"]) > 6.0:
                out.append(f"segment '{e['label']}' not silent: ref {e['ref_db']:.1f}, ours {e['ours_db']:.1f} dBFS")
            continue
        if e["mode"] == "rms" and max(e["ref_db"], e["ours_db"]) > -70.0 and abs(e["diff_db"]) > TH_LEVEL_DB:
            out.append(f"segment '{e['label']}' level {e['diff_db']:+.2f} dB (ref {e['ref_db']:.1f} dBFS)")
        if e["mode"] == "peak" and max(e["ref_db"], e["ours_db"]) > -60.0 and abs(e["diff_db"]) > 3.0:
            out.append(f"segment '{e['label']}' peak {e['diff_db']:+.2f} dB (ref {e['ref_db']:.1f}, ours {e['ours_db']:.1f} dBFS)")
        if "ref_rel_db" in e and abs(e["ours_rel_db"] - e["ref_rel_db"]) > TH_LEVEL_DB:
            out.append(f"segment '{e['label']}' re first segment: ours {e['ours_rel_db']:+.2f}, ref {e['ref_rel_db']:+.2f} dB")
        for p in e.get("probes", []):
            if max(p["ref_db"], p["ours_db"]) > -60.0 and abs(p["ours_db"] - p["ref_db"]) > TH_HARM_DB:
                out.append(f"segment '{e['label']}' IMD {p['hz']:.1f} Hz: ours {p['ours_db']:.1f}, ref {p['ref_db']:.1f} dB")
    g = r.get("gate")
    if g:
        for stage in ("start", "end"):
            o, rf = g[f"{stage}_ours_ms"], g[f"{stage}_ref_ms"]
            if (o is None) != (rf is None):
                out.append(f"gate {stage}: ref {rf}, ours {o}")
            elif o is not None and abs(o - rf) > max(TH_NES_EVENT_MS, g.get("resolution_ms", 0.0)):
                out.append(f"gate {stage} {o - rf:+.2f} ms (ref {rf:.2f} ms, ours {o:.2f} ms)")
    es = r.get("env_steps")
    if es:
        tol = TH_NES_EVENT_MS + es["resolution_ms"]
        if es["found_ref"] != es["found_ours"]:
            out.append(f"envelope steps found: ref {es['found_ref']}, ours {es['found_ours']} of {es['n']}")
        if es["ours_vs_ref_ms"] is not None and abs(es["ours_vs_ref_ms"]) > tol:
            out.append(f"envelope step timing: worst ours - ref {es['ours_vs_ref_ms']:+.2f} ms")
    sw = r.get("sweep")
    if sw:
        s = sw["ours_vs_ref"]
        if s["n"] and (s["median"] > TH_PITCH_CENTS or s["p90"] > 10.0):
            out.append(f"sweep pitch track |ours - ref| median {s['median']:.2f}, p90 {s['p90']:.2f}, max {s['max']:.1f} cents")
    return out


def spec_deviations(r: dict) -> list[str]:
    """Ours against the documented schedule / formula (independent of the reference)."""
    out = []
    g = r.get("gate")
    if g and not g["approx"]:
        for stage in ("start", "end"):
            o, x = g[f"{stage}_ours_ms"], g[f"{stage}_expected_ms"]
            if o is not None and x is not None and abs(o - x) > max(TH_NES_EVENT_MS, g.get("resolution_ms", 0.0)):
                out.append(f"gate {stage} {o - x:+.2f} ms vs documented {x:.2f} ms")
    es = r.get("env_steps")
    if es and es["ours_vs_doc_ms"] is not None and abs(es["ours_vs_doc_ms"]) > TH_NES_EVENT_MS + es["resolution_ms"]:
        out.append(f"envelope steps vs documented: worst {es['ours_vs_doc_ms']:+.2f} ms")
    if es and es["found_ours"] != es["n"]:
        out.append(f"envelope steps vs documented: {es['found_ours']} of {es['n']} found")
    sw = r.get("sweep")
    # Frames under 512 samples (sweep periods P <= 1) resolve about 2 cents: the analysis
    # filters' transient after each 8 ms step is still inside the next frame.
    if sw and sw["ours_vs_spec"]["n"] and sw["ours_vs_spec"]["median"] > (TH_PITCH_CENTS if sw.get("frame", 1024) >= 512 else 2.0):
        s = sw["ours_vs_spec"]
        out.append(f"sweep vs documented trajectory: median {s['median']:.2f}, p90 {s['p90']:.2f} cents")
    for e in r.get("segments", []):
        if "pred_rel_db" in e and abs(e["ours_rel_db"] - e["pred_rel_db"]) > TH_LEVEL_DB:
            out.append(f"segment '{e['label']}' re first: ours {e['ours_rel_db']:+.2f}, formula {e['pred_rel_db']:+.2f} dB")
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


def nes_tables(res: list[dict]) -> list[str]:
    """NES-specific tables: mixer segments and intermodulation, frame-sequencer events, sweeps."""
    f = lambda v, fmt="+.2f": "" if v is None else format(v, fmt)
    lines = []
    seg_rows = [r for r in res if r.get("segments")]
    if seg_rows:
        lines.append("#### 2a03 mixer and multi-segment stimuli\n")
        lines.append("Levels after the global gain. 're first' = level relative to the first segment of the same stimulus "
                     "(gain independent; the formula column is the exact non-linear mixer of research \"Mixer\"). "
                     "Peak segments: dBFS of the largest sample.\n")
        lines.append("| stimulus | segment | ref (dBFS) | ours - ref (dB) | ours re first | ref re first | formula re first |\n"
                     "|---|---|---|---|---|---|---|")
        for r in seg_rows:
            for e in r["segments"]:
                lines.append(f"| {r['name']} | {e['label']}{' (peak)' if e['mode'] == 'peak' else ''} | {e['ref_db']:.2f} | "
                             f"{e['diff_db']:+.2f} | {f(e.get('ours_rel_db'))} | {f(e.get('ref_rel_db'))} | {f(e.get('pred_rel_db'))} |")
        lines.append("")
        probes = [(r, e, p) for r in seg_rows for e in r["segments"] for p in e.get("probes", [])]
        if probes:
            lines.append("Intermodulation products (component amplitude in dB re sqrt(2) x segment RMS; no linear "
                         "component exists at these frequencies):\n")
            lines.append("| stimulus | segment | Hz | ours | ref | formula |\n|---|---|---|---|---|---|")
            for r, e, p in probes:
                lines.append(f"| {r['name']} | {e['label']} | {p['hz']:.1f} | {p['ours_db']:.1f} | {p['ref_db']:.1f} | "
                             f"{f(p['pred_db'], '.1f')} |")
            lines.append("")
    gate_rows = [r for r in res if r.get("gate")]
    if gate_rows:
        lines.append("#### 2a03 frame-sequencer events (ms after the note-start write)\n")
        lines.append("From the edges of the raw renders (largest sample step over 1.1 periods of the tone): start = "
                     "first edge, end = last edge (envelope decays: level 0). 'documented' = schedule computed by make_stimuli.py "
                     "from research \"Frame counter\" / \"Sweep unit\" (a = approximate); res = half a period of the "
                     "tone at the end (the last edge precedes the clock by up to that much).\n")
        lines.append("| stimulus | start ours | start ref | start documented | end ours | end ref | end documented | res |\n"
                     "|---|---|---|---|---|---|---|---|")
        for r in gate_rows:
            g = r["gate"]
            lines.append(f"| {r['name']} | {f(g['start_ours_ms'], '.2f')} | {f(g['start_ref_ms'], '.2f')} | "
                         f"{f(g['start_expected_ms'], '.2f')} | {f(g['end_ours_ms'], '.2f')} | {f(g['end_ref_ms'], '.2f')} | "
                         f"{f(g['end_expected_ms'], '.2f')}{' a' if g['approx'] else ''} | {g.get('resolution_ms', 0.0):.2f} |")
        lines.append("")
    es_rows = [r for r in res if r.get("env_steps")]
    if es_rows:
        lines.append("#### 2a03 envelope staircase (worst step-time difference, ms; steps found of documented)\n")
        lines.append("| stimulus | steps ours / ref / doc | ours - documented | ref - documented (worst / median) | ours - ref |\n"
                     "|---|---|---|---|---|")
        for r in es_rows:
            e = r["env_steps"]
            lines.append(f"| {r['name']} | {e['found_ours']} / {e['found_ref']} / {e['n']} | {f(e['ours_vs_doc_ms'])} | "
                         f"{f(e['ref_vs_doc_ms'])} / {f(e['ref_median_vs_doc_ms'])} | {f(e['ours_vs_ref_ms'])} |")
        lines.append("")
    sweep_rows = [r for r in res if r.get("sweep")]
    if sweep_rows:
        lines.append("#### 2a03 sweeps (frame-wise pitch, |cents|: median / 90th percentile / max)\n")
        lines.append("| stimulus | ours vs ref | ours vs documented | ref vs documented |\n|---|---|---|---|")
        c = lambda s: "" if not s["n"] else f"{s['median']:.2f} / {s['p90']:.2f} / {s['max']:.1f}"
        for r in sweep_rows:
            s = r["sweep"]
            lines.append(f"| {r['name']} | {c(s['ours_vs_ref'])} | {c(s['ours_vs_spec'])} | {c(s['ref_vs_spec'])} |")
        lines.append("")
    return lines


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
                 "per-channel level 0.5 dB; NES frame-sequencer events 1 ms plus half a period of the tone, sweep "
                 "pitch tracks 1 cent median / 10 cents 90th percentile. Columns: lag = our delay in samples after alignment (inv = inverted "
                 "polarity); null = reference energy over residual energy after alignment and gain; env shape = RMS dB "
                 "difference of the 4 ms sliding RMS envelopes; spectral = RMS dB difference of 1/6-octave band powers.\n")
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
        for unit in ("ym2612", "sn76489", "sdsp", "2a03"):
            rows = [r for r in res if r["unit"] == unit]
            if not rows:
                continue
            lines.append(f"#### {unit}\n")
            lines.append(HEADER)
            lines.extend(summary_row(r) for r in rows)
            lines.append("")
        if any(r["unit"] == "2a03" for r in res):
            lines.extend(nes_tables(res))
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
        spec = [(r, spec_deviations(r)) for r in res if r["unit"] == "2a03"]
        if spec:
            lines.append(f"#### Test side against the documented schedule and mixer formula ({cmp_name})\n")
            flagged = [f"* **{r['name']}**: " + "; ".join(d) for r, d in spec if d]
            lines.extend(flagged or ["None."])
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
    ap.add_argument("--chips", default="", help="comma-separated subset (genesis,snes,nes); the other chips' "
                    "results are taken from the existing --json file")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    args = ap.parse_args()

    full_manifest = json.load(open(os.path.join(args.stimuli, "manifest.json"), encoding="utf-8"))
    manifest = full_manifest
    chips = {c.strip() for c in args.chips.split(",") if c.strip()} or {m["chip"] for m in manifest}
    manifest = [m for m in manifest if m["chip"] in chips]
    if args.only:
        keep = set(CALIBRATION.values()) | set(LAG_CALIBRATION.values())
        manifest = [m for m in manifest if args.only in m["name"] or m["name"] in keep]
    os.makedirs(args.work, exist_ok=True)
    if not args.no_render:
        render_all(manifest, os.path.abspath(args.stimuli), os.path.abspath(args.work), os.path.abspath(args.chiptool))

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

    results: dict[str, list[dict]] = {}
    with ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for cmp_name, chip, test, refk in COMPARISONS:
            rows = [m for m in manifest if m["chip"] == chip and (refk != "ref_gmensf" or m.get("nsf"))]
            if not rows:
                continue
            # Calibration stimuli first (gain and, for GLOBAL_LAG_UNITS, the lag), then the rest
            # in parallel with those constants.
            cal_rows = [m for m in rows if m["name"] == CALIBRATION.get(m["unit"])]
            unit_gain: dict[str, float] = {}
            unit_lag: dict[str, int] = {}
            cache: dict[str, dict] = {}
            for m in cal_rows:
                r = run_row((m, args.work, chip, test, refk, timebase, None, None))
                unit_gain[m["unit"]] = r["gain"]
                if m["unit"] in GLOBAL_LAG_UNITS:
                    unit_lag[m["unit"]] = r["lag"]
                cache[m["name"]] = r
            # Units whose time base is measured on a separate stimulus (LAG_CALIBRATION).
            for m in rows:
                if m["name"] == LAG_CALIBRATION.get(m["unit"]):
                    r = run_row((m, args.work, chip, test, refk, timebase, unit_gain.get(m["unit"]), None))
                    unit_lag[m["unit"]] = r["lag"]
                    cache[m["name"]] = r
            jobs = [(m, args.work, chip, test, refk, timebase, unit_gain.get(m["unit"]),
                     unit_lag.get(m["unit"]) if uses_global_lag(m) else None) for m in rows if m["name"] not in cache]
            for m, r in zip([j[0] for j in jobs], pool.map(run_row, jobs)):
                cache[m["name"]] = r
            for m in rows:
                r = cache[m["name"]]
                print(f"{cmp_name[:28]:28s} {m['name']:24s} null {r['null_db']:6.1f} dB  devs {len(deviations(r))}", flush=True)
            results[cmp_name] = [cache[m["name"]] for m in rows]

    # Merge with the stored results of the chips not run now, keeping the comparison order.
    stored: dict = {}
    if os.path.exists(args.json):
        stored = json.load(open(args.json, encoding="utf-8"))
    merged: dict[str, list[dict]] = {}
    for cmp_name, chip, _, _ in COMPARISONS:
        if cmp_name in results:
            merged[cmp_name] = results[cmp_name]
        elif cmp_name in stored and chip not in chips:
            merged[cmp_name] = stored[cmp_name]
    os.makedirs(os.path.dirname(args.json), exist_ok=True)
    with open(args.json, "w", encoding="utf-8") as f:
        json.dump(merged, f, indent=1)
    # Gain constants and time base, from the rows (also for merged, stored comparisons).
    gains = {(cmp_name, r["unit"]): r["gain"] for cmp_name, res in merged.items() for r in res
             if r["name"] == CALIBRATION.get(r["unit"])}
    for res in merged.values():
        for r in res:
            if r["unit"] == "ym2612" and r.get("timebase", 1.0) != 1.0:
                timebase["ref_nuked"] = r["timebase"]
    write_report(args.report, merged, full_manifest, gains, timebase)
    print(f"report -> {args.report}")


# (name, chip, test side, reference side); side = render suffix in the work directory.
COMPARISONS = [
    ("ours vs Nuked OPN2 + MAME SN76496 (VGMPlay)", "genesis", "ours", "ref_nuked"),
    ("ours vs MAME/GPGX YM2612 + Maxim SN76489 (VGMPlay)", "genesis", "ours", "ref_mame"),
    ("reference vs reference: MAME/GPGX + Maxim against Nuked + MAME", "genesis", "ref_mame", "ref_nuked"),
    ("ours vs Game Music Emu SPC (FFmpeg libgme)", "snes", "ours", "ref_gme"),
    ("NES: ours vs NSFPlay core (VGMPlay)", "nes", "ours", "ref_nsfplay"),
    ("NES: ours vs MAME core (VGMPlay)", "nes", "ours", "ref_nesmame"),
    ("NES: reference vs reference: MAME core against NSFPlay core", "nes", "ref_nesmame", "ref_nsfplay"),
    ("NES: ours with the integrated-step kernel vs NSFPlay core", "nes", "ours_int", "ref_nsfplay"),
    ("NES: ours (engine, ImpulseSum kernel) vs ours with the integrated-step kernel (F2 measurement)", "nes", "ours", "ours_int"),
    ("NES: ours vs Game Music Emu NSF player (FFmpeg libgme; NSF subset)", "nes", "ours", "ref_gmensf"),
]
NES_TIMING_KINDS = {"env", "ssg", "gate", "sweep", "segments"}
# Time-base (lag) calibration measured on another stimulus than the gain: the 2A03 pulse phase
# at a note start depends on the timer divider, which a $4003 write does not reset (research
# "Pulse channels") but players may; the $4011 direct-load square has edges set by the write
# times alone.
LAG_CALIBRATION = {"2a03": "nes_dmc_direct"}


def uses_global_lag(m: dict) -> bool:
    """YM2612 and S-DSP: every stimulus but noise (see GLOBAL_LAG_UNITS). 2A03: the timing
    stimuli, whose frame-sequencer events must be compared on one time base (a per-stimulus
    correlation could absorb a quarter-frame offset); tones and noise align per stimulus
    because the initial timer and LFSR phases of the players are unknown."""
    if m["unit"] == "2a03":
        return m["kind"] in NES_TIMING_KINDS
    return m["unit"] in GLOBAL_LAG_UNITS and m["kind"] != "noise"



def run_row(job: tuple) -> dict:
    """One stimulus of one comparison (runs in a worker process)."""
    m, work, chip, test, refk, timebase, gain, fixed_lag = job
    rp = os.path.join(work, f"{m['name']}.{refk}.wav")
    tp = os.path.join(work, f"{m['name']}.{test}.wav")
    rate, rl, rr = read_wav(rp)
    # The DAC stimulus is exempt: its pitch is set by the VGM write timing, which the
    # player gets right (measured: uncorrected DAC pitch matches ours and the MAME core).
    tb = timebase.get(refk, 1.0) if m["unit"] == "ym2612" and not m["name"].startswith("fm_dac") else 1.0
    if tb != 1.0:
        # y[i] = x(i / tb): a reference running fast by tb is slowed back down. Cached on disk
        # next to the render (16-bit), keyed by the factor.
        cp = os.path.join(work, f"{m['name']}.{refk}.tb{tb:.6f}.wav")
        if os.path.exists(cp) and os.path.getmtime(cp) >= os.path.getmtime(rp):
            _, rl, rr = read_wav(cp)
        else:
            rl, rr = resample(rl, 1.0 / tb), resample(rr, 1.0 / tb)
            write_wav(cp, rate, rl, rr)
    rate2, tl, tr = read_wav(tp)
    if rate != rate2:
        raise SystemExit(f"{m['name']}: sample rates differ ({rate} vs {rate2})")
    n = min(len(rl), len(tl))
    # Player renders get the output coupling capacitor our Genesis / NES output paths have.
    coupled = lambda side: chip in ("genesis", "nes") and side.startswith("ref_")
    r = analyse(m, rate, (rl[:n], rr[:n]), (tl[:n], tr[:n]), gain,
                coupling_ref=coupled(refk), coupling_test=coupled(test), fixed_lag=fixed_lag)
    r["timebase"] = tb
    return r


if __name__ == "__main__":
    sys.exit(main())
