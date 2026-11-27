#!/usr/bin/env python3
"""Dump Ghidra P-code for libasphalt6.so.

Usage:
    python tools/dump_pcode.py                 # import, analyse, dump
    python tools/dump_pcode.py --reuse         # reuse existing project
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prereq  # noqa: E402
from pyrun import NO_WINDOW  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

RECOVERED = []

GHIDRA_GUESSES = [
    "/opt/tools/ghidra",
    "/usr/local/ghidra",
    "/opt/ghidra",
    "C:/ghidra",
    "C:/Program Files/ghidra",
    "C:/Program Files/ghidra_12.1.4_PUBLIC",
]


def log(msg: str) -> None:
    print(msg, flush=True)


def find_ghidra(explicit: str | None):
    candidates = []
    if not explicit:
        fetched = prereq.find_fetched("ghidra")
        if fetched:
            candidates.append(str(fetched))
    if explicit:
        candidates.append(explicit)
    env = os.environ.get("GHIDRA_INSTALL_DIR")
    if env:
        candidates.append(env)
    candidates.extend(GHIDRA_GUESSES)

    for cand in candidates:
        cand = cand.rstrip("/\\")
        home = prereq.find_home(Path(cand), prereq.PREREQS["ghidra"]["marker"])
        if home is not None:
            return [str(home / "support" / prereq.headless_name())], "native"

    for name in ("analyzeHeadless", "analyzeHeadless.bat"):
        found = shutil.which(name)
        if found:
            return [found], "native"

    if shutil.which("wsl"):
        try:
            probe = subprocess.run(
                ["wsl", "-e", "sh", "-c", "test -x /opt/tools/ghidra/support/analyzeHeadless && echo yes"],
                capture_output=True, text=True, timeout=60)
            if probe.stdout.strip() == "yes":
                return ["wsl", "-e", "/opt/tools/ghidra/support/analyzeHeadless"], "wsl"
        except (OSError, subprocess.SubprocessError):
            pass

    return None, None


def to_wsl_path(path: Path) -> str:
    text = str(path.resolve())
    if len(text) > 2 and text[1] == ":":
        drive = text[0].lower()
        rest = text[2:].replace("\\", "/")
        return f"/mnt/{drive}{rest}"
    return text


def run(cmd, cwd) -> int:
    log("  $ " + " ".join(str(c) for c in cmd))
    start = time.time()
    proc = subprocess.Popen(cmd, cwd=str(cwd), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", bufsize=1,
                            creationflags=NO_WINDOW)
    assert proc.stdout is not None
    for line in proc.stdout:
        line = line.rstrip()
        if line:
            log("    " + line)
    rc = proc.wait()
    log(f"  ... finished in {time.time() - start:.0f}s (exit {rc})")
    return rc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ghidra", help="Ghidra install directory")
    ap.add_argument("--so", help="native library path")
    ap.add_argument("--project", help="Ghidra project directory")
    ap.add_argument("--out", help="P-code TSV output path")
    ap.add_argument("--extra", help="recovered-functions TSV path")
    ap.add_argument("--max-function-size", type=int, default=0)
    ap.add_argument("--reuse", action="store_true")
    ap.add_argument("--no-recover", action="store_true")
    args = ap.parse_args()

    so = Path(args.so) if args.so else ROOT / "analysis/apk_extract/lib/armeabi-v7a/libasphalt6.so"
    project = Path(args.project) if args.project else ROOT / "analysis/ghidra-work"
    out = Path(args.out) if args.out else ROOT / "analysis/reports/pcode_all.tsv"
    extra = Path(args.extra) if args.extra else ROOT / "analysis/reports/pcode_extra.tsv"
    scripts = ROOT / "recomp/ghidra_scripts"

    if not so.is_file():
        log(f"ERROR: native library missing: {so}")
        return 2

    prefix, kind = find_ghidra(args.ghidra)
    if not prefix:
        log("ERROR: Ghidra not found.")
        return 2
    log(f"ghidra: {' '.join(prefix)}")

    out.parent.mkdir(parents=True, exist_ok=True)

    if args.reuse:
        if not (project / "asphalt6.gpr").exists() and not (project / "asphalt6.rep").exists():
            log(f"ERROR: no project to reuse in {project}")
            return 2
        mode = ["-process", "-noanalysis"]
    else:
        if project.exists():
            shutil.rmtree(project, ignore_errors=True)
        project.mkdir(parents=True, exist_ok=True)
        mode = ["-import", to_wsl_path(so) if kind == "wsl" else str(so)]

    cmd = prefix + [to_wsl_path(project) if kind == "wsl" else str(project), "asphalt6",
                    *mode,
                    "-scriptPath", to_wsl_path(scripts) if kind == "wsl" else str(scripts),
                    "-postScript", "DumpPcode.java",
                    to_wsl_path(out) if kind == "wsl" else str(out),
                    str(args.max_function_size)]
    if run(cmd, ROOT) != 0:
        log("ERROR: Ghidra failed.")
        return 1

    if not out.exists() or out.stat().st_size == 0:
        log(f"ERROR: Ghidra produced no P-code at {out}")
        return 1
    log(f"p-code: {out} ({out.stat().st_size / 1048576:.1f} MiB)")

    if not args.no_recover and RECOVERED:
        cmd = prefix + [to_wsl_path(project) if kind == "wsl" else str(project), "asphalt6",
                        "-process", "-noanalysis",
                        "-scriptPath", to_wsl_path(scripts) if kind == "wsl" else str(scripts),
                        "-postScript", "AddAndDumpFunctions.java",
                        to_wsl_path(extra) if kind == "wsl" else str(extra),
                        *RECOVERED]
        if run(cmd, ROOT) != 0:
            return 1
    elif not extra.exists():
        extra.write_text("H\tlibasphalt6.so\t00010000\nX\t0\n")

    digest = hashlib.sha256(so.read_bytes()).hexdigest()
    (out.parent / (out.stem + ".stamp")).write_text(digest + "\n")
    log(f"stamped: {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
