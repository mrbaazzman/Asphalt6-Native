/*
 * OpenGL ES 2.0 host bridge over ANGLE (libGLESv2.dll) for Asphalt 6 HD.
 *
 * Pre-resolves every ANGLE GLES2 entry point once at init and dispatches each
 * GL import via O(1) cached function pointers (gl_bridge_lookup / call_import).
 * Also supports software decode of ETC1 (0x8D64) and AMD ATC (0x8C92/0x8C93/0x87EE)
 * textures into RGBA8 if ANGLE does not expose those mobile compressed formats.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "mem.h"
#include "platform.h"
#include "resolution.h"

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef unsigned int GLbitfield;

typedef void (*GlHandlerFn)(Cpu *c, uint8_t *mem);

static HMODULE g_gles = NULL;
static int g_loaded = 0;
static int g_gl_trace = 0;
static uint32_t g_gl_draws = 0;
static uint32_t g_gl_texs = 0;
static uint32_t g_gl_comp_texs = 0;
static GLuint g_bound_fbo = 0;
static char *g_shader_src[256];

static void scale_rect_for_host(GLint *x, GLint *y, GLsizei *w, GLsizei *h) {
    if (g_bound_fbo != 0) return;
    int dw = resolution_width(), dh = resolution_height();
    int pw = platform_width(), ph = platform_height();
    if (dw > 0 && dh > 0 && pw > 0 && ph > 0 && (pw != dw || ph != dh)) {
        *x = (GLint)(((int64_t)(*x) * pw) / dw);
        *y = (GLint)(((int64_t)(*y) * ph) / dh);
        *w = (GLsizei)(((int64_t)(*w) * pw + dw - 1) / dw);
        *h = (GLsizei)(((int64_t)(*h) * ph + dh - 1) / dh);
    }
}

void gl_bridge_stats(uint32_t *draws, uint32_t *texs, uint32_t *comp_texs) {
    if (draws) *draws = g_gl_draws;
    if (texs) *texs = g_gl_texs;
    if (comp_texs) *comp_texs = g_gl_comp_texs;
}

typedef void (__stdcall *PFN_RP)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static PFN_RP p_glReadPixels = NULL;

void gl_bridge_dump_ppm(const char *path, int w, int h) {
    PFN_RP rp = p_glReadPixels;
    if (!rp) {
        HMODULE m = g_gles ? g_gles : GetModuleHandleA("libGLESv2.dll");
        if (m) rp = (PFN_RP)GetProcAddress(m, "glReadPixels");
    }
    if (!rp || w <= 0 || h <= 0) return;
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!rgba) return;
    rp(0, 0, w, h, 0x1908u, 0x1401u, rgba);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) {
            const uint8_t *row = rgba + (size_t)y * (size_t)w * 4u;
            for (int x = 0; x < w; x++) fwrite(row + x * 4, 1, 3, f);
        }
        fclose(f);
    }
    free(rgba);
}

static void *resolve_gl_proc(const char *name) {
    void *p = g_gles ? (void *)GetProcAddress(g_gles, name) : NULL;
    if (!p) p = platform_gl_proc(name);
    return p;
}

static inline uint32_t r32(const uint8_t *m, uint32_t a) { return ld4(m, a); }
static inline void w32(uint8_t *m, uint32_t a, uint32_t v) { st4(m, a, v); }
static inline float f32(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
static inline uint32_t sp32(Cpu *c, const uint8_t *m, uint32_t idx) { return r32(m, c->sp + idx * 4u); }

/* --- Minimal ETC1 + AMD ATC -> RGBA8 software fallback decoders --- */
static void decode_etc1_block(const uint8_t *src, uint8_t *dst, int stride) {
    static const int mod[8][4] = {
        {2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29}, {13, 42, -13, -42},
        {18, 60, -18, -60}, {24, 80, -24, -80}, {33, 106, -33, -106}, {47, 183, -47, -183}
    };
    uint32_t hi = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) | src[3];
    uint32_t lo = ((uint32_t)src[4] << 24) | ((uint32_t)src[5] << 16) | ((uint32_t)src[6] << 8) | src[7];
    int diff = (hi >> 1) & 1;
    int flip = hi & 1;
    int t1 = (hi >> 5) & 7;
    int t2 = (hi >> 2) & 7;
    int r[2], g[2], b[2];
    if (diff) {
        int r0 = (int)(hi >> 27) & 31, dr = ((int)(hi >> 24) & 7); if (dr >= 4) dr -= 8;
        int g0 = (int)(hi >> 19) & 31, dg = ((int)(hi >> 16) & 7); if (dg >= 4) dg -= 8;
        int b0 = (int)(hi >> 11) & 31, db = ((int)(hi >>  8) & 7); if (db >= 4) db -= 8;
        r[0] = (r0 << 3) | (r0 >> 2); g[0] = (g0 << 3) | (g0 >> 2); b[0] = (b0 << 3) | (b0 >> 2);
        int r1 = r0 + dr, g1 = g0 + dg, b1 = b0 + db;
        r[1] = (r1 << 3) | (r1 >> 2); g[1] = (g1 << 3) | (g1 >> 2); b[1] = (b1 << 3) | (b1 >> 2);
    } else {
        int r0 = (hi >> 28) & 15, r1 = (hi >> 24) & 15;
        int g0 = (hi >> 20) & 15, g1 = (hi >> 16) & 15;
        int b0 = (hi >> 12) & 15, b1 = (hi >>  8) & 15;
        r[0] = (r0 << 4) | r0; g[0] = (g0 << 4) | g0; b[0] = (b0 << 4) | b0;
        r[1] = (r1 << 4) | r1; g[1] = (g1 << 4) | g1; b[1] = (b1 << 4) | b1;
    }
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int sub = flip ? (y >= 2) : (x >= 2);
            int bit = x * 4 + y;
            int idx = (((lo >> (bit + 16)) & 1) << 1) | ((lo >> bit) & 1);
            static const int idx_map[4] = {2, 3, 1, 0};
            int m = mod[sub ? t2 : t1][idx_map[idx]];
            int cr = r[sub] + m; if (cr < 0) cr = 0; else if (cr > 255) cr = 255;
            int cg = g[sub] + m; if (cg < 0) cg = 0; else if (cg > 255) cg = 255;
            int cb = b[sub] + m; if (cb < 0) cb = 0; else if (cb > 255) cb = 255;
            uint8_t *px = dst + y * stride + x * 4;
            px[0] = (uint8_t)cr; px[1] = (uint8_t)cg; px[2] = (uint8_t)cb; px[3] = 255;
        }
    }
}

