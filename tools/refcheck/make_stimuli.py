"""Stimuli for the differential fidelity check (reference emulator vs our cores).

Usage::

    python tools\\refcheck\\make_stimuli.py [--out build-reports\\refcheck\\stimuli]

Writes, with our own register writes only (no game data):

* ``genesis/<name>.vgm``: VGM 1.51 files (public format, https://vgmrips.net/wiki/VGM_Specification)
  for an NTSC Mega Drive: YM2612 at 7670453 Hz, SN76489 at 3579545 Hz (Sega LFSR:
  feedback 0x0009, 16-bit shift register). Every file starts with the same init block
  (LFO/DAC off, all channels keyed off with TL 127 and RR 15, L+R enabled, PSG silent),
  then 50 ms of silence before the stimulus proper.
* ``snes/<name>.spc``: SPC files (v0.30 layout) holding our own BRR samples, a DSP register
  snapshot with the static set-up (KON = KOFF = 0) and a small SPC700 program that
  replays an event table of timed DSP writes (see ``SPC700 program`` below), plus
  ``snes/<name>.events``: the same DSP writes as ``sample register value`` lines at the
  32 kHz sample index at which the program performs them. chiptool reads the SPC's RAM
  image and DSP snapshot plus this list, because it has no SPC700 CPU.
* ``manifest.json``: one entry per stimulus with the analysis windows used by compare.py.

Everything is deterministic (fixed seeds) and standard-library Python.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import struct

# ----- common ---------------------------------------------------------------------------------

VGM_RATE = 44100
MASTER_NTSC = 53693175
YM_CLOCK = 7670453          # MASTER / 7, truncated (the VGM header holds an integer)
PSG_CLOCK = 3579545         # MASTER / 15
FM_RATE = MASTER_NTSC / 7 / 144

SPC_RATE = 32000
SPC_SAMPLES_PER_TICK = 4    # timer 0 at 8 kHz with target 1: one tick = 4 DSP samples


def fm_freq(block: int, fnum: int) -> float:
    """Expected YM2612 frequency (research "Frequency"): fnum * 2^block / 2^21 * FM rate."""
    return fnum * (1 << block) / 2.0 / 1048576.0 * FM_RATE


# ----- VGM writer -----------------------------------------------------------------------------

class Vgm:
    def __init__(self) -> None:
        self.data = bytearray()
        self.samples = 0

    def ym(self, bank: int, reg: int, value: int) -> None:
        self.data += bytes((0x52 + (bank & 1), reg & 0xFF, value & 0xFF))

    def psg(self, value: int) -> None:
        self.data += bytes((0x50, value & 0xFF))

    def wait(self, n: int) -> None:
        n = int(n)
        self.samples += n
        while n > 0:
            step = min(n, 65535)
            self.data += bytes((0x61, step & 0xFF, step >> 8))
            n -= step

    def wait_until(self, seconds: float) -> None:
        target = int(round(seconds * VGM_RATE))
        if target > self.samples:
            self.wait(target - self.samples)

    def build(self) -> bytes:
        body = bytes(self.data) + b"\x66"
        header = bytearray(0x80)
        header[0:4] = b"Vgm "
        struct.pack_into("<I", header, 0x08, 0x151)
        struct.pack_into("<I", header, 0x0C, PSG_CLOCK)
        struct.pack_into("<I", header, 0x18, self.samples)
        struct.pack_into("<H", header, 0x28, 0x0009)   # SN76489 feedback (Sega)
        header[0x2A] = 16                              # shift register width
        header[0x2B] = 0x00                            # SN76489 flags
        struct.pack_into("<I", header, 0x2C, YM_CLOCK)
        struct.pack_into("<I", header, 0x34, 0x80 - 0x34)
        out = bytes(header) + body
        struct.pack_into("<I", header, 0x04, len(out) - 4)
        return bytes(header) + body


# Operator register offsets for Yamaha slots S1..S4 (address order is S1, S3, S2, S4).
SLOT_OFFSET = (0, 8, 4, 12)
KEY_CODE = (0, 1, 2, 4, 5, 6)


def op_default(**kw) -> dict:
    op = dict(dt=0, mul=1, tl=127, rs=0, ar=31, am=0, dr=0, sr=0, sl=0, rr=15, ssg=0)
    op.update(kw)
    return op


def fm_patch(v: Vgm, ch: int, alg: int, fb: int, ops: list[dict], pan: int = 0xC0, ams: int = 0, fms: int = 0) -> None:
    bank, c = divmod(ch, 3)
    for s, op in enumerate(ops):
        base = c + SLOT_OFFSET[s]
        v.ym(bank, 0x30 + base, (op["dt"] << 4) | op["mul"])
        v.ym(bank, 0x40 + base, op["tl"])
        v.ym(bank, 0x50 + base, (op["rs"] << 6) | op["ar"])
        v.ym(bank, 0x60 + base, (op["am"] << 7) | op["dr"])
        v.ym(bank, 0x70 + base, op["sr"])
        v.ym(bank, 0x80 + base, (op["sl"] << 4) | op["rr"])
        v.ym(bank, 0x90 + base, op["ssg"])
    v.ym(bank, 0xB0 + c, (fb << 3) | alg)
    v.ym(bank, 0xB4 + c, pan | (ams << 4) | fms)


def fm_freq_write(v: Vgm, ch: int, block: int, fnum: int) -> None:
    bank, c = divmod(ch, 3)
    v.ym(bank, 0xA4 + c, (block << 3) | (fnum >> 8))
    v.ym(bank, 0xA0 + c, fnum & 0xFF)


def fm_key(v: Vgm, ch: int, slots: int) -> None:
    v.ym(0, 0x28, (slots << 4) | KEY_CODE[ch])


def genesis_init(v: Vgm) -> None:
    v.ym(0, 0x22, 0x00)
    v.ym(0, 0x27, 0x00)
    v.ym(0, 0x2B, 0x00)
    silent = [op_default() for _ in range(4)]
    for ch in range(6):
        fm_key(v, ch, 0)
        fm_patch(v, ch, 0, 0, silent)
        fm_freq_write(v, ch, 0, 0)
    for b in (0x9F, 0xBF, 0xDF, 0xFF):
        v.psg(b)


def psg_tone(v: Vgm, ch: int, period: int, att: int) -> None:
    v.psg(0x80 | (ch << 5) | (period & 0x0F))
    v.psg((period >> 4) & 0x3F)
    v.psg(0x90 | (ch << 5) | (att & 0x0F))


# ----- Genesis stimuli ------------------------------------------------------------------------

T_ON = 0.05


def genesis_stimuli() -> list[tuple[dict, bytes]]:
    out: list[tuple[dict, bytes]] = []

    def add(name: str, v: Vgm, end: float, meta: dict) -> None:
        v.wait_until(end)
        m = dict(name=name, chip="genesis", file=f"genesis/{name}.vgm", end=end)
        m.update(meta)
        out.append((m, v.build()))

    def fm_single(name: str, ops: list[dict], alg: int = 7, fb: int = 0, block: int = 4, fnum: int = 1083,
                  off: float = 0.85, end: float = 1.0, kind: str = "tone", desc: str = "", pan: int = 0xC0,
                  ams: int = 0, fms: int = 0, lfo: int | None = None, slots: int = 0xF, ch: int = 0,
                  steady: tuple[float, float] | None = None, f0_mult: float = 1.0) -> None:
        v = Vgm()
        genesis_init(v)
        if lfo is not None:
            v.ym(0, 0x22, 0x08 | lfo)
        fm_patch(v, ch, alg, fb, ops, pan=pan, ams=ams, fms=fms)
        fm_freq_write(v, ch, block, fnum)
        v.wait_until(T_ON)
        fm_key(v, ch, slots)
        v.wait_until(off)
        fm_key(v, ch, 0)
        meta = dict(unit="ym2612", kind=kind, f0=fm_freq(block, fnum) * f0_mult, on=T_ON, off=off, desc=desc)
        if steady:
            meta["steady"] = list(steady)
        add(name, v, end, meta)

    carrier = lambda **kw: [op_default(), op_default(), op_default(), op_default(tl=0, **kw)]

    # Level reference (gain calibration of the YM2612 path) and TL steps.
    fm_single("fm_sine_ref", carrier(), desc="alg 7, S4 alone, TL 0, MUL 1, A4 (block 4, fnum 1083)")
    for tl in (4, 8, 16, 32, 48, 64, 96, 112):
        ops = carrier()
        ops[3]["tl"] = tl
        fm_single(f"fm_tl_{tl:03d}", ops, off=0.55, end=0.6, desc=f"S4 alone, TL {tl} ({0.75 * tl:.2f} dB below ref)")
    # Multiplier and detune (pitch).
    for mul in (0, 2, 3, 5, 7, 11, 15):
        fm_single(f"fm_mul_{mul:02d}", carrier(mul=mul), block=3, off=0.55, end=0.6, f0_mult=mul if mul else 0.5,
                  desc=f"S4 alone, MUL {mul}, block 3")
    for block in (2, 6):
        for dt in (1, 2, 3, 5, 6, 7):
            fm_single(f"fm_dt{dt}_b{block}", carrier(dt=dt), block=block, off=0.55, end=0.6,
                      desc=f"S4 alone, DT {dt}, block {block}, fnum 1083")
    # Algorithms with one fixed patch. Static envelopes (AR 31, DR = SR = 0): the timbre does
    # not depend on the unknown power-on phase of the global EG counter, so these isolate the
    # modulation routing and the operator pipeline. The _eg variants add slow decays.
    patch = [op_default(tl=24, mul=1), op_default(tl=30, mul=2), op_default(tl=28, mul=3), op_default(tl=4, mul=1)]
    for alg in range(8):
        fm_single(f"fm_alg{alg}", [dict(o) for o in patch], alg=alg, fb=0, block=3,
                  desc=f"algorithm {alg}, fixed static 4-op patch (TL 24/30/28/4, MUL 1/2/3/1), A3")
    patch_eg = [op_default(tl=24, mul=1, dr=4, sl=2, sr=1, rr=8), op_default(tl=30, mul=2, dr=6, sl=3, sr=1, rr=8),
                op_default(tl=28, mul=3, dr=5, sl=2, sr=1, rr=8), op_default(tl=4, mul=1, dr=3, sl=2, sr=1, rr=8)]
    for alg in (0, 4):
        fm_single(f"fm_alg{alg}_eg", [dict(o) for o in patch_eg], alg=alg, fb=0, block=3,
                  desc=f"algorithm {alg}, same patch with slow decays (DR 4/6/5/3, SR 1)")
    # Feedback on S1 alone.
    for fb in range(8):
        ops = [op_default(tl=0), op_default(), op_default(), op_default()]
        fm_single(f"fm_fb{fb}", ops, alg=7, fb=fb, block=3, off=0.55, end=0.6, desc=f"S1 alone, feedback {fb}, A3")
    # Envelope rates (S4 alone): attack, decay to SL, sustain rate, release, rate scaling.
    for ar in (8, 12, 16, 20, 24):
        fm_single(f"fm_ar{ar:02d}", carrier(ar=ar), off=1.25, end=1.3, kind="env", desc=f"attack rate {ar}")
    for dr in (6, 10, 14, 18):
        fm_single(f"fm_dr{dr:02d}", carrier(dr=dr, sl=8), off=1.25, end=1.3, kind="env", desc=f"decay rate {dr} to SL 8 (-24 dB)")
    for sr in (6, 10, 14):
        fm_single(f"fm_sr{sr:02d}", carrier(sr=sr, sl=0), off=1.25, end=1.3, kind="env", desc=f"SL 0, sustain rate {sr}")
    for rr in (3, 5, 7, 9):
        fm_single(f"fm_rr{rr:02d}", carrier(rr=rr), off=0.3, end=1.3, kind="env", desc=f"key-off at 0.3 s, release rate {rr}")
    for rs in (0, 3):
        fm_single(f"fm_rs{rs}_ar14_b6", carrier(ar=14, rs=rs), block=6, off=1.0, end=1.05, kind="env",
                  desc=f"attack rate 14, rate scaling {rs}, block 6")
    # SSG-EG modes 8..15 on the carrier.
    for mode in range(8, 16):
        fm_single(f"fm_ssg{mode:x}", carrier(ssg=mode, dr=20, sr=20, sl=15), off=0.85, end=1.0, kind="ssg",
                  desc=f"SSG-EG {mode:X}, DR = SR = 20, SL 15")
    # LFO: AM on S4 with AMS 1..3, PM with FMS 2/5/7 (LFO frequency 3 = 6.2 Hz).
    for ams in (1, 2, 3):
        fm_single(f"fm_lfo_ams{ams}", carrier(am=1), lfo=3, ams=ams, off=1.45, end=1.5, kind="lfo_am",
                  desc=f"LFO 6.2 Hz, S4 AM on, AMS {ams}")
    for fms in (2, 5, 7):
        fm_single(f"fm_lfo_fms{fms}", carrier(), lfo=3, fms=fms, off=1.45, end=1.5, kind="lfo_pm",
                  desc=f"LFO 6.2 Hz, FMS {fms}")
    # Pan and low levels (ladder effect region).
    fm_single("fm_pan_left", carrier(), pan=0x80, off=0.55, end=0.6, desc="S4 alone, L only")
    fm_single("fm_pan_right", carrier(), pan=0x40, off=0.55, end=0.6, desc="S4 alone, R only")
    ops = carrier()
    ops[3]["tl"] = 100
    fm_single("fm_ladder_tl100", ops, off=0.55, end=0.6, desc="S4 alone, TL 100 (-75 dB): ladder-effect dominated")
    fm_single("fm_ch6_b1", carrier(), ch=5, off=0.55, end=0.6, desc="channel 6 (bank 1), S4 alone")

    # DAC: 8-bit saw streamed every 5 VGM samples (8820 Hz), 40 writes per period.
    v = Vgm()
    genesis_init(v)
    v.ym(1, 0xB6, 0xC0)
    v.ym(0, 0x2A, 0x80)
    v.ym(0, 0x2B, 0x80)
    v.wait_until(T_ON)
    k = 0
    while v.samples < int(0.85 * VGM_RATE):
        v.ym(0, 0x2A, 0x40 + (k % 40) * 128 // 40)
        v.wait(5)
        k += 1
    v.ym(0, 0x2A, 0x80)
    add("fm_dac_saw", v, 1.0, dict(unit="ym2612", kind="tone", f0=VGM_RATE / 5 / 40, on=T_ON, off=0.85,
                                    desc="DAC saw 0x40..0xBC, 40 steps at 8820 Hz"))

    # PSG tones, attenuation, other channels.
    def psg_single(name: str, ch: int, period: int, att: int, desc: str, off: float = 0.55, end: float = 0.6) -> None:
        v = Vgm()
        genesis_init(v)
        v.wait_until(T_ON)
        psg_tone(v, ch, period, att)
        v.wait_until(off)
        v.psg(0x9F | (ch << 5))
        add(name, v, end, dict(unit="sn76489", kind="tone", f0=PSG_CLOCK / (32.0 * period), on=T_ON, off=off, desc=desc))

    psg_single("psg_tone_ref", 0, 254, 0, "tone 1, period 254 (440.4 Hz), attenuation 0", off=0.85, end=1.0)
    for period in (30, 127, 508, 1016):
        psg_single(f"psg_tone_p{period:04d}", 0, period, 0, f"tone 1, period {period}")
    for att in (1, 2, 4, 8, 12, 14):
        psg_single(f"psg_att{att:02d}", 0, 254, att, f"tone 1, period 254, attenuation {att} ({2 * att} dB)")
    psg_single("psg_ch2", 1, 254, 0, "tone 2, period 254")
    psg_single("psg_ch3", 2, 254, 0, "tone 3, period 254")

    for white in (0, 1):
        for rate in range(4):
            v = Vgm()
            genesis_init(v)
            if rate == 3:
                v.psg(0xC0 | (64 & 15))   # tone 3 period 64 (silent: attenuation stays 15)
                v.psg(64 >> 4)
            v.wait_until(T_ON)
            v.psg(0xE0 | (white << 2) | rate)
            v.psg(0xF0)
            v.wait_until(0.55)
            v.psg(0xFF)
            mode = "white" if white else "periodic"
            name = f"psg_noise_{mode[0]}{rate}"
            reload = (16, 32, 64, 64)[rate]
            f0 = PSG_CLOCK / 16.0 / (2 * reload) / (16 if not white else 1)
            add(name, v, 0.6, dict(unit="sn76489", kind="tone" if not white else "noise", f0=f0, on=T_ON, off=0.55,
                                   desc=f"noise {mode}, rate {rate}" + (" (tone 3 period 64)" if rate == 3 else "")))
    return out


# ----- BRR ------------------------------------------------------------------------------------

def _clip15(x: int) -> int:
    x = max(-32768, min(32767, x))
    return ((x & 0x7FFF) ^ 0x4000) - 0x4000


def _brr_predict(filt: int, old: int, older: int) -> int:
    if filt == 0:
        return 0
    if filt == 1:
        return old + ((-old) >> 4)
    if filt == 2:
        return (old << 1) + ((-((old << 1) + old)) >> 5) - older + (older >> 4)
    return (old << 1) + ((-(old + (old << 2) + (old << 3))) >> 6) - older + (((older << 1) + older) >> 4)


def brr_encode(samples: list[int], loop: bool) -> bytes:
    """Greedy BRR encoder (research "BRR decoding" formulas). samples: 15-bit targets,
    length a multiple of 16. The first block uses filter 0 (loop target)."""
    out = bytearray()
    old = older = 0
    nblocks = len(samples) // 16
    for b in range(nblocks):
        block = samples[b * 16:(b + 1) * 16]
        best = None
        for filt in ((0,) if b == 0 else (0, 1, 2, 3)):
            for shift in range(13):
                o, oo, err, nibs = old, older, 0, []
                for s in block:
                    pred = _brr_predict(filt, o, oo)
                    best_n, best_v = 0, None
                    for n in range(-8, 8):
                        val = _clip15(((n << shift) >> 1) + pred)
                        if best_v is None or abs(val - s) < abs(best_v - s):
                            best_n, best_v = n, val
                    nibs.append(best_n)
                    err += (best_v - s) ** 2
                    oo, o = o, best_v
                if best is None or err < best[0]:
                    best = (err, filt, shift, nibs, o, oo)
        _, filt, shift, nibs, old, older = best
        header = (shift << 4) | (filt << 2)
        if b == nblocks - 1:
            header |= 0x03 if loop else 0x01
        out.append(header)
        for i in range(0, 16, 2):
            out.append(((nibs[i] & 15) << 4) | (nibs[i + 1] & 15))
    return bytes(out)


def sine_table(period: int, cycles: int, amp: int) -> list[int]:
    n = period * cycles
    return [int(round(amp * math.sin(2 * math.pi * i / period))) for i in range(n)]


def saw_table(period: int, cycles: int, amp: int) -> list[int]:
    return [int(round(amp * (2.0 * ((i % period) / period) - 1.0))) for i in range(period * cycles)]


def noise_table(n: int, amp: int, seed: int) -> list[int]:
    rng = random.Random(seed)
    return [int(round(amp * (2 * rng.random() - 1))) for _ in range(n)]


# ----- SPC700 program -------------------------------------------------------------------------
# Event table at TABLE: entries (delay_lo, delay_hi, reg, value); reg 0xFF ends the table.
# The delay is in timer-0 ticks (125 us = 4 DSP samples) since the previous event.
# Opcodes (SPC700 instruction set, fullsnes "SNES APU SPC700 CPU"): 8F MOV dp,#imm;
# 8D MOV Y,#imm; F7 MOV A,[dp]+Y; C4 MOV dp,A; 3A INCW dp; 68 CMP A,#imm; F0 BEQ;
# E4 MOV A,dp; 04 OR A,dp; 1A DECW dp; 2F BRA.

PROG = 0x0200
TABLE = 0x0400
DIR_PAGE = 0x30
SAMPLES_AT = 0x3100
ESA_PAGE = 0x80


def spc_program() -> bytes:
    code = bytearray()
    labels: dict[str, int] = {}
    fixups: list[tuple[int, str]] = []

    def emit(*b: int) -> None:
        code.extend(b)

    def label(name: str) -> None:
        labels[name] = PROG + len(code)

    def branch(op: int, target: str) -> None:
        code.append(op)
        fixups.append((len(code), target))
        code.append(0)

    emit(0x8F, 0x00, 0xF1)                 # mov $F1,#$00   timers off, IPL ROM off
    emit(0x8F, 0x01, 0xFA)                 # mov $FA,#$01   timer 0 target 1 (8 kHz)
    emit(0x8F, 0x01, 0xF1)                 # mov $F1,#$01   start timer 0
    emit(0x8F, TABLE & 0xFF, 0x00)         # mov $00,#<TABLE
    emit(0x8F, TABLE >> 8, 0x01)           # mov $01,#>TABLE
    emit(0x8D, 0x00)                       # mov y,#0
    label("next")
    emit(0xF7, 0x00, 0xC4, 0x02, 0x3A, 0x00)   # delay lo -> $02
    emit(0xF7, 0x00, 0xC4, 0x03, 0x3A, 0x00)   # delay hi -> $03
    emit(0xF7, 0x00, 0x68, 0xFF)               # reg; cmp #$FF
    branch(0xF0, "halt")
    emit(0xC4, 0x04, 0x3A, 0x00)               # reg -> $04
    emit(0xF7, 0x00, 0xC4, 0x05, 0x3A, 0x00)   # value -> $05
    label("wait")
    emit(0xE4, 0x02, 0x04, 0x03)               # a = $02 | $03
    branch(0xF0, "write")
    label("tick")
    emit(0xE4, 0xFD)                           # a = T0OUT (read clears)
    branch(0xF0, "tick")
    emit(0x1A, 0x02)                           # decw $02
    branch(0x2F, "wait")
    label("write")
    emit(0xE4, 0x04, 0xC4, 0xF2, 0xE4, 0x05, 0xC4, 0xF3)   # DSPADDR = reg, DSPDATA = value
    branch(0x2F, "next")
    label("halt")
    branch(0x2F, "halt")
    for pos, target in fixups:
        rel = labels[target] - (PROG + pos + 1)
        assert -128 <= rel <= 127
        code[pos] = rel & 0xFF
    return bytes(code)


def spc_file(ram: bytearray, dsp: list[int], seconds: int, title: str) -> bytes:
    f = bytearray(0x10200)
    f[0:33] = b"SNES-SPC700 Sound File Data v0.30"
    f[0x21] = 26
    f[0x22] = 26
    f[0x23] = 26          # ID666 tag present
    f[0x24] = 30
    struct.pack_into("<H", f, 0x25, PROG)
    f[0x27] = f[0x28] = f[0x29] = 0
    f[0x2A] = 0x00        # PSW
    f[0x2B] = 0xEF        # SP
    t = title.encode("ascii")[:32]
    f[0x2E:0x2E + len(t)] = t
    f[0x4E:0x4E + 16] = b"retro-chip-vst  "
    f[0xA9:0xAC] = f"{seconds:03d}".encode()
    f[0xAC:0xB1] = b"00000"
    f[0x100:0x10100] = ram
    f[0x10100:0x10180] = bytes(dsp)
    f[0x101C0:0x10200] = ram[0xFFC0:0x10000]
    return bytes(f)


class SpcStim:
    """DSP snapshot + timed events for one SNES stimulus."""

    def __init__(self) -> None:
        self.dsp = [0] * 128
        self.events: list[tuple[int, int, int]] = []   # (tick, reg, value)
        self.samples: list[bytes] = []
        self.dsp[0x6C] = 0x20        # FLG: echo writes off, no mute, no reset
        self.dsp[0x0C] = self.dsp[0x1C] = 0x7F   # MVOL
        self.dsp[0x5D] = DIR_PAGE
        self.dsp[0x6D] = ESA_PAGE
        self.dsp[0x0F] = 0x7F        # FIR0 = 0x7F, FIR1..7 = 0 (only matters with echo)

    def add_sample(self, data: bytes) -> int:
        self.samples.append(data)
        return len(self.samples) - 1

    def voice(self, v: int, srcn: int, pitch: int, vol_l: int = 0x40, vol_r: int = 0x40,
              adsr1: int = 0x00, adsr2: int = 0x00, gain: int = 0x7F) -> None:
        b = v * 16
        self.dsp[b + 0] = vol_l & 0xFF
        self.dsp[b + 1] = vol_r & 0xFF
        self.dsp[b + 2] = pitch & 0xFF
        self.dsp[b + 3] = (pitch >> 8) & 0x3F
        self.dsp[b + 4] = srcn
        self.dsp[b + 5] = adsr1
        self.dsp[b + 6] = adsr2
        self.dsp[b + 7] = gain

    def at(self, seconds: float, reg: int, value: int) -> None:
        self.events.append((int(round(seconds * SPC_RATE / SPC_SAMPLES_PER_TICK)), reg, value))

    def build(self, seconds_tag: int, title: str) -> tuple[bytes, str]:
        ram = bytearray(0x10000)
        ram[0xF0] = 0x0A
        prog = spc_program()
        ram[PROG:PROG + len(prog)] = prog
        # Sample directory and data.
        addr = SAMPLES_AT
        for i, data in enumerate(self.samples):
            struct.pack_into("<HH", ram, DIR_PAGE * 0x100 + 4 * i, addr, addr)
            ram[addr:addr + len(data)] = data
            addr += len(data)
        assert addr < ESA_PAGE * 0x100
        # Event table (relative delays) and the chiptool event list (absolute samples).
        events = sorted(self.events, key=lambda e: e[0])
        table = bytearray()
        lines = []
        last = 0
        for tick, reg, value in events:
            delay = tick - last
            last = tick
            table += struct.pack("<HBB", delay, reg, value)
            lines.append(f"{tick * SPC_SAMPLES_PER_TICK} {reg} {value}")
        table += struct.pack("<HBB", 0, 0xFF, 0)
        ram[TABLE:TABLE + len(table)] = table
        assert TABLE + len(table) < DIR_PAGE * 0x100
        return spc_file(ram, self.dsp, seconds_tag, title), "\n".join(lines) + "\n"


KON, KOFF, FLG, NON, PMON, EON, EFB, EDL = 0x4C, 0x5C, 0x6C, 0x3D, 0x2D, 0x4D, 0x0D, 0x7D
S_ON = 0.02


def snes_stimuli() -> list[tuple[dict, bytes, str]]:
    out: list[tuple[dict, bytes, str]] = []
    sine = brr_encode(sine_table(32, 4, 0x3000), loop=True)        # 1000 Hz at pitch 0x1000
    saw = brr_encode(saw_table(32, 4, 0x2C00), loop=True)
    bnoise = brr_encode(noise_table(1024, 0x2800, 1234), loop=True)

    def add(name: str, s: SpcStim, end: float, meta: dict) -> None:
        spc, events = s.build(int(math.ceil(end)) + 1, name)
        m = dict(name=name, chip="snes", unit="sdsp", file=f"snes/{name}.spc", events=f"snes/{name}.events", end=end)
        m.update(meta)
        out.append((m, spc, events))

    def single(name: str, sample: bytes, pitch: int, f_period: float | None, kind: str = "tone", off: float = 0.6,
               end: float = 0.7, desc: str = "", mvol: int = 0x7F, **voice) -> SpcStim:
        s = SpcStim()
        s.dsp[0x0C] = s.dsp[0x1C] = mvol
        src = s.add_sample(sample)
        s.voice(0, src, pitch, **voice)
        s.at(0.0, KOFF, 0x00)
        s.at(0.0, FLG, s.dsp[FLG])
        s.at(S_ON, KON, 0x01)
        s.at(off, KOFF, 0x01)
        f0 = SPC_RATE * pitch / 4096.0 / f_period if f_period else 0.0
        add(name, s, end, dict(kind=kind, f0=f0, on=S_ON, off=off, desc=desc))
        return s

    single("snes_sine_ref", sine, 0x1000, 32, desc="looped 32-sample BRR sine, pitch 0x1000 (1000 Hz), GAIN direct 0x7F")
    for pitch in (0x0400, 0x0800, 0x0C00, 0x1800, 0x2000, 0x3000, 0x3FFF, 0x0123):
        single(f"snes_pitch_{pitch:04x}", sine, pitch, 32, desc=f"sine, pitch 0x{pitch:04X}")
    single("snes_saw", saw, 0x0800, 32, desc="looped 32-sample BRR saw, pitch 0x0800 (500 Hz)")
    single("snes_saw_hi", saw, 0x2000, 32, desc="saw, pitch 0x2000 (2000 Hz)")
    single("snes_brr_noise", bnoise, 0x1000, None, kind="noise", desc="1024-sample random BRR, pitch 0x1000")
    for vol in (0x10, 0x20, 0x7F, 0xC0):
        single(f"snes_vol_{vol:02x}", sine, 0x1000, 32, vol_l=vol, vol_r=0x40, mvol=0x50,
               desc=f"sine, VOLL 0x{vol:02X}, VOLR 0x40")
    # ADSR: attack rates, decay/sustain, sustain rate, release.
    for ar in (4, 8, 12, 15):
        single(f"snes_adsr_ar{ar:x}", sine, 0x1000, 32, kind="env", off=0.9, end=1.0,
               adsr1=0x80 | ar, adsr2=0xE0, desc=f"ADSR AR {ar}, SL 7, SR 0")
    for dr in (2, 5, 7):
        single(f"snes_adsr_dr{dr}", sine, 0x1000, 32, kind="env", off=0.9, end=1.0,
               adsr1=0x80 | (dr << 4) | 0x0F, adsr2=0x60, desc=f"ADSR AR 15, DR {dr}, SL 3, SR 0")
    for sr in (10, 16, 20):
        single(f"snes_adsr_sr{sr}", sine, 0x1000, 32, kind="env", off=0.9, end=1.0,
               adsr1=0x8F | (7 << 4), adsr2=0xE0 | sr, desc=f"ADSR AR 15, DR 7, SL 7, SR {sr}")
    single("snes_adsr_release", sine, 0x1000, 32, kind="env", off=0.3, end=0.5, adsr1=0x8F, adsr2=0xE0,
           desc="ADSR AR 15, key-off at 0.3 s (fixed release)")
    # GAIN modes: linear increase, bent increase, linear decrease, exponential decrease.
    for mode, label_ in ((0xC0, "lin_inc"), (0xE0, "bent_inc"), (0x80, "lin_dec"), (0xA0, "exp_dec")):
        for rate in (14, 20):
            s = SpcStim()
            src = s.add_sample(sine)
            start_gain = 0x00 if mode >= 0xC0 else 0x7F
            s.voice(0, src, 0x1000, gain=start_gain)
            s.at(0.0, KOFF, 0x00)
            s.at(0.0, FLG, s.dsp[FLG])
            s.at(S_ON, KON, 0x01)
            s.at(S_ON + 0.1, 0x07, mode | rate)
            s.at(0.9, KOFF, 0x01)
            add(f"snes_gain_{label_}_{rate}", s, 1.0, dict(kind="env", f0=1000.0, on=S_ON, off=0.9,
                                                        desc=f"GAIN {label_} rate {rate} from 0.12 s"))
    # Pitch modulation: voice 0 (silent, VOL 0) modulates voice 1.
    s = SpcStim()
    src = s.add_sample(sine)
    s.voice(0, src, 0x0040, vol_l=0, vol_r=0)       # 15.6 Hz modulator
    s.voice(1, src, 0x1000)
    s.dsp[PMON] = 0x02
    s.at(0.0, KOFF, 0x00)
    s.at(0.0, FLG, s.dsp[FLG])
    s.at(S_ON, KON, 0x03)
    s.at(0.9, KOFF, 0x03)
    add("snes_pmon", s, 1.0, dict(kind="lfo_pm", f0=1000.0, on=S_ON, off=0.9, desc="PMON: voice 0 (15.6 Hz sine, muted) modulates voice 1"))
    # Audio-rate PMON: 125 Hz modulator, carrier 1000 Hz (FM sidebands every 125 Hz).
    s = SpcStim()
    src = s.add_sample(sine)
    s.voice(0, src, 0x0200, vol_l=0, vol_r=0)
    s.voice(1, src, 0x1000)
    s.dsp[PMON] = 0x02
    s.at(0.0, KOFF, 0x00)
    s.at(0.0, FLG, s.dsp[FLG])
    s.at(S_ON, KON, 0x03)
    s.at(0.6, KOFF, 0x03)
    add("snes_pmon_fast", s, 0.7, dict(kind="tone", f0=125.0, on=S_ON, off=0.6,
                                       desc="PMON: voice 0 (125 Hz sine, muted) modulates voice 1 (1000 Hz); f0 = 125 Hz sideband grid"))
    # Noise at several rates.
    for rate in (0x10, 0x18, 0x1C, 0x1F):
        s = SpcStim()
        src = s.add_sample(sine)
        s.voice(0, src, 0x1000)
        s.dsp[NON] = 0x01
        s.dsp[FLG] = 0x20 | rate
        s.at(0.0, KOFF, 0x00)
        s.at(0.0, FLG, s.dsp[FLG])
        s.at(S_ON, KON, 0x01)
        s.at(0.5, KOFF, 0x01)
        add(f"snes_noise_{rate:02x}", s, 0.6, dict(kind="noise", f0=0.0, on=S_ON, off=0.5, desc=f"noise, FLG rate {rate}"))
    # Echo: a short sine burst into a 48 ms echo with feedback, three FIR presets.
    fir_presets = {
        "id": [0x7F, 0, 0, 0, 0, 0, 0, 0],
        "lp": [0x0C, 0x21, 0x2B, 0x2B, 0x13, 0xFE, 0xF3, 0xF9],
        "hp": [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x40],
    }
    for label_, taps in fir_presets.items():
        s = SpcStim()
        src = s.add_sample(sine)
        s.voice(0, src, 0x0800)
        s.dsp[EON] = 0x01
        s.dsp[EDL] = 3
        s.dsp[EFB] = 0x50
        s.dsp[0x2C] = s.dsp[0x3C] = 0x40
        for i, t in enumerate(taps):
            s.dsp[0x0F + 16 * i] = t & 0xFF
        s.dsp[FLG] = 0x00                  # echo writes on
        s.at(0.0, KOFF, 0x00)
        s.at(0.0, FLG, 0x00)
        s.at(S_ON, KON, 0x01)
        s.at(S_ON + 0.06, KOFF, 0x01)
        add(f"snes_echo_{label_}", s, 0.8, dict(kind="echo", f0=500.0, on=S_ON, off=S_ON + 0.06,
                                              desc=f"60 ms sine burst, EDL 3, EFB 0x50, EVOL 0x40, FIR {label_}"))
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=os.path.join("build-reports", "refcheck", "stimuli"))
    args = ap.parse_args()
    os.makedirs(os.path.join(args.out, "genesis"), exist_ok=True)
    os.makedirs(os.path.join(args.out, "snes"), exist_ok=True)
    manifest = []
    for meta, data in genesis_stimuli():
        with open(os.path.join(args.out, meta["file"]), "wb") as f:
            f.write(data)
        manifest.append(meta)
    for meta, spc, events in snes_stimuli():
        with open(os.path.join(args.out, meta["file"]), "wb") as f:
            f.write(spc)
        with open(os.path.join(args.out, meta["events"]), "w", newline="\n") as f:
            f.write(events)
        manifest.append(meta)
    with open(os.path.join(args.out, "manifest.json"), "w", newline="\n") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    print(f"{len(manifest)} stimuli -> {args.out}")


if __name__ == "__main__":
    main()
