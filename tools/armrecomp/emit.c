/* emit.c â€” ARM to C.
 *
 * The output is meant to be read. Every instruction carries its address and
 * disassembly as a comment above the C it became, because the first thing
 * anyone does with a recompiled function that misbehaves is compare it against
 * the original.
 *
 * Two things this does NOT do, deliberately:
 *
 *   - It does not guess. An instruction whose operands the decoder did not
 *     fill becomes a named trap, never an approximation. A gap that stops the
 *     program is worth far more than one that produces a program which runs and
 *     is wrong.
 *   - It does not reorder. ARM has no delay slots, so the translation is
 *     instruction-for-instruction, and the only control flow that moves is a
 *     branch becoming a goto.
 */

#include "emit.h"

#include <stdlib.h>
#include <string.h>

#define MAX_INSNS_PER_FUNC 65536

static const char *reg_name(int r) {
    static const char *n[16] = {
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
        "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"
    };
    return (r >= 0 && r < 16) ? n[r] : "?";
}

/* --- collecting one function's instructions -------------------------------- */

typedef struct {
    arm_insn *v;
    uint32_t  n, cap;
    uint32_t *labels;
    uint32_t  nlabels, lcap;
    uint8_t  *in_it;        /* per v[]: covered by an IT before it          */
    uint32_t  into_it;      /* labels dropped for landing inside an IT block */
} fbody;

static void fb_push(fbody *b, const arm_insn *in) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->v = (arm_insn *)realloc(b->v, b->cap * sizeof(arm_insn));
    }
    b->v[b->n++] = *in;
}

static void fb_label(fbody *b, uint32_t addr) {
    for (uint32_t i = 0; i < b->nlabels; i++)
        if (b->labels[i] == addr) return;
    if (b->nlabels == b->lcap) {
        b->lcap = b->lcap ? b->lcap * 2 : 64;
        b->labels = (uint32_t *)realloc(b->labels, b->lcap * sizeof(uint32_t));
    }
    b->labels[b->nlabels++] = addr;
}

static int fb_has_label(const fbody *b, uint32_t addr) {
    for (uint32_t i = 0; i < b->nlabels; i++)
        if (b->labels[i] == addr) return 1;
    return 0;
}

/* Collect a function by FOLLOWING ITS CONTROL FLOW, not by sweeping its extent.
 *
 * The distinction is the difference between a usable output and a 2.4 GB one.
 * `fn->end` is the highest address the discovery walk reached, and because a
 * tail call is a branch, that walk follows branches into other functions â€” so
 * an extent routinely spans code and data belonging to many functions. Sweeping
 * it linearly re-emits all of that under every function whose extent covers it,
 * and the same instruction lands in the output dozens of times.
 *
 * Flow-following emits each instruction once, in exactly the places it is
 * genuinely reachable, and stops at returns. Extent is still useful as a bound;
 * it is not a body.
 */

#define VISIT_SLOTS 8192

typedef struct { uint32_t k[VISIT_SLOTS]; } visited;

static int visit_mark(visited *v, uint32_t addr) {
    uint32_t h = (addr * 2654435761u) & (VISIT_SLOTS - 1);
    for (uint32_t i = 0; i < VISIT_SLOTS; i++) {
        uint32_t s = (h + i) & (VISIT_SLOTS - 1);
        if (v->k[s] == addr) return 0;          /* already seen */
        if (v->k[s] == 0)    { v->k[s] = addr; return 1; }
    }
    return 0;                                    /* table full: stop expanding */
}