static void decode_atc_rgb_block(const uint8_t *src, uint8_t *dst, int stride) {
    uint16_t c0 = (uint16_t)(src[0] | (src[1] << 8));
    uint16_t c1 = (uint16_t)(src[2] | (src[3] << 8));
    uint32_t bits = (uint32_t)src[4] | ((uint32_t)src[5] << 8) | ((uint32_t)src[6] << 16) | ((uint32_t)src[7] << 24);
    int method = (c0 >> 15) & 1;
    int r0 = ((c0 >> 10) & 31) * 255 / 31;
    int g0 = ((c0 >>  5) & 31) * 255 / 31;
    int b0 = (c0 & 31) * 255 / 31;
    int r1 = ((c1 >> 11) & 31) * 255 / 31;
    int g1 = ((c1 >>  5) & 63) * 255 / 63;
    int b1 = (c1 & 31) * 255 / 31;
    uint8_t pal[4][3];
    if (method == 0) {
        pal[0][0] = (uint8_t)r0; pal[0][1] = (uint8_t)g0; pal[0][2] = (uint8_t)b0;
        pal[3][0] = (uint8_t)r1; pal[3][1] = (uint8_t)g1; pal[3][2] = (uint8_t)b1;
        for (int i = 0; i < 3; i++) {
            pal[1][i] = (uint8_t)((2 * pal[0][i] + pal[3][i]) / 3);
            pal[2][i] = (uint8_t)((pal[0][i] + 2 * pal[3][i]) / 3);
        }
    } else {
        pal[0][0] = 0; pal[0][1] = 0; pal[0][2] = 0;
        int dr = r1 - r0 / 4, dg = g1 - g0 / 4, db = b1 - b0 / 4;
        pal[1][0] = (uint8_t)(dr < 0 ? 0 : dr);
        pal[1][1] = (uint8_t)(dg < 0 ? 0 : dg);
        pal[1][2] = (uint8_t)(db < 0 ? 0 : db);
        pal[2][0] = (uint8_t)r0; pal[2][1] = (uint8_t)g0; pal[2][2] = (uint8_t)b0;
        pal[3][0] = (uint8_t)r1; pal[3][1] = (uint8_t)g1; pal[3][2] = (uint8_t)b1;
    }
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int code = (int)((bits >> ((y * 4 + x) * 2)) & 3u);
            uint8_t *px = dst + y * stride + x * 4;
            px[0] = pal[code][0]; px[1] = pal[code][1]; px[2] = pal[code][2]; px[3] = 255;
        }
    }
}

static void decode_atc_rgba_explicit_block(const uint8_t *src, uint8_t *dst, int stride) {
    decode_atc_rgb_block(src + 8, dst, stride);
    for (int y = 0; y < 4; y++) {
        uint16_t row = (uint16_t)(src[y * 2] | (src[y * 2 + 1] << 8));
        for (int x = 0; x < 4; x++) {
            int a4 = (row >> (x * 4)) & 15;
            dst[y * stride + x * 4 + 3] = (uint8_t)((a4 << 4) | a4);
        }
    }
}

static void decode_atc_rgba_interp_block(const uint8_t *src, uint8_t *dst, int stride) {
    decode_atc_rgb_block(src + 8, dst, stride);
    int a0 = src[0], a1 = src[1], apal[8];
    apal[0] = a0; apal[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i <= 6; i++) apal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int i = 1; i <= 4; i++) apal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        apal[6] = 0; apal[7] = 255;
    }
    uint64_t abits = 0;
    for (int i = 0; i < 6; i++) abits |= ((uint64_t)src[2 + i]) << (i * 8);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int code = (int)((abits >> ((y * 4 + x) * 3)) & 7u);
            dst[y * stride + x * 4 + 3] = (uint8_t)apal[code];
        }
    }
}

