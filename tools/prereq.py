#!/usr/bin/env python3
"""Tool discovery helpers for Asphalt 6 HD port."""
import os
import re
import shutil
import subprocess
from pathlib import Path

NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)

PREREQS = {
    "ghidra": {"marker": "support/" + ("analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless")},
    "jdk": {"marker": "bin/java.exe"},
    "mingw": {"marker": "bin/gcc.exe"},
}


def headless_name() -> str:
    return "analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless"


def find_home(root: Path, marker: str) -> Path | None:
    if (root / marker).exists():
        return root
    try:
        for sub in sorted(root.iterdir()):
            if sub.is_dir() and (sub / marker).exists():
                return sub
    except OSError:
        pass
    return None


def find_fetched(key: str) -> Path | None:
    local = os.environ.get("LOCALAPPDATA") or str(Path.home())
    for base in ("Asphalt6PortBuilder", "FastFivePortBuilder"):
        root = Path(local) / base / "tools" / key
        found = find_home(root, PREREQS[key]["marker"])
        if found:
            return found
    return None


def jdk_version(home: Path) -> int:
    java = home / "bin" / ("java.exe" if os.name == "nt" else "java")
    if not java.exists():
        return 0
    try:
        out = subprocess.run([str(java), "-version"], capture_output=True,
                             text=True, timeout=30, creationflags=NO_WINDOW)
        text = (out.stderr or "") + (out.stdout or "")
    except (OSError, subprocess.SubprocessError):
        return 0
    m = re.search(r'version "(\d+)', text)
    return int(m.group(1)) if m else 0
