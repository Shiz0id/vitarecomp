/* recomp_rt.h â€” the instructions whose C translation is not obvious.
 *
 * Everything here exists because a direct C expression would be WRONG, not
 * merely verbose. Each one is a place where ARM's definition and C's differ,
 * and where the difference is invisible until it produces a bad result deep in
 * arithmetic-heavy code.
 */

#ifndef VITARECOMP_RECOMP_RT_H
#define VITARECOMP_RECOMP_RT_H

#include "cpu.h"
#include "mem.h"
#include "vfp.h"
#include "neon.h"

#include <math.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- shifts ----------------------------------------------------------------
 *
 * C leaves a shift by >= the operand width UNDEFINED. ARM defines it: shifting
 * a 32-bit value left by 32 or more yields zero. On x86 the natural C
 * translation silently masks the count to 5 bits, so `x << 32` returns x
 * unchanged â€” the exact opposite of the right answer, and a bug that only
 * appears when a shift amount is computed rather than constant.
 */

static inline uint32_t vita_lsl(uint32_t v, uint32_t n) {
    return n >= 32 ? 0u : v << n;
}

static inline uint32_t vita_lsr(uint32_t v, uint32_t n) {
    return n >= 32 ? 0u : v >> n;
}

/* Arithmetic shift right saturates to the sign bit rather than reaching zero,
 * and C's >> on a signed value is only implementation-defined, so the sign
 * extension is done explicitly. */
static inline uint32_t vita_asr(uint32_t v, uint32_t n) {
    if (n >= 32) return (v & 0x80000000u) ? 0xFFFFFFFFu : 0u;
    if (n == 0)  return v;
    return (uint32_t)(((int32_t)v) >> n) |
           ((v & 0x80000000u) ? ~(0xFFFFFFFFu >> n) : 0u);
}

static inline uint32_t vita_ror(uint32_t v, uint32_t n) {
    n &= 31;
    return n ? ((v >> n) | (v << (32 - n))) : v;
}

/* RRX: a rotate right by one THROUGH the carry flag. It is what ROR #0 means
 * in an immediate shift, not a rotate by zero. */
static inline uint32_t vita_rrx(uint32_t v) {
    return (v >> 1) | ((uint32_t)flag_c << 31);
}

/* --- shifts that set the carry flag -------------------------------------------
 *
 * A flag-setting LOGICAL operation (ANDS, MOVS, TST, LSLS...) has no carry of
 * its own, so C becomes the last bit shifted out of operand 2. Arithmetic
 * ignores it; that is why these are separate from the plain shifts. An
 * amount of zero shifts nothing out and leaves C as it was, which is the case
 * for a shift-by-register whose low byte happens to be zero.
 */

static inline uint32_t vita_lsl_c(uint32_t v, uint32_t n) {
    if (n == 0) return v;
    flag_c = n <= 32 ? (int)((v >> (32 - n)) & 1) : 0;
    return vita_lsl(v, n);
}

static inline uint32_t vita_lsr_c(uint32_t v, uint32_t n) {
    if (n == 0) return v;
    flag_c = n <= 32 ? (int)((v >> (n - 1)) & 1) : 0;
    return vita_lsr(v, n);
}

static inline uint32_t vita_asr_c(uint32_t v, uint32_t n) {
    if (n == 0) return v;
    flag_c = (int)((n >= 32 ? v >> 31 : v >> (n - 1)) & 1);
    return vita_asr(v, n);
}

/* A rotate by a nonzero multiple of 32 leaves the value but still sets C to
 * its top bit, so the test is on n, not on n & 31. */
static inline uint32_t vita_ror_c(uint32_t v, uint32_t n) {
    if (n == 0) return v;
    uint32_t r = vita_ror(v, n);
    flag_c = (int)(r >> 31);
    return r;
}

static inline uint32_t vita_rrx_c(uint32_t v) {
    uint32_t r = vita_rrx(v);
    flag_c = (int)(v & 1);
    return r;
}

/* --- flags -----------------------------------------------------------------
 *
 * N and Z are mechanical. C and V are not, and they are the reason this file
 * exists: carry on an addition is an unsigned overflow, carry on a SUBTRACTION
 * is NOT a borrow but its complement, and overflow is a signed condition that
 * shares no bits with either. Writing these inline at every call site is how a
 * recompiler ends up with subtly different flag behaviour in three places.
 */

static inline void vita_flags_nz(uint32_t res) {
    flag_n = (int)(res >> 31);
    flag_z = (res == 0);
}

static inline void vita_flags_add(uint32_t a, uint32_t b, uint32_t res) {
    vita_flags_nz(res);
    flag_c = (res < a);
    flag_v = (int)(((a ^ res) & (b ^ res)) >> 31);
}

