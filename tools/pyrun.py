"""Run a Python script, from a normal interpreter or from a frozen executable."""
import runpy
import subprocess
import sys
from pathlib import Path

NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0x08000000)


def frozen() -> bool:
    return bool(getattr(sys, "frozen", False))


def command_for(script: Path, args) -> list:
    if frozen():
        return [sys.executable, "--internal-run", str(script), *[str(a) for a in args]]
    return [sys.executable, str(script), *[str(a) for a in args]]


def run(script, args=(), cwd=None, capture=True):
    script = Path(script)
    cmd = command_for(script, args)
    if capture:
        return subprocess.run(cmd, cwd=str(cwd) if cwd else None,
                              capture_output=True, text=True, encoding="utf-8",
                              errors="replace", creationflags=NO_WINDOW)
    return subprocess.run(cmd, cwd=str(cwd) if cwd else None,
                          creationflags=NO_WINDOW)


def popen(script, args=(), cwd=None, **kwargs):
    script = Path(script)
    cmd = command_for(script, args)
    kwargs.setdefault("creationflags", NO_WINDOW)
    return subprocess.Popen(cmd, cwd=str(cwd) if cwd else None, **kwargs)


def run_here(script: Path, args) -> None:
    sys.argv = [str(script), *[str(a) for a in args]]
    try:
        runpy.run_path(str(script), run_name="__main__")
    except SystemExit as exc:
        code = exc.code
        if isinstance(code, int):
            raise
        if code is not None:
            print(code, file=sys.stderr)
            raise SystemExit(1)
        raise SystemExit(0)
