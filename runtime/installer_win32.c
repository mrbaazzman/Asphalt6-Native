/* Pure C11 native Windows APK game-data downloader & CRC-32 verifier.
 *
 * Compiles BOTH as a module linked into asphalt6_win.exe (providing
 * installer_ensure_gamedata() at launch) AND as the standalone executable
 * Asphalt6_Installer.exe when built with -DINSTALLER_STANDALONE.
 *
 * Zero third-party dependencies: bundles RFC 1951 DEFLATE (inflate), IEEE
 * CRC-32, ZIP parser, pack*.info manifest parser, WinHTTP Range downloader
 * (loaded dynamically from winhttp.dll), and a native Win32 progress dialog.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "installer.h"

/* ---- WinHTTP minimal ABI declarations (loaded dynamically) ---- */
typedef void *HINTERNET;
typedef struct {
    DWORD dwStructSize;
    LPWSTR lpszScheme;
    DWORD dwSchemeLength;
    int nScheme;
    LPWSTR lpszHostName;
    DWORD dwHostNameLength;
    WORD nPort;
    LPWSTR lpszUserName;
    DWORD dwUserNameLength;
    LPWSTR lpszPassword;
    DWORD dwPasswordLength;
    LPWSTR lpszUrlPath;
    DWORD dwUrlPathLength;
    LPWSTR lpszExtraInfo;
    DWORD dwExtraInfoLength;
} INST_URL_COMPONENTS;

#define INST_WINHTTP_ACCESS_TYPE_DEFAULT_PROXY 0
#define INST_WINHTTP_FLAG_SECURE 0x00800000u
#define INST_WINHTTP_QUERY_CONTENT_LENGTH 5
#define INST_WINHTTP_QUERY_STATUS_CODE 19
#define INST_WINHTTP_QUERY_FLAG_NUMBER 0x20000000u
#define INST_WINHTTP_ADDREQ_FLAG_ADD 0x20000000u
#define INST_WINHTTP_ADDREQ_FLAG_REPLACE 0x80000000u

typedef HINTERNET (WINAPI *pfn_WinHttpOpen)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET (WINAPI *pfn_WinHttpConnect)(HINTERNET, LPCWSTR, WORD, DWORD);
typedef HINTERNET (WINAPI *pfn_WinHttpOpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL (WINAPI *pfn_WinHttpAddRequestHeaders)(HINTERNET, LPCWSTR, DWORD, DWORD);
typedef BOOL (WINAPI *pfn_WinHttpSendRequest)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL (WINAPI *pfn_WinHttpReceiveResponse)(HINTERNET, LPVOID);
typedef BOOL (WINAPI *pfn_WinHttpQueryHeaders)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL (WINAPI *pfn_WinHttpQueryDataAvailable)(HINTERNET, LPDWORD);
typedef BOOL (WINAPI *pfn_WinHttpReadData)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *pfn_WinHttpCloseHandle)(HINTERNET);
typedef BOOL (WINAPI *pfn_WinHttpCrackUrl)(LPCWSTR, DWORD, DWORD, INST_URL_COMPONENTS *);
typedef BOOL (WINAPI *pfn_WinHttpSetTimeouts)(HINTERNET, int, int, int, int);

typedef BOOL (WINAPI *pfn_GetOpenFileNameA)(LPOPENFILENAMEA);

typedef struct {
    HMODULE mod;
    pfn_WinHttpOpen Open;
    pfn_WinHttpConnect Connect;
    pfn_WinHttpOpenRequest OpenRequest;
    pfn_WinHttpAddRequestHeaders AddRequestHeaders;
    pfn_WinHttpSendRequest SendRequest;
    pfn_WinHttpReceiveResponse ReceiveResponse;
    pfn_WinHttpQueryHeaders QueryHeaders;
    pfn_WinHttpQueryDataAvailable QueryDataAvailable;
    pfn_WinHttpReadData ReadData;
    pfn_WinHttpCloseHandle CloseHandle;
    pfn_WinHttpCrackUrl CrackUrl;
    pfn_WinHttpSetTimeouts SetTimeouts;
} WinHttpApi;

static int load_winhttp(WinHttpApi *api) {
    memset(api, 0, sizeof(*api));
    api->mod = LoadLibraryA("winhttp.dll");
    if (!api->mod) return 0;
#define WH_SYM(field, name) do { \
    FARPROC fp = GetProcAddress(api->mod, name); \
    if (!fp) return 0; \
    memcpy(&api->field, &fp, sizeof(fp)); \
} while (0)
    WH_SYM(Open, "WinHttpOpen");
    WH_SYM(Connect, "WinHttpConnect");
    WH_SYM(OpenRequest, "WinHttpOpenRequest");
    WH_SYM(AddRequestHeaders, "WinHttpAddRequestHeaders");
    WH_SYM(SendRequest, "WinHttpSendRequest");
    WH_SYM(ReceiveResponse, "WinHttpReceiveResponse");
    WH_SYM(QueryHeaders, "WinHttpQueryHeaders");
    WH_SYM(QueryDataAvailable, "WinHttpQueryDataAvailable");
    WH_SYM(ReadData, "WinHttpReadData");
    WH_SYM(CloseHandle, "WinHttpCloseHandle");
    WH_SYM(CrackUrl, "WinHttpCrackUrl");
    WH_SYM(SetTimeouts, "WinHttpSetTimeouts");
#undef WH_SYM
    return 1;
}

/* ---- IEEE CRC-32 ---- */
static uint32_t g_crc_table[256];
static int g_crc_init = 0;

static void init_crc32(void) {
    if (g_crc_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[i] = c;
    }
    g_crc_init = 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len) {
    init_crc32();
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c = g_crc_table[(c ^ buf[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static int file_crc32_and_size(const char *path, uint32_t *out_crc, uint64_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t buf[65536];
    uint32_t crc = 0;
    uint64_t total = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crc = crc32_update(crc, buf, n);
        total += n;
    }
    int err = ferror(f);
    fclose(f);
    if (err) return 0;
    *out_crc = crc;
    *out_size = total;
    return 1;
}

/* ---- Pure C11 RFC 1951 Raw DEFLATE (inflate) ---- */
#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    const uint8_t *in;
    size_t in_len;
    size_t in_pos;
    uint8_t *out;
    size_t out_cap;
    size_t out_pos;
    uint32_t bitbuf;
    int bitcnt;
} InfState;

typedef struct {
    int16_t count[MAXBITS + 1];
    int16_t symbol[FIXLCODES];
} HuffTable;

static int inf_bits(InfState *s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) return -1;
        val |= ((uint32_t)s->in[s->in_pos++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1u));
}

static void inf_align_byte(InfState *s) {
    int full_bytes = s->bitcnt >> 3;
    if (full_bytes > 0 && s->in_pos >= (size_t)full_bytes) {
        s->in_pos -= (size_t)full_bytes;
    }
    s->bitbuf = 0;
    s->bitcnt = 0;
}

static int huff_build(HuffTable *h, const int16_t *lengths, int n) {
    int16_t offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) {
        if (lengths[sym] < 0 || lengths[sym] > MAXBITS) return -1;
        h->count[lengths[sym]]++;
    }
    if (h->count[0] == n) return 0;

    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }

    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) {
        offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    }
    for (int sym = 0; sym < n; sym++) {
        if (lengths[sym] != 0) {
            h->symbol[offs[lengths[sym]]++] = (int16_t)sym;
        }
    }
    return left;
}

static int huff_decode(InfState *s, const HuffTable *h) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int b = inf_bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int inflate_codes(InfState *s, const HuffTable *lencode, const HuffTable *distcode) {
    static const int16_t lens[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const int16_t lext[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const int16_t dists[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
    };
    static const int16_t dext[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };

    for (;;) {
        int sym = huff_decode(s, lencode);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->out_pos >= s->out_cap) return -1;
            s->out[s->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int ex = inf_bits(s, lext[sym]);
            if (ex < 0) return -1;
            size_t len = (size_t)(lens[sym] + ex);

            int dsym = huff_decode(s, distcode);
            if (dsym < 0 || dsym >= 30) return -1;
            int dex = inf_bits(s, dext[dsym]);
            if (dex < 0) return -1;
            size_t dist = (size_t)(dists[dsym] + dex);
            if (dist == 0 || dist > s->out_pos || s->out_pos + len > s->out_cap) return -1;

            for (size_t i = 0; i < len; i++) {
                s->out[s->out_pos + i] = s->out[s->out_pos - dist + i];
            }
            s->out_pos += len;
        }
    }
}

static int inflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len) {
    InfState s;
    memset(&s, 0, sizeof(s));
    s.in = in;
    s.in_len = in_len;
    s.out = out;
    s.out_cap = out_cap;

    int last;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (last < 0 || type < 0) return -1;

        if (type == 0) {
            inf_align_byte(&s);
            if (s.in_pos + 4 > s.in_len) return -1;
            uint16_t len = (uint16_t)(s.in[s.in_pos] | (s.in[s.in_pos + 1] << 8));
            uint16_t nlen = (uint16_t)(s.in[s.in_pos + 2] | (s.in[s.in_pos + 3] << 8));
            s.in_pos += 4;
            if ((uint16_t)~len != nlen) return -1;
            if (s.in_pos + len > s.in_len || s.out_pos + len > s.out_cap) return -1;
            memcpy(s.out + s.out_pos, s.in + s.in_pos, len);
            s.in_pos += len;
            s.out_pos += len;
        } else if (type == 1) {
            int16_t lengths[FIXLCODES];
            for (int i = 0; i < 144; i++) lengths[i] = 8;
            for (int i = 144; i < 256; i++) lengths[i] = 9;
            for (int i = 256; i < 280; i++) lengths[i] = 7;
            for (int i = 280; i < FIXLCODES; i++) lengths[i] = 8;
            HuffTable lencode, distcode;
            if (huff_build(&lencode, lengths, FIXLCODES) < 0) return -1;
            for (int i = 0; i < MAXDCODES; i++) lengths[i] = 5;
            huff_build(&distcode, lengths, MAXDCODES);
            if (inflate_codes(&s, &lencode, &distcode) != 0) return -1;
        } else if (type == 2) {
            static const int16_t order[19] = {
                16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
            };
            int nlen = inf_bits(&s, 5);
            int ndist = inf_bits(&s, 5);
            int ncode = inf_bits(&s, 4);
            if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
            nlen += 257;
            ndist += 1;
            ncode += 4;
            if (nlen > MAXLCODES || ndist > MAXDCODES) return -1;

            int16_t lengths[MAXLCODES + MAXDCODES];
            for (int i = 0; i < ncode; i++) {
                int v = inf_bits(&s, 3);
                if (v < 0) return -1;
                lengths[order[i]] = (int16_t)v;
            }
            for (int i = ncode; i < 19; i++) lengths[order[i]] = 0;

            HuffTable lencode, distcode;
            if (huff_build(&lencode, lengths, 19) != 0) return -1;

            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = huff_decode(&s, &lencode);
                if (sym < 0) return -1;
                if (sym < 16) {
                    lengths[idx++] = (int16_t)sym;
                } else {
                    int16_t rep_val = 0;
                    int rep_cnt = 0;
                    if (sym == 16) {
                        if (idx == 0) return -1;
                        rep_val = lengths[idx - 1];
                        int b = inf_bits(&s, 2);
                        if (b < 0) return -1;
                        rep_cnt = 3 + b;
                    } else if (sym == 17) {
                        int b = inf_bits(&s, 3);
                        if (b < 0) return -1;
                        rep_cnt = 3 + b;
                    } else {
                        int b = inf_bits(&s, 7);
                        if (b < 0) return -1;
                        rep_cnt = 11 + b;
                    }
                    if (idx + rep_cnt > nlen + ndist) return -1;
                    while (rep_cnt-- > 0) lengths[idx++] = rep_val;
                }
            }
            if (lengths[256] == 0) return -1;
            if (huff_build(&lencode, lengths, nlen) < 0) return -1;
            if (huff_build(&distcode, lengths + nlen, ndist) < 0) return -1;
            if (inflate_codes(&s, &lencode, &distcode) != 0) return -1;
        } else {
            return -1;
        }
    } while (!last);

    *out_len = s.out_pos;
    return 0;
}