static uint8_t *decompress_compressed(GLenum fmt, int w, int h, const uint8_t *src, int src_sz) {
    if (w <= 0 || h <= 0 || !src) return NULL;
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    int bsize = (fmt == 0x8D64u || fmt == 0x8C92u) ? 8 : 16;
    if (src_sz < bw * bh * bsize) return NULL;
    uint8_t *rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!rgba) return NULL;
    uint8_t tmp[4 * 4 * 4];
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            const uint8_t *blk = src + (by * bw + bx) * bsize;
            if (fmt == 0x8D64u) decode_etc1_block(blk, tmp, 16);
            else if (fmt == 0x8C92u) decode_atc_rgb_block(blk, tmp, 16);
            else if (fmt == 0x8C93u) decode_atc_rgba_explicit_block(blk, tmp, 16);
            else if (fmt == 0x87EEu) decode_atc_rgba_interp_block(blk, tmp, 16);
            else { free(rgba); return NULL; }
            for (int y = 0; y < 4 && by * 4 + y < h; y++) {
                for (int x = 0; x < 4 && bx * 4 + x < w; x++) {
                    memcpy(rgba + ((by * 4 + y) * w + (bx * 4 + x)) * 4, tmp + y * 16 + x * 4, 4);
                }
            }
        }
    }
    return rgba;
}

typedef void (__stdcall *PFN_void_0)(void);
typedef void (__stdcall *PFN_void_u1)(GLuint);
typedef void (__stdcall *PFN_void_u2)(GLuint, GLuint);
typedef void (__stdcall *PFN_void_u3)(GLuint, GLuint, GLuint);
typedef void (__stdcall *PFN_void_u4)(GLuint, GLuint, GLuint, GLuint);
typedef void (__stdcall *PFN_void_i4)(GLint, GLint, GLint, GLint);
typedef void (__stdcall *PFN_void_f4)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (__stdcall *PFN_void_f2)(GLfloat, GLfloat);
typedef void (__stdcall *PFN_void_f1)(GLfloat);
typedef GLuint (__stdcall *PFN_u_u1)(GLuint);
typedef GLuint (__stdcall *PFN_u_0)(void);
typedef GLint  (__stdcall *PFN_i_u_cp)(GLuint, const char *);
typedef const unsigned char * (__stdcall *PFN_s_u1)(GLuint);
typedef void (__stdcall *PFN_getiv)(GLenum, GLint *);
typedef void (__stdcall *PFN_getfv)(GLenum, GLfloat *);
typedef void (__stdcall *PFN_sampcov)(GLfloat, GLboolean);
typedef void (__stdcall *PFN_cmask)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (__stdcall *PFN_u1f)(GLint, GLfloat);
typedef void (__stdcall *PFN_texpf)(GLenum, GLenum, GLfloat);
typedef void (__stdcall *PFN_drawelem)(GLenum, GLsizei, GLenum, const void *);
typedef void (__stdcall *PFN_fbt2d)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (__stdcall *PFN_gendel)(GLsizei, GLuint *);
typedef void (__stdcall *PFN_sh_iv)(GLuint, GLenum, GLint *);
typedef void (__stdcall *PFN_infolog)(GLuint, GLsizei, GLsizei *, char *);
typedef void (__stdcall *PFN_active)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, char *);
typedef void (__stdcall *PFN_shsrc)(GLuint, GLsizei, const char **, const GLint *);
typedef void (__stdcall *PFN_bufdata)(GLenum, ptrdiff_t, const void *, GLenum);
typedef void (__stdcall *PFN_bufsub)(GLenum, ptrdiff_t, ptrdiff_t, const void *);
typedef void (__stdcall *PFN_va4f)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (__stdcall *PFN_vaptr)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (__stdcall *PFN_unifv)(GLint, GLsizei, const void *);
typedef void (__stdcall *PFN_unim4)(GLint, GLsizei, GLboolean, const GLfloat *);
typedef void (__stdcall *PFN_tex2d)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (__stdcall *PFN_texsub2d)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (__stdcall *PFN_cptexsub2d)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);
typedef void (__stdcall *PFN_ctex2d)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
typedef void (__stdcall *PFN_ctexsub2d)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);

#define GL_NOP(fn) \
    static void glw_##fn(Cpu *c, uint8_t *m) { (void)c; (void)m; }
#define GL_U1(fn) \
    static PFN_void_u1 p_##fn = NULL; \
    static void glw_##fn(Cpu *c, uint8_t *m) { (void)m; if (p_##fn) p_##fn(c->r0); }
#define GL_U2(fn) \
    static PFN_void_u2 p_##fn = NULL; \
    static void glw_##fn(Cpu *c, uint8_t *m) { (void)m; if (p_##fn) p_##fn(c->r0, c->r1); }
#define GL_U3(fn) \
    static PFN_void_u3 p_##fn = NULL; \
    static void glw_##fn(Cpu *c, uint8_t *m) { (void)m; if (p_##fn) p_##fn(c->r0, c->r1, c->r2); }
#define GL_U4(fn) \
    static PFN_void_u4 p_##fn = NULL; \
    static void glw_##fn(Cpu *c, uint8_t *m) { (void)m; if (p_##fn) p_##fn(c->r0, c->r1, c->r2, c->r3); }
#define GL_F1(fn) \
    static PFN_void_f1 p_##fn = NULL; \
    static void glw_##fn(Cpu *c, uint8_t *m) { (void)m; if (p_##fn) p_##fn(f32(c->r0)); }
#define GL_F2(fn) \
    static PFN_void_f2 p_##fn = NULL; \
    static void glw_##fn(Cpu *c, uint8_t *m) { (void)m; if (p_##fn) p_##fn(f32(c->r0), f32(c->r1)); }
#define GL_F4(fn) \
    static PFN_void_f4 p_##fn = NULL; \
    static void glw_##fn(Cpu *c, uint8_t *m) { (void)m; if (p_##fn) p_##fn(f32(c->r0), f32(c->r1), f32(c->r2), f32(c->r3)); }
