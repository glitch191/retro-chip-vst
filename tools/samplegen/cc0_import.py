"""Convert CC0 instrument recordings into chip samples, the way 1990s composers used sample CDs.

Driven by ``tools/samplegen/cc0_manifest.json``; called from ``tools/gen_samples.py``.
Standard library only, deterministic (same sources -> byte-identical WAVs).

Sources
-------

The manifest names each source repository (URL, pinned commit, licence) and, for every
output sample, the source files ("layers") by repository path and git blob id. The
files live in ``third_party/cc0/<repo>/<path>`` (git-ignored). ``fetch()`` (or
``python tools/samplegen/cc0_import.py --fetch``, or ``gen_samples.py --fetch-cc0``)
downloads the missing ones from ``raw.githubusercontent.com`` at the pinned commit,
together with each repository's licence file, and checks every file against its git
blob id. Only the converted results go into ``assets/samples``.

Conversion of one sample (``build_sample``)
-------------------------------------------

1. Read each layer (PCM 8/16/24/32-bit or 32-bit float, any channel count, WAVE_FORMAT
   _EXTENSIBLE included), average the channels to mono.
2. Trim: the layer starts 1 ms before the first frame that reaches ``onset_db`` (default
   -40 dB) of its peak; sustained winds, brass and strings use about -24 dB so that the
   breath or bow noise before the tone is dropped and the note speaks at once, as in
   game samples (the envelope shapes the attack).
3. Pitch (layers with a ``note``): normalised autocorrelation within +/-0.8 semitone of
   the period of the nominal note, refined by parabolic interpolation, median of three
   windows at ``pitch_ms`` (one-shots, default 60 ms) or at the loop start. The octave
   comes from the manifest, checked against the same search an octave above and below
   (``measure_pitch``). The layer is then retuned to its
   ``target_note`` (normally the sample's root note) by folding the ratio into the
   resampling step; ``shift_semitones`` is a fixed transposition for unpitched layers.
4. Resample to the chip rate: polyphase windowed sinc (Blackman window, 16 zero crossings
   per side, 1024 phases, cut-off at 0.46 of the lower of the two rates).
5. Layers are peak-normalised, scaled by ``gain_db``, delayed by ``delay_ms`` and summed;
   then an optional 2nd-order high-pass (``highpass_hz``) and DC removal.
6. Loop mode ``loop``: the loop start is searched within ``start_ms +/- search_ms`` and
   the loop length within ``min_ms .. max_ms``, both on 16-frame steps (BRR blocks), by
   maximising the normalised correlation between the frames around the loop start and
   the frames around the loop end. A loop of L frames can only play partials at
   multiples of rate / L, so on pitched samples only lengths holding a whole number of
   periods of the root within ``max_detune_cents`` (default 3 cents) are candidates;
   otherwise the sustained part plays off-key although the attack is in tune (a 1664-frame
   loop on a 24 kHz C4 holds 18.14 periods and would play 13 cents flat). With ``flatten`` the loop is given an exponential gain
   ramp so that its end has the RMS level of its start (no pumping on every repeat), then
   the last ``xfade_ms`` of the loop are cross-faded into the frames that precede the
   loop start (linear, or equal-power when the match is below 0.8) and the sample is cut
   at the loop end: ``loop_end == len`` and both loop
   points are multiples of 16.
   Loop mode ``one_shot``: cut at ``length_ms`` or where the signal stays below
   ``tail_db`` (relative to the peak), whichever is earlier, with a linear fade-out of
   ``fade_ms``; SNES one-shots are zero-padded to a multiple of 16 frames.
7. Optional compression (``compress``: feed-forward, peak envelope with attack/release,
   threshold and ratio) for the Genesis DAC: it raises the decays above the 8-bit
   quantisation floor the way game samples were mastered hot.
8. Peak normalisation to ``normalize_db``.
"""

import argparse
import hashlib
import json
import math
import operator
import os
import struct
import sys
import urllib.parse
import urllib.request
from array import array

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from samplegen import synth as S  # noqa: E402
else:
    from . import synth as S

TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT_ROOT = os.path.dirname(TOOLS_DIR)
MANIFEST_PATH = os.path.join(TOOLS_DIR, "samplegen", "cc0_manifest.json")
DEFAULT_SOURCE_DIR = os.path.join(PROJECT_ROOT, "third_party", "cc0")