/* ---- Minimal Pure C11 PNG Decoder (reusing inflate_raw, 8-bit ct=0,2,3,4,6) ---- */
static uint8_t paeth_pred(uint8_t a, uint8_t b, uint8_t c) {
    int p = (int)a + (int)b - (int)c;
    int pa = p >= (int)a ? p - (int)a : (int)a - p;
    int pb = p >= (int)b ? p - (int)b : (int)b - p;
    int pc = p >= (int)c ? p - (int)c : (int)c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

int installer_decode_png_mem(const uint8_t *png, size_t png_len,
                             uint8_t **out_rgba, int *out_w, int *out_h) {
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (!png || png_len < 33 || memcmp(png, sig, 8) != 0) return 0;

    uint32_t w = 0, h = 0;
    uint8_t bit_depth = 0, color_type = 0, interlace = 0;
    uint8_t plte[256 * 3];
    uint8_t trns[256];
    int has_ihdr = 0, has_plte = 0;
    size_t trns_len = 0;
    memset(trns, 255, sizeof(trns));

    uint8_t *idat = NULL;
    size_t idat_len = 0, idat_cap = 0;

    size_t pos = 8;
    while (pos + 12 <= png_len) {
        uint32_t clen = ((uint32_t)png[pos] << 24) | ((uint32_t)png[pos + 1] << 16) |
                        ((uint32_t)png[pos + 2] << 8) | (uint32_t)png[pos + 3];
        const uint8_t *ctype = png + pos + 4;
        const uint8_t *cdata = png + pos + 8;
        if (pos + 12 + (size_t)clen > png_len) break;

        if (memcmp(ctype, "IHDR", 4) == 0 && clen >= 13) {
            w = ((uint32_t)cdata[0] << 24) | ((uint32_t)cdata[1] << 16) |
                ((uint32_t)cdata[2] << 8) | (uint32_t)cdata[3];
            h = ((uint32_t)cdata[4] << 24) | ((uint32_t)cdata[5] << 16) |
                ((uint32_t)cdata[6] << 8) | (uint32_t)cdata[7];
            bit_depth = cdata[8];
            color_type = cdata[9];
            interlace = cdata[12];
            has_ihdr = 1;
        } else if (memcmp(ctype, "PLTE", 4) == 0 && clen <= sizeof(plte)) {
            memcpy(plte, cdata, clen);
            has_plte = 1;
        } else if (memcmp(ctype, "tRNS", 4) == 0 && clen <= sizeof(trns)) {
            memcpy(trns, cdata, clen);
            trns_len = clen;
        } else if (memcmp(ctype, "IDAT", 4) == 0 && clen > 0) {
            if (idat_len + clen > idat_cap) {
                size_t ncap = idat_cap ? idat_cap * 2 : (size_t)clen * 2 + 1024u;
                while (ncap < idat_len + clen) ncap *= 2;
                uint8_t *nb = (uint8_t *)realloc(idat, ncap);
                if (!nb) { free(idat); return 0; }
                idat = nb;
                idat_cap = ncap;
            }
            memcpy(idat + idat_len, cdata, clen);
            idat_len += clen;
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            break;
        }
        pos += 12u + (size_t)clen;
    }

    if (!has_ihdr || !idat || idat_len < 6 || w == 0 || h == 0 ||
        w > 4096 || h > 4096 || bit_depth != 8 || interlace != 0) {
        free(idat);
        return 0;
    }

    int bpp = 0;
    switch (color_type) {
    case 0: bpp = 1; break;
    case 2: bpp = 3; break;
    case 3: bpp = has_plte ? 1 : 0; break;
    case 4: bpp = 2; break;
    case 6: bpp = 4; break;
    default: bpp = 0; break;
    }
    if (bpp == 0) { free(idat); return 0; }

    size_t row_bytes = (size_t)w * (size_t)bpp;
    size_t raw_cap = (size_t)h * (1u + row_bytes);
    uint8_t *raw = (uint8_t *)malloc(raw_cap);
    if (!raw) { free(idat); return 0; }

    size_t raw_got = 0;
    int zok = inflate_raw(idat + 2, idat_len - 2, raw, raw_cap, &raw_got);
    free(idat);
    if (zok != 0 || raw_got < raw_cap) { free(raw); return 0; }

    uint8_t *rgba = (uint8_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!rgba) { free(raw); return 0; }

    uint8_t *prev = (uint8_t *)calloc(row_bytes, 1);
    uint8_t *curr = (uint8_t *)malloc(row_bytes);
    if (!prev || !curr) { free(prev); free(curr); free(raw); free(rgba); return 0; }

    int ok = 1;
    for (uint32_t y = 0; y < h && ok; y++) {
        const uint8_t *src = raw + (size_t)y * (1u + row_bytes);
        uint8_t ftype = src[0];
        const uint8_t *srow = src + 1;
        for (size_t x = 0; x < row_bytes; x++) {
            uint8_t a = (x >= (size_t)bpp) ? curr[x - (size_t)bpp] : 0;
            uint8_t b = prev[x];
            uint8_t c = (x >= (size_t)bpp) ? prev[x - (size_t)bpp] : 0;
            uint8_t v = srow[x];
            switch (ftype) {
            case 0: curr[x] = v; break;
            case 1: curr[x] = (uint8_t)(v + a); break;
            case 2: curr[x] = (uint8_t)(v + b); break;
            case 3: curr[x] = (uint8_t)(v + ((a + b) >> 1)); break;
            case 4: curr[x] = (uint8_t)(v + paeth_pred(a, b, c)); break;
            default: ok = 0; break;
            }
        }
        uint8_t *dst = rgba + (size_t)y * (size_t)w * 4u;
        for (uint32_t x = 0; x < w; x++) {
            if (color_type == 6) {
                memcpy(dst + x * 4u, curr + x * 4u, 4);
            } else if (color_type == 2) {
                dst[x * 4u + 0] = curr[x * 3u + 0];
                dst[x * 4u + 1] = curr[x * 3u + 1];
                dst[x * 4u + 2] = curr[x * 3u + 2];
                dst[x * 4u + 3] = 255;
            } else if (color_type == 3) {
                uint8_t idx = curr[x];
                dst[x * 4u + 0] = plte[idx * 3u + 0];
                dst[x * 4u + 1] = plte[idx * 3u + 1];
                dst[x * 4u + 2] = plte[idx * 3u + 2];
                dst[x * 4u + 3] = (idx < trns_len) ? trns[idx] : 255;
            } else if (color_type == 4) {
                dst[x * 4u + 0] = curr[x * 2u];
                dst[x * 4u + 1] = curr[x * 2u];
                dst[x * 4u + 2] = curr[x * 2u];
                dst[x * 4u + 3] = curr[x * 2u + 1];
            } else {
                dst[x * 4u + 0] = curr[x];
                dst[x * 4u + 1] = curr[x];
                dst[x * 4u + 2] = curr[x];
                dst[x * 4u + 3] = 255;
            }
        }
        memcpy(prev, curr, row_bytes);
    }

    free(prev);
    free(curr);
    free(raw);
    if (!ok) { free(rgba); return 0; }
    *out_rgba = rgba;
    *out_w = (int)w;
    *out_h = (int)h;
    return 1;
}

/* ---- Helpers ---- */
static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16be(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}
static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t rd64be(const uint8_t *p) {
    return ((uint64_t)rd32be(p) << 32) | (uint64_t)rd32be(p + 4);
}

static void mkdirs_for_file(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return;
    memcpy(tmp, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char ch = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] && tmp[strlen(tmp) - 1] != ':') {
                CreateDirectoryA(tmp, NULL);
            }
            tmp[i] = ch;
        }
    }
}

static void mkdirs_dir(const char *dir) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%.960s/placeholder", dir);
    mkdirs_for_file(tmp);
}

static const char *base_filename(const char *name) {
    const char *s = name;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\') s = p + 1;
    }
    return s;
}

static int write_file_atomic(const char *path, const uint8_t *data, size_t len) {
    mkdirs_for_file(path);
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%.980s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    size_t off = 0;
    while (off < len) {
        size_t w = fwrite(data + off, 1, len - off, f);
        if (w == 0) {
            fclose(f);
            DeleteFileA(tmp);
            return 0;
        }
        off += w;
    }
    fclose(f);
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp);
        return 0;
    }
    return 1;
}

/* ---- Minimal ZIP extraction from memory buffer & large file ---- */
static int zip_extract_first_from_mem(const uint8_t *zip_data, size_t zip_len,
                                      uint8_t **out_buf, size_t *out_len) {
    if (zip_len < 30 || rd32le(zip_data) != 0x04034B50u) return 0;
    uint16_t flags = rd16le(zip_data + 6);
    uint16_t method = rd16le(zip_data + 8);
    uint32_t expected_crc = rd32le(zip_data + 14);
    uint32_t comp_sz = rd32le(zip_data + 18);
    uint32_t uncomp_sz = rd32le(zip_data + 22);
    uint16_t fn_len = rd16le(zip_data + 26);
    uint16_t ex_len = rd16le(zip_data + 28);

    /* If data descriptor bit is set and sizes in LFH are 0, read from EOCD/CD. */
    size_t data_off = 30u + fn_len + ex_len;
    if ((flags & 0x08u) != 0 && (comp_sz == 0 || uncomp_sz == 0)) {
        size_t scan = zip_len > 65558u ? zip_len - 65558u : 0;
        for (size_t i = zip_len >= 22 ? zip_len - 22 : 0; ; i--) {
            if (rd32le(zip_data + i) == 0x06054B50u) {
                uint32_t cd_off = rd32le(zip_data + i + 16);
                if (cd_off + 46 <= zip_len && rd32le(zip_data + cd_off) == 0x02014B50u) {
                    expected_crc = rd32le(zip_data + cd_off + 16);
                    comp_sz = rd32le(zip_data + cd_off + 20);
                    uncomp_sz = rd32le(zip_data + cd_off + 24);
                }
                break;
            }
            if (i <= scan) break;
        }
    }

    if (data_off + comp_sz > zip_len || uncomp_sz > 256u * 1024u * 1024u) return 0;

    uint8_t *buf = (uint8_t *)malloc(uncomp_sz ? uncomp_sz : 1);
    if (!buf) return 0;

    if (method == 0) {
        if (comp_sz != uncomp_sz) { free(buf); return 0; }
        memcpy(buf, zip_data + data_off, uncomp_sz);
    } else if (method == 8) {
        size_t got = 0;
        if (inflate_raw(zip_data + data_off, comp_sz, buf, uncomp_sz, &got) != 0 || got != uncomp_sz) {
            free(buf);
            return 0;
        }
    } else {
        free(buf);
        return 0;
    }

    if (crc32_update(0, buf, uncomp_sz) != expected_crc) {
        free(buf);
        return 0;
    }
    *out_buf = buf;
    *out_len = uncomp_sz;
    return 1;
}

typedef struct {
    char *name;
    uint16_t method;
    uint32_t crc32;
    uint32_t comp_sz;
    uint32_t uncomp_sz;
    uint32_t lfh_off;
} ApkZipEntry;

typedef struct {
    FILE *fp;
    ApkZipEntry *entries;
    uint32_t count;
} ApkZip;

static void apk_zip_close(ApkZip *z) {
    if (z->fp) fclose(z->fp);
    for (uint32_t i = 0; i < z->count; i++) free(z->entries[i].name);
    free(z->entries);
    memset(z, 0, sizeof(*z));
}

static int apk_zip_open(const char *path, ApkZip *z) {
    memset(z, 0, sizeof(*z));
    z->fp = fopen(path, "rb");
    if (!z->fp) return 0;

    if (_fseeki64(z->fp, 0, SEEK_END) != 0) { apk_zip_close(z); return 0; }
    __int64 fsz64 = _ftelli64(z->fp);
    if (fsz64 < 22) { apk_zip_close(z); return 0; }

    size_t tail_sz = (size_t)(fsz64 > 65560 ? 65560 : fsz64);
    uint8_t *tail = (uint8_t *)malloc(tail_sz);
    if (!tail) { apk_zip_close(z); return 0; }
    _fseeki64(z->fp, fsz64 - (__int64)tail_sz, SEEK_SET);
    if (fread(tail, 1, tail_sz, z->fp) != tail_sz) { free(tail); apk_zip_close(z); return 0; }

    int found = 0;
    uint32_t cd_sz = 0, cd_off = 0;
    uint16_t total_entries = 0;
    for (size_t i = tail_sz - 22; ; i--) {
        if (rd32le(tail + i) == 0x06054B50u) {
            uint16_t comment_len = rd16le(tail + i + 20);
            if (i + 22 + comment_len <= tail_sz) {
                total_entries = rd16le(tail + i + 10);
                cd_sz = rd32le(tail + i + 12);
                cd_off = rd32le(tail + i + 16);
                found = 1;
                break;
            }
        }
        if (i == 0) break;
    }
    free(tail);
    if (!found || (__int64)cd_off + (__int64)cd_sz > fsz64) { apk_zip_close(z); return 0; }

    uint8_t *cd = (uint8_t *)malloc(cd_sz);
    if (!cd) { apk_zip_close(z); return 0; }
    _fseeki64(z->fp, (__int64)cd_off, SEEK_SET);
    if (fread(cd, 1, cd_sz, z->fp) != cd_sz) { free(cd); apk_zip_close(z); return 0; }

    z->entries = (ApkZipEntry *)calloc(total_entries ? total_entries : 1, sizeof(ApkZipEntry));
    if (!z->entries) { free(cd); apk_zip_close(z); return 0; }

    size_t pos = 0;
    while (pos + 46 <= cd_sz && z->count < total_entries) {
        if (rd32le(cd + pos) != 0x02014B50u) break;
        uint16_t method = rd16le(cd + pos + 10);
        uint32_t crc = rd32le(cd + pos + 16);
        uint32_t csz = rd32le(cd + pos + 20);
        uint32_t usz = rd32le(cd + pos + 24);
        uint16_t nlen = rd16le(cd + pos + 28);
        uint16_t xlen = rd16le(cd + pos + 30);
        uint16_t clen = rd16le(cd + pos + 32);
        uint32_t lfh = rd32le(cd + pos + 42);
        if (pos + 46 + nlen + xlen + clen > cd_sz) break;

        ApkZipEntry *e = &z->entries[z->count++];
        e->name = (char *)malloc((size_t)nlen + 1);
        if (!e->name) { free(cd); apk_zip_close(z); return 0; }
        memcpy(e->name, cd + pos + 46, nlen);
        e->name[nlen] = '\0';
        e->method = method;
        e->crc32 = crc;
        e->comp_sz = csz;
        e->uncomp_sz = usz;
        e->lfh_off = lfh;
        pos += 46u + nlen + xlen + clen;
    }
    free(cd);
    return z->count > 0;
}

static int apk_zip_read(const ApkZip *z, const ApkZipEntry *e, uint8_t **out_buf, size_t *out_len) {
    uint8_t lfh[30];
    _fseeki64(z->fp, (__int64)e->lfh_off, SEEK_SET);
    if (fread(lfh, 1, 30, z->fp) != 30 || rd32le(lfh) != 0x04034B50u) return 0;
    uint16_t nlen = rd16le(lfh + 26);
    uint16_t xlen = rd16le(lfh + 28);
    if (_fseeki64(z->fp, (__int64)nlen + (__int64)xlen, SEEK_CUR) != 0) return 0;

    uint8_t *comp = (uint8_t *)malloc(e->comp_sz ? e->comp_sz : 1);
    uint8_t *out = (uint8_t *)malloc(e->uncomp_sz ? e->uncomp_sz : 1);
    if (!comp || !out) { free(comp); free(out); return 0; }
    if (e->comp_sz > 0 && fread(comp, 1, e->comp_sz, z->fp) != e->comp_sz) {
        free(comp);
        free(out);
        return 0;
    }

    int ok = 0;
    if (e->method == 0) {
        if (e->comp_sz == e->uncomp_sz) {
            memcpy(out, comp, e->uncomp_sz);
            ok = 1;
        }
    } else if (e->method == 8) {
        size_t got = 0;
        if (inflate_raw(comp, e->comp_sz, out, e->uncomp_sz, &got) == 0 && got == e->uncomp_sz) {
            ok = 1;
        }
    }
    free(comp);
    if (ok && crc32_update(0, out, e->uncomp_sz) != e->crc32) ok = 0;
    if (!ok) {
        free(out);
        return 0;
    }
    *out_buf = out;
    *out_len = e->uncomp_sz;
    return 1;
}

