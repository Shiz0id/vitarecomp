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
#include <string.h>

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

/* --- Advanced SIMD single precision ----------------------------------------
 *
 * Advanced SIMD ignores FPSCR and always runs with the "standard FPSCR value":
 * round to nearest, flush-to-zero, default NaN. The host's float does none of
 * that by default (it keeps denormals, and its NaNs carry the operands' sign
 * and payload), so each lane is put through these rules explicitly:
 *
 *   an input denormal is read as a zero of the same sign;
 *   a NaN result is the default NaN, 0x7FC00000, whatever the operands;
 *   a denormal result is a zero of the same sign.
 *
 * Multiply-accumulate is NOT fused. The product is rounded to float (and
 * flushed) and only then added, which is what the architecture defines. A
 * fused fmaf() gives a different answer whenever the exact product carries
 * bits a float cannot hold, and nothing downstream can tell which was done.
 * The product is stored as bits through a volatile so the compiler cannot
 * contract the multiply and add into one operation, whatever -ffp-contract
 * says. */

static inline uint32_t neon_lo32(uint64_t x) { return (uint32_t)x; }
static inline uint32_t neon_hi32(uint64_t x) { return (uint32_t)(x >> 32); }
static inline uint64_t neon_pack32(uint32_t lo, uint32_t hi) {
    return ((uint64_t)hi << 32) | lo;
}

static inline float neon_f32_in(uint32_t bits) {
    if ((bits & 0x7F800000u) == 0) bits &= 0x80000000u;   /* denormal or zero */
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static inline uint32_t neon_f32_out(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    if ((u & 0x7F800000u) == 0x7F800000u && (u & 0x007FFFFFu)) return 0x7FC00000u;
    if ((u & 0x7F800000u) == 0) return u & 0x80000000u;   /* denormal: signed zero */
    return u;
}

static inline uint32_t neon_f32_add(uint32_t a, uint32_t b) {
    return neon_f32_out(neon_f32_in(a) + neon_f32_in(b));
}

static inline uint32_t neon_f32_sub(uint32_t a, uint32_t b) {
    return neon_f32_out(neon_f32_in(a) - neon_f32_in(b));
}

static inline uint32_t neon_f32_mul(uint32_t a, uint32_t b) {
    return neon_f32_out(neon_f32_in(a) * neon_f32_in(b));
}

/* d + n*m (sub == 0) or d - n*m (sub != 0), with the product rounded and
 * flushed on its own before the add. */
static inline uint32_t neon_f32_mla(uint32_t d, uint32_t n, uint32_t m, int sub) {
    volatile uint32_t p = neon_f32_mul(n, m);            /* no fusing across this */
    float pf = neon_f32_in(p), df = neon_f32_in(d);
    return neon_f32_out(sub ? df - pf : df + pf);
}

static inline uint64_t neon_fadd(uint64_t a, uint64_t b) {
    return neon_pack32(neon_f32_add(neon_lo32(a), neon_lo32(b)),
                       neon_f32_add(neon_hi32(a), neon_hi32(b)));
}

static inline uint64_t neon_fsub(uint64_t a, uint64_t b) {
    return neon_pack32(neon_f32_sub(neon_lo32(a), neon_lo32(b)),
                       neon_f32_sub(neon_hi32(a), neon_hi32(b)));
}

static inline uint64_t neon_fmul(uint64_t a, uint64_t b) {
    return neon_pack32(neon_f32_mul(neon_lo32(a), neon_lo32(b)),
                       neon_f32_mul(neon_hi32(a), neon_hi32(b)));
}

/* VMLA and VMLS: d is the accumulator, n and m the multiplicands. */
static inline uint64_t neon_fmla(uint64_t d, uint64_t n, uint64_t m) {
    return neon_pack32(neon_f32_mla(neon_lo32(d), neon_lo32(n), neon_lo32(m), 0),
                       neon_f32_mla(neon_hi32(d), neon_hi32(n), neon_hi32(m), 0));
}

static inline uint64_t neon_fmls(uint64_t d, uint64_t n, uint64_t m) {
    return neon_pack32(neon_f32_mla(neon_lo32(d), neon_lo32(n), neon_lo32(m), 1),
                       neon_f32_mla(neon_hi32(d), neon_hi32(n), neon_hi32(m), 1));
}

/* By scalar: `s` is the 32-bit lane, the same in every lane of the product. */
static inline uint64_t neon_fmul_s(uint64_t n, uint32_t s) {
    return neon_fmul(n, neon_dup(s, 2));
}

static inline uint64_t neon_fmla_s(uint64_t d, uint64_t n, uint32_t s) {
    return neon_fmla(d, n, neon_dup(s, 2));
}

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_NEON_H */
