#ifndef ASPHALT6_MEM_H
#define ASPHALT6_MEM_H

#include <stdint.h>
#include <string.h>

/*
 * Guest memory access.
 *
 * The host is little-endian, and every access goes through memcpy so the generated
 * code has no undefined behaviour and tolerates unaligned guest addresses.
 */
#define GUEST_MEM_SIZE 0x20000000u

extern uint32_t guest_mem_size;
void guest_fault(uint32_t addr, int write);

static inline int guest_ok(uint32_t addr, uint32_t n) {
    return __builtin_expect(addr <= (GUEST_MEM_SIZE - n), 1);
}

static inline uint8_t ld1(const uint8_t *m, uint32_t a) {
    uint8_t v;
    if (!guest_ok(a, 1)) { guest_fault(a, 0); return 0; }
    memcpy(&v, m + a, 1);
    return v;
}

static inline uint16_t ld2(const uint8_t *m, uint32_t a) {
    uint16_t v;
    if (!guest_ok(a, 2)) { guest_fault(a, 0); return 0; }
    memcpy(&v, m + a, 2);
    return v;
}

static inline uint32_t ld4(const uint8_t *m, uint32_t a) {
    uint32_t v;
    if (!guest_ok(a, 4)) { guest_fault(a, 0); return 0; }
    memcpy(&v, m + a, 4);
    return v;
}

static inline uint64_t ld8(const uint8_t *m, uint32_t a) {
    uint64_t v;
    if (!guest_ok(a, 8)) { guest_fault(a, 0); return 0; }
    memcpy(&v, m + a, 8);
    return v;
}

static inline void st1(uint8_t *m, uint32_t a, uint32_t v) {
    uint8_t b = (uint8_t)v;
    if (!guest_ok(a, 1)) { guest_fault(a, 1); return; }
    memcpy(m + a, &b, 1);
}

static inline void st2(uint8_t *m, uint32_t a, uint32_t v) {
    uint16_t h = (uint16_t)v;
    if (!guest_ok(a, 2)) { guest_fault(a, 1); return; }
    memcpy(m + a, &h, 2);
}

static inline void st4(uint8_t *m, uint32_t a, uint32_t v) {
    if (!guest_ok(a, 4)) { guest_fault(a, 1); return; }
    memcpy(m + a, &v, 4);
}

static inline void st8(uint8_t *m, uint32_t a, uint64_t v) {
    if (!guest_ok(a, 8)) { guest_fault(a, 1); return; }
    memcpy(m + a, &v, 8);
}

#endif /* ASPHALT6_MEM_H */