static const ApkZipEntry *apk_zip_find(const ApkZip *z, const char *name) {
    for (uint32_t i = 0; i < z->count; i++) {
        if (strcmp(z->entries[i].name, name) == 0) return &z->entries[i];
    }
    return NULL;
}

/* ---- Manifest structures ---- */
typedef struct {
    uint32_t offset;
    uint32_t uncomp_size;
    uint32_t crc32;
    uint32_t zip_size;
    char dat_name[128];
} PackItem;

static int parse_data_txt_url(const char *txt, size_t len, char *out_variant, size_t var_cap,
                              char *out_url, size_t url_cap) {
    static const char *prefs[] = { "ATC", "PVRT", "DXT" };
    for (int pass = 0; pass < 4; pass++) {
        size_t i = 0;
        while (i < len) {
            size_t line_start = i;
            while (i < len && txt[i] != '\n' && txt[i] != '\r') i++;
            size_t line_end = i;
            while (i < len && (txt[i] == '\n' || txt[i] == '\r')) i++;
            while (line_start < line_end && isspace((unsigned char)txt[line_start])) line_start++;
            while (line_end > line_start && isspace((unsigned char)txt[line_end - 1])) line_end--;

            const char *colon = NULL;
            for (size_t k = line_start; k < line_end; k++) {
                if (txt[k] == ':') { colon = txt + k; break; }
            }
            if (!colon) continue;
            size_t klen = (size_t)(colon - (txt + line_start));
            const char *val = colon + 1;
            size_t vlen = (size_t)((txt + line_end) - val);
            while (vlen > 0 && isspace((unsigned char)*val)) { val++; vlen--; }
            if (vlen < 8 || (strncmp(val, "http://", 7) != 0 && strncmp(val, "https://", 8) != 0)) {
                continue;
            }
            if (pass < 3) {
                if (klen == strlen(prefs[pass]) && _strnicmp(txt + line_start, prefs[pass], klen) == 0) {
                    snprintf(out_variant, var_cap, "%.*s", (int)klen, txt + line_start);
                    snprintf(out_url, url_cap, "%.*s", (int)vlen, val);
                    return 1;
                }
            } else {
                snprintf(out_variant, var_cap, "%.*s", (int)klen, txt + line_start);
                snprintf(out_url, url_cap, "%.*s", (int)vlen, val);
                return 1;
            }
        }
    }
    return 0;
}

static int parse_pack_info_blob(const uint8_t *blob, size_t len, PackItem **out_items,
                                uint32_t *out_count, uint64_t *out_total_pack_bytes) {
    if (len < 8) return 0;
    uint32_t count = rd32be(blob + 4);
    if (count == 0 || count > 100000u) return 0;
    PackItem *items = (PackItem *)calloc(count, sizeof(PackItem));
    if (!items) return 0;

    size_t pos = 8;
    uint64_t max_end = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 18 > len) { free(items); return 0; }
        items[i].offset = rd32be(blob + pos);
        items[i].uncomp_size = rd32be(blob + pos + 4);
        uint64_t crc64 = rd64be(blob + pos + 8);
        items[i].crc32 = (uint32_t)(crc64 & 0xFFFFFFFFu);
        pos += 16;

        uint16_t dat_len = rd16be(blob + pos);
        pos += 2;
        if (pos + dat_len + 6 > len) { free(items); return 0; }
        size_t copy_len = dat_len < sizeof(items[i].dat_name) - 1 ? dat_len : sizeof(items[i].dat_name) - 1;
        memcpy(items[i].dat_name, blob + pos, copy_len);
        items[i].dat_name[copy_len] = '\0';
        pos += dat_len;

        items[i].zip_size = rd32be(blob + pos);
        pos += 4;
        uint16_t zip_len = rd16be(blob + pos);
        pos += 2;
        if (pos + zip_len > len) { free(items); return 0; }
        pos += zip_len;

        uint64_t end = (uint64_t)items[i].offset + (uint64_t)items[i].zip_size;
        if (end > max_end) max_end = end;
    }
    *out_items = items;
    *out_count = count;
    *out_total_pack_bytes = max_end;
    return 1;
}

/* ---- Native Win32 Progress Window ---- */
typedef struct {
    HWND hwnd;
    HFONT font;
    HFONT font_bold;
    char title[128];
    char status[256];
    char detail[256];
    double fraction;
    volatile LONG canceled;
    int headless;
} ProgressUi;

#define IDC_CANCEL_BTN 1001

