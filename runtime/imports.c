/*
 * Dynamic import dispatch and C/POSIX/pthread/math/VFS/GL runtime for libasphalt6.so.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

#include <ctype.h>
#include <wctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cpu.h"
#include "imports_table.h"
#include "mem.h"
#ifdef _WIN32
#include "thread_win32.h"
#endif

#define GUEST_HEAP_BASE 0x02000000u
#define PLT_BASE 0x422610u
#define PLT_STRIDE 12u

/* Hot PLT fast paths (verified in runtime/imports_table.h) */
#define PLT_MEMCMP  0x422d0cu
#define PLT_MEMCPY  0x422ef8u
#define PLT_MEMSET  0x422bc8u
#define PLT_MEMMOVE 0x422778u
#define PLT_STRLEN  0x4226d0u

extern uint32_t g_errno;
void call_target(Cpu *c, uint8_t *mem, uint32_t target);

/* VFS functions (runtime/vfs.c) */
uint32_t vfs_open(const char *path, const char *mode);
uint32_t vfs_read(uint32_t h, void *buf, uint32_t sz, uint32_t cnt);
uint32_t vfs_write(uint32_t h, const void *buf, uint32_t sz, uint32_t cnt);
int      vfs_close(uint32_t h);
int      vfs_seek(uint32_t h, int32_t off, int whence);
int32_t  vfs_tell(uint32_t h);
int      vfs_eof(uint32_t h);
int      vfs_error(uint32_t h);
int      vfs_getc(uint32_t h);
int      vfs_ungetc(int ch, uint32_t h);
int      vfs_putc(int ch, uint32_t h);
int      vfs_flush(uint32_t h);
char    *vfs_gets(char *dst, int n, uint32_t h);
int      vfs_puts(const char *s, uint32_t h);
int      vfs_fileno(uint32_t h);
uint32_t vfs_fdopen(int gfd, const char *mode);
int      vfs_open_fd(const char *path, int oflag);
int      vfs_read_fd(int gfd, void *buf, uint32_t count);
int      vfs_write_fd(int gfd, const void *buf, uint32_t count);
int32_t  vfs_lseek_fd(int gfd, int32_t off, int whence);
int      vfs_close_fd(int gfd);
int      vfs_stat(const char *path, void *stat_buf);
int      vfs_fstat_fd(int gfd, void *stat_buf);
void     vfs_chdir(const char *path);

#ifndef M2_BOOT_STUB
typedef void (*GlHandlerFn)(Cpu *c, uint8_t *mem);
GlHandlerFn gl_bridge_lookup(const char *name);
int gl_bridge_call(Cpu *c, uint8_t *mem, const char *name);
int platform_width(void);
int platform_height(void);
void guest_dump_call_ring(void);
#else
typedef void (*GlHandlerFn)(Cpu *c, uint8_t *mem);
static inline GlHandlerFn gl_bridge_lookup(const char *name) {
    (void)name; return NULL;
}
static inline int gl_bridge_call(Cpu *c, uint8_t *mem, const char *name) {
    (void)c; (void)mem; (void)name; return 0;
}
static inline int platform_width(void) { return 800; }
static inline int platform_height(void) { return 480; }
static inline void guest_dump_call_ring(void) {}
#endif

static uint8_t *g_mem = NULL;
static uint8_t g_seen[IMPORT_COUNT];
static int g_verbose = -1;
static uint32_t g_gl_id = 100u;

static inline uint32_t r32(const uint8_t *m, uint32_t a) { return ld4(m, a); }
static inline void w32(uint8_t *m, uint32_t a, uint32_t v) { st4(m, a, v); }

static void put_str(uint8_t *m, uint32_t addr, const char *s) {
    size_t n = strlen(s) + 1u;
    if (addr + n <= guest_mem_size) memcpy(m + addr, s, n);
}

static inline void *hostp(uint32_t addr, uint32_t n) {
    if (__builtin_expect(!g_mem, 0)) return NULL;
    if (__builtin_expect(addr > (GUEST_MEM_SIZE - n), 0)) {
        guest_fault(addr, 0);
        return NULL;
    }
    return g_mem + addr;
}

static uint32_t arg_u32(Cpu *c, const uint8_t *mem, unsigned idx,
                        const uint32_t *reg_args, unsigned n_reg_args) {
    if (idx < n_reg_args) return reg_args[idx];
    uint32_t sp_off = (idx - n_reg_args) * 4u;
    return r32(mem, c->sp + sp_off);
}

static uint64_t arg_u64(Cpu *c, const uint8_t *mem, unsigned *idx_io,
                        const uint32_t *reg_args, unsigned n_reg_args) {
    unsigned idx = *idx_io;
    if (n_reg_args > 0u) {
        while (idx < n_reg_args && (((4u - n_reg_args) + idx) & 1u)) idx++;
        if (idx < n_reg_args && idx + 1u >= n_reg_args) idx = n_reg_args;
    }
    if (idx >= n_reg_args) {
        uint32_t sp_addr = c->sp + (idx - n_reg_args) * 4u;
        if (sp_addr & 4u) idx++;
    }
    uint32_t lo = arg_u32(c, mem, idx, reg_args, n_reg_args);
    uint32_t hi = arg_u32(c, mem, idx + 1u, reg_args, n_reg_args);
    *idx_io = idx + 2u;
    return ((uint64_t)hi << 32) | lo;
}

static double arg_f64(Cpu *c, const uint8_t *mem, unsigned *idx_io,
                      const uint32_t *reg_args, unsigned n_reg_args) {
    uint64_t bits = arg_u64(c, mem, idx_io, reg_args, n_reg_args);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static void format_va(char *dst, size_t cap, const char *fmt,
                      Cpu *c, const uint8_t *mem, const uint32_t *reg_args, unsigned n_reg) {
    size_t pos = 0;
    unsigned argi = 0;
    while (*fmt && pos + 1 < cap) {
        if (*fmt != '%') {
            dst[pos++] = *fmt++;
            continue;
        }
        const char *spec_start = fmt++;
        if (*fmt == '%') {
            dst[pos++] = '%';
            fmt++;
            continue;
        }
        while (*fmt && strchr("-+ #0", *fmt)) fmt++;
        if (*fmt == '*') { (void)arg_u32(c, mem, argi++, reg_args, n_reg); fmt++; }
        else while (*fmt && isdigit((unsigned char)*fmt)) fmt++;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') { (void)arg_u32(c, mem, argi++, reg_args, n_reg); fmt++; }
            else while (*fmt && isdigit((unsigned char)*fmt)) fmt++;
        }
        int is_ll = 0;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { is_ll = 1; fmt++; }
        } else if (*fmt == 'h' || *fmt == 'z') {
            fmt++;
            if (*fmt == 'h') fmt++;
        }
        char conv = *fmt ? *fmt++ : '\0';
        if (!conv) break;
        char local_fmt[32];
        size_t flen = (size_t)(fmt - spec_start);
        if (flen >= sizeof(local_fmt)) flen = sizeof(local_fmt) - 1;
        memcpy(local_fmt, spec_start, flen);
        local_fmt[flen] = '\0';

        char tmp[512];
        tmp[0] = '\0';
        if (conv == 's') {
            uint32_t ptr = arg_u32(c, mem, argi++, reg_args, n_reg);
            const char *s = (ptr && ptr < guest_mem_size) ? (const char *)(mem + ptr) : "(null)";
            snprintf(tmp, sizeof(tmp), local_fmt, s);
        } else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
                   conv == 'g' || conv == 'G') {
            double d = arg_f64(c, mem, &argi, reg_args, n_reg);
            snprintf(tmp, sizeof(tmp), local_fmt, d);
        } else if (is_ll) {
            uint64_t v = arg_u64(c, mem, &argi, reg_args, n_reg);
            snprintf(tmp, sizeof(tmp), local_fmt, v);
        } else {
            uint32_t v = arg_u32(c, mem, argi++, reg_args, n_reg);
            snprintf(tmp, sizeof(tmp), local_fmt, v);
        }
        size_t tlen = strlen(tmp);
        if (pos + tlen >= cap) tlen = cap - 1 - pos;
        memcpy(dst + pos, tmp, tlen);
        pos += tlen;
    }
    dst[pos] = '\0';
}

/* Format reading from guest va_list pointer (which on ARM is a single uint32_t pointer into stack). */
static void format_valist(char *dst, size_t cap, const char *fmt,
                          const uint8_t *mem, uint32_t ap) {
    Cpu fake;
    memset(&fake, 0, sizeof(fake));
    fake.sp = ap;
    format_va(dst, cap, fmt, &fake, mem, NULL, 0);
}

