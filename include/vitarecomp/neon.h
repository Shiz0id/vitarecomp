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

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_NEON_H */
