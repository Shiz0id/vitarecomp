/* neon.h — Advanced SIMD on the shared register file.
 *
 * NEON has no registers of its own. D0-D31 are the VFP double registers, and
 * Q0-Q15 are aligned pairs of them (Qn = D2n:D2n+1), so a vector instruction
 * and a scalar float instruction can name the same bits. Keeping one backing
 * store (vfp.h's 64 words) makes that aliasing exact rather than something to
 * synchronise.
 *
 * A D register is handled as one uint64_t. Lane-wise arithmetic is done on the
 * whole word at once with the carry chain cut at every lane boundary, which is
 * both shorter than a lane loop and impossible to get the lane order wrong in.
 */

#ifndef VITARECOMP_NEON_H
#define VITARECOMP_NEON_H

#include "vfp.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint64_t neon_getd(int d) {
    return ((uint64_t)vfp_s[(d * 2 + 1) & 63].u << 32) | vfp_s[(d * 2) & 63].u;
}

static inline void neon_setd(int d, uint64_t v) {
    vfp_s[(d * 2) & 63].u     = (uint32_t)v;
    vfp_s[(d * 2 + 1) & 63].u = (uint32_t)(v >> 32);
}

/* The top bit of every lane, for lanes of 8 << size bits. */
static inline uint64_t neon_lane_tops(int size) {
    static const uint64_t tops[4] = {
        0x8080808080808080ull, 0x8000800080008000ull,
        0x8000000080000000ull, 0x8000000000000000ull,
    };
    return tops[size & 3];
}

/* Lane-wise add: add everything below each lane's top bit, where no carry
 * can leave the lane, then put the top bits back as a carry-less sum. */
static inline uint64_t neon_add(uint64_t a, uint64_t b, int size) {
    uint64_t h = neon_lane_tops(size);
    return ((a & ~h) + (b & ~h)) ^ ((a ^ b) & h);
}

/* Lane-wise subtract, the same trick from the other side: set each lane's top
 * bit in the minuend so no borrow can leave the lane, then correct it. */
static inline uint64_t neon_sub(uint64_t a, uint64_t b, int size) {
    uint64_t h = neon_lane_tops(size);
    return ((a | h) - (b & ~h)) ^ ((a ^ ~b) & h);
}

/* Every lane all ones where it is zero, else all zeros. Adding each lane's
 * low bits to all-ones-but-the-top carries into the top bit exactly when the
 * low bits are not all zero, and cannot carry out of the lane; OR in the
 * lane's own top bit and that bit says "non-zero". */
static inline uint64_t neon_lanes_zero(uint64_t x, int size) {
    uint64_t h = neon_lane_tops(size);
    uint64_t nz = (((x & ~h) + ~h) | x) & h;
    uint64_t z = (~nz & h) >> ((8u << size) - 1);      /* bit 0 of each zero lane */
    uint64_t lane = size == 3 ? ~0ull : (1ull << (8u << size)) - 1;
    return z * lane;
}

static inline uint64_t neon_ceq(uint64_t a, uint64_t b, int size) {
    return neon_lanes_zero(a ^ b, size);
}

static inline uint64_t neon_tst(uint64_t a, uint64_t b, int size) {
    return ~neon_lanes_zero(a & b, size);
}

/* VPADD: the pairs of n, then the pairs of m, each sum in one lane. */
static inline uint64_t neon_padd(uint64_t n, uint64_t m, int size) {
    int w = 8 << size, lanes = 64 / w;
    uint64_t mask = (w == 64) ? ~0ull : ((1ull << w) - 1), r = 0;
    for (int i = 0; i < lanes; i++) {
        uint64_t src = (i < lanes / 2) ? n : m;
        int j = (i % (lanes / 2)) * 2;
        uint64_t s = ((src >> (j * w)) + (src >> ((j + 1) * w))) & mask;
        r |= s << (i * w);
    }
    return r;
}

/* VMOVN: the low half of each lane of the 128-bit lo:hi, into one D.
 * `size` is the RESULT lane size. */
static inline uint64_t neon_movn(uint64_t lo, uint64_t hi, int size) {
    int w = 8 << size, per = 32 / w;                 /* result lanes per source D */
    uint64_t mask = (1ull << w) - 1, r = 0;
    for (int i = 0; i < per; i++) {
        r |= ((lo >> (i * 2 * w)) & mask) << (i * w);
        r |= ((hi >> (i * 2 * w)) & mask) << ((i + per) * w);
    }
    return r;
}

/* VDUP: one value in every lane. */
static inline uint64_t neon_dup(uint32_t v, int size) {
    switch (size) {
        case 0:  return (uint64_t)(v & 0xFF) * 0x0101010101010101ull;
        case 1:  return (uint64_t)(v & 0xFFFF) * 0x0001000100010001ull;
        default: return (uint64_t)v * 0x0000000100000001ull;
    }
}

static inline uint64_t neon_set_lane(uint64_t d, uint32_t v, int size, int idx) {
    int w = 8 << size, sh = idx * w;
    uint64_t mask = (w == 32 ? 0xFFFFFFFFull : ((1ull << w) - 1)) << sh;
    return (d & ~mask) | (((uint64_t)v << sh) & mask);
}

static inline uint32_t neon_get_lane(uint64_t d, int size, int idx, int is_unsigned) {
    int w = 8 << size;
    uint32_t v = (uint32_t)(d >> (idx * w));
    if (size == 0) return is_unsigned ? (v & 0xFF) : (uint32_t)(int32_t)(int8_t)v;
    if (size == 1) return is_unsigned ? (v & 0xFFFF) : (uint32_t)(int32_t)(int16_t)v;
    return v;
}

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_NEON_H */
