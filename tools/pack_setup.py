#!/usr/bin/env python3
"""Build the standalone GitHub Release `build/Asphalt6_Setup.exe`.

Builds:
  1. Portable x86-64 baseline `build/release/asphalt6_win.exe` (`-march=x86-64 -mtune=generic`,
     works on ANY Windows x86-64 CPU without AVX/AVX2 requirements)
  2. `build/Asphalt6_Setup_stub.exe` (pure offline Win32 GUI installer)
  3. Compresses `asphalt6_win.exe` + ANGLE DLLs (`libEGL.dll`, `libGLESv2.dll`,
     `d3dcompiler_47.dll`) with raw DEFLATE into an A6PAK trailer appended to
     `build/Asphalt6_Setup.exe` (~26 MiB single file).
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import bootstrap  # noqa: E402


def raw_deflate(data: bytes) -> bytes:
    co = zlib.compressobj(level=6, method=zlib.DEFLATED, wbits=-15)
    return co.compress(data) + co.flush()


def pack_a6pak(stub_exe: Path, files: list[tuple[str, bytes]], out_exe: Path) -> None:
    stub_bytes = stub_exe.read_bytes()
    out_exe.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_exe.with_suffix(".exe.tmp")
    recs: list[tuple[bytes, int, int, int, int, int]] = []
    with tmp.open("wb") as f:
        f.write(stub_bytes)
        for name, raw in files:
            comp = raw_deflate(raw)
            method = 8
            if len(comp) >= len(raw):
                comp = raw
                method = 0
            offset = f.tell()
            f.write(comp)
            crc = zlib.crc32(raw) & 0xFFFFFFFF
            recs.append((name.encode("utf-8"), offset, len(comp), len(raw), crc, method))

        toc_off = f.tell()
        for name_b, off, csz, usz, crc, method in recs:
            f.write(struct.pack("<H", len(name_b)))
            f.write(name_b)
            f.write(struct.pack("<QQQIB", off, csz, usz, crc, method))
        toc_sz = f.tell() - toc_off
        f.write(struct.pack("<QQI4s", toc_off, toc_sz, len(recs), b"A6PK"))
    os.replace(tmp, out_exe)


def main() -> int:
    ap = argparse.ArgumentParser(description="Build single-file standalone Asphalt6_Setup.exe release binary.")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--angle-dir", default=None, help="Directory with libEGL.dll & libGLESv2.dll")
    args = ap.parse_args()

    angle_str = args.angle_dir or bootstrap.check_angle()
    if not angle_str:
        print("FAIL: ANGLE DLLs (libEGL.dll, libGLESv2.dll) not found")
        return 1
    angle_dir = Path(angle_str)

    rc = subprocess.run(
        [sys.executable, "-u", str(ROOT / "tools" / "build_win.py"),
         "--portable", "--jobs", str(args.jobs)],
        cwd=str(ROOT),
    ).returncode
    if rc != 0:
        return rc

    stub = ROOT / "build" / "Asphalt6_Setup_stub.exe"
    engine = ROOT / "build" / "release" / "asphalt6_win.exe"
    out_setup = ROOT / "build" / "Asphalt6_Setup.exe"

    payload: list[tuple[str, bytes]] = [("asphalt6_win.exe", engine.read_bytes())]
    for dll in ("libEGL.dll", "libGLESv2.dll", "d3dcompiler_47.dll"):
        dp = angle_dir / dll
        if dp.is_file():
            payload.append((dll, dp.read_bytes()))

    print(f"Packing {len(payload)} files into {out_setup} ...")
    pack_a6pak(stub, payload, out_setup)
    print(f"Built standalone release installer: {out_setup} ({out_setup.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
