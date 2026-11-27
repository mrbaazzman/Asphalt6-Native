/*
 * Virtual filesystem for Asphalt 6 HD 3.7.0.
 *
 * CRITICAL INVARIANT: gamedata/ and Asphalt 6 HD 3.7.0.apk are strictly read-only.
 * All writes, appends, and save-data mutations are redirected to build/userdata/.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <io.h>
#define mkdir_p(d) _mkdir(d)
#else
#include <unistd.h>
#define mkdir_p(d) mkdir(d, 0755)
#endif

#include "mem.h"
#include "installer.h"

#define VFS_MAX_FILES 256
#define VFS_FILE_BASE 0x100u
#define VFS_FD_BASE   16

static FILE *g_files[VFS_MAX_FILES];
static int   g_fds[VFS_MAX_FILES];
static char  g_cwd[512] = "/";

#ifdef _WIN32
static CRITICAL_SECTION g_vfs_cs;
static INIT_ONCE g_vfs_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_vfs_once(PINIT_ONCE o, PVOID p, PVOID *c) {
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_vfs_cs);
    return TRUE;
}
static inline void vfs_lock(void) {
    InitOnceExecuteOnce(&g_vfs_once, init_vfs_once, NULL, NULL);
    EnterCriticalSection(&g_vfs_cs);
}
static inline void vfs_unlock(void) { LeaveCriticalSection(&g_vfs_cs); }
#else
static inline void vfs_lock(void) {}
static inline void vfs_unlock(void) {}
#endif

static void ensure_parent_dirs(const char *path) {
    char tmp[512];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return;
    memcpy(tmp, path, len + 1u);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            mkdir_p(tmp);
            *p = saved;
        }
    }
}

static void normalize_rel(const char *in, char *out, size_t cap) {
    static const char *const prefixes[] = {
        "/sdcard/games/com.gameloft.android.GAND.GloftAsp6.asphalt6/",
        "/sdcard/games/com.gameloft.android.GAND.GloftAsp6.asphalt6",
        "/mnt/sdcard/games/com.gameloft.android.GAND.GloftAsp6.asphalt6/",
        "/mnt/sdcard/games/com.gameloft.android.GAND.GloftAsp6.asphalt6",
        "/data/data/com.gameloft.android.GAND.GloftAsp6.asphalt6/files/",
        "/data/data/com.gameloft.android.GAND.GloftAsp6.asphalt6/files",
        "/data/data/com.gameloft.android.GAND.GloftAsp6.asphalt6/",
        "/data/data/com.gameloft.android.GAND.GloftAsp6.asphalt6",
        "/sdcard/gameloft/games/asphalt6/",
        "/sdcard/gameloft/games/asphalt6",
        "/sdcard/",
        "./",
    };
    const char *s = in ? in : "";
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t plen = strlen(prefixes[i]);
        if (strncmp(s, prefixes[i], plen) == 0) {
            s += plen;
            break;
        }
    }
    while (*s == '/' || *s == '\\') s++;
    size_t j = 0;
    for (; *s && j + 1 < cap; s++) {
        char c = (*s == '\\') ? '/' : *s;
        /* Flatten duplicate slashes */
        if (c == '/' && j > 0 && out[j - 1] == '/') continue;
        out[j++] = c;
    }
    out[j] = '\0';
}

static int is_write_mode(const char *mode) {
    if (!mode) return 0;
    return strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
}

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static const char *userdata_root(void) {
    static char root[512] = {0};
    if (root[0]) return root;
    if (!a6pak_has_self() && file_exists("build/guest.bin")) {
        snprintf(root, sizeof(root), "build/userdata");
    } else if (!a6pak_local_userdata_dir(root, sizeof(root))) {
        snprintf(root, sizeof(root), "build/userdata");
    }
    return root;
}

static void resolve_path(const char *guest_path, int write, char *out, size_t cap) {
    char rel[512];
    normalize_rel(guest_path, rel, sizeof(rel));
    const char *ud = userdata_root();
    if (!rel[0]) {
        snprintf(out, cap, "%s/_empty", ud);
        return;
    }
    if (strstr(rel, "..")) {
        snprintf(out, cap, "%s/_blocked", ud);
        return;
    }
    if (write) {
        snprintf(out, cap, "%s/%s", ud, rel);
        ensure_parent_dirs(out);
        return;
    }
    snprintf(out, cap, "%s/%s", ud, rel);
    if (file_exists(out)) return;

    /* Optional external hi-res overrides (hires/<rel> or hires/<basename>) */
    snprintf(out, cap, "hires/%s", rel);
    if (file_exists(out)) return;
    const char *base = strrchr(rel, '/');
    base = base ? base + 1 : rel;
    snprintf(out, cap, "hires/%s", base);
    if (file_exists(out)) return;

    snprintf(out, cap, "gamedata/%s", rel);
    if (file_exists(out)) return;

    /* Fallback: extracted APK res/raw files or cached A6PAK res/raw */
    snprintf(out, cap, "analysis/apk_extract/res/raw/%s", base);
    if (file_exists(out)) return;
    if (a6pak_has_self() && a6pak_cache_raw_self(base, out, cap)) {
        if (file_exists(out)) return;
    }

    /* Default to gamedata/ so fopen returns NULL cleanly for missing files. */
    snprintf(out, cap, "gamedata/%s", rel);
}

