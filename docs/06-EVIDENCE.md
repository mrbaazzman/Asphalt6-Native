# 06 — Evidence Ledger

Every technical claim resolves to a tool record or direct byte inspection below.

## M0 — Package, binaries, Java layer, and engine identity

| Source / Evidence ID | Operation | Claim |
|---|---|---|
| Craft `t_70390c25fc65` | `core_target_open` | `Asphalt 6 HD 3.7.0.apk`: 9,621,003 bytes, SHA-256 `70390c25fc65b3c85676ce320e3eacd8c12fd5d1d548587f76c2f90efb7ae60d` |
| REA `ev_a7b9a893…` | `inspect_android_package` | `com.gameloft.android.GAND.GloftAsp6.asphalt6`, v3.7.0 / code 370, minSdk 9, 97 classes, 96 resources, 5 activities |
| REA `ev_0e41a2c3…` | `search_android_classes` | Complete inventory of 97 DEX classes |
| REA `ev_471d40e1…` | `inspect_android_method` `GLGame.<clinit>` | Calls `System.loadLibrary("asphalt6")` and `System.loadLibrary("StormGLOFT")` |
| REA `ev_cbf7bc6e…` | `inspect_android_method` `GLGame.onCreate` | Creates `GameGLSurfaceView`, sets language id `GLGame.d`, initializes `GLUtils.Device` |
| REA `ev_14b2fb3b…` | `inspect_android_method` `GameRenderer.onSurfaceCreated` | Calls `GLResLoader.init()`, `GLMediaPlayer.init()`, `GLGame.nativeInit()`, `nativeKeyboardEnabled` |
| REA `ev_c4319065…` | `inspect_android_method` `GameRenderer.onDrawFrame` | Calls `nativeInit(w, h, GLGame.d)` on first frame, then `nativeRender()` every frame |
| Craft `ev_a1e39750…` | `native_layout_show` (`libasphalt6.so`) | ELF32 ARM, 21 sections, 140,231 symbols, 216,412 relocations, NX yes, PIE no, RELRO none, canary yes, unstripped |
| Craft `ev_706f0bd9…` | `native_library_calls_list` (`libasphalt6.so`) | 301 imports from `libGLESv1_CM`, `libGLESv2`, `libOpenSLES`, `libc`, `libdl`, `liblog`, `libm`, `libstdc++` |
| Craft `ev_6781c5e4…` / `ev_c5c51e78…` | `native_layout_show` / `native_library_calls_list` (`libStormGLOFT.so`) | 825,616 B, stripped, 119 imports, NDK r16 stlport shim hooking `libasphalt6.so` GL imports + `glslopt` + ATC/ETC/PVRTC decompression |
| Direct `readelf -h -A` | WSL `readelf` | `libasphalt6.so`: ARMv7-A, `Thumb-2`, `VFPv2`, `Tag_ABI_HardFP_use: Deprecated` (softfp), entry `0x4233a0` |
| Direct `tools/elf_census.py` | Python ELF parser | `29,810` defined funcs in `.symtab` (`28,708` ARM, `1,102` Thumb all in `0x8899d5..0x8a78dd`), `.init_array` 392, `.ARM.exidx` 29,693 rows |
| Direct `tools/instruction_census.py` | Capstone ARM + Thumb-2 | `1,545,201` instructions (`1,496,208` ARM, `48,993` Thumb-2), `92,557` VFP, 0 atomics, `2,601` bitfield ops |
| Direct Capstone disasm | `JNI_OnLoad` `0x42a6b4` | `NVThreadInit`, `GetEnv(1.4)`, `mWaitForInit=3`, `VoxSetJavaVM`, `VoxSetAndroidAPILevel(9)` |
| Direct Capstone disasm | `appInit` `0x42a9bc` | Paths `/sdcard/games/com.gameloft.android.GAND.GloftAsp6.asphalt6/` and `/data/data/com.gameloft.android.GAND.GloftAsp6.asphalt6/`, `Application::CreateInstance` (`0x432054`) |
| Direct Capstone disasm | `Game::Init` `0x43aae8` | Calls `addObfuscationFileMap` (`0x957ad0`) with key `0x4F`, mounts 6 `.zip` archives (`trackTextures`, `carTextures`, `trafficTextures`, `objectsTextures`, `sceneTextures`, `particleTextures`) |
| Direct Capstone disasm | `vox::DriverAndroid::Init` `0x8ce488` | Branches on `apiLevel > 8` → `_InitOSL` (`0x8cdf34`) vs `_InitAT` (`0x8ce1c4`) |
| Direct byte scan | `gamedata/` | 843 files hashed in `analysis/data-manifest.sha256`; `file000000.dat` key `0x4F` yields `c|-|3k` trailer and 836 entries (`analysis/reports/dat-map.tsv`) |

