# 02 — Architecture

How Asphalt 6 HD 3.7.0 is put together, from observed DEX and ELF disassembly.

## 1. Layers

```
┌──────────────────────────────────────────────────────────────────────┐
│ Java (classes.dex, 97 classes)                                       │
│  SplashActivity ─ GameInstaller ─ GLGame (Activity)                  │
│  GameGLSurfaceView ─ GameRenderer ─ GLResLoader ─ GLMediaPlayer      │
│  GLUtils.Device   GLiveMain   MyVideoView                            │
└───────────────────────────────┬──────────────────────────────────────┘
                                │ JNI (System.loadLibrary("asphalt6"))
┌───────────────────────────────▼──────────────────────────────────────┐
│ libasphalt6.so — 22.1 MB, ARMv7-A (ARM + STLport Thumb-2), softfp    │
│                                                                      │
│  glitch::   Irrlicht-derived 3D engine (scene, collada, io, video)   │
│  gameswf::  Flash/SWF UI engine + render_handler_glitch              │
│  vox::      Audio engine (OpenSL ES _InitOSL + AudioTrack _InitAT)   │
│  game:      Application, Game, DeviceConfig, PhysicCar, RaceCar,     │
│             TrackScene, PostEffects, GS_*, NetworkManager,           │
│             ALicenseCheck                                            │
└───────────────────────────────┬──────────────────────────────────────┘
                                │ GLES2 (+5 GLES1 init calls) + OpenSL ES + libc/libm
┌───────────────────────────────▼──────────────────────────────────────┐
│ Android platform                                                     │
└──────────────────────────────────────────────────────────────────────┘
```

## 2. Boot sequence (confirmed)

### Java side (`classes.dex`, REA `ev_938cc2d5…`, `ev_cbf7bc6e…`, `ev_14b2fb3b…`, `ev_c4319065…`)

1. `SplashActivity.onCreate`: sleeps 5s, starts `GameInstaller`, finishes.
2. `GameInstaller`: verifies/installs data under `/sdcard/games/com.gameloft.android.GAND.GloftAsp6.asphalt6/`.
3. `GLGame.<clinit>`: loads `libasphalt6.so` (`JNI_OnLoad`) and `libStormGLOFT.so`.
4. `GLGame.onCreate`: creates `GameGLSurfaceView`, sets locale language code (`GLGame.d`, 1=eng, 2=fra, ...), initializes `GLUtils.Device.init()`.
5. `GameRenderer.onSurfaceCreated`:
   - `GLResLoader.init()` → `GLResLoader.nativeInit()`
   - `GLMediaPlayer.init()` → `GLMediaPlayer.nativeInit()` (`0x424300`)
   - `GLGame.nativeInit()`
   - `GLGame.nativeKeyboardEnabled(GLGame.a, false)`
   - sets `GameRenderer.b = true`.
6. `GameRenderer.onDrawFrame`:
   - On first frame (`b == true`): clears `b = false`, calls `nativeInit(width, height, GLGame.d)` (`appInit`), swaps buffers.
   - On every subsequent frame: calls `nativeRender()` (`appRender`).

### Native side (`libasphalt6.so`)

1. **`JNI_OnLoad` (`0x42a6b4`)**:
   - `NVThreadInit(vm)` (`0x42725c`)
   - `vm->GetEnv(&env, 0x10004)` (`JNI_VERSION_1_4`)
   - `mWaitForInit = 3`
   - `VoxSetJavaVM(vm)` (`0x8cd618`)
   - `VoxSetAndroidAPILevel(9)` (`0x8cd638`) — selects OpenSL ES (`_InitOSL`) in `vox::DriverAndroid::Init` (`0x8ce488`) since `apiLevel > 8`.
2. **`appInit(w, h, lang)` (`0x42a9bc`)**:
   - Calls `nativeGetVersion()` (`0x424118`).
   - Sets `g_appPath` = `"/sdcard/games/com.gameloft.android.GAND.GloftAsp6.asphalt6/"`
   - Sets `g_saveDirPath` = `"/data/data/com.gameloft.android.GAND.GloftAsp6.asphalt6/"`
   - Calls `GFX_InitializeAndroid()` (`0x42a8e0`, stub `bx lr`).
   - Calls `Application::CreateInstance(g_appPath, 1, 0, w, h, ...)` (`0x432054`).
