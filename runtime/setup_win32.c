/* Asphalt 6: Adrenaline HD — Offline Standalone Windows GUI Installer
 *
 * Single self-contained GitHub Release installer (~26 MiB) with ZERO build
 * tools, ZERO Python, and ZERO internet required during setup.
 *
 * User chooses their Asphalt 6 HD 3.7.0 APK + Install Folder and clicks
 * Install: Setup validates & relocates libasphalt6.so -> guest.bin in RAM,
 * unpacks the portable x86-64 asphalt6_win.exe + ANGLE runtime from Setup's
 * own A6PAK payload, and embeds everything inside <InstallDir>\asphalt6_win.exe
 * so the installed game folder contains ONLY asphalt6_win.exe and gamedata/.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "installer.h"

enum {
    IDC_EDIT_APK = 101,
    IDC_BTN_APK,
    IDC_EDIT_DIR,
    IDC_BTN_DIR,
    IDC_CHK_SHORTCUT,
    IDC_CHK_LAUNCH,
    IDC_STATUS,
    IDC_PROGRESS,
    IDC_BTN_INSTALL,
    IDC_BTN_CLOSE,
    IDC_BTN_LAUNCH
};

#define WM_SETUP_PROGRESS (WM_APP + 1)
#define WM_SETUP_DONE     (WM_APP + 2)

typedef struct {
    HWND hwnd;
    HFONT f_title;
    HFONT f_sub;
    HFONT f_ui;
    HFONT f_bold;
    HWND ed_apk, ed_dir, btn_apk, btn_dir;
    HWND chk_shortcut, chk_launch;
    HWND lbl_status, pb;
    HWND btn_install, btn_close, btn_launch;
    volatile LONG running;
    int installed_ok;
    int cli_mode;
    int prepare_mode;
    char cli_apk[MAX_PATH];
    char cli_dir[MAX_PATH];
    HANDLE worker;
} SetupApp;

static SetupApp g_app;

static int file_exists_a(const char *path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void on_progress(void *ctx, int pct, const char *status, const char *log_line) {
    (void)ctx;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (g_app.prepare_mode) {
        /* Launcher setup step: "progress <0-1> <text>" lines, errors as plain lines. */
        if (status && status[0]) printf("progress %.2f %s\n", pct / 100.0, status);
        if (log_line && strncmp(log_line, "ERROR", 5) == 0) printf("%s\n", log_line);
        fflush(stdout);
        return;
    }
    if (g_app.cli_mode) {
        printf("[%3d%%] %s%s%s\n", pct, status ? status : "",
               (status && log_line) ? " - " : "", log_line ? log_line : "");
        fflush(stdout);
        return;
    }
    const char *msg = (status && status[0]) ? status : log_line;
    char *heap = msg ? _strdup(msg) : NULL;
    if (g_app.hwnd) {
        PostMessageA(g_app.hwnd, WM_SETUP_PROGRESS, (WPARAM)pct, (LPARAM)heap);
    } else {
        free(heap);
    }
}

static void create_desktop_shortcut(const char *install_dir) {
    char desktop[MAX_PATH] = {0};
    if (!SHGetSpecialFolderPathA(NULL, desktop, CSIDL_DESKTOPDIRECTORY, FALSE)) return;

    char link_path[MAX_PATH + 64], exe_path[MAX_PATH + 64];
    snprintf(link_path, sizeof(link_path), "%.250s\\Asphalt 6 HD.lnk", desktop);
    snprintf(exe_path, sizeof(exe_path), "%.250s\\asphalt6_win.exe", install_dir);

    IShellLinkA *psl = NULL;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   &IID_IShellLinkA, (void **)&psl))) {
        psl->lpVtbl->SetPath(psl, exe_path);
        psl->lpVtbl->SetWorkingDirectory(psl, install_dir);
        psl->lpVtbl->SetDescription(psl, "Asphalt 6: Adrenaline HD");
        psl->lpVtbl->SetIconLocation(psl, exe_path, 0);

        IPersistFile *ppf = NULL;
        if (SUCCEEDED(psl->lpVtbl->QueryInterface(psl, &IID_IPersistFile, (void **)&ppf))) {
            wchar_t wlink[MAX_PATH + 64] = {0};
            if (MultiByteToWideChar(CP_ACP, 0, link_path, -1, wlink, MAX_PATH + 64) > 0) {
                ppf->lpVtbl->Save(ppf, wlink, TRUE);
            }
            ppf->lpVtbl->Release(ppf);
        }
        psl->lpVtbl->Release(psl);
    }
}