static LRESULT CALLBACK progress_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ProgressUi *ui = (ProgressUi *)(LONG_PTR)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_CANCEL_BTN && ui) {
            InterlockedExchange(&ui->canceled, 1);
        }
        return 0;
    case WM_CLOSE:
        if (ui) InterlockedExchange(&ui->canceled, 1);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(22, 25, 32));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        if (ui) {
            SetBkMode(hdc, TRANSPARENT);
            SelectObject(hdc, ui->font_bold);
            SetTextColor(hdc, RGB(240, 244, 250));
            RECT r_title = { 24, 18, rc.right - 24, 44 };
            DrawTextA(hdc, ui->title, -1, &r_title, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

            SelectObject(hdc, ui->font);
            SetTextColor(hdc, RGB(195, 204, 218));
            RECT r_stat = { 24, 48, rc.right - 24, 70 };
            DrawTextA(hdc, ui->status, -1, &r_stat, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

            RECT bar_bg = { 24, 80, rc.right - 24, 102 };
            HBRUSH bar_track = CreateSolidBrush(RGB(44, 50, 62));
            FillRect(hdc, &bar_bg, bar_track);
            DeleteObject(bar_track);

            double f = ui->fraction;
            if (f < 0.0) f = 0.0;
            if (f > 1.0) f = 1.0;
            int fill_w = (int)((bar_bg.right - bar_bg.left) * f);
            if (fill_w > 0) {
                RECT bar_fg = { bar_bg.left, bar_bg.top, bar_bg.left + fill_w, bar_bg.bottom };
                HBRUSH bar_fill = CreateSolidBrush(RGB(48, 138, 245));
                FillRect(hdc, &bar_fg, bar_fill);
                DeleteObject(bar_fill);
            }

            SetTextColor(hdc, RGB(150, 162, 182));
            RECT r_det = { 24, 112, rc.right - 130, 136 };
            DrawTextA(hdc, ui->detail, -1, &r_det, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
}

static void ui_init(ProgressUi *ui, int headless) {
    memset(ui, 0, sizeof(*ui));
    ui->headless = headless;
    snprintf(ui->title, sizeof(ui->title), "Asphalt 6: Adrenaline HD — Game Data Installer");
    snprintf(ui->status, sizeof(ui->status), "Initializing...");
    if (headless) return;

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = progress_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "A6InstallerProgressWnd";
    RegisterClassA(&wc);

    int w = 520, h = 195;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    ui->hwnd = CreateWindowExA(
        WS_EX_APPWINDOW, wc.lpszClassName, "Asphalt 6 HD Installer",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        sx, sy, w, h, NULL, NULL, wc.hInstance, NULL);
    if (!ui->hwnd) {
        ui->headless = 1;
        return;
    }
    SetWindowLongPtrA(ui->hwnd, GWLP_USERDATA, (LONG_PTR)ui);
    ui->font = CreateFontA(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    ui->font_bold = CreateFontA(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

    HWND btn = CreateWindowExA(
        0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        w - 116, 114, 86, 28, ui->hwnd, (HMENU)(INT_PTR)IDC_CANCEL_BTN, wc.hInstance, NULL);
    SendMessageA(btn, WM_SETFONT, (WPARAM)ui->font, TRUE);
    UpdateWindow(ui->hwnd);
}

static int ui_pump(ProgressUi *ui) {
    if (!ui->headless && ui->hwnd) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    return !InterlockedCompareExchange(&ui->canceled, 0, 0);
}

static void ui_set(ProgressUi *ui, const char *status, const char *detail, double frac) {
    if (status) snprintf(ui->status, sizeof(ui->status), "%s", status);
    if (detail) snprintf(ui->detail, sizeof(ui->detail), "%s", detail);
    ui->fraction = frac;
    if (!ui->headless && ui->hwnd) {
        InvalidateRect(ui->hwnd, NULL, FALSE);
        ui_pump(ui);
    }
}

static void ui_destroy(ProgressUi *ui) {
    if (ui->hwnd) DestroyWindow(ui->hwnd);
    if (ui->font) DeleteObject(ui->font);
    if (ui->font_bold) DeleteObject(ui->font_bold);
}

/* ---- WinHTTP Helpers ---- */
static int winhttp_crack(const WinHttpApi *wh, const char *url,
                         wchar_t *host, size_t host_cap,
                         wchar_t *path, size_t path_cap,
                         WORD *port, DWORD *flags) {
    wchar_t wurl[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024)) return 0;

    INST_URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;
    uc.dwHostNameLength = (DWORD)host_cap;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = (DWORD)path_cap;

    if (!wh->CrackUrl(wurl, 0, 0, &uc)) return 0;
    *port = uc.nPort;
    *flags = (uc.nScheme == 2) ? INST_WINHTTP_FLAG_SECURE : 0u;
    return 1;
}

static int winhttp_fetch_range_to_mem(const WinHttpApi *wh, HINTERNET hConnect, const wchar_t *wpath,
                                      DWORD flags, uint64_t off, uint32_t size, uint8_t **out_buf) {
    HINTERNET hReq = wh->OpenRequest(hConnect, L"GET", wpath, NULL, NULL, NULL, flags);
    if (!hReq) return 0;

    wchar_t hdr[128];
    swprintf(hdr, 128, L"Range: bytes=%llu-%llu\r\n",
             (unsigned long long)off, (unsigned long long)(off + size - 1u));
    wh->AddRequestHeaders(hReq, hdr, (DWORD)-1, INST_WINHTTP_ADDREQ_FLAG_ADD | INST_WINHTTP_ADDREQ_FLAG_REPLACE);

    int ok = 0;
    uint8_t *buf = NULL;
    if (wh->SendRequest(hReq, NULL, 0, NULL, 0, 0, 0) && wh->ReceiveResponse(hReq, NULL)) {
        DWORD status = 0, st_sz = sizeof(status);
        wh->QueryHeaders(hReq, INST_WINHTTP_QUERY_STATUS_CODE | INST_WINHTTP_QUERY_FLAG_NUMBER,
                         NULL, &status, &st_sz, NULL);
        if (status == 206 || status == 200) {
            buf = (uint8_t *)malloc(size);
            if (buf) {
                uint32_t got = 0;
                while (got < size) {
                    DWORD avail = 0;
                    if (!wh->QueryDataAvailable(hReq, &avail) || avail == 0) break;
                    DWORD to_read = avail;
                    if (got + to_read > size) to_read = size - got;
                    DWORD rd = 0;
                    if (!wh->ReadData(hReq, buf + got, to_read, &rd) || rd == 0) break;
                    got += rd;
                }
                if (got == size) ok = 1;
            }
        }
    }
    wh->CloseHandle(hReq);
    if (!ok) {
        free(buf);
        return 0;
    }
    *out_buf = buf;
    return 1;
}

typedef int (*InstDownloadProgressFn)(void *ctx, uint64_t done, uint64_t total, double mibs);

static int winhttp_download_pack_cb(const WinHttpApi *wh, HINTERNET hConnect, const wchar_t *wpath,
                                    DWORD flags, const char *part_path, uint64_t expected_total,
                                    InstDownloadProgressFn cb, void *ctx) {
    mkdirs_for_file(part_path);
    uint64_t existing = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(part_path, GetFileExInfoStandard, &fad)) {
        existing = ((uint64_t)fad.nFileSizeHigh << 32) | (uint64_t)fad.nFileSizeLow;
        if (existing > expected_total) {
            DeleteFileA(part_path);
            existing = 0;
        }
    }
    if (existing == expected_total) {
        return 1;
    }

    HINTERNET hReq = wh->OpenRequest(hConnect, L"GET", wpath, NULL, NULL, NULL, flags);
    if (!hReq) return 0;

    if (existing > 0) {
        wchar_t hdr[128];
        swprintf(hdr, 128, L"Range: bytes=%llu-\r\n", (unsigned long long)existing);
        wh->AddRequestHeaders(hReq, hdr, (DWORD)-1,
                              INST_WINHTTP_ADDREQ_FLAG_ADD | INST_WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    int ok = 0;
    if (!wh->SendRequest(hReq, NULL, 0, NULL, 0, 0, 0) || !wh->ReceiveResponse(hReq, NULL)) {
        wh->CloseHandle(hReq);
        return 0;
    }

    DWORD status = 0, st_sz = sizeof(status);
    wh->QueryHeaders(hReq, INST_WINHTTP_QUERY_STATUS_CODE | INST_WINHTTP_QUERY_FLAG_NUMBER,
                     NULL, &status, &st_sz, NULL);
    if (status == 416 && existing > 0) {
        wh->CloseHandle(hReq);
        DeleteFileA(part_path);
        return winhttp_download_pack_cb(wh, hConnect, wpath, flags, part_path, expected_total, cb, ctx);
    }
    if (status != 200 && status != 206) {
        wh->CloseHandle(hReq);
        return 0;
    }
    if (status == 200 && existing > 0) existing = 0;

    FILE *out = fopen(part_path, existing > 0 ? "ab" : "wb");
    if (!out) {
        wh->CloseHandle(hReq);
        return 0;
    }

    uint8_t *chunk = (uint8_t *)malloc(262144);
    if (!chunk) {
        fclose(out);
        wh->CloseHandle(hReq);
        return 0;
    }

    uint64_t downloaded = existing;
    DWORD t_start = GetTickCount();
    DWORD t_last = t_start;
    for (;;) {
        if (cb && !cb(ctx, downloaded, expected_total, 0.0)) goto done;
        DWORD avail = 0;
        if (!wh->QueryDataAvailable(hReq, &avail)) goto done;
        if (avail == 0) break;
        DWORD to_read = avail > 262144 ? 262144 : avail;
        DWORD rd = 0;
        if (!wh->ReadData(hReq, chunk, to_read, &rd)) goto done;
        if (rd == 0) break;
        if (fwrite(chunk, 1, rd, out) != rd) goto done;
        downloaded += rd;

        DWORD now = GetTickCount();
        if (cb && (now - t_last >= 120 || downloaded >= expected_total)) {
            double elapsed = (now - t_start) > 10 ? (double)(now - t_start) / 1000.0 : 0.01;
            double mibs = ((double)(downloaded - existing) / (1024.0 * 1024.0)) / elapsed;
            if (!cb(ctx, downloaded, expected_total, mibs)) goto done;
            t_last = now;
        }
    }
    ok = (downloaded == expected_total);

done:
    free(chunk);
    fclose(out);
    wh->CloseHandle(hReq);
    return ok;
}

static int winhttp_ui_progress_cb(void *ctx, uint64_t downloaded, uint64_t expected_total, double mibs) {
    ProgressUi *ui = (ProgressUi *)ctx;
    if (!ui_pump(ui)) return 0;
    if (mibs > 0.0 || downloaded >= expected_total) {
        double frac = expected_total ? (double)downloaded / (double)expected_total : 1.0;
        char st[128], det[128];
        snprintf(st, sizeof(st), "Downloading official Asphalt 6 HD game data from Gameloft...");
        snprintf(det, sizeof(det), "%.1f / %.1f MiB (%.0f%%)  —  %.1f MiB/s",
                 (double)downloaded / (1024.0 * 1024.0),
                 (double)expected_total / (1024.0 * 1024.0),
                 frac * 100.0, mibs);
        ui_set(ui, st, det, frac * 0.85);
    }
    return 1;
}

static int winhttp_download_pack(const WinHttpApi *wh, HINTERNET hConnect, const wchar_t *wpath,
                                 DWORD flags, const char *part_path, uint64_t expected_total,
                                 ProgressUi *ui) {
    return winhttp_download_pack_cb(wh, hConnect, wpath, flags, part_path, expected_total,
                                    winhttp_ui_progress_cb, ui);
}

/* ---- Core Installer Routine ---- */
int installer_find_default_apk(char *out_path, size_t cap) {
    const char *exact = "Asphalt 6 HD 3.7.0.apk";
    DWORD attr = GetFileAttributesA(exact);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        snprintf(out_path, cap, "%s", exact);
        return 1;
    }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("*.apk", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                snprintf(out_path, cap, "%s", fd.cFileName);
                FindClose(h);
                return 1;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return 0;
}

int installer_pick_apk_dialog(char *out_path, size_t cap) {
    HMODULE comdlg = LoadLibraryA("comdlg32.dll");
    if (!comdlg) return 0;
    pfn_GetOpenFileNameA pGetOpen = NULL;
    FARPROC fp = GetProcAddress(comdlg, "GetOpenFileNameA");
    if (!fp) { FreeLibrary(comdlg); return 0; }
    memcpy(&pGetOpen, &fp, sizeof(fp));

    out_path[0] = '\0';
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = "Android APK (*.apk)\0*.apk\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = out_path;
    ofn.nMaxFile = (DWORD)cap;
    ofn.lpstrTitle = "Select Asphalt 6 HD APK";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    int res = pGetOpen(&ofn) ? 1 : 0;
    FreeLibrary(comdlg);
    return res;
}

int installer_read_apk_asset(const char *apk_path, const char *entry_name,
                             uint8_t **out_buf, size_t *out_len) {
    if (!apk_path || !apk_path[0] || !entry_name) return 0;
    ApkZip apk;
    if (!apk_zip_open(apk_path, &apk)) return 0;
    const ApkZipEntry *e = apk_zip_find(&apk, entry_name);
    int ok = e ? apk_zip_read(&apk, e, out_buf, out_len) : 0;
    apk_zip_close(&apk);
    return ok;
}

static int read_whole_file_mem(const char *path, uint8_t **out_buf, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    __int64 sz = _ftelli64(f);
    if (sz < 0 || sz > 256LL * 1024LL * 1024LL) { fclose(f); return 0; }
    _fseeki64(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)(sz ? sz : 1));
    if (!buf) { fclose(f); return 0; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *out_buf = buf;
    *out_len = (size_t)sz;
    return 1;
}

/* ---- A6PAK Trailing Binary Container (self-contained single-EXE payload) ---- */
typedef struct {
    char *name;
    uint64_t offset;
    uint64_t comp_size;
    uint64_t uncomp_size;
    uint32_t crc32;
    uint8_t method;
} A6PakEntry;

typedef struct {
    FILE *fp;
    A6PakEntry *entries;
    uint32_t count;
} A6Pak;

static void wr16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}
static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}
static void wr64le(uint8_t *p, uint64_t v) {
    wr32le(p, (uint32_t)(v & 0xFFFFFFFFu));
    wr32le(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}
static uint64_t rd64le(const uint8_t *p) {
    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

static void a6pak_close(A6Pak *pak) {
    if (pak->fp) fclose(pak->fp);
    for (uint32_t i = 0; i < pak->count; i++) free(pak->entries[i].name);
    free(pak->entries);
    memset(pak, 0, sizeof(*pak));
}

static int a6pak_open(const char *path, A6Pak *pak) {
    memset(pak, 0, sizeof(*pak));
    pak->fp = fopen(path, "rb");
    if (!pak->fp) return 0;
    if (_fseeki64(pak->fp, 0, SEEK_END) != 0) { a6pak_close(pak); return 0; }
    __int64 fsz = _ftelli64(pak->fp);
    if (fsz < 24) { a6pak_close(pak); return 0; }

    uint8_t ftr[24];
    _fseeki64(pak->fp, fsz - 24, SEEK_SET);
    if (fread(ftr, 1, 24, pak->fp) != 24 || memcmp(ftr + 20, "A6PK", 4) != 0) {
        a6pak_close(pak);
        return 0;
    }

    uint64_t toc_off = rd64le(ftr + 0);
    uint64_t toc_sz = rd64le(ftr + 8);
    uint32_t count = rd32le(ftr + 16);
    if (count == 0 || count > 65536u || toc_sz > 64ULL * 1024ULL * 1024ULL ||
        toc_off + toc_sz > (uint64_t)(fsz - 24)) {
        a6pak_close(pak);
        return 0;
    }

    uint8_t *toc = (uint8_t *)malloc((size_t)toc_sz);
    if (!toc) { a6pak_close(pak); return 0; }
    _fseeki64(pak->fp, (__int64)toc_off, SEEK_SET);
    if (fread(toc, 1, (size_t)toc_sz, pak->fp) != (size_t)toc_sz) {
        free(toc);
        a6pak_close(pak);
        return 0;
    }

    pak->entries = (A6PakEntry *)calloc(count, sizeof(A6PakEntry));
    if (!pak->entries) { free(toc); a6pak_close(pak); return 0; }

    size_t pos = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 2u > (size_t)toc_sz) { free(toc); a6pak_close(pak); return 0; }
        uint16_t nlen = rd16le(toc + pos);
        pos += 2u;
        if (pos + nlen + 29u > (size_t)toc_sz) { free(toc); a6pak_close(pak); return 0; }
        A6PakEntry *e = &pak->entries[i];
        e->name = (char *)malloc((size_t)nlen + 1u);
        if (!e->name) { free(toc); a6pak_close(pak); return 0; }
        memcpy(e->name, toc + pos, nlen);
        e->name[nlen] = '\0';
        pos += nlen;
        e->offset = rd64le(toc + pos + 0);
        e->comp_size = rd64le(toc + pos + 8);
        e->uncomp_size = rd64le(toc + pos + 16);
        e->crc32 = rd32le(toc + pos + 24);
        e->method = toc[pos + 28];
        pos += 29u;
        pak->count++;
    }
    free(toc);
    return 1;
}

static const A6PakEntry *a6pak_find(const A6Pak *pak, const char *name) {
    for (uint32_t i = 0; i < pak->count; i++) {
        if (strcmp(pak->entries[i].name, name) == 0) return &pak->entries[i];
    }
    return NULL;
}

static int a6pak_read_entry(const A6Pak *pak, const A6PakEntry *e, uint8_t **out_buf, size_t *out_len) {
    if (!e || e->uncomp_size > 256ULL * 1024ULL * 1024ULL) return 0;
    uint8_t *comp = (uint8_t *)malloc((size_t)(e->comp_size ? e->comp_size : 1));
    uint8_t *out = (uint8_t *)malloc((size_t)(e->uncomp_size ? e->uncomp_size : 1));
    if (!comp || !out) { free(comp); free(out); return 0; }

    _fseeki64(pak->fp, (__int64)e->offset, SEEK_SET);
    if (e->comp_size > 0 && fread(comp, 1, (size_t)e->comp_size, pak->fp) != (size_t)e->comp_size) {
        free(comp);
        free(out);
        return 0;
    }

    int ok = 0;
    if (e->method == 0) {
        if (e->comp_size == e->uncomp_size) {
            memcpy(out, comp, (size_t)e->uncomp_size);
            ok = 1;
        }
    } else if (e->method == 8) {
        size_t got = 0;
        if (inflate_raw(comp, (size_t)e->comp_size, out, (size_t)e->uncomp_size, &got) == 0 &&
            got == (size_t)e->uncomp_size) {
            ok = 1;
        }
    }
    free(comp);
    if (ok && crc32_update(0, out, (size_t)e->uncomp_size) != e->crc32) ok = 0;
    if (!ok) {
        free(out);
        return 0;
    }
    *out_buf = out;
    *out_len = (size_t)e->uncomp_size;
    return 1;
}

static int a6pak_open_self(A6Pak *pak) {
    char exe[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    return a6pak_open(exe, pak);
}

int a6pak_has_self(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    A6Pak pak;
    cached = a6pak_open_self(&pak) ? 1 : 0;
    a6pak_close(&pak);
    return cached;
}

int a6pak_read_self(const char *name, uint8_t **out_buf, size_t *out_len) {
    A6Pak pak;
    if (!a6pak_open_self(&pak)) return 0;
    const A6PakEntry *e = a6pak_find(&pak, name);
    int ok = e ? a6pak_read_entry(&pak, e, out_buf, out_len) : 0;
    a6pak_close(&pak);
    return ok;
}

/* Optional data folder from --data <dir> (empty: %LOCALAPPDATA%\Asphalt6HD as before). */
static char g_data_dir[MAX_PATH];

void installer_set_data_dir(const char *dir) {
    snprintf(g_data_dir, sizeof(g_data_dir), "%s", dir ? dir : "");
}

static int a6pak_local_base(char *out, size_t cap) {
    if (g_data_dir[0]) {
        snprintf(out, cap, "%.240s", g_data_dir);
        return 1;
    }
    char local[MAX_PATH] = {0};
    if (!GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH) || !local[0]) {
        if (!GetTempPathA(MAX_PATH, local)) return 0;
    }
    snprintf(out, cap, "%.240s\\Asphalt6HD", local);
    return 1;
}

/* Extracted ANGLE DLLs and res/raw files: under <data>\cache with --data, else the base. */
static int a6pak_cache_base(char *out, size_t cap) {
    if (g_data_dir[0]) {
        snprintf(out, cap, "%.230s\\cache", g_data_dir);
        return 1;
    }
    return a6pak_local_base(out, cap);
}

int a6pak_local_userdata_dir(char *out_dir, size_t cap) {
    char base[MAX_PATH] = {0};
    if (!a6pak_local_base(base, sizeof(base))) return 0;
    snprintf(out_dir, cap, "%.220s\\userdata", base);
    mkdirs_dir(out_dir);
    return 1;
}

int a6pak_extract_angle_self(char *out_dir, size_t cap) {
    char base[MAX_PATH] = {0};
    if (!a6pak_cache_base(base, sizeof(base))) return 0;
    snprintf(out_dir, cap, "%.220s\\angle", base);
    mkdirs_dir(out_dir);

    A6Pak pak;
    if (!a6pak_open_self(&pak)) return 0;
    static const char *const dlls[] = { "libEGL.dll", "libGLESv2.dll", "d3dcompiler_47.dll" };
    int got_egl = 0, got_gles = 0;
    for (int i = 0; i < 3; i++) {
        const A6PakEntry *e = a6pak_find(&pak, dlls[i]);
        if (!e) continue;
        char dst[MAX_PATH + 64];
        snprintf(dst, sizeof(dst), "%.259s\\%s", out_dir, dlls[i]);
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(dst, GetFileExInfoStandard, &fad) &&
            fad.nFileSizeLow == (DWORD)e->uncomp_size) {
            if (i == 0) got_egl = 1;
            if (i == 1) got_gles = 1;
            continue;
        }
        uint8_t *buf = NULL;
        size_t len = 0;
        if (a6pak_read_entry(&pak, e, &buf, &len)) {
            int wok = write_file_atomic(dst, buf, len);
            free(buf);
            if (wok) {
                if (i == 0) got_egl = 1;
                if (i == 1) got_gles = 1;
            }
        }
    }
    a6pak_close(&pak);
    return got_egl && got_gles;
}

int a6pak_cache_raw_self(const char *base_name, char *out_path, size_t cap) {
    if (!base_name || !base_name[0]) return 0;
    char base[MAX_PATH] = {0};
    if (!a6pak_cache_base(base, sizeof(base))) return 0;
    char key[160];
    snprintf(key, sizeof(key), "res/raw/%.140s", base_name);

    A6Pak pak;
    if (!a6pak_open_self(&pak)) return 0;
    const A6PakEntry *e = a6pak_find(&pak, key);
    if (!e) { a6pak_close(&pak); return 0; }

    snprintf(out_path, cap, "%.200s\\raw\\%.140s", base, base_name);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(out_path, GetFileExInfoStandard, &fad) &&
        fad.nFileSizeLow == (DWORD)e->uncomp_size) {
        a6pak_close(&pak);
        return 1;
    }
    uint8_t *buf = NULL;
    size_t len = 0;
    int ok = a6pak_read_entry(&pak, e, &buf, &len);
    a6pak_close(&pak);
    if (!ok) return 0;
    ok = write_file_atomic(out_path, buf, len);
    free(buf);
    return ok;
}

int installer_read_asset(const char *apk_path, const char *extract_dir,
                         const char *entry_name, uint8_t **out_buf, size_t *out_len) {
    if (entry_name && a6pak_read_self(entry_name, out_buf, out_len)) {
        return 1;
    }
    if (extract_dir && extract_dir[0] && entry_name) {
        char path[MAX_PATH + 256];
        snprintf(path, sizeof(path), "%.259s/%.240s", extract_dir, entry_name);
        if (read_whole_file_mem(path, out_buf, out_len)) return 1;
    }
    return installer_read_apk_asset(apk_path, entry_name, out_buf, out_len);
}

/* Guest data cells the engine fills in (must match runtime/guest_data.c and tools/patch_guest.py). */
static const struct { const char *name; uint32_t addr; } syms_patch[9] = {
    { "_ctype_",            0x1200000u },
    { "_tolower_tab_",      0x1200200u },
    { "_toupper_tab_",      0x1200480u },
    { "SL_IID_ENGINE",      0x1201000u },
    { "SL_IID_PLAY",        0x1201004u },
    { "SL_IID_BUFFERQUEUE", 0x1201008u },
    { "__sF",               0x1202000u },
    { "__stack_chk_guard",  0x1202200u },
    { "__dso_handle",       0x1202204u },
};

/* guest.bin is made only from the APK's libasphalt6.so and this relocator, so a new engine
 * can reuse it while this key stays the same. Bump A6_GUEST_FORMAT when patch_guest_to_mem's
 * output changes; the data cell addresses above are hashed in on their own. */
#define A6_GUEST_FORMAT 1
#define A6_GUEST_KEY_ENTRY "build/guest.ver"

static void guest_build_key(char *out, size_t cap) {
    uint32_t crc = 0;
    for (size_t k = 0; k < 9; k++) {
        uint8_t a[4];
        wr32le(a, syms_patch[k].addr);
        crc = crc32_update(crc, (const uint8_t *)syms_patch[k].name, strlen(syms_patch[k].name) + 1);
        crc = crc32_update(crc, a, 4);
    }
    snprintf(out, cap, "asphalt6-guest %d %08x\n", A6_GUEST_FORMAT, (unsigned)crc);
}

/* Pure C11 ELF32/ARM relocator & guest.bin builder (mirrors recomp/elf_image.py + patch_guest.py) */
static int patch_guest_to_mem(const uint8_t *elf, size_t elf_len,
                              uint8_t **out_mem, size_t *out_len) {
    if (!elf || elf_len < 52 || memcmp(elf, "\x7f" "ELF", 4) != 0 || elf[4] != 1) return 0;
    uint32_t e_phoff = rd32le(elf + 28);
    uint16_t e_phentsize = rd16le(elf + 42);
    uint16_t e_phnum = rd16le(elf + 44);
    if (e_phentsize < 32 || (size_t)e_phoff + (size_t)e_phnum * e_phentsize > elf_len) return 0;

    uint32_t image_end = 0;
    uint32_t dyn_off = 0, dyn_sz = 0;
    for (uint16_t i = 0; i < e_phnum; i++) {
        const uint8_t *ph = elf + e_phoff + i * e_phentsize;
        uint32_t p_type = rd32le(ph + 0);
        uint32_t p_offset = rd32le(ph + 4);
        uint32_t p_vaddr = rd32le(ph + 8);
        uint32_t p_filesz = rd32le(ph + 16);
        uint32_t p_memsz = rd32le(ph + 20);
        if (p_type == 1u) {
            if ((uint64_t)p_offset + p_filesz > elf_len) return 0;
            uint32_t end = p_vaddr + p_memsz;
            if (end > image_end) image_end = end;
        } else if (p_type == 2u) {
            dyn_off = p_offset;
            dyn_sz = p_filesz;
        }
    }
    if (image_end == 0 || image_end > 64u * 1024u * 1024u) return 0;

    uint8_t *mem = (uint8_t *)calloc(1, image_end);
    if (!mem) return 0;
    for (uint16_t i = 0; i < e_phnum; i++) {
        const uint8_t *ph = elf + e_phoff + i * e_phentsize;
        if (rd32le(ph + 0) == 1u) {
            uint32_t p_offset = rd32le(ph + 4);
            uint32_t p_vaddr = rd32le(ph + 8);
            uint32_t p_filesz = rd32le(ph + 16);
            if (p_filesz > 0) memcpy(mem + p_vaddr, elf + p_offset, p_filesz);
        }
    }

    uint32_t dt_hash = 0, dt_strtab = 0, dt_symtab = 0, dt_strsz = 0;
    uint32_t dt_rel = 0, dt_relsz = 0, dt_jmprel = 0, dt_pltrelsz = 0;
    if (dyn_off > 0 && (size_t)dyn_off + dyn_sz <= elf_len) {
        for (uint32_t pos = 0; pos + 8 <= dyn_sz; pos += 8) {
            uint32_t tag = rd32le(elf + dyn_off + pos);
            uint32_t val = rd32le(elf + dyn_off + pos + 4);
            if (tag == 0) break;
            switch (tag) {
            case 4:  if (!dt_hash) dt_hash = val; break;
            case 5:  if (!dt_strtab) dt_strtab = val; break;
            case 6:  if (!dt_symtab) dt_symtab = val; break;
            case 10: if (!dt_strsz) dt_strsz = val; break;
            case 17: if (!dt_rel) dt_rel = val; break;
            case 18: if (!dt_relsz) dt_relsz = val; break;
            case 23: if (!dt_jmprel) dt_jmprel = val; break;
            case 2:  if (!dt_pltrelsz) dt_pltrelsz = val; break;
            default: break;
            }
        }
    }

    uint32_t sym_count = 0;
    if (dt_hash && dt_hash + 8 <= image_end) {
        sym_count = rd32le(mem + dt_hash + 4);
    } else if (dt_symtab && dt_strtab > dt_symtab) {
        sym_count = (dt_strtab - dt_symtab) / 16u;
    }

    uint32_t rel_tables[2][2] = { { dt_rel, dt_relsz }, { dt_jmprel, dt_pltrelsz } };
    for (int t = 0; t < 2; t++) {
        uint32_t raddr = rel_tables[t][0], rsz = rel_tables[t][1];
        if (!raddr || raddr + rsz > image_end) continue;
        for (uint32_t pos = 0; pos + 8 <= rsz; pos += 8) {
            uint32_t r_off = rd32le(mem + raddr + pos);
            uint32_t r_info = rd32le(mem + raddr + pos + 4);
            uint32_t r_type = r_info & 0xFFu;
            uint32_t sym_idx = r_info >> 8;
            if (r_off + 4 > image_end) continue;
            if (r_type == 2u || r_type == 20u || r_type == 21u || r_type == 22u) {
                if (sym_idx == 0 || sym_idx >= sym_count) continue;
                uint32_t soff = dt_symtab + sym_idx * 16u;
                if (soff + 16 > image_end) continue;
                uint32_t st_name = rd32le(mem + soff);
                uint32_t st_value = rd32le(mem + soff + 4);
                uint16_t st_shndx = rd16le(mem + soff + 14);
                uint32_t val = 0;
                int have_val = 0;
                if (st_shndx != 0) {
                    val = st_value;
                    if (r_type == 2u) val += rd32le(mem + r_off);
                    have_val = 1;
                } else if (dt_strtab && st_name < dt_strsz && dt_strtab + st_name < image_end) {
                    const char *sname = (const char *)(mem + dt_strtab + st_name);
                    for (size_t k = 0; k < 9; k++) {
                        if (strcmp(sname, syms_patch[k].name) == 0) {
                            val = syms_patch[k].addr;
                            have_val = 1;
                            break;
                        }
                    }
                }
                if (have_val) {
                    mem[r_off + 0] = (uint8_t)(val & 0xFFu);
                    mem[r_off + 1] = (uint8_t)((val >> 8) & 0xFFu);
                    mem[r_off + 2] = (uint8_t)((val >> 16) & 0xFFu);
                    mem[r_off + 3] = (uint8_t)((val >> 24) & 0xFFu);
                }
            }
        }
    }

    *out_mem = mem;
    *out_len = image_end;
    return 1;
}

typedef struct {
    char *name;
    uint64_t offset;
    uint64_t comp_size;
    uint64_t uncomp_size;
    uint32_t crc32;
    uint8_t method;
} A6OutRec;

static int write_all_fp(FILE *fp, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        size_t w = fwrite(buf + off, 1, len - off, fp);
        if (w == 0) return 0;
        off += w;
    }
    return 1;
}