#define GL_RECT(fn) \
    static PFN_void_i4 p_##fn = NULL; \
    static void glw_##fn(Cpu *c, uint8_t *m) { \
        (void)m; \
        GLint vx = (GLint)c->r0, vy = (GLint)c->r1; \
        GLsizei vw = (GLsizei)c->r2, vh = (GLsizei)c->r3; \
        scale_rect_for_host(&vx, &vy, &vw, &vh); \
        if (p_##fn) p_##fn(vx, vy, vw, vh); \
    }
#define GL_GENDEL(fn) \
    static PFN_gendel p_##fn = NULL; \
    static void glw_##fn(Cpu *c, uint8_t *m) { \
        if (p_##fn && c->r0 > 0 && c->r1 && guest_ok(c->r1, c->r0 * 4u)) \
            p_##fn((GLsizei)c->r0, (GLuint *)(m + c->r1)); \
    }
#define GL_LOC(fn) \
    static PFN_i_u_cp p_##fn = NULL; \
    static void glw_##fn(Cpu *c, uint8_t *m) { \
        c->r0 = (uint32_t)(p_##fn ? p_##fn(c->r0, (const char *)(m + c->r1)) : -1); \
    }
#define GL_UNIFV(fn) \
    static PFN_unifv p_##fn = NULL; \
    static void glw_##fn(Cpu *c, uint8_t *m) { \
        if (p_##fn && c->r2) p_##fn((GLint)c->r0, (GLsizei)c->r1, m + c->r2); \
    }

/* Legacy GLES1 init calls */
GL_NOP(glShadeModel)
GL_NOP(glMatrixMode)
GL_NOP(glLoadIdentity)

GL_U1(glClear)
GL_U1(glEnable)
GL_U1(glDisable)
GL_U1(glActiveTexture)
GL_U1(glCullFace)
GL_U1(glFrontFace)
GL_U1(glDepthFunc)
GL_U1(glDepthMask)
GL_U1(glClearStencil)
GL_U1(glStencilMask)
GL_U1(glUseProgram)
GL_U1(glEnableVertexAttribArray)
GL_U1(glDisableVertexAttribArray)
GL_U1(glCompileShader)
GL_U1(glLinkProgram)
GL_U1(glDeleteShader)
GL_U1(glDeleteProgram)
GL_U1(glGenerateMipmap)
GL_U1(glBlendEquation)

GL_U2(glBindBuffer)
GL_U2(glBindTexture)
GL_U2(glBindRenderbuffer)
GL_U2(glBlendFunc)
GL_U2(glAttachShader)
GL_U2(glPixelStorei)
GL_U2(glHint)
GL_U2(glUniform1i)

GL_U3(glTexParameteri)
GL_U3(glStencilFunc)
GL_U3(glStencilOp)

GL_U4(glRenderbufferStorage)
GL_U4(glFramebufferRenderbuffer)

GL_F1(glClearDepthf)
GL_F1(glLineWidth)
GL_F2(glDepthRangef)
GL_F2(glPolygonOffset)
GL_F4(glClearColor)
GL_F4(glBlendColor)

GL_RECT(glViewport)
GL_RECT(glScissor)

GL_GENDEL(glGenBuffers)
GL_GENDEL(glGenTextures)
GL_GENDEL(glGenFramebuffers)
GL_GENDEL(glGenRenderbuffers)
GL_GENDEL(glDeleteBuffers)
GL_GENDEL(glDeleteTextures)
GL_GENDEL(glDeleteFramebuffers)
GL_GENDEL(glDeleteRenderbuffers)

GL_LOC(glGetAttribLocation)
GL_LOC(glGetUniformLocation)

GL_UNIFV(glUniform1fv)
GL_UNIFV(glUniform2fv)
GL_UNIFV(glUniform3fv)
GL_UNIFV(glUniform4fv)
GL_UNIFV(glUniform1iv)
GL_UNIFV(glUniform2iv)
GL_UNIFV(glUniform3iv)
GL_UNIFV(glUniform4iv)

static PFN_u_0      p_glGetError = NULL;
static PFN_s_u1     p_glGetString = NULL;
static PFN_getiv    p_glGetIntegerv = NULL;
static PFN_getfv    p_glGetFloatv = NULL;
static PFN_void_0   p_glFlush = NULL;
static PFN_sampcov  p_glSampleCoverage = NULL;
static PFN_cmask    p_glColorMask = NULL;
static PFN_void_u2  p_glBindFramebuffer = NULL;
static PFN_u1f      p_glUniform1f = NULL;
static PFN_void_u3  p_glDrawArrays = NULL;
static PFN_texpf    p_glTexParameterf = NULL;
static PFN_drawelem p_glDrawElements = NULL;
static PFN_fbt2d    p_glFramebufferTexture2D = NULL;
static PFN_u_u1     p_glCreateShader = NULL;
static PFN_u_0      p_glCreateProgram = NULL;
static PFN_u_u1     p_glCheckFramebufferStatus = NULL;
static PFN_sh_iv    p_glGetShaderiv = NULL;
static PFN_sh_iv    p_glGetProgramiv = NULL;
static PFN_infolog  p_glGetShaderInfoLog = NULL;
static PFN_infolog  p_glGetProgramInfoLog = NULL;
static PFN_active   p_glGetActiveAttrib = NULL;
static PFN_active   p_glGetActiveUniform = NULL;
static PFN_shsrc    p_glShaderSource = NULL;
static PFN_bufdata  p_glBufferData = NULL;
static PFN_bufsub   p_glBufferSubData = NULL;
static PFN_va4f     p_glVertexAttrib4f = NULL;
static PFN_vaptr    p_glVertexAttribPointer = NULL;
static PFN_unim4    p_glUniformMatrix4fv = NULL;
static PFN_tex2d    p_glTexImage2D = NULL;
static PFN_texsub2d p_glTexSubImage2D = NULL;
static PFN_cptexsub2d p_glCopyTexSubImage2D = NULL;
static PFN_ctex2d   p_glCompressedTexImage2D = NULL;
static PFN_ctexsub2d p_glCompressedTexSubImage2D = NULL;

