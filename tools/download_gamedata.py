"""Download and verify Asphalt 6 HD game assets directly from an APK's Gameloft CDN manifest.

Reads `res/raw/data.txt` and `res/raw/pack*.info` from the user's APK, downloads
the official game data pack from Gameloft's CDN with HTTP Range resume support
(or per-entry Range fetches when only a few files are missing), unpacks each
compressed entry, verifies size and IEEE CRC-32 for all 843 files, extracts
`res/raw/*` and `lib/armeabi-v7a/libasphalt6.so` when missing, and optionally
generates `hires/` 2x UI sheets via `tools/gen_hires_ui.py`.
"""

from __future__ import annotations

import argparse
import io
import os
import posixpath
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile
import zlib
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
USER_AGENT = "Dalvik/1.6.0 (Linux; U; Android 4.1.2; R800i Build/4.1.B.0.587)"
PER_ENTRY_THRESHOLD = 40
CHUNK_SIZE = 256 * 1024


@dataclass(frozen=True)
class PackEntry:
    offset: int
    uncomp_size: int
    crc32: int
    dat_name: str
    zip_size: int
    zip_name: str


def find_apk(explicit: str | None) -> Path | None:
    if explicit:
        p = Path(explicit)
        if not p.is_absolute():
            p = (Path.cwd() / p).resolve()
        return p if p.is_file() else None
    for base in (Path.cwd(), ROOT):
        apks = sorted(base.glob("*.apk"))
        if apks:
            preferred = [a for a in apks if "asphalt" in a.name.lower()]
            return preferred[0] if preferred else apks[0]
    return None


def parse_data_txt(text: str) -> dict[str, str]:
    urls: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or ":" not in line:
            continue
        key, val = line.split(":", 1)
        key = key.strip().upper()
        val = val.strip()
        if val.startswith(("http://", "https://")):
            urls[key] = val
    return urls


def select_pack_source(apk: zipfile.ZipFile, url_override: str | None) -> tuple[str, str]:
    raw = apk.read("res/raw/data.txt").decode("utf-8", errors="replace")
    urls = parse_data_txt(raw)
    if url_override:
        return "ATC", url_override
    for variant in ("ATC", "PVRT", "DXT"):
        if variant in urls:
            return variant, urls[variant]
    if urls:
        first_key = next(iter(urls))
        return first_key, urls[first_key]
    raise RuntimeError("No CDN URLs found in APK res/raw/data.txt")


def parse_pack_info(blob: bytes) -> tuple[int, list[PackEntry]]:
    if len(blob) < 8:
        raise RuntimeError("pack info header truncated")
    hdr_val, count = struct.unpack_from(">II", blob, 0)
    pos = 8
    entries: list[PackEntry] = []
    for i in range(count):
        if pos + 18 > len(blob):
            raise RuntimeError(f"pack info truncated at entry {i}")
        offset, uncomp_size, crc32 = struct.unpack_from(">IIQ", blob, pos)
        pos += 16
        dat_len, = struct.unpack_from(">H", blob, pos)
        pos += 2
        dat_name = blob[pos:pos + dat_len].decode("utf-8", errors="strict")
        pos += dat_len
        zip_size, = struct.unpack_from(">I", blob, pos)
        pos += 4
        zip_len, = struct.unpack_from(">H", blob, pos)
        pos += 2
        zip_name = blob[pos:pos + zip_len].decode("utf-8", errors="strict")
        pos += zip_len
        entries.append(PackEntry(offset, uncomp_size, crc32 & 0xFFFFFFFF, dat_name, zip_size, zip_name))
    return hdr_val, entries


def load_manifest_from_apk(apk_path: Path, url_override: str | None) -> tuple[str, list[PackEntry]]:
    with zipfile.ZipFile(apk_path, "r") as apk:
        variant, url = select_pack_source(apk, url_override)
        names = set(apk.namelist())
        info_name = f"res/raw/pack{variant.lower()}.info"
        if info_name not in names:
            info_name = "res/raw/packatc.info"
        if info_name not in names:
            info_candidates = sorted(n for n in names if n.startswith("res/raw/pack") and n.endswith(".info"))
            if not info_candidates:
                raise RuntimeError("No res/raw/pack*.info manifest found inside APK")
            info_name = info_candidates[0]
        _, entries = parse_pack_info(apk.read(info_name))
        return url, entries


