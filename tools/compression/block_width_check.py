#!/usr/bin/env python3
"""Chatty's 'block predictor + fixed bitpacking' idea: pick a fixed bit
width PER BLOCK (not per sample, not globally fixed like my 2-byte idea),
based on that block's actual residual range. Decode stays branch-free
*within* a block (one width lookup, then straight fixed-width unpacking) -
only one branch per block, not per sample.

Compares against:
  - global fixed 16-bit (my flat idea, = the 1.500x baseline)
  - per-block adaptive width, various block sizes

Result: per-block adaptive width reaches ~2.14-2.30x depending on block
size - nearly matching Rice coding's ratio (entropy_ratio_check.py) while
being far simpler to decode (one width lookup per block, then plain
fixed-width unpacking, no unary/bit-level decoding). This became the
starting point for the C++ implementation in planar_blockwidth_check.cpp.
"""
import sys
import wave
import math


def load_wav_int32(path):
    w = wave.open(path, "rb")
    n, ch, sw = w.getnframes(), w.getnchannels(), w.getsampwidth()
    raw = w.readframes(n)
    assert sw == 3
    samples = []
    for i in range(0, len(raw), 3):
        v = raw[i] | (raw[i + 1] << 8) | (raw[i + 2] << 16)
        if v & 0x800000:
            v -= 1 << 24
        samples.append(v)
    return samples, ch, len(raw)


def go_residuals(samples, ch):
    value = [0] * ch
    prev = [0] * ch
    last = [0] * ch
    res = []
    nframes = len(samples) // ch
    for f in range(nframes):
        for c in range(ch):
            last[c] = prev[c]
            prev[c] = value[c]
            value[c] = samples[f * ch + c]
            pred = prev[c] + int((prev[c] - last[c]) / 2)
            res.append(value[c] - pred)
    return res


def zigzag(r):
    return (r << 1) if r >= 0 else (((-r) << 1) - 1)


def bits_needed(zz_max):
    return max(1, zz_max.bit_length())


def per_block_width_bits(residuals, block_size, header_bits=6, escape_extra_bits=32, escape_fraction_cap=0.0):
    """Sum of bits for: header (width, header_bits wide) + block_size*width,
    per block. No escapes modeled here (0 needed per our earlier finding)."""
    total_bits = 0
    n = len(residuals)
    for i in range(0, n, block_size):
        block = residuals[i : i + block_size]
        zz = [zigzag(r) for r in block]
        w = bits_needed(max(zz))
        total_bits += header_bits + w * len(block)
    return total_bits


def main():
    grand_raw = 0
    grand_global16 = 0
    block_sizes = [256, 1024, 4096, 16384]
    grand_block = {bs: 0 for bs in block_sizes}

    for path in sys.argv[1:]:
        samples, ch, raw_bytes = load_wav_int32(path)
        res = go_residuals(samples, ch)

        global16_bits = 16 * len(res)
        line = f"{path.split('/')[-1]:14s} raw {raw_bytes:9d} B  global16 {raw_bytes/(global16_bits/8):5.3f}x"
        for bs in block_sizes:
            bits = per_block_width_bits(res, bs)
            grand_block[bs] += bits / 8
            line += f"  blk{bs:<6d} {raw_bytes/(bits/8):5.3f}x"
        print(line)

        grand_raw += raw_bytes
        grand_global16 += global16_bits / 8

    print(f"\nTOTAL:")
    print(f"  global fixed 16-bit (flat):        {grand_raw/grand_global16:5.3f}x")
    for bs in block_sizes:
        print(f"  per-block adaptive width, N={bs:<6d}: {grand_raw/grand_block[bs]:5.3f}x")


if __name__ == "__main__":
    main()
