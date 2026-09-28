"""16-bit PCM mono WAV writer and a tolerant PCM WAV reader built on the ``wave`` module.

``write_wav`` always produces a canonical 44-byte-header RIFF/WAVE file, mono, 16-bit
little-endian PCM, so the same float data yields byte-identical files on every run.

``read_wav`` accepts 8/16/24/32-bit integer PCM with any channel count (channels are
averaged to mono) and returns floats in [-1, 1].
"""

import struct
import sys
import wave
from array import array


def float_to_pcm16(samples):
    """Convert an iterable of floats in [-1, 1] to a little-endian 16-bit byte string."""
    out = array("h")
    append = out.append
    for s in samples:
        v = int(round(s * 32767.0))
        if v > 32767:
            v = 32767
        elif v < -32768:
            v = -32768
        append(v)
    if sys.byteorder == "big":
        out.byteswap()
    return out.tobytes()


def write_wav(path, samples, sample_rate):
    """Write ``samples`` (floats) as a mono 16-bit PCM WAV at ``sample_rate`` Hz."""
    data = float_to_pcm16(samples)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(int(sample_rate))
        w.writeframes(data)


def read_wav(path):
    """Read a PCM WAV file. Returns ``(samples, sample_rate)`` with mono float samples."""
    with wave.open(str(path), "rb") as r:
        channels = r.getnchannels()
        width = r.getsampwidth()
        rate = r.getframerate()
        frames = r.getnframes()
        raw = r.readframes(frames)
    if width == 1:
        ints = [b - 128 for b in raw]
        scale = 1.0 / 128.0
    elif width == 2:
        ints = array("h")
        ints.frombytes(raw)
        if sys.byteorder == "big":
            ints.byteswap()
        scale = 1.0 / 32768.0
    elif width == 3:
        count = len(raw) // 3
        ints = [0] * count
        for i in range(count):
            b = raw[3 * i: 3 * i + 3]
            v = b[0] | (b[1] << 8) | (b[2] << 16)
            if v & 0x800000:
                v -= 0x1000000
            ints[i] = v
        scale = 1.0 / 8388608.0
    elif width == 4:
        count = len(raw) // 4
        ints = list(struct.unpack("<%di" % count, raw))
        scale = 1.0 / 2147483648.0
    else:
        raise ValueError("unsupported sample width %d in %s" % (width, path))
    if channels == 1:
        mono = [v * scale for v in ints]
    else:
        inv = scale / channels
        total = len(ints) // channels
        mono = [0.0] * total
        for i in range(total):
            base = i * channels
            acc = 0
            for c in range(channels):
                acc += ints[base + c]
            mono[i] = acc * inv
    return mono, rate


def wav_info(path):
    """Return ``(channels, sample_width_bytes, sample_rate, frames)`` for a WAV file."""
    with wave.open(str(path), "rb") as r:
        return r.getnchannels(), r.getsampwidth(), r.getframerate(), r.getnframes()