def file_crc32(path: Path) -> int:
    crc = 0
    with path.open("rb") as f:
        while True:
            buf = f.read(CHUNK_SIZE)
            if not buf:
                break
            crc = zlib.crc32(buf, crc)
    return crc & 0xFFFFFFFF


def safe_target(out_dir: Path, dat_name: str) -> Path:
    base = posixpath.basename(dat_name.replace("\\", "/"))
    if not base or base in (".", ".."):
        raise RuntimeError(f"Invalid manifest entry name: {dat_name!r}")
    return out_dir / base


def check_existing(out_dir: Path, entries: list[PackEntry]) -> tuple[list[PackEntry], list[PackEntry]]:
    valid: list[PackEntry] = []
    missing: list[PackEntry] = []
    for e in entries:
        target = safe_target(out_dir, e.dat_name)
        if not target.is_file():
            missing.append(e)
            continue
        if target.stat().st_size != e.uncomp_size:
            missing.append(e)
            continue
        if file_crc32(target) != e.crc32:
            missing.append(e)
            continue
        valid.append(e)
    return valid, missing


def http_head_length(url: str) -> int | None:
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            cl = resp.headers.get("Content-Length")
            return int(cl) if cl and cl.isdigit() else None
    except Exception:
        return None


def unpack_inner_zip(zip_bytes: bytes, entry: PackEntry, out_dir: Path) -> None:
    target = safe_target(out_dir, entry.dat_name)
    with zipfile.ZipFile(io.BytesIO(zip_bytes), "r") as zf:
        infos = [info for info in zf.infolist() if not info.is_dir()]
        if not infos:
            raise RuntimeError(f"Empty inner archive for {entry.dat_name}")
        member = infos[0]
        data = zf.read(member.filename)
    if len(data) != entry.uncomp_size:
        raise RuntimeError(
            f"Size mismatch for {entry.dat_name}: got {len(data)}, expected {entry.uncomp_size}"
        )
    actual_crc = zlib.crc32(data) & 0xFFFFFFFF
    if actual_crc != entry.crc32:
        raise RuntimeError(
            f"CRC-32 mismatch for {entry.dat_name}: got {actual_crc:#010x}, expected {entry.crc32:#010x}"
        )
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_name(target.name + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, target)


def fetch_missing_via_ranges(url: str, missing: list[PackEntry], out_dir: Path) -> None:
    total = len(missing)
    for idx, entry in enumerate(missing, 1):
        end = entry.offset + entry.zip_size - 1
        req = urllib.request.Request(
            url,
            headers={
                "User-Agent": USER_AGENT,
                "Range": f"bytes={entry.offset}-{end}",
            },
        )
        with urllib.request.urlopen(req, timeout=60) as resp:
            if resp.status not in (200, 206):
                raise RuntimeError(f"Unexpected HTTP {resp.status} for {entry.dat_name}")
            blob = resp.read()
        if resp.status == 200 and len(blob) > entry.zip_size:
            blob = blob[entry.offset:entry.offset + entry.zip_size]
        if len(blob) != entry.zip_size:
            raise RuntimeError(
                f"Range size mismatch for {entry.dat_name}: got {len(blob)}, expected {entry.zip_size}"
            )
        unpack_inner_zip(blob, entry, out_dir)
        print(f"  [{idx}/{total}] {entry.dat_name} ({entry.uncomp_size:,} bytes) OK")