## M1 — Translator & differential verification

| Source / Evidence ID | Operation | Claim |
|---|---|---|
| `recomp/elf_image.py` | ELF relocation dry run | 216,412 relocations applied, 0 bad, 392 `.init_array` entries (`0xfe9c68`) |
| `tools/patch_guest.py` | Guest RAM image generation | 9 extern data symbols patched above `0x11658c8`; `build/guest.bin` (18,241,736 bytes) |
| `tools/dump_pcode.py` | Native Ghidra 12.1.4 headless | `analysis/reports/pcode_all.tsv` (418.6 MiB): 30,357 functions, 30,066 with body |
| `recomp/coverage.py` | P-code op/userop audit | 30,066/30,066 emitted (0 failures); `coprocessor_*` only in WMMX unwind helpers (`0x00b50c90`) and `software_interrupt` only in `.rodata`-like `FUN_00db9328` |
| `recomp/diff_test.py` + `batch_diff.py` | Unicorn vs MinGW GCC C11 diff | Fixed `Emitter.emit()`, `ram:` derefs (`ld4`), `adr`/`bl` PC constants, and `tbh`/`tbb` `code_const`; 8/8 safe ARM (`batch_diff.py --safe 8 --cases 10`), 3/3 VFP (`0x450a0c`, `0x45098c`, `0x44fd20`), and 2/2 Thumb-2 (`0x8899d4`, `0x889ad4`) passed with 0 mismatches |
| `recomp/emit_all.py` | Whole-binary C11 compilation | 30,066 functions in 151 chunks compiled with `gcc -std=c11 -O1 -Wall -Wextra`: 151/151 chunks clean, 0 errors, 0 warnings |

## M2 — Full link & static constructors

| Source / Evidence ID | Operation | Claim |
|---|---|---|
| `recomp/link_all.py --boot` | Full Win64 link + `.init_array` probe | Linked `build/asphalt6_boot_test.exe`; fixed Bionic `_ctype_`/`_tolower_tab_`/`_toupper_tab_` indirection in `runtime/guest_data.c`; **392/392** `.init_array` constructors (389 ARM + 3 Thumb-2) executed with 0 faults |

## M3–M7 — Runtime, Gameplay, UI, and Final Verification

