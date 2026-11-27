/*
 * Fake JavaVM + JNIEnv bridge in guest memory for Asphalt 6 HD 3.7.0.
 *
 * Guest code invokes (*env)->Method(env, ...) via the JNIEnv vtable at JNI_VTABLE.
 * Each vtable slot points to a synthetic guest address in [JNI_STUB_BASE, JNI_STUB_END).
 * call_import/call_target routes those stub addresses here via java_bridge_dispatch().
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "mem.h"

#ifdef _WIN32
static CRITICAL_SECTION g_jni_cs;
static INIT_ONCE g_jni_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_jni_once(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_jni_cs);
    return TRUE;
}
static inline void jni_lock(void) {
    InitOnceExecuteOnce(&g_jni_once, init_jni_once, NULL, NULL);
    EnterCriticalSection(&g_jni_cs);
}
static inline void jni_unlock(void) { LeaveCriticalSection(&g_jni_cs); }
#else
static inline void jni_lock(void) {}
static inline void jni_unlock(void) {}
#endif

#define JNI_ENV_PTR      0x1210000u
#define JNI_VTABLE       0x1210100u
#define JNI_VM_PTR       0x1211000u
#define JNI_VM_VTABLE    0x1211100u
#define JNI_STUB_BASE    0x1212000u
#define JNI_STUB_SLOTS   256u
#define JNI_VM_STUB_BASE 0x1213000u
#define JNI_VM_SLOTS     16u
#define JNI_SCRATCH_BASE 0x1214000u
#define JNI_SCRATCH_SIZE 0x0002000u

static uint32_t g_scratch_off = 0;

static uint32_t alloc_jni_buf(uint8_t *mem, uint32_t n) {
    jni_lock();
    n = (n + 7u) & ~7u;
    if (g_scratch_off + n > JNI_SCRATCH_SIZE) g_scratch_off = 0;
    uint32_t addr = JNI_SCRATCH_BASE + g_scratch_off;
    g_scratch_off += n;
    memset(mem + addr, 0, n);
    jni_unlock();
    return addr;
}

static uint32_t alloc_jni_str(uint8_t *mem, const char *s) {
    if (!s) return 0;
    size_t len = strlen(s) + 1u;
    uint32_t addr = alloc_jni_buf(mem, (uint32_t)len);
    memcpy(mem + addr, s, len);
    return addr;
}

typedef struct {
    uint32_t id;
    char name[96];
} JniId;

static JniId g_ids[256];
static uint32_t g_id_count = 1u;

static uint32_t intern_id(const char *name) {
    if (!name) name = "";
    jni_lock();
    for (uint32_t i = 1; i < g_id_count; i++) {
        if (strcmp(g_ids[i].name, name) == 0) {
            uint32_t id = g_ids[i].id;
            jni_unlock();
            return id;
        }
    }
    if (g_id_count < 256u) {
        uint32_t idx = g_id_count++;
        g_ids[idx].id = 0x1218000u + idx * 16u;
        snprintf(g_ids[idx].name, sizeof(g_ids[idx].name), "%s", name);
        uint32_t id = g_ids[idx].id;
        jni_unlock();
        return id;
    }
    jni_unlock();
    return 0x1218FF0u;
}

static const char *lookup_id(uint32_t id) {
    jni_lock();
    for (uint32_t i = 1; i < g_id_count; i++) {
        if (g_ids[i].id == id) {
            const char *name = g_ids[i].name;
            jni_unlock();
            return name;
        }
    }
    jni_unlock();
    return "";
}

uint32_t java_bridge_env(void) { return JNI_ENV_PTR; }
uint32_t java_bridge_vm(void)  { return JNI_VM_PTR; }

void java_bridge_init(uint8_t *mem, uint32_t mem_size) {
    if (!mem || mem_size < 0x1220000u) return;
    st4(mem, JNI_ENV_PTR, JNI_VTABLE);
    for (uint32_t i = 0; i < JNI_STUB_SLOTS; i++) {
        st4(mem, JNI_VTABLE + i * 4u, JNI_STUB_BASE + i * 4u);
    }
    st4(mem, JNI_VM_PTR, JNI_VM_VTABLE);
    for (uint32_t i = 0; i < JNI_VM_SLOTS; i++) {
        st4(mem, JNI_VM_VTABLE + i * 4u, JNI_VM_STUB_BASE + i * 4u);
    }
}

static const char *cstr(uint8_t *mem, uint32_t addr) {
    if (!addr || addr >= guest_mem_size) return "";
    return (const char *)(mem + addr);
}

int java_bridge_is_stub(uint32_t target) {
    uint32_t key = target & ~1u;
    return (key >= JNI_STUB_BASE && key < JNI_STUB_BASE + JNI_STUB_SLOTS * 4u) ||
           (key >= JNI_VM_STUB_BASE && key < JNI_VM_STUB_BASE + JNI_VM_SLOTS * 4u);
}

int java_bridge_dispatch(Cpu *c, uint8_t *mem, uint32_t target) {
    uint32_t key = target & ~1u;
    if (key >= JNI_VM_STUB_BASE && key < JNI_VM_STUB_BASE + JNI_VM_SLOTS * 4u) {
        uint32_t slot = (key - JNI_VM_STUB_BASE) / 4u;
        /* JavaVM slots: 3=DestroyJavaVM, 4=AttachCurrentThread, 5=DetachCurrentThread, 6=GetEnv */
        if (slot == 4u || slot == 6u) {
            if (c->r1 && guest_ok(c->r1, 4)) st4(mem, c->r1, JNI_ENV_PTR);
            c->r0 = 0; /* JNI_OK */
            return 1;
        }
        c->r0 = 0;
        return 1;
    }
    if (key < JNI_STUB_BASE || key >= JNI_STUB_BASE + JNI_STUB_SLOTS * 4u) return 0;

    uint32_t slot = (key - JNI_STUB_BASE) / 4u;
    switch (slot) {
    case 4:  /* GetVersion */
        c->r0 = 0x00010006u;
        return 1;
    case 6:  /* FindClass(env, name) */
        c->r0 = intern_id(cstr(mem, c->r1));
        return 1;
    case 15: /* ExceptionCheck */
    case 16: /* ExceptionOccurred */
    case 17: /* ExceptionDescribe */
    case 18: /* ExceptionClear */
        c->r0 = 0;
        return 1;
    case 21: /* NewGlobalRef(env, obj) */
    case 25: /* NewLocalRef(env, obj) */
        c->r0 = c->r1 ? c->r1 : 0x1218FF0u;
        return 1;
    case 22: /* DeleteGlobalRef */
    case 23: /* DeleteLocalRef */
    case 26: /* EnsureLocalCapacity */
        c->r0 = 0;
        return 1;
    case 31: /* GetObjectClass(env, obj) */
        c->r0 = c->r1 ? c->r1 : 0x1218FF0u;
        return 1;
    case 33: /* GetMethodID(env, clazz, name, sig) */
    case 113: /* GetStaticMethodID(env, clazz, name, sig) */
        c->r0 = intern_id(cstr(mem, c->r2));
        return 1;
    case 94: /* GetFieldID */
    case 144: /* GetStaticFieldID */
        c->r0 = intern_id(cstr(mem, c->r2));
        return 1;
    case 114: /* CallStaticObjectMethod(env, clazz, mid, ...) */
    case 115: /* CallStaticObjectMethodV */
    case 116: /* CallStaticObjectMethodA */
    case 34:  /* CallObjectMethod */
    case 35:  /* CallObjectMethodV */
    case 36:  /* CallObjectMethodA */ {
        const char *m = lookup_id(c->r2);
        if (strcmp(m, "getVersion") == 0) { c->r0 = alloc_jni_str(mem, "3.7.0"); return 1; }
        if (strcmp(m, "getHostName") == 0) { c->r0 = alloc_jni_str(mem, "localhost"); return 1; }
        if (strcmp(m, "getWifiIP") == 0) { c->r0 = alloc_jni_str(mem, "127.0.0.1"); return 1; }
        if (strcmp(m, "getKeyboardText") == 0) { c->r0 = alloc_jni_str(mem, ""); return 1; }
        c->r0 = 0;
        return 1;
    }
    case 117: /* CallStaticBooleanMethod */
    case 118: case 119:
    case 37: case 38: case 39: {
        const char *m = lookup_id(c->r2);
        if (strcmp(m, "isKeyboardVisible") == 0 || strcmp(m, "isMusicLoaded") == 0) {
            c->r0 = 0;
            return 1;
        }
        c->r0 = 0;
        return 1;
    }
    case 129: /* CallStaticIntMethod */
    case 130: case 131:
    case 49: case 50: case 51: {
        const char *m = lookup_id(c->r2);
        (void)m;
        c->r0 = 0;
        return 1;
    }
    case 167: /* NewStringUTF(env, bytes) */
        c->r0 = alloc_jni_str(mem, cstr(mem, c->r1));
        return 1;
    case 168: /* GetStringUTFLength(env, str) */
        c->r0 = (uint32_t)strlen(cstr(mem, c->r1));
        return 1;
    case 169: /* GetStringUTFChars(env, str, isCopy) */
        if (c->r2 && guest_ok(c->r2, 1)) mem[c->r2] = 0;
        c->r0 = c->r1 ? c->r1 : alloc_jni_str(mem, "");
        return 1;
    case 170: /* ReleaseStringUTFChars */
        c->r0 = 0;
        return 1;
    case 171: /* GetArrayLength */
        c->r0 = 0;
        return 1;
    case 184: /* GetByteArrayElements */
    case 192: /* ReleaseByteArrayElements */
        c->r0 = 0;
        return 1;
    case 215: /* RegisterNatives */
        c->r0 = 0;
        return 1;
    default:
        c->r0 = 0;
        return 1;
    }
}