def download_pack_with_resume(url: str, part_path: Path, expected_total: int) -> None:
    part_path.parent.mkdir(parents=True, exist_ok=True)
    existing = part_path.stat().st_size if part_path.is_file() else 0
    if existing > expected_total:
        part_path.unlink()
        existing = 0
    if existing == expected_total:
        print(f"Using cached {part_path} ({expected_total:,} bytes)")
        return

    head_len = http_head_length(url)
    if head_len is not None and head_len != expected_total:
        raise RuntimeError(
            f"Server Content-Length ({head_len}) != manifest total ({expected_total})"
        )

    headers = {"User-Agent": USER_AGENT}
    mode = "wb"
    if existing > 0:
        headers["Range"] = f"bytes={existing}-"
        mode = "ab"
        print(f"Resuming {url} from {existing:,} / {expected_total:,} bytes ...")
    else:
        print(f"Downloading {url} ({expected_total / (1024 * 1024):.1f} MiB) ...")

    req = urllib.request.Request(url, headers=headers)
    try:
        resp_cm = urllib.request.urlopen(req, timeout=60)
    except urllib.error.HTTPError as exc:
        if exc.code == 416 and existing > 0:
            part_path.unlink(missing_ok=True)
            download_pack_with_resume(url, part_path, expected_total)
            return
        raise

    with resp_cm as resp:
        if existing > 0 and resp.status == 200:
            existing = 0
            mode = "wb"
        downloaded = existing
        t_start = time.monotonic()
        t_last = t_start
        with part_path.open(mode) as out:
            while True:
                buf = resp.read(CHUNK_SIZE)
                if not buf:
                    break
                out.write(buf)
                downloaded += len(buf)
                now = time.monotonic()
                if now - t_last >= 0.5 or downloaded >= expected_total:
                    elapsed = max(now - t_start, 1e-3)
                    rate_mibs = ((downloaded - existing) / (1024 * 1024)) / elapsed
                    pct = (100.0 * downloaded) / expected_total if expected_total else 100.0
                    print(
                        f"\r  {downloaded / (1024 * 1024):6.1f} / {expected_total / (1024 * 1024):.1f} MiB "
                        f"({pct:5.1f}%, {rate_mibs:4.1f} MiB/s)   ",
                        end="",
                        flush=True,
                    )
                    t_last = now
        print()

    final_sz = part_path.stat().st_size
    if final_sz != expected_total:
        raise RuntimeError(f"Download incomplete: got {final_sz} bytes, expected {expected_total}")


def extract_from_pack_file(part_path: Path, missing: list[PackEntry], out_dir: Path) -> None:
    total = len(missing)
    with part_path.open("rb") as pf:
        for idx, entry in enumerate(missing, 1):
            pf.seek(entry.offset)
            zip_bytes = pf.read(entry.zip_size)
            if len(zip_bytes) != entry.zip_size:
                raise RuntimeError(f"Short read from pack for {entry.dat_name}")
            unpack_inner_zip(zip_bytes, entry, out_dir)
            if idx % 50 == 0 or idx == total:
                print(f"\r  Extracted & verified {idx}/{total} files   ", end="", flush=True)
    print()


def extract_apk_support_files(apk_path: Path, extract_dir: Path) -> int:
    extracted = 0
    with zipfile.ZipFile(apk_path, "r") as apk:
        for info in apk.infolist():
            if info.is_dir():
                continue
            name = info.filename
            if not (name.startswith("res/raw/") or name == "lib/armeabi-v7a/libasphalt6.so"):
                continue
            dst = extract_dir / name
            if dst.is_file() and dst.stat().st_size == info.file_size:
                continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(apk.read(name))
            extracted += 1
    return extracted


def verify_sha256_manifest(out_dir: Path) -> tuple[int, int]:
    import hashlib

    manifest = ROOT / "analysis" / "data-manifest.sha256"
    if not manifest.is_file():
        return 0, 0
    ok = bad = 0
    for line in manifest.read_text(encoding="utf-8-sig").splitlines():
        line = line.strip()
        if not line:
            continue
        expected, rel = line.split(None, 1)
        p = out_dir / Path(rel).name
        if not p.is_file():
            bad += 1
            continue
        digest = hashlib.sha256(p.read_bytes()).hexdigest()
        if digest == expected:
            ok += 1
        else:
            bad += 1
    return ok, bad


