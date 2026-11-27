/*
 * Native Win32 host entry point for Asphalt 6 HD 3.7.0.
 *
 * Boots guest static constructors (.init_array), runs JNI_OnLoad and GLUtils/GLResLoader/
 * GameRenderer init, then enters the 60 Hz frame/event loop calling appRender.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "mem.h"
#include "platform.h"
#include "resolution.h"
#include "installer.h"

void call_target(Cpu *c, uint8_t *mem, uint32_t target);
void guest_dump_call_ring(void);
void guest_init_data(uint8_t *mem, uint32_t mem_size);
void java_bridge_init(uint8_t *mem, uint32_t mem_size);
uint32_t java_bridge_env(void);
uint32_t java_bridge_vm(void);
void gl_bridge_stats(uint32_t *draws, uint32_t *texs, uint32_t *comp_texs);
void gl_bridge_dump_ppm(const char *path, int w, int h);

/* Verified libasphalt6.so entry addresses (from .symtab) */
#define FN_JNI_ONLOAD           0x42a6b4u
#define FN_DEVICE_NATIVE_INIT   0x427930u
#define FN_RESLOADER_INIT       0x4262b0u
#define FN_MEDIAPLAYER_INIT     0x424300u
#define FN_GLGAME_INIT          0x4233a0u
#define FN_RENDERER_INIT        0x425760u
#define FN_RENDERER_RESIZE      0x425988u
#define FN_RENDERER_RENDER      0x4259a4u
#define FN_APP_INIT             0x42a9bcu
#define FN_APP_RENDER           0x42acd0u
#define FN_TOUCH_MOVED          0x42b0a0u
#define FN_TOUCH_PRESS          0x42b10cu
#define FN_TOUCH_RELEASED       0x42b17cu
#define FN_KEY_UP               0x4237a8u
#define FN_KEY_DOWN             0x4237ccu
#define FN_NATIVE_ACCEL         0x4239d0u
#define FN_ACT_MGR              0x4287f4u
#define FN_PROFILE_HOST         0x428838u
#define FN_IS_RACE_MENU         0x42a664u
#define FN_GAME_INST            0x439dc0u
#define FN_HUD_INST             0x439dd4u
#define FN_IS_RACE_ACTIVE       0x43c218u
#define FN_SET_ACTION_FLAG      0x5b1bccu
#define FN_GET_CONTROL_SCHEME   0x70a314u

#define MAIN_STACK_TOP 0x01800000u
#define CRASH_LOG_PATH "build/asphalt6_crash.log"

uint32_t guest_mem_size = 0x20000000u; /* 512 MiB guest RAM */
static uint8_t *g_mem = NULL;
static __thread jmp_buf g_fault_jmp;
static __thread int g_fault_armed = 0;
static __thread uint32_t g_fault_addr = 0;
static __thread int g_fault_kind = 0;
static volatile int g_crash_frame = -1;

static void crash_log_append_hdr(const char *tag, uint32_t a, int b) {
    DWORD tid = GetCurrentThreadId();
    fprintf(stderr, "%s addr=0x%08x kind=%d tid=%lu frame=%d\n",
            tag, a, b, (unsigned long)tid, g_crash_frame);
    guest_dump_call_ring();
    if (GetFileAttributesA("build") != INVALID_FILE_ATTRIBUTES &&
        freopen(CRASH_LOG_PATH, "a", stderr)) {
        fprintf(stderr, "=== %s addr=0x%08x kind=%d tid=%lu frame=%d ===\n",
                tag, a, b, (unsigned long)tid, g_crash_frame);
        guest_dump_call_ring();
        fflush(stderr);
    }
}