BRR_BLOCK = 16
ONSET_DB = -40.0
PREROLL_S = 0.001
PITCH_WINDOW = 4096
LOOP_MAX_DETUNE_CENTS = 3.0  # loop lengths of pitched samples must keep the root within this
SNES_BRR_BYTES_PER_BLOCK = 9


# ------------------------------------------------------------------------------ manifest

def load_manifest(path=MANIFEST_PATH):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def source_path(source_dir, repo, rel):
    return os.path.join(source_dir, repo, *rel.split("/"))


def git_blob_id(path):
    """Git blob id (SHA-1 of ``blob <size>\\0`` + content) of a file."""
    with open(path, "rb") as f:
        data = f.read()
    h = hashlib.sha1()
    h.update(b"blob %d\0" % len(data))
    h.update(data)
    return h.hexdigest()


def _needed_files(manifest, chips=None):
    """``(repo, path, blob id or None)`` for every licence file and layer, in a stable order."""
    items = []
    for repo in sorted(manifest["sources"]):
        items.append((repo, manifest["sources"][repo]["licence_file"], None))
    seen = set()
    for spec in manifest["samples"]:
        if chips is not None and spec["chip"] not in chips:
            continue
        for layer in spec["layers"]:
            key = (layer["repo"], layer["path"])
            if key not in seen:
                seen.add(key)
                items.append((layer["repo"], layer["path"], layer["git_blob"]))
    return items


def missing_sources(manifest, source_dir=DEFAULT_SOURCE_DIR, chips=None):
    """Files that are absent or do not match their git blob id."""
    missing = []
    for repo, rel, blob in _needed_files(manifest, chips):
        path = source_path(source_dir, repo, rel)
        if not os.path.isfile(path) or (blob is not None and git_blob_id(path) != blob):
            missing.append((repo, rel))
    return missing