static inline void vita_flags_adc(uint32_t a, uint32_t b, uint32_t carry,
                                  uint32_t res) {
    vita_flags_nz(res);
    /* With a carry in, `res < a` is not sufficient: a + b + 1 can equal a
     * exactly when b is 0xFFFFFFFF, which carries but does not decrease. */
    flag_c = carry ? (res <= a) : (res < a);
    flag_v = (int)(((a ^ res) & (b ^ res)) >> 31);
}

/* ARM's carry on subtraction is set when there is NO borrow â€” the opposite of
 * most people's intuition and of several other architectures. */
static inline void vita_flags_sub(uint32_t a, uint32_t b, uint32_t res) {
    vita_flags_nz(res);
    flag_c = (a >= b);
    flag_v = (int)(((a ^ b) & (a ^ res)) >> 31);
}

/* --- condition codes -------------------------------------------------------
 *
 * Used by conditional branches and by every instruction inside an IT block.
 */

static inline int vita_cond(int cc) {
    switch (cc & 0xF) {
        case 0x0: return  flag_z;                        /* eq */
        case 0x1: return !flag_z;                        /* ne */
        case 0x2: return  flag_c;                        /* cs/hs */
        case 0x3: return !flag_c;                        /* cc/lo */
        case 0x4: return  flag_n;                        /* mi */
        case 0x5: return !flag_n;                        /* pl */
        case 0x6: return  flag_v;                        /* vs */
        case 0x7: return !flag_v;                        /* vc */
        case 0x8: return  flag_c && !flag_z;             /* hi */
        case 0x9: return !flag_c ||  flag_z;             /* ls */
        case 0xA: return  flag_n == flag_v;              /* ge */
        case 0xB: return  flag_n != flag_v;              /* lt */
        case 0xC: return !flag_z && (flag_n == flag_v);  /* gt */
        case 0xD: return  flag_z || (flag_n != flag_v);  /* le */
        default:  return 1;                              /* al */
    }
}

/* --- sign extension --------------------------------------------------------- */

static inline uint32_t vita_sxtb(uint32_t v) { return (uint32_t)(int32_t)(int8_t)(v & 0xFF); }
static inline uint32_t vita_sxth(uint32_t v) { return (uint32_t)(int32_t)(int16_t)(v & 0xFFFF); }
static inline uint32_t vita_uxtb(uint32_t v) { return v & 0xFF; }
static inline uint32_t vita_uxth(uint32_t v) { return v & 0xFFFF; }

/* --- parallel add/subtract and SEL -------------------------------------------
 *
 * Byte- or halfword-wise arithmetic in a core register, with a GE bit per
 * byte recording carry-out (add) or no-borrow (subtract). The 16-bit forms
 * set both GE bits of their lane. SEL then picks each result byte from rn
 * where GE is set and from rm where it is not.
 */

static inline uint32_t vita_uadd8(uint32_t a, uint32_t b) {
    uint32_t r = 0, ge = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t s = ((a >> (8 * i)) & 0xFF) + ((b >> (8 * i)) & 0xFF);
        r |= (s & 0xFF) << (8 * i);
        if (s >= 0x100) ge |= 1u << i;
    }
    flag_ge = ge;
    return r;
}

static inline uint32_t vita_usub8(uint32_t a, uint32_t b) {
    uint32_t r = 0, ge = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t x = (a >> (8 * i)) & 0xFF, y = (b >> (8 * i)) & 0xFF;
        r |= ((x - y) & 0xFF) << (8 * i);
        if (x >= y) ge |= 1u << i;
    }
    flag_ge = ge;
    return r;
}

static inline uint32_t vita_uadd16(uint32_t a, uint32_t b) {
    uint32_t lo = (a & 0xFFFF) + (b & 0xFFFF), hi = (a >> 16) + (b >> 16);
    flag_ge = (lo >= 0x10000 ? 0x3u : 0u) | (hi >= 0x10000 ? 0xCu : 0u);
    return (lo & 0xFFFF) | (hi << 16);
}

static inline uint32_t vita_usub16(uint32_t a, uint32_t b) {
    uint32_t xl = a & 0xFFFF, yl = b & 0xFFFF, xh = a >> 16, yh = b >> 16;
    flag_ge = (xl >= yl ? 0x3u : 0u) | (xh >= yh ? 0xCu : 0u);
    return ((xl - yl) & 0xFFFF) | (((xh - yh) & 0xFFFF) << 16);
}

static inline uint32_t vita_sel(uint32_t n, uint32_t m) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++)
        r |= (((flag_ge >> i) & 1) ? n : m) & (0xFFu << (8 * i));
    return r;
}