static int cmp_addr(const void *a, const void *b) {
    uint32_t x = ((const arm_insn *)a)->addr, y = ((const arm_insn *)b)->addr;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* --- the set of addresses that are actually functions ----------------------- */
/*
 * The emitter must never name a symbol it does not define. A call target only
 * becomes `vita_func_<addr>()` if discovery registered it; anything else is a
 * trap.
 *
 * This is not defensive tidying — it is the only correct answer. A linear sweep
 * decodes literal pools and tables as instructions, and some of that data
 * decodes as a BL with a plausible target. Two such targets in this module land
 * *inside the import table*, one of them 0x32 bytes into the first entry: not
 * code, never called at run time, and pure decoding artefact. Emitting calls to
 * them produced C that compiled and then failed to link.
 */

typedef struct { uint32_t *v; uint32_t n; } funcset;

static int fs_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int fs_has(const funcset *s, uint32_t addr) {
    uint32_t lo = 0, hi = s->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->v[mid] == addr) return 1;
        if (s->v[mid] <  addr) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static void collect(const vm_image *img, const vf_func *fn, fbody *b) {
    const uint8_t *text = img->data + img->seg_file;
    const uint32_t base = img->seg_vaddr;

    visited seen;
    memset(&seen, 0, sizeof(seen));

    /* A function registered because something called it may have been walked
     * by discovery only briefly — its recorded extent can be zero or tiny. The
     * body is still real, so fall back to a generous bound and let control flow
     * terminate the walk, which it does at the first return. Extent is a
     * safety limit here, not the definition of the body. */
    uint32_t bound = (fn->end > fn->addr + 16) ? fn->end : fn->addr + 8192;

    uint32_t stack[512];
    int sp_ = 0;
    stack[sp_++] = fn->addr;

    while (sp_ > 0 && b->n < MAX_INSNS_PER_FUNC) {
        uint32_t a = stack[--sp_];
        uint8_t itstate = 0;

        for (;;) {
            if (a < base || a >= base + img->seg_len) break;
            if (a >= bound) break;                   /* extent as a bound */
            if (!visit_mark(&seen, a)) break;        /* already emitted */

            arm_insn in;
            int n = arm_decode(text, base, img->seg_len, a, fn->mode, &in);
            if (n == 0) break;
            arm_it_step(&itstate, &in);
            fb_push(b, &in);

            if ((in.cls == A_RETURN || in.cls == A_UNDEF) && !in.conditional) break;
            if (in.cls == A_INDIRECT && !in.conditional && !in.table_n) break;

            if (in.table_n) {                        /* switch: every case */
                for (uint32_t i = 0; i < in.table_n; i++) {
                    uint32_t t = arm_table_target(text, base, &in, i);
                    if (t < fn->addr || t >= bound) continue;
                    fb_label(b, t);
                    if (sp_ < (int)(sizeof(stack) / sizeof(stack[0])))
                        stack[sp_++] = t;
                }
                break;
            }

            /* `bound`, not `fn->end`.
             *
             * These must be the same notion of "inside this function" or the
             * walk and the labelling disagree. The walk above collects up to
             * `bound`, which falls back to a generous range when discovery
             * recorded only a tiny extent — and it records a tiny extent
             * whenever an earlier function's walk had already marked those
             * instructions seen, which is common.
             *
             * Gating labels on `fn->end` instead meant that for exactly those
             * functions, instructions were collected but branches into them got
             * no label. Each then fell through to the call-or-trap path and
             * trapped: 2,547 instructions, and every one of them an ordinary
             * intra-function branch that had already been translated correctly
             * a few lines above. */
            if (in.has_target && in.cls == A_BRANCH &&
                in.target >= fn->addr && in.target < bound) {
                fb_label(b, in.target);
                if (sp_ < (int)(sizeof(stack) / sizeof(stack[0])))
                    stack[sp_++] = in.target;
                if (!in.conditional) break;          /* flow ends here */
            } else if (in.cls == A_BRANCH && !in.conditional) {
                break;                               /* tail call */
            }

            a += (uint32_t)n;
        }
    }

    /* Blocks were discovered out of order; emit in address order so the output
     * reads alongside a disassembly. */
    if (b->n > 1) qsort(b->v, b->n, sizeof(arm_insn), cmp_addr);

    /* IT state is a property of the straight-line run, so it is decided again
     * here, in address order, rather than trusted from the walk.
     *
     * The walk visits blocks depth-first and marks each instruction once. If
     * a block starting INSIDE an IT block was walked before the run through
     * the IT, the covered instructions were decoded with no ITSTATE, and the
     * fall-through walk then stopped at them as already seen: they would be
     * emitted unconditional. Re-decoding every contiguous run with one
     * ITSTATE makes the result independent of visiting order. */
    b->in_it = (uint8_t *)calloc(b->n ? b->n : 1, 1);
    if (fn->mode == ARM_T32) {
        uint8_t itstate = 0;
        for (uint32_t k = 0; k < b->n; k++) {
            if (k == 0 || b->v[k - 1].addr + b->v[k - 1].width != b->v[k].addr)
                itstate = 0;
            b->in_it[k] = (itstate & 0xF) != 0;
            arm_insn in;
            if (arm_decode(text, base, img->seg_len, b->v[k].addr, fn->mode, &in) == 0)
                continue;
            arm_it_step(&itstate, &in);
            b->v[k] = in;
        }
    }

    /* Drop labels for blocks that were never collected.
     *
     * A label is recorded the moment a branch target is seen inside the
     * function's extent, but the block behind it is only reached if the walk
     * gets there — and the walk can stop short at any of three limits: the
     * visited table filling, the block stack filling, or the per-function
     * instruction cap. When that happens the label is referenced by a `goto`
     * and never placed, which is C that does not compile.
     *
     * Pruning here rather than raising the limits is the honest fix: the limits
     * exist for a reason, and a `goto` into a block we did not translate would
     * be a lie even if the compiler accepted it. Removing the label routes the
     * branch through the call-or-trap path below, which says what is actually
     * true — either the target is a function, or we cannot get there.
     *
     * This only showed up at 1,500 functions. At 100 nothing reached a limit,
     * so the output compiled and the bug was invisible. */
    uint32_t kept = 0;
    for (uint32_t i = 0; i < b->nlabels; i++) {
        int placed = 0;
        uint32_t lo = 0, hi = b->n;
        while (lo < hi) {                      /* body is sorted by address */
            uint32_t mid = lo + (hi - lo) / 2;
            if (b->v[mid].addr == b->labels[i]) {
                /* A branch into the middle of an IT block is UNPREDICTABLE;
                 * on hardware ITSTATE is cleared, so a `goto` to a label
                 * guarded by the block's condition would not be the same
                 * program. Compilers never emit one, so finding one means
                 * data decoded as a branch. Route it to the trap. */
                if (b->in_it[mid]) { b->into_it++; break; }
                placed = 1;
                break;
            }
            if (b->v[mid].addr <  b->labels[i]) lo = mid + 1; else hi = mid;
        }
        if (placed) b->labels[kept++] = b->labels[i];
    }
    b->nlabels = kept;
}

/* --- operand rendering ------------------------------------------------------ */

/* A register operand, with r15 resolved.
 *
 * A recompiled program has no PC, so `pc` is not a variable and emitting it
 * produces C that does not compile â€” which is the good failure. The value is
 * known at translation time: reading PC in Thumb yields the instruction's own
 * address plus 4. The high-register ADD/MOV/CMP forms are where this actually
 * shows up, and they are common enough (position-independent address
 * computation) that leaving them to a trap would cost real coverage. */
static void reg_operand(char *dst, size_t cap, int r, const arm_insn *in) {
    if (r == 15) snprintf(dst, cap, "0x%08Xu", in->addr + 4);
    else         snprintf(dst, cap, "%s", reg_name(r));
}

/* A SOURCE register as a C expression, for the many places that want one
 * inline in an fprintf rather than in a buffer.
 *
 * The division this enforces is the one that kept getting lost: a DESTINATION
 * of r15 is a branch and is trapped by the guard in emit_insn, while a SOURCE
 * of r15 is a CONSTANT this pass knows — the instruction's own address plus
 * four, which is how the architecture defines reading the PC. Naming it "pc"
 * is never right in either case, and reg_name() is now left naming only
 * destinations.
 *
 * Rotating buffers because two sources can appear in one fprintf; the most any
 * call site uses is two. */
static const char *src_name(const arm_insn *in, int r) {
    static char buf[4][32];
    static unsigned next;
    if (r != 15) return reg_name(r);
    char *b = buf[next++ & 3];
    snprintf(b, sizeof(buf[0]), "0x%08Xu", in->addr + 4);
    return b;
}

/* The second operand of a data-processing instruction: an immediate, a plain
 * register, or a shifted register.
 *
 * `carry` selects the shifts that also set C to the last bit shifted out,
 * which is what a flag-setting logical op does with its operand 2. An
 * immediate's carry is not an expression, so callers emit it themselves
 * (shifter_carry_imm). */
static void operand2_ex(char *dst, size_t cap, const arm_insn *in, int carry) {
    if (in->has_imm) {
        snprintf(dst, cap, "0x%X", in->imm);
        return;
    }
    if (in->rm == ARM_NO_REG) { snprintf(dst, cap, "/*?*/0"); return; }
    if (in->shift_amt == 0 && in->shift_type == SH_LSL) {
        reg_operand(dst, cap, in->rm, in);
        return;
    }
    if (in->rm == 15) { reg_operand(dst, cap, 15, in); return; }
    if (in->shift_type == SH_RRX) {
        snprintf(dst, cap, "%s(%s)", carry ? "vita_rrx_c" : "vita_rrx",
                 src_name(in, in->rm));
        return;
    }
    static const char *fn[2][4] = {
        { "vita_lsl",   "vita_lsr",   "vita_asr",   "vita_ror"   },
        { "vita_lsl_c", "vita_lsr_c", "vita_asr_c", "vita_ror_c" },
    };
    snprintf(dst, cap, "%s(%s, %u)", fn[carry != 0][in->shift_type & 3],
             src_name(in, in->rm), in->shift_amt);
}

static void operand2(char *dst, size_t cap, const arm_insn *in) {
    operand2_ex(dst, cap, in, 0);
}

/* A flag-setting logical op with a rotated immediate sets C to the
 * immediate's top bit. Constant, so it is decided here. */
static void shifter_carry_imm(FILE *f, const arm_insn *in) {
    if (in->sets_flags && in->has_imm && in->imm_carry)
        fprintf(f, "    flag_c = %d;\n", (int)(in->imm >> 31));
}

/* The address expression for a load or store. */
static int address(char *dst, size_t cap, const arm_insn *in) {
    if (in->rn == ARM_NO_REG) return 0;

    if (in->rn == 15) {
        /* PC-relative. A recompiled program has no PC, so it is resolved here:
         * for Thumb the base is the instruction address plus 4, aligned down to
         * a word. This is what turns a literal-pool load into a constant.
         *
         * Every caller must come through here rather than formatting `rn`
         * itself: `reg_name(15)` is "pc", and cpu.h deliberately has no such
         * variable, so a caller that builds its own address expression emits C
         * that does not compile. That is the good failure — the bad one is a
         * `pc` that exists and holds something plausible. */
        uint32_t pc = (in->addr + 4) & ~3u;
        snprintf(dst, cap, "0x%08X", in->mem_add ? pc + in->imm : pc - in->imm);
        return 1;
    }
    /* The U bit is not decoration: the T32 8-bit form encodes [rN, #-imm]
     * with the same fields as the positive one, so ignoring it reads from the
     * wrong side of the base register. */
    /* A post-indexed access uses the base alone; the offset only moves it
     * afterwards (writeback()). */
    if (in->postindex)
        snprintf(dst, cap, "%s", src_name(in, in->rn));
    else if (in->has_imm && in->imm)
        snprintf(dst, cap, "%s %c 0x%X", src_name(in, in->rn),
                 in->mem_add ? '+' : '-', in->imm);
    else if (in->rm != ARM_NO_REG && in->shift_amt)
        /* [rn, rm, lsl #n]: array indexing. Dropping the shift indexed
         * every word array by bytes. */
        snprintf(dst, cap, "%s + (%s << %u)", src_name(in, in->rn),
                 src_name(in, in->rm), in->shift_amt);
    else if (in->rm != ARM_NO_REG)
        snprintf(dst, cap, "%s + %s", src_name(in, in->rn), src_name(in, in->rm));
    else
        snprintf(dst, cap, "%s", src_name(in, in->rn));
    return 1;
}

/* The base update of a pre- or post-indexed load or store, emitted after the
 * access. For pre-indexing the access already used rn +/- imm, so both forms
 * leave rn at the same place. */
static void writeback(FILE *f, const arm_insn *in) {
    if (in->writeback && in->imm)
        fprintf(f, "    %s %c= 0x%X;\n", reg_name(in->rn),
                in->mem_add ? '+' : '-', in->imm);
}

/* --- one instruction --------------------------------------------------------- */

/* Mnemonics are static strings, so the pointer identifies the kind and the
 * table stays a short linear scan. */
static void note_trap(emit_stats *st, const char *what) {
    if (!what) what = "?";
    for (uint32_t i = 0; i < st->trap_kinds; i++) {
        if (st->traps[i].what == what) { st->traps[i].count++; return; }
    }
    if (st->trap_kinds < EM_MAX_TRAP_KINDS) {
        st->traps[st->trap_kinds].what  = what;
        st->traps[st->trap_kinds].count = 1;
        st->trap_kinds++;
        return;
    }
    st->traps_other++;
}

static void trap(FILE *f, const arm_insn *in, const char *why) {
    fprintf(f, "    vita_trap_unimpl(0x%08X, 0x%08X, \"%s\");\n",
            in->addr, in->raw, why);
}

/* Returns 1 if real C was emitted, 0 if a trap was. */
/* The C symbol for an imported firmware function.
 *
 * The resolved name is used when the database knows it, because the whole point
 * of binding imports is that the generated C says `vita_hle_sceGxmDraw()` where
 * it would otherwise say `vita_func_814BB75C()` and leave the reader to work
 * out that the callee is not ours at all. An unresolved NID still gets a stable
 * symbol so the output links either way. */
static void hle_symbol(char *dst, size_t cap, const nid_db *db,
                       const vm_stub *s) {
    const char *n = nid_func_name(db, s->lib_nid, s->func_nid);
    if (n) snprintf(dst, cap, "vita_hle_%s", n);
    else   snprintf(dst, cap, "vita_hle_nid_%08X", s->func_nid);
}

/* Follow a GNU ld Thumb->ARM interworking veneer to where it really goes.
 *
 * When Thumb code calls a symbol the linker believes is ARM (VitaSDK's import
 * stubs are), ld puts a veneer between them:
 *
 *     __sceFoo_from_thumb:  bx pc ; b.n .-4       (Thumb, to ARM at +4)
 *                           ldr ip, [pc]          (ARM)
 *                           add pc, ip, pc
 *                           .word  target - (here + 8)
 *
 * Nothing about it is worth translating: it is a jump, and its target is
 * fixed at link time. Returns the target, or `addr` if this is not a veneer. */
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t through_veneer(const vm_image *img, uint32_t addr) {
    const uint32_t base = img->seg_vaddr, len = img->seg_len;
    if (addr < base || addr + 16 > base + len || (addr & 3)) return addr;
    const uint8_t *p = img->data + img->seg_file + (addr - base);
    uint16_t t0 = (uint16_t)(p[0] | p[1] << 8), t1 = (uint16_t)(p[2] | p[3] << 8);
    uint32_t a0 = le32(p + 4), a1 = le32(p + 8), off = le32(p + 12);
    if (t0 != 0x4778 || (t1 != 0xE7FD && t1 != 0x46C0)) return addr;
    if (a0 != 0xE59FC000u || a1 != 0xE08CF00Fu) return addr;
    return addr + 8 + 8 + off;     /* the add's pc reads as its address + 8 */
}

/* The shuffles take their operands as arrays of one or two D words. Each
 * element is read into the array before the helper runs, and stored only
 * after it, so a destination that is also a source reads its old value. */
static void emit_neon_load(FILE *f, const char *arr, int reg, int regs) {
    for (int k = 0; k < regs; k++)
        fprintf(f, " %s[%d] = neon_getd(%d);", arr, k, reg + k);
}

static void emit_neon_store(FILE *f, const char *arr, int reg, int regs) {
    for (int k = 0; k < regs; k++)
        fprintf(f, " neon_setd(%d, %s[%d]);", reg + k, arr, k);
}

static int emit_insn(const vm_image *img, const vm_module *mod, const nid_db *db,
                     const funcset *fs, const fbody *b,
                     const arm_insn *in, FILE *f, emit_stats *st) {
    char o2[64], addr[64], rn_s[32];

    /* The first operand, with r15 resolved. Computed once because several
     * paths below need it and the ternary for "rn defaults to rd" is easy to
     * get inconsistently wrong when repeated. */
    reg_operand(rn_s, sizeof(rn_s),
                in->rn == ARM_NO_REG ? in->rd : in->rn, in);

    /* A call whose target is an import stub is a firmware call, and must be
     * bound before anything else looks at it. Otherwise it decodes as an
     * ordinary call to an ordinary address — the stub body is a placeholder the
     * loader overwrites at load time, so recompiling it would translate filler
     * and then "return" into whatever the filler happened to be. */
    if ((in->cls == A_CALL || in->cls == A_BRANCH) && in->has_target) {
        const vm_stub *s = vm_find_stub(mod, through_veneer(img, in->target));
        if (s) {
            char sym[96];
            hle_symbol(sym, sizeof(sym), db, s);
            /* This path runs before the condition guard below, so it applies
             * the condition itself: a conditional firmware call is common
             * (`it ne; blne sceFoo`), and emitting it unconditionally is wrong
             * only when the condition is false. */
            const char *open = "", *close = "";
            char cbuf[32];
            if (in->cond != ARM_COND_AL) {
                snprintf(cbuf, sizeof(cbuf), "if (vita_cond(%u)) { ", in->cond);
                open = cbuf; close = " }";
            }
            if (in->cls == A_CALL) fprintf(f, "    %s%s();%s\n", open, sym, close);
            else                   fprintf(f, "    %s%s(); return;%s\n", open, sym, close);  /* tail call */
            st->import_calls++;
            return 1;
        }
    }

    /* A destination of r15 is a write to the PC, which is a BRANCH. The
     * decoder classifies the real ones as branches, so anything still carrying
     * r15 as a destination here is an encoding this emitter does not model —
     * and emitting it would print `pc`, which cpu.h deliberately does not
     * define.
     *
     * One guard rather than one per case, because one per case is what was
     * tried and it kept not holding: this is the FOURTH distinct path to reach
     * a build emitting a bare `pc` (MOV pc,rN; VLDR from a literal pool; STRD;
     * and MOVT/MOVW/ADDW). Each new translation had to remember the rule
     * independently, and eventually one did not. Checking it in the one place
     * every translation passes through is the only version that stays fixed.
     *
     * Source operands are not covered here and must not be: reg_operand()
     * resolves rm == 15 to the instruction's own address at translation time,
     * which is correct and is how PC-relative arithmetic works at all.
     *
     * Scoped in two ways, both learned by getting it wrong. `rt == 15` only
     * means the PC in a LOAD or STORE: in `VMRS APSR_nzcv, FPSCR` the same
     * field encodes "the condition flags", and a blanket rule trapped 3,827
     * working instructions. And an instruction the decoder already left as
     * OP_NONE is going to trap anyway with a more specific name than this one,
     * so it is left alone rather than relabelled. */
    int writes_pc = (in->rd == 15)
                 || ((in->cls == A_LOAD || in->cls == A_STORE)
                     && (in->rt == 15 || in->rt2 == 15));
    if (in->op != OP_NONE && writes_pc) {
        trap(f, in, "writes pc");
        note_trap(st, "writes pc");
        return 0;
    }

    /* An instruction made conditional by an IT block, or a conditional branch,
     * is wrapped rather than translated differently. Getting this wrong is
     * invisible until a condition happens to be false. */
    int guarded = 0;
    if (in->cls != A_BRANCH && in->cond != ARM_COND_AL && in->op != OP_IT) {
        fprintf(f, "    if (vita_cond(%u)) {\n", in->cond);
        guarded = 1;
    }

    int ok = 1;
    switch (in->op) {
        case OP_MOV:
            operand2_ex(o2, sizeof(o2), in, in->sets_flags);
            shifter_carry_imm(f, in);
            fprintf(f, "    %s = %s;\n", reg_name(in->rd), o2);
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;

        case OP_MOVT:
            /* Top half written, bottom half preserved. One line, and the
             * difference between a program that builds correct pointers and one
             * whose every constructed address is missing its high 16 bits. */
            fprintf(f, "    %s = (%s & 0x0000FFFFu) | 0x%08Xu;\n",
                    reg_name(in->rd), reg_name(in->rd), in->imm << 16);
            break;

        case OP_MVN:
            operand2_ex(o2, sizeof(o2), in, in->sets_flags);
            shifter_carry_imm(f, in);
            fprintf(f, "    %s = ~(%s);\n", reg_name(in->rd), o2);
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;

        case OP_ADD: case OP_SUB: {
            operand2(o2, sizeof(o2), in);
            const char *rn = rn_s;
            if (in->sets_flags) {
                /* The operands have to be captured before the destination is
                 * written, because rd and rn are frequently the same register
                 * and the flag computation needs the ORIGINAL values. */
                fprintf(f, "    { uint32_t _a = %s, _b = %s;\n", rn, o2);
                fprintf(f, "      %s = _a %c _b;\n", reg_name(in->rd),
                        in->op == OP_ADD ? '+' : '-');
                fprintf(f, "      vita_flags_%s(_a, _b, %s); }\n",
                        in->op == OP_ADD ? "add" : "sub", reg_name(in->rd));
            } else {
                fprintf(f, "    %s = %s %c %s;\n", reg_name(in->rd), rn,
                        in->op == OP_ADD ? '+' : '-', o2);
            }
            break;
        }

        case OP_ADC: case OP_SBC: {
            const char *rn = rn_s;
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    { uint32_t _a = %s, _b = %s, _c = (uint32_t)flag_c;\n", rn, o2);
            if (in->op == OP_ADC) {
                fprintf(f, "      %s = _a + _b + _c;\n", reg_name(in->rd));
                if (in->sets_flags)
                    fprintf(f, "      vita_flags_adc(_a, _b, _c, %s);\n", reg_name(in->rd));
            } else {
                fprintf(f, "      %s = _a - _b - (1u - _c);\n", reg_name(in->rd));
                if (in->sets_flags)
                    fprintf(f, "      vita_flags_sub(_a, _b + (1u - _c), %s);\n", reg_name(in->rd));
            }
            fprintf(f, "    }\n");
            break;
        }

        case OP_RSB:
            /* rd = op2 - rn. The 16-bit NEG form decodes with a zero immediate,
             * which made "always zero" look right: rsb.w r1, r1, #2048 was
             * emitted as 0 - r1, and bridgetest's snprintf got a size of zero. */
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    { uint32_t _a = %s, _b = %s;\n", o2, src_name(in, in->rn));
            fprintf(f, "      %s = _a - _b;\n", reg_name(in->rd));
            if (in->sets_flags)
                fprintf(f, "      vita_flags_sub(_a, _b, %s);\n", reg_name(in->rd));
            fprintf(f, "    }\n");
            break;

        case OP_AND: case OP_ORR: case OP_EOR: case OP_BIC: case OP_ORN: {
            operand2_ex(o2, sizeof(o2), in, in->sets_flags);
            shifter_carry_imm(f, in);
            const char *rn = rn_s;
            const char *ops = in->op == OP_AND ? "&" : in->op == OP_ORR ? "|" : "^";
            if (in->op == OP_BIC)
                fprintf(f, "    %s = %s & ~(%s);\n", reg_name(in->rd), rn, o2);
            else if (in->op == OP_ORN)
                fprintf(f, "    %s = %s | ~(%s);\n", reg_name(in->rd), rn, o2);
            else
                fprintf(f, "    %s = %s %s %s;\n", reg_name(in->rd), rn, ops, o2);
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;
        }

        case OP_LSL: case OP_LSR: case OP_ASR: case OP_ROR: {
            static const char *fn2[2][4] = {
                { "vita_lsl",   "vita_lsr",   "vita_asr",   "vita_ror"   },
                { "vita_lsl_c", "vita_lsr_c", "vita_asr_c", "vita_ror_c" },
            };
            int which = in->op == OP_LSL ? 0 : in->op == OP_LSR ? 1
                      : in->op == OP_ASR ? 2 : 3;
            const char *fn_ = fn2[in->sets_flags != 0][which];
            if (in->has_imm) {
                const char *src = src_name(in, in->rm == ARM_NO_REG ? in->rd : in->rm);
                fprintf(f, "    %s = %s(%s, %u);\n", reg_name(in->rd), fn_, src, in->imm);
            } else {
                /* Shift by register: the value is rn and the amount is rm.
                 * The 16-bit form has rn == rd, which is how reading rd here
                 * went unnoticed until lsl.w r0, r1, r2 shifted r0. */
                int val = in->rn != ARM_NO_REG ? in->rn : in->rd;
                fprintf(f, "    %s = %s(%s, %s & 0xFF);\n", reg_name(in->rd),
                        fn_, src_name(in, val), src_name(in, in->rm));
            }
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;
        }

        case OP_MUL:
            /* rn, not rd: the 16-bit form has rn == rd, but MUL.W names three
             * registers, and using rd as the first source gave rd * rm
             * whenever they differed. */
            fprintf(f, "    %s = %s * %s;\n", reg_name(in->rd),
                    src_name(in, in->rn), src_name(in, in->rm));
            if (in->sets_flags) fprintf(f, "    vita_flags_nz(%s);\n", reg_name(in->rd));
            break;

        case OP_CMP: case OP_CMN:
            operand2(o2, sizeof(o2), in);
            fprintf(f, "    { uint32_t _a = %s, _b = %s;\n",
                    rn_s, o2);
            fprintf(f, "      vita_flags_%s(_a, _b, _a %c _b); }\n",
                    in->op == OP_CMP ? "sub" : "add",
                    in->op == OP_CMP ? '-' : '+');
            break;

        case OP_TST: case OP_TEQ:
            operand2_ex(o2, sizeof(o2), in, 1);
            shifter_carry_imm(f, in);
            fprintf(f, "    vita_flags_nz(%s %s %s);\n",
                    rn_s, in->op == OP_TEQ ? "^" : "&", o2);
            break;

        case OP_ADR:
            fprintf(f, "    %s = 0x%08X;\n", reg_name(in->rd),
                    ((in->addr + 4) & ~3u) + in->imm);
            break;

        case OP_LDR: case OP_LDRB: case OP_LDRH: case OP_LDRSB: case OP_LDRSH: {
            if (in->writeback && in->rn == 15) { ok = 0; break; }
            if (!address(addr, sizeof(addr), in)) { ok = 0; trap(f, in, "load addr"); break; }
            /* A literal-pool load has a fixed address, so the value is known at
             * translation time and becomes a constant rather than a memory
             * access. This is also what stops the pool from being mistaken for
             * code later. */
            if (in->rn == 15) {
                uint32_t pc = (in->addr + 4) & ~3u;
                const uint8_t *p = vm_va(img, pc + in->imm, 4);
                if (p) {
                    uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                               | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                    fprintf(f, "    %s = 0x%08X;  /* literal */\n", reg_name(in->rt), v);
                    st->literals++;
                    break;
                }
            }
            const char *rd = in->op == OP_LDR   ? "vita_read32(%s)"
                           : in->op == OP_LDRB  ? "vita_read8(%s)"
                           : in->op == OP_LDRH  ? "vita_read16(%s)"
                           : in->op == OP_LDRSB ? "vita_sxtb(vita_read8(%s))"
                                                : "vita_sxth(vita_read16(%s))";
            fprintf(f, "    %s = ", reg_name(in->rt));
            fprintf(f, rd, addr);
            fprintf(f, ";\n");
            writeback(f, in);
            break;
        }

        case OP_LDRD: case OP_STRD: {
            if (in->rn == ARM_NO_REG || in->rt2 == ARM_NO_REG) { ok = 0; break; }
            /* rt/rt2 are named as variables, so neither may be the PC — there
             * is no `pc` in cpu.h to load into or store from, deliberately. */
            if (in->rt == 15 || in->rt2 == 15) { ok = 0; trap(f, in, "ldrd/strd pc"); break; }
            /* Two adjacent words. The offset is added or subtracted per the U
             * bit, and the second register is always four bytes above the
             * first regardless of direction.
             *
             * Through the shared address(), which is the only thing that knows
             * how to resolve a PC-relative base. Building the string here is
             * what put a bare `pc` in the output — the same mistake VLDR made,
             * in the third place it could be made. */
            char base[64];
            if (!address(base, sizeof(base), in)) { ok = 0; trap(f, in, "ldrd/strd addr"); break; }

            if (in->writeback && in->rn == 15) { ok = 0; break; }
            /* The address is taken once: `ldrd r0, r1, [r0]` is legal, and
             * re-reading the base for the second word read it after the
             * first load had replaced it. */
            fprintf(f, "    { uint32_t _ea = %s;\n", base);
            if (in->op == OP_LDRD) {
                fprintf(f, "      %s = vita_read32(_ea);\n", reg_name(in->rt));
                fprintf(f, "      %s = vita_read32(_ea + 4); }\n", reg_name(in->rt2));
            } else {
                fprintf(f, "      vita_write32(_ea, %s);\n", reg_name(in->rt));
                fprintf(f, "      vita_write32(_ea + 4, %s); }\n", reg_name(in->rt2));
            }
            writeback(f, in);
            break;
        }

        case OP_STR: case OP_STRB: case OP_STRH: {
            if (!address(addr, sizeof(addr), in)) { ok = 0; trap(f, in, "store addr"); break; }
            const char *w = in->op == OP_STR ? "vita_write32"
                          : in->op == OP_STRB ? "vita_write8" : "vita_write16";
            if (in->writeback && in->rn == 15) { ok = 0; break; }
            fprintf(f, "    %s(%s, %s);\n", w, addr, reg_name(in->rt));
            writeback(f, in);
            break;
        }

        case OP_PUSH: {
            /* ARM pushes in descending register order to descending addresses,
             * so the lowest-numbered register ends up at the lowest address.
             * Emitting the stores in ascending order with a pre-decremented sp
             * gives the same layout without simulating the order. */
            int count = 0;
            for (int i = 0; i < 16; i++) if (in->reglist & (1u << i)) count++;
            fprintf(f, "    sp -= %d;\n", count * 4);
            int slot = 0;
            for (int i = 0; i < 16; i++) {
                if (!(in->reglist & (1u << i))) continue;
                /* A register list can name the PC. Its stored value is this
                 * instruction's own address plus four, which is known here —
                 * the same rule as any other PC source, and the reason this
                 * goes through reg_operand rather than reg_name. */
                { char rv[64]; reg_operand(rv, sizeof(rv), i, in);
                  fprintf(f, "    vita_write32(sp + %d, %s);\n", slot * 4, rv); }
                slot++;
            }
            break;
        }

        case OP_POP: {
            int slot = 0;
            for (int i = 0; i < 16; i++) {
                if (!(in->reglist & (1u << i))) continue;
                if (i == 15) { slot++; continue; }   /* PC: this is the return */
                fprintf(f, "    %s = vita_read32(sp + %d);\n", reg_name(i), slot * 4);
                slot++;
            }
            fprintf(f, "    sp += %d;\n", slot * 4);
            if (in->reglist & 0x8000) fprintf(f, "    return;\n");
            break;
        }

        case OP_UADD8: case OP_USUB8: case OP_UADD16: case OP_USUB16: case OP_SEL: {
            const char *fn = in->op == OP_UADD8 ? "vita_uadd8" : in->op == OP_USUB8 ? "vita_usub8"
                           : in->op == OP_UADD16 ? "vita_uadd16" : in->op == OP_USUB16 ? "vita_usub16"
                           : "vita_sel";
            fprintf(f, "    %s = %s(%s, %s);\n", reg_name(in->rd), fn,
                    src_name(in, in->rn), src_name(in, in->rm));
            break;
        }

        case OP_CLZ:
            fprintf(f, "    %s = vita_clz(%s);\n", reg_name(in->rd), src_name(in, in->rm));
            break;

        case OP_MLA: case OP_MLS:
            fprintf(f, "    %s = %s %c %s * %s;\n", reg_name(in->rd), src_name(in, in->rt),
                    in->op == OP_MLA ? '+' : '-', src_name(in, in->rn), src_name(in, in->rm));
            break;

        case OP_UMULL: case OP_SMULL: case OP_UMLAL: case OP_SMLAL: {
            /* Every source is read before either half is written: RdLo and
             * RdHi may name the sources, and the accumulate forms read both
             * halves as the 64-bit addend. */
            int sgn = in->op == OP_SMULL || in->op == OP_SMLAL;
            int acc = in->op == OP_UMLAL || in->op == OP_SMLAL;
            if (sgn)
                fprintf(f, "    { uint64_t _p = (uint64_t)((int64_t)(int32_t)%s * (int64_t)(int32_t)%s);\n",
                        src_name(in, in->rn), src_name(in, in->rm));
            else
                fprintf(f, "    { uint64_t _p = (uint64_t)%s * (uint64_t)%s;\n",
                        src_name(in, in->rn), src_name(in, in->rm));
            if (acc)
                fprintf(f, "      _p += ((uint64_t)%s << 32) | %s;\n",
                        src_name(in, in->rt2), src_name(in, in->rt));
            fprintf(f, "      %s = (uint32_t)_p; %s = (uint32_t)(_p >> 32); }\n",
                    reg_name(in->rt), reg_name(in->rt2));
            break;
        }

        case OP_LDM: case OP_STM: {
            /* The base is captured once, before anything is loaded: a base
             * register that is also in the list must address every slot from
             * its original value. Ascending registers to ascending addresses,
             * starting at rn (IA) or rn - 4n (DB). */
            int count = 0;
            for (int i = 0; i < 16; i++) if (in->reglist & (1u << i)) count++;
            int load = in->op == OP_LDM;
            if (count == 0) { ok = 0; break; }
            /* Writing back to the PC is UNPREDICTABLE, and an assignment to
             * a constant would not compile: it traps, like any PC destination. */
            if (in->rn == 15 && in->writeback) { ok = 0; break; }
            fprintf(f, "    { uint32_t _b = %s;\n", src_name(in, in->rn));
            if (in->decrement) fprintf(f, "      _b -= %d;\n", count * 4);
            int slot = 0;
            for (int i = 0; i < 16; i++) {
                if (!(in->reglist & (1u << i))) continue;
                if (load) {
                    if (i != 15)    /* PC: the return, handled below */
                        fprintf(f, "      %s = vita_read32(_b + %d);\n", reg_name(i), slot * 4);
                } else {
                    char rv[64]; reg_operand(rv, sizeof(rv), i, in);
                    fprintf(f, "      vita_write32(_b + %d, %s);\n", slot * 4, rv);
                }
                slot++;
            }
            /* Writeback leaves the base at the far end of the block: below it
             * for DB (where _b already is), above it for IA. A base that was
             * itself loaded keeps the loaded value. */
            if (in->writeback && !(load && (in->reglist & (1u << in->rn)))) {
                if (in->decrement) fprintf(f, "      %s = _b;\n", reg_name(in->rn));
                else               fprintf(f, "      %s = _b + %d;\n", reg_name(in->rn), count * 4);
            }
            fprintf(f, "    }\n");
            if (load && (in->reglist & 0x8000)) fprintf(f, "    return;\n");
            break;
        }

        case OP_SXTB: case OP_SXTH: case OP_UXTB: case OP_UXTH:
        case OP_SXTB16: case OP_UXTB16: {
            const char *fn3 = in->op == OP_SXTB ? "vita_sxtb"
                            : in->op == OP_SXTH ? "vita_sxth"
                            : in->op == OP_UXTB ? "vita_uxtb"
                            : in->op == OP_UXTH ? "vita_uxth"
                            : in->op == OP_SXTB16 ? "vita_sxtb16" : "vita_uxtb16";
            int b16 = in->op == OP_SXTB16 || in->op == OP_UXTB16;
            char v[96];
            if (in->shift_amt) snprintf(v, sizeof(v), "%s(vita_ror(%s, %u))", fn3,
                                        src_name(in, in->rm), in->shift_amt);
            else               snprintf(v, sizeof(v), "%s(%s)", fn3, src_name(in, in->rm));
            if (in->rn == ARM_NO_REG)
                fprintf(f, "    %s = %s;\n", reg_name(in->rd), v);
            else if (b16)
                fprintf(f, "    %s = vita_add16(%s, %s);\n", reg_name(in->rd), src_name(in, in->rn), v);
            else
                fprintf(f, "    %s = %s + %s;\n", reg_name(in->rd), src_name(in, in->rn), v);
            break;
        }

        case OP_REV: case OP_REV16: case OP_REVSH: case OP_RBIT:
            fprintf(f, "    %s = %s(%s);\n", reg_name(in->rd),
                    in->op == OP_REV ? "vita_rev" : in->op == OP_REV16 ? "vita_rev16"
                    : in->op == OP_REVSH ? "vita_revsh" : "vita_rbit",
                    src_name(in, in->rm));
            break;

        case OP_SMULXY: case OP_SMLAXY:
            if (in->op == OP_SMULXY)
                fprintf(f, "    %s = vita_smulxy(%s, %s, %d);\n", reg_name(in->rd),
                        src_name(in, in->rn), src_name(in, in->rm), in->half_sel);
            else
                fprintf(f, "    %s = vita_smulxy(%s, %s, %d) + %s;\n", reg_name(in->rd),
                        src_name(in, in->rn), src_name(in, in->rm), in->half_sel,
                        src_name(in, in->rt));
            break;

        case OP_SBFX: case OP_UBFX:
            fprintf(f, "    %s = %s(%s, %u, %u);\n", reg_name(in->rd),
                    in->op == OP_SBFX ? "vita_sbfx" : "vita_ubfx",
                    src_name(in, in->rn), in->bf_lsb, in->bf_width);
            break;

        case OP_BFI:
            fprintf(f, "    %s = vita_bfi(%s, %s, %u, %u);\n", reg_name(in->rd),
                    reg_name(in->rd), src_name(in, in->rn), in->bf_lsb, in->bf_width);
            break;

        case OP_BFC:
            fprintf(f, "    %s = vita_bfi(%s, 0, %u, %u);\n", reg_name(in->rd),
                    reg_name(in->rd), in->bf_lsb, in->bf_width);
            break;

        case OP_CBZ: case OP_CBNZ:
            if (fb_has_label(b, in->target))
                fprintf(f, "    if (%s %s 0) goto L_%08X;\n", src_name(in, in->rn),
                        in->op == OP_CBZ ? "==" : "!=", in->target);
            else
                if (fs_has(fs, in->target)) fprintf(f, "    if (%s %s 0) { vita_func_%08X(); return; }\n", src_name(in, in->rn), in->op == OP_CBZ ? "==" : "!=", in->target); else { fprintf(f, "    vita_trap_indirect(0x%08X, 0x%08X);\n", in->addr, in->target); ok = 0; }
            break;

        case OP_B:
            if (fb_has_label(b, in->target)) {
                if (in->cond == ARM_COND_AL) fprintf(f, "    goto L_%08X;\n", in->target);
                else fprintf(f, "    if (vita_cond(%u)) goto L_%08X;\n",
                             in->cond, in->target);
            } else if (in->target < img->seg_vaddr ||
                       in->target >= img->seg_vaddr + img->seg_len) {
                /* Outside the segment entirely. This is not a branch the
                 * program makes — it is a linear sweep decoding data as code,
                 * which happens wherever a literal pool or a table sits inside
                 * .text. Naming it separately keeps it out of the count of
                 * things worth implementing. */
                fprintf(f, "    vita_trap_unimpl(0x%08X, 0x%08X, \"branch out of segment\");\n",
                        in->addr, in->raw);
                note_trap(st, "branch: target off-segment");
            } else {
                /* A branch out of this function's extent is a tail call. */
                if (in->cond == ARM_COND_AL)
                    if (fs_has(fs, in->target)) fprintf(f, "    vita_func_%08X(); return;\n", in->target); else { fprintf(f, "    vita_trap_indirect(0x%08X, 0x%08X);\n", in->addr, in->target); note_trap(st, "branch: target not a function"); }
                else
                    if (fs_has(fs, in->target)) fprintf(f, "    if (vita_cond(%u)) { vita_func_%08X(); return; }\n", in->cond, in->target); else { fprintf(f, "    vita_trap_indirect(0x%08X, 0x%08X);\n", in->addr, in->target); note_trap(st, "branch: target not a function"); }
            }
            break;

        case OP_TBB: case OP_TBH: {
            /* A switch with a proven size becomes one. A case whose block was
             * not collected traps by name; the default is unreachable after the
             * guard, and traps rather than falling into the table bytes. */
            if (!in->table_n) { ok = 0; break; }
            const uint8_t *text = img->data + img->seg_file;
            fprintf(f, "    switch (%s) {\n", src_name(in, in->rm));
            for (uint32_t i = 0; i < in->table_n; i++) {
                uint32_t t = arm_table_target(text, img->seg_vaddr, in, i);
                if (fb_has_label(b, t)) fprintf(f, "    case %u: goto L_%08X;\n", i, t);
                else fprintf(f, "    case %u: vita_trap_indirect(0x%08X, 0x%08X); return;\n", i, in->addr, t);
            }
            fprintf(f, "    default: vita_trap_indirect(0x%08X, 0); return;\n    }\n", in->addr);
            break;
        }

        case OP_BX: case OP_BLX:
            /* An indirect transfer through a register. The destination is a
             * value, so it can only be resolved while running — this is the one
             * place a recompiler genuinely needs a lookup rather than a
             * translation. BX is a tail transfer and does not come back; BLX is
             * a call and does. */
            if (in->rm == ARM_NO_REG) { ok = 0; break; }
            /* `bx lr` is a return, and must be emitted as one. The address in
             * LR is the middle of the caller — never a function entry — so
             * dispatching to it misses every time; the C call stack already
             * holds the way back. This includes `pop {r3}; mov lr, r3; bx lr`,
             * which is how CRT code returns: whatever was loaded into LR, the
             * caller is the C frame below this one. */
            if (in->op == OP_BX && in->rm == 14) { fprintf(f, "    return;\n"); break; }
            /* Through reg_operand, not reg_name. `bx pc` is a real (if odd)
             * instruction, and as a SOURCE the PC is a constant this pass
             * knows — resolving it is correct where naming it would not
             * compile. */
            { char rm[64]; reg_operand(rm, sizeof(rm), in->rm, in);
              if (in->op == OP_BX) fprintf(f, "    vita_dispatch(0x%08X, %s); return;\n", in->addr, rm);
              else                 fprintf(f, "    vita_dispatch(0x%08X, %s);\n", in->addr, rm); }
            break;

        /* --- scalar VFP ---------------------------------------------------
         *
         * Emitted through accessors rather than as direct array indexing, so
         * the single/double aliasing lives in one place. `dp` selects between
         * the float and double forms of everything.
         */
        case OP_VLDR: case OP_VSTR: {
            if (in->vd == ARM_NO_REG) { ok = 0; break; }
            /* Through the shared address(), not a local snprintf. VFP constants
             * live in the same literal pools as integer ones, so `rn == 15` is
             * common here — and building the expression locally emitted a bare
             * `pc`, which does not compile. */
            char a[64];
            if (!address(a, sizeof(a), in)) { ok = 0; trap(f, in, "vldr addr"); break; }

            /* A VFP literal-pool load has a fixed address, so fold it to a
             * constant exactly as the integer path does — same reason, too:
             * it keeps the pool from being read back as code later. */
            if (in->rn == 15) {
                uint32_t pc = (in->addr + 4) & ~3u;
                uint32_t at = in->mem_add ? pc + in->imm : pc - in->imm;
                const uint8_t *p = vm_va(img, at, in->vfp_dp ? 8 : 4);
                if (p && in->op == OP_VLDR) {
                    for (int w = 0; w < (in->vfp_dp ? 2 : 1); w++) {
                        uint32_t v = (uint32_t)p[w*4] | ((uint32_t)p[w*4+1] << 8)
                                   | ((uint32_t)p[w*4+2] << 16) | ((uint32_t)p[w*4+3] << 24);
                        if (in->vfp_dp)
                            fprintf(f, "    vfp_s[%d].u = 0x%08Xu;  /* literal */\n",
                                    in->vd * 2 + w, v);
                        else
                            fprintf(f, "    vfp_setu(%d, 0x%08Xu);  /* literal */\n",
                                    in->vd, v);
                    }
                    st->literals++;
                    break;
                }
            }
            if (in->op == OP_VLDR) {
                if (in->vfp_dp) {
                    fprintf(f, "    vfp_s[%d].u = vita_read32(%s);\n", in->vd * 2, a);
                    fprintf(f, "    vfp_s[%d].u = vita_read32((%s) + 4);\n", in->vd * 2 + 1, a);
                } else {
                    fprintf(f, "    vfp_setu(%d, vita_read32(%s));\n", in->vd, a);
                }
            } else {
                if (in->vfp_dp) {
                    fprintf(f, "    vita_write32(%s, vfp_s[%d].u);\n", a, in->vd * 2);
                    fprintf(f, "    vita_write32((%s) + 4, vfp_s[%d].u);\n", a, in->vd * 2 + 1);
                } else {
                    fprintf(f, "    vita_write32(%s, vfp_getu(%d));\n", a, in->vd);
                }
            }
            break;
        }

        case OP_VADD: case OP_VSUB: case OP_VMUL: case OP_VDIV: {
            if (in->vd == ARM_NO_REG || in->vn == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            char c = in->op == OP_VADD ? '+' : in->op == OP_VSUB ? '-'
                   : in->op == OP_VMUL ? '*' : '/';
            if (in->vfp_dp)
                fprintf(f, "    vfp_setd(%d, vfp_getd(%d) %c vfp_getd(%d));\n",
                        in->vd, in->vn, c, in->vm);
            else
                fprintf(f, "    vfp_setf(%d, vfp_getf(%d) %c vfp_getf(%d));\n",
                        in->vd, in->vn, c, in->vm);
            break;
        }

        case OP_VLDM: case OP_VSTM: {
            if (in->rn == ARM_NO_REG || in->vd == ARM_NO_REG) { ok = 0; break; }
            /* Registers occupy ascending addresses regardless of direction. A
             * decrement-before form (VPUSH) moves the base down by the whole
             * block first, then writes upward from there — the same shape as
             * the integer PUSH, and for the same reason. */
            uint32_t n     = in->imm;
            uint32_t words = in->vfp_dp ? n * 2 : n;
            /* This base is written back, so it is an lvalue and not a
             * source: resolving r15 to a constant here would emit an
             * assignment to one. There is no such addressing mode. */
            if (in->rn == 15) { ok = 0; trap(f, in, "vldm/vstm pc base"); break; }
            const char *rn = reg_name(in->rn);

            if (!in->mem_add) fprintf(f, "    %s -= %u;\n", rn, words * 4);
            for (uint32_t k = 0; k < words; k++) {
                int reg = (in->vfp_dp ? in->vd * 2 : in->vd) + (int)k;
                /* & 63, the whole file: & 31 folded `vstmia r0, {d16-d31}`
                 * onto d0-d15, the same mistake vfp_getd once made. */
                if (in->op == OP_VLDM)
                    fprintf(f, "    vfp_s[%d].u = vita_read32(%s + %u);\n", reg & 63, rn, k * 4);
                else
                    fprintf(f, "    vita_write32(%s + %u, vfp_s[%d].u);\n", rn, k * 4, reg & 63);
            }
            if (in->mem_add && in->writeback)
                fprintf(f, "    %s += %u;\n", rn, words * 4);
            break;
        }

        case OP_VMLA: case OP_VMLS: {
            if (in->vd == ARM_NO_REG || in->vn == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            char c = (in->op == OP_VMLA) ? '+' : '-';
            if (in->vfp_dp)
                fprintf(f, "    vfp_setd(%d, vfp_getd(%d) %c (vfp_getd(%d) * vfp_getd(%d)));\n",
                        in->vd, in->vd, c, in->vn, in->vm);
            else
                fprintf(f, "    vfp_setf(%d, vfp_getf(%d) %c (vfp_getf(%d) * vfp_getf(%d)));\n",
                        in->vd, in->vd, c, in->vn, in->vm);
            break;
        }

        case OP_VNMUL:
            if (in->vd == ARM_NO_REG || in->vn == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp)
                fprintf(f, "    vfp_setd(%d, -(vfp_getd(%d) * vfp_getd(%d)));\n", in->vd, in->vn, in->vm);
            else
                fprintf(f, "    vfp_setf(%d, -(vfp_getf(%d) * vfp_getf(%d)));\n", in->vd, in->vn, in->vm);
            break;

        case OP_VNMLA: case OP_VNMLS: {
            if (in->vd == ARM_NO_REG || in->vn == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            char c = (in->op == OP_VNMLA) ? '-' : '+';
            if (in->vfp_dp)
                fprintf(f, "    vfp_setd(%d, -vfp_getd(%d) %c (vfp_getd(%d) * vfp_getd(%d)));\n",
                        in->vd, in->vd, c, in->vn, in->vm);
            else
                fprintf(f, "    vfp_setf(%d, -vfp_getf(%d) %c (vfp_getf(%d) * vfp_getf(%d)));\n",
                        in->vd, in->vd, c, in->vn, in->vm);
            break;
        }

        /* --- Advanced SIMD ---------------------------------------------------
         *
         * One statement per D register: a Q operation is the same thing done
         * to the pair d, d+1 (and n, n+1, m, m+1). Sources are read into
         * temporaries first, because the destination may also be a source. */
        case OP_NEON_ADD: case OP_NEON_SUB: case OP_NEON_LOGIC: {
            int regs = in->neon_q ? 2 : 1;
            for (int k = 0; k < regs; k++) {
                int d = in->vd + k, n = in->vn + k, m = in->vm + k;
                fprintf(f, "    { uint64_t _n = neon_getd(%d), _m = neon_getd(%d), _d = neon_getd(%d);\n", n, m, d);
                const char *expr = "_d";
                char buf[96];
                if (in->op == OP_NEON_ADD) { snprintf(buf, sizeof(buf), "neon_add(_n, _m, %d)", in->neon_size); expr = buf; }
                else if (in->op == OP_NEON_SUB) { snprintf(buf, sizeof(buf), "neon_sub(_n, _m, %d)", in->neon_size); expr = buf; }
                else switch (in->neon_fn) {
                    case NL_AND: expr = "_n & _m"; break;
                    case NL_BIC: expr = "_n & ~_m"; break;
                    case NL_ORR: expr = "_n | _m"; break;
                    case NL_ORN: expr = "_n | ~_m"; break;
                    case NL_EOR: expr = "_n ^ _m"; break;
                    case NL_BSL: expr = "(_d & _n) | (~_d & _m)"; break;   /* d is the selector */
                    case NL_BIT: expr = "(_n & _m) | (_d & ~_m)"; break;   /* insert where m set */
                    case NL_BIF: expr = "(_d & _m) | (_n & ~_m)"; break;   /* insert where m clear */
                    default: break;
                }
                fprintf(f, "      (void)_d; neon_setd(%d, %s); }\n", d, expr);
            }
            break;
        }

        /* Float. Each helper does the whole D word, two F32 lanes, under the
         * standard FPSCR rules (see neon.h). The scalar form reads its lane
         * before either half of a Q destination is written, because the
         * destination may be the very D register the scalar lives in. */
        case OP_NEON_FADD: case OP_NEON_FSUB: case OP_NEON_FMUL:
        case OP_NEON_FMLA: case OP_NEON_FMLS: {
            int regs = in->neon_q ? 2 : 1;
            for (int k = 0; k < regs; k++) {
                int d = in->vd + k, n = in->vn + k, m = in->vm + k;
                if (in->op == OP_NEON_FMLA || in->op == OP_NEON_FMLS)
                    fprintf(f, "    neon_setd(%d, neon_%s(neon_getd(%d), neon_getd(%d), neon_getd(%d)));\n",
                            d, in->op == OP_NEON_FMLA ? "fmla" : "fmls", d, n, m);
                else
                    fprintf(f, "    neon_setd(%d, neon_%s(neon_getd(%d), neon_getd(%d)));\n",
                            d, in->op == OP_NEON_FADD ? "fadd" : in->op == OP_NEON_FSUB ? "fsub" : "fmul",
                            n, m);
            }
            break;
        }

        case OP_NEON_FMUL_SCALAR: case OP_NEON_FMLA_SCALAR: case OP_NEON_FMLS_SCALAR: {
            int regs = in->neon_q ? 2 : 1;
            fprintf(f, "    { uint64_t _s = neon_dup(neon_get_lane(neon_getd(%d), 2, %u, 1), 2);\n",
                    in->vm, (unsigned)in->imm);
            for (int k = 0; k < regs; k++) {
                int d = in->vd + k, n = in->vn + k;
                if (in->op == OP_NEON_FMLA_SCALAR)
                    fprintf(f, "      neon_setd(%d, neon_fmla(neon_getd(%d), neon_getd(%d), _s));\n", d, d, n);
                else if (in->op == OP_NEON_FMLS_SCALAR)
                    fprintf(f, "      neon_setd(%d, neon_fmls(neon_getd(%d), neon_getd(%d), _s));\n", d, d, n);
                else
                    fprintf(f, "      neon_setd(%d, neon_fmul(neon_getd(%d), _s));\n", d, n);
            }
            fprintf(f, "    }\n");
            break;
        }

        /* Max, min and compares: one helper per lane pair, as above. The
         * compare helpers return all-ones or zero in each lane. */
        case OP_NEON_FMAX: case OP_NEON_FMIN:
        case OP_NEON_FCEQ: case OP_NEON_FCGE: case OP_NEON_FCGT: {
            int regs = in->neon_q ? 2 : 1;
            const char *fn = in->op == OP_NEON_FMAX ? "fmax" : in->op == OP_NEON_FMIN ? "fmin"
                           : in->op == OP_NEON_FCEQ ? "fceq" : in->op == OP_NEON_FCGE ? "fcge" : "fcgt";
            for (int k = 0; k < regs; k++)
                fprintf(f, "    neon_setd(%d, neon_%s(neon_getd(%d), neon_getd(%d)));\n",
                        in->vd + k, fn, in->vn + k, in->vm + k);
            break;
        }

        case OP_NEON_FABS: case OP_NEON_FNEG: {
            int regs = in->neon_q ? 2 : 1;
            for (int k = 0; k < regs; k++)
                fprintf(f, "    neon_setd(%d, neon_%s(neon_getd(%d)));\n",
                        in->vd + k, in->op == OP_NEON_FABS ? "fabs" : "fneg", in->vm + k);
            break;
        }

        /* Compares with zero, and integer VABS and VNEG. Each D half reads only
         * its own half of vm, so the result may overwrite vm (vclt.s16 d1, d1,
         * #0). The compare kind is imm: 0 GT, 1 GE, 2 EQ, 3 LE, 4 LT. LE and LT
         * put the zero on the left, so x <= 0 is 0 >= x. The F32 forms use the
         * register compares with a zero operand, so a NaN is false and a
         * denormal reads as zero, as in VCGT.F32 and friends. */
        case OP_NEON_CMPZ: case OP_NEON_FCMPZ: {
            static const char *const ifmt[5] = {
                "neon_cmp(neon_getd(%d), 0, %d, 0, 0)", "neon_cmp(neon_getd(%d), 0, %d, 0, 1)",
                "neon_ceq(neon_getd(%d), 0, %d)",
                "neon_cmp(0, neon_getd(%d), %d, 0, 1)", "neon_cmp(0, neon_getd(%d), %d, 0, 0)",
            };
            static const char *const ffmt[5] = {
                "neon_fcgt(neon_getd(%d), 0)", "neon_fcge(neon_getd(%d), 0)",
                "neon_fceq(neon_getd(%d), 0)",
                "neon_fcge(0, neon_getd(%d))", "neon_fcgt(0, neon_getd(%d))",
            };
            int regs = in->neon_q ? 2 : 1;
            for (int k = 0; k < regs; k++) {
                fprintf(f, "    neon_setd(%d, ", in->vd + k);
                if (in->op == OP_NEON_FCMPZ)
                    fprintf(f, ffmt[in->imm], in->vm + k);
                else
                    fprintf(f, ifmt[in->imm], in->vm + k, in->neon_size);
                fprintf(f, ");\n");
            }
            break;
        }
        case OP_NEON_ABS: case OP_NEON_NEG: {
            int regs = in->neon_q ? 2 : 1;
            for (int k = 0; k < regs; k++) {
                if (in->op == OP_NEON_ABS)
                    fprintf(f, "    neon_setd(%d, neon_abs(neon_getd(%d), %d));\n",
                            in->vd + k, in->vm + k, in->neon_size);
                else
                    fprintf(f, "    neon_setd(%d, neon_sub(0, neon_getd(%d), %d));\n",
                            in->vd + k, in->vm + k, in->neon_size);
            }
            break;
        }

        /* The integer signedness is the vfp_unsigned flag, as in the VFP forms. */
        case OP_NEON_FCVT_TO_INT: case OP_NEON_FCVT_FROM_INT: {
            int regs = in->neon_q ? 2 : 1;
            for (int k = 0; k < regs; k++)
                fprintf(f, "    neon_setd(%d, neon_%s(neon_getd(%d), %d));\n",
                        in->vd + k, in->op == OP_NEON_FCVT_TO_INT ? "f2i" : "i2f",
                        in->vm + k, in->vfp_unsigned);
            break;
        }

        /* Integer multiply, saturating add and subtract, and shifts by
         * immediate. Each helper does the whole D word (neon.h). The Q halves
         * are independent (the registers of a Q are aligned pairs, so no half
         * reads what the other half has written), so the order does not matter.
         * The accumulate forms read the destination before they write it. */
        case OP_NEON_MUL: case OP_NEON_MLA: case OP_NEON_MLS: {
            int regs = in->neon_q ? 2 : 1;
            for (int k = 0; k < regs; k++) {
                int d = in->vd + k, n = in->vn + k, m = in->vm + k;
                if (in->op == OP_NEON_MUL)
                    fprintf(f, "    neon_setd(%d, neon_mul(neon_getd(%d), neon_getd(%d), %d));\n",
                            d, n, m, in->neon_size);
                else
                    fprintf(f, "    neon_setd(%d, neon_%s(neon_getd(%d), neon_getd(%d), neon_getd(%d), %d));\n",
                            d, in->op == OP_NEON_MLA ? "mla" : "mls", d, n, m, in->neon_size);
            }
            break;
        }

        case OP_NEON_QADD: case OP_NEON_QSUB: {
            int regs = in->neon_q ? 2 : 1;
            for (int k = 0; k < regs; k++)
                fprintf(f, "    neon_setd(%d, neon_qop(neon_getd(%d), neon_getd(%d), %d, %d, %d));\n",
                        in->vd + k, in->vn + k, in->vm + k, in->neon_size,
                        in->vfp_unsigned, in->op == OP_NEON_QSUB);
            break;
        }

        /* Integer compares, max/min and absolute difference (added to vd for
         * VABA). Each D half reads only its own half of vd, vn and vm, so an
         * overlapping destination (vaba q0, q0, q1) reads its old value. */
        case OP_NEON_CGT: case OP_NEON_CGE: case OP_NEON_MAX: case OP_NEON_MIN:
        case OP_NEON_ABD: case OP_NEON_ABA: {
            int regs = in->neon_q ? 2 : 1;
            for (int k = 0; k < regs; k++) {
                int d = in->vd + k, n = in->vn + k, m = in->vm + k;
                int sz = in->neon_size, u = in->vfp_unsigned;
                if (in->op == OP_NEON_CGT || in->op == OP_NEON_CGE)
                    fprintf(f, "    neon_setd(%d, neon_cmp(neon_getd(%d), neon_getd(%d), %d, %d, %d));\n",
                            d, n, m, sz, u, in->op == OP_NEON_CGE);
                else if (in->op == OP_NEON_MAX || in->op == OP_NEON_MIN)
                    fprintf(f, "    neon_setd(%d, neon_minmax(neon_getd(%d), neon_getd(%d), %d, %d, %d));\n",
                            d, n, m, sz, u, in->op == OP_NEON_MIN);
                else if (in->op == OP_NEON_ABD)
                    fprintf(f, "    neon_setd(%d, neon_abd(neon_getd(%d), neon_getd(%d), %d, %d));\n",
                            d, n, m, sz, u);
                else
                    fprintf(f, "    neon_setd(%d, neon_add(neon_getd(%d), neon_abd(neon_getd(%d), neon_getd(%d), %d, %d), %d));\n",
                            d, d, n, m, sz, u, sz);
            }
            break;
        }

        /* Shift by register. The value is vm and the amounts are vn, so the
         * helper takes them in that order. Each D half reads only its own half
         * of vm and vn, and Q registers are even-aligned, so a Q store to vd
         * never lands on a later input of the same instruction. */
        case OP_NEON_SHL_REG: case OP_NEON_QSHL_REG:
        case OP_NEON_RSHL_REG: case OP_NEON_QRSHL_REG: {
            int regs = in->neon_q ? 2 : 1;
            int rnd = in->op == OP_NEON_RSHL_REG || in->op == OP_NEON_QRSHL_REG;
            int sat = in->op == OP_NEON_QSHL_REG || in->op == OP_NEON_QRSHL_REG;
            for (int k = 0; k < regs; k++)
                fprintf(f, "    neon_setd(%d, neon_shl_reg(neon_getd(%d), neon_getd(%d), %d, %d, %d, %d));\n",
                        in->vd + k, in->vm + k, in->vn + k, in->neon_size,
                        in->vfp_unsigned, rnd, sat);
            break;
        }

        /* Halving add and subtract, and pairwise max and min. A Q halving op is
         * two D halves, each reading its own half of vn and vm. Half 0 stores
         * to an even D and half 1 reads only odd Ds, so the first store never
         * reaches a later input. VPMAX and VPMIN are D only, and neon_pminmax
         * takes its inputs by value, so vpmax d0, d0, d1 reads both first. */
        case OP_NEON_HADD: case OP_NEON_RHADD: case OP_NEON_HSUB: {
            int regs = in->neon_q ? 2 : 1;
            int kind = in->op == OP_NEON_HADD ? 0 : in->op == OP_NEON_RHADD ? 1 : 2;
            for (int k = 0; k < regs; k++)
                fprintf(f, "    neon_setd(%d, neon_halve(neon_getd(%d), neon_getd(%d), %d, %d, %d));\n",
                        in->vd + k, in->vn + k, in->vm + k, in->neon_size,
                        in->vfp_unsigned, kind);
            break;
        }

        case OP_NEON_PMAX: case OP_NEON_PMIN:
            fprintf(f, "    neon_setd(%d, neon_pminmax(neon_getd(%d), neon_getd(%d), %d, %d, %d));\n",
                    in->vd, in->vn, in->vm, in->neon_size, in->vfp_unsigned,
                    in->op == OP_NEON_PMIN);
            break;

        case OP_NEON_SHL: case OP_NEON_SHR: case OP_NEON_SRA: {
            int regs = in->neon_q ? 2 : 1;
            for (int k = 0; k < regs; k++) {
                int d = in->vd + k, m = in->vm + k;
                if (in->op == OP_NEON_SHL)
                    fprintf(f, "    neon_setd(%d, neon_shl(neon_getd(%d), %u, %d));\n",
                            d, m, (unsigned)in->imm, in->neon_size);
                else if (in->op == OP_NEON_SHR)
                    fprintf(f, "    neon_setd(%d, neon_shr(neon_getd(%d), %u, %d, %d));\n",
                            d, m, (unsigned)in->imm, in->neon_size, in->vfp_unsigned);
                else
                    fprintf(f, "    neon_setd(%d, neon_sra(neon_getd(%d), neon_getd(%d), %u, %d, %d));\n",
                            d, d, m, (unsigned)in->imm, in->neon_size, in->vfp_unsigned);
            }
            break;
        }

        case OP_NEON_IMM: {
            int regs = in->neon_q ? 2 : 1;
            for (int k = 0; k < regs; k++) {
                int d = in->vd + k;
                unsigned long long v = (unsigned long long)in->imm64;
                switch (in->neon_fn) {
                    case NL_MOV: fprintf(f, "    neon_setd(%d, 0x%016llXull);\n", d, v); break;
                    case NL_MVN: fprintf(f, "    neon_setd(%d, 0x%016llXull);\n", d, ~v); break;
                    case NL_ORR: fprintf(f, "    neon_setd(%d, neon_getd(%d) | 0x%016llXull);\n", d, d, v); break;
                    case NL_BIC: fprintf(f, "    neon_setd(%d, neon_getd(%d) & 0x%016llXull);\n", d, d, ~v); break;
                    default: ok = 0; break;
                }
            }
            break;
        }

        case OP_NEON_CEQ: case OP_NEON_TST: {
            int regs = in->neon_q ? 2 : 1;
            for (int k = 0; k < regs; k++)
                fprintf(f, "    neon_setd(%d, neon_%s(neon_getd(%d), neon_getd(%d), %d));\n",
                        in->vd + k, in->op == OP_NEON_CEQ ? "ceq" : "tst",
                        in->vn + k, in->vm + k, in->neon_size);
            break;
        }

        case OP_NEON_PADD:
            fprintf(f, "    neon_setd(%d, neon_padd(neon_getd(%d), neon_getd(%d), %d));\n",
                    in->vd, in->vn, in->vm, in->neon_size);
            break;

        case OP_NEON_MOVN:
            fprintf(f, "    neon_setd(%d, neon_movn(neon_getd(%d), neon_getd(%d), %d));\n",
                    in->vd, in->vm, in->vm + 1, in->neon_size);
            break;

        /* Widening (neon.h). Each helper takes its sources as arguments, which
         * are read before the result is stored, so a Q destination that is also
         * a source reads its old value (vsubw q0, q0, d2; vmull.s16 q0, d0,
         * d1). MLAL and MLSL read the old vd as the accumulator, and a W form
         * reads its Q vn in full. */
        case OP_NEON_ADDL: case OP_NEON_SUBL: case OP_NEON_ADDW: case OP_NEON_SUBW: {
            int wide = in->op == OP_NEON_ADDW || in->op == OP_NEON_SUBW;
            int sub = in->op == OP_NEON_SUBL || in->op == OP_NEON_SUBW;
            /* n is the D word for the L forms and the Q pair for the W forms. */
            if (wide)
                fprintf(f, "    { uint64_t n[2] = {neon_getd(%d), neon_getd(%d)}, r[2];",
                        in->vn, in->vn + 1);
            else
                fprintf(f, "    { uint64_t n[2] = {neon_getd(%d), 0}, r[2];", in->vn);
            fprintf(f, " neon_wide_addsub(r, n, neon_getd(%d), %d, %d, %d, %d);",
                    in->vm, in->neon_size, in->vfp_unsigned, wide, sub);
            fprintf(f, " neon_setd(%d, r[0]); neon_setd(%d, r[1]); }\n", in->vd, in->vd + 1);
            break;
        }

        case OP_NEON_MULL: {
            fprintf(f, "    { uint64_t r[2];");
            fprintf(f, " neon_wide_mul(r, neon_getd(%d), neon_getd(%d), %d, %d);",
                    in->vn, in->vm, in->neon_size, in->vfp_unsigned);
            fprintf(f, " neon_setd(%d, r[0]); neon_setd(%d, r[1]); }\n", in->vd, in->vd + 1);
            break;
        }

        case OP_NEON_MLAL: case OP_NEON_MLSL: {
            fprintf(f, "    { uint64_t r[2] = {neon_getd(%d), neon_getd(%d)};",
                    in->vd, in->vd + 1);
            fprintf(f, " neon_wide_mac(r, neon_getd(%d), neon_getd(%d), %d, %d, %d);",
                    in->vn, in->vm, in->neon_size, in->vfp_unsigned,
                    in->op == OP_NEON_MLSL);
            fprintf(f, " neon_setd(%d, r[0]); neon_setd(%d, r[1]); }\n", in->vd, in->vd + 1);
            break;
        }

        /* VSHLL and VMOVL: the D source widened and shifted into the Q pair. */
        case OP_NEON_SHLL:
            fprintf(f, "    { uint64_t r[2]; neon_widen(r, neon_getd(%d), %d, %u, %d);",
                    in->vm, in->neon_size, (unsigned)in->imm, in->vfp_unsigned);
            fprintf(f, " neon_setd(%d, r[0]); neon_setd(%d, r[1]); }\n", in->vd, in->vd + 1);
            break;

        /* Byte and element shuffles, and table lookup (neon.h). The Q forms
         * are the two D words of each pair, and the helpers work on them as
         * the architecture defines for each op. */
        case OP_NEON_EXT: {
            int regs = in->neon_q ? 2 : 1;
            fprintf(f, "    { uint64_t a[2] = {0, 0}, b[2] = {0, 0};");
            emit_neon_load(f, "a", in->vn, regs);
            emit_neon_load(f, "b", in->vm, regs);
            fprintf(f, " neon_ext(a, b, %d, %u);", regs, (unsigned)in->imm);
            emit_neon_store(f, "a", in->vd, regs);
            fprintf(f, " }\n");
            break;
        }

        case OP_NEON_REV: {
            int regs = in->neon_q ? 2 : 1;
            fprintf(f, "    { uint64_t a[2] = {0, 0};");
            emit_neon_load(f, "a", in->vm, regs);
            fprintf(f, " neon_rev(a, %d, %u, %d);", regs, (unsigned)in->imm, in->neon_size);
            emit_neon_store(f, "a", in->vd, regs);
            fprintf(f, " }\n");
            break;
        }

        /* Two outputs: d takes a and m takes b. Both are read first. */
        case OP_NEON_SWP: case OP_NEON_TRN: case OP_NEON_UZP: case OP_NEON_ZIP: {
            int regs = in->neon_q ? 2 : 1;
            fprintf(f, "    { uint64_t a[2] = {0, 0}, b[2] = {0, 0};");
            emit_neon_load(f, "a", in->vd, regs);
            emit_neon_load(f, "b", in->vm, regs);
            if (in->op == OP_NEON_SWP)
                fprintf(f, " neon_swp(a, b, %d);", regs);
            else
                fprintf(f, " neon_%s(a, b, %d, %d);",
                        in->op == OP_NEON_TRN ? "trn" : in->op == OP_NEON_UZP ? "uzp" : "zip",
                        regs, in->neon_size);
            emit_neon_store(f, "a", in->vd, regs);
            emit_neon_store(f, "b", in->vm, regs);
            fprintf(f, " }\n");
            break;
        }

        /* The lane is read before any of the destination is written, so vd
         * may equal vm. */
        case OP_NEON_DUP_LANE: {
            int regs = in->neon_q ? 2 : 1;
            fprintf(f, "    { uint64_t _s = neon_dup_lane(neon_getd(%d), %d, %u);",
                    in->vm, in->neon_size, (unsigned)in->imm);
            for (int k = 0; k < regs; k++)
                fprintf(f, " neon_setd(%d, _s);", in->vd + k);
            fprintf(f, " }\n");
            break;
        }

        /* The table is read in full, and the index and the old vd too, before
         * the destination is written. VTBX passes the old vd as the value for
         * an out-of-range index; VTBL passes zero. */
        case OP_NEON_TBL: case OP_NEON_TBX: {
            int cnt = (int)in->imm;
            fprintf(f, "    { uint64_t t[4] = {0, 0, 0, 0};");
            for (int k = 0; k < cnt; k++)
                fprintf(f, " t[%d] = neon_getd(%d);", k, in->vn + k);
            fprintf(f, " uint64_t ix = neon_getd(%d);", in->vm);
            if (in->op == OP_NEON_TBX)
                fprintf(f, " neon_setd(%d, neon_tbl(t, %d, ix, neon_getd(%d))); }\n",
                        in->vd, cnt, in->vd);
            else
                fprintf(f, " neon_setd(%d, neon_tbl(t, %d, ix, 0)); }\n", in->vd, cnt);
            break;
        }

        case OP_NEON_DUP: {
            int regs = in->neon_q ? 2 : 1;
            for (int k = 0; k < regs; k++)
                fprintf(f, "    neon_setd(%d, neon_dup(%s, %d));\n",
                        in->vd + k, reg_name(in->rt), in->neon_size);
            break;
        }

        case OP_NEON_TO_SCALAR:
            fprintf(f, "    neon_setd(%d, neon_set_lane(neon_getd(%d), %s, %d, %u));\n",
                    in->vd, in->vd, reg_name(in->rt), in->neon_size, in->imm);
            break;

        case OP_NEON_FROM_SCALAR:
            fprintf(f, "    %s = neon_get_lane(neon_getd(%d), %d, %u, %d);\n",
                    reg_name(in->rt), in->vn, in->neon_size, in->imm, in->vfp_unsigned);
            break;

        case OP_NEON_VLD1: case OP_NEON_VST1: {
            /* Whole D registers at ascending addresses from the base, read
             * once so a base that is also the increment register is used
             * before either changes. */
            const char *rn = reg_name(in->rn);
            fprintf(f, "    { uint32_t _a = %s;\n", rn);
            for (uint32_t k = 0; k < in->imm; k++) {
                int d = in->vd + (int)k;
                if (in->op == OP_NEON_VLD1)
                    fprintf(f, "      vfp_s[%d].u = vita_read32(_a + %u); vfp_s[%d].u = vita_read32(_a + %u);\n",
                            d * 2, k * 8, d * 2 + 1, k * 8 + 4);
                else
                    fprintf(f, "      vita_write32(_a + %u, vfp_s[%d].u); vita_write32(_a + %u, vfp_s[%d].u);\n",
                            k * 8, d * 2, k * 8 + 4, d * 2 + 1);
            }
            if (in->writeback) {
                if (in->rm != ARM_NO_REG) fprintf(f, "      %s = _a + %s;\n", rn, src_name(in, in->rm));
                else                      fprintf(f, "      %s = _a + %u;\n", rn, in->imm * 8);
            }
            fprintf(f, "    }\n");
            break;
        }

        case OP_NEON_VLDN: case OP_NEON_VSTN: {
            /* VLD2-4/VST2-4: the whole run of n*words D words is moved as one
             * block, then neon_deinterleave/neon_interleave map it to and from
             * the member registers. Member k is D register vd + k*inc onward,
             * words of them. The base is read once, as for VLD1, and every
             * register is gathered before the first store. */
            int words = in->neon_q ? 2 : 1, n = (int)in->imm, total = n * words;
            const char *rn = reg_name(in->rn);
            fprintf(f, "    { uint32_t _a = %s; uint64_t _m[8], _r[8];\n", rn);
            if (in->op == OP_NEON_VLDN) {
                for (int i = 0; i < total; i++)
                    fprintf(f, "      _m[%d] = vita_read32(_a + %d); _m[%d] |= (uint64_t)vita_read32(_a + %d) << 32;\n",
                            i, i * 8, i, i * 8 + 4);
                fprintf(f, "      neon_deinterleave(_m, _r, %d, %d, %u);\n", n, words, in->neon_size);
                for (int k = 0; k < n; k++)
                    for (int w = 0; w < words; w++)
                        fprintf(f, "      neon_setd(%d, _r[%d]);\n",
                                in->vd + k * in->neon_inc + w, k * words + w);
            } else {
                for (int k = 0; k < n; k++)
                    for (int w = 0; w < words; w++)
                        fprintf(f, "      _r[%d] = neon_getd(%d);\n",
                                k * words + w, in->vd + k * in->neon_inc + w);
                fprintf(f, "      neon_interleave(_r, _m, %d, %d, %u);\n", n, words, in->neon_size);
                for (int i = 0; i < total; i++)
                    fprintf(f, "      vita_write32(_a + %d, (uint32_t)_m[%d]); vita_write32(_a + %d, (uint32_t)(_m[%d] >> 32));\n",
                            i * 8, i, i * 8 + 4, i);
            }
            if (in->writeback) {
                if (in->rm != ARM_NO_REG) fprintf(f, "      %s = _a + %s;\n", rn, src_name(in, in->rm));
                else                      fprintf(f, "      %s = _a + %d;\n", rn, total * 8);
            }
            fprintf(f, "    }\n");
            break;
        }

        /* Narrowing shifts and the saturating narrows (neon.h). Both source D
         * words are read as arguments before neon_setd stores Dd, so Dd may be
         * one of the halves of Qm. */
        case OP_NEON_NARROW_SHR: case OP_NEON_NARROW_RSHR:
            fprintf(f, "    neon_setd(%d, neon_shrn(neon_getd(%d), neon_getd(%d), %u, %d, %d));\n",
                    in->vd, in->vm, in->vm + 1, (unsigned)in->imm, in->neon_size,
                    in->op == OP_NEON_NARROW_RSHR);
            break;

        case OP_NEON_NARROW_QSHR: case OP_NEON_NARROW_QRSHR:
        case OP_NEON_NARROW_QSHRU: case OP_NEON_NARROW_QRSHRU: {
            /* The source is signed for every form but the unsigned-to-unsigned
             * ones; the result is unsigned for VQSHRUN/VQRSHRUN and the U forms. */
            int rnd = in->op == OP_NEON_NARROW_QRSHR || in->op == OP_NEON_NARROW_QRSHRU;
            int uns = in->op == OP_NEON_NARROW_QSHRU || in->op == OP_NEON_NARROW_QRSHRU;
            int src_signed = uns ? 1 : !in->vfp_unsigned;
            int dst_unsigned = uns ? 1 : in->vfp_unsigned;
            fprintf(f, "    neon_setd(%d, neon_qshrn(neon_getd(%d), neon_getd(%d), %u, %d, %d, %d, %d));\n",
                    in->vd, in->vm, in->vm + 1, (unsigned)in->imm, in->neon_size,
                    rnd, src_signed, dst_unsigned);
            break;
        }

        case OP_VMOV_V_RR: case OP_VMOV_RR_V: {
            int w = in->vfp_dp ? in->vm * 2 : in->vm;     /* first word of the pair */
            if (in->op == OP_VMOV_V_RR)
                fprintf(f, "    vfp_s[%d].u = %s; vfp_s[%d].u = %s;\n",
                        w, reg_name(in->rt), w + 1, reg_name(in->rt2));
            else
                fprintf(f, "    %s = vfp_s[%d].u; %s = vfp_s[%d].u;\n",
                        reg_name(in->rt), w, reg_name(in->rt2), w + 1);
            break;
        }

        case OP_VCVT_F2F:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp) fprintf(f, "    vfp_setf(%d, (float)vfp_getd(%d));\n", in->vd, in->vm);
            else            fprintf(f, "    vfp_setd(%d, (double)vfp_getf(%d));\n", in->vd, in->vm);
            break;

        case OP_VMOV_IMM:
            if (in->vd == ARM_NO_REG) { ok = 0; break; }
            /* Bits, not a float literal: the decoder already expanded the
             * constant exactly as the hardware does. A double's low word is
             * always zero. */
            if (in->vfp_dp)
                fprintf(f, "    vfp_s[%d].u = 0u; vfp_s[%d].u = 0x%08Xu;\n",
                        in->vd * 2, in->vd * 2 + 1, in->imm);
            else
                fprintf(f, "    vfp_setu(%d, 0x%08Xu);\n", in->vd, in->imm);
            break;

        case OP_VMOV:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp) fprintf(f, "    vfp_setd(%d, vfp_getd(%d));\n", in->vd, in->vm);
            else            fprintf(f, "    vfp_setu(%d, vfp_getu(%d));\n", in->vd, in->vm);
            break;

        case OP_VABS:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            /* Clearing the sign bit rather than calling fabs: it is the
             * architecture's definition, and it preserves NaN payloads where
             * a library call need not. */
            if (in->vfp_dp) fprintf(f, "    vfp_setd(%d, vfp_getd(%d) < 0.0 ? -vfp_getd(%d) : vfp_getd(%d));\n",
                                    in->vd, in->vm, in->vm, in->vm);
            else            fprintf(f, "    vfp_setu(%d, vfp_getu(%d) & 0x7FFFFFFFu);\n", in->vd, in->vm);
            break;

        case OP_VNEG:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp) fprintf(f, "    vfp_setd(%d, -vfp_getd(%d));\n", in->vd, in->vm);
            else            fprintf(f, "    vfp_setu(%d, vfp_getu(%d) ^ 0x80000000u);\n", in->vd, in->vm);
            break;

        case OP_VSQRT:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            if (in->vfp_dp) fprintf(f, "    vfp_setd(%d, sqrt(vfp_getd(%d)));\n", in->vd, in->vm);
            else            fprintf(f, "    vfp_setf(%d, (float)sqrt((double)vfp_getf(%d)));\n", in->vd, in->vm);
            break;

        case OP_VCMP:
            if (in->vd == ARM_NO_REG) { ok = 0; break; }
            /* Writes FPSCR, not the integer flags. A later VMRS moves them. */
            if (in->vm == ARM_NO_REG) {
                if (in->vfp_dp) fprintf(f, "    vfp_cmp_f64(vfp_getd(%d), 0.0);\n", in->vd);
                else            fprintf(f, "    vfp_cmp_f32(vfp_getf(%d), 0.0f);\n", in->vd);
            } else {
                if (in->vfp_dp) fprintf(f, "    vfp_cmp_f64(vfp_getd(%d), vfp_getd(%d));\n", in->vd, in->vm);
                else            fprintf(f, "    vfp_cmp_f32(vfp_getf(%d), vfp_getf(%d));\n", in->vd, in->vm);
            }
            break;

        case OP_VMRS:
            fprintf(f, "    vfp_mrs_apsr();\n");
            break;

        case OP_VMOV_TO_V:
            if (in->rt == ARM_NO_REG || in->vn == ARM_NO_REG) { ok = 0; break; }
            fprintf(f, "    vfp_setu(%d, %s);\n", in->vn, reg_name(in->rt));
            break;

        case OP_VMOV_TO_C:
            if (in->rt == ARM_NO_REG || in->vn == ARM_NO_REG) { ok = 0; break; }
            fprintf(f, "    %s = vfp_getu(%d);\n", reg_name(in->rt), in->vn);
            break;

        case OP_VCVT_I2F:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            fprintf(f, "    vfp_setf(%d, (float)(%s)vfp_getu(%d));\n",
                    in->vd, in->vfp_unsigned ? "uint32_t" : "int32_t", in->vm);
            break;

        case OP_VCVT_F2I:
            if (in->vd == ARM_NO_REG || in->vm == ARM_NO_REG) { ok = 0; break; }
            /* Toward zero, which is what a C cast does — the only rounding
             * mode claimed by the decoder for exactly that reason. */
            fprintf(f, "    vfp_setu(%d, (uint32_t)(%s)vfp_getf(%d));\n",
                    in->vd, in->vfp_unsigned ? "uint32_t" : "int32_t", in->vm);
            break;

        case OP_LDREX: case OP_STREX: {
            if (in->rn == ARM_NO_REG || in->rn == 15 || in->rt == 15 ||
                (in->xsize == 8 && (in->rt2 == ARM_NO_REG || in->rt2 == 15)) ||
                (in->op == OP_STREX && (in->rd == ARM_NO_REG || in->rd == 15))) {
                ok = 0; break;
            }
            char a[48];
            if (in->imm) snprintf(a, sizeof(a), "%s + %uu", src_name(in, in->rn), in->imm);
            else         snprintf(a, sizeof(a), "%s", src_name(in, in->rn));
            if (in->op == OP_LDREX) {
                if (in->xsize == 8)
                    fprintf(f, "    { uint64_t _x = vita_ldrex(%s, 8); %s = (uint32_t)_x; %s = (uint32_t)(_x >> 32); }\n",
                            a, reg_name(in->rt), reg_name(in->rt2));
                else
                    fprintf(f, "    %s = (uint32_t)vita_ldrex(%s, %u);\n", reg_name(in->rt), a, in->xsize);
            } else {
                if (in->xsize == 8)
                    fprintf(f, "    %s = vita_strex(%s, ((uint64_t)%s << 32) | %s, 8);\n",
                            reg_name(in->rd), a, src_name(in, in->rt2), src_name(in, in->rt));
                else
                    fprintf(f, "    %s = vita_strex(%s, %s, %u);\n",
                            reg_name(in->rd), a, src_name(in, in->rt), in->xsize);
            }
            break;
        }

        case OP_CLREX:
            fprintf(f, "    vita_clrex();\n");
            break;

        case OP_DMB:
            fprintf(f, "    vita_barrier();\n");
            break;

        case OP_NOP:
            fprintf(f, "    /* nop */\n");
            break;

        case OP_IT:
            /* The IT instruction itself emits nothing; its effect is that the
             * following instructions carry a condition, which the decoder has
             * already attached to them. */
            fprintf(f, "    /* it %s */\n", arm_cond_name(in->cond));
            break;

        default:
            ok = 0;
            break;
    }

    /* Control-flow classes the operand decoder does not cover are handled by
     * class rather than by op, so a call is still a call even when its exact
     * encoding was not decoded. */
    if (!ok) {
        if (in->cls == A_CALL && in->has_target && fs_has(fs, in->target)) {
            fprintf(f, "    vita_func_%08X();\n", in->target);
            ok = 1;
        } else if (in->cls == A_CALL && in->has_target) {
            fprintf(f, "    vita_trap_indirect(0x%08X, 0x%08X);\n", in->addr, in->target);
        } else if (in->cls == A_RETURN) {
            fprintf(f, "    return;\n");
            ok = 1;
        } else if (in->cls == A_CALL || in->cls == A_INDIRECT) {
            fprintf(f, "    vita_trap_indirect(0x%08X, 0);\n", in->addr);
            note_trap(st, "indirect transfer");
        } else if (in->cls == A_SIMD) {
            trap(f, in, "simd");
            note_trap(st, "simd/vfp");
        } else {
            trap(f, in, in->mnemonic);
            note_trap(st, in->mnemonic);
        }
    }

    if (guarded) fprintf(f, "    }\n");
    return ok;
}

/* --- the whole module -------------------------------------------------------- */

int em_emit(const vm_image *img, const vm_module *mod, const nid_db *db,
            const vf_result *disc, FILE *f, const char *impdir,
            uint32_t limit, emit_stats *st) {
    memset(st, 0, sizeof(*st));

    uint32_t n_funcs = disc->count;
    if (limit && limit < n_funcs) n_funcs = limit;

    /* Every address that is a defined symbol in this output, sorted for lookup.
     * Under --limit the un-emitted ones still get trapping stubs, so they are
     * genuinely defined and belong in the set. */
    funcset fs;
    fs.n = disc->count;
    fs.v = (uint32_t *)malloc((fs.n ? fs.n : 1) * sizeof(uint32_t));
    if (!fs.v) return 1;
    for (uint32_t i = 0; i < disc->count; i++) fs.v[i] = disc->funcs[i].addr;
    qsort(fs.v, fs.n, sizeof(uint32_t), fs_cmp);

    fprintf(f, "/* Generated by armrecomp. Do not edit.\n");
    fprintf(f, " *\n");
    fprintf(f, " * module: %s\n", mod->info.name);
    fprintf(f, " * %u functions\n", disc->count);
    fprintf(f, " */\n\n");
    fprintf(f, "#include \"vitarecomp/recomp_rt.h\"\n");
    fprintf(f, "#include \"vitarecomp/dispatch.h\"\n\n");

    /* Declare every import the module could call. Emitting these unconditionally
     * — rather than only the ones a limited emit happens to reach — keeps the
     * declaration set stable, so adding functions to the emit does not change
     * which symbols exist. */
    if (mod->stub_count) {
        fprintf(f, "/* firmware imports */\n");
        for (uint32_t i = 0; i < mod->stub_count; i++) {
            char sym[96];
            hle_symbol(sym, sizeof(sym), db, &mod->stubs[i]);
            fprintf(f, "void %s(void);\n", sym);
        }
        fprintf(f, "\n");
    }

    /* Forward declarations: the call graph has cycles, so every function must
     * be visible before any body is emitted. */
    for (uint32_t i = 0; i < disc->count; i++)
        fprintf(f, "void vita_func_%08X(void);\n", disc->funcs[i].addr);
    fprintf(f, "\n");

    /* A limited emit still references functions it did not emit, so those get
     * a stub that traps. Without this the output would not link, and a silently
     * missing function is exactly the failure this project refuses. */
    if (n_funcs < disc->count) {
        fprintf(f, "/* Not emitted under --limit; present so the output links. */\n");
        for (uint32_t i = n_funcs; i < disc->count; i++)
            fprintf(f, "void vita_func_%08X(void) { vita_trap_unimpl(0x%08X, 0, \"not emitted\"); }\n",
                    disc->funcs[i].addr, disc->funcs[i].addr);
        fprintf(f, "\n");
    }

    for (uint32_t i = 0; i < n_funcs; i++) {
        const vf_func *fn = &disc->funcs[i];
        fbody b;
        memset(&b, 0, sizeof(b));
        collect(img, fn, &b);

        fprintf(f, "/* ---------------------------------------------------------------\n");
        fprintf(f, " * vita_func_%08X  --  %u instructions, %u bytes%s\n",
                fn->addr, b.n, fn->end - fn->addr,
                fn->from_heuristic ? ", discovered by shape (heuristic)" : "");
        fprintf(f, " * ------------------------------------------------------------- */\n");
        fprintf(f, "void vita_func_%08X(void) {\n", fn->addr);

        for (uint32_t k = 0; k < b.n; k++) {
            const arm_insn *in = &b.v[k];
            if (fb_has_label(&b, in->addr))
                fprintf(f, "L_%08X:\n", in->addr);

            fprintf(f, "    /* %08X  %-16s */\n", in->addr, in->mnemonic);
            if (emit_insn(img, mod, db, &fs, &b, in, f, st)) st->translated++;
            else                               st->trapped++;
            st->insns++;
        }

        fprintf(f, "}\n\n");
        st->funcs++;

        st->branches_into_it += b.into_it;
        free(b.v);
        free(b.labels);
        free(b.in_it);
    }

    /* --- the HLE defaults ----------------------------------------------------
     *
     * One default implementation per import, each of which traps by name. This
     * is what makes the HLE incrementally implementable: the generated C links
     * from the first build, and every unimplemented firmware call announces
     * exactly which function the game wanted rather than failing to link or,
     * worse, silently returning zero.
     *
     * One FILE per import, for the reason in emit.h: at one archive member per
     * function, writing a real implementation and relinking is the entire
     * override procedure. No list to keep in sync, and no generated code to
     * hand-edit.
     */
    if (impdir) {
        for (uint32_t i = 0; i < mod->stub_count; i++) {
            const vm_stub *s = &mod->stubs[i];
            char sym[96];
            hle_symbol(sym, sizeof(sym), db, s);
            const char *lib = nid_lib_name(db, s->lib_nid);
            const char *fn  = nid_func_name(db, s->lib_nid, s->func_nid);

            char path[1200];
            snprintf(path, sizeof(path), "%s/%s.c", impdir, sym);
            FILE *one = fopen(path, "w");
            if (!one) continue;

            fprintf(one, "/* Generated by armrecomp. Do not edit.\n");
            fprintf(one, " *\n");
            fprintf(one, " * %s::%s  --  stub 0x%08X in module '%s'.\n",
                    lib ? lib : "?", fn ? fn : "(unresolved)", s->addr,
                    mod->info.name);
            fprintf(one, " *\n");
            fprintf(one, " * The default: trap by name. To implement it, define this symbol\n");
            fprintf(one, " * in the runtime and relink -- this file is one member of a static\n");
            fprintf(one, " * library, so the linker stops extracting it. Nothing to delete.\n");
            fprintf(one, " */\n\n");
            fprintf(one, "#include \"vitarecomp/recomp_rt.h\"\n\n");
            /* The name goes into the trap, not just the comment: at run time
             * the NID alone is a hash, and the whole reason imports are bound
             * is so a gap says which firmware function the game wanted. */
            if (fn)
                fprintf(one,
                        "void %s(void) { vita_trap_import(0x%08X, 0x%08X, \"%s::%s\"); }\n",
                        sym, s->addr, s->func_nid, lib ? lib : "?", fn);
            else
                fprintf(one,
                        "void %s(void) { vita_trap_import(0x%08X, 0x%08X, 0); }\n",
                        sym, s->addr, s->func_nid);
            fclose(one);
            st->imports_used++;
        }
    }

    /* The dispatch table: every function AND every import stub, sorted by
     * guest address, so an indirect transfer can find either at run time.
     * Functions are emitted for ALL discovered addresses — including any not
     * emitted under --limit, which have trapping stubs — because a dispatch
     * that misses should report "we never translated that" rather than "that
     * address is not a function".
     *
     * The stubs belong in the same table because a transfer to one is a
     * firmware call. Imports bind at emit time on a direct BL, but a module
     * reaches firmware through pointers too — `module_start` makes its very
     * first firmware call that way — and those never pass through the BL path.
     * Without this the dispatcher reports the game's first C++ runtime call as
     * an unresolved address, which names the wrong problem: the code is not
     * missing, it is firmware.
     *
     * One merged table rather than a second lookup: the entries have the same
     * shape already (guest address -> void(void)), both lists arrive sorted,
     * and merging keeps vita_dispatch a single binary search.
     */
    fprintf(f, "/* ---------------------------------------------------------------\n");
    fprintf(f, " * dispatch table -- %u functions + %u import stubs,\n",
            disc->count, mod->stub_count);
    fprintf(f, " * sorted by guest address\n");
    fprintf(f, " * ------------------------------------------------------------- */\n");
    fprintf(f, "static const vita_dispatch_entry vita_functions[] = {\n");
    {
        uint32_t fi = 0, si = 0;
        while (fi < fs.n || si < mod->stub_count) {
            int take_stub;
            if (fi >= fs.n)                            take_stub = 1;
            else if (si >= mod->stub_count)            take_stub = 0;
            else if (mod->stubs[si].addr < fs.v[fi])   take_stub = 1;
            else if (mod->stubs[si].addr > fs.v[fi])   take_stub = 0;
            else {
                /* Both lists claim this address. The stub wins: being in the
                 * import table is what the address MEANS, and the bytes
                 * discovery found there are the placeholder body the loader
                 * was supposed to overwrite. Translating that placeholder and
                 * returning from it is the failure this whole layer exists to
                 * prevent. */
                take_stub = 1;
                fi++;
                st->stub_shadowed++;
            }

            if (take_stub) {
                char sym[96];
                hle_symbol(sym, sizeof(sym), db, &mod->stubs[si]);
                fprintf(f, "    { 0x%08Xu, %s },\n", mod->stubs[si].addr, sym);
                si++;
            } else {
                fprintf(f, "    { 0x%08Xu, vita_func_%08X },\n", fs.v[fi], fs.v[fi]);
                fi++;
            }
        }
    }
    fprintf(f, "};\n\n");
    fprintf(f, "void vita_register_functions(void) {\n");
    fprintf(f, "    vita_dispatch_init(vita_functions,\n");
    fprintf(f, "        (uint32_t)(sizeof(vita_functions) / sizeof(vita_functions[0])));\n");
    fprintf(f, "}\n");

    free(fs.v);
    return 0;
}



