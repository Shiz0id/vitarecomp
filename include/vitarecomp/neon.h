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

/* Integer VABS, wrapping: a negative lane is negated in place and abs(-2^(w-1))
 * stays -2^(w-1), as the architecture has it. m is all ones in the lanes whose
 * sign bit is set, else zero, so (x ^ m) - m negates exactly those lanes. The
 * subtraction is neon_sub, which cannot borrow across a lane. This sits above
 * neon_lmask and neon_lane_of, so it uses only neon_sub and neon_lane_tops and
 * writes the lane mask out, as neon_lanes_zero does. */
static inline uint64_t neon_abs(uint64_t x, int size) {
    uint64_t h = neon_lane_tops(size);
    uint64_t lane = size == 3 ? ~0ull : (1ull << (8u << size)) - 1;
    uint64_t m = ((x & h) >> ((8u << size) - 1)) * lane;
    return neon_sub(x ^ m, m, size);
}

/* --- Advanced SIMD two registers, miscellaneous, A = 00 ---------------------
 *
 * The pairwise adds, the per-lane counts, and the saturating abs and neg.
 * Each takes one D word. These sit above neon_lmask, neon_lane_of and
 * neon_sext, so they use only neon_add (for the accumulate) and plain shifts,
 * and work each lane as an unsigned value with its sign taken by hand. */

/* The leading zeros of a w-bit value, w for zero. A loop, not __builtin_clz,
 * which is undefined at zero. */
static inline int neon_clz_lane(uint64_t v, int w) {
    int n = 0;
    for (int b = w - 1; b >= 0 && !((v >> b) & 1); b--) n++;
    return n;
}

/* VCLZ: the leading zeros of each lane. */
static inline uint64_t neon_clz(uint64_t x, int size) {
    int w = 8 << size;
    uint64_t mask = size == 3 ? ~0ull : (1ull << w) - 1, r = 0;
    for (int i = 0; i < 64 / w; i++)
        r |= (uint64_t)neon_clz_lane((x >> (i * w)) & mask, w) << (i * w);
    return r;
}

/* VCLS: the redundant sign bits of each lane, that is the bits below the top
 * one that equal it. A negative lane is complemented first, which turns its
 * run of ones into zeros, so the count is the leading zeros of that value
 * less one: the sign bit, now clear, is one of those zeros and is not counted.
 * The top bit of the complement is always clear, so the count is never
 * negative. Zero and all ones both give w - 1. */
static inline uint64_t neon_cls(uint64_t x, int size) {
    int w = 8 << size;
    uint64_t mask = size == 3 ? ~0ull : (1ull << w) - 1, r = 0;
    for (int i = 0; i < 64 / w; i++) {
        uint64_t v = (x >> (i * w)) & mask;
        if ((v >> (w - 1)) & 1) v = ~v & mask;
        r |= (uint64_t)(neon_clz_lane(v, w) - 1) << (i * w);
    }
    return r;
}

/* VCNT: the set bits of each byte, written in that byte. */
static inline uint64_t neon_cnt(uint64_t x) {
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t b = (x >> (i * 8)) & 0xFF;
        int c = 0;
        for (int k = 0; k < 8; k++) c += (int)((b >> k) & 1);
        r |= (uint64_t)c << (i * 8);
    }
    return r;
}

/* VQABS (neg = 0) and VQNEG (neg = 1): the absolute value or the negation of
 * each lane, saturated. The minimum 2^(w-1) negated is one past the top, so it
 * gives the maximum for both. Any other negative value negates as 0 - v in w
 * bits, which is exact there. A positive value under VQABS is left alone.
 * QC is not set, as with neon_qop. */
static inline uint64_t neon_qabs(uint64_t x, int size, int neg) {
    int w = 8 << size;
    uint64_t mask = size == 3 ? ~0ull : (1ull << w) - 1;
    uint64_t sign = 1ull << (w - 1), top = sign - 1, r = 0;
    for (int i = 0; i < 64 / w; i++) {
        uint64_t v = (x >> (i * w)) & mask, y;
        if (v == sign) y = top;
        else if (neg || (v & sign)) y = (0 - v) & mask;
        else y = v;
        r |= y << (i * w);
    }
    return r;
}

/* VPADDL: lane i of the result, which is 2w bits wide, is the sum of source
 * lanes 2i and 2i+1 of x. Each is read signed or unsigned (is_unsigned). The
 * sum is formed in int64_t, which holds two 32-bit lanes exactly, and cut to
 * the result lane. size is the SOURCE lane size, as in the mnemonic. The
 * result of one D word is 64 / (2w) lanes, so size 2 gives one 64-bit lane. */
static inline uint64_t neon_paddl(uint64_t x, int size, int is_unsigned) {
    int w = 8 << size, rw = 2 * w;
    uint64_t mask = (1ull << w) - 1, sign = 1ull << (w - 1);
    uint64_t rmask = rw == 64 ? ~0ull : (1ull << rw) - 1, r = 0;
    for (int i = 0; i < 64 / rw; i++) {
        int64_t s = 0;
        for (int k = 0; k < 2; k++) {
            uint64_t v = (x >> ((2 * i + k) * w)) & mask;
            s += is_unsigned ? (int64_t)v : (int64_t)(v ^ sign) - (int64_t)sign;
        }
        r |= ((uint64_t)s & rmask) << (i * rw);
    }
    return r;
}

/* VPADAL: VPADDL of x, added to the result lanes of d, wrapping in each one.
 * The add is neon_add at the result size, which is size + 1. */
static inline uint64_t neon_padal(uint64_t d, uint64_t x, int size, int is_unsigned) {
    return neon_add(d, neon_paddl(x, size, is_unsigned), size + 1);
}

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