/* CLZ. __builtin_clz(0) is undefined, and ARM defines CLZ of zero as 32. */
static inline uint32_t vita_clz(uint32_t v) {
    uint32_t n = 0;
    if (!v) return 32;
    while (!(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

static inline uint32_t vita_rev(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8)
         | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
}

/* Bytes swapped within each halfword. */
static inline uint32_t vita_rev16(uint32_t v) {
    return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
}

/* The low halfword byte-swapped, then sign-extended. */
static inline uint32_t vita_revsh(uint32_t v) {
    return (uint32_t)(int32_t)(int16_t)(uint16_t)(((v & 0xFF) << 8) | ((v >> 8) & 0xFF));
}

static inline uint32_t vita_rbit(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    return vita_rev(v);
}

/* Bytes 0 and 2, each widened to a halfword. */
static inline uint32_t vita_uxtb16(uint32_t v) { return v & 0x00FF00FFu; }
static inline uint32_t vita_sxtb16(uint32_t v) {
    return (vita_sxtb(v) & 0xFFFFu) | (vita_sxtb(v >> 16) << 16);
}
/* The B16 extend-and-add: each halfword added on its own, no carry between. */
static inline uint32_t vita_add16(uint32_t a, uint32_t b) {
    return ((a + b) & 0xFFFFu) | (((a >> 16) + (b >> 16)) << 16);
}

/* SMULxy: the chosen signed halves multiplied. `sel` bit 1 takes n's top
 * half, bit 0 m's. */
static inline uint32_t vita_smulxy(uint32_t n, uint32_t m, int sel) {
    int32_t a = (int16_t)(uint16_t)((sel & 2) ? n >> 16 : n);
    int32_t b = (int16_t)(uint16_t)((sel & 1) ? m >> 16 : m);
    return (uint32_t)(a * b);
}

/* --- bitfields ---------------------------------------------------------------
 *
 * The obvious C is wrong in two places. `(1u << width) - 1` is UNDEFINED at
 * width 32, which is a legal UBFX width — and the natural way to write SBFX,
 * a left shift followed by a signed right shift, leans on `>>` sign-extending,
 * which C only implementation-defines. Both produce correct answers on the
 * usual compilers right up until they do not.
 */

static inline uint32_t vita_bf_mask(uint32_t width) {
    return width >= 32 ? 0xFFFFFFFFu : (1u << width) - 1u;
}

static inline uint32_t vita_ubfx(uint32_t v, uint32_t lsb, uint32_t width) {
    return vita_lsr(v, lsb) & vita_bf_mask(width);
}

static inline uint32_t vita_sbfx(uint32_t v, uint32_t lsb, uint32_t width) {
    uint32_t f = vita_lsr(v, lsb) & vita_bf_mask(width);
    if (width == 0 || width >= 32) return f;
    /* Sign is the field's top bit, not the register's. */
    return (f & (1u << (width - 1))) ? (f | ~vita_bf_mask(width)) : f;
}

/* BFI, and BFC with src == 0. The bits outside the field are preserved, which
 * is the whole point — this is the one place a destination register is read
 * before it is written. */
static inline uint32_t vita_bfi(uint32_t dst, uint32_t src,
                                uint32_t lsb, uint32_t width) {
    uint32_t m = vita_lsl(vita_bf_mask(width), lsb);
    return (dst & ~m) | (vita_lsl(src, lsb) & m);
}

/* --- gaps that announce themselves ------------------------------------------
 *
 * Anything not translated calls one of these. A gap that stops the program and
 * names itself is worth far more than one that produces a program which runs
 * and is quietly wrong.
 */

void vita_trap_unimpl(uint32_t addr, uint32_t raw, const char *what);
void vita_trap_indirect(uint32_t addr, uint32_t target);
void vita_trap_import(uint32_t addr, uint32_t nid, const char *name);

/* A firmware layer that can answer import calls by NID.
 *
 * The generated default for every import calls vita_trap_import. With a
 * handler installed, that call is offered to the handler first, and a nonzero
 * return means the call was serviced — arguments read from r0-r3 and the
 * stack, the result left in r0 — so the trap does not fire. Zero falls through
 * to the trap exactly as before, so a handler that knows only some NIDs is
 * still incrementally useful.
 *
 * A `vita_hle_*` defined in C still wins: it is linked in instead of the
 * default and never reaches this path. So the order of precedence is a
 * hand-written HLE body, then the handler, then the named trap. */
typedef int (*vita_import_handler)(uint32_t stub, uint32_t nid, const char *name);
void vita_set_import_handler(vita_import_handler fn);

/* Total traps hit. Only meaningful under VITARECOMP_TRACE=1, where execution
 * continues past them; otherwise the first one ends the program. */
uint32_t vita_trap_count(void);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_RECOMP_RT_H */