def main() -> int:
    ap = argparse.ArgumentParser(description="Download and verify Asphalt 6 HD game data from an APK.")
    ap.add_argument("--apk", help="Path to Asphalt 6 HD APK (defaults to auto-detect *.apk)")
    ap.add_argument("--out", default=str(ROOT / "gamedata"), help="Output gamedata directory")
    ap.add_argument(
        "--apk-extract",
        default=str(ROOT / "analysis" / "apk_extract"),
        help="Output directory for res/raw/* and libasphalt6.so",
    )
    ap.add_argument("--url", help="Override CDN pack URL from res/raw/data.txt")
    ap.add_argument("--verify-only", action="store_true", help="Verify files without downloading")
    ap.add_argument("--repair", action="store_true", help="Download missing/corrupt files only")
    ap.add_argument("--no-hires", action="store_true", help="Skip generating hires/ UI sheets")
    ap.add_argument("--keep-part", action="store_true", help="Keep build/packatc.zip.part after unpack")
    args = ap.parse_args()

    apk_path = find_apk(args.apk)
    if not apk_path:
        print("FAIL: No Asphalt 6 APK found. Pass --apk <path-to-apk>.")
        return 1

    out_dir = Path(args.out)
    if not out_dir.is_absolute():
        out_dir = (Path.cwd() / out_dir).resolve()
    extract_dir = Path(args.apk_extract)
    if not extract_dir.is_absolute():
        extract_dir = (Path.cwd() / extract_dir).resolve()

    print(f"APK: {apk_path}")
    url, entries = load_manifest_from_apk(apk_path, args.url)
    expected_pack_bytes = max((e.offset + e.zip_size) for e in entries)
    print(f"Manifest: {len(entries)} entries, CDN archive {expected_pack_bytes:,} bytes")
    print(f"Server URL: {url}")

    valid, missing = check_existing(out_dir, entries)
    print(f"Existing check ({out_dir}): ok={len(valid)} missing_or_bad={len(missing)}")

    if args.verify_only:
        if out_dir == (ROOT / "gamedata").resolve():
            sha_ok, sha_bad = verify_sha256_manifest(out_dir)
            if sha_ok or sha_bad:
                print(f"SHA-256 manifest: ok={sha_ok} bad={sha_bad}")
                if sha_bad:
                    return 1
        return 0 if not missing else 1

    support_count = extract_apk_support_files(apk_path, extract_dir)
    if support_count:
        print(f"Extracted {support_count} support file(s) from APK into {extract_dir}")

    if missing:
        out_dir.mkdir(parents=True, exist_ok=True)
        if len(missing) <= PER_ENTRY_THRESHOLD:
            print(f"Fetching {len(missing)} missing/corrupt file(s) via HTTP Range ...")
            fetch_missing_via_ranges(url, missing, out_dir)
        else:
            part_path = ROOT / "build" / "packatc.zip.part"
            download_pack_with_resume(url, part_path, expected_pack_bytes)
            extract_from_pack_file(part_path, missing, out_dir)
            if not args.keep_part:
                part_path.unlink(missing_ok=True)

        valid, missing = check_existing(out_dir, entries)
        print(f"Post-install check: ok={len(valid)} missing_or_bad={len(missing)}")
        if missing:
            print(f"FAIL: {len(missing)} file(s) failed verification")
            return 1

    if out_dir == (ROOT / "gamedata").resolve():
        sha_ok, sha_bad = verify_sha256_manifest(out_dir)
        if sha_ok or sha_bad:
            print(f"SHA-256 manifest: ok={sha_ok} bad={sha_bad}")
            if sha_bad:
                return 1

    if not args.no_hires and out_dir == (ROOT / "gamedata").resolve():
        hires_dir = ROOT / "hires"
        gen_hires = ROOT / "tools" / "gen_hires_ui.py"
        if gen_hires.is_file() and (not hires_dir.is_dir() or not any(hires_dir.glob("*_hires.tga"))):
            print("Generating 2x HiRes UI textures via tools/gen_hires_ui.py ...")
            rc = subprocess.run([sys.executable, str(gen_hires)], cwd=str(ROOT)).returncode
            if rc != 0:
                print(f"WARNING: gen_hires_ui.py returned {rc}")

    print("Game data installation and verification complete.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
