/* reloc.h — SCE relocations, read for what they say rather than applied.
 *
 * An ET_SCE_RELEXEC module (every VitaSDK homebrew, and the firmware's own
 * modules) carries PT_SCE_RELA segments listing each word the loader patches.
 * For discovery that list is the exact answer the pointer-shape heuristic
 * guesses at: a relocated word whose target is in the executable segment with
 * the Thumb bit set IS a stored pointer to a Thumb function. Leaf functions
 * with no prologue, which the heuristic must reject outside a table, are found
 * the same as any other.
 *
 * ET_SCE_EXEC modules — the commercial launch window — have none, and this
 * reports nothing for them.
 *
 * The encoding is a stream of variable-size entries (formats 0-9) that carry
 * state between them: later entries are deltas against the last full one.
 * Field layout follows vita-toolchain's sce-elf.h. Nothing here writes; the
 * target of each entry is computed from the module's link-time segment
 * addresses, which is where the generated code runs it.
 */

#ifndef ARMRECOMP_RELOC_H
#define ARMRECOMP_RELOC_H

#include "module.h"

#include <stdint.h>

/* The ARM ELF relocation codes the walk reports. */
enum {
    VR_ABS32      = 2,
    VR_TARGET1    = 38,
    VR_THM_MOVW   = 47,     /* R_ARM_THM_MOVW_ABS_NC */
    VR_THM_MOVT   = 48,
    VR_MOVW       = 43,
    VR_MOVT       = 44,
};

/* Called once per relocated location. `target` is S + A: the address the
 * patched word (or MOVW/MOVT pair) ends up holding. */
typedef void (*vr_fn)(void *ctx, uint32_t patch, uint32_t code, uint32_t target);

/* Walk every PT_SCE_RELA segment of the image. Returns the number of entries
 * walked, or -1 if the stream is malformed (the walk stops there). */
int vr_walk(const vm_image *img, vr_fn fn, void *ctx);

#endif /* ARMRECOMP_RELOC_H */
