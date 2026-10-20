/* vfp.h — the floating-point register file.
 *
 * Most of what a decoder lumps together as "SIMD" on this platform is not
 * vector work at all. Measured on this module, 82% of the coprocessor space is
 * scalar VFP — ordinary single-precision floating point, plus the loads and
 * stores that feed it — and only 18% is Advanced SIMD proper. Scalar VFP maps
 * onto C `float` almost one to one, so it is worth implementing well before
 * anyone writes a line of vector code.
 *
 * The Cortex-A9 has VFPv3-D32: 32 double-precision registers, D0-D31, and
 * the low half of them is also addressable as 32 singles, S0-S31, where Dn
 * occupies S(2n) and S(2n+1) with S(2n) holding the LOW half. D16-D31 have no
 * single-precision names, but they are real and the compiler allocates them
 * (and NEON's Q0-Q15 alias the whole file). So the backing store is 64 words,
 * not 32 — sized for the doubles, with the singles as its lower half. Keeping
 * one array rather than two makes the aliasing automatic instead of something
 * to remember at every access.
 */

#ifndef VITARECOMP_VFP_H
#define VITARECOMP_VFP_H

#include "cpu.h"   /* VITA_TLS */

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Accessed as bits or as a float depending on the instruction, so a union
 * rather than a cast: type-punning through a pointer is undefined behaviour
 * and compilers do miscompile it at higher optimisation levels. */
typedef union {
    uint32_t u;
    float    f;
} vfp_reg;

/* 64 words: D0-D31. Singles index the low 32. */
#define VFP_WORDS 64
extern VITA_TLS vfp_reg vfp_s[VFP_WORDS];

/* VFP comparisons write FPSCR, not the integer flags. A separate VMRS then
 * copies them into APSR, and only after that can a conditional branch see
 * them. Modelling FPSCR separately is what makes that two-step visible — a
 * recompiler that folds the comparison straight into the integer flags gets
 * the right answer right up until code compares two floats, does integer work,
 * and only then branches. */
extern VITA_TLS int fpscr_n, fpscr_z, fpscr_c, fpscr_v;

static inline float vfp_getf(int n) { return vfp_s[n & 31].f; }
static inline void  vfp_setf(int n, float v) { vfp_s[n & 31].f = v; }

static inline uint32_t vfp_getu(int n) { return vfp_s[n & 31].u; }
static inline void     vfp_setu(int n, uint32_t v) { vfp_s[n & 31].u = v; }

/* Doubles occupy a register pair, low half first. The mask is 63, not 31:
 * masking by the singles' range folded D16-D31 onto D0-D15, so code using the
 * upper bank silently overwrote the lower one. */
static inline double vfp_getd(int n) {
    uint32_t lo = vfp_s[(n * 2) & 63].u, hi = vfp_s[(n * 2 + 1) & 63].u;
    uint64_t bits = ((uint64_t)hi << 32) | lo;
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static inline void vfp_setd(int n, double v) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    vfp_s[(n * 2) & 63].u     = (uint32_t)bits;
    vfp_s[(n * 2 + 1) & 63].u = (uint32_t)(bits >> 32);
}

/* Compare two floats and write FPSCR.
 *
 * The unordered case is the one that matters: if either operand is NaN the
 * result is "unordered", which sets C and V and clears N and Z — deliberately
 * failing every ordered comparison in both directions. Treating NaN as merely
 * "not equal" makes `a < b` and `a >= b` both false in C too, but for the wrong
 * reason, and the flags a later branch reads would be wrong. */
void vfp_cmp_f32(float a, float b);
void vfp_cmp_f64(double a, double b);

/* VMRS APSR_nzcv, FPSCR — copy the float flags to the integer ones. */
void vfp_mrs_apsr(void);

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_VFP_H */
