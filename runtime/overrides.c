/*
 * Targeted native hooks for Asphalt 6 HD (license bypass, WinMM waveOut audio pump,
 * Mini Cooper car paint overlay fix, debug printer log).
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu.h"
#include "mem.h"

void call_target(Cpu *c, uint8_t *mem, uint32_t target);

/* Verified libasphalt6.so addresses:
 *   0x42cd90  ALicenseCheck::GetIMEI
 *   0x51fb24  T_TextureHelper::CalcOverlayPixel (blue tint/overlay luminance preservation)
 *   0x8cdf34  vox::DriverAndroid::_InitOSL
 *   0x8ce1c4  vox::DriverAndroid::_InitAT
 *   0x8cedd0  vox::DriverCallbackSourceInterface::SetDriverSampleRate
 *   0x8d1850  vox::DriverCallbackInterface::FillBuffer (locks drv+4 around 0x8d170c _FillBuffer)
 *   0xa7a99c  _PrinterLog1
 *   0xa7a9e4  _PrinterLog
 *   0xb38004  ALicenseCheck_InitLicense
 *   0xb38d54  ALicenseCheck_ValidateLicense
 */
#define FN_GET_IMEI           0x42cd90u
#define FN_IS_LOWRES_PLAT     0x50ca98u
#define FN_IS_LOWRES_SCREEN   0x50cb3cu
#define FN_CALC_OVERLAY       0x51fb24u
#define FN_IS_USING_LOWRES_TX 0x6a8a8cu
#define FN_VOX_INIT_OSL       0x8cdf34u
#define FN_VOX_INIT_AT        0x8ce1c4u
#define FN_VOX_SET_RATE       0x8cedd0u
#define FN_VOX_FILL_BUFFER    0x8d1850u
#define FN_PRINTER_LOG1       0xa7a99cu
#define FN_PRINTER_LOG        0xa7a9e4u
#define FN_INIT_LICENSE       0xb38004u
#define FN_VALIDATE_LICENSE   0xb38d54u

#ifdef _WIN32
#define AUDIO_BUFFERS 6
#define AUDIO_FRAMES  1024u
#define AUDIO_BUF_ADDR 0x1220000u

static HWAVEOUT g_wave = NULL;
static WAVEHDR g_hdr[AUDIO_BUFFERS];
static short *g_pcm[AUDIO_BUFFERS];
static volatile int g_audio_running = 0;
static HANDLE g_audio_thread = NULL;
static uint8_t *g_audio_mem = NULL;
static uint32_t g_audio_drv = 0;
static uint32_t g_audio_buf_count = 0;
static uint32_t g_audio_nonzero_bufs = 0;
static int g_audio_peak = 0;

static DWORD WINAPI audio_pump(LPVOID unused) {
    (void)unused;
    Cpu *cpu = (Cpu *)calloc(1, sizeof(Cpu));
    if (!cpu) return 1;
    int idx = 0;
    int trace = getenv("ASPHALT6_AUDIO_TRACE") != NULL;
    while (g_audio_running && g_wave && g_audio_mem && g_audio_drv) {
        WAVEHDR *h = &g_hdr[idx];
        while ((h->dwFlags & WHDR_INQUEUE) && g_audio_running) {
            Sleep(2);
        }
        if (!g_audio_running) break;

        memset(g_audio_mem + AUDIO_BUF_ADDR, 0, AUDIO_FRAMES * 4u);
        memset(cpu, 0, sizeof(*cpu));
        cpu->r0 = g_audio_drv;
        cpu->r1 = AUDIO_BUF_ADDR;
        cpu->r2 = AUDIO_FRAMES;
        cpu->sp = 0x01FC0000u; /* dedicated audio stack below 0x02000000 heap */
        cpu->lr = 0xDEADBEE0u;
        call_target(cpu, g_audio_mem, FN_VOX_FILL_BUFFER);

        memcpy(g_pcm[idx], g_audio_mem + AUDIO_BUF_ADDR, AUDIO_FRAMES * 4u);
        int buf_peak = 0;
        for (uint32_t s = 0; s < AUDIO_FRAMES * 2u; s++) {
            int v = g_pcm[idx][s];
            if (v < 0) v = -v;
            if (v > buf_peak) buf_peak = v;
        }
        g_audio_buf_count++;
        if (buf_peak > 0) g_audio_nonzero_bufs++;
        if (buf_peak > g_audio_peak) g_audio_peak = buf_peak;
        if (trace && (g_audio_buf_count % 25u == 0u || (buf_peak > 0 && g_audio_nonzero_bufs <= 3u))) {
            fprintf(stderr, "audio_trace: bufs=%u nonzero=%u buf_peak=%d max_peak=%d\n",
                    g_audio_buf_count, g_audio_nonzero_bufs, buf_peak, g_audio_peak);
        }
        waveOutWrite(g_wave, h, sizeof(*h));
        idx = (idx + 1) % AUDIO_BUFFERS;
    }
    free(cpu);
    return 0;
}