static DWORD WINAPI setup_worker_thread(LPVOID param) {
    (void)param;
    char apk[MAX_PATH] = {0}, dir[MAX_PATH] = {0};
    int mk_shortcut = 0;

    if (g_app.cli_mode) {
        snprintf(apk, sizeof(apk), "%s", g_app.cli_apk);
        snprintf(dir, sizeof(dir), "%s", g_app.cli_dir);
    } else {
        GetWindowTextA(g_app.ed_apk, apk, MAX_PATH);
        GetWindowTextA(g_app.ed_dir, dir, MAX_PATH);
        mk_shortcut = (SendMessageA(g_app.chk_shortcut, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }

    if (!file_exists_a(apk)) {
        on_progress(NULL, 0, "Select a valid Asphalt 6 HD 3.7.0 APK file.", NULL);
        if (!g_app.cli_mode) PostMessageA(g_app.hwnd, WM_SETUP_DONE, 0, 0);
        return 1;
    }
    if (!dir[0]) {
        on_progress(NULL, 0, "Specify a valid install folder.", NULL);
        if (!g_app.cli_mode) PostMessageA(g_app.hwnd, WM_SETUP_DONE, 0, 0);
        return 1;
    }

    int ok = installer_install_standalone_from_setup(apk, dir, on_progress, NULL);
    if (ok && mk_shortcut) {
        create_desktop_shortcut(dir);
    }
    g_app.installed_ok = ok;
    if (!g_app.cli_mode) PostMessageA(g_app.hwnd, WM_SETUP_DONE, (WPARAM)(ok ? 1 : 0), 0);
    return ok ? 0 : 1;
}

static void browse_apk(HWND hwnd) {
    char buf[MAX_PATH] = {0};
    GetWindowTextA(g_app.ed_apk, buf, MAX_PATH);
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "Android APK (*.apk)\0*.apk\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = "Select Asphalt 6 HD 3.7.0 APK";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
        SetWindowTextA(g_app.ed_apk, buf);
    }
}

static void browse_dir(HWND hwnd) {
    BROWSEINFOA bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = hwnd;
    bi.lpszTitle = "Select Install Folder for Asphalt 6: Adrenaline HD";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderA(&bi);
    if (pidl) {
        char path[MAX_PATH] = {0};
        if (SHGetPathFromIDListA(pidl, path)) {
            SetWindowTextA(g_app.ed_dir, path);
        }
        CoTaskMemFree(pidl);
    }
}

static void launch_installed_game(void) {
    char dir[MAX_PATH] = {0}, exe[MAX_PATH + 32] = {0};
    GetWindowTextA(g_app.ed_dir, dir, MAX_PATH);
    snprintf(exe, sizeof(exe), "%.259s\\asphalt6_win.exe", dir);
    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    char cmd[MAX_PATH + 48];
    snprintf(cmd, sizeof(cmd), "\"%s\"", exe);
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        DestroyWindow(g_app.hwnd);
    } else {
        MessageBoxA(g_app.hwnd, "Failed to start asphalt6_win.exe.", "Launch Error", MB_OK | MB_ICONERROR);
    }
}

static void set_inputs_enabled(BOOL en) {
    EnableWindow(g_app.ed_apk, en);
    EnableWindow(g_app.btn_apk, en);
    EnableWindow(g_app.ed_dir, en);
    EnableWindow(g_app.btn_dir, en);
    EnableWindow(g_app.chk_shortcut, en);
    EnableWindow(g_app.chk_launch, en);
    EnableWindow(g_app.btn_install, en);
    EnableWindow(g_app.btn_close, en);
}

