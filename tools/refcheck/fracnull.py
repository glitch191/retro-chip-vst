"""Null test at a fractional lag (diagnostic for docs/research/refcheck-report.md).

compare.py aligns in whole host samples. Our output has a constant sub-sample latency against
each reference, so its null values are bounded by the leftover fraction (a 0.365-sample
residual limits a 440 Hz sine to about 33 dB). This script measures that fraction on the
YM2612 calibration stimulus (phase of H1, after compare.py's pre-conditioning), delays our
render by it with a windowed-sinc fractional delay, and prints the null with and without it.

Usage::

    python tools\\refcheck\\fracnull.py [--ref ref_nuked|ref_mame] NAME [NAME ...]

Uses the renders of the last compare.py run (build-reports/refcheck/renders). Standard-library
Python only; slow (pure-Python convolution, a few seconds per stimulus).
"""

from __future__ import annotations

import argparse
import cmath
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import compare as C  # noqa: E402

TIMEBASE_SUFFIX = {"ref_nuked": ".tb1.000132"}   # compare.py's cached, time-base-corrected renders


def fractional_delay(x: list[float], d: float, half: int = 24) -> list[float]:
    """y[n] = x(n - d), Hann-windowed sinc with 2 * half + 1 taps."""
    taps = []
    for k in range(-half, half + 1):
        t = k - d
        s = 1.0 if abs(t) < 1e-12 else math.sin(math.pi * t) / (math.pi * t)
        taps.append(s * (0.5 + 0.5 * math.cos(math.pi * t / (half + 1))))
    n = len(x)
    y = [0.0] * n
    for i in range(n):
        acc = 0.0
        for j, h in enumerate(taps):
            m = i - (j - half)
            if 0 <= m < n:
                acc += h * x[m]
        y[i] = acc
    return y


def load(work: str, name: str, refk: str) -> tuple[int, tuple, tuple]:
    suffix = "" if name.startswith("fm_dac") else TIMEBASE_SUFFIX.get(refk, "")
    rate, rl, rr = C.read_wav(os.path.join(work, f"{name}.{refk}{suffix}.wav"))
    _, ol, orr = C.read_wav(os.path.join(work, f"{name}.ours.wav"))
    n = min(len(rl), len(ol))
    return rate, (rl[:n], rr[:n]), (ol[:n], orr[:n])


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--ref", default="ref_nuked")
    ap.add_argument("--work", default=os.path.join(C.ROOT, "build-reports", "refcheck", "renders"))
    ap.add_argument("--stimuli", default=os.path.join(C.ROOT, "build-reports", "refcheck", "stimuli"))
    ap.add_argument("names", nargs="*")
    args = ap.parse_args()
    manifest = {m["name"]: m for m in json.load(open(os.path.join(args.stimuli, "manifest.json"), encoding="utf-8"))}

    cal = manifest[C.CALIBRATION["ym2612"]]
    rate, ref, ours = load(args.work, cal["name"], args.ref)
    on = int(cal["on"] * rate)
    r = C.precondition(ref[0], rate, on, coupling=True, lowpass=True)
    o = C.precondition(ours[0], rate, on, lowpass=True)
    a = on + int(0.1 * rate)
    b = min(int(cal["off"] * rate) - int(0.02 * rate), a + 16384)
    w = 2.0 * math.pi * cal["f0"] / rate
    phase = lambda x: cmath.phase(sum(x[i] * cmath.exp(-1j * w * i) for i in range(a, b)))
    d = (phase(o) - phase(r) + math.pi) % (2.0 * math.pi) - math.pi
    late = -d / w                       # ours later than the reference, in host samples
    lag = C.analyse(cal, rate, ref, ours, None, coupling_ref=True)["lag"]
    frac = -(late - lag)                # delay applied to ours so that the integer lag is exact
    print(f"{args.ref}: ours later by {late:+.3f} samples, integer lag {lag:+d}, fractional delay {frac:+.3f}")

    gain = None
    for name in [cal["name"]] + [n for n in args.names if n != cal["name"]]:
        m = manifest[name]
        rate, ref, ours = load(args.work, name, args.ref)
        shifted = tuple(fractional_delay(c, frac) for c in ours)
        r0 = C.analyse(m, rate, ref, ours, gain, coupling_ref=True, fixed_lag=lag)
        r1 = C.analyse(m, rate, ref, shifted, gain, coupling_ref=True, fixed_lag=lag)
        if gain is None:
            gain = r1["gain"]
        print(f"{name:18s} null {r0['null_db']:5.1f} dB at the integer lag, {r1['null_db']:5.1f} dB at the fractional lag", flush=True)


if __name__ == "__main__":
    main()
