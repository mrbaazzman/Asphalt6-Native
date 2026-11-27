# 03 — Strategy

Follows the exact static recompilation methodology proven in **Asphalt5-Native** and **FastFiveHD-Native**.

## 1. Core requirement

A native Windows x64 executable (`build/asphalt6_win.exe`) that reproduces the Android build 1:1,
with **no** emulator, interpreter, JIT, or third-party engine/middleware at runtime, reading the
untouched `gamedata/` tree in place.

## 2. Pipeline — static recompilation (ARMv7 / Thumb-2 → P-code → C → x64)

```
libasphalt6.so  (22.1 MB, ARM + Thumb-2 STLport, VFPv2, softfp)
   │  ELF32 loader: PT_LOAD segments, 216,412 relocations, 392 .init_array constructors
   ▼
Ghidra 12.1.4 headless (DumpPcode.java + AddAndDumpFunctions.java)
   │  P-code TSV per instruction (normalizes ARM and Thumb-2 uniformly)
   ▼
recomp/emit_c.py & recomp/link_all.py
   │  Literal P-code -> C11 state machines + sorted address dispatch table
   ▼
MinGW-w64 GCC x86-64 (-O2) + runtime/*.c (Win32 / ANGLE GLES2 / VFS / OpenSL->waveOut / JNI / pthreads)
   ▼
build/asphalt6_win.exe + build/guest.bin
```

## 3. Fidelity decisions

- **Instructions, not decompilation:** `emit_c.py` translates Ghidra's raw P-code per machine
  instruction. Ghidra's C decompiler is for human analysis only, never code generation.
- **Original guest address space:** Guest memory preserves link addresses (`0x0..0x11658c8`) inside
  a contiguous guest buffer (`256 MiB`–`512 MiB`) so vtables, RTTI, `.data.rel.ro`, and pointer
  arithmetic remain valid without pointer rewriting.
- **ARM + Thumb-2:** Unlike Fast Five HD (100% ARM), `libasphalt6.so` contains 1,102 Thumb-2
  functions (all in STLport `0x8899d5..0x8a78dd`). Ghidra P-code handles both ISA modes uniformly;
  our differential harness (`recomp/diff_test.py`) verifies both ARM (`UC_MODE_ARM`) and Thumb
  (`UC_MODE_THUMB`) functions against Unicorn before full-binary linking.
- **Soft-float ABI:** `libasphalt6.so` uses `softfp` (VFPv2 registers `s0–s31` / `d0–d15`, float
  arguments passed in core registers `r0–r3` / stack).

## 4. Platform layer (`runtime/`)

`runtime/` is pure C11 over Win32 / ANGLE / WinMM:

- `cpu.h`, `mem.h`: Guest CPU state (`r0–r12`, `sp`, `lr`, `pc`, `vfp[64]`, flags) and bounds-checked memory access.
- `imports.c`, `imports_table.h`: 289 PLT import handlers (libc, libm, libstdc++, pthread, OpenSL ES, GLES2, log, sockets).
- `guest_data.c`: Bionic `_ctype_`, `_tolower_tab_`, `_toupper_tab_`, `__sF`, `__stack_chk_guard`, `SL_IID_*` GUIDs.
- `vfs.c`: Read-only path mapping into `gamedata/`, writable saves in `build/userdata/`, optional derived cache in `build/overlay/`.
- `thread_win32.c`: Win32-backed pthread subset (`pthread_create`, mutex, cond, TLS, once).
- `platform_win32.c`, `gl_win32.c`: Win32 window + ANGLE EGL/GLES2 context + GLES2/GLES1 dispatch + ATC/PVRTC/ETC texture handling + input mapping.
- `java_bridge.c`: Minimal `_JNIEnv` / `_JavaVM` + `GLGame` / `GLResLoader` / `GLUtils.Device` / `ALicenseCheck` callbacks.
- `overrides.c`: Targeted host overrides (`call_override`) if any guest function requires host-side timing/audio/resolution hooks.
- `resolution.c`: Unified presentation transform (design canvas ↔ native window).
- `main_win32.c`: Relocated guest image loader, `.init_array` runner, `appInit` / `appRender` loop, crash diagnostics.

## 5. Toolchain

- **Analysis:** WSL Ubuntu (`readelf`, `python3`, `/opt/tools/ghidra` 12.1.4) + REA + Craft Anything
- **Host compiler:** Windows MinGW-w64 GCC 16.2.0 (`x86_64-w64-mingw32`)
- **Python:** Python 3.12 (`pyelftools`, `capstone`, `unicorn` installed and verified on both Windows and WSL)
- **Diff oracle:** Unicorn (`unicorn` ARM/Thumb, offline test gate only)