static void glw_glGetError(Cpu *c, uint8_t *m) {
    (void)m;
    c->r0 = p_glGetError ? p_glGetError() : 0u;
}

static void glw_glGetString(Cpu *c, uint8_t *mem) {
    static const uint32_t slots[5] = { 0x1219000u, 0x1219100u, 0x1219200u, 0x1219800u, 0x1219400u };
    if (c->r0 == 0x1F03u) {
        const char *ext = "GL_OES_texture_npot GL_OES_compressed_ETC1_RGB8_texture "
                          "GL_AMD_compressed_ATC_texture GL_OES_rgb8_rgba8 "
                          "GL_ARM_rgba8 GL_OES_depth24 GL_OES_packed_depth_stencil "
                          "GL_OES_framebuffer_object";
        memcpy(mem + slots[3], ext, strlen(ext) + 1u);
        c->r0 = slots[3];
        return;
    }
    const char *s = p_glGetString ? (const char *)p_glGetString(c->r0) : NULL;
    if (!s) { c->r0 = 0; return; }
    uint32_t a = slots[4];
    if (c->r0 == 0x1F00u) a = slots[1];
    else if (c->r0 == 0x1F01u) a = slots[2];
    else if (c->r0 == 0x1F02u) a = slots[0];
    size_t len = strlen(s);
    if (len > 240u) len = 240u;
    memcpy(mem + a, s, len);
    mem[a + len] = 0;
    c->r0 = a;
}

static void glw_glGetIntegerv(Cpu *c, uint8_t *mem) {
    if (!p_glGetIntegerv || !c->r1 || !guest_ok(c->r1, 16)) return;
    GLint tmp[4] = {0, 0, 0, 0};
    p_glGetIntegerv(c->r0, tmp);
    int cnt = (c->r0 == 0x0BA2 || c->r0 == 0x0C10) ? 4 : (c->r0 == 0x0D3A ? 2 : 1);
    for (int i = 0; i < cnt; i++) w32(mem, c->r1 + (uint32_t)i * 4u, (uint32_t)tmp[i]);
}

static void glw_glGetFloatv(Cpu *c, uint8_t *mem) {
    if (p_glGetFloatv && c->r1 && guest_ok(c->r1, 16)) {
        GLfloat tmp[4] = {0};
        p_glGetFloatv(c->r0, tmp);
        memcpy(mem + c->r1, tmp, 4);
    }
}

static void glw_glFlush(Cpu *c, uint8_t *m) { (void)c; (void)m; if (p_glFlush) p_glFlush(); }
static void glw_glSampleCoverage(Cpu *c, uint8_t *m) { (void)m; if (p_glSampleCoverage) p_glSampleCoverage(f32(c->r0), (GLboolean)c->r1); }
static void glw_glColorMask(Cpu *c, uint8_t *m) { (void)m; if (p_glColorMask) p_glColorMask((GLboolean)c->r0, (GLboolean)c->r1, (GLboolean)c->r2, (GLboolean)c->r3); }
static void glw_glBindFramebuffer(Cpu *c, uint8_t *m) { (void)m; g_bound_fbo = c->r1; if (p_glBindFramebuffer) p_glBindFramebuffer(c->r0, c->r1); }
static void glw_glUniform1f(Cpu *c, uint8_t *m) { (void)m; if (p_glUniform1f) p_glUniform1f((GLint)c->r0, f32(c->r1)); }
static void glw_glDrawArrays(Cpu *c, uint8_t *m) { (void)m; g_gl_draws++; if (p_glDrawArrays) p_glDrawArrays(c->r0, c->r1, c->r2); }
static void glw_glTexParameterf(Cpu *c, uint8_t *m) { (void)m; if (p_glTexParameterf) p_glTexParameterf(c->r0, c->r1, f32(c->r2)); }

static void glw_glDrawElements(Cpu *c, uint8_t *mem) {
    g_gl_draws++;
    const void *idx = (c->r3 >= 0x10000u && c->r3 < guest_mem_size) ? (const void *)(mem + c->r3)
                                                                      : (const void *)(uintptr_t)c->r3;
    if (p_glDrawElements) p_glDrawElements(c->r0, (GLsizei)c->r1, c->r2, idx);
}

static void glw_glFramebufferTexture2D(Cpu *c, uint8_t *mem) {
    if (p_glFramebufferTexture2D) p_glFramebufferTexture2D(c->r0, c->r1, c->r2, c->r3, (GLint)sp32(c, mem, 0));
}

static void glw_glCreateShader(Cpu *c, uint8_t *m) { (void)m; c->r0 = p_glCreateShader ? p_glCreateShader(c->r0) : 0u; }
static void glw_glCreateProgram(Cpu *c, uint8_t *m) { (void)m; c->r0 = p_glCreateProgram ? p_glCreateProgram() : 0u; }
static void glw_glCheckFramebufferStatus(Cpu *c, uint8_t *m) { (void)m; c->r0 = p_glCheckFramebufferStatus ? p_glCheckFramebufferStatus(c->r0) : 0x8CD5u; }