3. **`Application::CreateInstance` (`0x432054`)**:
   - Defaults to `854 × 480` (`0x356 × 0x1e0`) overridden by `SCR_WIDTH` / `SCR_HEIGHT`.
   - If `width < 512`, halves `aFlashFileData` entries > 511 (22 entries).
   - Allocates `Application` (`0x64` bytes), calls `Application::Init` (`0x4326b4`).
4. **`Application::Init` (`0x4326b4`)**:
   - `Game::CreateGame()` (`0x439e70`)
   - `Application::SetResourcePath(path)` (`0x431c2c`)
   - `CCpuTaskHandler::init(1)` (`0xa7f450`)
   - `setProcessBufferHeapSize(0x80000)` (`0x911fec`)
   - `SetUIOrientation(0, 1)` (`0x433ac8`)
   - `DeviceConfig::SetupGameForDevice()` (`0x4d014c`)
   - `Game::Init()` (`0x43aae8`)
   - `ProfileManager` load + language detection (`GetDeviceLanguage`)
   - `StringManager::RefreshPacks(1)` (`0x65470c`)
5. **`Game::Init` (`0x43aae8`)**:
   - `sMenuData::MenuDataInit()` (`0x6631e0`)
   - `IFileSystem::changeWorkingDirectoryTo(resourcePath)`
   - `IFileSystem::addObfuscationFileMap("file000000.dat", 0x4f)` (`0x957ad0`)
   - `CTextureManager::setFlag(1, false)`
   - `T_SWFManager::SWFInit()` (`0x606304`), `StringManager::InitStrings(1)` (`0x655310`)
   - Loops `0..5` calling `IFileSystem::addZipFileArchive` for:
     `trackTextures.zip`, `carTextures.zip`, `trafficTextures.zip`, `objectsTextures.zip`, `sceneTextures.zip`, `particleTextures.zip`.
   - Initializes `CarManager`, `TrafficManager`, `EventManager`, `CareerManager`, `TrackManager`, `SponsorMng`, `TuningManager`.
6. **`appRender(0, 0, 0)` (`0x42acd0`)**:
   - Handles pause/exit flags, `mWaitForInit` / `m_timerForResume` countdowns (`ResumeGame` at `0x433550`), `Application::Update()` (`0x432944`), and accelerometer/orientation reads.

## 3. Subsystems and port boundaries

| Subsystem | Observed in `libasphalt6.so` | Port strategy |
|---|---|---|
| Window & GL | GLES2 (`libGLESv2.so`) via `CProgrammableGLDriver<CGLSLShaderHandler>` (plus 5 dead/init GLES1 calls) | Win32 window + EGL/GLES2 via ANGLE (D3D11) |
| Textures | `packatc.zip` hint; `glCompressedTexImage2D` + Glitch image loaders | Native GLES2 upload or ATC/PVRTC/ETC decode in `runtime/gl_win32.c` |
| Audio | `vox::DriverAndroid` (`_InitOSL` via `libOpenSLES.so` if API > 8, else `_InitAT`) | Implement OpenSL ES buffer-queue sink over Win32 `waveOut` (or route `_InitAT`/`_InitOSL` to `waveOut` pump thread) |
| VFS / Data | Plain `fopen`/`open` + `file000000.dat` (`0x4f`) + `file00a.bin` (`QL`) | `runtime/vfs.c`: read-only mapping into `gamedata/`, writes to `build/userdata/` |
| UI | `gameswf` (`T_SWFManager`) + 2D sprites | Translated in full; host `resolution.c` maps design space to window |
| Threads | `pthread_*` (28 imports) | `runtime/thread_win32.c` over Win32 threads/critical sections/TLS |
| Exceptions | `.ARM.exidx` (29,693 rows) + `__cxa_*` | Runtime unwinder / stubbed `__cxa_*` as required |
| STLport | 1,102 Thumb-2 functions (`0x8899d5..0x8a78dd`) | Translated from Ghidra P-code cleanly alongside ARM code |