/* Appends one stored (uncompressed) payload entry at the end of out. */
static int a6out_add_mem(FILE *out, A6OutRec *r, const char *name, const uint8_t *buf, size_t len) {
    r->name = _strdup(name);
    r->offset = (uint64_t)_ftelli64(out);
    r->comp_size = len;
    r->uncomp_size = len;
    r->crc32 = crc32_update(0, buf, len);
    r->method = 0;
    return r->name && write_all_fp(out, buf, len);
}

/* Appends src's entry as it is stored there (no recompression). Stored entries are
 * checked against their CRC on the way, so a damaged source is never copied.
 * 1: copied. 0: src unreadable or damaged. -1: writing failed. */
static int a6out_copy_entry(FILE *out, A6OutRec *r, const A6Pak *src, const A6PakEntry *se) {
    r->name = _strdup(se->name);
    r->offset = (uint64_t)_ftelli64(out);
    r->comp_size = se->comp_size;
    r->uncomp_size = se->uncomp_size;
    r->crc32 = se->crc32;
    r->method = se->method;
    if (!r->name) return -1;
    if (_fseeki64(src->fp, (__int64)se->offset, SEEK_SET) != 0) return 0;

    static uint8_t buf[1u << 20];
    uint64_t left = se->comp_size;
    uint32_t crc = 0;
    while (left > 0) {
        size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
        if (fread(buf, 1, want, src->fp) != want) return 0;
        if (!write_all_fp(out, buf, want)) return -1;
        if (se->method == 0) crc = crc32_update(crc, buf, want);
        left -= want;
    }
    return se->method != 0 || (se->comp_size == se->uncomp_size && crc == se->crc32);
}

/* Writes the TOC and the 24-byte A6PK footer after the entries. */
static int a6out_finish(FILE *out, const A6OutRec *recs, uint32_t nrecs) {
    uint64_t toc_off = (uint64_t)_ftelli64(out);
    for (uint32_t i = 0; i < nrecs; i++) {
        uint16_t nlen = (uint16_t)strlen(recs[i].name);
        uint8_t rec_hdr[2];
        uint8_t meta[29];
        wr16le(rec_hdr, nlen);
        wr64le(meta + 0, recs[i].offset);
        wr64le(meta + 8, recs[i].comp_size);
        wr64le(meta + 16, recs[i].uncomp_size);
        wr32le(meta + 24, recs[i].crc32);
        meta[28] = recs[i].method;
        if (!write_all_fp(out, rec_hdr, 2) ||
            !write_all_fp(out, (const uint8_t *)recs[i].name, nlen) ||
            !write_all_fp(out, meta, 29)) {
            return 0;
        }
    }
    uint64_t toc_sz = (uint64_t)_ftelli64(out) - toc_off;
    uint8_t ftr[24];
    wr64le(ftr + 0, toc_off);
    wr64le(ftr + 8, toc_sz);
    wr32le(ftr + 16, nrecs);
    memcpy(ftr + 20, "A6PK", 4);
    return write_all_fp(out, ftr, 24);
}

