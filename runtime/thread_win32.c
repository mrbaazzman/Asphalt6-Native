/* Minimal pthread subset backed by Win32 threads, CRITICAL_SECTION, CONDITION_VARIABLE, and TLS. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu.h"
#include "mem.h"
#include "thread_win32.h"

extern void call_target(Cpu *c, uint8_t *mem, uint32_t target);

#define MAX_GUEST_THREADS 32
#define MAX_GUEST_MUTEXES 4096 /* power-of-2 open-addressed hash table */
#define MUTEX_HASH_MASK   (MAX_GUEST_MUTEXES - 1u)
#define MAX_GUEST_CONDS   512
#define MAX_TLS_KEYS      64

/* Guest thread stacks sit between 0x01840000 and 0x01FC0000 (below GUEST_HEAP_BASE = 0x02000000). */
#define THREAD_STACK_BASE 0x01840000u
#define THREAD_STACK_STEP 0x00040000u /* 256 KiB per worker stack */

typedef struct {
    HANDLE handle;
    uint32_t guest_id;
    uint32_t start_fn;
    uint32_t arg;
    uint32_t stack_top;
    uint32_t retval;
    uint8_t *mem;
    int used;
} GuestThread;

typedef struct {
    uint32_t guest_addr;
    CRITICAL_SECTION cs;
    int in_use; /* 0 = empty, 1 = active, 2 = tombstone */
} GuestMutex;

typedef struct {
    uint32_t guest_addr;
    CONDITION_VARIABLE cv;
    int in_use;
} GuestCond;

static GuestThread g_threads[MAX_GUEST_THREADS];
static GuestMutex g_mutexes[MAX_GUEST_MUTEXES];
static GuestCond g_conds[MAX_GUEST_CONDS];
static DWORD g_tls_slots[MAX_TLS_KEYS];
static uint32_t g_tls_count = 0;

static CRITICAL_SECTION g_table_lock;
static INIT_ONCE g_table_once = INIT_ONCE_STATIC_INIT;
static uint32_t g_next_tid = 100u;

static BOOL CALLBACK init_table_once(PINIT_ONCE once, PVOID p, PVOID *ctx) {
    (void)once; (void)p; (void)ctx;
    InitializeCriticalSection(&g_table_lock);
    return TRUE;
}

static void ensure_table_lock(void) {
    InitOnceExecuteOnce(&g_table_once, init_table_once, NULL, NULL);
}

static GuestThread *find_thread_by_id(uint32_t id) {
    for (int i = 0; i < MAX_GUEST_THREADS; i++) {
        if (g_threads[i].used && g_threads[i].guest_id == id) return &g_threads[i];
    }
    return NULL;
}

static DWORD WINAPI guest_thread_trampoline(LPVOID param) {
    GuestThread *gt = (GuestThread *)param;
    Cpu *c = (Cpu *)calloc(1, sizeof(Cpu));
    if (!c) return 1;
    c->r0 = gt->arg;
    c->sp = gt->stack_top;
    c->lr = 0xDEADBEE0u;
    c->TB = gt->start_fn & 1u;
    call_target(c, gt->mem, gt->start_fn);
    gt->retval = c->r0;
    free(c);
    return 0;
}