static LONG WINAPI unhandled_seh_filter(EXCEPTION_POINTERS *ep) {
    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void *addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : NULL;
    DWORD tid = GetCurrentThreadId();
    uint64_t rip = (ep && ep->ContextRecord) ? (uint64_t)ep->ContextRecord->Rip : 0;
    uint64_t rsp = (ep && ep->ContextRecord) ? (uint64_t)ep->ContextRecord->Rsp : 0;
    uint64_t rax = (ep && ep->ContextRecord) ? (uint64_t)ep->ContextRecord->Rax : 0;
    uint64_t rcx = (ep && ep->ContextRecord) ? (uint64_t)ep->ContextRecord->Rcx : 0;
    fprintf(stderr, "FATAL SEH code=0x%08lx excAddr=%p rip=0x%llx rsp=0x%llx tid=%lu frame=%d\n",
            (unsigned long)code, addr, (unsigned long long)rip, (unsigned long long)rsp,
            (unsigned long)tid, g_crash_frame);
    guest_dump_call_ring();
    if (GetFileAttributesA("build") != INVALID_FILE_ATTRIBUTES &&
        freopen(CRASH_LOG_PATH, "a", stderr)) {
        fprintf(stderr,
                "=== FATAL SEH code=0x%08lx excAddr=%p rip=0x%llx rsp=0x%llx rax=0x%llx rcx=0x%llx tid=%lu frame=%d ===\n",
                (unsigned long)code, addr, (unsigned long long)rip, (unsigned long long)rsp,
                (unsigned long long)rax, (unsigned long long)rcx,
                (unsigned long)tid, g_crash_frame);
        guest_dump_call_ring();
        fflush(stderr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void guest_fault(uint32_t addr, int write) {
    g_fault_addr = addr;
    g_fault_kind = write;
    if (g_fault_armed) longjmp(g_fault_jmp, 1);
    crash_log_append_hdr("FATAL guest_fault", addr, write);
    exit(2);
}

static uint32_t load_init_array(const char *so_path, uint32_t *out, uint32_t max_n) {
    (void)so_path;
    /* guest.bin already has relocated .init_array at 0x00fe9c68 (392 entries). */
    uint32_t n = 392u;
    if (n > max_n) n = max_n;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = ld4(g_mem, 0x00fe9c68u + i * 4u);
    }
    return n;
}

static void run_guest(Cpu *c, uint32_t addr) {
    c->lr = 0xDEADBEE0u;
    c->sp = MAIN_STACK_TOP;
    c->TB = addr & 1u;
    call_target(c, g_mem, addr);
}

static void ensure_install_cwd(void) {
#ifdef _WIN32
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        char *slash = strrchr(exe, '\\');
        if (!slash) slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            if (a6pak_has_self()) {
                SetCurrentDirectoryA(exe);
                return;
            }
        }
    }
    if (GetFileAttributesA("build/guest.bin") != INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesA("analysis/apk_extract/res/raw/data.txt") != INVALID_FILE_ATTRIBUTES) {
        return;
    }
    if (len == 0 || len >= MAX_PATH) return;

    char probe[MAX_PATH + 64];
    snprintf(probe, sizeof(probe), "%s\\build\\guest.bin", exe);
    if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
        SetCurrentDirectoryA(exe);
        return;
    }
    snprintf(probe, sizeof(probe), "%s\\analysis\\apk_extract\\res\\raw\\data.txt", exe);
    if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
        SetCurrentDirectoryA(exe);
        return;
    }
    /* When running from <repo>/build/asphalt6_win.exe */
    char *parent = strrchr(exe, '\\');
    if (!parent) parent = strrchr(exe, '/');
    if (parent) {
        *parent = '\0';
        snprintf(probe, sizeof(probe), "%s\\build\\guest.bin", exe);
        if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
            SetCurrentDirectoryA(exe);
        }
    }
#endif
}

/* Optional launcher arguments. Without them the game starts exactly as before.
 *   --data <dir>  saves in <dir>\userdata, runtime cache in <dir>\cache, log in <dir>\logs
 *   --windowed, --fps=<n>, --no-cap  same as ASPHALT6_WINDOWED=1, ASPHALT6_FPS, ASPHALT6_NO_CAP=1 */