int installer_install_standalone_from_setup(
    const char *apk_path, const char *install_dir,
    void (*progress_cb)(void *ctx, int pct, const char *status, const char *log_line),
    void *ctx) {
    if (!apk_path || !install_dir) return 0;

    A6Pak setup_pak;
    if (!a6pak_open_self(&setup_pak)) {
        if (progress_cb) progress_cb(ctx, 0, "Setup payload missing.",
                                     "ERROR: This Asphalt6_Setup.exe does not contain an embedded game payload.");
        return 0;
    }

    const A6PakEntry *pe_exe = a6pak_find(&setup_pak, "asphalt6_win.exe");
    const A6PakEntry *pe_egl = a6pak_find(&setup_pak, "libEGL.dll");
    const A6PakEntry *pe_gles = a6pak_find(&setup_pak, "libGLESv2.dll");
    const A6PakEntry *pe_d3dc = a6pak_find(&setup_pak, "d3dcompiler_47.dll");
    if (!pe_exe || !pe_egl || !pe_gles) {
        a6pak_close(&setup_pak);
        if (progress_cb) progress_cb(ctx, 0, "Incomplete setup payload.",
                                     "ERROR: Embedded engine or ANGLE runtime missing.");
        return 0;
    }

    if (progress_cb) progress_cb(ctx, 10, "Reading Asphalt 6 HD APK...", "Validating APK...");
    ApkZip apk;
    if (!apk_zip_open(apk_path, &apk)) {
        a6pak_close(&setup_pak);
        if (progress_cb) progress_cb(ctx, 0, "Unable to open APK.", "ERROR: Invalid or unreadable APK file.");
        return 0;
    }

    const ApkZipEntry *e_so = apk_zip_find(&apk, "lib/armeabi-v7a/libasphalt6.so");
    if (!e_so) {
        apk_zip_close(&apk);
        a6pak_close(&setup_pak);
        if (progress_cb) progress_cb(ctx, 0, "Unsupported APK.",
                                     "ERROR: APK is missing lib/armeabi-v7a/libasphalt6.so (v3.7.0 required).");
        return 0;
    }

    if (progress_cb) progress_cb(ctx, 25, "Patching Android binary from APK...",
                                 "Applying ELF32 relocations in memory...");
    uint8_t *elf = NULL;
    size_t elf_len = 0;
    uint8_t *guest_mem = NULL;
    size_t guest_len = 0;
    if (!apk_zip_read(&apk, e_so, &elf, &elf_len) ||
        !patch_guest_to_mem(elf, elf_len, &guest_mem, &guest_len)) {
        free(elf);
        apk_zip_close(&apk);
        a6pak_close(&setup_pak);
        if (progress_cb) progress_cb(ctx, 0, "ELF relocation failed.",
                                     "ERROR: Failed to relocate libasphalt6.so.");
        return 0;
    }
    free(elf);

    mkdirs_dir(install_dir);
    char final_exe[1024], tmp_exe[1024];
    snprintf(final_exe, sizeof(final_exe), "%.950s\\asphalt6_win.exe", install_dir);
    snprintf(tmp_exe, sizeof(tmp_exe), "%.950s\\asphalt6_win.exe.tmp", install_dir);

    if (progress_cb) progress_cb(ctx, 45, "Unpacking portable game executable...",
                                 "Writing asphalt6_win.exe...");
    uint8_t *exe_buf = NULL;
    size_t exe_len = 0;
    if (!a6pak_read_entry(&setup_pak, pe_exe, &exe_buf, &exe_len)) {
        free(guest_mem);
        apk_zip_close(&apk);
        a6pak_close(&setup_pak);
        return 0;
    }

    FILE *out = fopen(tmp_exe, "wb");
    if (!out || !write_all_fp(out, exe_buf, exe_len)) {
        if (out) fclose(out);
        DeleteFileA(tmp_exe);
        free(exe_buf);
        free(guest_mem);
        apk_zip_close(&apk);
        a6pak_close(&setup_pak);
        if (progress_cb) progress_cb(ctx, 0, "Write failed.",
                                     "ERROR: Unable to write asphalt6_win.exe to install folder.");
        return 0;
    }
    free(exe_buf);

    if (progress_cb) progress_cb(ctx, 70, "Embedding game manifest, UI & graphics runtime...",
                                 "Bundling self-contained A6PAK payload...");

    A6OutRec *recs = (A6OutRec *)calloc(apk.count + 8u, sizeof(A6OutRec));
    uint32_t nrecs = 0;
    int ok = (recs != NULL);

    /* 1. Append build/guest.bin and the key of the relocator that made it */
    if (ok) ok = a6out_add_mem(out, &recs[nrecs++], "build/guest.bin", guest_mem, guest_len);
    free(guest_mem);
    guest_mem = NULL;
    if (ok) {
        char key[64];
        guest_build_key(key, sizeof(key));
        ok = a6out_add_mem(out, &recs[nrecs++], A6_GUEST_KEY_ENTRY, (const uint8_t *)key, strlen(key));
    }

    /* 2. Append APK raw resources and installer PNGs */
    int got_data_txt = 0, got_pack_info = 0;
    for (uint32_t i = 0; ok && i < apk.count; i++) {
        const ApkZipEntry *e = &apk.entries[i];
        int is_raw = (strncmp(e->name, "res/raw/", 8) == 0);
        int is_gi = (strncmp(e->name, "res/drawable-hdpi-v4/gi_", 24) == 0) ||
                    (strncmp(e->name, "res/drawable/gi_", 16) == 0);
        if (!is_raw && !is_gi) continue;
        if (!e->name[0] || e->name[strlen(e->name) - 1] == '/') continue;

        uint8_t *buf = NULL;
        size_t len = 0;
        if (!apk_zip_read(&apk, e, &buf, &len)) { ok = 0; break; }

        ok = a6out_add_mem(out, &recs[nrecs++], e->name, buf, len);
        free(buf);

        if (strcmp(e->name, "res/raw/data.txt") == 0) got_data_txt = 1;
        if (strncmp(e->name, "res/raw/pack", 12) == 0 && strstr(e->name, ".info")) got_pack_info = 1;
    }
    apk_zip_close(&apk);
    if (!got_data_txt || !got_pack_info) ok = 0;

    /* 3. Copy already-compressed ANGLE DLLs straight from setup_pak */
    const A6PakEntry *dlls[3] = { pe_egl, pe_gles, pe_d3dc };
    for (int d = 0; ok && d < 3; d++) {
        if (dlls[d]) ok = a6out_copy_entry(out, &recs[nrecs++], &setup_pak, dlls[d]) > 0;
    }
    a6pak_close(&setup_pak);

    /* 4. Write TOC + 24-byte A6PK footer */
    if (ok) ok = a6out_finish(out, recs, nrecs);

    fclose(out);
    for (uint32_t i = 0; i < nrecs; i++) free(recs[i].name);
    free(recs);

    if (!ok || !MoveFileExA(tmp_exe, final_exe, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp_exe);
        if (progress_cb) progress_cb(ctx, 0, "Install failed.",
                                     "ERROR: Failed to finalize standalone asphalt6_win.exe.");
        return 0;
    }

    if (progress_cb) progress_cb(ctx, 100, "Installation complete!",
                                 "Installed standalone asphalt6_win.exe cleanly.");
    return 1;
}

/* NULL when the installed payload holds everything taken from the APK, made by this
 * setup's relocator; else why not, in words for the player. */
static const char *a6pak_apk_data_missing(const A6Pak *inst) {
    const A6PakEntry *e_key = a6pak_find(inst, A6_GUEST_KEY_ENTRY);
    int have_info = 0;
    for (uint32_t i = 0; i < inst->count; i++) {
        const char *n = inst->entries[i].name;
        if (strncmp(n, "res/raw/pack", 12) == 0 && strstr(n, ".info")) have_info = 1;
    }
    if (!a6pak_find(inst, "build/guest.bin") || !a6pak_find(inst, "res/raw/data.txt") || !have_info) {
        return "the installed game is missing data from the APK";
    }
    uint8_t *key = NULL;
    size_t key_len = 0;
    char want[64];
    guest_build_key(want, sizeof(want));
    int same = e_key && e_key->uncomp_size < sizeof(want) &&
               a6pak_read_entry(inst, e_key, &key, &key_len) &&
               key_len == strlen(want) && memcmp(key, want, key_len) == 0;
    free(key);
    if (!same) return "this update changes how the game is made from the APK";
    return NULL;
}

/* 1 when <install_dir>\asphalt6_win.exe was built by this same setup: the same engine
 * bytes in front of its payload and the same ANGLE runtime, with the APK data in it. */
int installer_standalone_is_current(const char *install_dir) {
    char exe_path[1024];
    snprintf(exe_path, sizeof(exe_path), "%.950s\\asphalt6_win.exe", install_dir);

    A6Pak setup_pak, inst_pak;
    if (!a6pak_open_self(&setup_pak)) return 0;
    if (!a6pak_open(exe_path, &inst_pak)) {
        a6pak_close(&setup_pak);
        return 0;
    }

    int ok = 1;
    const A6PakEntry *pe_exe = a6pak_find(&setup_pak, "asphalt6_win.exe");
    const A6PakEntry *ie_guest = a6pak_find(&inst_pak, "build/guest.bin");
    if (!pe_exe || !ie_guest || a6pak_apk_data_missing(&inst_pak) ||
        ie_guest->offset != pe_exe->uncomp_size) {
        ok = 0;
    }
    for (uint32_t i = 0; ok && i < setup_pak.count; i++) {
        const A6PakEntry *se = &setup_pak.entries[i];
        if (strcmp(se->name, "asphalt6_win.exe") == 0) continue;
        const A6PakEntry *ie = a6pak_find(&inst_pak, se->name);
        if (!ie || ie->crc32 != se->crc32 || ie->uncomp_size != se->uncomp_size) ok = 0;
    }
    if (ok) {
        uint8_t buf[65536];
        uint64_t left = pe_exe->uncomp_size;
        uint32_t crc = 0;
        _fseeki64(inst_pak.fp, 0, SEEK_SET);
        while (left > 0) {
            size_t want = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
            if (fread(buf, 1, want, inst_pak.fp) != want) { ok = 0; break; }
            crc = crc32_update(crc, buf, want);
            left -= want;
        }
        if (ok && crc != pe_exe->crc32) ok = 0;
    }
    a6pak_close(&inst_pak);
    a6pak_close(&setup_pak);
    return ok;
}

/* Holds <install_dir>\asphalt6_setup.lock open until this process ends. Windows closes
 * (and deletes) it even when the process is killed, so a cancelled run never blocks the
 * next one; that run then clears the half-written exe it left. 0: another run is active. */
int installer_prepare_lock(const char *install_dir) {
    mkdirs_dir(install_dir);
    char path[1024];
    snprintf(path, sizeof(path), "%.950s\\asphalt6_setup.lock", install_dir);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    snprintf(path, sizeof(path), "%.950s\\asphalt6_win.exe.tmp", install_dir);
    DeleteFileA(path);
    return 1;
}

int installer_http_download_file(const char *url, const char *dst_path,
                                 int (*cb)(void *ctx, uint64_t done, uint64_t total, double mibs),
                                 void *ctx) {
    WinHttpApi wh;
    if (!load_winhttp(&wh)) return 0;
    wchar_t host[256] = {0}, wpath[512] = {0};
    WORD port = 80;
    DWORD flags = 0;
    if (!winhttp_crack(&wh, url, host, 256, wpath, 512, &port, &flags)) return 0;

    HINTERNET hSession = wh.Open(L"Asphalt6Setup/1.0", INST_WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return 0;
    wh.SetTimeouts(hSession, 15000, 15000, 30000, 120000);
    HINTERNET hConnect = wh.Connect(hSession, host, port, 0);
    if (!hConnect) { wh.CloseHandle(hSession); return 0; }

    HINTERNET hReq = wh.OpenRequest(hConnect, L"GET", wpath, NULL, NULL, NULL, flags);
    if (!hReq) { wh.CloseHandle(hConnect); wh.CloseHandle(hSession); return 0; }

    int ok = 0;
    if (wh.SendRequest(hReq, NULL, 0, NULL, 0, 0, 0) && wh.ReceiveResponse(hReq, NULL)) {
        DWORD status = 0, st_sz = sizeof(status);
        wh.QueryHeaders(hReq, INST_WINHTTP_QUERY_STATUS_CODE | INST_WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &status, &st_sz, NULL);
        DWORD total_dw = 0, cl_sz = sizeof(total_dw);
        wh.QueryHeaders(hReq, INST_WINHTTP_QUERY_CONTENT_LENGTH | INST_WINHTTP_QUERY_FLAG_NUMBER,
                        NULL, &total_dw, &cl_sz, NULL);
        uint64_t total = (uint64_t)total_dw;
        if (status == 200) {
            mkdirs_for_file(dst_path);
            FILE *out = fopen(dst_path, "wb");
            uint8_t *chunk = (uint8_t *)malloc(262144);
            if (out && chunk) {
                uint64_t done = 0;
                DWORD t0 = GetTickCount(), tl = t0;
                ok = 1;
                for (;;) {
                    if (cb && !cb(ctx, done, total, 0.0)) { ok = 0; break; }
                    DWORD avail = 0;
                    if (!wh.QueryDataAvailable(hReq, &avail)) { ok = 0; break; }
                    if (avail == 0) break;
                    DWORD to_rd = avail > 262144 ? 262144 : avail;
                    DWORD rd = 0;
                    if (!wh.ReadData(hReq, chunk, to_rd, &rd)) { ok = 0; break; }
                    if (rd == 0) break;
                    if (fwrite(chunk, 1, rd, out) != rd) { ok = 0; break; }
                    done += rd;
                    DWORD now = GetTickCount();
                    if (cb && (now - tl >= 120 || avail == 0)) {
                        double el = (now - t0) > 10 ? (double)(now - t0) / 1000.0 : 0.01;
                        if (!cb(ctx, done, total, ((double)done / 1048576.0) / el)) { ok = 0; break; }
                        tl = now;
                    }
                }
            }
            free(chunk);
            if (out) fclose(out);
        }
    }
    wh.CloseHandle(hReq);
    wh.CloseHandle(hConnect);
    wh.CloseHandle(hSession);
    return ok;
}

int installer_unzip_file_to_dir(const char *zip_path, const char *out_dir,
                                int (*cb)(void *ctx, uint32_t done, uint32_t total, const char *name),
                                void *ctx) {
    ApkZip z;
    if (!apk_zip_open(zip_path, &z)) return 0;
    int ok = 1;
    for (uint32_t i = 0; i < z.count; i++) {
        const ApkZipEntry *e = &z.entries[i];
        if (!e->name[0] || e->name[strlen(e->name) - 1] == '/') continue;
        if (strstr(e->name, "..")) continue;
        uint8_t *buf = NULL;
        size_t len = 0;
        if (!apk_zip_read(&z, e, &buf, &len)) { ok = 0; break; }
        char dst[MAX_PATH + 256];
        snprintf(dst, sizeof(dst), "%.220s/%.240s", out_dir, e->name);
        int w_ok = write_file_atomic(dst, buf, len);
        free(buf);
        if (!w_ok) { ok = 0; break; }
        if (cb && ((i & 31u) == 0 || i + 1 == z.count)) {
            if (!cb(ctx, i + 1, z.count, e->name)) { ok = 0; break; }
        }
    }
    apk_zip_close(&z);
    return ok;
}

static void extract_support_from_apk(const ApkZip *apk, const char *extract_dir) {
    for (uint32_t i = 0; i < apk->count; i++) {
        const ApkZipEntry *e = &apk->entries[i];
        int want = (strncmp(e->name, "res/raw/", 8) == 0) ||
                   (strcmp(e->name, "lib/armeabi-v7a/libasphalt6.so") == 0);
        if (!want || !e->name[0] || e->name[strlen(e->name) - 1] == '/') continue;

        char dst[MAX_PATH];
        snprintf(dst, sizeof(dst), "%s/%s", extract_dir, e->name);
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(dst, GetFileExInfoStandard, &fad) &&
            fad.nFileSizeLow == e->uncomp_sz) {
            continue;
        }
        uint8_t *buf = NULL;
        size_t len = 0;
        if (apk_zip_read(apk, e, &buf, &len)) {
            write_file_atomic(dst, buf, len);
            free(buf);
        }
    }
}

