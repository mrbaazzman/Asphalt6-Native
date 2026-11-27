/*
 * Win32 window + ANGLE EGL host surface + keyboard/mouse input queue for Asphalt 6 HD.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "installer.h"

typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef int   EGLint;
typedef unsigned int EGLenum;
typedef unsigned int EGLBoolean;

#define EGL_DEFAULT_DISPLAY    ((void *)0)
#define EGL_OPENGL_ES_API      0x30A0u
#define EGL_SURFACE_TYPE       0x3033
#define EGL_WINDOW_BIT         0x0004
#define EGL_RENDERABLE_TYPE    0x3040
#define EGL_OPENGL_ES2_BIT     0x0004
#define EGL_RED_SIZE           0x3024
#define EGL_GREEN_SIZE         0x3023
#define EGL_BLUE_SIZE          0x3022
#define EGL_ALPHA_SIZE         0x3021
#define EGL_DEPTH_SIZE         0x3025
#define EGL_STENCIL_SIZE       0x3026
#define EGL_WIDTH              0x3057
#define EGL_HEIGHT             0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_NONE               0x3038

typedef EGLDisplay (__stdcall *PFN_eglGetDisplay)(void *);
typedef EGLBoolean (__stdcall *PFN_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
typedef EGLBoolean (__stdcall *PFN_eglBindAPI)(EGLenum);
typedef EGLBoolean (__stdcall *PFN_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
typedef EGLSurface (__stdcall *PFN_eglCreateWindowSurface)(EGLDisplay, EGLConfig, HWND, const EGLint *);
typedef EGLBoolean (__stdcall *PFN_eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
typedef EGLContext (__stdcall *PFN_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
typedef EGLBoolean (__stdcall *PFN_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
typedef EGLBoolean (__stdcall *PFN_eglSwapBuffers)(EGLDisplay, EGLSurface);
typedef EGLBoolean (__stdcall *PFN_eglSwapInterval)(EGLDisplay, EGLint);
typedef EGLBoolean (__stdcall *PFN_eglTerminate)(EGLDisplay);
typedef void *     (__stdcall *PFN_eglGetProcAddress)(const char *);

/* Minimal XInput structures (loaded dynamically from xinput1_4.dll / xinput9_1_0.dll) */
typedef struct {
    WORD  wButtons;
    BYTE  bLeftTrigger;
    BYTE  bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XINPUT_GAMEPAD_A6;

typedef struct {
    DWORD             dwPacketNumber;
    XINPUT_GAMEPAD_A6 Gamepad;
} XINPUT_STATE_A6;

typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE_A6 *);

#define XGP_DPAD_UP    0x0001
#define XGP_DPAD_DOWN  0x0002
#define XGP_DPAD_LEFT  0x0004
#define XGP_DPAD_RIGHT 0x0008
#define XGP_START      0x0010
#define XGP_BACK       0x0020
#define XGP_RB         0x0200
#define XGP_A          0x1000
#define XGP_B          0x2000
#define XGP_X          0x4000

static HMODULE g_egl_module = NULL;
static HMODULE g_xinput_module = NULL;
static PFN_XInputGetState g_xinput_get_state = NULL;
static HWND g_hwnd = NULL;
static EGLDisplay g_display = NULL;
static EGLSurface g_surface = NULL;
static EGLContext g_context = NULL;
static PFN_eglSwapBuffers g_swap = NULL;
static PFN_eglSwapInterval g_swap_interval = NULL;
static PFN_eglTerminate g_terminate = NULL;
static PFN_eglGetProcAddress g_get_proc = NULL;

static LARGE_INTEGER g_qpc_freq;
static LARGE_INTEGER g_last_frame_qpc;
static int g_qpc_ready = 0;
static int g_timer_period_set = 0;
static int g_no_cap = 0;
static double g_target_fps = 60.0;
static double g_frame_dt = 1.0 / 60.0;

static float g_gp_steer = 0.0f;
static int g_gp_gas = 0;
static int g_gp_brake = 0;
static int g_gp_nitro = 0;
static int g_gp_b = 0;
static WORD g_gp_prev_btns = 0;
static int g_gp_prev_ls_dir = 0; /* -1 left, +1 right, 0 center */
static int g_gp_prev_ls_ud = 0;  /* -1 up, +1 down, 0 center */

