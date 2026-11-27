#!/usr/bin/env python3
"""Decode the Glitch engine obfuscation map (gamedata/file000000.dat, read-only).

Usage:
    python tools/deobfuscate_map.py gamedata/file000000.dat --key 0x4f --out analysis/reports/dat-map.tsv
"""
import argparse
from pathlib import Path


def sext(b: int) -> int:
    return b - 256 if b >= 128 else b


def trunc_div(a: int, b: int) -> int:
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def c_mod(a: int, b: int) -> int:
    return a - trunc_div(a, b) * b


def decode(data: bytes, key: int) -> bytes:
    out = bytearray(data)
    hist = [0, 0, 0]
    k = key
    divisor = key
    for i in range(len(out)):
        x = sext(out[i]) - k
        if x < 0:
            x += 0x100
        adj = x + (1 if x < 0 else 0)
        q2 = trunc_div(adj, 2)
        bit = (adj & 1) - (1 if x < 0 else 0)
        rem = c_mod(q2 * bit, divisor) if divisor else 0
        stored = x & 0xFF
        out[i] = stored
        hist[i % 3] = stored
        total = sext(hist[0]) + sext(hist[1]) + sext(hist[2])
        nk = k + trunc_div(total, 3) + ((i % 3) - 1) * rem
        nk = c_mod(nk, 0x100)
        if nk < 0:
            nk += 0x100
        k = nk
    return bytes(out)


def parse_map(decoded: bytes):
    text = decoded.split(b"c|-|3k", 1)[0]
    entries = []
    for line in text.replace(b"\r", b"").split(b"\n"):
        if b":" not in line:
            continue
        name, _, value = line.partition(b":")
        if name:
            entries.append((name.decode("latin-1"), value.decode("latin-1")))
    return entries


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--key", default="0x4f")
    ap.add_argument("--out")
    ap.add_argument("--limit", type=int, default=20)
    args = ap.parse_args()

    data = Path(args.path).read_bytes()
    key = int(args.key, 0)
    decoded = decode(data, key)
    stripped = decoded.rstrip(b"\x00")
    marker = stripped[-6:]
    ok = marker == b"c|-|3k"
    print(f"file       : {args.path}")
    print(f"size       : {len(data):,} bytes")
    print(f"key        : 0x{key:02x}")
    print(f"trailer    : {marker!r}")
    print(f"marker ok  : {ok}")
    if not ok:
        return 1

    entries = parse_map(decoded)
    print(f"entries    : {len(entries):,}")
    for name, value in entries[: args.limit]:
        print(f"  {name}  ->  {value}")
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text("\n".join(f"{n}\t{v}" for n, v in entries) + "\n", encoding="utf-8")
        print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
