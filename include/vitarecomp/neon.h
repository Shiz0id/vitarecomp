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

static inline uint64_t neon_fmls_s(uint64_t d, uint64_t n, uint32_t s) {
    return neon_fmls(d, n, neon_dup(s, 2));
}

/* VMAX and VMIN on one lane. The inputs are flushed first, so a denormal
 * competes as its zero. The zeros are ordered by sign: max(+0, -0) is +0 and
 * min(+0, -0) is -0, whatever the operand order. Plain float max would give
 * whichever it met first, since +0 == -0 is true. */
static inline uint32_t neon_f32_minmax(uint32_t a, uint32_t b, int is_max) {
    float fa = neon_f32_in(a), fb = neon_f32_in(b);
    if (fa != fa || fb != fb) return 0x7FC00000u;          /* NaN: default NaN */
    int pick_a;
    if (fa == fb)                                           /* equal, or a zero pair */
        pick_a = is_max ? !(a >> 31) : (a >> 31);
    else
        pick_a = is_max ? (fa > fb) : (fa < fb);
    return neon_f32_out(pick_a ? fa : fb);
}

/* The compares on one lane: rel 0 is ==, 1 is >=, 2 is >. A NaN on either side
 * makes every one of them false, which the C operators already do. */
static inline uint32_t neon_f32_cmp(uint32_t a, uint32_t b, int rel) {
    float fa = neon_f32_in(a), fb = neon_f32_in(b);
    int t = rel == 0 ? (fa == fb) : rel == 1 ? (fa >= fb) : (fa > fb);
    return t ? 0xFFFFFFFFu : 0;
}

static inline uint64_t neon_fmax(uint64_t a, uint64_t b) {
    return neon_pack32(neon_f32_minmax(neon_lo32(a), neon_lo32(b), 1),
                       neon_f32_minmax(neon_hi32(a), neon_hi32(b), 1));
}

static inline uint64_t neon_fmin(uint64_t a, uint64_t b) {
    return neon_pack32(neon_f32_minmax(neon_lo32(a), neon_lo32(b), 0),
                       neon_f32_minmax(neon_hi32(a), neon_hi32(b), 0));
}

static inline uint64_t neon_fceq(uint64_t a, uint64_t b) {
    return neon_pack32(neon_f32_cmp(neon_lo32(a), neon_lo32(b), 0),
                       neon_f32_cmp(neon_hi32(a), neon_hi32(b), 0));
}

static inline uint64_t neon_fcge(uint64_t a, uint64_t b) {
    return neon_pack32(neon_f32_cmp(neon_lo32(a), neon_lo32(b), 1),
                       neon_f32_cmp(neon_hi32(a), neon_hi32(b), 1));
}

static inline uint64_t neon_fcgt(uint64_t a, uint64_t b) {
    return neon_pack32(neon_f32_cmp(neon_lo32(a), neon_lo32(b), 2),
                       neon_f32_cmp(neon_hi32(a), neon_hi32(b), 2));
}

/* VABS and VNEG clear or flip the sign bit of every lane and nothing else.
 * A denormal stays denormal and a NaN keeps its payload: the architecture
 * says so, and it differs from the flushing arithmetic above. */
static inline uint64_t neon_fabs(uint64_t x) { return x & 0x7FFFFFFF7FFFFFFFull; }
static inline uint64_t neon_fneg(uint64_t x) { return x ^ 0x8000000080000000ull; }

/* F32 to S32 or U32, round toward zero. The float is checked against the
 * integer range before the cast: a cast of an out-of-range float is undefined
 * in C, and the architecture wants saturation there, not a host-dependent
 * value. NaN gives 0, and a flushed denormal gives 0 like any zero. */
static inline uint32_t neon_f32_to_int(uint32_t bits, int is_unsigned) {
    float f = neon_f32_in(bits);
    if (f != f) return 0;                                   /* NaN */
    if (is_unsigned) {
        if (!(f > 0.0f)) return 0;                          /* negatives and -0 */
        if (f >= 4294967296.0f) return 0xFFFFFFFFu;
        return (uint32_t)f;
    }
    if (f >= 2147483648.0f) return 0x7FFFFFFFu;
    if (f < -2147483648.0f) return 0x80000000u;             /* -2^31 itself is in range */
    return (uint32_t)(int32_t)f;
}

/* S32 or U32 to F32. The host's int-to-float conversion rounds to nearest,
 * ties to even, which is the rounding the architecture uses here. Every result
 * is zero or normal, so neon_f32_out has nothing to flush. */
static inline uint32_t neon_i32_to_f32(uint32_t v, int is_unsigned) {
    float f = is_unsigned ? (float)v : (float)(int32_t)v;
    return neon_f32_out(f);
}

static inline uint64_t neon_f2i(uint64_t x, int is_unsigned) {
    return neon_pack32(neon_f32_to_int(neon_lo32(x), is_unsigned),
                       neon_f32_to_int(neon_hi32(x), is_unsigned));
}

static inline uint64_t neon_i2f(uint64_t x, int is_unsigned) {
    return neon_pack32(neon_i32_to_f32(neon_lo32(x), is_unsigned),
                       neon_i32_to_f32(neon_hi32(x), is_unsigned));
}

/* --- Advanced SIMD integer multiply, saturation and shifts ------------------
 *
 * Lanes are 8 << size bits. The helpers read each lane as an unsigned value
 * and write it back masked, so no bit crosses a lane boundary. A signed lane
 * is sign-extended by hand (neon_sext): a right shift of a negative int is
 * implementation-defined in C, and the shifts below never rely on it.
 * The lane adds and subtracts of the same-length group (neon_add, neon_sub)
 * are reused for the accumulating forms. */