static int alloc_file_slot(FILE *f) {
    vfs_lock();
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (!g_files[i]) {
            g_files[i] = f;
            vfs_unlock();
            return (int)VFS_FILE_BASE + i;
        }
    }
    vfs_unlock();
    fclose(f);
    return 0;
}

static FILE *get_file_slot(uint32_t handle) {
    if (handle < VFS_FILE_BASE || handle >= VFS_FILE_BASE + VFS_MAX_FILES) return NULL;
    vfs_lock();
    FILE *f = g_files[handle - VFS_FILE_BASE];
    vfs_unlock();
    return f;
}

static int alloc_fd_slot(int fd) {
    vfs_lock();
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (g_fds[i] <= 0) {
            g_fds[i] = fd;
            vfs_unlock();
            return VFS_FD_BASE + i;
        }
    }
    vfs_unlock();
    _close(fd);
    return -1;
}

static int get_host_fd(int gfd) {
    if (gfd < VFS_FD_BASE || gfd >= VFS_FD_BASE + VFS_MAX_FILES) return -1;
    vfs_lock();
    int fd = g_fds[gfd - VFS_FD_BASE];
    vfs_unlock();
    return fd > 0 ? fd : -1;
}

uint32_t vfs_open(const char *path, const char *mode) {
    if (!path || !mode) return 0;
    int wr = is_write_mode(mode);
    char host[512];
    resolve_path(path, wr, host, sizeof(host));

    /* Force binary mode on Windows unless text explicitly safe */
    char binmode[16];
    snprintf(binmode, sizeof(binmode), "%s", mode);
    if (!strchr(binmode, 'b') && !strchr(binmode, 't') && strlen(binmode) + 1 < sizeof(binmode)) {
        strcat(binmode, "b");
    }
    FILE *f = fopen(host, binmode);
    if (!f && getenv("ASPHALT6_VFS_TRACE")) {
        fprintf(stderr, "vfs_open MISS: '%s' (%s) -> '%s'\n", path, mode, host);
    }
    return f ? (uint32_t)alloc_file_slot(f) : 0u;
}

uint32_t vfs_read(uint32_t h, void *buf, uint32_t sz, uint32_t cnt) {
    FILE *f = get_file_slot(h);
    if (!f || !buf || !sz || !cnt) return 0;
    return (uint32_t)fread(buf, sz, cnt, f);
}

uint32_t vfs_write(uint32_t h, const void *buf, uint32_t sz, uint32_t cnt) {
    FILE *f = get_file_slot(h);
    if (!f || !buf || !sz || !cnt) return 0;
    return (uint32_t)fwrite(buf, sz, cnt, f);
}

int vfs_close(uint32_t h) {
    if (h < VFS_FILE_BASE || h >= VFS_FILE_BASE + VFS_MAX_FILES) return -1;
    vfs_lock();
    FILE *f = g_files[h - VFS_FILE_BASE];
    if (!f) { vfs_unlock(); return -1; }
    g_files[h - VFS_FILE_BASE] = NULL;
    vfs_unlock();
    return fclose(f);
}

int vfs_seek(uint32_t h, int32_t off, int whence) {
    FILE *f = get_file_slot(h);
    return f ? fseek(f, off, whence) : -1;
}

int32_t vfs_tell(uint32_t h) {
    FILE *f = get_file_slot(h);
    return f ? (int32_t)ftell(f) : -1;
}

int vfs_eof(uint32_t h) {
    FILE *f = get_file_slot(h);
    return f ? feof(f) : 1;
}

int vfs_error(uint32_t h) {
    FILE *f = get_file_slot(h);
    return f ? ferror(f) : 1;
}

int vfs_getc(uint32_t h) {
    FILE *f = get_file_slot(h);
    return f ? fgetc(f) : EOF;
}

int vfs_ungetc(int ch, uint32_t h) {
    FILE *f = get_file_slot(h);
    return f ? ungetc(ch, f) : EOF;
}

int vfs_putc(int ch, uint32_t h) {
    FILE *f = get_file_slot(h);
    return f ? fputc(ch, f) : EOF;
}