static int g_width = 1920;
static int g_height = 1080;
static int g_design_w = 1920;
static int g_design_h = 1080;
static int g_is_fullscreen = 1;
static volatile int g_quit = 0;
static int g_key = 0;
static int g_mouse_down = 0;

#define KEY_QUEUE_SIZE 128
#define KEY_STATE_MAX  512
#define POINTER_QUEUE_SIZE 128

static struct { int code, down; } g_key_queue[KEY_QUEUE_SIZE];
static int g_key_head = 0, g_key_tail = 0;
static uint8_t g_key_down[KEY_STATE_MAX];
static uint8_t g_key_src_mask[KEY_STATE_MAX]; /* bit0=kbd/inj, bit1=gp_dpad, bit2=gp_stick */

static struct { int x, y, down; } g_pointer_queue[POINTER_QUEUE_SIZE];
static int g_pointer_head = 0, g_pointer_tail = 0;

static void queue_key_src(int android_code, int down, int src_bit) {
    if (android_code <= 0 || android_code >= KEY_STATE_MAX) return;
    uint8_t was = g_key_src_mask[android_code];
    if (down) g_key_src_mask[android_code] = (uint8_t)(was | (1u << src_bit));
    else      g_key_src_mask[android_code] = (uint8_t)(was & ~(1u << src_bit));
    int now_down = (g_key_src_mask[android_code] != 0);
    g_key_down[android_code] = (uint8_t)(now_down ? 1 : 0);
    if (now_down != (was != 0)) {
        int nxt = (g_key_tail + 1) % KEY_QUEUE_SIZE;
        if (nxt != g_key_head) {
            g_key_queue[g_key_tail].code = android_code;
            g_key_queue[g_key_tail].down = now_down;
            g_key_tail = nxt;
        }
    }
}

static void queue_key(int android_code, int down) {
    queue_key_src(android_code, down, 0);
}

static void queue_pointer(int px, int py, int down) {
    int lx = g_width > 0 ? (px * g_design_w) / g_width : px;
    int ly = g_height > 0 ? (py * g_design_h) / g_height : py;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx >= g_design_w) lx = g_design_w - 1;
    if (ly >= g_design_h) ly = g_design_h - 1;
    int nxt = (g_pointer_tail + 1) % POINTER_QUEUE_SIZE;
    if (nxt != g_pointer_head) {
        g_pointer_queue[g_pointer_tail].x = lx;
        g_pointer_queue[g_pointer_tail].y = ly;
        g_pointer_queue[g_pointer_tail].down = down;
        g_pointer_tail = nxt;
    }
}

/* Map Windows VK_* to Linux/Gameloft keycodes used by notifyKeyPressed (0x42b5d8):
 *   0x67 (103) = UP     (Menu Up / Manual Accelerate)
 *   0x6c (108) = DOWN   (Menu Down / Brake & Drift)
 *   0x69 (105) = LEFT   (Menu Prev / Steer Left)
 *   0x6a (106) = RIGHT  (Menu Next / Steer Right)
 *   0x130(304) = BTN_A  (Enter / UI Confirm)
 *   0x137(311) = BTN_TR (Space / Shift -> Nitro)
 *   4          = Android BACK (Esc / Backspace -> Pause / Back)
 */
static int vk_to_android(WPARAM vk) {
    switch (vk) {
    case VK_UP:
    case 'W':        return 0x67;
    case VK_DOWN:
    case 'S':        return 0x6c;
    case VK_LEFT:
    case 'A':        return 0x69;
    case VK_RIGHT:
    case 'D':        return 0x6a;
    case VK_RETURN:  return 0x130;
    case VK_SPACE:
    case VK_SHIFT:   return 0x137;
    case VK_ESCAPE:
    case VK_BACK:    return 4;
    default:         return 0;
    }
}