static inline uint64_t neon_lmask(int size) {
    return size == 3 ? ~0ull : (1ull << (8u << size)) - 1;
}

static inline uint64_t neon_lane_of(uint64_t x, int size, int i) {
    return (x >> (i * (8u << size))) & neon_lmask(size);
}

/* A lane of w bits (w < 64) as a signed value. The sign is taken by
 * subtraction: a lane v with its top bit set is -(2^w - v), and 2^w - v is
 * computed as sign - (v below the sign bit), all in unsigned values. */
static inline int64_t neon_sext(uint64_t v, int w) {
    uint64_t sign = 1ull << (w - 1);
    if (v & sign) return -(int64_t)(sign - (v & (sign - 1)));
    return (int64_t)v;
}

/* VMUL, VMLA and VMLS. The product of two lanes is formed in a uint64_t, so
 * 16- and 32-bit lanes never promote to a signed int and overflow. Only the
 * low lane bits are kept, which is what wrapping means here. The low bits of a
 * product do not depend on signedness, so one helper serves all of them. */
static inline uint64_t neon_mul(uint64_t n, uint64_t m, int size) {
    int w = 8 << size;
    uint64_t r = 0;
    for (int i = 0; i < 64 / w; i++)
        r |= ((neon_lane_of(n, size, i) * neon_lane_of(m, size, i)) & neon_lmask(size)) << (i * w);
    return r;
}

static inline uint64_t neon_mla(uint64_t d, uint64_t n, uint64_t m, int size) {
    return neon_add(d, neon_mul(n, m, size), size);
}

static inline uint64_t neon_mls(uint64_t d, uint64_t n, uint64_t m, int size) {
    return neon_sub(d, neon_mul(n, m, size), size);
}

/* VQADD and VQSUB: each lane's sum or difference, clamped to the lane type.
 * Signed clamps to [-2^(w-1), 2^(w-1)-1], unsigned to [0, 2^w-1]. For lanes
 * under 64 bits the arithmetic is exact in int64 or uint64, so the clamp is a
 * plain comparison. A 64-bit lane has no wider type to sum in, so overflow is
 * read from the operands' sign bits (signed) or from a wrap (unsigned), and the
 * clamp value is picked by the sign of the first operand. Signed overflow is
 * never computed in C, so none of this is undefined.
 *
 * QC, the cumulative saturation flag, is not modelled: FPSCR has no QC bit in
 * vfp.h and nothing in the runtime reads one. */
static inline uint64_t neon_qop(uint64_t n, uint64_t m, int size, int is_unsigned, int sub) {
    int w = 8 << size;
    uint64_t mask = neon_lmask(size), r = 0;
    for (int i = 0; i < 64 / w; i++) {
        uint64_t a = neon_lane_of(n, size, i), b = neon_lane_of(m, size, i), x;
        if (size == 3) {
            uint64_t s = sub ? a - b : a + b;
            if (is_unsigned)
                x = sub ? (a < b ? 0 : s) : (s < a ? ~0ull : s);
            else if (sub ? (((a ^ b) & (a ^ s)) >> 63) : (((~(a ^ b)) & (a ^ s)) >> 63))
                x = (a >> 63) ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull;
            else
                x = s;
        } else if (is_unsigned) {
            uint64_t s = sub ? (a < b ? 0 : a - b) : a + b;
            x = s > mask ? mask : s;
        } else {
            int64_t sa = neon_sext(a, w), sb = neon_sext(b, w);
            int64_t s = sub ? sa - sb : sa + sb;
            int64_t hi = (1ll << (w - 1)) - 1, lo = -hi - 1;
            x = (uint64_t)(s > hi ? hi : s < lo ? lo : s) & mask;
        }
        r |= x << (i * w);
    }
    return r;
}

/* VSHL by immediate: each lane shifted left by amt, 0 <= amt < lane width.
 * Bits shifted out of a lane are masked off rather than carried upward. */
static inline uint64_t neon_shl(uint64_t n, int amt, int size) {
    int w = 8 << size;
    uint64_t r = 0;
    for (int i = 0; i < 64 / w; i++)
        r |= ((neon_lane_of(n, size, i) << amt) & neon_lmask(size)) << (i * w);
    return r;
}

/* VSHR by immediate, 1 <= amt <= lane width. The full width is a legal
 * amount and is not a C shift (which would be undefined): unsigned lanes give
 * zero, and signed lanes give all sign bits. A negative signed lane is shifted
 * as ~((~v) >> amt), which is the arithmetic shift written in unsigned
 * arithmetic, so it rounds toward minus infinity as VSHR does. */
static inline uint64_t neon_shr(uint64_t n, int amt, int size, int is_unsigned) {
    int w = 8 << size;
    uint64_t mask = neon_lmask(size), sign = 1ull << (w - 1), r = 0;
    for (int i = 0; i < 64 / w; i++) {
        uint64_t v = neon_lane_of(n, size, i), x;
        int neg = !is_unsigned && (v & sign);
        if (amt >= w)     x = neg ? mask : 0;
        else if (neg)     x = mask & ~(((~v) & mask) >> amt);
        else              x = v >> amt;
        r |= x << (i * w);
    }
    return r;
}

/* VSRA: each lane of m shifted right as VSHR does, then added to d, wrapping. */
static inline uint64_t neon_sra(uint64_t d, uint64_t m, int amt, int size, int is_unsigned) {
    return neon_add(d, neon_shr(m, amt, size, is_unsigned), size);
}

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_NEON_H */