static void audio_start(uint8_t *mem, uint32_t drv) {
    if (g_audio_thread) return;
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = 44100;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = 44100u * 4u;
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "audio: waveOutOpen failed\n");
        return;
    }
    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        g_pcm[i] = (short *)calloc(AUDIO_FRAMES, 4);
        memset(&g_hdr[i], 0, sizeof(g_hdr[i]));
        g_hdr[i].lpData = (LPSTR)g_pcm[i];
        g_hdr[i].dwBufferLength = AUDIO_FRAMES * 4u;
        waveOutPrepareHeader(g_wave, &g_hdr[i], sizeof(g_hdr[i]));
    }
    g_audio_mem = mem;
    g_audio_drv = drv;
    g_audio_running = 1;
    g_audio_thread = CreateThread(NULL, 0, audio_pump, NULL, 0, NULL);
    fprintf(stderr, "audio: WinMM waveOut started at 44100 Hz (drv=0x%08x)\n", drv);
}
#endif

static uint32_t g_hook_bitmap[0x10000u / 32u];
static int g_hook_ready = 0;

static void hook_init(void) {
    static const uint32_t addrs[] = {
        FN_GET_IMEI, FN_IS_LOWRES_PLAT, FN_IS_LOWRES_SCREEN,
        FN_CALC_OVERLAY, FN_IS_USING_LOWRES_TX, FN_VOX_INIT_OSL, FN_VOX_INIT_AT,
        FN_PRINTER_LOG1, FN_PRINTER_LOG, FN_INIT_LICENSE, FN_VALIDATE_LICENSE,
    };
    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        uint32_t p = addrs[i] >> 16;
        g_hook_bitmap[p >> 5] |= 1u << (p & 31u);
    }
    g_hook_ready = 1;
}