static void toggle_fullscreen(void) {
    if (!g_hwnd) return;
    g_is_fullscreen = !g_is_fullscreen;
    int mon_w = GetSystemMetrics(SM_CXSCREEN);
    int mon_h = GetSystemMetrics(SM_CYSCREEN);
    if (g_is_fullscreen && mon_w > 0 && mon_h > 0) {
        SetWindowLongPtrA(g_hwnd, GWL_STYLE, WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, 0, 0, mon_w, mon_h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VISIBLE;
        int w = (mon_w > 1280) ? 1280 : (mon_w > 0 ? mon_w : 1280);
        int h = (mon_h > 720) ? 720 : (mon_h > 0 ? mon_h : 720);
        RECT rc = {0, 0, w, h};
        AdjustWindowRectEx(&rc, style, FALSE, WS_EX_APPWINDOW);
        int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        int wx = (mon_w - ww) / 2; if (wx < 0) wx = 0;
        int wy = (mon_h - wh) / 2; if (wy < 0) wy = 0;
        SetWindowLongPtrA(g_hwnd, GWL_STYLE, (LONG_PTR)style);
        SetWindowPos(g_hwnd, HWND_TOP, wx, wy, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

static LRESULT CALLBACK platform_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
    case WM_DESTROY:
        g_quit = 1;
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (LOWORD(lp) > 0 && HIWORD(lp) > 0) {
            g_width = (int)LOWORD(lp);
            g_height = (int)HIWORD(lp);
        }
        return 0;
    case WM_KILLFOCUS:
        for (int i = 1; i < KEY_STATE_MAX; i++) {
            if (g_key_src_mask[i] & 1u) queue_key_src(i, 0, 0);
        }
        if (g_mouse_down) {
            g_mouse_down = 0;
            ReleaseCapture();
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if ((lp & 0x40000000L) != 0) return 0; /* ignore auto-repeat */
        if (wp == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000)) {
            toggle_fullscreen();
            return 0;
        }
        if (wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) { g_quit = 1; return 0; }
        int ac = vk_to_android(wp);
        if (ac) queue_key(ac, 1);
        g_key = (int)wp;
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int ac = vk_to_android(wp);
        if (ac) queue_key(ac, 0);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        g_mouse_down = 1;
        queue_pointer((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), 1);
        return 0;
    case WM_MOUSEMOVE:
        if (g_mouse_down) {
            queue_pointer((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), 2);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_mouse_down) {
            g_mouse_down = 0;
            ReleaseCapture();
            queue_pointer((int)(short)LOWORD(lp), (int)(short)HIWORD(lp), 0);
        }
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

static HMODULE load_angle(void) {
    char exe_dir[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, exe_dir, MAX_PATH) > 0) {
        char *sl = strrchr(exe_dir, '\\');
        if (!sl) sl = strrchr(exe_dir, '/');
        if (sl) {
            *sl = '\0';
            char probe[MAX_PATH + 32];
            snprintf(probe, sizeof(probe), "%s\\libEGL.dll", exe_dir);
            if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
                SetDllDirectoryA(exe_dir);
                HMODULE m = LoadLibraryA(probe);
                if (m) {
                    fprintf(stderr, "platform: ANGLE loaded from %s\n", exe_dir);
                    return m;
                }
            }
        }
    }
    if (a6pak_has_self()) {
        char pak_ang[MAX_PATH] = {0};
        if (a6pak_extract_angle_self(pak_ang, sizeof(pak_ang))) {
            char probe[MAX_PATH + 32];
            snprintf(probe, sizeof(probe), "%s\\libEGL.dll", pak_ang);
            SetDllDirectoryA(pak_ang);
            HMODULE m = LoadLibraryA(probe);
            if (m) {
                fprintf(stderr, "platform: ANGLE loaded from embedded A6PAK cache (%s)\n", pak_ang);
                return m;
            }
        }
    }
    char local_cc[512] = {0};
    const char *local_env = getenv("LOCALAPPDATA");
    if (local_env && *local_env) {
        snprintf(local_cc, sizeof(local_cc), "%s\\Programs\\Command Code", local_env);
    }
    const char *dirs[4] = {
        ".",
        "build",
        "third_party/angle",
        local_cc[0] ? local_cc : NULL,
    };
    const char *env = getenv("ASPHALT6_ANGLE_DIR");
    if (env && *env) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s\\libEGL.dll", env);
        SetDllDirectoryA(env);
        HMODULE m = LoadLibraryA(buf);
        if (m) {
            fprintf(stderr, "platform: ANGLE loaded from %s\n", env);
            return m;
        }
    }
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (!dirs[i]) continue;
        char buf[512];
        snprintf(buf, sizeof(buf), "%s\\libEGL.dll", dirs[i]);
        DWORD attr = GetFileAttributesA(buf);
        if (attr != INVALID_FILE_ATTRIBUTES) {
            SetDllDirectoryA(dirs[i]);
            HMODULE m = LoadLibraryA(buf);
            if (m) {
                fprintf(stderr, "platform: ANGLE loaded from %s\n", dirs[i]);
                return m;
            }
        }
    }
    fprintf(stderr, "platform: libEGL.dll not found\n");
    return NULL;
}