static int run_installer(const char *apk_path_in, const char *out_dir,
                         const char *extract_dir, int verify_only, int headless) {
    ProgressUi ui;
    ui_init(&ui, verify_only || headless);
    ui_set(&ui, "Reading APK manifest...", apk_path_in, 0.02);

    ApkZip apk;
    if (!apk_zip_open(apk_path_in, &apk)) {
        ui_destroy(&ui);
        fprintf(stderr, "installer: failed to open APK: %s\n", apk_path_in);
        return 0;
    }

    const ApkZipEntry *e_data = apk_zip_find(&apk, "res/raw/data.txt");
    if (!e_data) {
        apk_zip_close(&apk);
        ui_destroy(&ui);
        fprintf(stderr, "installer: res/raw/data.txt not found inside APK\n");
        return 0;
    }

    uint8_t *data_buf = NULL;
    size_t data_len = 0;
    if (!apk_zip_read(&apk, e_data, &data_buf, &data_len)) {
        apk_zip_close(&apk);
        ui_destroy(&ui);
        return 0;
    }

    char variant[32] = {0}, url[512] = {0};
    int got_url = parse_data_txt_url((const char *)data_buf, data_len, variant, sizeof(variant), url, sizeof(url));
    free(data_buf);
    if (!got_url) {
        apk_zip_close(&apk);
        ui_destroy(&ui);
        fprintf(stderr, "installer: no CDN URL found in res/raw/data.txt\n");
        return 0;
    }

    char info_name[64];
    snprintf(info_name, sizeof(info_name), "res/raw/pack%s.info", variant);
    for (char *p = info_name; *p; p++) *p = (char)tolower((unsigned char)*p);
    const ApkZipEntry *e_info = apk_zip_find(&apk, info_name);
    if (!e_info) e_info = apk_zip_find(&apk, "res/raw/packatc.info");
    if (!e_info) {
        apk_zip_close(&apk);
        ui_destroy(&ui);
        fprintf(stderr, "installer: pack info manifest not found inside APK\n");
        return 0;
    }

    uint8_t *info_buf = NULL;
    size_t info_len = 0;
    if (!apk_zip_read(&apk, e_info, &info_buf, &info_len)) {
        apk_zip_close(&apk);
        ui_destroy(&ui);
        return 0;
    }

    PackItem *items = NULL;
    uint32_t count = 0;
    uint64_t pack_total = 0;
    int parsed = parse_pack_info_blob(info_buf, info_len, &items, &count, &pack_total);
    free(info_buf);
    if (!parsed) {
        apk_zip_close(&apk);
        ui_destroy(&ui);
        fprintf(stderr, "installer: invalid pack info binary manifest\n");
        return 0;
    }

    /* Check existing files in out_dir */
    uint8_t *is_missing = (uint8_t *)calloc(count, 1);
    uint32_t missing_cnt = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (!ui_pump(&ui)) {
            free(is_missing);
            free(items);
            apk_zip_close(&apk);
            ui_destroy(&ui);
            return 0;
        }
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", out_dir, base_filename(items[i].dat_name));
        uint32_t actual_crc = 0;
        uint64_t actual_sz = 0;
        if (!file_crc32_and_size(path, &actual_crc, &actual_sz) ||
            actual_sz != items[i].uncomp_size || actual_crc != items[i].crc32) {
            is_missing[i] = 1;
            missing_cnt++;
        }
        if ((i & 31u) == 0) {
            char det[128];
            snprintf(det, sizeof(det), "Checked %u / %u files...", i + 1, count);
            ui_set(&ui, "Verifying existing game data...", det, 0.05);
        }
    }

    printf("installer: manifest=%u pack_bytes=%llu out=%s missing=%u\n",
           count, (unsigned long long)pack_total, out_dir, missing_cnt);

    if (verify_only) {
        free(is_missing);
        free(items);
        apk_zip_close(&apk);
        ui_destroy(&ui);
        return missing_cnt == 0 ? 1 : 0;
    }

    extract_support_from_apk(&apk, extract_dir);
    apk_zip_close(&apk);

    if (missing_cnt > 0) {
        mkdirs_dir(out_dir);
        WinHttpApi wh;
        if (!load_winhttp(&wh)) {
            free(is_missing);
            free(items);
            ui_destroy(&ui);
            fprintf(stderr, "installer: failed to load winhttp.dll\n");
            return 0;
        }

        wchar_t host[256] = {0}, wpath[512] = {0};
        WORD port = 80;
        DWORD req_flags = 0;
        if (!winhttp_crack(&wh, url, host, 256, wpath, 512, &port, &req_flags)) {
            free(is_missing);
            free(items);
            ui_destroy(&ui);
            return 0;
        }

        HINTERNET hSession = wh.Open(
            L"Dalvik/1.6.0 (Linux; U; Android 4.1.2; R800i Build/4.1.B.0.587)",
            INST_WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
        if (!hSession) {
            free(is_missing);
            free(items);
            ui_destroy(&ui);
            return 0;
        }
        wh.SetTimeouts(hSession, 15000, 15000, 30000, 60000);
        HINTERNET hConnect = wh.Connect(hSession, host, port, 0);
        if (!hConnect) {
            wh.CloseHandle(hSession);
            free(is_missing);
            free(items);
            ui_destroy(&ui);
            return 0;
        }

        int dl_ok = 1;
        if (missing_cnt <= 40) {
            uint32_t done = 0;
            for (uint32_t i = 0; i < count; i++) {
                if (!is_missing[i]) continue;
                if (!ui_pump(&ui)) { dl_ok = 0; break; }
                done++;
                char det[128];
                snprintf(det, sizeof(det), "[%u / %u] %s", done, missing_cnt, items[i].dat_name);
                ui_set(&ui, "Fetching missing files from Gameloft CDN...", det,
                       (double)done / (double)missing_cnt);

                uint8_t *zip_mem = NULL;
                if (!winhttp_fetch_range_to_mem(&wh, hConnect, wpath, req_flags,
                                                items[i].offset, items[i].zip_size, &zip_mem)) {
                    dl_ok = 0;
                    break;
                }
                uint8_t *raw = NULL;
                size_t raw_len = 0;
                int uok = zip_extract_first_from_mem(zip_mem, items[i].zip_size, &raw, &raw_len);
                free(zip_mem);
                if (!uok || raw_len != items[i].uncomp_size ||
                    crc32_update(0, raw, raw_len) != items[i].crc32) {
                    free(raw);
                    dl_ok = 0;
                    break;
                }
                char target[MAX_PATH];
                snprintf(target, sizeof(target), "%s/%s", out_dir, base_filename(items[i].dat_name));
                uok = write_file_atomic(target, raw, raw_len);
                free(raw);
                if (!uok) { dl_ok = 0; break; }
            }
        } else {
            const char *part_path = "build/packatc.zip.part";
            dl_ok = winhttp_download_pack(&wh, hConnect, wpath, req_flags, part_path, pack_total, &ui);
            if (dl_ok) {
                FILE *pf = fopen(part_path, "rb");
                if (!pf) {
                    dl_ok = 0;
                } else {
                    uint32_t done = 0;
                    for (uint32_t i = 0; i < count; i++) {
                        if (!is_missing[i]) continue;
                        if (!ui_pump(&ui)) { dl_ok = 0; break; }
                        uint8_t *zip_mem = (uint8_t *)malloc(items[i].zip_size);
                        if (!zip_mem) { dl_ok = 0; break; }
                        _fseeki64(pf, (__int64)items[i].offset, SEEK_SET);
                        if (fread(zip_mem, 1, items[i].zip_size, pf) != items[i].zip_size) {
                            free(zip_mem);
                            dl_ok = 0;
                            break;
                        }
                        uint8_t *raw = NULL;
                        size_t raw_len = 0;
                        int uok = zip_extract_first_from_mem(zip_mem, items[i].zip_size, &raw, &raw_len);
                        free(zip_mem);
                        if (!uok || raw_len != items[i].uncomp_size ||
                            crc32_update(0, raw, raw_len) != items[i].crc32) {
                            free(raw);
                            dl_ok = 0;
                            break;
                        }
                        char target[MAX_PATH];
                        snprintf(target, sizeof(target), "%s/%s", out_dir, base_filename(items[i].dat_name));
                        uok = write_file_atomic(target, raw, raw_len);
                        free(raw);
                        if (!uok) { dl_ok = 0; break; }

                        done++;
                        if ((done & 15u) == 0 || done == missing_cnt) {
                            char det[128];
                            snprintf(det, sizeof(det), "Unpacked & verified %u / %u files", done, missing_cnt);
                            ui_set(&ui, "Installing & CRC-verifying assets...", det,
                                   0.85 + 0.15 * ((double)done / (double)missing_cnt));
                        }
                    }
                    fclose(pf);
                }
                if (dl_ok) DeleteFileA(part_path);
            }
        }

        wh.CloseHandle(hConnect);
        wh.CloseHandle(hSession);
        if (!dl_ok) {
            free(is_missing);
            free(items);
            ui_destroy(&ui);
            return 0;
        }
    }

    free(is_missing);
    free(items);
    ui_destroy(&ui);
    return 1;
}

struct InstallerSession {
    CRITICAL_SECTION lock;
    volatile LONG cancel_flag;
    HANDLE thread;
    InstallerSnapshot snap;
    char url[512];
    PackItem *items;
    uint32_t count;
    uint8_t *is_missing;
    uint32_t missing_cnt;
    uint64_t pack_total;
    int started;
};

static void sess_set(InstallerSession *s, const InstallerPhase *ph, const uint32_t *checked,
                     const uint32_t *done, const uint64_t *bytes_dl, const double *mibs,
                     const double *frac, const char *status, const char *detail) {
    EnterCriticalSection(&s->lock);
    if (ph) s->snap.phase = *ph;
    if (checked) s->snap.checked_files = *checked;
    if (done) s->snap.done_files = *done;
    if (bytes_dl) s->snap.bytes_downloaded = *bytes_dl;
    if (mibs) s->snap.speed_mibs = *mibs;
    if (frac) s->snap.fraction = *frac;
    if (status) snprintf(s->snap.status, sizeof(s->snap.status), "%.159s", status);
    if (detail) snprintf(s->snap.detail, sizeof(s->snap.detail), "%.159s", detail);
    LeaveCriticalSection(&s->lock);
}

static int sess_dl_progress_cb(void *ctx, uint64_t done, uint64_t total, double mibs) {
    InstallerSession *s = (InstallerSession *)ctx;
    if (InterlockedCompareExchange(&s->cancel_flag, 0, 0) != 0) return 0;
    if (mibs > 0.0 || done >= total) {
        InstallerPhase ph = INST_PHASE_DOWNLOADING;
        double frac = total ? ((double)done / (double)total) * 0.85 : 0.85;
        char det[160];
        snprintf(det, sizeof(det), "%.1f / %.1f MB (%.0f%%)  -  %.1f MB/s",
                 (double)done / (1024.0 * 1024.0),
                 (double)total / (1024.0 * 1024.0),
                 total ? (100.0 * (double)done / (double)total) : 100.0,
                 mibs);
        sess_set(s, &ph, NULL, NULL, &done, &mibs, &frac,
                 "Downloading official game files from Gameloft...", det);
    }
    return 1;
}

static DWORD WINAPI sess_check_thread(LPVOID param) {
    InstallerSession *s = (InstallerSession *)param;
    InstallerPhase ph = INST_PHASE_CHECKING;
    double zero = 0.0;
    sess_set(s, &ph, NULL, NULL, NULL, NULL, &zero,
             "Checking required files...", "Reading game manifest...");

    uint8_t *data_buf = NULL;
    size_t data_len = 0;
    if (!installer_read_asset(s->snap.apk_path, s->snap.extract_dir, "res/raw/data.txt",
                              &data_buf, &data_len)) {
        ph = INST_PHASE_ERROR;
        sess_set(s, &ph, NULL, NULL, NULL, NULL, &zero,
                 "Game manifest missing.", "Missing analysis/apk_extract/res/raw/data.txt");
        return 0;
    }
    char variant[32] = {0};
    int got_url = parse_data_txt_url((const char *)data_buf, data_len,
                                     variant, sizeof(variant), s->url, sizeof(s->url));
    free(data_buf);
    if (!got_url) {
        ph = INST_PHASE_ERROR;
        sess_set(s, &ph, NULL, NULL, NULL, NULL, &zero,
                 "No server URL found.", "Missing pack URL in res/raw/data.txt");
        return 0;
    }

    char info_name[64];
    snprintf(info_name, sizeof(info_name), "res/raw/pack%s.info", variant);
    for (char *p = info_name; *p; p++) *p = (char)tolower((unsigned char)*p);
    uint8_t *info_buf = NULL;
    size_t info_len = 0;
    if (!installer_read_asset(s->snap.apk_path, s->snap.extract_dir, info_name, &info_buf, &info_len) &&
        !installer_read_asset(s->snap.apk_path, s->snap.extract_dir, "res/raw/packatc.info",
                              &info_buf, &info_len)) {
        ph = INST_PHASE_ERROR;
        sess_set(s, &ph, NULL, NULL, NULL, NULL, &zero,
                 "Missing pack manifest.", info_name);
        return 0;
    }

    PackItem *items = NULL;
    uint32_t count = 0;
    uint64_t pack_total = 0;
    int parsed = parse_pack_info_blob(info_buf, info_len, &items, &count, &pack_total);
    free(info_buf);
    if (!parsed) {
        ph = INST_PHASE_ERROR;
        sess_set(s, &ph, NULL, NULL, NULL, NULL, &zero,
                 "Invalid pack manifest.", info_name);
        return 0;
    }

    if (!a6pak_has_self() && s->snap.apk_path[0]) {
        ApkZip apk;
        if (apk_zip_open(s->snap.apk_path, &apk)) {
            extract_support_from_apk(&apk, s->snap.extract_dir);
            apk_zip_close(&apk);
        }
    }

    uint8_t *is_missing = (uint8_t *)calloc(count, 1);
    uint32_t missing_cnt = 0;
    uint64_t missing_bytes = 0;

    EnterCriticalSection(&s->lock);
    free(s->items);
    free(s->is_missing);
    s->items = items;
    s->count = count;
    s->pack_total = pack_total;
    s->is_missing = is_missing;
    s->snap.total_files = count;
    s->snap.pack_total_bytes = pack_total;
    LeaveCriticalSection(&s->lock);

    for (uint32_t i = 0; i < count; i++) {
        if (InterlockedCompareExchange(&s->cancel_flag, 0, 0) != 0) {
            ph = INST_PHASE_CANCELED;
            sess_set(s, &ph, NULL, NULL, NULL, NULL, NULL, "Canceled.", "");
            return 0;
        }
        char path[MAX_PATH + 140];
        snprintf(path, sizeof(path), "%.259s/%.127s", s->snap.out_dir, base_filename(items[i].dat_name));
        uint32_t actual_crc = 0;
        uint64_t actual_sz = 0;
        if (!file_crc32_and_size(path, &actual_crc, &actual_sz) ||
            actual_sz != items[i].uncomp_size || actual_crc != items[i].crc32) {
            is_missing[i] = 1;
            missing_cnt++;
            missing_bytes += (missing_cnt <= 40 ? items[i].zip_size : items[i].uncomp_size);
        }
        if ((i & 31u) == 0 || i + 1 == count) {
            uint32_t chk = i + 1;
            double f = (double)chk / (double)count;
            char det[160];
            snprintf(det, sizeof(det), "Verified %u / %u files...", chk, count);
            sess_set(s, NULL, &chk, NULL, NULL, NULL, &f, "Checking required files...", det);
        }
    }

    if (missing_cnt > 40) missing_bytes = pack_total;

    EnterCriticalSection(&s->lock);
    s->missing_cnt = missing_cnt;
    s->snap.missing_files = missing_cnt;
    s->snap.missing_bytes = missing_bytes;
    LeaveCriticalSection(&s->lock);

    const char *force_env = getenv("ASPHALT6_FORCE_INSTALLER");
    if (missing_cnt == 0 && force_env && strcmp(force_env, "prompt") == 0) {
        EnterCriticalSection(&s->lock);
        s->snap.missing_files = count;
        s->snap.missing_bytes = pack_total;
        s->snap.phase = INST_PHASE_ASK_DOWNLOAD;
        snprintf(s->snap.status, sizeof(s->snap.status), "Download prompt preview");
        snprintf(s->snap.detail, sizeof(s->snap.detail), "%.1f MB",
                 (double)pack_total / (1024.0 * 1024.0));
        LeaveCriticalSection(&s->lock);
        return 0;
    }

    if (missing_cnt == 0) {
        double one = 1.0;
        ph = INST_PHASE_DONE;
        sess_set(s, &ph, &count, &count, &pack_total, NULL, &one,
                 "The game has been successfully downloaded.",
                 "Touch OK and start playing!");
    } else {
        double z = 0.0;
        ph = INST_PHASE_ASK_DOWNLOAD;
        char det[160];
        snprintf(det, sizeof(det), "%.1f MB (%u file%s needed)",
                 (double)missing_bytes / (1024.0 * 1024.0),
                 missing_cnt, missing_cnt == 1 ? "" : "s");
        sess_set(s, &ph, &count, NULL, NULL, NULL, &z,
                 "Additional data required", det);
    }
    return 0;
}

