# Asphalt 6: Adrenaline HD (v3.7.0) — Native Windows x86-64 Port

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/mrbaazzman)

<a href="https://recomplauncher.com"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>

Static ARMv7-A -> C11 -> MinGW-w64 x86-64 recompilation of **Asphalt 6: Adrenaline HD 3.7.0** (`libasphalt6.so`).

- **No copyrighted game assets are included**: you must supply your own legal `Asphalt 6 HD 3.7.0.apk`.
- `gamedata/` and `Asphalt 6 HD 3.7.0.apk` remain strictly unmodified read-only inputs.
- Save data (`profile1.sav`, `profile1.bak`, `profileInit.sav`) is written to `%LOCALAPPDATA%\Asphalt6HD\userdata\` in standalone installs (or `build/userdata/` in dev builds).

## Quick Start

### Option A — Standalone Release Installer (Recommended — Zero Build Tools / Offline)

Download the single standalone **`Asphalt6_Setup.exe`** (`~26.8 MiB`) from GitHub Releases — no compiler, Python, ANGLE DLLs, or extra tools needed:

```powershell
.\build\Asphalt6_Setup.exe
```

1. Select your **Asphalt 6 HD 3.7.0 APK** (`Asphalt 6 HD 3.7.0.apk`).
2. Choose any **Install Location** (e.g., `C:\Games\Asphalt 6 HD`).
3. Click **Install** — completely offline (~2 seconds) on any Windows x86-64 PC (`-march=x86-64` portable binary):
   - Relocates `libasphalt6.so` -> `guest.bin` in memory and bundles `guest.bin`, `res/raw/*`, `gi_*.png`, and ANGLE runtime DLLs inside `<InstallDir>\asphalt6_win.exe` (`A6PAK` trailer).
   - Your install folder contains **only `asphalt6_win.exe`** (and `gamedata/` once copied manually or downloaded).
4. Launch `asphalt6_win.exe`: if `gamedata/` isn't placed manually, the in-game Gameloft Installer prompts to download & CRC-verify the official game data from Gameloft servers, then shows **`Touch OK and start playing!`**.

Release packaging command (maintainers): `python -u tools\pack_setup.py --jobs 8`. It also writes `build\Asphalt6-Native-<version>-Windows-x64.zip` (`Asphalt6_Setup.exe` with `recomplauncher.json` on top) for RecompLauncher: attach it to every release next to `Asphalt6_Setup.exe`.

### Option B — Command-Line / Developer Build

Verify prerequisites and compile incrementally (pass `--download` to fetch missing game data from your APK via Gameloft's CDN):

```powershell
python -u tools\bootstrap.py --jobs 8
```

Or run the standalone downloader CLI ([tools/download_gamedata.py](tools/download_gamedata.py) / `Asphalt6_Installer.exe`):

```powershell
python tools\download_gamedata.py --apk "Asphalt 6 HD 3.7.0.apk"
```

Run the game:

```powershell
.\build\asphalt6_win.exe
```

## Controls (Keyboard, Mouse & XInput Gamepad)

- **Mouse**: Point & click 2D HUD / `gameswf` UI and 3D ray-cast menu planes (`MainMenu`, `Career`, `QuickRace`, `Options`).
- **Keyboard**:
  - **W / Up Arrow**: Menu Up / Accelerate (manual schemes)
  - **S / Down Arrow**: Menu Down / Brake & Drift (all schemes)
  - **A / Left Arrow**, **D / Right Arrow**: Menu Left/Right / Smooth Progressive Steering
  - **Space / Shift**: Nitro boost (all schemes)
  - **Enter**: Confirm menu selection
  - **Esc / Backspace**: Pause / Back
- **XInput / Xbox Controller**:
  - **Left Stick / D-Pad**: Analog Steering / Menu Navigation
  - **RT**: Accelerate (manual schemes)
  - **LT / B**: Brake & Drift (in-race); **B**: Back (menus)
  - **A / X / RB**: Nitro boost (in-race); **A**: Confirm (menus)
  - **Start / Back**: Pause / Back

## Runtime Environment Variables

- `ASPHALT6_WIDTH`, `ASPHALT6_HEIGHT`: Override startup window size (default: native borderless fullscreen at desktop resolution, e.g., `1920x1080`).
- `ASPHALT6_WINDOWED=1`: Start in a bordered `1280x720` window (or press `Alt+Enter` in-game to toggle fullscreen/windowed).
- `ASPHALT6_BORDERLESS=1`: Force borderless fullscreen even when custom width/height are specified.
- `ASPHALT6_ANGLE_DIR`: Directory containing ANGLE `libEGL.dll` / `libGLESv2.dll`.
- `ASPHALT6_MAX_FRAMES`: Exit cleanly after N frames (headless/automation testing).
- `ASPHALT6_FPS`: Override target refresh/frame rate (default: auto-detected monitor refresh rate via `EnumDisplaySettingsA`).
- `ASPHALT6_NO_CAP=1`: Disable VSync (`eglSwapInterval(0)`) and the host frame limiter.
- `ASPHALT6_AUTO_TAPS` / `ASPHALT6_AUTO_KEYS`: Automated tap/key scripts for regression verification.
- `ASPHALT6_AUDIO_TRACE=1`, `ASPHALT6_VFS_TRACE=1`, `ASPHALT6_GL_TRACE=1`, `ASPHALT6_IMPORT_TRACE=1`, `ASPHALT6_PROFILE=1`: Optional diagnostics.
- `ASPHALT6_NO_INSTALLER=1`: Skip the startup `gamedata/` installer check in `asphalt6_win.exe`.
- Optional arguments (used by launchers; without them nothing changes):
  - `asphalt6_win.exe --data <dir>`: keep saves in `<dir>\userdata`, the runtime cache in `<dir>\cache` and a log in `<dir>\logs` instead of `%LOCALAPPDATA%\Asphalt6HD\`. `--windowed`, `--fps=<n>` and `--no-cap` match `ASPHALT6_WINDOWED=1`, `ASPHALT6_FPS` and `ASPHALT6_NO_CAP=1`.
  - `Asphalt6_Setup.exe --prepare --game=<apk> --install-dir=<dir>`: install without a window, printing `progress <0-1> <text>` lines. It exits right away when `<dir>` already holds this setup's build, so the APK is only needed when the build changes.
- Loose custom high-res textures can be placed in `hires/` (checked before `gamedata/`). Run `python tools\gen_hires_ui.py` to generate 2x `*_hires.tga` UI textures into `hires/` from the read-only `gamedata/` UI sheets.

## Architecture Overview

- `recomp/`: Ghidra 12.1.4 P-code -> C11 translator (`emit_c.py`, `link_all.py`, `coverage.py`, `diff_test.py`).
- `runtime/`: Native Windows host layer:
  - `main_win32.c`: Constructor boot, first-launch in-game installer gate (`installer_ensure_gamedata_gl()`), JNI init sequence, monitor-refresh frame/input loop.
  - `setup_win32.c`: Standalone Win32 GUI Installer (`Asphalt6_Setup.exe`, zero build tools, zero internet required during setup) embedding compressed portable `-march=x86-64` `asphalt6_win.exe` + ANGLE DLLs; relocates the user's 3.7.0 APK -> `guest.bin` in RAM and outputs a single self-contained `<InstallDir>\asphalt6_win.exe` (`A6PAK` payload) so `<InstallDir>` holds only `asphalt6_win.exe` and `gamedata/`.
  - `installer_win32.c`: `A6PAK` payload reader/builder, APK/disk manifest, ZIP/DEFLATE/PNG/ELF32/WinHTTP CDN downloader & CRC-32 verifier.
  - `installer_gl.c`: Full-screen ANGLE OpenGL ES Gameloft Game Installer screen inside `asphalt6_win.exe` using embedded `gi_*` UI assets (`Yes` / `Exit` / `OK` / `Cancel`) and prompting to download Gameloft game data when `gamedata/` is missing (`Touch OK and start playing!`).
  - `dispatch.c`: O(1) two-level function dispatch page directory compiled at `-O3`.
  - `platform_win32.c`: Win32 window, ANGLE EGL context, `timeBeginPeriod(1)` + monitor refresh rate detection, mouse, keyboard, and XInput queue.
  - `gl_win32.c`: GLES2 bridge over ANGLE (`libGLESv2.dll`) with pre-resolved GL procs + ETC1/ATC software texture decompression + default-FBO resize viewport scaling.
  - `vfs.c`: Virtual filesystem mapping Gameloft SD/data paths read-only to `gamedata/` (with `build/userdata/` overrides checked first) and writes to `build/userdata/`.
  - `imports.c`: 289 mapped Bionic/POSIX/C++/zlib/pthread/stdio/wchar imports with O(1) PLT handler cache and AAPCS register/stack varargs & float64 alignment.
  - `overrides.c`: Targeted hooks for `ALicenseCheck`, WinMM waveOut audio pump (`vox::DriverAndroid`), and `CalcOverlayPixel` paint overlay preservation.
  - `thread_win32.c`: Win32 pthread/cond/TLS backend with 4096-slot O(1) open-addressed mutex hash table.
- `tools/`: Pipeline and build scripts (`bootstrap.py`, `build_win.py`, `pack_setup.py`, `download_gamedata.py`, `dump_pcode.py`, `patch_guest.py`, etc.).

## Support

If you find this project helpful and want to support ongoing preservation & native porting work:

- **Ko-fi**: https://ko-fi.com/mrbaazzman