static uint32_t guest_sscanf(Cpu *c, const uint8_t *mem) {
    const char *s = (const char *)hostp(c->r0, 1);
    const char *fmt = (const char *)hostp(c->r1, 1);
    if (!s || !fmt) return 0;
    uint32_t reg_args[2] = { c->r2, c->r3 };
    unsigned argi = 0;
    uint32_t assigned = 0;
    while (*fmt && *s) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s)) s++;
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) break;
            s++; fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            if (*s != '%') break;
            s++; fmt++;
            continue;
        }
        while (*fmt && isdigit((unsigned char)*fmt)) fmt++;
        if (*fmt == 'l') fmt++;
        char conv = *fmt++;
        if (!conv) break;
        uint32_t dst = arg_u32(c, mem, argi++, reg_args, 2);
        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X') {
            char *end = NULL;
            int base = (conv == 'd' || conv == 'u') ? 10 : (conv == 'x' || conv == 'X') ? 16 : 0;
            long v = strtol(s, &end, base);
            if (end == s) break;
            if (dst && dst + 4u <= guest_mem_size) w32(g_mem, dst, (uint32_t)(int32_t)v);
            s = end;
            assigned++;
        } else if (conv == 'f' || conv == 'g' || conv == 'e') {
            char *end = NULL;
            float v = strtof(s, &end);
            if (end == s) break;
            if (dst && dst + 4u <= guest_mem_size) {
                uint32_t bits;
                memcpy(&bits, &v, 4);
                w32(g_mem, dst, bits);
            }
            s = end;
            assigned++;
        } else if (conv == 's') {
            while (isspace((unsigned char)*s)) s++;
            if (!*s) break;
            uint32_t k = 0;
            while (*s && !isspace((unsigned char)*s)) {
                if (dst && dst + k + 1u < guest_mem_size) g_mem[dst + k] = (uint8_t)*s;
                s++; k++;
            }
            if (dst && dst + k < guest_mem_size) g_mem[dst + k] = 0;
            assigned++;
        } else if (conv == 'c') {
            if (!*s) break;
            if (dst && dst < guest_mem_size) g_mem[dst] = (uint8_t)*s;
            s++;
            assigned++;
        } else {
            break;
        }
    }
    return assigned;
}

/* --- Coalescing boundary-tag heap allocator --- */
#define BLK_HDR 8u
#define BLK_USED 1u
#define BLK_PREV_USED 2u
#define BLK_ALIGN 7u
#define NBINS 32

typedef struct Block {
    uint32_t size;
    uint32_t prev_size;
} Block;

typedef struct FreeLinks {
    Block *next;
    Block *prev;
} FreeLinks;

#define BLK_MIN_PAYLOAD ((uint32_t)sizeof(FreeLinks))

static Block *g_bins[NBINS];
static Block *g_last;
static uint32_t g_top;
static uint32_t g_live;
static uint32_t g_peak_live;
#ifdef _WIN32
static CRITICAL_SECTION g_alloc_lock;
static INIT_ONCE g_alloc_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK alloc_init_once(PINIT_ONCE once, PVOID p, PVOID *ctx) {
    (void)once; (void)p; (void)ctx;
    InitializeCriticalSection(&g_alloc_lock);
    return TRUE;
}
static void lock_heap(void) {
    InitOnceExecuteOnce(&g_alloc_once, alloc_init_once, NULL, NULL);
    EnterCriticalSection(&g_alloc_lock);
}
static void unlock_heap(void) { LeaveCriticalSection(&g_alloc_lock); }
#else
static void lock_heap(void) {}
static void unlock_heap(void) {}
#endif

static inline uint32_t blk_size(const Block *b) { return b->size & ~7u; }
static inline int blk_used(const Block *b) { return (b->size & BLK_USED) != 0; }
static inline int blk_prev_used(const Block *b) { return (b->size & BLK_PREV_USED) != 0; }
static inline void blk_set(Block *b, uint32_t payload, int used, int prev_used) {
    b->size = payload | (used ? BLK_USED : 0u) | (prev_used ? BLK_PREV_USED : 0u);
}
static inline uint32_t blk_total(const Block *b) { return BLK_HDR + blk_size(b); }
static inline Block *blk_next(Block *b) { return (Block *)((uint8_t *)b + blk_total(b)); }
static inline Block *blk_prev(Block *b) { return (Block *)((uint8_t *)b - BLK_HDR - b->prev_size); }
static inline void *blk_payload(Block *b) { return (uint8_t *)b + BLK_HDR; }
static inline FreeLinks *blk_links(Block *b) { return (FreeLinks *)blk_payload(b); }

static int bin_index(uint32_t n) {
    int i = 0;
    uint32_t v = n >> 1;
    while (v) { v >>= 1; i++; }
    if (i >= NBINS) i = NBINS - 1;
    return i;
}

static void bin_insert(Block *b) {
    int i = bin_index(blk_size(b));
    FreeLinks *l = blk_links(b);
    l->prev = NULL;
    l->next = g_bins[i];
    if (g_bins[i]) blk_links(g_bins[i])->prev = b;
    g_bins[i] = b;
}

static void bin_remove(Block *b) {
    int i = bin_index(blk_size(b));
    FreeLinks *l = blk_links(b);
    if (l->prev) blk_links(l->prev)->next = l->next;
    else g_bins[i] = l->next;
    if (l->next) blk_links(l->next)->prev = l->prev;
    l->next = NULL;
    l->prev = NULL;
}

static Block *bin_find(uint32_t need) {
    for (int i = bin_index(need); i < NBINS; i++) {
        for (Block *b = g_bins[i]; b; b = blk_links(b)->next) {
            if (blk_size(b) >= need) return b;
        }
    }
    return NULL;
}

static Block *heap_extend(uint32_t need) {
    uint32_t want = need + BLK_HDR;
    uint32_t chunk = 256u * 1024u;
    if (want > chunk) chunk = (want + 0xFFFFu) & ~0xFFFFu;
    if (g_top == 0) g_top = GUEST_HEAP_BASE;

    if (g_last && !blk_used(g_last)) {
        Block *b = g_last;
        bin_remove(b);
        uint32_t grow = chunk;
        if (g_top + grow > guest_mem_size) grow = guest_mem_size - g_top;
        if (grow > 0) {
            blk_set(b, blk_size(b) + grow, 0, blk_prev_used(b));
            g_top += grow;
        }
        return blk_size(b) >= need ? b : NULL;
    }

    if (g_top + chunk > guest_mem_size) {
        chunk = guest_mem_size - g_top;
        if (chunk < want) return NULL;
    }
    Block *b = (Block *)(g_mem + g_top);
    blk_set(b, chunk - BLK_HDR, 0, g_last ? blk_used(g_last) : 1);
    b->prev_size = g_last ? blk_size(g_last) : 0;
    g_last = b;
    g_top += chunk;
    return b;
}

static void heap_free_block(Block *b) {
    uint32_t size = blk_size(b);
    if (g_last != b) {
        Block *n = blk_next(b);
        if (!blk_used(n)) {
            bin_remove(n);
            uint32_t merged = size + blk_total(n);
            blk_set(b, merged, 0, blk_prev_used(b));
            size = merged;
            if (g_last == n) g_last = b;
        }
    }
    if (!blk_prev_used(b)) {
        Block *p = blk_prev(b);
        bin_remove(p);
        uint32_t merged = blk_size(p) + blk_total(b);
        blk_set(p, merged, 0, blk_prev_used(p));
        if (g_last == b) g_last = p;
        b = p;
    }
    if (g_last != b) {
        Block *n = blk_next(b);
        n->prev_size = blk_size(b);
        n->size &= ~BLK_PREV_USED;
    }
    bin_insert(b);
}

static uint32_t gmalloc_locked(uint32_t n) {
    n = (n + BLK_ALIGN) & ~BLK_ALIGN;
    if (n < BLK_MIN_PAYLOAD) n = BLK_MIN_PAYLOAD;
    Block *b = bin_find(n);
    if (b) bin_remove(b);
    else {
        b = heap_extend(n);
        if (!b) return 0;
    }
    uint32_t size = blk_size(b);
    int prev_used = blk_prev_used(b);
    if (size >= n + BLK_HDR + BLK_MIN_PAYLOAD) {
        int was_last = (g_last == b);
        Block *rest = (Block *)((uint8_t *)b + BLK_HDR + n);
        blk_set(rest, size - n - BLK_HDR, 0, 1);
        rest->prev_size = n;
        blk_set(b, n, 1, prev_used);
        if (was_last) g_last = rest;
        else {
            Block *nn = blk_next(rest);
            nn->prev_size = blk_size(rest);
            nn->size &= ~BLK_PREV_USED;
        }
        bin_insert(rest);
    } else {
        blk_set(b, size, 1, prev_used);
        if (g_last != b) {
            Block *nn = blk_next(b);
            nn->prev_size = size;
            nn->size |= BLK_PREV_USED;
        }
    }
    g_live += blk_size(b);
    if (g_live > g_peak_live) g_peak_live = g_live;
    return (uint32_t)((uint8_t *)blk_payload(b) - g_mem);
}