int call_override(Cpu *c, uint8_t *mem, uint32_t target) {
    if (!g_hook_ready) hook_init();
    uint32_t page = target >> 16;
    if (page >= 0x10000u || !(g_hook_bitmap[page >> 5] & (1u << (page & 31u)))) {
        return 0;
    }

    if (target == FN_IS_LOWRES_PLAT || target == FN_IS_LOWRES_SCREEN || target == FN_IS_USING_LOWRES_TX) {
        c->r0 = 0u;
        return 1;
    }
    if (target == FN_CALC_OVERLAY) {
        uint32_t in_px = c->r1;
        uint32_t mask_px = c->r2;
        /* Input pixels from CarTga TGAs are BGRA (B=0..7, G=8..15, R=16..23, A=24..31),
         * while glTexImage2D(GL_RGBA) consumes RGBA (R=0..7, G=8..15, B=16..23, A=24..31). */
        uint32_t in_rgba = (in_px & 0xff00ff00u) | ((in_px & 0xffu) << 16) | ((in_px >> 16) & 0xffu);
        if (mask_px == 0xff000000u) {
            c->r0 = in_rgba;
            return 1;
        }
        const uint8_t *b = (c->r3 && guest_ok(c->r3, 12u)) ? (mem + c->r3) : NULL;
        if (!b) { c->r0 = in_rgba; return 1; }
        int m3 = 255 - (int)(mask_px >> 24);
        int m2 = (int)((mask_px >> 16) & 0xffu);
        int m1 = (int)((mask_px >> 8) & 0xffu);
        int m0 = (int)(mask_px & 0xffu);
        int mmax = m3;
        if (m2 > mmax) mmax = m2;
        if (m1 > mmax) mmax = m1;
        if (m0 > mmax) mmax = m0;
        int tr = 0, tg = 0, tb = 0;
        float strength = 0.0f;
        if (m3 == mmax) { tr = b[9]; tg = b[10]; tb = b[11]; strength = (float)m3 / 255.0f; }
        else if (m0 == mmax) { tr = b[6]; tg = b[7]; tb = b[8]; strength = (float)m0 / 255.0f; }
        else if (m1 == mmax) { tr = b[0]; tg = b[1]; tb = b[2]; strength = (float)m1 / 255.0f; }
        else if (m2 == mmax) { tr = b[3]; tg = b[4]; tb = b[5]; strength = (float)m2 / 255.0f; }
        if (strength > 1.0f) strength = 1.0f;
        int in_a = (int)(in_px >> 24);
        int in_r = (int)((in_px >> 16) & 0xffu);
        int in_g = (int)((in_px >> 8) & 0xffu);
        int in_b = (int)(in_px & 0xffu);
        int fr = (int)((float)tr * strength - (float)(128 - in_r));
        int fg = (int)((float)tg * strength - (float)(128 - in_g));
        int fb = (int)((float)tb * strength - (float)(128 - in_b));
        if (fr < 0) fr = 0; else if (fr > 255) fr = 255;
        if (fg < 0) fg = 0; else if (fg > 255) fg = 255;
        if (fb < 0) fb = 0; else if (fb > 255) fb = 255;
        /* Call guest premultiply flag helper or match premul when flag != 0 */
        Cpu s = *c;
        s.lr = 0xDEADBEE0u;
        call_target(&s, mem, 0x438fd4u);
        if (s.r0 != 0) {
            fr = (fr * in_a) / 255;
            fg = (fg * in_a) / 255;
            fb = (fb * in_a) / 255;
        }
        /* Output GL_RGBA little-endian: R in bits 0..7, G in 8..15, B in 16..23, A in 24..31 */
        c->r0 = (uint32_t)fr | ((uint32_t)fg << 8) | ((uint32_t)fb << 16) | ((uint32_t)in_a << 24);
        return 1;
    }
    if (target == FN_PRINTER_LOG1) {
        const char *msg = (c->r0 && c->r0 < guest_mem_size) ? (const char *)(mem + c->r0) : "";
        fprintf(stderr, "[glitch:%u] %s\n", c->r1, msg);
        return 1;
    }
    if (target == FN_PRINTER_LOG) {
        const char *tag = (c->r0 && c->r0 < guest_mem_size) ? (const char *)(mem + c->r0) : "";
        const char *msg = (c->r1 && c->r1 < guest_mem_size) ? (const char *)(mem + c->r1) : "";
        fprintf(stderr, "[glitch:%u] %s: %s\n", c->r2, tag, msg);
        return 1;
    }
    if (target == FN_INIT_LICENSE || target == FN_VALIDATE_LICENSE) {
        c->r0 = 1u;
        return 1;
    }
    if (target == FN_GET_IMEI) {
        static const char imei[] = "000000000000000";
        uint32_t dst = c->r1 ? c->r1 : 0x1219600u;
        if (guest_ok(dst, sizeof(imei))) memcpy(mem + dst, imei, sizeof(imei));
        c->r0 = dst;
        return 1;
    }
    if (target == FN_VOX_INIT_AT || target == FN_VOX_INIT_OSL) {
        uint32_t drv = c->r0;
        if (!drv || drv + 0x70u >= guest_mem_size) return 0;
        Cpu s = *c;
        s.r0 = 44100u;
        s.lr = 0xDEADBEE0u;
        call_target(&s, mem, FN_VOX_SET_RATE);

        st4(mem, drv + 0x54u, 1u);
        st4(mem, drv + 0x58u, 1024u);
        st4(mem, drv + 0x5cu, 1024u);
        mem[drv + 0x60u] = 0;
        mem[drv + 0x08u] = 1;
#ifdef _WIN32
        audio_start(mem, drv);
#endif
        c->r0 = 1u;
        return 1;
    }
    return 0;
}