int vfs_flush(uint32_t h) {
    if (!h) return fflush(NULL);
    FILE *f = get_file_slot(h);
    return f ? fflush(f) : 0;
}

char *vfs_gets(char *dst, int n, uint32_t h) {
    FILE *f = get_file_slot(h);
    return (f && dst && n > 0) ? fgets(dst, n, f) : NULL;
}

int vfs_puts(const char *s, uint32_t h) {
    FILE *f = get_file_slot(h);
    return (f && s) ? fputs(s, f) : EOF;
}

int vfs_fileno(uint32_t h) {
    FILE *f = get_file_slot(h);
    if (!f) return -1;
    int raw = _fileno(f);
    return raw >= 0 ? alloc_fd_slot(_dup(raw)) : -1;
}

uint32_t vfs_fdopen(int gfd, const char *mode) {
    vfs_lock();
    int fd = (gfd >= VFS_FD_BASE && gfd < VFS_FD_BASE + VFS_MAX_FILES) ? g_fds[gfd - VFS_FD_BASE] : -1;
    if (fd <= 0 || !mode) { vfs_unlock(); return 0; }
    FILE *f = _fdopen(fd, mode);
    if (!f) { vfs_unlock(); return 0; }
    g_fds[gfd - VFS_FD_BASE] = 0;
    vfs_unlock();
    return (uint32_t)alloc_file_slot(f);
}

int vfs_open_fd(const char *path, int oflag) {
    if (!path) return -1;
    int wr = (oflag & (_O_WRONLY | _O_RDWR | _O_CREAT | _O_TRUNC | _O_APPEND)) != 0;
    char host[512];
    resolve_path(path, wr, host, sizeof(host));
    int flags = _O_BINARY;
    if (wr) {
        flags |= (oflag & _O_RDWR) ? _O_RDWR : _O_WRONLY;
        if (oflag & 0x40) flags |= _O_CREAT;   /* Linux O_CREAT = 0x40 */
        if (oflag & 0x200) flags |= _O_TRUNC;  /* Linux O_TRUNC = 0x200 */
        if (oflag & 0x400) flags |= _O_APPEND; /* Linux O_APPEND = 0x400 */
    } else {
        flags |= _O_RDONLY;
    }
    int fd = _open(host, flags, 0644);
    if (fd < 0) return -1;
    return alloc_fd_slot(fd);
}

int vfs_read_fd(int gfd, void *buf, uint32_t count) {
    int fd = get_host_fd(gfd);
    return (fd >= 0 && buf) ? _read(fd, buf, count) : -1;
}

int vfs_write_fd(int gfd, const void *buf, uint32_t count) {
    int fd = get_host_fd(gfd);
    return (fd >= 0 && buf) ? _write(fd, buf, count) : -1;
}

int32_t vfs_lseek_fd(int gfd, int32_t off, int whence) {
    int fd = get_host_fd(gfd);
    return fd >= 0 ? (int32_t)_lseek(fd, off, whence) : -1;
}

int vfs_close_fd(int gfd) {
    if (gfd < VFS_FD_BASE || gfd >= VFS_FD_BASE + VFS_MAX_FILES) return -1;
    vfs_lock();
    int fd = g_fds[gfd - VFS_FD_BASE];
    if (fd <= 0) { vfs_unlock(); return -1; }
    g_fds[gfd - VFS_FD_BASE] = 0;
    vfs_unlock();
    return _close(fd);
}

/* Android ARM Bionic struct stat layout: st_size at offset 0x2c (44), st_mode at offset 0x10. */
static void write_android_stat(uint8_t *dst, const struct _stat64 *st) {
    if (!dst || !st) return;
    memset(dst, 0, 96);
    uint32_t mode = (st->st_mode & _S_IFDIR) ? 0040755u : 0100644u;
    uint32_t size = (uint32_t)st->st_size;
    memcpy(dst + 0x10u, &mode, 4);
    memcpy(dst + 0x2cu, &size, 4);
    uint32_t mtime = (uint32_t)st->st_mtime;
    memcpy(dst + 0x48u, &mtime, 4);
}

int vfs_stat(const char *path, void *stat_buf) {
    if (!path || !stat_buf) return -1;
    char host[512];
    resolve_path(path, 0, host, sizeof(host));
    struct _stat64 st;
    if (_stat64(host, &st) != 0) return -1;
    write_android_stat((uint8_t *)stat_buf, &st);
    return 0;
}

int vfs_fstat_fd(int gfd, void *stat_buf) {
    int fd = get_host_fd(gfd);
    if (fd < 0 || !stat_buf) return -1;
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0) return -1;
    write_android_stat((uint8_t *)stat_buf, &st);
    return 0;
}

void vfs_chdir(const char *path) {
    if (path && *path) {
        snprintf(g_cwd, sizeof(g_cwd), "%s", path);
    }
}
