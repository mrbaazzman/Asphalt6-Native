# 01 — Target Dossier

Everything in this file is observed. Evidence ids and tool outputs are recorded in `docs/06-EVIDENCE.md`.

## 1. Package

| Field | Value |
|---|---|
| APK | `Asphalt 6 HD 3.7.0.apk` |
| Size | 9,621,003 bytes |
| SHA-256 | `70390c25fc65b3c85676ce320e3eacd8c12fd5d1d548587f76c2f90efb7ae60d` |
| Package | `com.gameloft.android.GAND.GloftAsp6.asphalt6` |
| Version | 3.7.0 (`versionCode` 370) |
| minSdk | 9 (Android 2.3) |
| targetSdk | not declared |
| Native ABI | `armeabi-v7a` (`libasphalt6.so` + `libStormGLOFT.so`) |
| DEX | 1 file (`classes.dex`, 147,428 bytes), 97 classes, 96 resources |

Permissions: `WRITE_EXTERNAL_STORAGE`, `INTERNET`, `ACCESS_WIFI_STATE`, `KILL_BACKGROUND_PROCESSES`,
`READ_PHONE_STATE`, `ACCESS_NETWORK_STATE`, `CHANGE_NETWORK_STATE`, `RESTART_PACKAGES`,
`CHANGE_WIFI_STATE`, `WAKE_LOCK`, `DEVICE_POWER`, `WRITE_SETTINGS`, `VIBRATE`.

### Activities (from manifest, REA `ev_a7b9a893…`)

| Activity | Role |
|---|---|
| `…asphalt6.SplashActivity` | MAIN / LAUNCHER entry |
| `…asphalt6.installer.GameInstaller` | expansion download / verify / install |
| `…asphalt6.MyVideoView` | intro movie playback |
| `…asphalt6.GLGame` | game Activity + `GameGLSurfaceView` |
| `…asphalt6.GLiveMain` | Gameloft Live web layer |

### Resource hints (`res/raw/*`)

- `res/raw/infoversion.txt`: `3.7.0`
- `res/raw/serialkey.txt`: `0qJUOTuwDiQ`
- `res/raw/data.txt`: `http://dl.gameloft.com/hdplus/android/asphalt6/se_zeus/ml/v362/packatc.zip`
  (Sony Ericsson Zeus / Xperia Play variant, ATC textures)
- `res/raw/gi_settings.xml`: `<atc_textures>1</atc_textures>` default, `<pvrt_textures>1</pvrt_textures>` for Samsung, `<dxt_textures>1</dxt_textures>` for Acer.

### Role of `libStormGLOFT.so`

`GLGame.<clinit>` calls `System.loadLibrary("asphalt6")` then `System.loadLibrary("StormGLOFT")`.
Direct inspection of `libStormGLOFT.so` (`825,616` bytes, SHA-256 `7673ee85c3c9f3eb8d043a6e1d0edc7948432bf04471e87b8f051e73e059c0dd`,
NDK r16 / stlport, `ev_c5c51e78…`) shows it is a **third-party Android compatibility shim** exporting
`hook_import_function`, `my_glCompressedTexImage2D`, `my_glTexImage2D`, `my_glShaderSource`,
`DecompressATC`/`ETC`/`PVRTC`/`S3TC`, and `glslopt_*`, targeting `/data/data/com.gameloft.android.GAND.GloftAsp6.asphalt6/lib/libasphalt6.so`.
`libasphalt6.so` itself has **no** dependency on or reference to `libStormGLOFT.so`. In our native PC
port, GL texture decompression / shader compatibility is handled directly in our `runtime/` GL layer.

## 2. Native library (`libasphalt6.so`)

| Field | Value |
|---|---|
| Path in APK | `lib/armeabi-v7a/libasphalt6.so` |
| Size | 22,103,415 bytes |
| SHA-256 | `4f4999c0099c3f55f007a0b00daf94f0982a2424a52cf20075a94bd855f99a40` |
| ELF | ELF32, little-endian, `ET_DYN` (`3`), `EM_ARM` (`40`) |
| SONAME | `libasphalt6.so` |
| Entry | `0x4233a0` (start of `.text`) |
| Arch | ARMv7-A (`Tag_CPU_name "7-A"`), `Tag_THUMB_ISA_use: Thumb-2`, `Tag_FP_arch: VFPv2` |
| Float ABI | **softfp** — `Tag_ABI_HardFP_use: Deprecated`, no `Tag_ABI_VFP_args` |
| Linkage | non-PIE (`ET_DYN` at base 0), NX yes, stack canary yes, no RELRO |
| Symbols | **Full `.symtab` preserved** (`140,272` entries, `29,810` functions); `.dynsym` has `29,369` entries (`26,638` functions) |
| Relocations | `216,412` (`R_ARM_RELATIVE`: `211,393`, `R_ARM_ABS32`: `4,716`, `R_ARM_JUMP_SLOT`: `289`, `R_ARM_GLOB_DAT`: `14`) |

### Measured inventory (Craft `ev_a1e39750…`, `ev_706f0bd9…`, `analysis/reports/elf-census.md`, `instruction-census.md`)

