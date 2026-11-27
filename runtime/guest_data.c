/* Guest static data symbols and libc tables patched into guest RAM at startup. */
#include <stdint.h>
#include <ctype.h>
#include <string.h>

/* Must match tools/patch_guest.py */
#define ADDR_CTYPE_       0x1200000u
#define ADDR_TOLOWER_TAB_ 0x1200200u
#define ADDR_TOUPPER_TAB_ 0x1200480u
#define ADDR_CTYPE_DATA   0x1200800u
#define ADDR_TOLOWER_DATA 0x1200a00u
#define ADDR_TOUPPER_DATA 0x1200d00u
#define ADDR_SL_ENGINE    0x1201000u
#define ADDR_SL_PLAY      0x1201004u
#define ADDR_SL_BQUEUE    0x1201008u
#define ADDR_SF           0x1202000u
#define ADDR_STACK_CHK    0x1202200u
#define ADDR_DSO_HANDLE   0x1202204u
#define ADDR_ERRNO        0x1202400u

/* Fake JNI / JavaVM blocks in reserved low runtime page (0x1210000..0x121ffff). */
#define ADDR_JNI_BASE     0x1210000u

uint32_t g_errno = ADDR_ERRNO;

static void w32_raw(uint8_t *mem, uint32_t addr, uint32_t val) {
    memcpy(mem + addr, &val, 4);
}

static void w16_raw(uint8_t *mem, uint32_t addr, uint16_t val) {
    memcpy(mem + addr, &val, 2);
}

void guest_init_data(uint8_t *mem, uint32_t mem_size) {
    if (!mem || mem_size < 0x1300000u) return;

    /*
     * In Android Bionic, _ctype_, _tolower_tab_, and _toupper_tab_ are POINTER variables
     * (const char *_ctype_; const short *_tolower_tab_; const short *_toupper_tab_;).
     * Guest code loads the pointer from ADDR_* then indexes [1 + c].
     */
    w32_raw(mem, ADDR_CTYPE_, ADDR_CTYPE_DATA);
    w32_raw(mem, ADDR_TOLOWER_TAB_, ADDR_TOLOWER_DATA);
    w32_raw(mem, ADDR_TOUPPER_TAB_, ADDR_TOUPPER_DATA);

    /*
     * Bionic ctype masks (indexed [1 + c], c in -1..255):
     *   _U=0x01, _L=0x02, _N=0x04, _S=0x08, _P=0x10, _C=0x20, _X=0x40, _B=0x80
     */
    mem[ADDR_CTYPE_DATA] = 0;
    for (int ch = 0; ch < 256; ch++) {
        uint8_t m = 0;
        if (ch >= 'A' && ch <= 'Z') m |= 0x01u;
        if (ch >= 'a' && ch <= 'z') m |= 0x02u;
        if (ch >= '0' && ch <= '9') m |= 0x04u;
        if (isspace(ch)) m |= 0x08u;
        if (ispunct(ch)) m |= 0x10u;
        if (iscntrl(ch)) m |= 0x20u;
        if (isxdigit(ch)) m |= 0x40u;
        if (ch == ' ') m |= 0x80u;
        mem[ADDR_CTYPE_DATA + 1u + (uint32_t)ch] = m;
    }

    /* Bionic 16-bit tolower / toupper tables: [0] = -1, [1 + c] = mapped char */
    w16_raw(mem, ADDR_TOLOWER_DATA, 0xFFFFu);
    w16_raw(mem, ADDR_TOUPPER_DATA, 0xFFFFu);
    for (int ch = 0; ch < 256; ch++) {
        w16_raw(mem, ADDR_TOLOWER_DATA + 2u * (1u + (uint32_t)ch), (uint16_t)tolower(ch));
        w16_raw(mem, ADDR_TOUPPER_DATA + 2u * (1u + (uint32_t)ch), (uint16_t)toupper(ch));
    }

    /* OpenSL ES interface ID pointers */
    w32_raw(mem, ADDR_SL_ENGINE, 0x534C0001u);
    w32_raw(mem, ADDR_SL_PLAY,   0x534C0002u);
    w32_raw(mem, ADDR_SL_BQUEUE, 0x534C0003u);

    /* __sF[3]: stdin/stdout/stderr placeholders (84 bytes each) */
    memset(mem + ADDR_SF, 0, 84u * 3u);
    w32_raw(mem, ADDR_SF + 0x12u, 0u);  /* fd 0 */
    w32_raw(mem, ADDR_SF + 84u + 0x12u, 1u);
    w32_raw(mem, ADDR_SF + 168u + 0x12u, 2u);

    w32_raw(mem, ADDR_STACK_CHK, 0x0A1B2C3Du);
    w32_raw(mem, ADDR_DSO_HANDLE, ADDR_DSO_HANDLE);
    w32_raw(mem, ADDR_ERRNO, 0u);
    (void)ADDR_JNI_BASE;
}