static int host_dimension(const char *name, int fallback) {
    const char *v = getenv(name);
    if (v && *v) {
        long n = strtol(v, NULL, 10);
        if (n >= 320 && n <= 7680) return (int)n;
    }
    return fallback;
}

static double detect_monitor_refresh_hz(void) {
    const char *env_fps = getenv("ASPHALT6_FPS");
    if (env_fps && *env_fps) {
        double req = strtod(env_fps, NULL);
        if (req >= 15.0 && req <= 1000.0) return req;
    }
    DEVMODEA dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmDisplayFrequency > 1 && dm.dmDisplayFrequency <= 1000) {
        return (double)dm.dmDisplayFrequency;
    }
    return 60.0;
}

int platform_init(int width, int height, const char *title) {
    SetProcessDPIAware();
    if (timeBeginPeriod(1) == TIMERR_NOERROR) g_timer_period_set = 1;
    g_no_cap = getenv("ASPHALT6_NO_CAP") != NULL;
    g_target_fps = detect_monitor_refresh_hz();
    g_frame_dt = 1.0 / g_target_fps;
    const char *env_w = getenv("ASPHALT6_WIDTH");
    const char *env_h = getenv("ASPHALT6_HEIGHT");
    int windowed_req = getenv("ASPHALT6_WINDOWED") != NULL;
    int borderless_req = getenv("ASPHALT6_BORDERLESS") != NULL;
    int mon_w = GetSystemMetrics(SM_CXSCREEN);
    int mon_h = GetSystemMetrics(SM_CYSCREEN);
    int borderless = borderless_req || (!windowed_req && !env_w && !env_h);
    g_is_fullscreen = borderless;
    int want_w = host_dimension("ASPHALT6_WIDTH", borderless && mon_w > 0 ? mon_w : width);
    int want_h = host_dimension("ASPHALT6_HEIGHT", borderless && mon_h > 0 ? mon_h : height);
    if (mon_w > 0 && want_w > mon_w) want_w = mon_w;
    if (mon_h > 0 && want_h > mon_h) want_h = mon_h;
    g_width = want_w;
    g_height = want_h;

    g_egl_module = load_angle();
    if (!g_egl_module) return 0;

    PFN_eglGetDisplay get_display = (PFN_eglGetDisplay)GetProcAddress(g_egl_module, "eglGetDisplay");
    PFN_eglInitialize initialize = (PFN_eglInitialize)GetProcAddress(g_egl_module, "eglInitialize");
    PFN_eglBindAPI bind_api = (PFN_eglBindAPI)GetProcAddress(g_egl_module, "eglBindAPI");
    PFN_eglChooseConfig choose_config = (PFN_eglChooseConfig)GetProcAddress(g_egl_module, "eglChooseConfig");
    PFN_eglCreateWindowSurface create_surface = (PFN_eglCreateWindowSurface)GetProcAddress(g_egl_module, "eglCreateWindowSurface");
    PFN_eglQuerySurface query_surface = (PFN_eglQuerySurface)GetProcAddress(g_egl_module, "eglQuerySurface");
    PFN_eglCreateContext create_context = (PFN_eglCreateContext)GetProcAddress(g_egl_module, "eglCreateContext");
    PFN_eglMakeCurrent make_current = (PFN_eglMakeCurrent)GetProcAddress(g_egl_module, "eglMakeCurrent");
    g_swap = (PFN_eglSwapBuffers)GetProcAddress(g_egl_module, "eglSwapBuffers");
    g_swap_interval = (PFN_eglSwapInterval)GetProcAddress(g_egl_module, "eglSwapInterval");
    g_terminate = (PFN_eglTerminate)GetProcAddress(g_egl_module, "eglTerminate");
    g_get_proc = (PFN_eglGetProcAddress)GetProcAddress(g_egl_module, "eglGetProcAddress");
    if (!get_display || !initialize || !bind_api || !choose_config || !create_surface ||
        !create_context || !make_current || !g_swap || !g_get_proc) {
        fprintf(stderr, "platform: missing EGL entry points\n");
        return 0;
    }

    RECT frame = {0, 0, g_width, g_height};
    DWORD style = borderless ? (WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN)
                             : (WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    if (!borderless) AdjustWindowRectEx(&frame, style, FALSE, WS_EX_APPWINDOW);
    int win_w = frame.right - frame.left;
    int win_h = frame.bottom - frame.top;
    int win_x = (mon_w - win_w) / 2; if (win_x < 0) win_x = 0;
    int win_y = (mon_h - win_h) / 2; if (win_y < 0) win_y = 0;

    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = platform_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "Asphalt6HD";
    if (!RegisterClassExA(&wc)) return 0;

    g_hwnd = CreateWindowExA(WS_EX_APPWINDOW, wc.lpszClassName, title ? title : "Asphalt 6 HD",
                             style | WS_VISIBLE, win_x, win_y, win_w, win_h,
                             NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) return 0;

    if (getenv("ASPHALT6_TEST_MINIMIZED")) {
        ShowWindow(g_hwnd, SW_HIDE);
    } else {
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
        UpdateWindow(g_hwnd);
    }

    RECT cr;
    if (GetClientRect(g_hwnd, &cr) && cr.right > 0 && cr.bottom > 0) {
        g_width = cr.right;
        g_height = cr.bottom;
    }

    g_display = get_display(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (!g_display || !initialize(g_display, &major, &minor)) return 0;
    if (!bind_api(EGL_OPENGL_ES_API)) return 0;

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg = NULL;
    EGLint ncfg = 0;
    if (!choose_config(g_display, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) return 0;
    g_surface = create_surface(g_display, cfg, g_hwnd, NULL);
    if (!g_surface) return 0;
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_context = create_context(g_display, cfg, NULL, ctx_attr);
    if (!g_context) return 0;
    if (!make_current(g_display, g_surface, g_surface, g_context)) return 0;
    if (g_swap_interval) g_swap_interval(g_display, g_no_cap ? 0 : 1);
    if (query_surface) {
        EGLint sw = 0, sh = 0;
        query_surface(g_display, g_surface, EGL_WIDTH, &sw);
        query_surface(g_display, g_surface, EGL_HEIGHT, &sh);
        fprintf(stderr, "platform: EGL %d.%d surface %dx%d target=%.1fHz (vsync=%d)\n",
                major, minor, (int)sw, (int)sh, g_target_fps, g_no_cap ? 0 : 1);
    }
    QueryPerformanceFrequency(&g_qpc_freq);
    QueryPerformanceCounter(&g_last_frame_qpc);
    g_qpc_ready = 1;
    g_xinput_module = LoadLibraryA("xinput1_4.dll");
    if (!g_xinput_module) g_xinput_module = LoadLibraryA("xinput9_1_0.dll");
    if (g_xinput_module) {
        g_xinput_get_state = (PFN_XInputGetState)GetProcAddress(g_xinput_module, "XInputGetState");
    }
    return 1;
}

int platform_width(void)  { return g_width; }
int platform_height(void) { return g_height; }

void platform_set_design_size(int width, int height) {
    if (width > 0) g_design_w = width;
    if (height > 0) g_design_h = height;
}

static void poll_xinput(void) {
    if (!g_xinput_get_state) return;
    XINPUT_STATE_A6 xs;
    int connected = 0;
    for (DWORD slot = 0; slot < 4; slot++) {
        memset(&xs, 0, sizeof(xs));
        if (g_xinput_get_state(slot, &xs) == ERROR_SUCCESS) {
            connected = 1;
            break;
        }
    }
    if (!connected) {
        if (g_gp_prev_btns & XGP_DPAD_UP)    queue_key_src(0x67, 0, 1);
        if (g_gp_prev_btns & XGP_DPAD_DOWN)  queue_key_src(0x6c, 0, 1);
        if (g_gp_prev_btns & XGP_DPAD_LEFT)  queue_key_src(0x69, 0, 1);
        if (g_gp_prev_btns & XGP_DPAD_RIGHT) queue_key_src(0x6a, 0, 1);
        if (g_gp_prev_btns & XGP_A)          queue_key_src(0x130, 0, 1);
        if (g_gp_prev_btns & (XGP_X | XGP_RB)) queue_key_src(0x137, 0, 1);
        if (g_gp_prev_btns & (XGP_START | XGP_BACK)) queue_key_src(4, 0, 1);
        if (g_gp_prev_ls_dir == -1) queue_key_src(0x69, 0, 2);
        if (g_gp_prev_ls_dir ==  1) queue_key_src(0x6a, 0, 2);
        if (g_gp_prev_ls_ud  == -1) queue_key_src(0x67, 0, 2);
        if (g_gp_prev_ls_ud  ==  1) queue_key_src(0x6c, 0, 2);
        g_gp_prev_btns = 0;
        g_gp_prev_ls_dir = 0;
        g_gp_prev_ls_ud = 0;
        g_gp_steer = 0.0f;
        g_gp_gas = 0;
        g_gp_brake = 0;
        g_gp_nitro = 0;
        g_gp_b = 0;
        return;
    }
    WORD b = xs.Gamepad.wButtons;
    WORD changed = (WORD)(b ^ g_gp_prev_btns);

    /* D-Pad -> navigation / digital steer */
    if (changed & XGP_DPAD_UP)    queue_key_src(0x67, (b & XGP_DPAD_UP) != 0, 1);
    if (changed & XGP_DPAD_DOWN)  queue_key_src(0x6c, (b & XGP_DPAD_DOWN) != 0, 1);
    if (changed & XGP_DPAD_LEFT)  queue_key_src(0x69, (b & XGP_DPAD_LEFT) != 0, 1);
    if (changed & XGP_DPAD_RIGHT) queue_key_src(0x6a, (b & XGP_DPAD_RIGHT) != 0, 1);
    if (changed & XGP_A)          queue_key_src(0x130, (b & XGP_A) != 0, 1);
    if (changed & (XGP_X | XGP_RB)) {
        int nd = ((b & (XGP_X | XGP_RB)) != 0);
        queue_key_src(0x137, nd, 1);
    }
    if (changed & (XGP_START | XGP_BACK)) {
        queue_key_src(4, (b & (XGP_START | XGP_BACK)) != 0, 1);
    }
    g_gp_b = ((b & XGP_B) != 0);
    g_gp_prev_btns = b;

    /* Left Thumbstick X/Y: analog steering + menu stick navigation */
    int lx = xs.Gamepad.sThumbLX;
    int ly = xs.Gamepad.sThumbLY;
    const int dz = 4600;
    if (lx > dz) {
        float t = (float)(lx - dz) / (32767.0f - (float)dz);
        if (t > 1.0f) t = 1.0f;
        g_gp_steer = t * (0.45f + 0.55f * t); /* fine precision near center, full lock at edge */
    } else if (lx < -dz) {
        float t = (float)(-lx - dz) / (32768.0f - (float)dz);
        if (t > 1.0f) t = 1.0f;
        g_gp_steer = -(t * (0.45f + 0.55f * t));
    } else {
        g_gp_steer = 0.0f;
    }

    int abs_lx = (lx < 0) ? -lx : lx;
    int abs_ly = (ly < 0) ? -ly : ly;
    int ls_dir = (lx > 18000) ? 1 : (lx < -18000 ? -1 : 0);
    if (ls_dir != g_gp_prev_ls_dir) {
        if (g_gp_prev_ls_dir == -1) queue_key_src(0x69, 0, 2);
        if (g_gp_prev_ls_dir ==  1) queue_key_src(0x6a, 0, 2);
        if (ls_dir == -1) queue_key_src(0x69, 1, 2);
        if (ls_dir ==  1) queue_key_src(0x6a, 1, 2);
        g_gp_prev_ls_dir = ls_dir;
    }
    int ls_ud = (abs_ly > 18000 && abs_ly > abs_lx) ? ((ly > 0) ? -1 : 1) : 0;
    if (ls_ud != g_gp_prev_ls_ud) {
        if (g_gp_prev_ls_ud == -1) queue_key_src(0x67, 0, 2);
        if (g_gp_prev_ls_ud ==  1) queue_key_src(0x6c, 0, 2);
        if (ls_ud == -1) queue_key_src(0x67, 1, 2);
        if (ls_ud ==  1) queue_key_src(0x6c, 1, 2);
        g_gp_prev_ls_ud = ls_ud;
    }

    g_gp_gas = (xs.Gamepad.bRightTrigger > 35);
    g_gp_brake = (xs.Gamepad.bLeftTrigger > 35) || ((b & XGP_B) != 0);
    g_gp_nitro = ((b & (XGP_X | XGP_RB | XGP_A)) != 0);
}

void platform_poll(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    poll_xinput();
}

void platform_swap(void) {
    if (g_display && g_surface && g_swap) g_swap(g_display, g_surface);
    if (g_qpc_ready) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - g_last_frame_qpc.QuadPart);
        if (!g_no_cap) {
            double target_ticks = (double)g_qpc_freq.QuadPart / g_target_fps;
            if (elapsed < target_ticks) {
                double rem_ms = ((target_ticks - elapsed) * 1000.0) / (double)g_qpc_freq.QuadPart;
                if (rem_ms > 2.0) Sleep((DWORD)(rem_ms - 1.5));
                for (;;) {
                    QueryPerformanceCounter(&now);
                    elapsed = (double)(now.QuadPart - g_last_frame_qpc.QuadPart);
                    if (elapsed >= target_ticks) break;
                }
            }
        }
        g_frame_dt = elapsed / (double)g_qpc_freq.QuadPart;
        if (g_frame_dt < 0.001) g_frame_dt = 0.001;
        if (g_frame_dt > 0.05)  g_frame_dt = 0.05;
        g_last_frame_qpc = now;
    }
}

void platform_shutdown(void) {
    if (g_timer_period_set) { timeEndPeriod(1); g_timer_period_set = 0; }
    if (g_display && g_terminate) g_terminate(g_display);
    g_display = NULL;
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_hwnd = NULL;
    if (g_egl_module) FreeLibrary(g_egl_module);
    g_egl_module = NULL;
    if (g_xinput_module) FreeLibrary(g_xinput_module);
    g_xinput_module = NULL;
    g_xinput_get_state = NULL;
}

int platform_should_quit(void) { return g_quit; }

int platform_take_key(void) { int k = g_key; g_key = 0; return k; }

int platform_take_game_key(int *code, int *down) {
    if (g_key_head == g_key_tail) return 0;
    if (code) *code = g_key_queue[g_key_head].code;
    if (down) *down = g_key_queue[g_key_head].down;
    g_key_head = (g_key_head + 1) % KEY_QUEUE_SIZE;
    return 1;
}

int platform_held_key(int code) {
    return (code > 0 && code < KEY_STATE_MAX) ? g_key_down[code] != 0 : 0;
}

void platform_inject_key(int code, int down) { queue_key(code, down); }

int platform_take_pointer(int *x, int *y, int *down) {
    if (g_pointer_head == g_pointer_tail) return 0;
    if (x) *x = g_pointer_queue[g_pointer_head].x;
    if (y) *y = g_pointer_queue[g_pointer_head].y;
    if (down) *down = g_pointer_queue[g_pointer_head].down;
    g_pointer_head = (g_pointer_head + 1) % POINTER_QUEUE_SIZE;
    return 1;
}

void *platform_gl_proc(const char *name) {
    return g_get_proc ? g_get_proc(name) : NULL;
}

float platform_gamepad_steer(void)  { return g_gp_steer; }
int   platform_gamepad_gas(void)    { return g_gp_gas; }
int   platform_gamepad_brake(void)  { return g_gp_brake; }
int   platform_gamepad_nitro(void)  { return g_gp_nitro; }
int   platform_gamepad_b(void)      { return g_gp_b; }
double platform_frame_dt(void)      { return g_frame_dt; }
#endif /* _WIN32 */