| Source / Evidence ID | Operation | Claim |
|---|---|---|
| `runtime/imports.c` (`arg_u64`/`arg_f64`) | AAPCS varargs & f64 parity fix | Fixed `r3` padding slot alignment in `arg_u64` so `snprintf("%.14g", ...)` in `gameswf::as_value::to_tu_string` (`0x788e10`) reads exact IEEE-754 doubles, fixing Career league locks (`15*`/`30*`/`45*`), `CarSelect` (`MINI John Cooper Works`, stats, badges), and `Tuning` |
| `runtime/overrides.c` (`FN_CALC_OVERLAY`) | `0x51fb24` paint & taillight fix | Swizzled BGRA TGA input (`Nissan_370Znismo_2010_Body.tga`, `BMW_Mini_2010_Body.tga`) to RGBA (`GL_RGBA`) while preserving paint luminance -> electric-blue MINI and red 370Z taillights/brake lights |
| `runtime/overrides.c` (`FN_VOX_FILL_BUFFER`) & `runtime/vfs.c` / `java_bridge.c` / `thread_win32.c` | Audio mixer race & thread-safety fix | Updated `FN_VOX_FILL_BUFFER` from unlocked `0x8d170c` to locked `0x8d1850` (`DriverCallbackInterface::FillBuffer`), resolving voice-node use-after-free (`0x97b5ac50` at frame 6409); added VFS/JNI locks and 2048 mutex slots |
| `runtime/imports.c` (`div` `0x422af0`) | Android EABI `div` `sret` fix | Fixed `div` to write `{quot, rem}` via hidden `sret` pointer (`r0`) with `r1=num, r2=den`, resolving `__stack_chk_fail` in timer/split `fn_53a8c0` (`0x0053aa40`) |
| `runtime/platform_win32.c` / `main_win32.c` | Native 1080p borderless fullscreen default | Defaults to monitor resolution (`1920x1080` borderless fullscreen via `SM_CXSCREEN`/`SM_CYSCREEN`, `ASPHALT6_WINDOWED=1` / `Alt+Enter` toggle) so `GameRenderer_nativeInit(1920, 1080, 1)` initializes natively without FBO upscaling; verified `platform: EGL 1.5 surface 1920x1080 target=120.0Hz (vsync=1)` across Main Menu and Nassau 3D race (`EXIT=0`) |
| Visual PNG & execution checks | `asphalt6_win.exe` end-to-end runs | Verified `MainMenu` (`Up`/`Down`), `Career` (`Left`/`Right`), `CarSelect`, live 3D race controls (`Gas`, `Left`/`Right` steer, `Brake`, `Nitro`, `Esc` Pause) across 1,610 frames (`glDraws=102344`, `EXIT=0`, `CRASH_LOG=False`) and 8,500-frame **Nassau** endurance run (`glDraws=502619`, `EXIT=0`), `build/userdata/profile1.sav`, and `843/843` `gamedata/` SHA-256 intact |
| `runtime/dispatch.c`, `gl_win32.c`, `imports.c`, `thread_win32.c`, `mem.h`, `platform_win32.c`, `tools/build_win.py` | High-FPS monitor-refresh optimization & benchmarks | Replaced `-O0` binary search + TLS ring with `-O3` O(1) page directory (`g_fn_dir[4096]`), pre-resolved ANGLE GL procs + O(1) PLT cache, 4096-slot O(1) mutex hash, `GUEST_MEM_SIZE` constant bounds checks (`-march=native`), `timeBeginPeriod(1)` + `EnumDisplaySettingsA` refresh detection; uncapped 3D race rose from `~38.3 FPS` to `175.9–266.5 FPS` (`535–952 FPS` menus), and paced 120 Hz VSync Nassau race held `120.0 FPS` (`EXIT=0`, `crash_log=False`) |
| `analysis/reports/dat-map.tsv` & `Asphalt 6 HD 3.7.0.apk` check | Hi-res texture audit (`*_hires.tga`) | `dat-map.tsv` holds `103` `*hires*` files (`84` `.bdae` car meshes + `19` loading/splash/unlock `.tga` files, including `Loading02_hires.tga`), and `Asphalt 6 HD 3.7.0.apk` holds `0` `*hires*` files; `MainMenu_hires.tga`, `MenuHud_hires.tga`, `CarSelect_new_hires.tga`, `Hud_hires.tga` do not ship in this Android build, so the engine probes `<name>_hires.tga` first and falls back to `<name>.tga` |
| `runtime/overrides.c`, `dispatch.c`, `vfs.c`, `tools/gen_hires_ui.py` | Forced HiRes textures & generated 2x `hires/` UI pack | Overrode `0x6a8a8c` (`Game::IsUsingLowResTexture`->0) and `0x50ca98`/`0x50cb3c` (`IsLowResPlatform/Screen`->0), keeping `0x444894` (`Game::UseHiResCarMeshType`) native so AI cars preserve `BaseCar::SetLOD(2)` exterior shells close-up; generated 29 2x standard type-2 `*_hires.tga` UI sheets into `hires/` via `tools/gen_hires_ui.py` and verified `Loaded texture: MainMenu_hires.tga`..`IgMenu_hires.tga` plus full-res opponent `*_body.tga` (`EXIT=0`) |
| `runtime/setup_win32.c`, `runtime/installer_win32.c`, `runtime/installer_gl.c`, `runtime/vfs.c`, `runtime/platform_win32.c`, `runtime/main_win32.c`, `tools/pack_setup.py`, `tools/build_win.py` | Self-contained release `Asphalt6_Setup.exe` (`28,156,034` B) & exe+`gamedata`-only install | `tools/pack_setup.py` builds portable `-march=x86-64 -mtune=generic` `build/release/asphalt6_win.exe` + embeds compressed engine & ANGLE DLLs (`A6PK`) inside `build/Asphalt6_Setup.exe`; zero build tools/internet needed at setup; relocates APK -> `guest.bin` + embeds `res/raw/*`, `gi_*.png`, ANGLE DLLs into single `<InstallDir>\asphalt6_win.exe` (saves & ANGLE cache in `%LOCALAPPDATA%\Asphalt6HD\`); isolated install test verified `['asphalt6_win.exe']` -> `"DOWNLOAD REQUIRED"` prompt -> `['asphalt6_win.exe', 'gamedata']` 60-frame boot `RC=0` |