| Metric | Value |
|---|---|
| Functions in `.symtab` (defined) | 29,810 (29,795 non-zero size) |
| — ARM entry points (even) | 28,708 |
| — Thumb entry points (odd) | 1,102 (all in `0x8899d5..0x8a78dd`, STLport) |
| Instructions decoded (`.symtab`) | 1,545,201 (ARM: 1,496,208; Thumb-2: 48,993) |
| Mean / median instructions per function | 51 / 19 (max 2,409) |
| VFP instructions (`v*`) | 92,557 across 3,813 functions |
| Atomics (`ldrex/strex`) | 0 |
| `svc` / coprocessor | 2 / 0 |
| Bitfield ops (`ubfx/sbfx/bfi/bfc`) | 2,601 |
| `.ARM.exidx` rows | 29,693 (`237,544` bytes) |
| `.ARM.extab` size | 10,232 bytes |
| `.init_array` constructors | 392 (`1,568` bytes at `0xfe9c68`) |
| Imports | 301 undefined dynamic symbols (`289` PLT slots at `0x422610..0x42339c`) |

### Imported libraries (`DT_NEEDED`)

`libc.so`, `libstdc++.so`, `libm.so`, `libGLESv1_CM.so`, `libGLESv2.so`, `libdl.so`, `liblog.so`, `libOpenSLES.so`.

Note: `libGLESv1_CM.so` is referenced by only 5 `BL` sites at `0x42a8ec..0x42a958` (`glShadeModel`, `glMatrixMode`, `glLoadIdentity`). All scene/material rendering uses `libGLESv2.so` (`CGLSLShader`, `CProgrammableGLDriver`).

### Sections that matter to the recompiler

| Section | Addr | Size | Role |
|---|---|---|---|
| `.rel.plt` | `0x421cf4` | 2,312 | 289 PLT relocations |
| `.plt` | `0x4225fc` | 3,488 | PLT0 (20B) + 289 × 12B stubs (`0x422610..0x42339c`) |
| `.text` | `0x4233a0` | 7,465,384 | code |
| `.rodata` | `0xb41d48` | 4,630,336 | literals, strings |
| `.ARM.extab` | `0xfac488` | 10,232 | exception unwind tables |
| `.ARM.exidx` | `0xfaec80` | 237,544 | exception index (29,693 rows) |
| `.init_array` | `0xfe9c68` | 1,568 | 392 global constructors |
| `.data.rel.ro` | `0xfea288` | 302,216 | vtables / RTTI |
| `.got` | `0x1034008` | 10,656 | GOT |
| `.data` | `0x10369a8` | 724,224 | initialised globals |
| `.bss` | `0x10e76a8` | 516,640 | zero-initialised globals |

Runtime reservation: `LOAD0` `0x0–0xfe8c68` (16,682,088 B), `LOAD1` `0xfe9c68–0x11658c8` (`image_end = 0x11658c8` = 18,241,736 B).

## 3. Game data (`gamedata/`)

**Immutable (Rule R2):** `gamedata/` contains **843 files** (`analysis/data-manifest.sha256` records every SHA-256).

- `file000000.dat` (31,384 B): Glitch obfuscation map, key `0x4F`, trailer `c|-|3k`, **836 entries** (`file000001.dat` .. `file000836.dat` -> original filenames in `analysis/reports/dat-map.tsv`).
- `file00a.bin` (143,543,426 B): `QL` archive container (`51 4c 04 05 ...`).
- `file01c.bin` (444 B), `file01e.bin` (6,156 B)
- `intro.mp4` (23,622,770 B): intro movie
- `oconf.bar` (259 B): Gameloft Live / XPlayer configuration (`GGI: 25157`, `GameVer: 1.0.0`)
- `soundinfo.bin` (17,130 B): Vox sound descriptor table (`mem_gp_sfx_*`, etc.)

## 4. Identity of the engine

Confirmed from `.symtab` and `.rodata`:

| Layer | Identity | Evidence |
|---|---|---|
| Engine | **Glitch** (Irrlicht fork) | `glitch::scene`, `glitch::video`, `CProgrammableGLDriver<CGLSLShaderHandler>`, `"Found unknown element in irrlicht scene file"` |
| UI / Menus | **GameSWF** (`gameswf::`) | 2,115+ functions (`action_buffer`, `as_3_function`, `render_handler_glitch`, `.swf` assets) |
| Audio | **Vox** (`vox::`, `SoundManager`) | 897+ functions, OpenSL ES (`_InitOSL`) + AudioTrack (`_InitAT`) drivers |
| Physics / Gameplay | In-house (`PhysicCar`, `RaceCar`, `LogicCar`, `Camera`, `TrackScene`, `GS_Race`) | Full C++ symbols in `.symtab` |
| Font / Text | FreeType (`TT_RunIns`, `cff_decoder_parse_charstrings`) + `StringManager` | Full symbols in `.symtab` |
| Support | STLport (`std::std::priv`, Thumb-2), Boost (`intrusive_ptr`), GNU/ARM EABI | Full symbols in `.symtab` |
