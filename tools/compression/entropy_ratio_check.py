#!/usr/bin/env python3
"""How much disk-size reduction is realistically available for GO's cache,
if we entropy-code its own prediction residuals instead of wrapping the
whole cache in zlib (today's approach)?

Compares, all on the SAME GO residual stream (interleaved int16, computed
exactly like GOSoundAudioSection::Compress()'s predictor):
  zlib   - today's cache wrapper
  bz2    - block-sort + Huffman, for reference
  lzma   - range coder + dictionary (7-Zip's algorithm), strong general case
  rice   - the actual technique FLAC/WavPack use for residuals: optimal
           Golomb-Rice parameter per block, unary+binary remainder bits

Result: rice reaches 2.31x overall (up to 3.82x on clean pipe tones) -
this is where the "3-4x" figure people remember actually comes from.
It requires real entropy coding though, which is far slower to decode
than GO's current byte-granularity scheme (see real_sample_compression_check.cpp
for the 1.5x ceiling of that simpler scheme). Ratio and decode speed are
in tension here; we ended up prioritizing speed (see planar_blockwidth_check.cpp,
simd_multivoice_check.cpp).
"""
import sys
import wave
import struct
import zlib
import bz2
import lzma


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


def zigzag(n):
    return (n << 1) ^ (n >> 31) if n >= 0 else ((-n) << 1) - 1


def zigzag_list(residuals):
    out = []
    for r in residuals:
        out.append((r << 1) if r >= 0 else (((-r) << 1) - 1))
    return out


class BitWriter:
    def __init__(self):
        self.bits = []

    def write_unary(self, q):
        self.bits.extend([1] * q)
        self.bits.append(0)

    def write_bits(self, value, n):
        for i in range(n - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def byte_length(self):
        return (len(self.bits) + 7) // 8


def rice_encode_block(values, k):
    """Golomb-Rice with parameter k: quotient q=v>>k unary, remainder k bits."""
    bw = BitWriter()
    mask = (1 << k) - 1
    for v in values:
        q = v >> k
        bw.write_unary(q)
        bw.write_bits(v & mask, k)
    return bw.byte_length()


def best_rice_k(values):
    """FLAC/WavPack pick k from the block's mean magnitude; try a small
    range around the estimate and keep the actual best (what real encoders
    effectively do via a short search)."""
    if not values:
        return 0, 0
    mean = sum(values) / len(values)
    k_est = max(0, int(mean).bit_length() - 1)
    best = None
    for k in range(max(0, k_est - 2), k_est + 3):
        size = rice_encode_block(values, k)
        if best is None or size < best[1]:
            best = (k, size)
    return best


def rice_total(residuals, block_size=4096):
    zz = zigzag_list(residuals)
    total = 0
    for i in range(0, len(zz), block_size):
        block = zz[i : i + block_size]
        k, size = best_rice_k(block)
        total += size + 1  # +1 byte to store k per block
    return total


def main():
    grand_raw = 0
    grand = {"zlib": 0, "bz2": 0, "lzma": 0, "rice": 0}

    for path in sys.argv[1:]:
        samples, ch, raw_bytes = load_wav_int32(path)
        res = go_residuals(samples, ch)
        packed = struct.pack(f"<{len(res)}h", *[max(-32768, min(32767, r)) for r in res])

        z = len(zlib.compress(packed, 9))
        b = len(bz2.compress(packed, 9))
        l = len(lzma.compress(packed, preset=6))
        r = rice_total(res)

        name = path.split("/")[-1]
        print(
            f"{name:14s} raw {raw_bytes:9d} B  residual-packed {len(packed):9d} B | "
            f"zlib {raw_bytes/z:5.3f}x  bz2 {raw_bytes/b:5.3f}x  "
            f"lzma {raw_bytes/l:5.3f}x  rice {raw_bytes/r:5.3f}x"
        )

        grand_raw += raw_bytes
        grand["zlib"] += z
        grand["bz2"] += b
        grand["lzma"] += l
        grand["rice"] += r

    print(f"\nTOTAL (baseline fixed-2-byte = 1.500x always):")
    for name, val in grand.items():
        print(f"  {name:6s} on GO's own residuals: {grand_raw/val:5.3f}x")


if __name__ == "__main__":
    main()
