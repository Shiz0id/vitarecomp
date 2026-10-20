/* cpu.h — the recompiled program's machine state.
 *
 * Registers are globals rather than a struct passed around, for the same
 * reason every recompiler does it: the generated C is millions of lines and
 * `r0` reads better than `cpu->r[0]` a few hundred thousand times over. There
 * is one thread of recompiled control at a time.
 */

#ifndef VITARECOMP_CPU_H
#define VITARECOMP_CPU_H

#include <stdint.h>

/* VITA_TLS — one register file per host thread, or one for the process.
 *
 * The generated C names registers as plain globals. That is right for a single
 * thread of recompiled control and wrong the moment a game creates a second
 * one: two host threads running guest code would share r0. Building with
 * VITARECOMP_THREADED gives every host thread its own copy, so a firmware
 * layer that runs each guest thread on its own host thread (Vita3K's kernel
 * does) gets correct state without the generated code changing at all.
 *
 * Off by default: a thread-local access costs a segment-relative load, and a
 * single-threaded bring-up gains nothing for it. */
#if defined(VITARECOMP_THREADED)
#  if defined(__cplusplus)
#    define VITA_TLS thread_local
#  elif defined(_MSC_VER)
#    define VITA_TLS __declspec(thread)
#  else
#    define VITA_TLS _Thread_local
#  endif
#else
#  define VITA_TLS
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* r0-r12, then the three with architectural meaning. r15 (PC) is deliberately
 * absent: a recompiled program has no program counter, and any instruction
 * that genuinely reads PC has its value resolved at translation time. */
extern VITA_TLS uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
extern VITA_TLS uint32_t r8, r9, r10, r11, r12;
extern VITA_TLS uint32_t sp, lr;

/* NZCV, held as separate ints rather than packed into a CPSR word.
 *
 * ARM leans on these far harder than MIPS does — a comparison and its branch
 * are separate instructions with arbitrary distance between them, and carry
 * feeds ADC/SBC chains. Keeping them unpacked means the compiler can usually
 * fold a flag write into the operation that produced it, and it makes the
 * generated C readable. */
extern VITA_TLS int flag_n, flag_z, flag_c, flag_v;

/* APSR.GE, four bits, one per byte lane. Written by the ARMv6 parallel
 * add/subtract instructions and read by SEL — the pair newlib's strcmp and
 * memchr use to test a word four bytes at a time. CPSR bits 19:16. */
extern VITA_TLS uint32_t flag_ge;

#ifdef __cplusplus
}
#endif

#endif /* VITARECOMP_CPU_H */