static void glw_glGetShaderiv(Cpu *c, uint8_t *mem) {
    if (p_glGetShaderiv && c->r2 && guest_ok(c->r2, 4)) {
        GLint val = 0;
        p_glGetShaderiv(c->r0, c->r1, &val);
        w32(mem, c->r2, (uint32_t)val);
        if (!val && (c->r1 == 0x8B81u || c->r1 == 0x8B82u)) {
            char log[1024]; GLsizei len = 0; log[0] = '\0';
            if (p_glGetShaderInfoLog) p_glGetShaderInfoLog(c->r0, sizeof(log), &len, log);
            fprintf(stderr, "gl: glGetShaderiv(%u) = 0! log: %s\n", c->r0, log);
            if (c->r0 < 256u && g_shader_src[c->r0]) {
                fprintf(stderr, "--- shader %u source ---\n%s\n--- end shader ---\n",
                        c->r0, g_shader_src[c->r0]);
            }
        }
    }
}

static void glw_glGetProgramiv(Cpu *c, uint8_t *mem) {
    if (p_glGetProgramiv && c->r2 && guest_ok(c->r2, 4)) {
        GLint val = 0;
        p_glGetProgramiv(c->r0, c->r1, &val);
        w32(mem, c->r2, (uint32_t)val);
        if (!val && (c->r1 == 0x8B81u || c->r1 == 0x8B82u)) {
            char log[1024]; GLsizei len = 0; log[0] = '\0';
            if (p_glGetProgramInfoLog) p_glGetProgramInfoLog(c->r0, sizeof(log), &len, log);
            fprintf(stderr, "gl: glGetProgramiv(%u) = 0! log: %s\n", c->r0, log);
        }
    }
}

static void glw_glGetShaderInfoLog(Cpu *c, uint8_t *mem) {
    if (p_glGetShaderInfoLog && c->r3 && guest_ok(c->r3, c->r1)) {
        p_glGetShaderInfoLog(c->r0, (GLsizei)c->r1, c->r2 ? (GLsizei *)(mem + c->r2) : NULL, (char *)(mem + c->r3));
    }
}

static void glw_glGetProgramInfoLog(Cpu *c, uint8_t *mem) {
    if (p_glGetProgramInfoLog && c->r3 && guest_ok(c->r3, c->r1)) {
        p_glGetProgramInfoLog(c->r0, (GLsizei)c->r1, c->r2 ? (GLsizei *)(mem + c->r2) : NULL, (char *)(mem + c->r3));
    }
}

static void glw_active_helper(PFN_active p, Cpu *c, uint8_t *mem) {
    if (!p) return;
    uint32_t lenp = c->r3, szp = sp32(c, mem, 0), typ = sp32(c, mem, 1), namep = sp32(c, mem, 2);
    p(c->r0, c->r1, (GLsizei)c->r2,
      lenp ? (GLsizei *)(mem + lenp) : NULL,
      szp ? (GLint *)(mem + szp) : NULL,
      typ ? (GLenum *)(mem + typ) : NULL,
      namep ? (char *)(mem + namep) : NULL);
}

static void glw_glGetActiveAttrib(Cpu *c, uint8_t *mem) { glw_active_helper(p_glGetActiveAttrib, c, mem); }
static void glw_glGetActiveUniform(Cpu *c, uint8_t *mem) { glw_active_helper(p_glGetActiveUniform, c, mem); }

static void glw_glShaderSource(Cpu *c, uint8_t *mem) {
    if (!p_glShaderSource || c->r1 == 0 || c->r1 > 64u || !c->r2 || !guest_ok(c->r2, c->r1 * 4u)) return;
    const char *ptrs[64];
    for (uint32_t i = 0; i < c->r1; i++) {
        uint32_t gp = r32(mem, c->r2 + i * 4u);
        ptrs[i] = gp ? (const char *)(mem + gp) : "";
    }
    const GLint *lens = c->r3 ? (const GLint *)(mem + c->r3) : NULL;
    size_t total = 0;
    for (uint32_t i = 0; i < c->r1; i++) {
        total += (lens && lens[i] >= 0) ? (size_t)lens[i] : strlen(ptrs[i]);
    }
    char *buf = (char *)malloc(total + 1u);
    if (buf) {
        size_t pos = 0;
        for (uint32_t i = 0; i < c->r1; i++) {
            size_t l = (lens && lens[i] >= 0) ? (size_t)lens[i] : strlen(ptrs[i]);
            memcpy(buf + pos, ptrs[i], l);
            pos += l;
        }
        buf[pos] = '\0';
        /* Shipped Gameloft GLSL has "#endif]" typo ignored on Android drivers but rejected by ANGLE. */
        for (char *q = strstr(buf, "#endif]"); q; q = strstr(q, "#endif]")) {
            q[6] = ' ';
        }
        if (c->r0 < 256u) {
            free(g_shader_src[c->r0]);
            g_shader_src[c->r0] = buf;
        }
        const char *patched = buf;
        p_glShaderSource(c->r0, 1, &patched, NULL);
        if (c->r0 >= 256u) free(buf);
    } else {
        p_glShaderSource(c->r0, (GLsizei)c->r1, ptrs, lens);
    }
}

static void glw_glBufferData(Cpu *c, uint8_t *mem) {
    if (p_glBufferData) p_glBufferData(c->r0, (ptrdiff_t)c->r1, c->r2 ? (const void *)(mem + c->r2) : NULL, c->r3);
}