def fetch(manifest, source_dir=DEFAULT_SOURCE_DIR, chips=None, log=print):
    """Download the missing source files at the pinned commits. Returns the problems."""
    problems = []
    for repo, rel, blob in _needed_files(manifest, chips):
        path = source_path(source_dir, repo, rel)
        if os.path.isfile(path) and (blob is None or git_blob_id(path) == blob):
            continue
        src = manifest["sources"][repo]
        owner_repo = urllib.parse.urlparse(src["url"]).path.strip("/")
        url = "https://raw.githubusercontent.com/%s/%s/%s" % (
            owner_repo, src["commit"], urllib.parse.quote(rel, safe="/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        log("fetch %s/%s" % (repo, rel))
        try:
            with urllib.request.urlopen(url, timeout=120) as r:
                data = r.read()
        except OSError as exc:
            problems.append("%s/%s: download failed (%s)" % (repo, rel, exc))
            continue
        with open(path, "wb") as f:
            f.write(data)
        if blob is not None and git_blob_id(path) != blob:
            problems.append("%s/%s: git blob id mismatch after download" % (repo, rel))
    return problems


# ----------------------------------------------------------------------------- WAV read

def read_wav(path, max_seconds=None):
    """Read a RIFF/WAVE file (PCM or float, any width/channels). Returns ``(mono, rate)``."""
    with open(path, "rb") as f:
        head = f.read(12)
        if len(head) < 12 or head[0:4] != b"RIFF" or head[8:12] != b"WAVE":
            raise ValueError("%s: not a RIFF/WAVE file" % path)
        fmt = None
        while True:
            chunk = f.read(8)
            if len(chunk) < 8:
                raise ValueError("%s: no data chunk" % path)
            cid, size = chunk[0:4], struct.unpack("<I", chunk[4:8])[0]
            if cid == b"fmt ":
                body = f.read(size + (size & 1))
                tag, channels, rate = struct.unpack("<HHI", body[0:8])
                bits = struct.unpack("<H", body[14:16])[0]
                if tag == 0xFFFE and size >= 40:
                    tag = struct.unpack("<H", body[24:26])[0]
                fmt = (tag, channels, rate, bits)
            elif cid == b"data":
                if fmt is None:
                    raise ValueError("%s: data before fmt" % path)
                tag, channels, rate, bits = fmt
                width = bits // 8
                frames = size // (width * channels)
                if max_seconds is not None:
                    frames = min(frames, int(max_seconds * rate))
                raw = f.read(frames * width * channels)
                break
            else:
                f.seek(size + (size & 1), 1)
    return _decode(raw, tag, channels, width, path), rate


def _decode(raw, tag, channels, width, path):
    if tag == 3 and width == 4:
        vals = array("f")
        vals.frombytes(raw)
        scale = 1.0
    elif tag == 1 and width == 2:
        vals = array("h")
        vals.frombytes(raw)
        scale = 1.0 / 32768.0
    elif tag == 1 and width == 3:
        count = len(raw) // 3
        buf = bytearray(4 * count)
        buf[1::4] = raw[0::3]
        buf[2::4] = raw[1::3]
        buf[3::4] = raw[2::3]
        vals = array("i")
        vals.frombytes(bytes(buf))
        scale = 1.0 / 2147483648.0
    elif tag == 1 and width == 4:
        vals = array("i")
        vals.frombytes(raw)
        scale = 1.0 / 2147483648.0
    elif tag == 1 and width == 1:
        vals = array("h", (b - 128 for b in raw))
        scale = 1.0 / 128.0
    else:
        raise ValueError("%s: unsupported WAV format tag %d, %d bytes" % (path, tag, width))
    if sys.byteorder == "big" and width > 1:
        vals.byteswap()
    if channels == 1:
        return [v * scale for v in vals]
    inv = scale / channels
    chans = [vals[c::channels] for c in range(channels)]
    return [sum(t) * inv for t in zip(*chans)]


# ------------------------------------------------------------------------ DSP helpers

def midi_hz(note):
    return 440.0 * 2.0 ** ((note - 69.0) / 12.0)


def hz_midi(freq):
    return 69.0 + 12.0 * math.log2(freq / 440.0)


def onset(x, db=ONSET_DB):
    peak = max((abs(v) for v in x), default=0.0)
    if peak <= 0.0:
        return 0
    thr = peak * 10.0 ** (db / 20.0)
    for i, v in enumerate(x):
        if abs(v) >= thr:
            return i
    return 0


def _ncc(a, b):
    num = sum(map(operator.mul, a, b))
    den = math.sqrt(sum(map(operator.mul, a, a)) * sum(map(operator.mul, b, b)))
    return num / den if den > 0.0 else 0.0


def _best_lag(x, rate, note, pos):
    """``(correlation, refined lag)`` of the best lag within +/-0.8 semitone of ``note``."""
    lo = max(2, int(math.floor(rate / midi_hz(note + 0.8))))
    hi = int(math.ceil(rate / midi_hz(note - 0.8)))
    n = PITCH_WINDOW
    seg = x[pos:pos + n + hi + 2]
    if len(seg) < n + hi + 2:
        n = len(seg) - hi - 2
        if n < 256:
            raise ValueError("too short for pitch analysis")
    a = seg[:n]
    scores = {lag: _ncc(a, seg[lag:lag + n]) for lag in range(lo - 1, hi + 2)}
    lag = max(range(lo, hi + 1), key=lambda l: (scores[l], -l))
    r0, r1, r2 = scores[lag - 1], scores[lag], scores[lag + 1]
    den = r0 - 2.0 * r1 + r2
    frac = 0.5 * (r0 - r2) / den if den < 0.0 else 0.0
    return r1, lag + max(-0.5, min(0.5, frac))


def measure_pitch(x, rate, nominal, pos):
    """Measured MIDI note (float) of a layer labelled ``nominal``, and its correlation.

    Median of three analysis windows (``pos``, +1024, +2048 frames). The period is
    searched near the nominal note only: many recordings (pizzicato, organ mixtures)
    correlate almost as well at half the period, so the octave comes from the label. The
    same search an octave below and above serves as a check: a nominal correlation under
    75 % of the best one means the label names the wrong octave, which is an error.
    """
    notes = []
    corr = []
    for k in range(3):
        p = pos + 1024 * k
        r, lag = _best_lag(x, rate, nominal, p)
        others = [_best_lag(x, rate, nominal + d, p)[0] for d in (-12, 12)]
        if r < 0.75 * max(others):
            raise ValueError("pitch is an octave away from note %d" % nominal)
        notes.append(hz_midi(rate / lag))
        corr.append(r)
    return sorted(notes)[1], min(corr)


_KERNELS = {}


def _kernel_table(fc, half, phases):
    key = (round(fc, 12), half, phases)
    table = _KERNELS.get(key)
    if table is not None:
        return table
    table = []
    pi = math.pi
    for p in range(phases):
        frac = p / phases
        taps = []
        for j in range(-half + 1, half + 1):
            d = j - frac
            if abs(d) >= half:
                taps.append(0.0)
                continue
            w = 0.42 + 0.5 * math.cos(pi * d / half) + 0.08 * math.cos(2.0 * pi * d / half)
            arg = 2.0 * fc * d
            s = 1.0 if arg == 0.0 else math.sin(pi * arg) / (pi * arg)
            taps.append(s * w)
        norm = sum(taps)
        table.append([t / norm for t in taps])
    _KERNELS[key] = table
    return table


def resample(x, rate_in, rate_out, zero_crossings=16, phases=1024):
    """Polyphase windowed-sinc resampler (Blackman window), unity DC gain."""
    if abs(rate_in - rate_out) < 1e-9:
        return list(x)
    step = rate_in / rate_out
    fc = 0.46 * min(1.0, rate_out / rate_in)  # cycles per input sample
    half = int(math.ceil(zero_crossings / (2.0 * fc)))
    table = _kernel_table(fc, half, phases)
    n_out = int(math.floor((len(x) - 1) / step)) + 1
    xp = [0.0] * half + list(x) + [0.0] * (half + 1)
    out = [0.0] * n_out
    mul = operator.mul
    width = 2 * half
    for k in range(n_out):
        pos = k * step
        i0 = int(pos)
        p = int((pos - i0) * phases + 0.5)
        if p == phases:
            i0 += 1
            p = 0
        out[k] = sum(map(mul, xp[i0 + 1:i0 + 1 + width], table[p]))
    return out


def compress(x, rate, threshold_db, ratio, attack_ms, release_ms):
    """Feed-forward compressor on a peak envelope (instant-ish attack, exponential release)."""
    thr = 10.0 ** (threshold_db / 20.0)
    a_att = math.exp(-1.0 / max(1.0, attack_ms * 0.001 * rate))
    a_rel = math.exp(-1.0 / max(1.0, release_ms * 0.001 * rate))
    peak = max((abs(v) for v in x), default=0.0)
    if peak <= 0.0:
        return list(x)
    inv_peak = 1.0 / peak
    env = 0.0
    out = [0.0] * len(x)
    expo = 1.0 / ratio - 1.0
    for i, v in enumerate(x):
        level = abs(v) * inv_peak
        coef = a_att if level > env else a_rel
        env = level + coef * (env - level)
        g = (env / thr) ** expo if env > thr else 1.0
        out[i] = v * g
    return out


def _rms(seg):
    return math.sqrt(sum(v * v for v in seg) / len(seg)) if seg else 0.0


def loop_detune_cents(length, period):
    """Pitch error of a loop of ``length`` frames on a tone of ``period`` frames.

    A loop repeats every ``length`` frames, so its spectrum only has lines at multiples of
    rate / length: the fundamental plays at the nearest line, round(length / period)
    periods per loop, whatever the recording did.
    """
    periods = length / period
    return 1200.0 * math.log2(max(1, round(periods)) / periods)


def find_loop(y, rate, start_ms, search_ms, min_ms, max_ms, window, period=None, max_cents=None):
    """Best ``(loop_start, loop_length, score)`` on 16-frame steps (see module docstring).

    With ``period`` (frames per cycle of the root note) only lengths whose loop pitch is
    within ``max_cents`` of the root are candidates.
    """
    blk = BRR_BLOCK
    s_lo = max(blk * int(math.ceil(window / blk)), blk * int(round((start_ms - search_ms) * rate / 1000.0 / blk)))
    s_hi = blk * int(round((start_ms + search_ms) * rate / 1000.0 / blk))
    l_lo = max(blk, blk * int(round(min_ms * rate / 1000.0 / blk)))
    l_hi = blk * int(round(max_ms * rate / 1000.0 / blk))
    n = len(y)
    mul = operator.mul
    sq = [0.0] * (n + 1)
    for i, v in enumerate(y):
        sq[i + 1] = sq[i] + v * v
    best = None
    for s in range(s_lo, s_hi + 1, blk):
        a = y[s - window:s + window]
        ea = sq[s + window] - sq[s - window]
        if ea <= 0.0:
            continue
        for length in range(l_lo, l_hi + 1, blk):
            e = s + length
            if e + window > n:
                break
            if period is not None and abs(loop_detune_cents(length, period)) > max_cents:
                continue
            eb = sq[e + window] - sq[e - window]
            if eb <= 0.0:
                continue
            score = sum(map(mul, a, y[e - window:e + window])) / math.sqrt(ea * eb)
            if best is None or score > best[2]:
                best = (s, length, score)
    if best is None:
        raise ValueError("no loop candidate (sample too short for the requested loop)")
    return best


def make_loop(y, loop_start, loop_length, xfade, flatten, window, score):
    """Flatten, cross-fade and cut at the loop end. Returns the new frame list.

    The cross-fade is linear (constant amplitude) when the two ends match well and
    equal-power (constant energy) when ``score`` < 0.8, e.g. on string sections whose
    vibrato and chorus never repeat exactly.
    """
    e = loop_start + loop_length
    y = list(y[:e])
    if flatten:
        r0 = _rms(y[loop_start:loop_start + window])
        r1 = _rms(y[e - window:e])
        if r0 > 0.0 and r1 > 0.0:
            ln_g = math.log(r0 / r1)
            for i in range(loop_start, e):
                y[i] *= math.exp(ln_g * (i - loop_start) / loop_length)
    xfade = min(xfade, loop_start, loop_length // 2)
    src = list(y[loop_start - xfade:loop_start])
    power = score < 0.8
    for i in range(xfade):
        t = (i + 1) / (xfade + 1)
        a, b = (math.sqrt(1.0 - t), math.sqrt(t)) if power else (1.0 - t, t)
        pos = e - xfade + i
        y[pos] = y[pos] * a + src[i] * b
    return y


# ------------------------------------------------------------------------ one sample

def _read_seconds(spec):
    lp = spec["loop"]
    if lp["mode"] == "loop":
        need = (lp["start_ms"] + lp["search_ms"] + lp["max_ms"]) / 1000.0 + 0.1
    else:
        need = lp["length_ms"] / 1000.0 + 0.05
    return need * 1.35 + 2.0  # retune margin plus room for a late onset


def build_sample(spec, source_dir=DEFAULT_SOURCE_DIR):
    """Convert one manifest entry. Returns a dict (frames, rate, loop points, report)."""
    rate = spec["rate"]
    proc = spec.get("processing", {})
    lp = spec["loop"]
    report = []
    layers = []
    for layer in spec["layers"]:
        path = source_path(source_dir, layer["repo"], layer["path"])
        x, src_rate = read_wav(path, _read_seconds(spec))
        start = max(0, onset(x, float(proc.get("onset_db", ONSET_DB))) - int(PREROLL_S * src_rate))
        if layer.get("start_ms") is not None:
            start = int(layer["start_ms"] * 0.001 * src_rate)
        x = x[start:]
        shift = float(layer.get("shift_semitones", 0.0))
        if "note" in layer:
            if lp["mode"] == "loop":
                pos = int(lp["start_ms"] * 0.001 * src_rate)
            else:
                pos = int(float(layer.get("pitch_ms", 60.0)) * 0.001 * src_rate)
            try:
                measured, corr = measure_pitch(x, src_rate, layer["note"], pos)
            except ValueError as exc:
                raise ValueError("%s/%s: %s: %s" % (spec["chip"], spec["name"], layer["path"], exc))
            if abs(measured - layer["note"]) > 0.6:
                raise ValueError("%s/%s: %s measures %.2f, not note %d"
                                 % (spec["chip"], spec["name"], layer["path"], measured, layer["note"]))
            shift += layer["target_note"] - measured
            report.append("%s: measured %.2f (r %.2f), retuned %+.1f cents"
                          % (layer["path"].rsplit("/", 1)[-1], measured, corr,
                             (layer["target_note"] - measured) * 100.0))
        y = resample(x, src_rate * 2.0 ** (shift / 12.0), rate)
        peak = max((abs(v) for v in y), default=0.0)
        g = 10.0 ** (float(layer.get("gain_db", 0.0)) / 20.0) / peak if peak > 0.0 else 0.0
        delay = int(round(float(layer.get("delay_ms", 0.0)) * 0.001 * rate))
        layers.append(([v * g for v in y], delay))

    n = max(len(y) + d for y, d in layers)
    mix = [0.0] * n
    for y, d in layers:
        for i, v in enumerate(y):
            mix[i + d] += v
    if proc.get("highpass_hz"):
        mix = S.biquad(mix, rate, "hp", float(proc["highpass_hz"]))
    mix = S.remove_dc(mix)
    preroll = max(1, int(PREROLL_S * rate))
    for i in range(min(preroll, len(mix))):
        mix[i] *= (i + 1) / (preroll + 1)

    loop_start = loop_end = None
    if lp["mode"] == "loop":
        window = BRR_BLOCK * 8
        period = None
        if spec["layers"] and "target_note" in spec["layers"][0]:
            period = rate / midi_hz(spec["root_note"])
            window = max(64, min(256, int(round(2.0 * period))))
        max_cents = float(lp.get("max_detune_cents", LOOP_MAX_DETUNE_CENTS))
        s, length, score = find_loop(mix, rate, lp["start_ms"], lp["search_ms"], lp["min_ms"],
                                     lp["max_ms"], window, period, max_cents)
        xfade = int(round(lp["xfade_ms"] * 0.001 * rate))
        mix = make_loop(mix, s, length, xfade, lp.get("flatten", True), window, score)
        loop_start, loop_end = s, s + length
        report.append("loop %d..%d (%.1f ms), match %.3f%s"
                      % (s, s + length, length * 1000.0 / rate, score,
                         ", loop pitch %+.1f cents" % loop_detune_cents(length, period) if period else ""))
    else:
        limit = min(len(mix), int(round(lp["length_ms"] * 0.001 * rate)))
        peak = max((abs(v) for v in mix), default=0.0)
        thr = peak * 10.0 ** (float(lp.get("tail_db", -48.0)) / 20.0)
        last = 0
        for i in range(limit):
            if abs(mix[i]) > thr:
                last = i
        fade_n = int(round(float(lp.get("fade_ms", 20.0)) * 0.001 * rate))
        end = min(limit, last + 1 + fade_n // 2)
        mix = mix[:max(end, BRR_BLOCK)]
        fade_n = max(1, min(fade_n, len(mix) // 2))
        m = len(mix)
        for i in range(fade_n):
            mix[m - 1 - i] *= i / fade_n
        if spec["chip"] == "snes":
            mix = S.pad_to_multiple(mix, BRR_BLOCK)

    if proc.get("compress"):
        c = proc["compress"]
        mix = compress(mix, rate, c["threshold_db"], c["ratio"], c["attack_ms"], c["release_ms"])
    mix = S.normalize(mix, float(proc.get("normalize_db", -1.0)))
    if loop_end is not None and (loop_start % BRR_BLOCK or loop_end % BRR_BLOCK or loop_end != len(mix)):
        raise ValueError("%s/%s: loop not block aligned" % (spec["chip"], spec["name"]))
    return {"frames": mix, "rate": rate, "loop_start": loop_start, "loop_end": loop_end, "report": report}


def source_text(spec):
    return " + ".join("%s:%s" % (l["repo"], l["path"]) for l in spec["layers"])


def licence_text(manifest, spec):
    return " / ".join(sorted({manifest["sources"][l["repo"]]["licence"] for l in spec["layers"]}))


def brr_bytes(frames):
    return (frames + BRR_BLOCK - 1) // BRR_BLOCK * SNES_BRR_BYTES_PER_BLOCK


# ------------------------------------------------------------------------------- CLI

def main(argv=None):
    parser = argparse.ArgumentParser(description="Fetch and analyse the CC0 sample sources.")
    parser.add_argument("--fetch", action="store_true", help="download missing source files")
    parser.add_argument("--analyse", nargs="*", metavar="CHIP/NAME",
                        help="build the given samples (all when empty) and print the report")
    parser.add_argument("--source-dir", default=DEFAULT_SOURCE_DIR)
    args = parser.parse_args(argv)
    manifest = load_manifest()
    status = 0
    if args.fetch:
        for p in fetch(manifest, args.source_dir):
            print("error: " + p)
            status = 1
    if args.analyse is not None:
        wanted = set(args.analyse)
        for spec in manifest["samples"]:
            key = "%s/%s" % (spec["chip"], spec["name"])
            if wanted and key not in wanted:
                continue
            try:
                r = build_sample(spec, args.source_dir)
            except (OSError, ValueError) as exc:
                print("%s: ERROR %s" % (key, exc))
                status = 1
                continue
            n = len(r["frames"])
            print("%s: %d frames at %d Hz (%.0f ms), BRR %d bytes" % (
                key, n, r["rate"], n * 1000.0 / r["rate"], brr_bytes(n)))
            for line in r["report"]:
                print("    " + line)
    return status


if __name__ == "__main__":
    sys.exit(main())