static void gfree_locked(uint32_t p) {
    if (p < GUEST_HEAP_BASE || p >= g_top) return;
    Block *b = (Block *)(g_mem + p - BLK_HDR);
    if (!blk_used(b)) return;
    g_live -= blk_size(b);
    heap_free_block(b);
}

static uint32_t gmalloc(uint32_t n) {
    lock_heap();
    uint32_t r = gmalloc_locked(n);
    unlock_heap();
    return r;
}

static void gfree(uint32_t p) {
    lock_heap();
    gfree_locked(p);
    unlock_heap();
}

static uint32_t grealloc(uint32_t p, uint32_t n) {
    if (!p) return gmalloc(n);
    if (n == 0) { gfree(p); return 0; }
    lock_heap();
    uint32_t result = 0;
    uint32_t old_size = 0;
    if (p >= GUEST_HEAP_BASE && p < g_top) {
        Block *b = (Block *)(g_mem + p - BLK_HDR);
        if (blk_used(b)) old_size = blk_size(b);
    }
    if (old_size >= n) {
        result = p;
    } else {
        uint32_t q = gmalloc_locked(n);
        if (q) {
            uint32_t copy = old_size < n ? old_size : n;
            memcpy(g_mem + q, g_mem + p, copy);
            if (old_size) gfree_locked(p);
            result = q;
        }
    }
    unlock_heap();
    return result;
}

/* qsort support for guest comparator function */
static __thread Cpu *g_qsort_cpu = NULL;
static __thread uint32_t g_qsort_cmp = 0;
static __thread uint32_t g_qsort_scratch_base = 0;
static int guest_qsort_cmp(const void *a, const void *b) {
    uint32_t ga = g_qsort_scratch_base + (uint32_t)((const uint8_t *)a - (g_mem + g_qsort_scratch_base));
    uint32_t gb = g_qsort_scratch_base + (uint32_t)((const uint8_t *)b - (g_mem + g_qsort_scratch_base));
    Cpu sub = *g_qsort_cpu;
    sub.r0 = ga;
    sub.r1 = gb;
    sub.lr = 0xDEADBEE0u;
    call_target(&sub, g_mem, g_qsort_cmp);
    return (int)(int32_t)sub.r0;
}

