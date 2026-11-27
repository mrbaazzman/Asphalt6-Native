/*
 * O(1) two-level call dispatch table for translated ARMv7/Thumb-2 functions.
 *
 * Replaces the -O0 binary search, per-call override scan, and per-call MinGW
 * emutls ring buffer with a flat 2-level page directory compiled at -O3.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cpu.h"
#include "mem.h"

typedef void (*FnPtr)(Cpu *c, uint8_t *mem);
typedef struct { uint32_t addr; FnPtr fn; } FnEntry;

extern const uint32_t k_plt_start;
extern const uint32_t k_plt_end;
extern const uint32_t k_func_count;
extern const FnEntry k_funcs[];

void call_import(Cpu *c, uint8_t *mem, uint32_t plt);
int  java_bridge_is_stub(uint32_t target);
int  java_bridge_dispatch(Cpu *c, uint8_t *mem, uint32_t target);

#ifndef M2_BOOT_STUB
int call_override(Cpu *c, uint8_t *mem, uint32_t target);
#endif

/*
 * Each page covers 16 KiB of guest code address space (8192 halfword slots).
 * 4096 directory entries cover 0x00000000 .. 0x03FFFFFF (64 MiB).
 */
#define DISPATCH_DIR_SHIFT 14u
#define DISPATCH_PAGE_SLOTS 8192u
#define DISPATCH_SLOT_MASK (DISPATCH_PAGE_SLOTS - 1u)
#define DISPATCH_DIR_SIZE  4096u

static FnPtr *g_fn_dir[DISPATCH_DIR_SIZE];
static volatile LONG g_dispatch_init_state = 0; /* 0 = uninit, 1 = building, 2 = ready */

static FnPtr lookup_fn_bsearch(uint32_t addr) {
    size_t lo = 0, hi = (size_t)k_func_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t a = k_funcs[mid].addr;
        if (addr < a) hi = mid;
        else if (addr > a) lo = mid + 1;
        else return k_funcs[mid].fn;
    }
    return NULL;
}

static void dir_set(uint32_t key, FnPtr fn) {
    uint32_t dir_idx = key >> DISPATCH_DIR_SHIFT;
    if (dir_idx >= DISPATCH_DIR_SIZE) return;
    FnPtr *page = g_fn_dir[dir_idx];
    if (!page) {
        page = (FnPtr *)calloc(DISPATCH_PAGE_SLOTS, sizeof(FnPtr));
        if (!page) return;
        g_fn_dir[dir_idx] = page;
    }
    page[(key >> 1) & DISPATCH_SLOT_MASK] = fn;
}

#ifndef M2_BOOT_STUB
#define DEF_OV_WRAP(hex) \
    static FnPtr g_orig_##hex = NULL; \
    static void ov_##hex(Cpu *c, uint8_t *mem) { \
        if (call_override(c, mem, 0x##hex##u)) return; \
        if (g_orig_##hex) { g_orig_##hex(c, mem); return; } \
        guest_fault(0x##hex##u, 2); \
    }

DEF_OV_WRAP(42cd90)
DEF_OV_WRAP(50ca98)
DEF_OV_WRAP(50cb3c)
DEF_OV_WRAP(51fb24)
DEF_OV_WRAP(6a8a8c)
DEF_OV_WRAP(8cdf34)
DEF_OV_WRAP(8ce1c4)
DEF_OV_WRAP(a7a99c)
DEF_OV_WRAP(a7a9e4)
DEF_OV_WRAP(b38004)
DEF_OV_WRAP(b38d54)

static void install_overrides(void) {
#define INSTALL_OV(hex) do { \
    g_orig_##hex = lookup_fn_bsearch(0x##hex##u); \
    dir_set(0x##hex##u, ov_##hex); \
} while (0)
    INSTALL_OV(42cd90);
    INSTALL_OV(50ca98);
    INSTALL_OV(50cb3c);
    INSTALL_OV(51fb24);
    INSTALL_OV(6a8a8c);
    INSTALL_OV(8cdf34);
    INSTALL_OV(8ce1c4);
    INSTALL_OV(a7a99c);
    INSTALL_OV(a7a9e4);
    INSTALL_OV(b38004);
    INSTALL_OV(b38d54);
#undef INSTALL_OV
}
#endif

static void dispatch_ensure_init(void) {
#ifdef _WIN32
    if (g_dispatch_init_state == 2) return;
    if (InterlockedCompareExchange(&g_dispatch_init_state, 1, 0) == 0) {
        for (uint32_t i = 0; i < k_func_count; i++) {
            dir_set(k_funcs[i].addr & ~1u, k_funcs[i].fn);
        }
#ifndef M2_BOOT_STUB
        install_overrides();
#endif
        InterlockedExchange(&g_dispatch_init_state, 2);
    } else {
        while (g_dispatch_init_state != 2) {
            SwitchToThread();
        }
    }
#else
    if (g_dispatch_init_state != 2) {
        for (uint32_t i = 0; i < k_func_count; i++) {
            dir_set(k_funcs[i].addr & ~1u, k_funcs[i].fn);
        }
#ifndef M2_BOOT_STUB
        install_overrides();
#endif
        g_dispatch_init_state = 2;
    }
#endif
}

void guest_dump_call_ring(void) {
    fprintf(stderr, "(call ring disabled in fast dispatch mode)\n");
}

static __attribute__((noinline)) void call_target_slow(Cpu *c, uint8_t *mem, uint32_t target, uint32_t key) {
    if (g_dispatch_init_state != 2) {
        dispatch_ensure_init();
        uint32_t dir_idx = key >> DISPATCH_DIR_SHIFT;
        if (dir_idx < DISPATCH_DIR_SIZE) {
            FnPtr *page = g_fn_dir[dir_idx];
            if (page) {
                FnPtr fn = page[(key >> 1) & DISPATCH_SLOT_MASK];
                if (fn) {
                    c->TB = target & 1u;
                    fn(c, mem);
                    return;
                }
            }
        }
    }
    if (java_bridge_is_stub(key)) {
        java_bridge_dispatch(c, mem, key);
        return;
    }
#ifndef M2_BOOT_STUB
    if (call_override(c, mem, key)) return;
#endif
    FnPtr fn = lookup_fn_bsearch(key);
    if (fn) {
        c->TB = target & 1u;
        fn(c, mem);
        return;
    }
    guest_fault(target, 2);
}

void call_target(Cpu *c, uint8_t *mem, uint32_t target) {
    uint32_t key = target & ~1u;
    if ((uint32_t)(key - k_plt_start) < (k_plt_end - k_plt_start)) {
        call_import(c, mem, key);
        return;
    }
    if (__builtin_expect(g_dispatch_init_state == 2, 1)) {
        uint32_t dir_idx = key >> DISPATCH_DIR_SHIFT;
        if (__builtin_expect(dir_idx < DISPATCH_DIR_SIZE, 1)) {
            FnPtr *page = g_fn_dir[dir_idx];
            if (__builtin_expect(page != NULL, 1)) {
                FnPtr fn = page[(key >> 1) & DISPATCH_SLOT_MASK];
                if (__builtin_expect(fn != NULL, 1)) {
                    c->TB = target & 1u;
                    fn(c, mem);
                    return;
                }
            }
        }
    }
    call_target_slow(c, mem, target, key);
}

void call_indirect(Cpu *c, uint8_t *mem, uint32_t target) {
    call_target(c, mem, target);
}
