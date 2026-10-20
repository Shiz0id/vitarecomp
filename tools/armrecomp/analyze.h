/* analyze.h — function discovery.
 *
 * Recursive descent from seeds, carrying instruction-set state. Mode is the
 * part with no PSP analogue: a seed is worthless without knowing whether its
 * target is ARM or Thumb, and decoding a Thumb function as ARM produces
 * plausible instructions rather than an obvious failure.
 *
 * Where the seeds come from, in increasing order of what they find:
 *
 *   1. module_start, from .sce_module_info
 *   2. the export entry table
 *   3. a linear harvest of BL/BLX targets
 *   4. pointer-shape recovery for stored function pointers
 *
 * Step 4 is a HEURISTIC and is reported separately for that reason. On
 * ET_SCE_RELEXEC modules the relocations enumerate stored pointers exactly; on
 * ET_SCE_EXEC modules — which is every Vita launch-window title, and therefore
 * every exclusive worth recompiling — there are no relocations and shape is all
 * there is.
 */

#ifndef ARMRECOMP_ANALYZE_H
#define ARMRECOMP_ANALYZE_H

#include "decode.h"
#include "module.h"

typedef struct {
    uint32_t addr;          /* virtual address                      */
    arm_mode mode;
    uint32_t end;           /* highest address reached from here    */
    uint32_t insns;
    uint8_t  from_heuristic;
} vf_func;

typedef struct {
    vf_func *funcs;
    uint32_t count;
    uint32_t cap;

    uint8_t *seen;          /* one bit per 2 bytes: instruction start  */
    uint32_t seen_bytes;

    uint32_t insns_total;   /* instructions reached                    */
    uint32_t bytes_covered;
    uint32_t seeds_entry;
    uint32_t seeds_export;
    uint32_t seeds_call;
    uint32_t seeds_shape;
    /* Of seeds_shape, how many came from the DATA segments rather than from
     * .text. Kept apart because they are found in a table of pointers rather
     * than among instructions, which is a far better place to look — merging
     * the counts would hide which sweep is doing the work. */
    uint32_t seeds_shape_data;
    uint32_t rejected_shape;
    /* Thumb code pointers built in a register by a MOVW/MOVT pair and never
     * stored or called directly: thread entries, callbacks, handlers passed
     * to firmware. Kept apart from the shape tiers because the evidence is
     * different — the value is constructed by an instruction, not found in a
     * stored word. */
    uint32_t seeds_movw;
    /* Mode-switching calls whose target is not a known import stub, and so
     * were not followed into ARM. Counted rather than hidden: if a module has
     * genuine ARM code outside its stub table, this is the number that says
     * so. */
    uint32_t seeds_arm_refused;
    uint32_t simd_insns;
    uint32_t indirect_sites;
    /* TBB/TBH switches followed through their table, and those whose size
     * could not be proven from the guarding compare (left as indirect). */
    uint32_t jump_tables;
    uint32_t jump_tables_unbounded;
} vf_result;

/* Discover functions in the image's executable segment. Returns 0 on success.
 * The caller frees with vf_free(). */
int vf_discover(const vm_image *img, const vm_module *mod, vf_result *out);

void vf_free(vf_result *r);

#endif /* ARMRECOMP_ANALYZE_H */