static void apply_launch_args(int argc, char **argv) {
    const char *data = NULL;
    char env[64];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) data = argv[++i];
        else if (strncmp(argv[i], "--data=", 7) == 0) data = argv[i] + 7;
        else if (strcmp(argv[i], "--windowed") == 0) _putenv("ASPHALT6_WINDOWED=1");
        else if (strcmp(argv[i], "--no-cap") == 0) _putenv("ASPHALT6_NO_CAP=1");
        else if (strncmp(argv[i], "--fps=", 6) == 0) {
            snprintf(env, sizeof(env), "ASPHALT6_FPS=%.40s", argv[i] + 6);
            _putenv(env);
        }
    }
    if (!data || !data[0]) return;
    installer_set_data_dir(data);

    char logs[MAX_PATH], log_path[MAX_PATH + 32], prev_path[MAX_PATH + 32];
    snprintf(logs, sizeof(logs), "%.240s\\logs", data);
    CreateDirectoryA(data, NULL);
    CreateDirectoryA(logs, NULL);
    snprintf(log_path, sizeof(log_path), "%s\\asphalt6.log", logs);
    snprintf(prev_path, sizeof(prev_path), "%s\\asphalt6-previous.log", logs);
    MoveFileExA(log_path, prev_path, MOVEFILE_REPLACE_EXISTING);
    if (freopen(log_path, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
}

int main(int argc, char **argv) {
    SetUnhandledExceptionFilter(unhandled_seh_filter);
    apply_launch_args(argc, argv);
    ensure_install_cwd();
    if (!platform_init(1280, 720, "Asphalt 6: Adrenaline HD")) {
        fprintf(stderr, "host: platform_init failed\n");
        return 1;
    }
    resolution_init(platform_width(), platform_height());
    platform_set_design_size(resolution_width(), resolution_height());

    if (!installer_ensure_gamedata_gl()) {
        fprintf(stderr, "host: game data unavailable or installation canceled\n");
        platform_shutdown();
        return 1;
    }

    g_mem = (uint8_t *)calloc(1, guest_mem_size);
    if (!g_mem) {
        fprintf(stderr, "host: out of memory allocating 512 MiB guest RAM\n");
        return 1;
    }
    uint8_t *pak_guest = NULL;
    size_t pak_sz = 0;
    if (a6pak_read_self("build/guest.bin", &pak_guest, &pak_sz)) {
        if (pak_sz == 0 || pak_sz > guest_mem_size) {
            fprintf(stderr, "host: invalid embedded guest.bin size\n");
            free(pak_guest);
            return 1;
        }
        memcpy(g_mem, pak_guest, pak_sz);
        free(pak_guest);
    } else {
        FILE *f = fopen("build/guest.bin", "rb");
        if (!f) { perror("build/guest.bin"); return 1; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || (uint32_t)sz > guest_mem_size ||
            fread(g_mem, 1, (size_t)sz, f) != (size_t)sz) {
            fprintf(stderr, "host: failed to read build/guest.bin\n");
            fclose(f);
            return 1;
        }
        fclose(f);
    }

    guest_init_data(g_mem, guest_mem_size);
    java_bridge_init(g_mem, guest_mem_size);

    uint32_t ctors[512];
    uint32_t nctors = load_init_array(NULL, ctors, 512);
    for (uint32_t i = 0; i < nctors; i++) {
        Cpu c;
        memset(&c, 0, sizeof(c));
        g_fault_armed = 1;
        if (setjmp(g_fault_jmp) == 0) {
            run_guest(&c, ctors[i]);
        } else {
            g_fault_armed = 0;
            fprintf(stderr, "CTOR[%u]=0x%08x faulted at 0x%08x (write=%d)\n",
                    i, ctors[i], g_fault_addr, g_fault_kind);
            return 1;
        }
        g_fault_armed = 0;
    }
    fprintf(stderr, "boot: %u static constructors OK\n", nctors);

    Cpu c;
    /* 1. JNI_OnLoad(JavaVM*, NULL) */
    memset(&c, 0, sizeof(c));
    c.r0 = java_bridge_vm();
    c.r1 = 0;
    run_guest(&c, FN_JNI_ONLOAD);
    fprintf(stderr, "boot: JNI_OnLoad -> 0x%08x\n", c.r0);

    /* 2. Register Java class hooks via JNIEnv (matches GameGLSurfaceView / GameRenderer.onSurfaceCreated) */
    uint32_t env = java_bridge_env();
    memset(&c, 0, sizeof(c)); c.r0 = env; c.r1 = 0x1218FF0u;
    run_guest(&c, FN_DEVICE_NATIVE_INIT);
    memset(&c, 0, sizeof(c)); c.r0 = env; c.r1 = 0x1218FF0u;
    run_guest(&c, FN_RESLOADER_INIT);
    memset(&c, 0, sizeof(c)); c.r0 = env; c.r1 = 0x1218FF0u;
    run_guest(&c, FN_MEDIAPLAYER_INIT);
    memset(&c, 0, sizeof(c)); c.r0 = env; c.r1 = 0x1218FF0u;
    run_guest(&c, FN_GLGAME_INIT);

    /* 3. GameRenderer.nativeInit(env, obj, width, height, languageOrDeviceFlag=0) */
    int w = platform_width(), h = platform_height();
    memset(&c, 0, sizeof(c));
    c.r0 = env;
    c.r1 = 0x1218FF0u;
    c.r2 = (uint32_t)w;
    c.r3 = (uint32_t)h;
    c.sp = MAIN_STACK_TOP - 8u;
    st4(g_mem, c.sp, 1u);
    c.lr = 0xDEADBEE0u;
    c.TB = 0;
    fprintf(stderr, "boot: calling GameRenderer_nativeInit(%d, %d, 1)...\n", w, h);
    call_target(&c, g_mem, FN_RENDERER_INIT);
    fprintf(stderr, "boot: GameRenderer_nativeInit returned\n");

    memset(&c, 0, sizeof(c));
    c.r0 = env; c.r1 = 0x1218FF0u; c.r2 = (uint32_t)w; c.r3 = (uint32_t)h;
    run_guest(&c, FN_RENDERER_RESIZE);

    const char *max_frames_s = getenv("ASPHALT6_MAX_FRAMES");
    int max_frames = max_frames_s ? atoi(max_frames_s) : 0;
    int dump_frames = getenv("ASPHALT6_DUMP_FRAME") != NULL;
    int auto_tap = getenv("ASPHALT6_AUTO_TAP") != NULL;
    const char *auto_taps = getenv("ASPHALT6_AUTO_TAPS");
    const char *dump_every_s = getenv("ASPHALT6_DUMP_EVERY");
    int dump_every = dump_every_s ? atoi(dump_every_s) : 0;
    const char *auto_keys = getenv("ASPHALT6_AUTO_KEYS");
    int prof = getenv("ASPHALT6_PROFILE") != NULL;
    LARGE_INTEGER pf_freq, pf0, pf1, pf2, pf3;
    QueryPerformanceFrequency(&pf_freq);
    double acc_poll = 0, acc_render = 0, acc_swap = 0;
    int frame = 0;

    /* Feed initial neutral accelerometer (portrait-mounted landscape: X=9.81 or Y=9.81) */
    while (!platform_should_quit()) {
        g_crash_frame = frame;
        if (prof) QueryPerformanceCounter(&pf0);
        platform_poll();

        if (auto_tap && !auto_taps) {
            if (frame == 45) {
                memset(&c, 0, sizeof(c));
                c.r0 = (uint32_t)(w / 2); c.r1 = (uint32_t)((h * 3) / 4); c.r2 = 0;
                run_guest(&c, FN_TOUCH_PRESS);
            }
            if (frame == 48) {
                memset(&c, 0, sizeof(c));
                c.r0 = (uint32_t)(w / 2); c.r1 = (uint32_t)((h * 3) / 4); c.r2 = 0;
                run_guest(&c, FN_TOUCH_RELEASED);
            }
        }
        if (auto_taps) {
            const char *p = auto_taps;
            while (*p) {
                int tf = 0, tx = 0, ty = 0;
                if (sscanf(p, "%d:%d:%d", &tf, &tx, &ty) == 3) {
                    if (frame == tf) {
                        memset(&c, 0, sizeof(c));
                        c.r0 = (uint32_t)tx; c.r1 = (uint32_t)ty; c.r2 = 0;
                        run_guest(&c, FN_TOUCH_PRESS);
                    } else if (frame == tf + 3) {
                        memset(&c, 0, sizeof(c));
                        c.r0 = (uint32_t)tx; c.r1 = (uint32_t)ty; c.r2 = 0;
                        run_guest(&c, FN_TOUCH_RELEASED);
                    }
                }
                const char *comma = strchr(p, ',');
                if (!comma) break;
                p = comma + 1;
            }
        }
        if (auto_keys) {
            const char *p = auto_keys;
            while (*p) {
                int kf0 = 0, kf1 = 0, kcode = 0;
                if (sscanf(p, "%d-%d:%d", &kf0, &kf1, &kcode) == 3) {
                    if (frame == kf0) platform_inject_key(kcode, 1);
                    if (frame == kf1) platform_inject_key(kcode, 0);
                }
                const char *comma = strchr(p, ',');
                if (!comma) break;
                p = comma + 1;
            }
        }

        /* Guarantee at least one rendered frame between PRESS and RELEASE on fast mouse clicks. */
        static int has_deferred_up = 0, deferred_x = 0, deferred_y = 0;
        if (has_deferred_up) {
            memset(&c, 0, sizeof(c));
            c.r0 = (uint32_t)deferred_x;
            c.r1 = (uint32_t)deferred_y;
            c.r2 = 0;
            run_guest(&c, FN_TOUCH_RELEASED);
            has_deferred_up = 0;
        }
        int x = 0, y = 0, down = 0, saw_press = 0;
        while (!has_deferred_up && platform_take_pointer(&x, &y, &down)) {
            if (saw_press && down == 0) {
                has_deferred_up = 1;
                deferred_x = x;
                deferred_y = y;
                break;
            }
            memset(&c, 0, sizeof(c));
            c.r0 = (uint32_t)x;
            c.r1 = (uint32_t)y;
            c.r2 = 0;
            if (down == 1)      { saw_press = 1; run_guest(&c, FN_TOUCH_PRESS); }
            else if (down == 2) run_guest(&c, FN_TOUCH_MOVED);
            else                run_guest(&c, FN_TOUCH_RELEASED);
        }

        int in_race = 0;
        uint32_t act_obj = 0;
        {
            Cpu q;
            memset(&q, 0, sizeof(q)); run_guest(&q, FN_ACT_MGR);
            act_obj = q.r0;
            if (act_obj) {
                memset(&q, 0, sizeof(q)); q.r0 = act_obj; run_guest(&q, FN_IS_RACE_ACTIVE);
                if (q.r0) {
                    memset(&q, 0, sizeof(q)); run_guest(&q, FN_HUD_INST);
                    if (q.r0) {
                        uint32_t hud = q.r0;
                        memset(&q, 0, sizeof(q)); q.r0 = hud; run_guest(&q, FN_IS_RACE_MENU);
                        in_race = (q.r0 == 0u);
                    }
                }
            }
        }

        uint32_t ctrl_scheme = 0;
        {
            Cpu q;
            memset(&q, 0, sizeof(q)); run_guest(&q, FN_GAME_INST);
            if (q.r0) {
                uint32_t gi = q.r0;
                memset(&q, 0, sizeof(q)); q.r0 = gi; run_guest(&q, FN_PROFILE_HOST);
                if (q.r0) {
                    uint32_t pr = q.r0;
                    memset(&q, 0, sizeof(q)); q.r0 = pr; run_guest(&q, FN_GET_CONTROL_SCHEME);
                    ctrl_scheme = q.r0;
                }
            }
        }

        static int prev_gp_b = 0;
        int cur_gp_b = platform_gamepad_b();
        if (!in_race && cur_gp_b != prev_gp_b) {
            platform_inject_key(4, cur_gp_b);
        }
        prev_gp_b = cur_gp_b;

        /* Deliver key events to guest:
         * - Menus use a 90-deg CCW D-pad layout on this Android build; rotate
         *   directional keys 90-deg CW so Up/Down/Left/Right match screen directions.
         * - Track active_guest_code per input code so KEY_UP always releases the
         *   exact scancode sent on KEY_DOWN across menu<->race transitions.
         * - Suppress drive/confirm KEY_DOWN in-race so FN_SET_ACTION_FLAG +
         *   FN_NATIVE_ACCEL below own steering, gas, brake/drift, and nitro. */
        static uint16_t active_guest_code[512];
        int code = 0, kdown = 0;
        while (platform_take_game_key(&code, &kdown)) {
            if (in_race && kdown &&
                (code == 0x137 || code == 0x130 ||
                 code == 0x69 || code == 0x6a || code == 0x67 || code == 0x6c)) {
                continue;
            }
            uint32_t send_code = (uint32_t)code;
            if (kdown) {
                if (!in_race) {
                    switch (code) {
                    case 0x67: send_code = 0x6a; break; /* UP    -> Screen Up    */
                    case 0x6c: send_code = 0x69; break; /* DOWN  -> Screen Down  */
                    case 0x69: send_code = 0x67; break; /* LEFT  -> Screen Left  */
                    case 0x6a: send_code = 0x6c; break; /* RIGHT -> Screen Right */
                    default: break;
                    }
                }
                if (code >= 0 && code < 512) active_guest_code[code] = (uint16_t)send_code;
            } else if (code >= 0 && code < 512 && active_guest_code[code]) {
                send_code = active_guest_code[code];
                active_guest_code[code] = 0;
            }
            memset(&c, 0, sizeof(c));
            c.r0 = env; c.r1 = 0; c.r2 = send_code;
            run_guest(&c, kdown ? FN_KEY_DOWN : FN_KEY_UP);
        }

        /* Consistent in-race Gas (0x1), Brake/Reverse (0x2), Steer Left (0x4),
         * Steer Right (0x8), and Nitro (0x200) matching the guest's 90-deg D-pad layout. */
        static int prev_gas = 0, prev_brake = 0, prev_nitro = 0;
        static int prev_left = 0, prev_right = 0, prev_in_race = 0;
        int is_tilt = (ctrl_scheme == 0u || ctrl_scheme == 2u || ctrl_scheme == 6u);
        int cur_gas   = (in_race && act_obj) ? (platform_held_key(0x67) || platform_gamepad_gas())   : 0;
        int cur_brake = (in_race && act_obj) ? (platform_held_key(0x6c) || platform_gamepad_brake()) : 0;
        int cur_nitro = (in_race && act_obj) ? (platform_held_key(0x137) || platform_gamepad_nitro()) : 0;
        int cur_left  = (in_race && act_obj && !is_tilt) ? (platform_held_key(0x69) || platform_gamepad_steer() < -0.18f) : 0;
        int cur_right = (in_race && act_obj && !is_tilt) ? (platform_held_key(0x6a) || platform_gamepad_steer() >  0.18f) : 0;
        struct { int cur; int *prev; uint32_t flag; } actions[] = {
            { cur_gas,   &prev_gas,   0x1u   },
            { cur_brake, &prev_brake, 0x2u   },
            { cur_nitro, &prev_nitro, 0x200u },
            { cur_left,  &prev_left,  0x4u   },
            { cur_right, &prev_right, 0x8u   },
        };
        if (act_obj && (in_race || prev_in_race)) {
            for (int ai = 0; ai < (int)(sizeof(actions) / sizeof(actions[0])); ai++) {
                if (actions[ai].cur != *actions[ai].prev) {
                    memset(&c, 0, sizeof(c));
                    c.r0 = act_obj + 0xc0u;
                    c.r1 = (uint32_t)(actions[ai].cur ? 1 : 0);
                    c.r2 = actions[ai].flag;
                    c.r3 = 0;
                    run_guest(&c, FN_SET_ACTION_FLAG);
                    *actions[ai].prev = actions[ai].cur;
                }
            }
        } else {
            prev_gas = prev_brake = prev_nitro = prev_left = prev_right = 0;
        }
        prev_in_race = in_race;

        /* Smooth progressive keyboard + analog XInput stick steering via FN_NATIVE_ACCEL */
        {
            static float g_steer = 0.0f;
            float dt = (float)platform_frame_dt();
            float gp = platform_gamepad_steer();
            float abs_gp = gp < 0.0f ? -gp : gp;
            if (!in_race) {
                g_steer = 0.0f;
            } else if (abs_gp > 0.01f) {
                g_steer = gp;
            } else {
                int kl = platform_held_key(0x69);
                int kr = platform_held_key(0x6a);
                float target = (kl && !kr) ? -1.0f : ((kr && !kl) ? 1.0f : 0.0f);
                float rate = (target == 0.0f || (target * g_steer < 0.0f)) ? 7.5f : 4.2f;
                float step = rate * dt;
                float diff = target - g_steer;
                if (diff > step) g_steer += step;
                else if (diff < -step) g_steer -= step;
                else g_steer = target;
            }
            float abs_s = g_steer < 0.0f ? -g_steer : g_steer;
            float ax = -7.0f + abs_s * 1.2f;
            float ay = g_steer * 6.0f;
            float az = -9.81f;
            uint32_t uax, uay, uaz;
            memcpy(&uax, &ax, 4); memcpy(&uay, &ay, 4); memcpy(&uaz, &az, 4);
            memset(&c, 0, sizeof(c));
            c.r0 = env; c.r1 = 0; c.r2 = uax; c.r3 = uay;
            c.sp = MAIN_STACK_TOP - 8u;
            st4(g_mem, c.sp, uaz);
            c.lr = 0xDEADBEE0u;
            call_target(&c, g_mem, FN_NATIVE_ACCEL);
        }

        if (prof) QueryPerformanceCounter(&pf1);
        memset(&c, 0, sizeof(c));
        c.r0 = env; c.r1 = 0x1218FF0u;
        run_guest(&c, FN_RENDERER_RENDER);
        if (prof) QueryPerformanceCounter(&pf2);

        if (dump_frames && ((dump_every > 0 && frame % dump_every == 0) ||
            frame == 5 || frame == 30 || frame == 100 || (max_frames > 0 && frame + 1 == max_frames))) {
            char ppm[128];
            snprintf(ppm, sizeof(ppm), "build/frame_%04d.ppm", frame);
            gl_bridge_dump_ppm(ppm, platform_width(), platform_height());
        }

        platform_swap();
        if (prof) {
            QueryPerformanceCounter(&pf3);
            acc_poll   += (double)(pf1.QuadPart - pf0.QuadPart) * 1000.0 / (double)pf_freq.QuadPart;
            acc_render += (double)(pf2.QuadPart - pf1.QuadPart) * 1000.0 / (double)pf_freq.QuadPart;
            acc_swap   += (double)(pf3.QuadPart - pf2.QuadPart) * 1000.0 / (double)pf_freq.QuadPart;
            if ((frame % 500) == 499) {
                fprintf(stderr, "prof[%d..%d]: poll=%.2fms render=%.2fms swap=%.2fms (avg %.1f FPS)\n",
                        frame - 499, frame, acc_poll / 500.0, acc_render / 500.0, acc_swap / 500.0,
                        500000.0 / (acc_poll + acc_render + acc_swap));
                acc_poll = acc_render = acc_swap = 0;
            }
        }
        frame++;
        if (max_frames > 0 && frame >= max_frames) break;
    }

    uint32_t draws = 0, texs = 0, comp = 0;
    gl_bridge_stats(&draws, &texs, &comp);
    fprintf(stderr, "stats: frames=%d glDraws=%u glTexImage2D=%u glCompressedTexImage2D=%u\n",
            frame, draws, texs, comp);

    platform_shutdown();
    return 0;
}
