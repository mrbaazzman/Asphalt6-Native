# 05 — Live State

**Read this first when resuming.** Update at the end of every work block (Rule R5).

## Current Status: M0 -> M7 Complete (Playable Native Windows Port)

### What was done
- Implemented complete native Windows x86-64 static recompilation port of **Asphalt 6 HD 3.7.0** (`libasphalt6.so`, 30,066 translated functions across 151 chunks -> `build/asphalt6_win.exe`).
- Verified `gamedata/` remains completely unmodified (`843/843` SHA-256 hashes match `analysis/data-manifest.sha256`); all runtime writes redirect to `build/userdata/` (`profile1.sav`, `profile1.bak`, `profileInit.sav`).
- Fixed ARM AAPCS varargs/double alignment (`arg_u64` in `runtime/imports.c`), resolving `snprintf("%.14g", ...)` corruption in `gameswf::as_value::to_tu_string` (`0x788e10`) that had broken Career league/event unlock state, HUD/stat strings, and `CarSelect` frame selection.
- Fixed `T_TextureHelper::CalcOverlayPixel` (`0x51fb24`) in `runtime/overrides.c` to preserve paint luminance and swizzle BGRA TGA input pixels into RGBA (`GL_RGBA`), fixing both the electric-blue MINI Cooper paint and red taillights/brake lights (Nissan Nismo 370Z and other cars).
- Fixed two mid-race crash root causes:
  - Updated `FN_VOX_FILL_BUFFER` in `runtime/overrides.c` from unlocked `0x8d170c` (`_FillBuffer`) to mutex-locked `0x8d1850` (`vox::DriverCallbackInterface::FillBuffer`), eliminating voice-list races (`0x97b5ac50`) on the WinMM audio thread.
  - Fixed `div` (`0x422af0`) in `runtime/imports.c` for Android GCC EABI hidden `sret` struct return (`r0=&div_t`, `r1=num`, `r2=den`), eliminating `__stack_chk_fail` in `fn_53a8c0` timer/split `sprintf`.
  - Added `CRITICAL_SECTION` / TLS thread-safety guards across `runtime/vfs.c`, `runtime/java_bridge.c`, `runtime/imports.c`, and `runtime/main_win32.c`, expanded `MAX_GUEST_MUTEXES` to `2048` in `runtime/thread_win32.c`, and added `build/asphalt6_crash.log` SEH/`guest_fault`/`__stack_chk_fail` diagnostics.
- Overhauled controls & frame pacing in `runtime/platform_win32.c`, `runtime/main_win32.c`, and `runtime/gl_win32.c`:
  - Added `eglSwapInterval(1)` + QPC 60 FPS frame cap (`ASPHALT6_NO_CAP=1` to uncap).
  - Dynamically loaded `xinput1_4.dll` / `xinput9_1_0.dll` with multi-slot XInput (`0..3`) polling, clean disconnect key-release handling, independent keyboard/D-pad/Left-Stick key tracking (`queue_key_src`), and dominant-axis thumbstick Y guard for Xbox controllers.
  - Mapped `W/Up`->`0x67`, `S/Down`->`0x6c`, `A/Left`->`0x69`, `D/Right`->`0x6a`, `Enter`->`0x130`, `Space/Shift`->`0x137`, `Esc/Backspace`->`4`, suppressed Windows key auto-repeat, fixed inverted `in_race` (`FN_IS_RACE_MENU` == `0`), rotated menu D-pad events 90° CW (`0x67->0x6a, 0x6c->0x69, 0x69->0x67, 0x6a->0x6c`) with paired `KEY_UP` tracking, and aligned in-race action flags (`0x1` Gas, `0x2` Brake/Reverse, `0x4` Steer Left, `0x8` Steer Right, `0x200` Nitro) plus smooth tilt steering across control schemes.
- Eliminated the 25–30 FPS gameplay bottleneck and enabled native monitor-refresh rendering (verified locked at `120.0 FPS` on 120 Hz display):
  - Moved `call_target`/`call_indirect` out of `-O0` `build/link/table.c` into `-O3` `runtime/dispatch.c` using a 2-level O(1) page directory (`g_fn_dir[4096]`, 8192 slots/page), direct override wrappers for the 8 hooked functions, and no per-call MinGW `emutls` ring overhead.
  - Pre-resolved all ANGLE `libGLESv2.dll` entry points once in `runtime/gl_win32.c` (`init_gl_procs` + `gl_bridge_lookup`) and added O(1) PLT handler caching (`g_plt_fn[IMPORT_COUNT]`) in `runtime/imports.c`, including math (`sinf`, `cosf`, `fmin`, `fmax`, etc.), string/heap, `pthread_self`, and GL calls.
  - Replaced the 2048-entry linear mutex scan in `runtime/thread_win32.c` with a 4096-slot O(1) open-addressed hash table (`InitializeCriticalSectionAndSpinCount(..., 4000)`).
  - Constant-folded guest bounds checks in `runtime/mem.h` (`GUEST_MEM_SIZE = 0x20000000u`) and enabled `-march=native` (`-O2` for translated chunks, `-O3` for hot runtime units) in `tools/build_win.py`.
  - Updated `runtime/platform_win32.c` to default to native desktop borderless fullscreen (`1920x1080` on 1080p displays, with `ASPHALT6_WINDOWED=1` / `Alt+Enter` toggle), detect monitor refresh rate via `EnumDisplaySettingsA` (`ASPHALT6_FPS` override), enable `timeBeginPeriod(1)` / `timeEndPeriod(1)`, and lower `g_frame_dt` floor to `0.001`.