static void glw_glBufferSubData(Cpu *c, uint8_t *mem) {
    if (p_glBufferSubData) p_glBufferSubData(c->r0, (ptrdiff_t)(int32_t)c->r1, (ptrdiff_t)c->r2, c->r3 ? (const void *)(mem + c->r3) : NULL);
}

static void glw_glVertexAttrib4f(Cpu *c, uint8_t *mem) {
    if (p_glVertexAttrib4f) p_glVertexAttrib4f(c->r0, f32(c->r1), f32(c->r2), f32(c->r3), f32(sp32(c, mem, 0)));
}

static void glw_glVertexAttribPointer(Cpu *c, uint8_t *mem) {
    uint32_t ptr_arg = sp32(c, mem, 1);
    const void *ptr = (ptr_arg >= 0x10000u && ptr_arg < guest_mem_size)
                      ? (const void *)(mem + ptr_arg) : (const void *)(uintptr_t)ptr_arg;
    if (p_glVertexAttribPointer) p_glVertexAttribPointer(c->r0, (GLint)c->r1, c->r2, (GLboolean)c->r3, (GLsizei)sp32(c, mem, 0), ptr);
}

static void glw_glUniformMatrix4fv(Cpu *c, uint8_t *mem) {
    if (p_glUniformMatrix4fv && c->r3) p_glUniformMatrix4fv((GLint)c->r0, (GLsizei)c->r1, (GLboolean)c->r2, (const GLfloat *)(mem + c->r3));
}

static void glw_glTexImage2D(Cpu *c, uint8_t *mem) {
    g_gl_texs++;
    uint32_t px = sp32(c, mem, 4);
    if (g_gl_trace && (GLint)c->r1 == 0) {
        uint32_t p0 = px ? r32(mem, px) : 0;
        uint32_t pm = px ? r32(mem, px + ((uint32_t)c->r3 * (sp32(c, mem, 0) / 2u) + c->r3 / 2u) * 4u) : 0;
        fprintf(stderr, "glTexImage2D ifmt=0x%04x fmt=0x%04x type=0x%04x %dx%d px0=0x%08x mid=0x%08x\n",
                c->r2, sp32(c, mem, 2), sp32(c, mem, 3), c->r3, sp32(c, mem, 0), p0, pm);
    }
    if (p_glTexImage2D) {
        p_glTexImage2D(c->r0, (GLint)c->r1, (GLint)c->r2, (GLsizei)c->r3, (GLsizei)sp32(c, mem, 0),
                       (GLint)sp32(c, mem, 1), sp32(c, mem, 2), sp32(c, mem, 3),
                       px ? (const void *)(mem + px) : NULL);
    }
}

static void glw_glTexSubImage2D(Cpu *c, uint8_t *mem) {
    g_gl_texs++;
    uint32_t px = sp32(c, mem, 4);
    if (p_glTexSubImage2D) {
        p_glTexSubImage2D(c->r0, (GLint)c->r1, (GLint)c->r2, (GLint)c->r3, (GLsizei)sp32(c, mem, 0),
                          (GLsizei)sp32(c, mem, 1), sp32(c, mem, 2), sp32(c, mem, 3),
                          px ? (const void *)(mem + px) : NULL);
    }
}

static void glw_glCopyTexSubImage2D(Cpu *c, uint8_t *mem) {
    if (p_glCopyTexSubImage2D) {
        p_glCopyTexSubImage2D(c->r0, (GLint)c->r1, (GLint)c->r2, (GLint)c->r3,
                              (GLint)sp32(c, mem, 0), (GLint)sp32(c, mem, 1),
                              (GLsizei)sp32(c, mem, 2), (GLsizei)sp32(c, mem, 3));
    }
}

static void glw_glCompressedTexImage2D(Cpu *c, uint8_t *mem) {
    g_gl_comp_texs++;
    GLenum target = c->r0;
    GLint level = (GLint)c->r1;
    GLenum ifmt = c->r2;
    GLsizei w = (GLsizei)c->r3;
    GLsizei h = (GLsizei)sp32(c, mem, 0);
    GLint border = (GLint)sp32(c, mem, 1);
    GLsizei sz = (GLsizei)sp32(c, mem, 2);
    uint32_t datap = sp32(c, mem, 3);
    const uint8_t *data = (datap && guest_ok(datap, (uint32_t)sz)) ? (mem + datap) : NULL;
    if (g_gl_trace) {
        fprintf(stderr, "glCompressedTexImage2D fmt=0x%04x %dx%d level=%d sz=%d\n",
                ifmt, w, h, level, sz);
    }
    uint8_t *rgba = decompress_compressed(ifmt, w, h, data, sz);
    if (rgba) {
        if (p_glTexImage2D) p_glTexImage2D(target, level, 0x1908, w, h, border, 0x1908u, 0x1401u, rgba);
        free(rgba);
        return;
    }
    if (p_glCompressedTexImage2D) p_glCompressedTexImage2D(target, level, ifmt, w, h, border, sz, data);
}

static void glw_glCompressedTexSubImage2D(Cpu *c, uint8_t *mem) {
    uint32_t datap = sp32(c, mem, 4);
    if (p_glCompressedTexSubImage2D) {
        p_glCompressedTexSubImage2D(c->r0, (GLint)c->r1, (GLint)c->r2, (GLint)c->r3,
                                    (GLsizei)sp32(c, mem, 0), (GLsizei)sp32(c, mem, 1),
                                    sp32(c, mem, 2), (GLsizei)sp32(c, mem, 3),
                                    datap ? (const void *)(mem + datap) : NULL);
    }
}

