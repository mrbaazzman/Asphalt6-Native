#ifndef ASPHALT6_INSTALLER_H
#define ASPHALT6_INSTALLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INST_PHASE_NO_MANIFEST = 0,
    INST_PHASE_CHECKING,
    INST_PHASE_ASK_DOWNLOAD,
    INST_PHASE_DOWNLOADING,
    INST_PHASE_VERIFYING,
    INST_PHASE_DONE,
    INST_PHASE_ERROR,
    INST_PHASE_CANCELED
} InstallerPhase;

typedef struct {
    InstallerPhase phase;
    uint32_t total_files;
    uint32_t checked_files;
    uint32_t missing_files;
    uint32_t done_files;
    uint64_t pack_total_bytes;
    uint64_t missing_bytes;
    uint64_t bytes_downloaded;
    double speed_mibs;
    double fraction;
    char status[160];
    char detail[160];
    char apk_path[260];
    char out_dir[260];
    char extract_dir[260];
} InstallerSnapshot;

typedef struct InstallerSession InstallerSession;

/* Standalone Win32 / CLI check & install before game boot. Returns 1 on success. */
int installer_ensure_gamedata(void);

/* In-game OpenGL ES first-launch installer UI (uses platform_* window/input/GLES2). */
int installer_ensure_gamedata_gl(void);

/* Shared APK / Disk / A6PAK / PNG helpers. */
int installer_find_default_apk(char *out_path, size_t cap);
int installer_pick_apk_dialog(char *out_path, size_t cap);
int installer_read_apk_asset(const char *apk_path, const char *entry_name,
                             uint8_t **out_buf, size_t *out_len);
int installer_read_asset(const char *apk_path, const char *extract_dir,
                         const char *entry_name, uint8_t **out_buf, size_t *out_len);
int installer_decode_png_mem(const uint8_t *png, size_t png_len,
                             uint8_t **out_rgba, int *out_w, int *out_h);

/* Self-contained A6PAK trailer helpers (used by standalone asphalt6_win.exe & Asphalt6_Setup.exe). */
int a6pak_has_self(void);
int a6pak_read_self(const char *name, uint8_t **out_buf, size_t *out_len);
int a6pak_extract_angle_self(char *out_dir, size_t cap);
int a6pak_cache_raw_self(const char *base_name, char *out_path, size_t cap);
int a6pak_local_userdata_dir(char *out_dir, size_t cap);

/* Offline standalone installer builder: unpacks setup's own A6PAK asphalt6_win.exe + ANGLE DLLs,
 * relocates libasphalt6.so -> guest.bin from apk_path, and writes ONE self-contained
 * <install_dir>\asphalt6_win.exe with all assets appended in its A6PAK tail. */
int installer_install_standalone_from_setup(
    const char *apk_path, const char *install_dir,
    void (*progress_cb)(void *ctx, int pct, const char *status, const char *log_line),
    void *ctx);

InstallerSession *installer_session_open(const char *apk_path, const char *out_dir,
                                         const char *extract_dir);
void installer_session_start_check(InstallerSession *s);
void installer_session_start_download(InstallerSession *s);
void installer_session_snapshot(InstallerSession *s, InstallerSnapshot *out);
void installer_session_cancel(InstallerSession *s);
void installer_session_close(InstallerSession *s);

#ifdef __cplusplus
}
#endif

#endif