static DWORD WINAPI sess_dl_thread(LPVOID param) {
    InstallerSession *s = (InstallerSession *)param;
    uint32_t count = s->count;
    uint32_t missing_cnt = s->missing_cnt;
    PackItem *items = s->items;
    uint8_t *is_missing = s->is_missing;
    const char *out_dir = s->snap.out_dir;

    mkdirs_dir(out_dir);
    WinHttpApi wh;
    if (!load_winhttp(&wh)) {
        InstallerPhase ph = INST_PHASE_ERROR;
        sess_set(s, &ph, NULL, NULL, NULL, NULL, NULL,
                 "Download failed", "Could not initialize winhttp.dll");
        return 0;
    }

    wchar_t host[256] = {0}, wpath[512] = {0};
    WORD port = 80;
    DWORD req_flags = 0;
    if (!winhttp_crack(&wh, s->url, host, 256, wpath, 512, &port, &req_flags)) {
        InstallerPhase ph = INST_PHASE_ERROR;
        sess_set(s, &ph, NULL, NULL, NULL, NULL, NULL,
                 "Download failed", "Invalid Gameloft server URL");
        return 0;
    }

    HINTERNET hSession = wh.Open(
        L"Dalvik/1.6.0 (Linux; U; Android 4.1.2; R800i Build/4.1.B.0.587)",
        INST_WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        InstallerPhase ph = INST_PHASE_ERROR;
        sess_set(s, &ph, NULL, NULL, NULL, NULL, NULL,
                 "Connection failed", "Unable to open network session");
        return 0;
    }
    wh.SetTimeouts(hSession, 15000, 15000, 30000, 60000);
    HINTERNET hConnect = wh.Connect(hSession, host, port, 0);
    if (!hConnect) {
        wh.CloseHandle(hSession);
        InstallerPhase ph = INST_PHASE_ERROR;
        sess_set(s, &ph, NULL, NULL, NULL, NULL, NULL,
                 "Connection failed", "Could not reach dl.gameloft.com");
        return 0;
    }

    int dl_ok = 1;
    if (missing_cnt <= 40) {
        uint32_t done = 0;
        uint64_t dl_bytes = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!is_missing[i]) continue;
            if (InterlockedCompareExchange(&s->cancel_flag, 0, 0) != 0) { dl_ok = 0; break; }
            double frac = missing_cnt ? (double)done / (double)missing_cnt : 1.0;
            char det[160];
            snprintf(det, sizeof(det), "File %u / %u: %s", done + 1, missing_cnt, items[i].dat_name);
            InstallerPhase ph = INST_PHASE_DOWNLOADING;
            sess_set(s, &ph, NULL, &done, &dl_bytes, NULL, &frac,
                     "Downloading required files...", det);

            uint8_t *zip_mem = NULL;
            if (!winhttp_fetch_range_to_mem(&wh, hConnect, wpath, req_flags,
                                            items[i].offset, items[i].zip_size, &zip_mem)) {
                dl_ok = 0;
                break;
            }
            dl_bytes += items[i].zip_size;
            uint8_t *raw = NULL;
            size_t raw_len = 0;
            int uok = zip_extract_first_from_mem(zip_mem, items[i].zip_size, &raw, &raw_len);
            free(zip_mem);
            if (!uok || raw_len != items[i].uncomp_size ||
                crc32_update(0, raw, raw_len) != items[i].crc32) {
                free(raw);
                dl_ok = 0;
                break;
            }
            char target[MAX_PATH + 140];
            snprintf(target, sizeof(target), "%.259s/%.127s", out_dir, base_filename(items[i].dat_name));
            uok = write_file_atomic(target, raw, raw_len);
            free(raw);
            if (!uok) { dl_ok = 0; break; }
            done++;
        }
    } else {
        char part_path[MAX_PATH + 64];
        if (GetFileAttributesA("build/guest.bin") != INVALID_FILE_ATTRIBUTES) {
            snprintf(part_path, sizeof(part_path), "build/packatc.zip.part");
        } else {
            char tp[MAX_PATH] = {0};
            if (GetTempPathA(MAX_PATH, tp) > 0) {
                snprintf(part_path, sizeof(part_path), "%.250sasphalt6_packatc.zip.part", tp);
            } else {
                snprintf(part_path, sizeof(part_path), "packatc.zip.part");
            }
        }
        dl_ok = winhttp_download_pack_cb(&wh, hConnect, wpath, req_flags, part_path,
                                         s->pack_total, sess_dl_progress_cb, s);
        if (dl_ok) {
            FILE *pf = fopen(part_path, "rb");
            if (!pf) {
                dl_ok = 0;
            } else {
                uint32_t done = 0;
                for (uint32_t i = 0; i < count; i++) {
                    if (!is_missing[i]) continue;
                    if (InterlockedCompareExchange(&s->cancel_flag, 0, 0) != 0) { dl_ok = 0; break; }
                    uint8_t *zip_mem = (uint8_t *)malloc(items[i].zip_size);
                    if (!zip_mem) { dl_ok = 0; break; }
                    _fseeki64(pf, (__int64)items[i].offset, SEEK_SET);
                    if (fread(zip_mem, 1, items[i].zip_size, pf) != items[i].zip_size) {
                        free(zip_mem);
                        dl_ok = 0;
                        break;
                    }
                    uint8_t *raw = NULL;
                    size_t raw_len = 0;
                    int uok = zip_extract_first_from_mem(zip_mem, items[i].zip_size, &raw, &raw_len);
                    free(zip_mem);
                    if (!uok || raw_len != items[i].uncomp_size ||
                        crc32_update(0, raw, raw_len) != items[i].crc32) {
                        free(raw);
                        dl_ok = 0;
                        break;
                    }
                    char target[MAX_PATH + 140];
                    snprintf(target, sizeof(target), "%.259s/%.127s", out_dir, base_filename(items[i].dat_name));
                    uok = write_file_atomic(target, raw, raw_len);
                    free(raw);
                    if (!uok) { dl_ok = 0; break; }
                    done++;
                    if ((done & 7u) == 0 || done == missing_cnt) {
                        InstallerPhase ph = INST_PHASE_VERIFYING;
                        double frac = 0.85 + 0.15 * ((double)done / (double)missing_cnt);
                        char det[160];
                        snprintf(det, sizeof(det), "Unpacked & verified %u / %u files", done, missing_cnt);
                        sess_set(s, &ph, NULL, &done, NULL, NULL, &frac,
                                 "Verifying & installing files...", det);
                    }
                }
                fclose(pf);
                if (dl_ok) DeleteFileA(part_path);
            }
        }
    }

    wh.CloseHandle(hConnect);
    wh.CloseHandle(hSession);

    if (InterlockedCompareExchange(&s->cancel_flag, 0, 0) != 0) {
        InstallerPhase ph = INST_PHASE_CANCELED;
        sess_set(s, &ph, NULL, NULL, NULL, NULL, NULL, "Download canceled", "");
    } else if (!dl_ok) {
        InstallerPhase ph = INST_PHASE_ERROR;
        sess_set(s, &ph, NULL, NULL, NULL, NULL, NULL,
                 "Download failed", "Check your internet connection and retry.");
    } else {
        double one = 1.0;
        InstallerPhase ph = INST_PHASE_DONE;
        sess_set(s, &ph, NULL, &missing_cnt, NULL, NULL, &one,
                 "The game has been successfully downloaded.",
                 "Touch OK and start playing!");
    }
    return 0;
}

InstallerSession *installer_session_open(const char *apk_path, const char *out_dir,
                                         const char *extract_dir) {
    InstallerSession *s = (InstallerSession *)calloc(1, sizeof(InstallerSession));
    if (!s) return NULL;
    InitializeCriticalSection(&s->lock);
    s->snap.phase = INST_PHASE_CHECKING;
    if (apk_path) snprintf(s->snap.apk_path, sizeof(s->snap.apk_path), "%s", apk_path);
    snprintf(s->snap.out_dir, sizeof(s->snap.out_dir), "%s", out_dir ? out_dir : "gamedata");
    snprintf(s->snap.extract_dir, sizeof(s->snap.extract_dir), "%s",
             extract_dir ? extract_dir : "analysis/apk_extract");
    return s;
}

void installer_session_start_check(InstallerSession *s) {
    if (!s) return;
    if (s->thread) {
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread);
        s->thread = NULL;
    }
    InterlockedExchange(&s->cancel_flag, 0);
    s->thread = CreateThread(NULL, 0, sess_check_thread, s, 0, NULL);
}

void installer_session_start_download(InstallerSession *s) {
    if (!s) return;
    if (s->thread) {
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread);
        s->thread = NULL;
    }
    InterlockedExchange(&s->cancel_flag, 0);
    InstallerPhase ph = INST_PHASE_DOWNLOADING;
    double z = 0.0;
    sess_set(s, &ph, NULL, NULL, NULL, NULL, &z,
             "Connecting to Gameloft server...", "");
    s->thread = CreateThread(NULL, 0, sess_dl_thread, s, 0, NULL);
}

void installer_session_snapshot(InstallerSession *s, InstallerSnapshot *out) {
    if (!s || !out) return;
    EnterCriticalSection(&s->lock);
    *out = s->snap;
    LeaveCriticalSection(&s->lock);
}

void installer_session_cancel(InstallerSession *s) {
    if (!s) return;
    InterlockedExchange(&s->cancel_flag, 1);
}

void installer_session_close(InstallerSession *s) {
    if (!s) return;
    InterlockedExchange(&s->cancel_flag, 1);
    if (s->thread) {
        WaitForSingleObject(s->thread, INFINITE);
        CloseHandle(s->thread);
    }
    DeleteCriticalSection(&s->lock);
    free(s->items);
    free(s->is_missing);
    free(s);
}

int installer_ensure_gamedata(void) {
    const char *skip = getenv("ASPHALT6_NO_INSTALLER");
    if (skip && skip[0] == '1') return 1;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    int have_file0 = GetFileAttributesExA("gamedata/file000000.dat", GetFileExInfoStandard, &fad);
    int have_sound = GetFileAttributesExA("gamedata/soundinfo.bin", GetFileExInfoStandard, &fad);
    if (have_file0 && have_sound) {
        return 1;
    }

    char apk_path[MAX_PATH] = {0};
    if (!installer_find_default_apk(apk_path, sizeof(apk_path))) {
        int ans = MessageBoxA(
            NULL,
            "Asphalt 6 HD game assets were not found in gamedata/.\n\n"
            "Would you like to select your Asphalt 6 HD APK so the official game data "
            "(~460 MiB) can be downloaded and verified directly from Gameloft's servers?",
            "Asphalt 6: Adrenaline HD — Game Installer",
            MB_YESNO | MB_ICONQUESTION);
        if (ans != IDYES || !installer_pick_apk_dialog(apk_path, sizeof(apk_path))) {
            return 0;
        }
    }

    if (!run_installer(apk_path, "gamedata", "analysis/apk_extract", 0, 0)) {
        MessageBoxA(NULL,
                    "Failed to download or verify Asphalt 6 HD game data.\n"
                    "Please check your internet connection and APK file.",
                    "Asphalt 6: Adrenaline HD — Installer Error",
                    MB_OK | MB_ICONERROR);
        return 0;
    }
    return 1;
}

#ifdef INSTALLER_STANDALONE
int main(int argc, char **argv) {
    const char *apk = NULL;
    const char *out_dir = "gamedata";
    const char *extract_dir = "analysis/apk_extract";
    int verify_only = 0;
    int headless = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--apk") == 0 && i + 1 < argc) {
            apk = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "--apk-extract") == 0 && i + 1 < argc) {
            extract_dir = argv[++i];
        } else if (strcmp(argv[i], "--verify-only") == 0) {
            verify_only = 1;
        } else if (strcmp(argv[i], "--no-gui") == 0) {
            headless = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: Asphalt6_Installer.exe [--apk <path>] [--out <dir>] "
                   "[--apk-extract <dir>] [--verify-only] [--no-gui]\n");
            return 0;
        }
    }

    char apk_buf[MAX_PATH] = {0};
    if (!apk) {
        if (installer_find_default_apk(apk_buf, sizeof(apk_buf))) {
            apk = apk_buf;
        } else if (!verify_only && !headless && installer_pick_apk_dialog(apk_buf, sizeof(apk_buf))) {
            apk = apk_buf;
        } else {
            fprintf(stderr, "No Asphalt 6 HD APK found. Pass --apk <path>.\n");
            return 1;
        }
    }

    int ok = run_installer(apk, out_dir, extract_dir, verify_only, headless);
    if (!ok) {
        if (!verify_only && !headless) {
            MessageBoxA(NULL, "Game data installation/verification failed.",
                        "Asphalt 6 HD Installer", MB_OK | MB_ICONERROR);
        }
        return 1;
    }
    printf("installer: SUCCESS (%s)\n", out_dir);
    return 0;
}
#endif