/* VRECPE and VRSQRTE: the ARM manual's 8-bit estimates. Both are defined by
 * a C routine on doubles; these are the same values in integers, so they do
 * not depend on the host's float rounding or on libm.
 *
 * The input x is the 9 top bits of a value scaled into [0.25, 1), x / 512.
 * For the reciprocal x is 256..511 and the manual takes 1 / ((x + 0.5) /
 * 512), rounded to 1/256: 256 * 1024 / (2x + 1), plus a half, floored. The
 * result s is 257..511 and stands for s / 256. */
static inline uint32_t neon_recip_est9(uint32_t x) {
    return (524288u / (2 * x + 1) + 1) / 2;
}

/* The reciprocal square root of x / 512, x 128..511, also as s / 256. The
 * manual keeps 9 bits below 0.5 and only 8 at or above it, so x >= 256 drops
 * its low bit: q = x / 2 and the value is (q + 0.5) / 256. s is the nearest
 * integer to 2^14 / sqrt(D) with D = 2x + 1 (below 0.5) or 2^13.5 / sqrt(D)
 * with D = 2q + 1, so s - 0.5 <= that root, squared: (2s - 1)^2 D <= M, M
 * 2^28 or 2^27. (2s - 1)^2 D is odd and M a power of two, so they are never
 * equal, and s is the largest integer with (2s - 1)^2 <= M / D, which is
 * (isqrt(M / D) + 1) / 2. */
static inline uint32_t neon_rsqrt_est9(uint32_t x) {
    uint32_t v = x < 256 ? (1u << 28) / (2 * x + 1) : (1u << 27) / ((x & ~1u) + 1);
    uint32_t r = 0;                                 /* isqrt(v), v < 2^20 */
    for (uint32_t bit = 1u << 20; bit; bit >>= 2) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
    }
    return (r + 1) / 2;
}

/* VRECPE.U32 and VRSQRTE.U32. The lane is a fixed-point value, x / 2^32. A
 * lane below 0.5 (bit 31 clear) for the reciprocal, or below 0.25 for the
 * root, gives all ones; otherwise the estimate s / 256 is returned with its
 * leading 1 at bit 31, so the result is s << 23. */
static inline uint32_t neon_urecpe_lane(uint32_t x) {
    if (!(x >> 31)) return 0xFFFFFFFFu;
    return neon_recip_est9(x >> 23) << 23;
}

static inline uint32_t neon_ursqrte_lane(uint32_t x) {
    if (!(x >> 30)) return 0xFFFFFFFFu;
    return neon_rsqrt_est9(x >> 23) << 23;
}

/* VRECPE.F32 under the standard FPSCR. Any NaN gives the default NaN, an
 * infinity a zero of its sign, and a zero (a denormal is flushed to one) an
 * infinity of its sign. With exponent e the input is 1.f * 2^(e - 127) and
 * the estimate of 1 / 0.1f is between 1 and 2, so the result exponent is 253
 * - e. From e = 253 (|x| >= 2^126) that would be denormal, and it is flushed
 * to a zero of the input's sign. The 8 fraction bits of s are the top of the
 * result's fraction. */
static inline uint32_t neon_frecpe_lane(uint32_t x) {
    uint32_t sign = x & 0x80000000u, e = (x >> 23) & 0xFF, frac = x & 0x7FFFFF;
    if (e == 0xFF) return frac ? 0x7FC00000u : sign;
    if (e == 0) return sign | 0x7F800000u;
    if (e >= 253) return sign;
    return sign | (253 - e) << 23 | (neon_recip_est9(256 | frac >> 15) & 0xFF) << 15;
}

/* VRSQRTE.F32 under the standard FPSCR. NaN first, then a zero (or flushed
 * denormal) gives an infinity of its sign, any other negative the default NaN
 * and +infinity +0. An odd exponent scales the input to 0.01f, in [0.25, 0.5),
 * and an even one to 0.1f, so the exponent left over is even and halves:
 * the result exponent is (380 - e) / 2. Every result is normal. */
static inline uint32_t neon_frsqrte_lane(uint32_t x) {
    uint32_t e = (x >> 23) & 0xFF, frac = x & 0x7FFFFF;
    if (e == 0xFF && frac) return 0x7FC00000u;
    if (e == 0) return (x & 0x80000000u) | 0x7F800000u;
    if (x >> 31) return 0x7FC00000u;
    if (e == 0xFF) return 0;
    uint32_t q = (e & 1) ? 128 | frac >> 16 : 256 | frac >> 15;
    return (380 - e) / 2 << 23 | (neon_rsqrt_est9(q) & 0xFF) << 15;
}

static inline uint64_t neon_urecpe(uint64_t x) {
    return neon_pack32(neon_urecpe_lane(neon_lo32(x)), neon_urecpe_lane(neon_hi32(x)));
}

static inline uint64_t neon_ursqrte(uint64_t x) {
    return neon_pack32(neon_ursqrte_lane(neon_lo32(x)), neon_ursqrte_lane(neon_hi32(x)));
}

static inline uint64_t neon_frecpe(uint64_t x) {
    return neon_pack32(neon_frecpe_lane(neon_lo32(x)), neon_frecpe_lane(neon_hi32(x)));
}

