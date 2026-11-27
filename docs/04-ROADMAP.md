# 04 — Roadmap

Milestones are sequential. Each gate must be verified with evidence before advancing.

## M0 — Foundation and reconnaissance ✅

- [x] Reference repos analyzed (`Asphalt5-Native`, `FastFiveHD-Native`)
- [x] Toolchain verified (Python 3.12, MinGW GCC 16.2, Ghidra 12.1.4 in WSL, pyelftools/capstone/unicorn)
- [x] Immutable data manifest generated (`analysis/data-manifest.sha256`, 843 files)
- [x] APK & native library hashes and architecture recorded (`docs/01-TARGET.md`)
- [x] ELF census (`analysis/reports/elf-census.md`) & instruction census (`analysis/reports/instruction-census.md`)
- [x] `file000000.dat` key `0x4F` verified (`analysis/reports/dat-map.tsv`, 836 entries)
- [x] Java/native boot sequence documented (`docs/02-ARCHITECTURE.md`)
- [x] Rules, strategy, state, evidence ledger written (`docs/00-RULES.md` .. `06-EVIDENCE.md`)

## M1 — Translator & differential proof gate ✅

- [x] `recomp/elf_image.py` verified on `libasphalt6.so` (0 bad relocations, 392 constructors)
- [x] Headless Ghidra P-code dump (`analysis/reports/pcode_all.tsv`) via `tools/dump_pcode.py` (30,357 functions, 30,066 with body)
- [x] `recomp/coverage.py` — 30,066/30,066 functions emitted cleanly (0 failed) across ARM, Thumb-2, and VFP
- [x] `recomp/diff_test.py` + `batch_diff.py` — differential verification against Unicorn (8/8 safe ARM + 3/3 VFP + 2/2 Thumb-2) with 0 mismatches
- [x] `recomp/emit_all.py` — whole-binary C compilation clean (151/151 chunks, 0 errors, 0 warnings)

## M2 — Full translation and Win64 link ✅

- [x] `recomp/gen_imports.py` maps all 289 PLT entries (`runtime/imports_table.h`)
- [x] `recomp/link_all.py` emits all chunks (`build/link/chunk_*.c`) + sorted `table.c`
- [x] Minimal Win64 link test (`build/asphalt6_boot_test.exe`) runs **392/392 `.init_array` constructors cleanly (0 faults)**
- [x] Boot frontier reaches `appInit` -> `Game::Init` -> `appRender`

## M3 — Native platform layer (Win32 / ANGLE / VFS / Audio / Threads) ✅

- [x] `runtime/vfs.c` serves `gamedata/` read-only, writes saves to `build/userdata/`
- [x] `runtime/platform_win32.c` + `runtime/gl_win32.c` create Win32 window + ANGLE EGL/GLES2 context
- [x] `runtime/thread_win32.c` provides pthread primitives
- [x] `runtime/java_bridge.c` satisfies `GLResLoader`, `GLUtils.Device`, `GLGame`, `ALicenseCheck`
- [x] Audio path (`_InitOSL` / `_InitAT` -> WinMM/waveOut) verified

## M4 — First boot & menu ✅

- [x] Game boots cleanly to Gameloft splash / title screen / `MainMenu` with real textures and audio
- [x] 0 guest faults, 0 GL shader/program errors

## M5 — Asset pipeline parity ✅

- [x] All `gamedata/` archives (`*.dat`, `file00a.bin`, `soundinfo.bin`) and textures (ATC/PVRTC/TGA) load cleanly
- [x] Save/profile round-trip verified in `build/userdata/` (`profile1.sav`, `profile1.bak`, `profileInit.sav`)

## M6 — Gameplay, controls & presentation ✅

- [x] Menus (`MainMenu`, `Career`, `Free Race`, `Garage`, `Options`, `Info`, `CarSelect`, `Tuning`), race start, live 3D driving loop, HUD, and pause (`IgMenu`) verified
- [x] Keyboard (`W/A/S/D`, Arrows, `Space`/`Shift`, `Enter`, `Esc`/`Backspace`), XInput gamepad (`Left Stick`/`D-Pad`, `RT`/`LT`, `A/B/X/RB`, `Start`), mouse touch/raycast input, `eglSwapInterval(1)` + 60 FPS pacing, and resize-safe viewport scaling
- [x] Car paint/taillight luminance preservation (`CalcOverlayPixel` BGRA->RGBA), AAPCS varargs float64 alignment (`arg_u64`), locked `vox::DriverCallbackInterface::FillBuffer` (`0x8d1850`), and GCC EABI `div` `sret` ABI (`0x422af0`) fixes

## M7 — 1:1 validation & Release polish ✅

- [x] `gamedata/` SHA-256 manifest parity verified (843/843 untouched), thread-safe VFS/JNI/pthread runtime, 8,500-frame live 3D race verified (`EXIT=0`), clean `tools/bootstrap.py`, `tools/build_win.py`, and `README.md`