static int streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
static int is_prefix(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static float f32_of(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
static uint32_t f32_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static double f64_of(uint32_t lo, uint32_t hi) {
    uint64_t b = ((uint64_t)hi << 32) | lo; double d; memcpy(&d, &b, 8); return d;
}
static void f64_to(double d, uint32_t *lo, uint32_t *hi) {
    uint64_t b; memcpy(&b, &d, 8); *lo = (uint32_t)b; *hi = (uint32_t)(b >> 32);
}

static const ImportEntry *find_import(uint32_t plt) {
    if (plt < PLT_BASE) return NULL;
    uint32_t off = plt - PLT_BASE;
    if (off % PLT_STRIDE) return NULL;
    uint32_t idx = off / PLT_STRIDE;
    if (idx >= IMPORT_COUNT || k_import_table[idx].plt != plt) return NULL;
    return &k_import_table[idx];
}

static void import_dispatch(Cpu *c, uint8_t *mem, const char *n) {
    /* --- C++ runtime / allocator --- */
    if (streq(n, "_ZnajRKSt9nothrow_t")) { c->r0 = gmalloc(c->r0); return; }
    if (streq(n, "__cxa_guard_acquire")) {
        uint32_t g = c->r0;
        if (g && guest_ok(g, 4) && r32(mem, g) == 0) {
            c->r0 = 1;
        } else {
            c->r0 = 0;
        }
        return;
    }
    if (streq(n, "__cxa_guard_release")) {
        if (c->r0 && guest_ok(c->r0, 4)) w32(mem, c->r0, 1u);
        return;
    }
    if (streq(n, "__aeabi_atexit") || streq(n, "__cxa_atexit") || streq(n, "__cxa_finalize")) {
        c->r0 = 0; return;
    }
    if (streq(n, "__cxa_pure_virtual")) { fprintf(stderr, "import: pure virtual\n"); exit(4); }
    if (is_prefix(n, "_Unwind_") || is_prefix(n, "__gnu_Unwind_") || is_prefix(n, "__cxa_")) {
        c->r0 = 0; return;
    }

    /* --- Heap / memory / string --- */
    if (streq(n, "malloc")) { c->r0 = gmalloc(c->r0); return; }
    if (streq(n, "calloc")) {
        uint32_t total = c->r0 * c->r1;
        uint32_t p = gmalloc(total);
        if (p) memset(mem + p, 0, total);
        c->r0 = p;
        return;
    }
    if (streq(n, "realloc")) { c->r0 = grealloc(c->r0, c->r1); return; }
    if (streq(n, "free")) { gfree(c->r0); return; }
    if (streq(n, "memcpy")) { memcpy(hostp(c->r0, c->r2), hostp(c->r1, c->r2), c->r2); return; }
    if (streq(n, "memmove")) { memmove(hostp(c->r0, c->r2), hostp(c->r1, c->r2), c->r2); return; }
    if (streq(n, "memset")) { memset(hostp(c->r0, c->r2), (int)c->r1, c->r2); return; }
    if (streq(n, "memchr")) {
        void *p = memchr(hostp(c->r0, c->r2), (int)c->r1, c->r2);
        c->r0 = p ? (uint32_t)((uint8_t *)p - g_mem) : 0;
        return;
    }
    if (streq(n, "memcmp")) { c->r0 = (uint32_t)memcmp(hostp(c->r0, c->r2), hostp(c->r1, c->r2), c->r2); return; }
    if (streq(n, "strlen")) { c->r0 = (uint32_t)strlen((const char *)hostp(c->r0, 1)); return; }
    if (streq(n, "strcmp")) { c->r0 = (uint32_t)strcmp((const char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1)); return; }
    if (streq(n, "strncmp")) { c->r0 = (uint32_t)strncmp((const char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1), c->r2); return; }
    if (streq(n, "strcasecmp")) { c->r0 = (uint32_t)strcasecmp((const char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1)); return; }
    if (streq(n, "strchr")) {
        char *p = strchr((char *)hostp(c->r0, 1), (int)c->r1);
        c->r0 = p ? (uint32_t)(p - (char *)g_mem) : 0;
        return;
    }
    if (streq(n, "strrchr")) {
        char *p = strrchr((char *)hostp(c->r0, 1), (int)c->r1);
        c->r0 = p ? (uint32_t)(p - (char *)g_mem) : 0;
        return;
    }
    if (streq(n, "strstr")) {
        char *p = strstr((char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1));
        c->r0 = p ? (uint32_t)(p - (char *)g_mem) : 0;
        return;
    }
    if (streq(n, "strcpy")) { strcpy((char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1)); return; }
    if (streq(n, "strncpy")) { strncpy((char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1), c->r2); return; }
    if (streq(n, "strlcpy")) {
        char *d = (char *)hostp(c->r0, 1);
        const char *s = (const char *)hostp(c->r1, 1);
        size_t sz = c->r2;
        size_t sl = s ? strlen(s) : 0;
        if (sz && d) {
            size_t copy = sl >= sz ? sz - 1 : sl;
            if (s) memcpy(d, s, copy);
            d[copy] = '\0';
        }
        c->r0 = (uint32_t)sl;
        return;
    }
    if (streq(n, "strcat")) { strcat((char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1)); return; }
    if (streq(n, "strncat")) {
        char *dst = (char *)hostp(c->r0, 1);
        const char *src = (const char *)hostp(c->r1, 1);
        size_t k = c->r2;
        char *p = dst + strlen(dst);
        while (k-- && *src) *p++ = *src++;
        *p = '\0';
        return;
    }
    if (streq(n, "strdup")) {
        const char *s = (const char *)hostp(c->r0, 1);
        if (!s) { c->r0 = 0; return; }
        size_t len = strlen(s) + 1u;
        uint32_t p = gmalloc((uint32_t)len);
        if (p) memcpy(mem + p, s, len);
        c->r0 = p;
        return;
    }
    if (streq(n, "strtok")) {
        char *s = c->r0 ? (char *)hostp(c->r0, 1) : NULL;
        const char *delim = (const char *)hostp(c->r1, 1);
        char *p = delim ? strtok(s, delim) : NULL;
        c->r0 = p ? (uint32_t)(p - (char *)g_mem) : 0;
        return;
    }
    if (streq(n, "sprintf")) {
        char buf[4096];
        const char *fmt = (const char *)hostp(c->r1, 1);
        if (!fmt) { c->r0 = 0; return; }
        format_va(buf, sizeof(buf), fmt, c, mem, (uint32_t[]){c->r2, c->r3}, 2);
        size_t len = strlen(buf);
        if (c->r0 + len + 1u <= guest_mem_size) memcpy(mem + c->r0, buf, len + 1u);
        c->r0 = (uint32_t)len;
        return;
    }
    if (streq(n, "snprintf")) {
        char buf[4096];
        const char *fmt = (const char *)hostp(c->r2, 1);
        if (!fmt) { c->r0 = 0; return; }
        format_va(buf, sizeof(buf), fmt, c, mem, (uint32_t[]){c->r3}, 1);
        size_t len = strlen(buf);
        if (c->r1 > 0 && c->r0 < guest_mem_size) {
            size_t copy = len >= c->r1 ? c->r1 - 1u : len;
            memcpy(mem + c->r0, buf, copy);
            mem[c->r0 + copy] = 0;
        }
        c->r0 = (uint32_t)len;
        return;
    }
    if (streq(n, "vsprintf")) {
        char buf[4096];
        const char *fmt = (const char *)hostp(c->r1, 1);
        if (!fmt) { c->r0 = 0; return; }
        format_valist(buf, sizeof(buf), fmt, mem, c->r2);
        size_t len = strlen(buf);
        if (c->r0 + len + 1u <= guest_mem_size) memcpy(mem + c->r0, buf, len + 1u);
        c->r0 = (uint32_t)len;
        return;
    }
    if (streq(n, "vsnprintf")) {
        char buf[4096];
        const char *fmt = (const char *)hostp(c->r2, 1);
        if (!fmt) { c->r0 = 0; return; }
        format_valist(buf, sizeof(buf), fmt, mem, c->r3);
        size_t len = strlen(buf);
        if (c->r1 > 0 && c->r0 < guest_mem_size) {
            size_t copy = len >= c->r1 ? c->r1 - 1u : len;
            memcpy(mem + c->r0, buf, copy);
            mem[c->r0 + copy] = 0;
        }
        c->r0 = (uint32_t)len;
        return;
    }
    if (streq(n, "sscanf")) { c->r0 = guest_sscanf(c, mem); return; }
    if (streq(n, "atoi")) { const char *s = (const char *)hostp(c->r0, 1); c->r0 = s ? (uint32_t)(int32_t)atoi(s) : 0; return; }
    if (streq(n, "atol")) { const char *s = (const char *)hostp(c->r0, 1); c->r0 = s ? (uint32_t)(int32_t)atol(s) : 0; return; }
    if (streq(n, "strtol")) {
        const char *s = (const char *)hostp(c->r0, 1);
        char *end = NULL;
        long v = s ? strtol(s, &end, (int)c->r2) : 0;
        if (c->r1) w32(mem, c->r1, end ? (uint32_t)((uint8_t *)end - g_mem) : 0);
        c->r0 = (uint32_t)(int32_t)v;
        return;
    }
    if (streq(n, "strtod")) {
        const char *s = (const char *)hostp(c->r0, 1);
        char *end = NULL;
        double v = s ? strtod(s, &end) : 0.0;
        if (c->r1) w32(mem, c->r1, end ? (uint32_t)((uint8_t *)end - g_mem) : 0);
        f64_to(v, &c->r0, &c->r1);
        return;
    }
    if (streq(n, "qsort")) {
        if (c->r1 > 1u && c->r2 > 0u && c->r3 && c->r0 + c->r1 * c->r2 <= guest_mem_size) {
            Cpu *prev_cpu = g_qsort_cpu;
            uint32_t prev_cmp = g_qsort_cmp, prev_base = g_qsort_scratch_base;
            g_qsort_cpu = c; g_qsort_cmp = c->r3; g_qsort_scratch_base = c->r0;
            qsort(mem + c->r0, c->r1, c->r2, guest_qsort_cmp);
            g_qsort_cpu = prev_cpu; g_qsort_cmp = prev_cmp; g_qsort_scratch_base = prev_base;
        }
        return;
    }
    if (streq(n, "div")) {
        /* GCC/Bionic ARM EABI returns 8-byte div_t via hidden sret pointer in r0 */
        uint32_t out = c->r0;
        int32_t num = (int32_t)c->r1, den = (int32_t)c->r2;
        int32_t q = den ? (num / den) : 0;
        int32_t r = den ? (num % den) : 0;
        if (out && guest_ok(out, 8u)) {
            w32(mem, out + 0u, (uint32_t)q);
            w32(mem, out + 4u, (uint32_t)r);
        }
        return;
    }
    if (streq(n, "basename")) {
        char *p = (char *)hostp(c->r0, 1);
        if (!p) { c->r0 = 0; return; }
        char *sl = strrchr(p, '/');
        c->r0 = sl ? (uint32_t)((sl + 1) - (char *)g_mem) : c->r0;
        return;
    }

    /* --- wide char (Android wchar_t is 32-bit, Windows wchar_t is 16-bit! Handle directly in uint32_t.) --- */
    if (streq(n, "wcslen")) {
        uint32_t p = c->r0, len = 0;
        while (p + len * 4u + 4u <= guest_mem_size && r32(mem, p + len * 4u) != 0) len++;
        c->r0 = len;
        return;
    }
    if (streq(n, "wcscpy")) {
        uint32_t dst = c->r0, src = c->r1, i = 0;
        while (src + i * 4u + 4u <= guest_mem_size && dst + i * 4u + 4u <= guest_mem_size) {
            uint32_t ch = r32(mem, src + i * 4u);
            w32(mem, dst + i * 4u, ch);
            if (!ch) break;
            i++;
        }
        return;
    }
    if (streq(n, "wcsncpy")) {
        uint32_t dst = c->r0, src = c->r1, nmax = c->r2, i = 0;
        for (; i < nmax && dst + i * 4u + 4u <= guest_mem_size; i++) {
            uint32_t ch = (src + i * 4u + 4u <= guest_mem_size) ? r32(mem, src + i * 4u) : 0;
            w32(mem, dst + i * 4u, ch);
            if (!ch) {
                for (i++; i < nmax && dst + i * 4u + 4u <= guest_mem_size; i++) w32(mem, dst + i * 4u, 0);
                break;
            }
        }
        return;
    }
    if (streq(n, "wcscmp")) {
        uint32_t a = c->r0, b = c->r1, i = 0;
        for (;; i++) {
            uint32_t ca = (a + i * 4u + 4u <= guest_mem_size) ? r32(mem, a + i * 4u) : 0;
            uint32_t cb = (b + i * 4u + 4u <= guest_mem_size) ? r32(mem, b + i * 4u) : 0;
            if (ca != cb) { c->r0 = (ca < cb) ? 0xFFFFFFFFu : 1u; return; }
            if (!ca) { c->r0 = 0; return; }
        }
    }
    if (streq(n, "iswprint")) { c->r0 = iswprint((wint_t)c->r0) ? 1u : 0u; return; }
    if (streq(n, "iswpunct")) { c->r0 = iswpunct((wint_t)c->r0) ? 1u : 0u; return; }
    if (streq(n, "iswxdigit")) { c->r0 = iswxdigit((wint_t)c->r0) ? 1u : 0u; return; }
    if (streq(n, "iswalpha")) { c->r0 = iswalpha((wint_t)c->r0) ? 1u : 0u; return; }
    if (streq(n, "iswspace")) { c->r0 = iswspace((wint_t)c->r0) ? 1u : 0u; return; }
    if (streq(n, "iswlower")) { c->r0 = iswlower((wint_t)c->r0) ? 1u : 0u; return; }
    if (streq(n, "iswupper")) { c->r0 = iswupper((wint_t)c->r0) ? 1u : 0u; return; }
    if (streq(n, "iswcntrl")) { c->r0 = iswcntrl((wint_t)c->r0) ? 1u : 0u; return; }
    if (streq(n, "iswdigit")) { c->r0 = iswdigit((wint_t)c->r0) ? 1u : 0u; return; }
    if (streq(n, "towlower")) { c->r0 = (uint32_t)towlower((wint_t)c->r0); return; }
    if (streq(n, "towupper")) { c->r0 = (uint32_t)towupper((wint_t)c->r0); return; }

    /* --- 48-bit pseudo-random --- */
    {
        static uint64_t s48 = 0x1234ABCD330EULL;
        if (streq(n, "lrand48")) {
            s48 = (s48 * 0x5DEECE66DULL + 0xBULL) & 0xFFFFFFFFFFFFULL;
            c->r0 = (uint32_t)(s48 >> 17);
            return;
        }
        if (streq(n, "srand48")) {
            s48 = (((uint64_t)(uint32_t)c->r0) << 16) | 0x330EULL;
            return;
        }
    }

    /* --- time --- */
    if (streq(n, "gettimeofday")) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        if (c->r0 && guest_ok(c->r0, 8)) {
            w32(mem, c->r0, (uint32_t)tv.tv_sec);
            w32(mem, c->r0 + 4u, (uint32_t)tv.tv_usec);
        }
        if (c->r1 && guest_ok(c->r1, 8)) { w32(mem, c->r1, 0); w32(mem, c->r1 + 4u, 0); }
        c->r0 = 0;
        return;
    }
    if (streq(n, "time")) {
        time_t t = time(NULL);
        if (c->r0 && guest_ok(c->r0, 4)) w32(mem, c->r0, (uint32_t)t);
        c->r0 = (uint32_t)t;
        return;
    }
    if (streq(n, "clock")) { c->r0 = (uint32_t)clock(); return; }
    if (streq(n, "localtime_r") || streq(n, "gmtime_r")) {
        time_t tt = (time_t)(int32_t)r32(mem, c->r0);
        struct tm out;
        memset(&out, 0, sizeof(out));
        struct tm *p = streq(n, "gmtime_r") ? gmtime(&tt) : localtime(&tt);
        if (p) out = *p;
        uint32_t dst = c->r1;
        w32(mem, dst + 0u, (uint32_t)out.tm_sec);
        w32(mem, dst + 4u, (uint32_t)out.tm_min);
        w32(mem, dst + 8u, (uint32_t)out.tm_hour);
        w32(mem, dst + 12u, (uint32_t)out.tm_mday);
        w32(mem, dst + 16u, (uint32_t)out.tm_mon);
        w32(mem, dst + 20u, (uint32_t)out.tm_year);
        w32(mem, dst + 24u, (uint32_t)out.tm_wday);
        w32(mem, dst + 28u, (uint32_t)out.tm_yday);
        w32(mem, dst + 32u, (uint32_t)out.tm_isdst);
        c->r0 = dst;
        return;
    }
    if (streq(n, "localtime") || streq(n, "gmtime")) {
        time_t tt = (time_t)(int32_t)r32(mem, c->r0);
        struct tm *p = streq(n, "gmtime") ? gmtime(&tt) : localtime(&tt);
        uint32_t dst = 0x1202600u;
        if (p && guest_ok(dst, 36)) {
            w32(mem, dst + 0u, (uint32_t)p->tm_sec);
            w32(mem, dst + 4u, (uint32_t)p->tm_min);
            w32(mem, dst + 8u, (uint32_t)p->tm_hour);
            w32(mem, dst + 12u, (uint32_t)p->tm_mday);
            w32(mem, dst + 16u, (uint32_t)p->tm_mon);
            w32(mem, dst + 20u, (uint32_t)p->tm_year);
            w32(mem, dst + 24u, (uint32_t)p->tm_wday);
            w32(mem, dst + 28u, (uint32_t)p->tm_yday);
            w32(mem, dst + 32u, (uint32_t)p->tm_isdst);
            c->r0 = dst;
        } else {
            c->r0 = 0;
        }
        return;
    }
    if (streq(n, "mktime")) {
        uint32_t src = c->r0;
        struct tm t;
        memset(&t, 0, sizeof(t));
        t.tm_sec   = (int)r32(mem, src + 0u);
        t.tm_min   = (int)r32(mem, src + 4u);
        t.tm_hour  = (int)r32(mem, src + 8u);
        t.tm_mday  = (int)r32(mem, src + 12u);
        t.tm_mon   = (int)r32(mem, src + 16u);
        t.tm_year  = (int)r32(mem, src + 20u);
        t.tm_isdst = (int)r32(mem, src + 32u);
        c->r0 = (uint32_t)mktime(&t);
        return;
    }

    /* --- stdio / POSIX / VFS --- */
    if (streq(n, "fopen")) {
        c->r0 = vfs_open((const char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1));
        return;
    }
    if (streq(n, "fread") || streq(n, "fwrite")) {
        uint64_t total = (uint64_t)c->r1 * c->r2;
        void *buf = hostp(c->r0, (uint32_t)total);
        if (!buf) { c->r0 = 0; return; }
        c->r0 = streq(n, "fread") ? vfs_read(c->r3, buf, c->r1, c->r2)
                                  : vfs_write(c->r3, buf, c->r1, c->r2);
        return;
    }
    if (streq(n, "fclose")) { c->r0 = (uint32_t)vfs_close(c->r0); return; }
    if (streq(n, "fseek")) { c->r0 = (uint32_t)vfs_seek(c->r0, (int32_t)c->r1, (int)c->r2); return; }
    if (streq(n, "ftell")) { c->r0 = (uint32_t)vfs_tell(c->r0); return; }
    if (streq(n, "fflush")) { c->r0 = (uint32_t)vfs_flush(c->r0); return; }
    if (streq(n, "fgetpos")) {
        int32_t pos = vfs_tell(c->r0);
        if (pos >= 0 && c->r1 && guest_ok(c->r1, 4)) { w32(mem, c->r1, (uint32_t)pos); c->r0 = 0; }
        else c->r0 = 0xFFFFFFFFu;
        return;
    }
    if (streq(n, "fsetpos")) {
        int32_t pos = c->r1 ? (int32_t)r32(mem, c->r1) : -1;
        c->r0 = (pos >= 0) ? (uint32_t)vfs_seek(c->r0, pos, SEEK_SET) : 0xFFFFFFFFu;
        return;
    }
    if (streq(n, "fgetc") || streq(n, "getc")) { c->r0 = (uint32_t)vfs_getc(c->r0); return; }
    if (streq(n, "ungetc")) { c->r0 = (uint32_t)vfs_ungetc((int)c->r0, c->r1); return; }
    if (streq(n, "fputc") || streq(n, "putc")) { c->r0 = (uint32_t)vfs_putc((int)c->r0, c->r1); return; }
    if (streq(n, "fgets")) {
        char *dst = (char *)hostp(c->r0, c->r1);
        c->r0 = dst ? (uint32_t)(vfs_gets(dst, (int)c->r1, c->r2) ? c->r0 : 0) : 0;
        return;
    }
    if (streq(n, "fputs")) {
        const char *s = (const char *)hostp(c->r0, 1);
        c->r0 = s ? (uint32_t)vfs_puts(s, c->r1) : 0;
        return;
    }
    if (streq(n, "fileno")) { c->r0 = (uint32_t)vfs_fileno(c->r0); return; }
    if (streq(n, "fdopen")) { c->r0 = vfs_fdopen((int)c->r0, (const char *)hostp(c->r1, 1)); return; }
    if (streq(n, "open")) { c->r0 = (uint32_t)vfs_open_fd((const char *)hostp(c->r0, 1), (int)c->r1); return; }
    if (streq(n, "stat")) { c->r0 = (uint32_t)vfs_stat((const char *)hostp(c->r0, 1), hostp(c->r1, 96)); return; }
    if (streq(n, "fstat")) { c->r0 = (uint32_t)vfs_fstat_fd((int)c->r0, hostp(c->r1, 96)); return; }
    if (streq(n, "lseek")) { c->r0 = (uint32_t)vfs_lseek_fd((int)c->r0, (int32_t)c->r1, (int)c->r2); return; }
    if (streq(n, "read")) {
        void *dst = hostp(c->r1, c->r2);
        c->r0 = dst ? (uint32_t)vfs_read_fd((int)c->r0, dst, c->r2) : 0xFFFFFFFFu;
        return;
    }
    if (streq(n, "write")) {
        const void *src = hostp(c->r1, c->r2);
        c->r0 = src ? (uint32_t)vfs_write_fd((int)c->r0, src, c->r2) : 0xFFFFFFFFu;
        return;
    }
    if (streq(n, "close")) { c->r0 = (uint32_t)vfs_close_fd((int)c->r0); return; }
    if (streq(n, "chdir")) { vfs_chdir((const char *)hostp(c->r0, 1)); c->r0 = 0; return; }
    if (streq(n, "setvbuf")) { c->r0 = 0; return; }
    if (streq(n, "mmap")) {
        uint32_t len = c->r1;
        uint32_t fd = r32(mem, c->sp);
        uint32_t off = r32(mem, c->sp + 4u);
        uint32_t p = gmalloc(len);
        if (!p) { c->r0 = 0xFFFFFFFFu; return; }
        memset(mem + p, 0, len);
        if ((int32_t)fd >= 0) {
            vfs_lseek_fd((int)fd, (int32_t)off, SEEK_SET);
            vfs_read_fd((int)fd, mem + p, len);
        }
        c->r0 = p;
        return;
    }
    if (streq(n, "munmap")) { gfree(c->r0); c->r0 = 0; return; }

    /* --- env / misc POSIX --- */
    if (streq(n, "__errno")) { c->r0 = g_errno; return; }
    if (streq(n, "getenv")) { c->r0 = 0; return; }
    if (streq(n, "setlocale")) { c->r0 = 0; return; }
    if (streq(n, "sysconf")) { c->r0 = 4096; return; }
    if (streq(n, "__assert")) {
        const char *file = (c->r0 && c->r0 < guest_mem_size) ? (const char *)(mem + c->r0) : "?";
        const char *expr = (c->r2 && c->r2 < guest_mem_size) ? (const char *)(mem + c->r2) : "?";
        fprintf(stderr, "import: __assert FAILED at %s:%u expr='%s'\n", file, c->r1, expr);
        exit(3);
    }
    if (streq(n, "exit") || streq(n, "_exit") || streq(n, "abort") ||
        streq(n, "__stack_chk_fail")) {
        fprintf(stderr, "import: %s(r0=0x%08x) sp=0x%08x lr=0x%08x -> terminating\n",
                n, c->r0, c->sp, c->lr);
        guest_dump_call_ring();
        if (GetFileAttributesA("build") != INVALID_FILE_ATTRIBUTES &&
            freopen("build/asphalt6_crash.log", "a", stderr)) {
            fprintf(stderr, "=== import: %s(r0=0x%08x) sp=0x%08x lr=0x%08x ===\n",
                    n, c->r0, c->sp, c->lr);
            guest_dump_call_ring();
            fflush(stderr);
        }
        exit(3);
    }

    /* --- pthread --- */
#ifdef _WIN32
    if (streq(n, "pthread_create")) { c->r0 = (uint32_t)thread_win32_create(mem, guest_mem_size, c->r0, c->r2, c->r3); return; }
    if (streq(n, "pthread_join"))   { c->r0 = (uint32_t)thread_win32_join(mem, c->r0, c->r1); return; }
    if (streq(n, "pthread_self"))   { c->r0 = thread_win32_self(); return; }
    if (streq(n, "pthread_exit"))   { ExitThread(0); return; }
    if (streq(n, "pthread_mutex_init"))    { c->r0 = (uint32_t)thread_win32_mutex_init(mem, guest_mem_size, c->r0); return; }
    if (streq(n, "pthread_mutex_lock"))    { c->r0 = (uint32_t)thread_win32_mutex_lock(mem, guest_mem_size, c->r0); return; }
    if (streq(n, "pthread_mutex_trylock")) { c->r0 = (uint32_t)thread_win32_mutex_trylock(mem, guest_mem_size, c->r0); return; }
    if (streq(n, "pthread_mutex_unlock"))  { c->r0 = (uint32_t)thread_win32_mutex_unlock(mem, guest_mem_size, c->r0); return; }
    if (streq(n, "pthread_mutex_destroy")) { c->r0 = (uint32_t)thread_win32_mutex_destroy(mem, guest_mem_size, c->r0); return; }
    if (streq(n, "pthread_cond_init"))      { c->r0 = (uint32_t)thread_win32_cond_init(mem, guest_mem_size, c->r0); return; }
    if (streq(n, "pthread_cond_signal"))    { c->r0 = (uint32_t)thread_win32_cond_signal(mem, guest_mem_size, c->r0); return; }
    if (streq(n, "pthread_cond_broadcast")) { c->r0 = (uint32_t)thread_win32_cond_broadcast(mem, guest_mem_size, c->r0); return; }
    if (streq(n, "pthread_cond_wait"))      { c->r0 = (uint32_t)thread_win32_cond_wait(mem, guest_mem_size, c->r0, c->r1); return; }
    if (streq(n, "pthread_cond_timedwait")) { c->r0 = (uint32_t)thread_win32_cond_timedwait(mem, guest_mem_size, c->r0, c->r1, c->r2); return; }
    if (streq(n, "pthread_cond_destroy"))   { c->r0 = (uint32_t)thread_win32_cond_destroy(mem, guest_mem_size, c->r0); return; }
    if (streq(n, "pthread_key_create"))     { c->r0 = (uint32_t)thread_win32_key_create(mem, guest_mem_size, c->r0, c->r1); return; }
    if (streq(n, "pthread_getspecific"))    { c->r0 = thread_win32_getspecific(c->r0); return; }
    if (streq(n, "pthread_setspecific"))    { c->r0 = (uint32_t)thread_win32_setspecific(c->r0, c->r1); return; }
    if (streq(n, "usleep")) {
        uint32_t us = c->r0;
        if (us < 1000u) SwitchToThread();
        else Sleep((DWORD)((us + 999u) / 1000u));
        c->r0 = 0;
        return;
    }
    if (streq(n, "nanosleep")) {
        uint32_t req = c->r0;
        uint64_t us = 0;
        if (req && guest_ok(req, 8)) {
            us = (uint64_t)r32(mem, req) * 1000000u + (uint64_t)r32(mem, req + 4u) / 1000u;
        }
        if (us < 1000u) SwitchToThread();
        else Sleep((DWORD)((us + 999u) / 1000u));
        c->r0 = 0;
        return;
    }
#endif
    if (is_prefix(n, "pthread_") || is_prefix(n, "sched_")) { c->r0 = 0; return; }

    /* --- logging --- */
    if (streq(n, "__android_log_print")) {
        char buf[2048];
        const char *tag = (const char *)hostp(c->r1, 1);
        const char *fmt = (const char *)hostp(c->r2, 1);
        if (fmt) format_va(buf, sizeof(buf), fmt, c, mem, (uint32_t[]){c->r3}, 1);
        else buf[0] = '\0';
        fprintf(stderr, "[android] %s: %s\n", tag ? tag : "?", buf);
        c->r0 = 0;
        return;
    }
    if (streq(n, "printf")) {
        char buf[2048];
        const char *fmt = (const char *)hostp(c->r0, 1);
        if (fmt) { format_va(buf, sizeof(buf), fmt, c, mem, (uint32_t[]){c->r1, c->r2, c->r3}, 3); fputs(buf, stderr); }
        c->r0 = 0;
        return;
    }
    if (streq(n, "fprintf")) {
        char buf[2048];
        const char *fmt = (const char *)hostp(c->r1, 1);
        if (fmt) { format_va(buf, sizeof(buf), fmt, c, mem, (uint32_t[]){c->r2, c->r3}, 2); fputs(buf, stderr); }
        c->r0 = 0;
        return;
    }
    if (streq(n, "puts")) {
        const char *s = (const char *)hostp(c->r0, 1);
        if (s) fprintf(stderr, "%s\n", s);
        c->r0 = 0;
        return;
    }
    if (streq(n, "putchar")) { fputc((int)c->r0, stderr); c->r0 = c->r0; return; }

    /* --- math --- */
    if (streq(n, "sqrtf"))  { c->r0 = f32_bits(sqrtf(f32_of(c->r0))); return; }
    if (streq(n, "sinf"))   { c->r0 = f32_bits(sinf(f32_of(c->r0))); return; }
    if (streq(n, "cosf"))   { c->r0 = f32_bits(cosf(f32_of(c->r0))); return; }
    if (streq(n, "tanf"))   { c->r0 = f32_bits(tanf(f32_of(c->r0))); return; }
    if (streq(n, "asinf"))  { c->r0 = f32_bits(asinf(f32_of(c->r0))); return; }
    if (streq(n, "acosf"))  { c->r0 = f32_bits(acosf(f32_of(c->r0))); return; }
    if (streq(n, "atanf"))  { c->r0 = f32_bits(atanf(f32_of(c->r0))); return; }
    if (streq(n, "atan2f")) { c->r0 = f32_bits(atan2f(f32_of(c->r0), f32_of(c->r1))); return; }
    if (streq(n, "powf"))   { c->r0 = f32_bits(powf(f32_of(c->r0), f32_of(c->r1))); return; }
    if (streq(n, "expf"))   { c->r0 = f32_bits(expf(f32_of(c->r0))); return; }
    if (streq(n, "logf"))   { c->r0 = f32_bits(logf(f32_of(c->r0))); return; }
    if (streq(n, "ceilf"))  { c->r0 = f32_bits(ceilf(f32_of(c->r0))); return; }
    if (streq(n, "floorf")) { c->r0 = f32_bits(floorf(f32_of(c->r0))); return; }
    if (streq(n, "fmodf"))  { c->r0 = f32_bits(fmodf(f32_of(c->r0), f32_of(c->r1))); return; }
    if (streq(n, "ldexpf")) { c->r0 = f32_bits(ldexpf(f32_of(c->r0), (int32_t)c->r1)); return; }

    if (streq(n, "sin"))   { f64_to(sin(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "cos"))   { f64_to(cos(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "tan"))   { f64_to(tan(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "asin"))  { f64_to(asin(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "acos"))  { f64_to(acos(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "atan"))  { f64_to(atan(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "atan2")) { f64_to(atan2(f64_of(c->r0, c->r1), f64_of(c->r2, c->r3)), &c->r0, &c->r1); return; }
    if (streq(n, "sqrt"))  { f64_to(sqrt(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "pow"))   { f64_to(pow(f64_of(c->r0, c->r1), f64_of(c->r2, c->r3)), &c->r0, &c->r1); return; }
    if (streq(n, "exp"))   { f64_to(exp(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "log"))   { f64_to(log(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "log10")) { f64_to(log10(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "floor")) { f64_to(floor(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "ceil"))  { f64_to(ceil(f64_of(c->r0, c->r1)), &c->r0, &c->r1); return; }
    if (streq(n, "fmod"))  { f64_to(fmod(f64_of(c->r0, c->r1), f64_of(c->r2, c->r3)), &c->r0, &c->r1); return; }
    if (streq(n, "fmin"))  { f64_to(fmin(f64_of(c->r0, c->r1), f64_of(c->r2, c->r3)), &c->r0, &c->r1); return; }
    if (streq(n, "fmax"))  { f64_to(fmax(f64_of(c->r0, c->r1), f64_of(c->r2, c->r3)), &c->r0, &c->r1); return; }

    /* --- GLES2 bridge / fallback stubs --- */
    if (n[0] == 'g' && n[1] == 'l' && gl_bridge_call(c, mem, n)) return;
    if (streq(n, "glGetError")) { c->r0 = 0; return; }
    if (streq(n, "glGetString")) {
        uint32_t a = 0;
        switch (c->r0) {
        case 0x1F00: a = 0x1219100u; put_str(g_mem, a, "Imagination Technologies"); break;
        case 0x1F01: a = 0x1219200u; put_str(g_mem, a, "PowerVR SGX 540"); break;
        case 0x1F02: a = 0x1219000u; put_str(g_mem, a, "OpenGL ES 2.0"); break;
        case 0x1F03: a = 0x1219300u; put_str(g_mem, a,
            "GL_OES_texture_npot GL_OES_compressed_ETC1_RGB8_texture "
            "GL_AMD_compressed_ATC_texture GL_EXT_texture_compression_dxt1"); break;
        case 0x8B8C: a = 0x1219400u; put_str(g_mem, a, "OpenGL ES GLSL ES 1.00"); break;
        default: break;
        }
        c->r0 = a;
        return;
    }
    if (streq(n, "glGetIntegerv")) {
        int sw = platform_width(), sh = platform_height();
        uint32_t pname = c->r0, dst = c->r1;
        if (pname == 0x0D3A) { w32(mem, dst, (uint32_t)sw); w32(mem, dst + 4u, (uint32_t)sh); }
        else if (pname == 0x0BA2 || pname == 0x0C10) {
            w32(mem, dst, 0); w32(mem, dst + 4u, 0);
            w32(mem, dst + 8u, (uint32_t)sw); w32(mem, dst + 12u, (uint32_t)sh);
        } else {
            w32(mem, dst, 2048u);
        }
        return;
    }
    if (streq(n, "glCreateShader") || streq(n, "glCreateProgram")) { c->r0 = g_gl_id++; return; }
    if (streq(n, "glGetShaderiv") || streq(n, "glGetProgramiv")) {
        w32(mem, c->r2, (c->r1 == 0x8B81 || c->r1 == 0x8B82) ? 1u : 0u);
        return;
    }
    if (streq(n, "glGetAttribLocation") || streq(n, "glGetUniformLocation")) { c->r0 = 0xFFFFFFFFu; return; }
    if (streq(n, "glGenBuffers") || streq(n, "glGenTextures") ||
        streq(n, "glGenFramebuffers") || streq(n, "glGenRenderbuffers")) {
        for (uint32_t i = 0; i < c->r0; i++) w32(mem, c->r1 + i * 4u, g_gl_id++);
        return;
    }
    if (streq(n, "glCheckFramebufferStatus")) { c->r0 = 0x8CD5u; return; }

    /* Default logged no-op */
    c->r0 = 0;
}

/* Hot O(1) PLT fast-path handlers (eliminates binary search + strcmp ladder per call) */
typedef void (*PltHandlerFn)(Cpu *c, uint8_t *mem);
static PltHandlerFn g_plt_fn[IMPORT_COUNT];
static uint8_t g_plt_state[IMPORT_COUNT]; /* 0 = unresolved, 1 = direct g_plt_fn, 2 = slow fallback */

static void plt_malloc(Cpu *c, uint8_t *m)  { (void)m; c->r0 = gmalloc(c->r0); }
static void plt_free(Cpu *c, uint8_t *m)    { (void)m; gfree(c->r0); }
static void plt_calloc(Cpu *c, uint8_t *m)  {
    uint32_t total = c->r0 * c->r1;
    uint32_t p = gmalloc(total);
    if (p) memset(m + p, 0, total);
    c->r0 = p;
}
static void plt_realloc(Cpu *c, uint8_t *m) { (void)m; c->r0 = grealloc(c->r0, c->r1); }
static void plt_memcpy(Cpu *c, uint8_t *m)  { (void)m; memcpy(hostp(c->r0, c->r2), hostp(c->r1, c->r2), c->r2); }
static void plt_memmove(Cpu *c, uint8_t *m) { (void)m; memmove(hostp(c->r0, c->r2), hostp(c->r1, c->r2), c->r2); }
static void plt_memset(Cpu *c, uint8_t *m)  { (void)m; memset(hostp(c->r0, c->r2), (int)c->r1, c->r2); }
static void plt_memcmp(Cpu *c, uint8_t *m)  { (void)m; c->r0 = (uint32_t)memcmp(hostp(c->r0, c->r2), hostp(c->r1, c->r2), c->r2); }
static void plt_strlen(Cpu *c, uint8_t *m)  { (void)m; c->r0 = (uint32_t)strlen((const char *)hostp(c->r0, 1)); }
static void plt_strcmp(Cpu *c, uint8_t *m)     { (void)m; c->r0 = (uint32_t)strcmp((const char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1)); }
static void plt_strncmp(Cpu *c, uint8_t *m)    { (void)m; c->r0 = (uint32_t)strncmp((const char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1), c->r2); }
static void plt_strcasecmp(Cpu *c, uint8_t *m) { (void)m; c->r0 = (uint32_t)strcasecmp((const char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1)); }
static void plt_strcpy(Cpu *c, uint8_t *m)  { (void)m; strcpy((char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1)); }
static void plt_strncpy(Cpu *c, uint8_t *m) { (void)m; strncpy((char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1), c->r2); }
static void plt_strchr(Cpu *c, uint8_t *m)  { (void)m; char *p = strchr((char *)hostp(c->r0, 1), (int)c->r1); c->r0 = p ? (uint32_t)(p - (char *)g_mem) : 0; }
static void plt_strrchr(Cpu *c, uint8_t *m) { (void)m; char *p = strrchr((char *)hostp(c->r0, 1), (int)c->r1); c->r0 = p ? (uint32_t)(p - (char *)g_mem) : 0; }
static void plt_strstr(Cpu *c, uint8_t *m)  { (void)m; char *p = strstr((char *)hostp(c->r0, 1), (const char *)hostp(c->r1, 1)); c->r0 = p ? (uint32_t)(p - (char *)g_mem) : 0; }
static void plt_atoi(Cpu *c, uint8_t *m)    { (void)m; const char *s = (const char *)hostp(c->r0, 1); c->r0 = s ? (uint32_t)(int32_t)atoi(s) : 0; }
static void plt_errno(Cpu *c, uint8_t *m)   { (void)m; c->r0 = g_errno; }
static void plt_cxa_guard_acquire(Cpu *c, uint8_t *m) {
    uint32_t g = c->r0;
    c->r0 = (g && guest_ok(g, 4) && r32(m, g) == 0) ? 1u : 0u;
}
static void plt_cxa_guard_release(Cpu *c, uint8_t *m) {
    if (c->r0 && guest_ok(c->r0, 4)) w32(m, c->r0, 1u);
}
static void plt_gettimeofday(Cpu *c, uint8_t *m) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (c->r0 && guest_ok(c->r0, 8)) {
        w32(m, c->r0, (uint32_t)tv.tv_sec);
        w32(m, c->r0 + 4u, (uint32_t)tv.tv_usec);
    }
    if (c->r1 && guest_ok(c->r1, 8)) { w32(m, c->r1, 0); w32(m, c->r1 + 4u, 0); }
    c->r0 = 0;
}
static void plt_clock(Cpu *c, uint8_t *m) { (void)m; c->r0 = (uint32_t)clock(); }

#ifdef _WIN32
static void plt_pthread_self(Cpu *c, uint8_t *m)  { (void)m; c->r0 = thread_win32_self(); }
static void plt_mutex_lock(Cpu *c, uint8_t *m)    { c->r0 = (uint32_t)thread_win32_mutex_lock(m, guest_mem_size, c->r0); }
static void plt_mutex_trylock(Cpu *c, uint8_t *m) { c->r0 = (uint32_t)thread_win32_mutex_trylock(m, guest_mem_size, c->r0); }
static void plt_mutex_unlock(Cpu *c, uint8_t *m)  { c->r0 = (uint32_t)thread_win32_mutex_unlock(m, guest_mem_size, c->r0); }
static void plt_getspecific(Cpu *c, uint8_t *m)   { (void)m; c->r0 = thread_win32_getspecific(c->r0); }
static void plt_setspecific(Cpu *c, uint8_t *m)   { (void)m; c->r0 = (uint32_t)thread_win32_setspecific(c->r0, c->r1); }
#endif

#define DEF_PLT_F32_1(fn) static void plt_##fn(Cpu *c, uint8_t *m) { (void)m; c->r0 = f32_bits(fn(f32_of(c->r0))); }
#define DEF_PLT_F32_2(fn) static void plt_##fn(Cpu *c, uint8_t *m) { (void)m; c->r0 = f32_bits(fn(f32_of(c->r0), f32_of(c->r1))); }
#define DEF_PLT_F64_1(fn) static void plt_##fn(Cpu *c, uint8_t *m) { (void)m; f64_to(fn(f64_of(c->r0, c->r1)), &c->r0, &c->r1); }
#define DEF_PLT_F64_2(fn) static void plt_##fn(Cpu *c, uint8_t *m) { (void)m; f64_to(fn(f64_of(c->r0, c->r1), f64_of(c->r2, c->r3)), &c->r0, &c->r1); }

DEF_PLT_F32_1(sqrtf)  DEF_PLT_F32_1(sinf)  DEF_PLT_F32_1(cosf)  DEF_PLT_F32_1(tanf)
DEF_PLT_F32_1(asinf)  DEF_PLT_F32_1(acosf) DEF_PLT_F32_1(atanf)
DEF_PLT_F32_1(expf)   DEF_PLT_F32_1(logf)  DEF_PLT_F32_1(ceilf) DEF_PLT_F32_1(floorf)
DEF_PLT_F32_2(atan2f) DEF_PLT_F32_2(powf)  DEF_PLT_F32_2(fmodf)
DEF_PLT_F64_1(sqrt)   DEF_PLT_F64_1(sin)   DEF_PLT_F64_1(cos)   DEF_PLT_F64_1(tan)
DEF_PLT_F64_1(exp)    DEF_PLT_F64_1(log)   DEF_PLT_F64_1(log10)
DEF_PLT_F64_1(floor)  DEF_PLT_F64_1(ceil)
DEF_PLT_F64_2(atan2)  DEF_PLT_F64_2(pow)   DEF_PLT_F64_2(fmod)
DEF_PLT_F64_2(fmin)   DEF_PLT_F64_2(fmax)

static PltHandlerFn resolve_plt_direct(const char *n) {
    if (n[0] == 'g' && n[1] == 'l') {
        GlHandlerFn glfn = gl_bridge_lookup(n);
        if (glfn) return glfn;
    }
    static const struct { const char *name; PltHandlerFn fn; } k_fast[] = {
        { "_ZnajRKSt9nothrow_t", plt_malloc },
        { "__cxa_guard_acquire", plt_cxa_guard_acquire },
        { "__cxa_guard_release", plt_cxa_guard_release },
        { "__errno",             plt_errno },
        { "malloc",              plt_malloc },
        { "calloc",              plt_calloc },
        { "realloc",             plt_realloc },
        { "free",                plt_free },
        { "memcpy",              plt_memcpy },
        { "memmove",             plt_memmove },
        { "memset",              plt_memset },
        { "memcmp",              plt_memcmp },
        { "strlen",              plt_strlen },
        { "strcmp",              plt_strcmp },
        { "strncmp",             plt_strncmp },
        { "strcasecmp",          plt_strcasecmp },
        { "strcpy",              plt_strcpy },
        { "strncpy",             plt_strncpy },
        { "strchr",              plt_strchr },
        { "strrchr",             plt_strrchr },
        { "strstr",              plt_strstr },
        { "atoi",                plt_atoi },
        { "gettimeofday",        plt_gettimeofday },
        { "clock",               plt_clock },
#ifdef _WIN32
        { "pthread_self",          plt_pthread_self },
        { "pthread_mutex_lock",    plt_mutex_lock },
        { "pthread_mutex_trylock", plt_mutex_trylock },
        { "pthread_mutex_unlock",  plt_mutex_unlock },
        { "pthread_getspecific",   plt_getspecific },
        { "pthread_setspecific",   plt_setspecific },
#endif
        { "sqrtf",  plt_sqrtf },  { "sinf",   plt_sinf },   { "cosf",   plt_cosf },  { "tanf",  plt_tanf },
        { "asinf",  plt_asinf },  { "acosf",  plt_acosf },  { "atanf",  plt_atanf }, { "atan2f", plt_atan2f },
        { "powf",   plt_powf },   { "expf",   plt_expf },   { "logf",   plt_logf },
        { "ceilf",  plt_ceilf },  { "floorf", plt_floorf }, { "fmodf",  plt_fmodf },
        { "sqrt",   plt_sqrt },   { "sin",    plt_sin },    { "cos",    plt_cos },   { "tan",   plt_tan },
        { "exp",    plt_exp },    { "log",    plt_log },    { "log10",  plt_log10 },
        { "atan2",  plt_atan2 },  { "pow",    plt_pow },    { "floor",  plt_floor }, { "ceil",  plt_ceil },
        { "fmod",   plt_fmod },   { "fmin",   plt_fmin },   { "fmax",   plt_fmax },
    };
    size_t count = sizeof(k_fast) / sizeof(k_fast[0]);
    for (size_t i = 0; i < count; i++) {
        if (streq(k_fast[i].name, n)) return k_fast[i].fn;
    }
    return NULL;
}

static __attribute__((noinline)) void call_import_resolve(Cpu *c, uint8_t *mem, uint32_t idx, uint32_t plt) {
    const ImportEntry *e = &k_import_table[idx];
    if (!g_seen[idx]) {
        g_seen[idx] = 1;
        if (g_verbose < 0) g_verbose = getenv("ASPHALT6_IMPORT_TRACE") != NULL;
        if (g_verbose) {
            fprintf(stderr, "import: %s (0x%08x) r0=%08x r1=%08x\n",
                    e->name, plt, c->r0, c->r1);
        }
    }
    PltHandlerFn direct = resolve_plt_direct(e->name);
    if (direct) {
        g_plt_fn[idx] = direct;
        g_plt_state[idx] = 1u;
        direct(c, mem);
        return;
    }
    g_plt_state[idx] = 2u;
    import_dispatch(c, mem, e->name);
}

void call_import(Cpu *c, uint8_t *mem, uint32_t plt) {
    g_mem = mem;
    if (__builtin_expect(plt >= PLT_BASE, 1)) {
        uint32_t off = plt - PLT_BASE;
        if (__builtin_expect((off % PLT_STRIDE) == 0u, 1)) {
            uint32_t idx = off / PLT_STRIDE;
            if (__builtin_expect(idx < IMPORT_COUNT, 1)) {
                uint8_t st = g_plt_state[idx];
                if (__builtin_expect(st == 1u, 1)) {
                    g_plt_fn[idx](c, mem);
                    return;
                }
                if (st == 2u) {
                    import_dispatch(c, mem, k_import_table[idx].name);
                    return;
                }
                if (k_import_table[idx].plt == plt) {
                    call_import_resolve(c, mem, idx, plt);
                    return;
                }
            }
        }
    }
    fprintf(stderr, "import: unknown PLT 0x%08x\n", plt);
    c->r0 = 0;
}