static inline uint64_t neon_frsqrte(uint64_t x) {
    return neon_pack32(neon_frsqrte_lane(neon_lo32(x)), neon_frsqrte_lane(neon_hi32(x)));
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

/* VCGT (ge = 0) and VCGE (ge = 1): a lane is all ones where n > m (or n >= m)
 * and zero elsewhere. A signed lane is compared as its sign-extended value,
 * since comparing the raw bits would put 0x80 above 0x7F. Sizes 0-2 only:
 * neon_sext needs a lane under 64 bits, and decode refuses size 3 here. */
static inline uint64_t neon_cmp(uint64_t n, uint64_t m, int size, int is_unsigned, int ge) {
    int w = 8 << size;
    uint64_t mask = neon_lmask(size), r = 0;
    for (int i = 0; i < 64 / w; i++) {
        uint64_t a = neon_lane_of(n, size, i), b = neon_lane_of(m, size, i);
        int t;
        if (is_unsigned) t = ge ? a >= b : a > b;
        else {
            int64_t sa = neon_sext(a, w), sb = neon_sext(b, w);
            t = ge ? sa >= sb : sa > sb;
        }
        r |= (t ? mask : 0) << (i * w);
    }
    return r;
}

/* VMAX (min = 0) and VMIN (min = 1), lane by lane. Each result lane is one of
 * the two input lanes copied as it stood, so no value is rebuilt here. */
static inline uint64_t neon_minmax(uint64_t n, uint64_t m, int size, int is_unsigned, int min) {
    int w = 8 << size;
    uint64_t r = 0;
    for (int i = 0; i < 64 / w; i++) {
        uint64_t a = neon_lane_of(n, size, i), b = neon_lane_of(m, size, i);
        int a_greater = is_unsigned ? a > b : neon_sext(a, w) > neon_sext(b, w);
        int take_a = min ? !a_greater : a_greater;
        r |= (take_a ? a : b) << (i * w);
    }
    return r;
}

/* VABD: |n - m| per lane. The difference is formed exactly and then truncated
 * to the lane. For signed lanes |sa - sb| is at most 2^w - 1, so the result
 * fits the lane as an unsigned value (|-128 - 127| = 255 for s8), and the
 * int64 difference cannot overflow for lanes up to 32 bits. */
static inline uint64_t neon_abd(uint64_t n, uint64_t m, int size, int is_unsigned) {
    int w = 8 << size;
    uint64_t mask = neon_lmask(size), r = 0;
    for (int i = 0; i < 64 / w; i++) {
        uint64_t a = neon_lane_of(n, size, i), b = neon_lane_of(m, size, i), x;
        if (is_unsigned) x = a > b ? a - b : b - a;
        else {
            int64_t sa = neon_sext(a, w), sb = neon_sext(b, w);
            x = (uint64_t)(sa > sb ? sa - sb : sb - sa);
        }
        r |= (x & mask) << (i * w);
    }
    return r;
}

/* One lane of VSHL, VQSHL, VRSHL and VQRSHL by register. x is the lane bits
 * and sh the amount, the low byte of the shift lane read signed, so -128..127.
 * sh >= 0 shifts left: the exact result, truncated to the lane unless sat, and
 * then clamped. sh < 0 shifts right by R = -sh, rounding when round is set.
 *
 * A left shift fits the lane exactly when |x| <= edge >> sh, where edge is the
 * lane's limit on x's side: mask for unsigned, 2^(w-1) for a negative x and
 * 2^(w-1) - 1 for a positive one. Clamping goes to that same edge. A negative
 * x has the magnitude 0 - x within the lane. Shifts by w or more never reach
 * C's undefined range: a left shift of a nonzero x by w or more saturates, and
 * a right shift by w or more is only the sign bits.
 *
 * A right shift is the floor of x / 2^R. For a negative x it is ~((~x) >> R),
 * as in neon_shr. With rounding, floor(x / 2^R + 1/2) is that floor plus bit
 * R-1 of x, the bit the shift drops, and the sum is taken mod 2^w. R = w gives
 * the top bit alone for unsigned and 0 for signed, and R > w rounds to 0. */
static inline uint64_t neon_shl_lane(uint64_t x, int sh, int w, int is_unsigned, int round, int sat) {
    uint64_t mask = w == 64 ? ~0ull : (1ull << w) - 1;
    uint64_t sbit = 1ull << (w - 1);
    int neg = !is_unsigned && (x & sbit) != 0;
    uint64_t edge = is_unsigned ? mask : neg ? sbit : sbit - 1;
    if (sh >= 0) {
        if (x == 0) return 0;
        if (sh >= w) return sat ? edge : 0;
        uint64_t y = (x << sh) & mask;
        if (!sat) return y;
        uint64_t mag = neg ? (0 - x) & mask : x;
        return mag <= (edge >> sh) ? y : edge;
    }
    int R = -sh;
    if (round && R > w) return 0;
    uint64_t fl;
    if (R >= w)   fl = neg ? mask : 0;
    else if (neg) fl = mask & ~(((~x) & mask) >> R);
    else          fl = x >> R;
    if (!round) return fl;
    uint64_t bit = (x >> (R - 1)) & 1;               /* R - 1 < 64: R <= w */
    return (fl + bit) & mask;
}

/* VSHL, VQSHL, VRSHL and VQRSHL by register, lane by lane. m is the value
 * (Vm) and n the amounts (Vn), in the order of the assembly, so vshl.s8 Dd, Dm,
 * Dn is neon_shl_reg(Dm, Dn, ...). Each amount is the low byte of its lane,
 * sign-extended, and the higher bits of a wider lane are ignored. round picks
 * the rounding right shifts and sat the saturating left shifts. Size 3 is valid
 * here: the one 64-bit lane is read whole. */
static inline uint64_t neon_shl_reg(uint64_t m, uint64_t n, int size, int is_unsigned, int round, int sat) {
    int w = 8 << size;
    uint64_t r = 0;
    for (int i = 0; i < 64 / w; i++) {
        int sh = (int)neon_sext(neon_lane_of(n, size, i) & 0xFF, 8);
        r |= neon_shl_lane(neon_lane_of(m, size, i), sh, w, is_unsigned, round, sat) << (i * w);
    }
    return r;
}

/* VHADD (kind 0), VRHADD (kind 1) and VHSUB (kind 2), lane by lane. The exact
 * sum or difference of two lanes needs one bit more than the lane, so it is
 * formed in int64 (lanes here are at most 32 bits, so nothing overflows), then
 * halved and truncated to the lane. VRHADD adds 1 before halving. The halving
 * is a floor: C's >> of a negative int64 is implementation-defined, so it is
 * taken by division, and -3 halves to -2, not -1. Size 3 is UNDEFINED and
 * decode refuses it before this is called. */
static inline uint64_t neon_halve(uint64_t n, uint64_t m, int size, int is_unsigned, int kind) {
    int w = 8 << size;
    uint64_t mask = neon_lmask(size), r = 0;
    for (int i = 0; i < 64 / w; i++) {
        uint64_t a = neon_lane_of(n, size, i), b = neon_lane_of(m, size, i);
        int64_t sa = is_unsigned ? (int64_t)a : neon_sext(a, w);
        int64_t sb = is_unsigned ? (int64_t)b : neon_sext(b, w);
        int64_t t = kind == 2 ? sa - sb : sa + sb + (kind == 1);
        int64_t h = t >= 0 ? t / 2 : -((-t + 1) / 2);    /* floor(t / 2) */
        r |= ((uint64_t)h & mask) << (i * w);
    }
    return r;
}

/* VPMAX (min = 0) and VPMIN (min = 1). As in neon_padd, the pairs of n fill
 * the low half of the result and the pairs of m the high half, and each pair
 * is reduced to its larger or smaller lane. The inputs are values, so the
 * caller's store comes after both are read: vpmax d0, d0, d1 is safe. D only:
 * a Q form is UNDEFINED and decode refuses it. */
static inline uint64_t neon_pminmax(uint64_t n, uint64_t m, int size, int is_unsigned, int min) {
    int w = 8 << size, lanes = 64 / w;
    uint64_t r = 0;
    for (int i = 0; i < lanes; i++) {
        uint64_t src = (i < lanes / 2) ? n : m;
        int j = (i % (lanes / 2)) * 2;
        uint64_t a = neon_lane_of(src, size, j), b = neon_lane_of(src, size, j + 1);
        int a_greater = is_unsigned ? a > b : neon_sext(a, w) > neon_sext(b, w);
        int take_a = min ? !a_greater : a_greater;
        r |= (take_a ? a : b) << (i * w);
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

/* VRSHR by immediate, 1 <= amt <= lane width: each lane is the rounding right
 * shift of neon_shl_lane, the negative-amount half of VRSHL by register. It
 * is floor(x / 2^amt + 1/2) exactly, with the lane's signedness. amt = w gives
 * the top bit for unsigned lanes and 0 for signed ones. */
static inline uint64_t neon_rshr(uint64_t n, int amt, int size, int is_unsigned) {
    int w = 8 << size;
    uint64_t r = 0;
    for (int i = 0; i < 64 / w; i++)
        r |= neon_shl_lane(neon_lane_of(n, size, i), -amt, w, is_unsigned, 1, 0) << (i * w);
    return r;
}

/* VRSRA: each lane of m rounding-shifted as VRSHR does, added to d, wrapping. */
static inline uint64_t neon_rsra(uint64_t d, uint64_t m, int amt, int size, int is_unsigned) {
    return neon_add(d, neon_rshr(m, amt, size, is_unsigned), size);
}

/* VSRI: each lane keeps its top amt bits from d, and the lane of m shifted
 * right logically by amt fills the bits below them. The keep mask is the lane
 * mask shifted right, complemented: ~(mask >> amt) selects the top amt bits.
 * amt = w (the full lane) inserts nothing, so d is returned unchanged. The
 * branch is needed: at 64 bits mask >> 64 is a shift by the width of the
 * type, which is undefined C. */
static inline uint64_t neon_sri(uint64_t d, uint64_t m, int amt, int size) {
    int w = 8 << size;
    uint64_t mask = neon_lmask(size), r = 0;
    for (int i = 0; i < 64 / w; i++) {
        uint64_t dl = neon_lane_of(d, size, i), x;
        if (amt >= w) x = dl;
        else x = (dl & ~(mask >> amt) & mask)
               | neon_shl_lane(neon_lane_of(m, size, i), -amt, w, 1, 0, 0);
        r |= x << (i * w);
    }
    return r;
}

/* VSLI: each lane keeps its bottom amt bits from d, and the lane of m shifted
 * left by amt, within the lane, fills the bits above them. The mask of bits at
 * and above amt is the lane mask shifted left, so amt 0 clears all of d's
 * bits and copies m. amt is 0 to w-1, so no shift reaches the lane width. */
static inline uint64_t neon_sli(uint64_t d, uint64_t m, int amt, int size) {
    int w = 8 << size;
    uint64_t mask = neon_lmask(size), r = 0;
    for (int i = 0; i < 64 / w; i++) {
        uint64_t dl = neon_lane_of(d, size, i);
        uint64_t x = (dl & ~((mask << amt) & mask) & mask)
                   | neon_shl_lane(neon_lane_of(m, size, i), amt, w, 1, 0, 0);
        r |= x << (i * w);
    }
    return r;
}

/* VQSHL by immediate, 0 <= amt < w: the saturating left shift of neon_shl_lane.
 * is_unsigned picks the signedness of both the source and the result, and a
 * result past the lane's range clamps to its limit (see neon_shl_lane). */
static inline uint64_t neon_qshl_imm(uint64_t m, int amt, int size, int is_unsigned) {
    int w = 8 << size;
    uint64_t r = 0;
    for (int i = 0; i < 64 / w; i++)
        r |= neon_shl_lane(neon_lane_of(m, size, i), amt, w, is_unsigned, 0, 1) << (i * w);
    return r;
}

/* VQSHLU: the source lane is signed, the result unsigned. A negative lane gives
 * 0, since no unsigned value is below zero. A positive lane is an unsigned
 * value already, so it is shifted as one, saturating to the lane's all-ones. */
static inline uint64_t neon_qshlu_imm(uint64_t m, int amt, int size) {
    int w = 8 << size;
    uint64_t r = 0;
    for (int i = 0; i < 64 / w; i++) {
        uint64_t x = neon_lane_of(m, size, i);
        uint64_t y = ((x >> (w - 1)) & 1) ? 0 : neon_shl_lane(x, amt, w, 1, 0, 1);
        r |= y << (i * w);
    }
    return r;
}

/* F32 to S32 or U32 with fbits fraction bits: x * 2^fbits, rounded toward zero
 * and saturated to the 32-bit range. A float has 24 significant bits and fbits
 * is at most 32, so the product is exact in double, and truncation is the only
 * rounding. NaN gives 0. Infinities saturate like any out-of-range value. */
static inline uint32_t neon_f32_to_fixed(uint32_t bits, int fbits, int is_unsigned) {
    float f = neon_f32_in(bits);
    if (f != f) return 0;                                   /* NaN */
    double x = (double)f * (double)(1ull << fbits);
    if (is_unsigned) {
        if (x < 0.0) return 0;                              /* negatives and -0 */
        if (x >= 4294967296.0) return 0xFFFFFFFFu;
        return (uint32_t)x;
    }
    if (x >= 2147483648.0) return 0x7FFFFFFFu;
    if (x < -2147483648.0) return 0x80000000u;              /* -2^31 itself is in range */
    return (uint32_t)(int32_t)x;
}

/* S32 or U32 with fbits fraction bits to F32: v / 2^fbits. The integer and the
 * power of two are both exact in double, and the division by a power of two is
 * exact too, so the one rounding is the conversion to float, which rounds to
 * nearest, ties to even. The smallest result is 2^-32, a normal float, so the
 * flush in neon_f32_out has nothing to do. */
static inline uint32_t neon_fixed_to_f32(uint32_t v, int fbits, int is_unsigned) {
    double x = is_unsigned ? (double)v : (double)(int32_t)v;
    return neon_f32_out((float)(x / (double)(1ull << fbits)));
}

static inline uint64_t neon_f2fixed(uint64_t x, int fbits, int is_unsigned) {
    return neon_pack32(neon_f32_to_fixed(neon_lo32(x), fbits, is_unsigned),
                       neon_f32_to_fixed(neon_hi32(x), fbits, is_unsigned));
}

static inline uint64_t neon_fixed2f(uint64_t x, int fbits, int is_unsigned) {
    return neon_pack32(neon_fixed_to_f32(neon_lo32(x), fbits, is_unsigned),
                       neon_fixed_to_f32(neon_hi32(x), fbits, is_unsigned));
}

/* --- Advanced SIMD widening ------------------------------------------------
 *
 * The three-registers-of-different-lengths group and VSHLL/VMOVL. A source
 * lane is w = 8 << size bits and the result lane is 2w bits, so a Q result
 * is two D words r[0] (the low D) and r[1]: lane i of the 128 bits sits at
 * bit i * 2w. The helpers work on the Q word pairs directly, so a Q operand
 * that overlaps the destination is read in full before the emitter stores r.
 * Each lane is extended by hand (neon_sext for signed), and the sums and
 * products wrap modulo 2^(2w) by working at the wide lane size. */

/* Widen each lane of the D word src into r, zero-extended if is_unsigned and
 * sign-extended otherwise, then shifted left by shift. shift is below the
 * source lane width (VSHLL takes 0 to w - 1), and the shifted bits are masked
 * to the wide lane. VMOVL is shift 0. size is the SOURCE lane size. */
static inline void neon_widen(uint64_t r[2], uint64_t src, int size, int shift, int is_unsigned) {
    int w = 8 << size, ww = 2 * w;
    uint64_t mask = neon_lmask(size), wmask = neon_lmask(size + 1);
    r[0] = r[1] = 0;
    for (int i = 0; i < 64 / w; i++) {
        uint64_t v = (src >> (i * w)) & mask;
        uint64_t e = is_unsigned ? v : (uint64_t)neon_sext(v, w);
        r[(i * ww) / 64] |= ((e << shift) & wmask) << ((i * ww) % 64);
    }
}

/* r = a +/- b, wrapping at the wide lane size, for VADDL/VSUBL and the W forms
 * VADDW/VSUBW. size is the source size, so the lanes here are size + 1. r may
 * be a, since each word is read before it is written. */
static inline void neon_wadd(uint64_t r[2], const uint64_t a[2], const uint64_t b[2], int size, int sub) {
    for (int k = 0; k < 2; k++)
        r[k] = sub ? neon_sub(a[k], b[k], size + 1) : neon_add(a[k], b[k], size + 1);
}

/* r = a * b at the wide lane size, the low wide bits kept. The operands are
 * widened and so below 2^(2w) already. A product of two 32-bit values can need
 * 64 bits, and the uint64_t product keeps exactly those, since the signed one
 * fits too. */
static inline void neon_wmul(uint64_t r[2], const uint64_t a[2], const uint64_t b[2], int size) {
    r[0] = neon_mul(a[0], b[0], size + 1);
    r[1] = neon_mul(a[1], b[1], size + 1);
}

/* VADDL, VSUBL (wide = 0) and VADDW, VSUBW (wide = 1): r = n +/- m. n is the
 * D word n[0] for the L forms, and the Q pair n[0], n[1] for the W forms,
 * which is taken as it is, since its lanes are already wide. m is a D word
 * either way and is widened by the signedness. */
static inline void neon_wide_addsub(uint64_t r[2], const uint64_t n[2], uint64_t m,
                                    int size, int is_unsigned, int wide, int sub) {
    uint64_t wn[2], wm[2];
    if (wide) { wn[0] = n[0]; wn[1] = n[1]; }
    else      neon_widen(wn, n[0], size, 0, is_unsigned);
    neon_widen(wm, m, size, 0, is_unsigned);
    neon_wadd(r, wn, wm, size, sub);
}

/* VMULL: r = n ext * m ext, both D words widened by the signedness. */
static inline void neon_wide_mul(uint64_t r[2], uint64_t n, uint64_t m, int size, int is_unsigned) {
    uint64_t wn[2], wm[2];
    neon_widen(wn, n, size, 0, is_unsigned);
    neon_widen(wm, m, size, 0, is_unsigned);
    neon_wmul(r, wn, wm, size);
}

/* VMLAL (sub = 0) and VMLSL (sub = 1): r = r +/- n ext * m ext, with r the old
 * Qd on input. The product is formed first, so r may be read as the
 * accumulator without anything being overwritten early. */
static inline void neon_wide_mac(uint64_t r[2], uint64_t n, uint64_t m, int size,
                                 int is_unsigned, int sub) {
    uint64_t p[2];
    neon_wide_mul(p, n, m, size, is_unsigned);
    neon_wadd(r, r, p, size, sub);
}

/* VADDHN and VSUBHN (round = 0), VRADDHN and VRSUBHN (round = 1), which add
 * 1 << (w - 1) before taking the high half. size is the RESULT lane, w = 8 <<
 * size, and the sources have 2w-bit lanes: wide lane i of the Q pair n:m is in
 * word i / (32 / w) at lane i % (32 / w) of that word. Each wide lane is formed
 * at the wide size, so a carry or borrow stays inside its own lane, and the
 * result is bits 2w-1..w of it. For size 2 the wide lanes are 64 bits and the
 * uint64_t sum wraps at 2^64 as the architecture does. n and m are copied in,
 * so a D destination that overlaps them is written only after both are read. */
static inline uint64_t neon_addhn(const uint64_t n[2], const uint64_t m[2], int size,
                                  int sub, int round) {
    int w = 8 << size, per = 32 / w;
    uint64_t r = 0;
    for (int i = 0; i < 64 / w; i++) {
        int k = i / per, j = i % per;
        uint64_t a = neon_lane_of(n[k], size + 1, j), b = neon_lane_of(m[k], size + 1, j);
        uint64_t s = sub ? neon_sub(a, b, size + 1) : neon_add(a, b, size + 1);
        if (round) s = neon_add(s, 1ull << (w - 1), size + 1);
        r |= ((s >> w) & neon_lmask(size)) << (i * w);
    }
    return r;
}

/* VABDL (acc = 0) and VABAL (acc = 1): |n - m| per lane of the D words n and
 * m, zero-extended into the 2w-bit lanes of r. The difference of two w-bit
 * lanes is at most 2^w - 1, so neon_abd holds it exactly as an unsigned w-bit
 * value, and it is zero-extended whatever U is. Sign-extending it would turn
 * the 255 of s8 (0x80 against 0x7F) into 0xFFFF.
 * For VABAL, r is the old Qd on input and the difference is added to it, the
 * wide sum wrapping at 2^(2w) per lane. */
static inline void neon_abdl(uint64_t r[2], uint64_t n, uint64_t m, int size,
                             int is_unsigned, int acc) {
    uint64_t d[2];
    neon_widen(d, neon_abd(n, m, size, is_unsigned), size, 0, 1);
    if (acc) neon_wadd(r, r, d, size, 0);
    else     { r[0] = d[0]; r[1] = d[1]; }
}

/* The carry-less product of two 8-bit values, 15 bits wide: each set bit k of
 * b adds a copy of a shifted by k, and the copies are XORed, not added, which
 * is what makes it polynomial (GF(2)) multiplication. */
static inline uint64_t neon_pmul_lane(uint64_t a, uint64_t b) {
    uint64_t p = 0;
    for (int k = 0; k < 8; k++)
        if ((b >> k) & 1) p ^= a << k;
    return p;
}

/* VMUL.P8: the low 8 bits of each lane's carry-less product. Only the low bits
 * are kept, as VMUL keeps them for the integer lanes. */
static inline uint64_t neon_pmul8(uint64_t n, uint64_t m) {
    uint64_t r = 0;
    for (int i = 0; i < 8; i++)
        r |= (neon_pmul_lane(neon_lane_of(n, 0, i), neon_lane_of(m, 0, i)) & 0xFF) << (i * 8);
    return r;
}

/* VMULL.P8: the whole 15-bit product of each 8-bit lane, in a 16-bit lane of
 * the Q result. Lane i is at bit 16 i, so lanes 0-3 are r[0] and lanes 4-7
 * are r[1]. */
static inline void neon_pmull8(uint64_t r[2], uint64_t n, uint64_t m) {
    r[0] = r[1] = 0;
    for (int i = 0; i < 8; i++)
        r[i / 4] |= neon_pmul_lane(neon_lane_of(n, 0, i), neon_lane_of(m, 0, i)) << ((i % 4) * 16);
}

/* --- Advanced SIMD shuffles, table lookup and lane duplicate ----------------
 *
 * The array helpers take one or two D words (a[0], a[1] for a Q pair) and
 * update them in place. They are written so that no output is stored until
 * every input has been read, and the emitter loads its operands before it
 * calls them, so a destination that is also a source behaves as the
 * architecture defines it. */

/* Byte i (0..7 within the word) of a word array, counted from word 0. */
static inline uint64_t neon_byte(const uint64_t *w, int i) {
    return (w[i >> 3] >> (8 * (i & 7))) & 0xFF;
}

/* Lane `i` of x, lanes of 8 << size bits, with v in place of its old value. */
static inline uint64_t neon_put_lane(uint64_t x, int size, int i, uint64_t v) {
    int sh = i * (8 << size);
    return (x & ~(neon_lmask(size) << sh)) | (v << sh);
}

/* Element j of a word array, lanes of 8 << size bits, counted from word 0. */
static inline uint64_t neon_elem(const uint64_t *w, int size, int j) {
    int per = 8 >> size;                         /* lanes per D word */
    return neon_lane_of(w[j / per], size, j % per);
}

/* VEXT: the bytes of a then b (a first), read from byte imm on, become a.
 * imm is below the register width, so the index stays within a:b. */
static inline void neon_ext(uint64_t *a, const uint64_t *b, int words, int imm) {
    uint64_t r[2] = {0, 0};
    int nb = 8 * words;
    for (int i = 0; i < nb; i++) {
        int j = i + imm;
        uint64_t v = j < nb ? neon_byte(a, j) : neon_byte(b, j - nb);
        r[i >> 3] |= v << (8 * (i & 7));
    }
    for (int k = 0; k < words; k++) a[k] = r[k];
}

/* VREV: the size-bit elements of each container of `cont` bits (64, 32 or
 * 16) come in reverse order. Q does this to each D word. */
static inline void neon_rev(uint64_t *a, int words, int cont, int size) {
    int w = 8 << size, per = cont / w;           /* elements per container */
    for (int k = 0; k < words; k++) {
        uint64_t r = 0;
        for (int j = 0; j < 64 / w; j++) {
            int base = (j / per) * per;
            r |= neon_lane_of(a[k], size, base + per - 1 - (j - base)) << (j * w);
        }
        a[k] = r;
    }
}

/* VSWP: a and b exchange their words. */
static inline void neon_swp(uint64_t *a, uint64_t *b, int words) {
    for (int k = 0; k < words; k++) {
        uint64_t t = a[k];
        a[k] = b[k];
        b[k] = t;
    }
}

/* VTRN: within each D word, lane 2i+1 of a swaps with lane 2i of b. Q is two
 * independent words, so nothing crosses the halves. */
static inline void neon_trn(uint64_t *a, uint64_t *b, int words, int size) {
    int lanes = 8 >> size;
    for (int k = 0; k < words; k++)
        for (int i = 0; i + 1 < lanes; i += 2) {
            uint64_t x = neon_lane_of(a[k], size, i + 1), y = neon_lane_of(b[k], size, i);
            a[k] = neon_put_lane(a[k], size, i + 1, y);
            b[k] = neon_put_lane(b[k], size, i, x);
        }
}

/* VUZP: a then b is one run of 2n elements, n = words * lanes-per-word, and
 * it is split by parity: a gets the even ones, b the odd ones. For Q this
 * runs across the two D words, not within each. */
static inline void neon_uzp(uint64_t *a, uint64_t *b, int words, int size) {
    int lanes = 8 >> size, n = words * lanes;
    uint64_t ra[2] = {0, 0}, rb[2] = {0, 0};
    for (int k = 0; k < n; k++) {
        int e0 = 2 * k, e1 = 2 * k + 1;
        uint64_t x = e0 < n ? neon_elem(a, size, e0) : neon_elem(b, size, e0 - n);
        uint64_t y = e1 < n ? neon_elem(a, size, e1) : neon_elem(b, size, e1 - n);
        ra[k / lanes] = neon_put_lane(ra[k / lanes], size, k % lanes, x);
        rb[k / lanes] = neon_put_lane(rb[k / lanes], size, k % lanes, y);
    }
    for (int k = 0; k < words; k++) { a[k] = ra[k]; b[k] = rb[k]; }
}

/* VZIP: the elements of a and b are interleaved a0,b0,a1,b1,... over the
 * whole register (both words of a Q). a takes the first n of them, b the
 * rest. As with VUZP, a Q is not done half by half. */
static inline void neon_zip(uint64_t *a, uint64_t *b, int words, int size) {
    int lanes = 8 >> size, n = words * lanes;
    uint64_t ra[2] = {0, 0}, rb[2] = {0, 0};
    for (int k = 0; k < n; k++) {
        int j0 = k, j1 = n + k;                  /* positions in the interleave */
        uint64_t x = (j0 & 1) ? neon_elem(b, size, j0 >> 1) : neon_elem(a, size, j0 >> 1);
        uint64_t y = (j1 & 1) ? neon_elem(b, size, j1 >> 1) : neon_elem(a, size, j1 >> 1);
        ra[k / lanes] = neon_put_lane(ra[k / lanes], size, k % lanes, x);
        rb[k / lanes] = neon_put_lane(rb[k / lanes], size, k % lanes, y);
    }
    for (int k = 0; k < words; k++) { a[k] = ra[k]; b[k] = rb[k]; }
}

/* VDUP (scalar): lane `idx` of the D word m, copied to every lane. */
static inline uint64_t neon_dup_lane(uint64_t m, int size, int idx) {
    int w = 8 << size;
    uint64_t v = neon_lane_of(m, size, idx), r = 0;
    for (int i = 0; i < 64 / w; i++)
        r |= v << (i * w);
    return r;
}

/* VTBL and VTBX: each byte of idx selects a byte of the table, which is
 * `count` D words (1 to 4) laid end to end. An index at or past 8 * count
 * takes the byte of `fill`: zero for VTBL, the old destination for VTBX. */
static inline uint64_t neon_tbl(const uint64_t *t, int count, uint64_t idx, uint64_t fill) {
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t ix = (idx >> (8 * i)) & 0xFF, b;
        b = ix < 8u * (uint64_t)count ? neon_byte(t, (int)ix) : ((fill >> (8 * i)) & 0xFF);
        r |= b << (8 * i);
    }
    return r;
}

/* VLD2-4 and VST2-4 move n members of `words` D registers each (words is 2
 * for the Q-pair form). In memory the run of n*words D words holds element j
 * of member k at element index j*n + k, so the members are interleaved one
 * element at a time: member 0's first element, member 1's first, ..., then
 * the second element of each. Elements are counted across the whole member,
 * words * (8 >> size) of them, lowest address first.
 *
 * Both are pure functions on word arrays, with no memory access, so the tests
 * can check them directly. mem and in hold n*words D words; member k of out or
 * in is words D words at k*words. The arrays must not overlap. */

/* mem (memory order, lowest address first) to members. */
static inline void neon_deinterleave(const uint64_t *mem, uint64_t *out, int n, int words, int size) {
    int per = 8 >> size;                       /* elements per D word          */
    int elems = words * per;                   /* elements in one member       */
    for (int i = 0; i < n * words; i++) out[i] = 0;
    for (int k = 0; k < n; k++)
        for (int j = 0; j < elems; j++) {
            uint64_t *w = &out[k * words + j / per];
            *w = neon_put_lane(*w, size, j % per, neon_elem(mem, size, j * n + k));
        }
}

/* Members to mem (memory order): the inverse of neon_deinterleave. */
static inline void neon_interleave(const uint64_t *in, uint64_t *mem, int n, int words, int size) {
    int per = 8 >> size;
    int elems = words * per;
    for (int i = 0; i < n * words; i++) mem[i] = 0;
    for (int k = 0; k < n; k++)
        for (int j = 0; j < elems; j++) {
            int m = j * n + k;
            uint64_t *w = &mem[m / per];
            *w = neon_put_lane(*w, size, m % per, neon_elem(in + k * words, size, j));
        }
}

/* --- Advanced SIMD narrowing ------------------------------------------------
 *
 * VSHRN, VRSHRN, VQSHRN, VQRSHRN, VQSHRUN and VQRSHRUN, with VQMOVN and VQMOVUN
 * as the zero-amount case. The source is the Q register lo:hi and the result is
 * one D: the lanes of lo narrow into its low half, those of hi into its high
 * half. `size` is the RESULT lane size (8 << size bits), so a source lane is
 * twice that. Size 3 would need a 128-bit source and returns 0.
 *
 * A lane is shifted right by amt (arithmetic for a signed source), rounded if
 * asked, then truncated or saturated. The rounding bit is bit amt-1 of the
 * source and is added after the shift: adding 1 << (amt-1) to the source first
 * would overflow a 64-bit lane. amt is at most the result width, so no C shift
 * reaches 64 bits.
 *
 * The shifted lane is kept as a 64-bit two's complement pattern. An unsigned
 * source can reach 2^63 after rounding, which a signed read would take for a
 * negative value, so it is compared as unsigned. FPSCR.QC is not modelled, as
 * for neon_qop. */

/* One lane. x holds a source lane of 2*ew bits, sign-extended when src_signed.
 * With sat, the result clamps to the signed range of ew bits, or to
 * [0, 2^ew-1] when dst_unsigned. An unsigned source always takes
 * dst_unsigned, as no encoding narrows unsigned to signed. */
static inline uint64_t neon_narrow_lane(uint64_t x, int ew, int amt, int round, int sat, int src_signed, int dst_unsigned) {
    int sw = 2 * ew;
    if (src_signed && sw < 64 && ((x >> (sw - 1)) & 1)) x |= ~((1ull << sw) - 1);
    int neg = src_signed && (x >> 63);
    uint64_t y = x;
    if (amt > 0) {
        /* A negative value shifts as ~((~x) >> amt), the arithmetic shift in
         * unsigned arithmetic, as neon_shr does. */
        y = neg ? ~((~x) >> amt) : (x >> amt);
        if (round) y += (x >> (amt - 1)) & 1;
    }
    uint64_t emask = (1ull << ew) - 1;
    if (!sat) return y & emask;
    if (src_signed) {
        int64_t s = (int64_t)y;                    /* in range, see above */
        if (dst_unsigned) {
            if (s < 0) return 0;
            return (uint64_t)s > emask ? emask : (uint64_t)s;
        }
        int64_t hi = (int64_t)(emask >> 1), lo = -hi - 1;
        if (s > hi) s = hi;
        else if (s < lo) s = lo;
        return (uint64_t)s & emask;
    }
    return y > emask ? emask : y;
}

/* The whole narrowing: lanes of lo into the low half of the result, lanes of
 * hi into the high half, each lane narrowed by neon_narrow_lane. Both source
 * words are arguments, so Dd may be one of them. */
static inline uint64_t neon_narrow(uint64_t lo, uint64_t hi, int amt, int size, int round, int sat, int src_signed, int dst_unsigned) {
    if (size == 3) return 0;
    int ew = 8 << size, per = 32 / ew;            /* source lanes per D word */
    uint64_t r = 0;
    for (int i = 0; i < per; i++) {
        r |= neon_narrow_lane(neon_lane_of(lo, size + 1, i), ew, amt, round, sat, src_signed, dst_unsigned) << (i * ew);
        r |= neon_narrow_lane(neon_lane_of(hi, size + 1, i), ew, amt, round, sat, src_signed, dst_unsigned) << ((i + per) * ew);
    }
    return r;
}

/* VSHRN and VRSHRN: truncated to the result width, no saturation. amt is
 * 1..esize for a shift; a rounding overflow past the width wraps, as VRSHRN
 * does. */
static inline uint64_t neon_shrn(uint64_t lo, uint64_t hi, int amt, int size, int round) {
    return neon_narrow(lo, hi, amt, size, round, 0, 0, 0);
}

/* VQSHRN, VQRSHRN, VQSHRUN, VQRSHRUN, VQMOVN and VQMOVUN (amt 0, round 0).
 * src_signed reads the source as signed. dst_unsigned clamps to [0, 2^ew-1]
 * rather than the signed range. */
static inline uint64_t neon_qshrn(uint64_t lo, uint64_t hi, int amt, int size, int round, int src_signed, int dst_unsigned) {
    return neon_narrow(lo, hi, amt, size, round, 1, src_signed, dst_unsigned);
}

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_NEON_H */
