/* In-game OpenGL ES Game Installer UI (phone-authentic first-launch screen).
 *
 * Renders the APK's Gameloft Installer assets (`gi_background.png`,
 * `gi_top_bar.png`, `gi_logo_tittle_horiz.png`, `gi_alpha_box.png`) inside the
 * live ANGLE OpenGL ES viewport before libasphalt6.so initializes, runs async
 * file checking & Gameloft CDN download/CRC-32 verification, and displays
 * "Touch OK and start playing!" on completion.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "installer.h"
#include "platform.h"
#include "resolution.h"

/* Minimal GLES2 definitions resolved via platform_gl_proc() */
typedef unsigned int   GLuint;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned int   GLenum;
typedef float          GLfloat;
typedef unsigned char  GLboolean;

#define GL_FALSE              0
#define GL_TRUE               1
#define GL_SRC_ALPHA          0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_BLEND              0x0BE2
#define GL_TEXTURE_2D         0x0DE1
#define GL_UNSIGNED_BYTE      0x1401
#define GL_FLOAT              0x1406
#define GL_TRIANGLES          0x0004
#define GL_RGBA               0x1908
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S     0x2802
#define GL_TEXTURE_WRAP_T     0x2803
#define GL_LINEAR             0x2601
#define GL_CLAMP_TO_EDGE      0x812F
#define GL_COLOR_BUFFER_BIT   0x00004000
#define GL_FRAGMENT_SHADER    0x8B30
#define GL_VERTEX_SHADER      0x8B31
#define GL_COMPILE_STATUS     0x8B81
#define GL_TEXTURE0           0x84C0