int thread_win32_create(uint8_t *mem, uint32_t mem_size, uint32_t thread_ptr,
                        uint32_t start_routine, uint32_t arg) {
    (void)mem_size;
    ensure_table_lock();
    EnterCriticalSection(&g_table_lock);
    int slot = -1;
    for (int i = 0; i < MAX_GUEST_THREADS; i++) {
        if (!g_threads[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        LeaveCriticalSection(&g_table_lock);
        return 12; /* ENOMEM */
    }
    GuestThread *gt = &g_threads[slot];
    memset(gt, 0, sizeof(*gt));
    gt->used = 1;
    gt->guest_id = g_next_tid++;
    gt->start_fn = start_routine;
    gt->arg = arg;
    gt->mem = mem;
    gt->stack_top = THREAD_STACK_BASE + (uint32_t)(slot + 1) * THREAD_STACK_STEP;
    gt->handle = CreateThread(NULL, 0, guest_thread_trampoline, gt, 0, NULL);
    if (!gt->handle) {
        gt->used = 0;
        LeaveCriticalSection(&g_table_lock);
        return 11; /* EAGAIN */
    }
    if (thread_ptr && guest_ok(thread_ptr, 4)) {
        st4(mem, thread_ptr, gt->guest_id);
    }
    LeaveCriticalSection(&g_table_lock);
    return 0;
}

int thread_win32_join(uint8_t *mem, uint32_t thread_id, uint32_t retval_ptr) {
    ensure_table_lock();
    EnterCriticalSection(&g_table_lock);
    GuestThread *gt = find_thread_by_id(thread_id);
    HANDLE h = gt ? gt->handle : NULL;
    LeaveCriticalSection(&g_table_lock);
    if (!h) return 3; /* ESRCH */
    WaitForSingleObject(h, INFINITE);
    EnterCriticalSection(&g_table_lock);
    if (retval_ptr && guest_ok(retval_ptr, 4)) {
        st4(mem, retval_ptr, gt->retval);
    }
    CloseHandle(gt->handle);
    gt->used = 0;
    LeaveCriticalSection(&g_table_lock);
    return 0;
}

uint32_t thread_win32_self(void) {
    return (uint32_t)GetCurrentThreadId();
}

static inline uint32_t mutex_hash(uint32_t addr) {
    return ((addr * 2654435761u) >> 20) & MUTEX_HASH_MASK;
}

static GuestMutex *find_or_alloc_mutex(uint8_t *mem, uint32_t mem_size, uint32_t addr, int create) {
    (void)mem;
    if (!addr || addr >= mem_size) return NULL;
    ensure_table_lock();
    EnterCriticalSection(&g_table_lock);
    uint32_t h = mutex_hash(addr);
    int first_free = -1;
    for (uint32_t probe = 0; probe < MAX_GUEST_MUTEXES; probe++) {
        uint32_t i = (h + probe) & MUTEX_HASH_MASK;
        int st = g_mutexes[i].in_use;
        if (st == 1) {
            if (g_mutexes[i].guest_addr == addr) {
                LeaveCriticalSection(&g_table_lock);
                return &g_mutexes[i];
            }
        } else {
            if (first_free < 0) first_free = (int)i;
            if (st == 0) break;
        }
    }
    if (!create || first_free < 0) {
        LeaveCriticalSection(&g_table_lock);
        return NULL;
    }
    GuestMutex *m = &g_mutexes[first_free];
    m->in_use = 1;
    m->guest_addr = addr;
    InitializeCriticalSectionAndSpinCount(&m->cs, 4000);
    LeaveCriticalSection(&g_table_lock);
    return m;
}

int thread_win32_mutex_init(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr) {
    return find_or_alloc_mutex(mem, mem_size, mutex_ptr, 1) ? 0 : 12;
}

int thread_win32_mutex_lock(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr) {
    GuestMutex *m = find_or_alloc_mutex(mem, mem_size, mutex_ptr, 1);
    if (!m) return 22;
    EnterCriticalSection(&m->cs);
    return 0;
}

int thread_win32_mutex_trylock(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr) {
    GuestMutex *m = find_or_alloc_mutex(mem, mem_size, mutex_ptr, 1);
    if (!m) return 22;
    return TryEnterCriticalSection(&m->cs) ? 0 : 16; /* EBUSY */
}

int thread_win32_mutex_unlock(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr) {
    GuestMutex *m = find_or_alloc_mutex(mem, mem_size, mutex_ptr, 0);
    if (!m) return 0;
    LeaveCriticalSection(&m->cs);
    return 0;
}

int thread_win32_mutex_destroy(uint8_t *mem, uint32_t mem_size, uint32_t mutex_ptr) {
    if (!mutex_ptr || mutex_ptr >= mem_size) return 0;
    (void)mem;
    ensure_table_lock();
    EnterCriticalSection(&g_table_lock);
    uint32_t h = mutex_hash(mutex_ptr);
    for (uint32_t probe = 0; probe < MAX_GUEST_MUTEXES; probe++) {
        uint32_t i = (h + probe) & MUTEX_HASH_MASK;
        int st = g_mutexes[i].in_use;
        if (st == 0) break;
        if (st == 1 && g_mutexes[i].guest_addr == mutex_ptr) {
            DeleteCriticalSection(&g_mutexes[i].cs);
            g_mutexes[i].in_use = 2; /* tombstone preserves probe chains */
            g_mutexes[i].guest_addr = 0;
            break;
        }
    }
    LeaveCriticalSection(&g_table_lock);
    return 0;
}

static GuestCond *find_or_alloc_cond(uint8_t *mem, uint32_t mem_size, uint32_t addr, int create) {
    (void)mem;
    if (!addr || addr >= mem_size) return NULL;
    ensure_table_lock();
    EnterCriticalSection(&g_table_lock);
    for (int i = 0; i < MAX_GUEST_CONDS; i++) {
        if (g_conds[i].in_use && g_conds[i].guest_addr == addr) {
            LeaveCriticalSection(&g_table_lock);
            return &g_conds[i];
        }
    }
    if (!create) {
        LeaveCriticalSection(&g_table_lock);
        return NULL;
    }
    for (int i = 0; i < MAX_GUEST_CONDS; i++) {
        if (!g_conds[i].in_use) {
            g_conds[i].in_use = 1;
            g_conds[i].guest_addr = addr;
            InitializeConditionVariable(&g_conds[i].cv);
            LeaveCriticalSection(&g_table_lock);
            return &g_conds[i];
        }
    }
    LeaveCriticalSection(&g_table_lock);
    return NULL;
}

int thread_win32_cond_init(uint8_t *mem, uint32_t mem_size, uint32_t cond_ptr) {
    return find_or_alloc_cond(mem, mem_size, cond_ptr, 1) ? 0 : 12;
}

int thread_win32_cond_signal(uint8_t *mem, uint32_t mem_size, uint32_t cond_ptr) {
    GuestCond *cv = find_or_alloc_cond(mem, mem_size, cond_ptr, 0);
    if (cv) WakeConditionVariable(&cv->cv);
    return 0;
}

int thread_win32_cond_broadcast(uint8_t *mem, uint32_t mem_size, uint32_t cond_ptr) {
    GuestCond *cv = find_or_alloc_cond(mem, mem_size, cond_ptr, 0);
    if (cv) WakeAllConditionVariable(&cv->cv);
    return 0;
}

int thread_win32_cond_wait(uint8_t *mem, uint32_t mem_size, uint32_t cond_ptr, uint32_t mutex_ptr) {
    GuestCond *cv = find_or_alloc_cond(mem, mem_size, cond_ptr, 1);
    GuestMutex *m = find_or_alloc_mutex(mem, mem_size, mutex_ptr, 1);
    if (!cv || !m) return 22;
    SleepConditionVariableCS(&cv->cv, &m->cs, INFINITE);
    return 0;
}

int thread_win32_cond_timedwait(uint8_t *mem, uint32_t mem_size, uint32_t cond_ptr,
                                uint32_t mutex_ptr, uint32_t abstime_ptr) {
    GuestCond *cv = find_or_alloc_cond(mem, mem_size, cond_ptr, 1);
    GuestMutex *m = find_or_alloc_mutex(mem, mem_size, mutex_ptr, 1);
    if (!cv || !m) return 22;
    DWORD ms = 16;
    if (abstime_ptr && abstime_ptr + 8u <= mem_size) {
        int32_t sec = (int32_t)ld4(mem, abstime_ptr);
        int32_t nsec = (int32_t)ld4(mem, abstime_ptr + 4u);
        time_t now_s = time(NULL);
        int64_t diff_ms = ((int64_t)sec - (int64_t)now_s) * 1000 + (int64_t)nsec / 1000000;
        ms = diff_ms <= 0 ? 0u : (diff_ms > 5000 ? 5000u : (DWORD)diff_ms);
    }
    if (!SleepConditionVariableCS(&cv->cv, &m->cs, ms)) {
        return 110; /* ETIMEDOUT */
    }
    return 0;
}

int thread_win32_cond_destroy(uint8_t *mem, uint32_t mem_size, uint32_t cond_ptr) {
    if (!cond_ptr || cond_ptr >= mem_size) return 0;
    (void)mem;
    ensure_table_lock();
    EnterCriticalSection(&g_table_lock);
    for (int i = 0; i < MAX_GUEST_CONDS; i++) {
        if (g_conds[i].in_use && g_conds[i].guest_addr == cond_ptr) {
            g_conds[i].in_use = 0;
            g_conds[i].guest_addr = 0;
            break;
        }
    }
    LeaveCriticalSection(&g_table_lock);
    return 0;
}

int thread_win32_key_create(uint8_t *mem, uint32_t mem_size, uint32_t key_ptr, uint32_t dtor) {
    (void)dtor;
    ensure_table_lock();
    EnterCriticalSection(&g_table_lock);
    if (g_tls_count >= MAX_TLS_KEYS) {
        LeaveCriticalSection(&g_table_lock);
        return 11;
    }
    DWORD slot = TlsAlloc();
    if (slot == TLS_OUT_OF_INDEXES) {
        LeaveCriticalSection(&g_table_lock);
        return 11;
    }
    uint32_t key = g_tls_count++;
    g_tls_slots[key] = slot;
    if (key_ptr && key_ptr + 4u <= mem_size) st4(mem, key_ptr, key);
    LeaveCriticalSection(&g_table_lock);
    return 0;
}

uint32_t thread_win32_getspecific(uint32_t key) {
    if (key >= g_tls_count) return 0;
    return (uint32_t)(uintptr_t)TlsGetValue(g_tls_slots[key]);
}

int thread_win32_setspecific(uint32_t key, uint32_t val) {
    if (key >= g_tls_count) return 22;
    TlsSetValue(g_tls_slots[key], (LPVOID)(uintptr_t)val);
    return 0;
}
#endif /* _WIN32 */