static void glw_glReadPixels(Cpu *c, uint8_t *mem) {
    uint32_t px = sp32(c, mem, 2);
    if (p_glReadPixels && px) {
        p_glReadPixels((GLint)c->r0, (GLint)c->r1, (GLsizei)c->r2, (GLsizei)c->r3,
                       sp32(c, mem, 0), sp32(c, mem, 1), mem + px);
    }
}

typedef struct {
    const char *name;
    void **proc_slot;
    GlHandlerFn handler;
} GlEntry;

#define E_NOP(fn) { #fn, NULL, glw_##fn }
#define E_GL(fn)  { #fn, (void **)&p_##fn, glw_##fn }

static const GlEntry k_gl_entries[] = {
    E_NOP(glShadeModel), E_NOP(glMatrixMode), E_NOP(glLoadIdentity),
    E_GL(glGetError), E_GL(glGetString), E_GL(glGetIntegerv), E_GL(glGetFloatv),
    E_GL(glClear), E_GL(glEnable), E_GL(glDisable), E_GL(glActiveTexture),
    E_GL(glCullFace), E_GL(glFrontFace), E_GL(glDepthFunc), E_GL(glDepthMask),
    E_GL(glClearStencil), E_GL(glStencilMask), E_GL(glUseProgram),
    E_GL(glEnableVertexAttribArray), E_GL(glDisableVertexAttribArray),
    E_GL(glCompileShader), E_GL(glLinkProgram), E_GL(glDeleteShader),
    E_GL(glDeleteProgram), E_GL(glGenerateMipmap), E_GL(glBlendEquation),
    E_GL(glFlush), E_GL(glClearDepthf), E_GL(glLineWidth),
    E_GL(glDepthRangef), E_GL(glPolygonOffset), E_GL(glSampleCoverage),
    E_GL(glClearColor), E_GL(glBlendColor), E_GL(glColorMask),
    E_GL(glViewport), E_GL(glScissor),
    E_GL(glBindBuffer), E_GL(glBindTexture), E_GL(glBindFramebuffer), E_GL(glBindRenderbuffer),
    E_GL(glBlendFunc), E_GL(glAttachShader), E_GL(glPixelStorei), E_GL(glHint), E_GL(glUniform1i),
    E_GL(glUniform1f), E_GL(glTexParameteri), E_GL(glStencilFunc), E_GL(glStencilOp),
    E_GL(glDrawArrays), E_GL(glTexParameterf), E_GL(glDrawElements),
    E_GL(glRenderbufferStorage), E_GL(glFramebufferRenderbuffer), E_GL(glFramebufferTexture2D),
    E_GL(glGenBuffers), E_GL(glGenTextures), E_GL(glGenFramebuffers), E_GL(glGenRenderbuffers),
    E_GL(glDeleteBuffers), E_GL(glDeleteTextures), E_GL(glDeleteFramebuffers), E_GL(glDeleteRenderbuffers),
    E_GL(glCreateShader), E_GL(glCreateProgram), E_GL(glCheckFramebufferStatus),
    E_GL(glGetAttribLocation), E_GL(glGetUniformLocation),
    E_GL(glGetShaderiv), E_GL(glGetProgramiv), E_GL(glGetShaderInfoLog), E_GL(glGetProgramInfoLog),
    E_GL(glGetActiveAttrib), E_GL(glGetActiveUniform), E_GL(glShaderSource),
    E_GL(glBufferData), E_GL(glBufferSubData), E_GL(glVertexAttrib4f), E_GL(glVertexAttribPointer),
    E_GL(glUniform1fv), E_GL(glUniform2fv), E_GL(glUniform3fv), E_GL(glUniform4fv),
    E_GL(glUniform1iv), E_GL(glUniform2iv), E_GL(glUniform3iv), E_GL(glUniform4iv),
    E_GL(glUniformMatrix4fv),
    E_GL(glTexImage2D), E_GL(glTexSubImage2D), E_GL(glCopyTexSubImage2D),
    E_GL(glCompressedTexImage2D), E_GL(glCompressedTexSubImage2D), E_GL(glReadPixels),
};

static void init_gl_procs(void) {
    if (g_loaded) return;
    g_loaded = 1;
    g_gl_trace = getenv("ASPHALT6_GL_TRACE") != NULL;
    g_gles = GetModuleHandleA("libGLESv2.dll");
    if (!g_gles) g_gles = LoadLibraryA("libGLESv2.dll");
    size_t n = sizeof(k_gl_entries) / sizeof(k_gl_entries[0]);
    for (size_t i = 0; i < n; i++) {
        if (k_gl_entries[i].proc_slot) {
            *k_gl_entries[i].proc_slot = resolve_gl_proc(k_gl_entries[i].name);
        }
    }
}

GlHandlerFn gl_bridge_lookup(const char *n) {
    init_gl_procs();
    size_t count = sizeof(k_gl_entries) / sizeof(k_gl_entries[0]);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(k_gl_entries[i].name, n) == 0) return k_gl_entries[i].handler;
    }
    return NULL;
}

int gl_bridge_call(Cpu *c, uint8_t *mem, const char *n) {
    GlHandlerFn fn = gl_bridge_lookup(n);
    if (!fn) return 0;
    fn(c, mem);
    return 1;
}
#endif /* _WIN32 */