- Forced full-res car textures while preserving AI close-up car geometry (`runtime/overrides.c`, `runtime/dispatch.c`, `runtime/vfs.c`):
  - Overrode `Game::IsUsingLowResTexture()` (`0x6a8a8c`) -> `0` and `DeviceConfig::IsLowResPlatform()` / `IsLowResScreen()` (`0x50ca98`/`0x50cb3c`) -> `0` so player and opponent cars load full-res `*_Body.tga` textures; kept `Game::UseHiResCarMeshType()` (`0x444894`) native so AI opponents retain `BaseCar::SetLOD(2)` and keep solid painted exterior shells at close range.
  - Added read-only `hires/` override lookup in `runtime/vfs.c` (checked before `gamedata/`) and `tools/gen_hires_ui.py`, generating 29 2x Lanczos+sharpened standard type-2 `*_hires.tga` UI sheets into `hires/` (`MainMenu_hires.tga`, `MenuHud_hires.tga`, `GLLive_hires.tga`, `company_logoss_hires.tga`, `CarSelect_new_hires.tga`, `Career_hires.tga`, `CarSelect_hires.tga`, `A6_multiplayerB_hires.tga`, `ZeusE_hires.tga`, `ZeusF_hires.tga`, `option_texture_hires.tga`, `IgMenu_hires.tga`, etc.).
- Verified clean full rebuild (`python -u tools/bootstrap.py --jobs 8`, `gamedata manifest: ok=843 bad=0`), uncapped benchmark (`535–952 FPS` menus, `175.9–266.5 FPS` 3D race), paced 120 Hz VSync Nassau 3D race (`120.0 FPS` / `119.5 FPS`, `glCompressedTexImage2D=362`, `EXIT=0`, `crash_log=False`), and verified all `*_hires.tga` UI sheets load without `Could not find` errors (`Loaded texture: MainMenu_hires.tga`, etc.) across clean menu/race screenshots, then cleaned temporary `.ppm`/`.log` artifacts in `build/`.
- Built self-contained single-file GitHub Release installer (`tools/pack_setup.py` + `runtime/setup_win32.c` -> `build/Asphalt6_Setup.exe`, `28,156,034` bytes, offline & zero build tools required):
  - `tools/pack_setup.py` compiles baseline portable `-march=x86-64 -mtune=generic` `build/release/asphalt6_win.exe` (guaranteed compatible across all x86-64 CPUs) and embeds compressed `asphalt6_win.exe` + `libEGL.dll` + `libGLESv2.dll` + `d3dcompiler_47.dll` into `Asphalt6_Setup.exe` via an `A6PK` payload trailer.
  - User selects their 3.7.0 APK + install path and clicks **Install**: `Asphalt6_Setup.exe` extracts `res/raw/*` + `gi_*.png` and relocates `libasphalt6.so` -> `guest.bin` in memory, embedding everything (`guest.bin`, `res/raw/*`, `gi_*.png`, ANGLE DLLs) inside `<InstallDir>\asphalt6_win.exe`. Saves & ANGLE runtime cache cleanly to `%LOCALAPPDATA%\Asphalt6HD\`, leaving `<InstallDir>` with strictly `asphalt6_win.exe` and `gamedata/` only.
  - Verified isolated clean release install outside the repo: `Asphalt6_Setup.exe` produced `['asphalt6_win.exe']`, launch without `gamedata/` rendered the in-game Gameloft CDN prompt (`460.5 MB`), and launch with `gamedata/` ran 60 frames cleanly (`RC=0`, `['asphalt6_win.exe', 'gamedata']`).

### Build & Run
- Pack standalone GitHub Release Setup: `python -u tools/pack_setup.py --jobs 8`
- Run GUI Setup: `.\build\Asphalt6_Setup.exe`
- Developer incremental build: `python -u tools/bootstrap.py --jobs 8`
- Launch dev build: `.\build\asphalt6_win.exe`