typedef struct {
    void   (__stdcall *Viewport)(GLint, GLint, GLsizei, GLsizei);
    void   (__stdcall *ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void   (__stdcall *Clear)(GLuint);
    void   (__stdcall *Enable)(GLenum);
    void   (__stdcall *Disable)(GLenum);
    void   (__stdcall *BlendFunc)(GLenum, GLenum);
    void   (__stdcall *GenTextures)(GLsizei, GLuint *);
    void   (__stdcall *DeleteTextures)(GLsizei, const GLuint *);
    void   (__stdcall *BindTexture)(GLenum, GLuint);
    void   (__stdcall *ActiveTexture)(GLenum);
    void   (__stdcall *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void   (__stdcall *TexParameteri)(GLenum, GLenum, GLint);
    GLuint (__stdcall *CreateShader)(GLenum);
    void   (__stdcall *ShaderSource)(GLuint, GLsizei, const char **, const GLint *);
    void   (__stdcall *CompileShader)(GLuint);
    void   (__stdcall *DeleteShader)(GLuint);
    GLuint (__stdcall *CreateProgram)(void);
    void   (__stdcall *AttachShader)(GLuint, GLuint);
    void   (__stdcall *LinkProgram)(GLuint);
    void   (__stdcall *UseProgram)(GLuint);
    void   (__stdcall *DeleteProgram)(GLuint);
    GLint  (__stdcall *GetAttribLocation)(GLuint, const char *);
    GLint  (__stdcall *GetUniformLocation)(GLuint, const char *);
    void   (__stdcall *Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    void   (__stdcall *Uniform1i)(GLint, GLint);
    void   (__stdcall *EnableVertexAttribArray)(GLuint);
    void   (__stdcall *VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    void   (__stdcall *DrawArrays)(GLenum, GLint, GLsizei);
    void   (__stdcall *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
} GlUiProcs;

static int load_glui_procs(GlUiProcs *g) {
    memset(g, 0, sizeof(*g));
#define GL_SYM(field, name) do { \
    void *p = platform_gl_proc(name); \
    if (!p) return 0; \
    memcpy(&g->field, &p, sizeof(p)); \
} while (0)
    GL_SYM(Viewport, "glViewport");
    GL_SYM(ClearColor, "glClearColor");
    GL_SYM(Clear, "glClear");
    GL_SYM(Enable, "glEnable");
    GL_SYM(Disable, "glDisable");
    GL_SYM(BlendFunc, "glBlendFunc");
    GL_SYM(GenTextures, "glGenTextures");
    GL_SYM(DeleteTextures, "glDeleteTextures");
    GL_SYM(BindTexture, "glBindTexture");
    GL_SYM(ActiveTexture, "glActiveTexture");
    GL_SYM(TexImage2D, "glTexImage2D");
    GL_SYM(TexParameteri, "glTexParameteri");
    GL_SYM(CreateShader, "glCreateShader");
    GL_SYM(ShaderSource, "glShaderSource");
    GL_SYM(CompileShader, "glCompileShader");
    GL_SYM(DeleteShader, "glDeleteShader");
    GL_SYM(CreateProgram, "glCreateProgram");
    GL_SYM(AttachShader, "glAttachShader");
    GL_SYM(LinkProgram, "glLinkProgram");
    GL_SYM(UseProgram, "glUseProgram");
    GL_SYM(DeleteProgram, "glDeleteProgram");
    GL_SYM(GetAttribLocation, "glGetAttribLocation");
    GL_SYM(GetUniformLocation, "glGetUniformLocation");
    GL_SYM(Uniform4f, "glUniform4f");
    GL_SYM(Uniform1i, "glUniform1i");
    GL_SYM(EnableVertexAttribArray, "glEnableVertexAttribArray");
    GL_SYM(VertexAttribPointer, "glVertexAttribPointer");
    GL_SYM(DrawArrays, "glDrawArrays");
    GL_SYM(ReadPixels, "glReadPixels");
#undef GL_SYM
    return 1;
}

#define VW 854.0f
#define VH 480.0f

#define CARD_X 133.0f
#define CARD_Y 114.0f
#define CARD_W 587.0f
#define CARD_H 286.0f
#define OV_W 1174
#define OV_H 572

static GLuint make_rgba_tex(const GlUiProcs *g, const uint8_t *rgba, int w, int h) {
    GLuint tex = 0;
    g->GenTextures(1, &tex);
    g->BindTexture(GL_TEXTURE_2D, tex);
    g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    g->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return tex;
}

static GLuint load_ui_png_tex(const GlUiProcs *g, const char *apk_path, const char *entry_name) {
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    if (!installer_read_asset(apk_path, "analysis/apk_extract", entry_name, &raw, &raw_len)) return 0;
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    int ok = installer_decode_png_mem(raw, raw_len, &rgba, &w, &h);
    free(raw);
    if (!ok) return 0;
    GLuint tex = make_rgba_tex(g, rgba, w, h);
    free(rgba);
    return tex;
}

typedef struct {
    const GlUiProcs *g;
    GLuint prog;
    GLint a_pos;
    GLint a_uv;
    GLint u_color;
    GLuint white_tex;
} QuadRenderer;

static int quad_init(QuadRenderer *qr, const GlUiProcs *g) {
    static const char *vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() {\n"
        "  v_uv = a_uv;\n"
        "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
        "}\n";
    static const char *fs_src =
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform vec4 u_color;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(u_tex, v_uv) * u_color;\n"
        "}\n";

    qr->g = g;
    GLuint vs = g->CreateShader(GL_VERTEX_SHADER);
    GLuint fs = g->CreateShader(GL_FRAGMENT_SHADER);
    g->ShaderSource(vs, 1, &vs_src, NULL);
    g->ShaderSource(fs, 1, &fs_src, NULL);
    g->CompileShader(vs);
    g->CompileShader(fs);
    qr->prog = g->CreateProgram();
    g->AttachShader(qr->prog, vs);
    g->AttachShader(qr->prog, fs);
    g->LinkProgram(qr->prog);
    g->DeleteShader(vs);
    g->DeleteShader(fs);

    qr->a_pos = g->GetAttribLocation(qr->prog, "a_pos");
    qr->a_uv = g->GetAttribLocation(qr->prog, "a_uv");
    qr->u_color = g->GetUniformLocation(qr->prog, "u_color");
    GLint u_tex = g->GetUniformLocation(qr->prog, "u_tex");
    g->UseProgram(qr->prog);
    g->Uniform1i(u_tex, 0);

    uint8_t white[4] = { 255, 255, 255, 255 };
    qr->white_tex = make_rgba_tex(g, white, 1, 1);
    return 1;
}

static void quad_draw(const QuadRenderer *qr, GLuint tex, float x, float y, float w, float h,
                      float r, float gr, float b, float a) {
    float x0 = (2.0f * x / VW) - 1.0f;
    float y0 = 1.0f - (2.0f * y / VH);
    float x1 = (2.0f * (x + w) / VW) - 1.0f;
    float y1 = 1.0f - (2.0f * (y + h) / VH);

    float pos[12] = { x0, y0, x1, y0, x1, y1,  x0, y0, x1, y1, x0, y1 };
    static const float uv[12] = { 0, 0, 1, 0, 1, 1,  0, 0, 1, 1, 0, 1 };

    const GlUiProcs *g = qr->g;
    g->UseProgram(qr->prog);
    g->ActiveTexture(GL_TEXTURE0);
    g->BindTexture(GL_TEXTURE_2D, tex ? tex : qr->white_tex);
    g->Uniform4f(qr->u_color, r, gr, b, a);
    g->EnableVertexAttribArray((GLuint)qr->a_pos);
    g->EnableVertexAttribArray((GLuint)qr->a_uv);
    g->VertexAttribPointer((GLuint)qr->a_pos, 2, GL_FLOAT, GL_FALSE, 0, pos);
    g->VertexAttribPointer((GLuint)qr->a_uv, 2, GL_FLOAT, GL_FALSE, 0, uv);
    g->DrawArrays(GL_TRIANGLES, 0, 6);
}

/* ---- Crisp GDI Antialiased Text -> RGBA Card Texture ---- */
typedef struct {
    HDC hdc;
    HBITMAP bmp;
    uint32_t *dib;
    uint8_t *rgba;
    HFONT f_title;
    HFONT f_body;
    HFONT f_small;
    HFONT f_btn;
} TextCanvas;

static void canvas_init(TextCanvas *c) {
    memset(c, 0, sizeof(*c));
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = OV_W;
    bi.bmiHeader.biHeight = -OV_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(NULL);
    c->hdc = CreateCompatibleDC(screen);
    void *bits = NULL;
    c->bmp = CreateDIBSection(c->hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    SelectObject(c->hdc, c->bmp);
    c->dib = (uint32_t *)bits;
    c->rgba = (uint8_t *)calloc((size_t)OV_W * (size_t)OV_H * 4u, 1);

    c->f_title = CreateFontA(-38, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             ANTIALIASED_QUALITY, 0, "Segoe UI");
    c->f_body = CreateFontA(-32, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            ANTIALIASED_QUALITY, 0, "Segoe UI");
    c->f_small = CreateFontA(-25, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                             ANTIALIASED_QUALITY, 0, "Segoe UI");
    c->f_btn = CreateFontA(-32, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                           ANTIALIASED_QUALITY, 0, "Segoe UI");
}

static void canvas_destroy(TextCanvas *c) {
    if (c->f_title) DeleteObject(c->f_title);
    if (c->f_body) DeleteObject(c->f_body);
    if (c->f_small) DeleteObject(c->f_small);
    if (c->f_btn) DeleteObject(c->f_btn);
    if (c->bmp) DeleteObject(c->bmp);
    if (c->hdc) DeleteDC(c->hdc);
    free(c->rgba);
}

static void blend_over(uint8_t *dst, uint8_t sr, uint8_t sg, uint8_t sb, uint8_t sa) {
    if (sa == 0) return;
    uint32_t inv = 255u - sa;
    dst[0] = (uint8_t)((sr * sa + dst[0] * inv) / 255u);
    dst[1] = (uint8_t)((sg * sa + dst[1] * inv) / 255u);
    dst[2] = (uint8_t)((sb * sa + dst[2] * inv) / 255u);
    dst[3] = (uint8_t)(sa + (dst[3] * inv) / 255u);
}

static void canvas_draw_text(TextCanvas *c, HFONT font, RECT r, const char *txt,
                             uint8_t cr, uint8_t cg, uint8_t cb, UINT fmt) {
    if (!txt || !txt[0]) return;
    memset(c->dib, 0, (size_t)OV_W * (size_t)OV_H * 4u);
    SetBkMode(c->hdc, TRANSPARENT);
    SetTextColor(c->hdc, RGB(255, 255, 255));
    SelectObject(c->hdc, font);
    DrawTextA(c->hdc, txt, -1, &r, fmt);

    int x0 = r.left > 0 ? r.left : 0;
    int y0 = r.top > 0 ? r.top : 0;
    int x1 = r.right < OV_W - 3 ? r.right : OV_W - 3;
    int y1 = r.bottom < OV_H - 3 ? r.bottom : OV_H - 3;

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            uint32_t px = c->dib[y * OV_W + x];
            uint8_t lum = (uint8_t)((px >> 8) & 0xFFu);
            if (lum == 0) continue;
            uint8_t sa = (uint8_t)((lum * 195u) / 255u);
            blend_over(&c->rgba[((y + 2) * OV_W + (x + 2)) * 4], 0, 0, 0, sa);
        }
    }
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            uint32_t px = c->dib[y * OV_W + x];
            uint8_t lum = (uint8_t)((px >> 8) & 0xFFu);
            if (lum == 0) continue;
            blend_over(&c->rgba[(y * OV_W + x) * 4], cr, cg, cb, lum);
        }
    }
}

typedef struct {
    float x, y, w, h;
    char label[48];
    int primary;
} UiButton;

static int hit_btn(const UiButton *b, float vx, float vy) {
    return vx >= b->x && vx <= b->x + b->w && vy >= b->y && vy <= b->y + b->h;
}

static void dump_gl_ppm(const GlUiProcs *g, const char *path, int pw, int ph) {
    if (pw <= 0 || ph <= 0) return;
    uint8_t *px = (uint8_t *)malloc((size_t)pw * (size_t)ph * 4u);
    if (!px) return;
    g->ReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", pw, ph);
        uint8_t *row = (uint8_t *)malloc((size_t)pw * 3u);
        if (row) {
            for (int y = ph - 1; y >= 0; y--) {
                const uint8_t *src = px + (size_t)y * (size_t)pw * 4u;
                for (int x = 0; x < pw; x++) {
                    row[x * 3 + 0] = src[x * 4 + 0];
                    row[x * 3 + 1] = src[x * 4 + 1];
                    row[x * 3 + 2] = src[x * 4 + 2];
                }
                fwrite(row, 1, (size_t)pw * 3u, f);
            }
            free(row);
        }
        fclose(f);
    }
    free(px);
}

static void draw_beveled_button(const QuadRenderer *qr, const UiButton *b, int hovered, float pulse) {
    float glow = hovered ? 0.25f + 0.12f * pulse : (b->primary ? 0.14f + 0.10f * pulse : 0.0f);
    if (glow > 0.0f) {
        qr->g->BindTexture(GL_TEXTURE_2D, qr->white_tex);
        quad_draw(qr, 0, b->x - 3.0f, b->y - 3.0f, b->w + 6.0f, b->h + 6.0f,
                  0.25f, 0.72f, 1.0f, glow);
    }
    /* Border */
    quad_draw(qr, 0, b->x, b->y, b->w, b->h,
              b->primary ? 0.35f : 0.45f,
              b->primary ? 0.78f : 0.52f,
              b->primary ? 1.00f : 0.62f, 0.95f);
    /* Top glossy half */
    float br = b->primary ? (hovered ? 0.18f : 0.10f) : (hovered ? 0.28f : 0.20f);
    float bg = b->primary ? (hovered ? 0.56f : 0.45f) : (hovered ? 0.32f : 0.24f);
    float bb = b->primary ? (hovered ? 0.98f : 0.88f) : (hovered ? 0.40f : 0.30f);
    quad_draw(qr, 0, b->x + 2.0f, b->y + 2.0f, b->w - 4.0f, (b->h - 4.0f) * 0.5f,
              br + 0.10f, bg + 0.10f, bb, 0.96f);
    /* Bottom deeper half */
    quad_draw(qr, 0, b->x + 2.0f, b->y + 2.0f + (b->h - 4.0f) * 0.5f, b->w - 4.0f, (b->h - 4.0f) * 0.5f,
              br * 0.75f, bg * 0.75f, bb * 0.82f, 0.96f);
}

int installer_ensure_gamedata_gl(void) {
    const char *skip = getenv("ASPHALT6_NO_INSTALLER");
    const char *force = getenv("ASPHALT6_FORCE_INSTALLER");
    if (skip && skip[0] == '1') return 1;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    int have_file0 = GetFileAttributesExA("gamedata/file000000.dat", GetFileExInfoStandard, &fad);
    int have_sound = GetFileAttributesExA("gamedata/soundinfo.bin", GetFileExInfoStandard, &fad);
    if (!force && have_file0 && have_sound) {
        return 1;
    }

    GlUiProcs g;
    QuadRenderer qr;
    if (!load_glui_procs(&g) || !quad_init(&qr, &g)) {
        return installer_ensure_gamedata();
    }

    char apk_path[MAX_PATH] = {0};
    int have_self_pak = a6pak_has_self();
    int have_apk = have_self_pak ? 0 : installer_find_default_apk(apk_path, sizeof(apk_path));
    int have_manifest = have_self_pak ||
                        (GetFileAttributesExA("analysis/apk_extract/res/raw/data.txt",
                                              GetFileExInfoStandard, &fad) &&
                         GetFileAttributesExA("analysis/apk_extract/res/raw/packatc.info",
                                              GetFileExInfoStandard, &fad));

    GLuint tex_bg = 0, tex_bar = 0, tex_logo = 0, tex_box = 0, tex_ov = 0;
    tex_bg = load_ui_png_tex(&g, have_apk ? apk_path : NULL, "res/drawable-hdpi-v4/gi_background.png");
    tex_bar = load_ui_png_tex(&g, have_apk ? apk_path : NULL, "res/drawable-hdpi-v4/gi_top_bar.png");
    tex_logo = load_ui_png_tex(&g, have_apk ? apk_path : NULL, "res/drawable-hdpi-v4/gi_logo_tittle_horiz.png");
    tex_box = load_ui_png_tex(&g, have_apk ? apk_path : NULL, "res/drawable-hdpi-v4/gi_alpha_box.png");

    TextCanvas canvas;
    canvas_init(&canvas);

    InstallerSession *sess = NULL;
    if (have_manifest || have_apk) {
        sess = installer_session_open(have_apk ? apk_path : "", "gamedata", "analysis/apk_extract");
        installer_session_start_check(sess);
    }

    const char *shot_path = getenv("ASPHALT6_INSTALLER_SHOT");
    const char *auto_ok = getenv("ASPHALT6_AUTO_TOUCH_OK");
    int shot_taken = 0;
    int done_frames = 0;

    InstallerPhase last_phase = (InstallerPhase)-1;
    uint32_t last_done_files = (uint32_t)-1;
    uint64_t last_bytes = (uint64_t)-1;
    int last_focus = -1;

    float hover_vx = -1.0f, hover_vy = -1.0f;
    int focus_idx = 0;
    int prev_btn_count = 0;
    int result = 0;
    DWORD t0 = GetTickCount();

    g.Enable(GL_BLEND);
    g.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (;;) {
        platform_poll();
        if (platform_should_quit()) {
            result = 0;
            break;
        }

        InstallerSnapshot snap;
        if (sess) {
            installer_session_snapshot(sess, &snap);
        } else {
            memset(&snap, 0, sizeof(snap));
            snap.phase = INST_PHASE_NO_MANIFEST;
            snprintf(snap.status, sizeof(snap.status), "Game data or setup manifest missing");
            snprintf(snap.detail, sizeof(snap.detail),
                     "Run Asphalt6_Setup.exe or copy gamedata/ manually.");
        }

        /* Define active interactive buttons for current state */
        UiButton btns[2];
        int btn_count = 0;
        if (snap.phase == INST_PHASE_ASK_DOWNLOAD) {
            btn_count = 2;
            btns[0] = (UiButton){ 224.0f, 326.0f, 184.0f, 46.0f, "Yes", 1 };
            btns[1] = (UiButton){ 446.0f, 326.0f, 184.0f, 46.0f, "Exit", 0 };
        } else if (snap.phase == INST_PHASE_DOWNLOADING || snap.phase == INST_PHASE_VERIFYING) {
            btn_count = 1;
            btns[0] = (UiButton){ 327.0f, 326.0f, 200.0f, 46.0f, "Cancel", 0 };
        } else if (snap.phase == INST_PHASE_DONE) {
            btn_count = 1;
            btns[0] = (UiButton){ 327.0f, 326.0f, 200.0f, 46.0f, "OK", 1 };
        } else if (snap.phase == INST_PHASE_NO_MANIFEST) {
            btn_count = 1;
            btns[0] = (UiButton){ 327.0f, 326.0f, 200.0f, 46.0f, "Exit", 0 };
        } else if (snap.phase == INST_PHASE_ERROR || snap.phase == INST_PHASE_CANCELED) {
            btn_count = 2;
            btns[0] = (UiButton){ 224.0f, 326.0f, 184.0f, 46.0f, "Retry", 1 };
            btns[1] = (UiButton){ 446.0f, 326.0f, 184.0f, 46.0f, "Exit", 0 };
        }

        if (btn_count != prev_btn_count) {
            focus_idx = 0;
            prev_btn_count = btn_count;
        }

        /* Handle pointer + keyboard/gamepad actions */
        int activate = -1;
        int px = 0, py = 0, pdown = 0;
        int dw = resolution_width() > 0 ? resolution_width() : platform_width();
        int dh = resolution_height() > 0 ? resolution_height() : platform_height();
        while (platform_take_pointer(&px, &py, &pdown)) {
            hover_vx = (float)px * VW / (float)(dw > 0 ? dw : 1);
            hover_vy = (float)py * VH / (float)(dh > 0 ? dh : 1);
            for (int i = 0; i < btn_count; i++) {
                if (hit_btn(&btns[i], hover_vx, hover_vy)) {
                    focus_idx = i;
                    if (pdown) activate = i;
                }
            }
        }
        int kc = 0, kdown = 0;
        while (platform_take_game_key(&kc, &kdown)) {
            if (!kdown) continue;
            if ((kc == 0x69 || kc == 0x6a) && btn_count > 1) {
                focus_idx = (focus_idx + 1) % btn_count;
            } else if ((kc == 0x130 || kc == 0x137) && btn_count > 0) {
                activate = focus_idx;
            } else if (kc == 4) {
                if (snap.phase == INST_PHASE_DOWNLOADING || snap.phase == INST_PHASE_VERIFYING) {
                    if (sess) installer_session_cancel(sess);
                } else {
                    result = 0;
                    goto cleanup;
                }
            }
        }
        if (snap.phase == INST_PHASE_ASK_DOWNLOAD && auto_ok && strcmp(auto_ok, "exit") == 0) {
            done_frames++;
            if (done_frames >= 10) {
                if (shot_path && shot_path[0] && !shot_taken) {
                    dump_gl_ppm(&g, shot_path, platform_width(), platform_height());
                    shot_taken = 1;
                }
                result = 0;
                goto cleanup;
            }
        }
        if (snap.phase == INST_PHASE_DONE && auto_ok && auto_ok[0] == '1') {
            done_frames++;
            if (done_frames >= 15) activate = 0;
        }

        /* Process activated button */
        if (activate >= 0 && activate < btn_count) {
            if (snap.phase == INST_PHASE_ASK_DOWNLOAD) {
                if (activate == 0) {
                    installer_session_start_download(sess);
                } else {
                    result = 0;
                    break;
                }
            } else if (snap.phase == INST_PHASE_DOWNLOADING || snap.phase == INST_PHASE_VERIFYING) {
                installer_session_cancel(sess);
            } else if (snap.phase == INST_PHASE_DONE) {
                result = 1;
                break;
            } else if (snap.phase == INST_PHASE_NO_MANIFEST) {
                result = 0;
                break;
            } else if (snap.phase == INST_PHASE_ERROR || snap.phase == INST_PHASE_CANCELED) {
                if (activate == 0) {
                    installer_session_start_check(sess);
                } else {
                    result = 0;
                    break;
                }
            }
        }

        /* Rebuild crisp card text texture only when text/buttons change */
        if (!tex_ov || snap.phase != last_phase || snap.done_files != last_done_files ||
            snap.checked_files != last_done_files ||
            (snap.bytes_downloaded >> 17) != (last_bytes >> 17) || focus_idx != last_focus) {

            memset(canvas.rgba, 0, (size_t)OV_W * (size_t)OV_H * 4u);

            const char *heading = "GAME INSTALLER";
            uint8_t hr = 95, hg = 205, hb = 255;
            char body[512] = {0};
            char sub[256] = {0};

            if (snap.phase == INST_PHASE_CHECKING) {
                heading = "GAME INSTALLER";
                snprintf(body, sizeof(body), "Checking required files...");
                snprintf(sub, sizeof(sub), "Verified %u / %u files",
                         snap.checked_files, snap.total_files);
            } else if (snap.phase == INST_PHASE_ASK_DOWNLOAD) {
                heading = "DOWNLOAD REQUIRED";
                double mb = (double)snap.missing_bytes / (1024.0 * 1024.0);
                if (mb < 0.1) mb = 460.5;
                snprintf(body, sizeof(body),
                         "The game requires additional data (%.1f MB) to run.\n"
                         "Do you want to launch the download now?", mb);
                snprintf(sub, sizeof(sub), "Official Gameloft CDN  -  %u files needed", snap.missing_files);
            } else if (snap.phase == INST_PHASE_DOWNLOADING) {
                heading = "DOWNLOADING...";
                snprintf(body, sizeof(body),
                         "Downloading official game data from Gameloft.\n"
                         "Please do not exit while the download is in progress.");
                snprintf(sub, sizeof(sub), "%s", snap.detail);
            } else if (snap.phase == INST_PHASE_VERIFYING) {
                heading = "INSTALLING...";
                snprintf(body, sizeof(body), "Verifying & unpacking game assets...");
                snprintf(sub, sizeof(sub), "%s", snap.detail);
            } else if (snap.phase == INST_PHASE_DONE) {
                heading = "DOWNLOAD COMPLETE";
                hr = 115; hg = 235; hb = 255;
                snprintf(body, sizeof(body),
                         "The game has been successfully downloaded.\n"
                         "Touch OK and start playing!");
                snprintf(sub, sizeof(sub), "All %u game files verified OK", snap.total_files);
            } else if (snap.phase == INST_PHASE_NO_MANIFEST) {
                heading = "GAME SETUP REQUIRED";
                hr = 255; hg = 200; hb = 90;
                snprintf(body, sizeof(body),
                         "Game data and APK manifest were not found.\n"
                         "Run Asphalt6_Setup.exe or place gamedata/ manually.");
            } else {
                heading = "INSTALLER NOTICE";
                hr = 255; hg = 120; hb = 100;
                snprintf(body, sizeof(body), "%s\n%s", snap.status, snap.detail);
            }

            RECT r_head = { 60, 42, OV_W - 60, 102 };
            RECT r_body = { 64, 128, OV_W - 64, 290 };
            RECT r_sub  = { 64, 304, OV_W - 64, 356 };
            canvas_draw_text(&canvas, canvas.f_title, r_head, heading, hr, hg, hb,
                             DT_CENTER | DT_SINGLELINE);
            canvas_draw_text(&canvas, canvas.f_body, r_body, body, 244, 247, 252,
                             DT_CENTER | DT_WORDBREAK);
            canvas_draw_text(&canvas, canvas.f_small, r_sub, sub, 232, 240, 250,
                             DT_CENTER | DT_SINGLELINE);

            for (int i = 0; i < btn_count; i++) {
                int bx = (int)((btns[i].x - CARD_X) * 2.0f);
                int by = (int)((btns[i].y - CARD_Y) * 2.0f);
                int bw = (int)(btns[i].w * 2.0f);
                int bh = (int)(btns[i].h * 2.0f);
                RECT r_btn = { bx, by + 20, bx + bw, by + bh };
                canvas_draw_text(&canvas, canvas.f_btn, r_btn, btns[i].label,
                                 255, 255, 255, DT_CENTER | DT_SINGLELINE);
            }

            if (!tex_ov) {
                tex_ov = make_rgba_tex(&g, canvas.rgba, OV_W, OV_H);
            } else {
                g.BindTexture(GL_TEXTURE_2D, tex_ov);
                g.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, OV_W, OV_H, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, canvas.rgba);
            }
            last_phase = snap.phase;
            last_done_files = snap.done_files ? snap.done_files : snap.checked_files;
            last_bytes = snap.bytes_downloaded;
            last_focus = focus_idx;
        }

        /* Draw full-screen frame */
        int pw = platform_width();
        int ph = platform_height();
        g.Viewport(0, 0, pw, ph);
        g.ClearColor(0.03f, 0.05f, 0.09f, 1.0f);
        g.Clear(GL_COLOR_BUFFER_BIT);

        float sec = (float)(GetTickCount() - t0) / 1000.0f;
        float pulse = 0.5f + 0.5f * sinf(sec * 3.8f);

        if (tex_bg) {
            quad_draw(&qr, tex_bg, 0.0f, 0.0f, VW, VH, 1.0f, 1.0f, 1.0f, 1.0f);
            quad_draw(&qr, 0, 0.0f, 0.0f, VW, VH, 0.02f, 0.04f, 0.08f, 0.30f);
        }
        if (tex_bar) {
            quad_draw(&qr, tex_bar, 0.0f, 0.0f, VW, 86.0f, 1.0f, 1.0f, 1.0f, 0.96f);
        } else {
            quad_draw(&qr, 0, 0.0f, 0.0f, VW, 74.0f, 0.05f, 0.09f, 0.16f, 0.85f);
        }
        if (tex_logo) {
            quad_draw(&qr, tex_logo, (VW - 280.0f) * 0.5f, 19.0f, 280.0f, 34.0f,
                      1.0f, 1.0f, 1.0f, 1.0f);
        }

        if (tex_box) {
            quad_draw(&qr, tex_box, CARD_X, CARD_Y, CARD_W, CARD_H, 0.36f, 0.42f, 0.52f, 0.92f);
        } else {
            quad_draw(&qr, 0, CARD_X, CARD_Y, CARD_W, CARD_H, 0.05f, 0.08f, 0.13f, 0.90f);
        }

        /* Glossy progress bar track for checking / downloading / verifying / done */
        if (snap.phase == INST_PHASE_CHECKING || snap.phase == INST_PHASE_DOWNLOADING ||
            snap.phase == INST_PHASE_VERIFYING || snap.phase == INST_PHASE_DONE) {
            float px_bar = CARD_X + 52.0f;
            float py_bar = CARD_Y + 186.0f;
            float pw_bar = CARD_W - 104.0f;
            float ph_bar = 22.0f;

            quad_draw(&qr, 0, px_bar - 2.0f, py_bar - 2.0f, pw_bar + 4.0f, ph_bar + 4.0f,
                      0.30f, 0.52f, 0.78f, 0.65f);
            quad_draw(&qr, 0, px_bar, py_bar, pw_bar, ph_bar,
                      0.06f, 0.09f, 0.15f, 0.95f);

            float frac = (float)snap.fraction;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            if (snap.phase == INST_PHASE_DONE) frac = 1.0f;

            float fill_w = (pw_bar - 4.0f) * frac;
            if (fill_w > 1.0f) {
                quad_draw(&qr, 0, px_bar + 2.0f, py_bar + 2.0f, fill_w, (ph_bar - 4.0f) * 0.5f,
                          0.28f, 0.78f, 1.00f, 0.96f);
                quad_draw(&qr, 0, px_bar + 2.0f, py_bar + 2.0f + (ph_bar - 4.0f) * 0.5f,
                          fill_w, (ph_bar - 4.0f) * 0.5f,
                          0.08f, 0.45f, 0.88f, 0.96f);
            }
        }

        /* Interactive buttons */
        for (int i = 0; i < btn_count; i++) {
            draw_beveled_button(&qr, &btns[i], i == focus_idx, pulse);
        }

        /* Crisp card text + button labels overlay */
        if (tex_ov) {
            quad_draw(&qr, tex_ov, CARD_X, CARD_Y, CARD_W, CARD_H, 1.0f, 1.0f, 1.0f, 1.0f);
        }

        if (shot_path && shot_path[0] && !shot_taken && snap.phase == INST_PHASE_DONE) {
            dump_gl_ppm(&g, shot_path, pw, ph);
            shot_taken = 1;
        }

        platform_swap();
    }

cleanup:
    if (sess) installer_session_close(sess);
    canvas_destroy(&canvas);
    if (tex_bg) g.DeleteTextures(1, &tex_bg);
    if (tex_bar) g.DeleteTextures(1, &tex_bar);
    if (tex_logo) g.DeleteTextures(1, &tex_logo);
    if (tex_box) g.DeleteTextures(1, &tex_box);
    if (tex_ov) g.DeleteTextures(1, &tex_ov);
    if (qr.white_tex) g.DeleteTextures(1, &qr.white_tex);
    if (qr.prog) g.DeleteProgram(qr.prog);
    g.ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    g.Clear(GL_COLOR_BUFFER_BIT);
    platform_swap();
    return result;
}
