#!/usr/bin/env python3
"""Verify toolchain, immutable gamedata manifest, and build Asphalt 6 HD natively."""
import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prereq  # noqa: E402
from pyrun import NO_WINDOW  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def find_gcc() -> str | None:
    if shutil.which("gcc"):
        return shutil.which("gcc")
    fetched = prereq.find_fetched("mingw")
    if fetched:
        cand = fetched / "bin" / ("gcc.exe" if sys.platform == "win32" else "gcc")
        if cand.exists():
            return str(cand)
    return None


def verify_gamedata() -> tuple[int, int]:
    manifest = ROOT / "analysis" / "data-manifest.sha256"
    if not manifest.exists():
        print("WARN: analysis/data-manifest.sha256 missing; skipping data check")
        return 0, 0
    ok = 0
    bad = 0
    with manifest.open("r", encoding="utf-8-sig") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            expected, rel = line.split(None, 1)
            target = ROOT / rel
            if not target.exists():
                print(f"MISSING: {rel}")
                bad += 1
                continue
            actual = hashlib.sha256(target.read_bytes()).hexdigest()
            if actual != expected:
                print(f"MISMATCH: {rel}")
                bad += 1
            else:
                ok += 1
    return ok, bad


def check_angle() -> str | None:
    candidates = [
        ROOT / "libEGL.dll",
        ROOT / "build" / "libEGL.dll",
        ROOT / "third_party" / "angle" / "libEGL.dll",
        Path.home() / "AppData" / "Local" / "Programs" / "Command Code" / "libEGL.dll",
    ]
    for c in candidates:
        if c.exists():
            return str(c.parent)
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true", help="verify prerequisites only")
    ap.add_argument("--jobs", type=int, default=8, help="build_win.py parallel jobs")
    ap.add_argument("--download", action="store_true", help="download/repair gamedata from APK CDN if missing")
    ap.add_argument("--apk", default=None, help="path to Asphalt 6 HD APK (optional)")
    args = ap.parse_args()

    gcc = find_gcc()
    if not gcc:
        print("FAIL: MinGW gcc not found in PATH or fetched prereqs")
        return 1
    print(f"gcc: {gcc}")

    ok, bad = verify_gamedata()
    if bad and (args.download or not (ROOT / "gamedata" / "file000000.dat").exists()):
        dl_script = ROOT / "tools" / "download_gamedata.py"
        if dl_script.is_file():
            print("gamedata missing or incomplete; running tools/download_gamedata.py ...")
            dl_cmd = [sys.executable, "-u", str(dl_script)]
            if args.apk:
                dl_cmd.extend(["--apk", args.apk])
            dl_rc = subprocess.run(dl_cmd, cwd=str(ROOT)).returncode
            if dl_rc == 0:
                ok, bad = verify_gamedata()
    print(f"gamedata manifest: ok={ok} bad={bad}")
    if bad:
        print("Tip: run `python tools/download_gamedata.py --apk <path-to-apk>` or `python tools/bootstrap.py --download`")
        return 1

    angle = check_angle()
    print(f"ANGLE dir: {angle if angle else 'not found (set ASPHALT6_ANGLE_DIR at runtime)'}")

    if not (ROOT / "build" / "guest.bin").exists():
        print("Generating build/guest.bin via tools/patch_guest.py ...")
        rc = subprocess.run([sys.executable, str(ROOT / "tools" / "patch_guest.py")],
                            cwd=str(ROOT), creationflags=NO_WINDOW).returncode
        if rc != 0:
            return rc

    if not (ROOT / "build" / "link" / "table.c").exists():
        print("FAIL: build/link/table.c missing; run recomp/link_all.py first")
        return 1

    if args.no_build:
        return 0

    cmd = [sys.executable, "-u", str(ROOT / "tools" / "build_win.py"), "--jobs", str(args.jobs)]
    return subprocess.run(cmd, cwd=str(ROOT)).returncode


if __name__ == "__main__":
    raise SystemExit(main())