static LRESULT CALLBACK setup_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT hdr = { 0, 0, rc.right, 68 };
        HBRUSH bh = CreateSolidBrush(RGB(16, 36, 68));
        FillRect(hdc, &hdr, bh);
        DeleteObject(bh);

        RECT sep = { 0, 68, rc.right, 70 };
        HBRUSH bs = CreateSolidBrush(RGB(45, 142, 245));
        FillRect(hdc, &sep, bs);
        DeleteObject(bs);

        SetBkMode(hdc, TRANSPARENT);
        SelectObject(hdc, g_app.f_title);
        SetTextColor(hdc, RGB(245, 248, 255));
        RECT rt = { 18, 10, rc.right - 18, 38 };
        DrawTextA(hdc, "Asphalt 6: Adrenaline HD — Game Installer", -1, &rt, DT_LEFT | DT_SINGLELINE);

        SelectObject(hdc, g_app.f_sub);
        SetTextColor(hdc, RGB(178, 206, 238));
        RECT rs = { 18, 40, rc.right - 18, 62 };
        DrawTextA(hdc, "Select your Asphalt 6 HD 3.7.0 APK and install folder. No internet or extra tools needed.", -1, &rs, DT_LEFT | DT_SINGLELINE);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_BTN_APK) browse_apk(hwnd);
        else if (id == IDC_BTN_DIR) browse_dir(hwnd);
        else if (id == IDC_BTN_INSTALL && !InterlockedCompareExchange(&g_app.running, 0, 0)) {
            InterlockedExchange(&g_app.running, 1);
            set_inputs_enabled(FALSE);
            g_app.worker = CreateThread(NULL, 0, setup_worker_thread, NULL, 0, NULL);
        } else if (id == IDC_BTN_LAUNCH && g_app.installed_ok) {
            launch_installed_game();
        } else if (id == IDC_BTN_CLOSE && !InterlockedCompareExchange(&g_app.running, 0, 0)) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_SETUP_PROGRESS: {
        SendMessageA(g_app.pb, PBM_SETPOS, wp, 0);
        char *st = (char *)lp;
        if (st) {
            SetWindowTextA(g_app.lbl_status, st);
            free(st);
        }
        return 0;
    }
    case WM_SETUP_DONE: {
        if (g_app.worker) {
            WaitForSingleObject(g_app.worker, INFINITE);
            CloseHandle(g_app.worker);
            g_app.worker = NULL;
        }
        InterlockedExchange(&g_app.running, 0);
        set_inputs_enabled(TRUE);
        if (wp == 1) {
            SetWindowTextA(g_app.btn_close, "Done");
            ShowWindow(g_app.btn_launch, SW_SHOW);
            SetWindowTextA(g_app.lbl_status,
                           "Installed! Start asphalt6_win.exe to play (will prompt to download gamedata if needed).");
            if (SendMessageA(g_app.chk_launch, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                launch_installed_game();
            }
        }
        return 0;
    }
    case WM_CLOSE:
        if (InterlockedCompareExchange(&g_app.running, 0, 0)) return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

int main(int argc, char **argv) {
    memset(&g_app, 0, sizeof(g_app));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cli") == 0) g_app.cli_mode = 1;
        else if (strcmp(argv[i], "--apk") == 0 && i + 1 < argc) snprintf(g_app.cli_apk, MAX_PATH, "%s", argv[++i]);
        else if (strcmp(argv[i], "--install-dir") == 0 && i + 1 < argc) snprintf(g_app.cli_dir, MAX_PATH, "%s", argv[++i]);
        else if (strcmp(argv[i], "--prepare") == 0) g_app.cli_mode = g_app.prepare_mode = 1;
        else if (strcmp(argv[i], "--game") == 0 && i + 1 < argc) snprintf(g_app.cli_apk, MAX_PATH, "%s", argv[++i]);
        else if (strncmp(argv[i], "--game=", 7) == 0) snprintf(g_app.cli_apk, MAX_PATH, "%s", argv[i] + 7);
        else if (strncmp(argv[i], "--install-dir=", 14) == 0) snprintf(g_app.cli_dir, MAX_PATH, "%s", argv[i] + 14);
    }

    if (g_app.prepare_mode) {
        /* Windowless setup step for launchers: --prepare [--game <apk>] --install-dir <dir>.
         * Exits 0 right away when <dir> already holds this setup's build. Without --game, a
         * new engine is rebuilt from the APK data kept in <dir>\asphalt6_win.exe. */
        if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN &&
            AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        if (!g_app.cli_dir[0]) {
            printf("No install folder given: pass --install-dir <folder>.\n");
            return 2;
        }
        if (!installer_prepare_lock(g_app.cli_dir)) {
            printf("Another Asphalt 6 setup is running for %s. Wait for it to finish, then try again.\n",
                   g_app.cli_dir);
            return 3;
        }
        printf("progress 0.00 Checking the installed game\n");
        fflush(stdout);
        if (installer_standalone_is_current(g_app.cli_dir)) {
            printf("progress 1.00 Already set up\n");
            return 0;
        }
        if (!g_app.cli_apk[0]) {
            /* An update: reuse what the last build took from the APK, kept in the installed exe. */
            char why[160];
            int rc = installer_rebuild_standalone_from_install(g_app.cli_dir, on_progress, NULL,
                                                               why, sizeof(why));
            if (rc > 0) return 0;
            if (rc < 0) {
                printf("Couldn't write the updated game to %s. Make sure there's free space, then set it up again.\n",
                       g_app.cli_dir);
                return 1;
            }
            printf("The game needs to be set up again from your Asphalt 6 HD 3.7.0 APK: %s. Pick it on the game's page.\n",
                   why);
            return 4;
        }
        DWORD rc = setup_worker_thread(NULL);
        if (rc == 0 && g_app.installed_ok) return 0;
        printf("Setup failed. Pick your own Asphalt 6 HD 3.7.0 APK (with lib/armeabi-v7a/libasphalt6.so) and try again.\n");
        return 1;
    }

    if (g_app.cli_mode) {
        if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN &&
            AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        DWORD rc = setup_worker_thread(NULL);
        return (rc == 0 && g_app.installed_ok) ? 0 : 1;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = setup_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "Asphalt6SetupWizard";
    RegisterClassA(&wc);

    g_app.f_title = CreateFontA(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    g_app.f_sub   = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    g_app.f_ui    = CreateFontA(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    g_app.f_bold  = CreateFontA(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

    int W = 580, H = 376;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
    g_app.hwnd = CreateWindowExA(
        WS_EX_APPWINDOW, wc.lpszClassName, "Asphalt 6: Adrenaline HD — Setup",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        sx, sy, W, H, NULL, NULL, wc.hInstance, NULL);

    HINSTANCE inst = wc.hInstance;
    HWND hw = g_app.hwnd;
    #define MK(cls, txt, st, x, y, w, h, id, fnt) do { \
        HWND c = CreateWindowExA(0, cls, txt, WS_CHILD | WS_VISIBLE | (st), (x), (y), (w), (h), hw, (HMENU)(INT_PTR)(id), inst, NULL); \
        SendMessageA(c, WM_SETFONT, (WPARAM)(fnt), TRUE); \
    } while (0)
    #define MKH(out, cls, txt, st, ex, x, y, w, h, id, fnt) do { \
        out = CreateWindowExA(ex, cls, txt, WS_CHILD | WS_VISIBLE | (st), (x), (y), (w), (h), hw, (HMENU)(INT_PTR)(id), inst, NULL); \
        SendMessageA(out, WM_SETFONT, (WPARAM)(fnt), TRUE); \
    } while (0)

    MK("STATIC", "1. Asphalt 6 HD 3.7.0 APK File:", 0, 18, 84, 340, 20, 0, g_app.f_bold);
    MKH(g_app.ed_apk, "EDIT", "", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 18, 106, 440, 26, IDC_EDIT_APK, g_app.f_ui);
    MKH(g_app.btn_apk, "BUTTON", "Browse...", WS_TABSTOP, 0, 466, 106, 88, 26, IDC_BTN_APK, g_app.f_ui);

    MK("STATIC", "2. Install Location:", 0, 18, 144, 340, 20, 0, g_app.f_bold);
    MKH(g_app.ed_dir, "EDIT", "C:\\Games\\Asphalt 6 HD", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, 18, 166, 440, 26, IDC_EDIT_DIR, g_app.f_ui);
    MKH(g_app.btn_dir, "BUTTON", "Browse...", WS_TABSTOP, 0, 466, 166, 88, 26, IDC_BTN_DIR, g_app.f_ui);

    MKH(g_app.chk_shortcut, "BUTTON", "Create Desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 18, 204, 240, 22, IDC_CHK_SHORTCUT, g_app.f_ui);
    MKH(g_app.chk_launch, "BUTTON", "Launch game after install", BS_AUTOCHECKBOX | WS_TABSTOP, 0, 270, 204, 260, 22, IDC_CHK_LAUNCH, g_app.f_ui);

    MKH(g_app.lbl_status, "STATIC",
        "Ready. Installs only asphalt6_win.exe cleanly (zero build tools required).",
        0, 0, 18, 238, 536, 20, IDC_STATUS, g_app.f_bold);
    MKH(g_app.pb, PROGRESS_CLASSA, "", PBS_SMOOTH, 0, 18, 262, 536, 20, IDC_PROGRESS, g_app.f_ui);

    MKH(g_app.btn_launch, "BUTTON", "Launch Game", WS_TABSTOP, 0, 210, 298, 114, 32, IDC_BTN_LAUNCH, g_app.f_bold);
    ShowWindow(g_app.btn_launch, SW_HIDE);
    MKH(g_app.btn_install, "BUTTON", "Install", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 332, 298, 126, 32, IDC_BTN_INSTALL, g_app.f_bold);
    MKH(g_app.btn_close, "BUTTON", "Close", WS_TABSTOP, 0, 466, 298, 88, 32, IDC_BTN_CLOSE, g_app.f_ui);

    char def_apk[MAX_PATH] = {0};
    if (installer_find_default_apk(def_apk, sizeof(def_apk))) {
        char abs_apk[MAX_PATH] = {0};
        GetFullPathNameA(def_apk, MAX_PATH, abs_apk, NULL);
        SetWindowTextA(g_app.ed_apk, abs_apk);
    }
    if (!a6pak_has_self()) {
        SetWindowTextA(g_app.lbl_status,
                       "NOTE: Run pack_setup.py to embed release binaries inside Asphalt6_Setup.exe.");
    }

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageA(g_app.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    DeleteObject(g_app.f_title);
    DeleteObject(g_app.f_sub);
    DeleteObject(g_app.f_ui);
    DeleteObject(g_app.f_bold);
    CoUninitialize();
    return 0;
}
