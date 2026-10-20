/* decode.c — ARMv7-A instruction decoding.
 *
 * Two instruction sets sharing one address space. The single most important
 * output here is WIDTH: get it wrong once and every subsequent instruction in
 * the run is decoded from the wrong offset, producing a long stretch of
 * plausible-looking nonsense rather than an obvious failure.
 *
 * Thumb width is determined by the top five bits of the first halfword, which
 * is a rule rather than a table, so it is written as one.
 */

#include "decode.h"

#include <string.h>

const char *arm_class_name(arm_class c) {
    switch (c) {
        case A_UNKNOWN:  return "unknown";
        case A_UNDEF:    return "undefined";
        case A_ALU:      return "alu";
        case A_LOAD:     return "load";
        case A_STORE:    return "store";
        case A_LOADM:    return "loadm";
        case A_STOREM:   return "storem";
        case A_BRANCH:   return "branch";
        case A_CALL:     return "call";
        case A_RETURN:   return "return";
        case A_INDIRECT: return "indirect";
        case A_SIMD:     return "simd";
        case A_SYS:      return "sys";
        case A_NOP:      return "nop";
        default:         return "?";
    }
}

const char *arm_cond_name(uint8_t cond) {
    static const char *n[16] = {
        "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
        "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"
    };
    return n[cond & 0xF];
}

static uint32_t sign_extend(uint32_t v, int bits) {
    uint32_t m = 1u << (bits - 1);
    return (v ^ m) - m;
}

static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void set(arm_insn *o, arm_class c, const char *m) {
    o->cls = c;
    o->mnemonic = m;
}

/* --- Thumb, 16-bit ---------------------------------------------------------- */

/* The 16 data-processing operations of the 010000 group, in encoding order. */
static const arm_op t16_dp[16] = {
    OP_AND, OP_EOR, OP_LSL, OP_LSR, OP_ASR, OP_ADC, OP_SBC, OP_ROR,
    OP_TST, OP_RSB, OP_CMP, OP_CMN, OP_ORR, OP_MUL, OP_BIC, OP_MVN
};

/* The eight register-offset load/store forms, in encoding order. */
static const arm_op t16_ldst_reg[8] = {
    OP_STR, OP_STRH, OP_STRB, OP_LDRSB, OP_LDR, OP_LDRH, OP_LDRB, OP_LDRSH
};

static void decode_t16(uint16_t h, arm_insn *o) {
    o->raw = h;

    /* --- fully decoded operand forms -------------------------------------- */
    /*
     * These are ordered by encoding, and every one sets `op` so the emitter can
     * translate it. Anything falling past them keeps its class but leaves
     * op == OP_NONE and becomes a trap rather than a guess.
     *
     * Flag setting is the detail most easily got wrong here: 16-bit Thumb data
     * processing sets NZCV *implicitly*, with no S bit to read. The exception
     * is inside an IT block, where the same encodings do not. The decoder
     * records the instruction's own behaviour; honouring IT is the emitter's
     * job, because only it knows the surrounding context.
     */

    /* 000 op(2) imm5 rm rd — shift by immediate; op==11 is add/sub register */
    if ((h & 0xE000) == 0x0000 && (h & 0x1800) != 0x1800) {
        static const arm_op sh[3] = { OP_LSL, OP_LSR, OP_ASR };
        o->op = sh[(h >> 11) & 3];
        o->rd = h & 7;
        o->rm = (h >> 3) & 7;
        o->imm = (h >> 6) & 0x1F;
        /* LSR/ASR encode a shift by 32 as 0. LSL #0 is a real zero: it is
         * MOVS, which sets N and Z and leaves C. */
        if (o->imm == 0 && o->op != OP_LSL) o->imm = 32;
        o->has_imm = 1;
        o->sets_flags = 1;
        set(o, A_ALU, "shift");
        return;
    }

    /* 00011 I op rm/imm3 rn rd */
    if ((h & 0xF800) == 0x1800) {
        o->op = (h & 0x0200) ? OP_SUB : OP_ADD;
        o->rd = h & 7;
        o->rn = (h >> 3) & 7;
        if (h & 0x0400) { o->imm = (h >> 6) & 7; o->has_imm = 1; }
        else            { o->rm  = (h >> 6) & 7; }
        o->sets_flags = 1;
        set(o, A_ALU, "add/sub");
        return;
    }

    /* 001 op(2) rd imm8 */
    if ((h & 0xE000) == 0x2000) {
        static const arm_op ops[4] = { OP_MOV, OP_CMP, OP_ADD, OP_SUB };
        o->op = ops[(h >> 11) & 3];
        o->rd = o->rn = (h >> 8) & 7;
        o->imm = h & 0xFF;
        o->has_imm = 1;
        o->sets_flags = 1;
        set(o, A_ALU, "alu imm8");
        return;
    }

    /* 010000 op(4) rm rd — register data processing */
    if ((h & 0xFC00) == 0x4000) {
        o->op = t16_dp[(h >> 6) & 0xF];
        o->rd = o->rn = h & 7;
        o->rm = (h >> 3) & 7;
        o->sets_flags = 1;
        /* RSB in this group is the "negate" form: rd = 0 - rm. */
        if (o->op == OP_RSB) { o->imm = 0; o->has_imm = 1; o->rn = o->rm; }
        set(o, A_ALU, "alu reg");
        return;
    }

    /* 01000111 L Rm — interworking branches. These are the only 16-bit
     * instructions that can change instruction set, so they are handled before
     * the general data-processing case that would otherwise swallow them.
     *
     * The mask is 0xFF00, NOT 0xFF80: bit 7 is the L bit that distinguishes BX
     * from BLX, so masking it out excludes every BLX from this case. It then
     * falls through to "add/cmp/mov hi" and decodes as an ALU operation —
     * which means every indirect *call* becomes invisible, and phase 4 would
     * silently never discover the functions reached through them. */
    if ((h & 0xFF00) == 0x4700) {
        int rm = (h >> 3) & 0xF;
        if (h & 0x0080) {
            set(o, A_CALL, "blx");
            o->switches_mode = 1;
        } else if (rm == 14) {
            set(o, A_RETURN, "bx lr");
        } else {
            set(o, A_INDIRECT, "bx");
            o->switches_mode = 1;
        }
        o->op = (h & 0x0080) ? OP_BLX : OP_BX;
        o->rm = rm;
        return;
    }

    /* 010001 op(2) DN rm(4) rd(3) — high-register forms. These do NOT set
     * flags, unlike almost everything else in 16-bit Thumb; CMP is the sole
     * exception because comparing is all it does. */
    if ((h & 0xFC00) == 0x4400) {
        static const arm_op ops[3] = { OP_ADD, OP_CMP, OP_MOV };
        int which = (h >> 8) & 3;
        if (which < 3) {
            int rd = ((h >> 4) & 8) | (h & 7);
            int rm = (h >> 3) & 0xF;

            /* Writing r15 is a BRANCH, not a register write. `MOV pc, rN` is
             * an indirect jump and `ADD pc, rN` a computed one; treating either
             * as arithmetic emits an assignment where control flow belongs, and
             * the recompiled function then runs straight on past a jump it
             * should have taken. `MOV pc, lr` is the plain return form. */
            if (rd == 15 && which != 1 /* CMP never writes rd */) {
                o->rm = rm;
                if (which == 2 && rm == 14) { o->op = OP_BX; set(o, A_RETURN, "mov pc,lr"); }
                else                        { o->op = OP_BX; set(o, A_INDIRECT, "mov/add pc"); }
                return;
            }

            o->op = ops[which];
            o->rd = o->rn = (int8_t)rd;
            o->rm = (int8_t)rm;
            o->sets_flags = (o->op == OP_CMP);
            set(o, A_ALU, "add/cmp/mov hi");
            return;
        }
    }

    /* 01001 rt imm8 — LDR from the literal pool. The address is PC-relative
     * with PC aligned down to a word boundary, which is why the emitter can
     * resolve these to a constant at translation time. */
    if ((h & 0xF800) == 0x4800) {
        o->op = OP_LDR;
        o->rt = (h >> 8) & 7;
        o->rn = 15;
        o->imm = (uint32_t)(h & 0xFF) * 4;
        o->has_imm = 1;
        o->mem_add = 1;
        set(o, A_LOAD, "ldr literal");
        return;
    }

    /* 0101 op(3) rm rn rt — register-offset load/store */
    if ((h & 0xF000) == 0x5000) {
        o->op = t16_ldst_reg[(h >> 9) & 7];
        o->rt = h & 7;
        o->rn = (h >> 3) & 7;
        o->rm = (h >> 6) & 7;
        o->mem_add = 1;
        set(o, (o->op >= OP_STR) ? A_STORE : A_LOAD, "ldr/str reg");
        return;
    }

    /* 011 B L imm5 rn rt — word/byte immediate. The immediate is scaled by the
     * access size, so a byte form is NOT the word form with a smaller range. */
    if ((h & 0xE000) == 0x6000) {
        int byte = (h & 0x1000) != 0;
        int load = (h & 0x0800) != 0;
        o->op = byte ? (load ? OP_LDRB : OP_STRB) : (load ? OP_LDR : OP_STR);
        o->rt = h & 7;
        o->rn = (h >> 3) & 7;
        o->imm = (uint32_t)((h >> 6) & 0x1F) * (byte ? 1u : 4u);
        o->has_imm = 1;
        o->mem_add = 1;
        set(o, load ? A_LOAD : A_STORE, "ldr/str imm");
        return;
    }

    /* 1000 L imm5 rn rt — halfword, scaled by 2 */
    if ((h & 0xF000) == 0x8000) {
        int load = (h & 0x0800) != 0;
        o->op = load ? OP_LDRH : OP_STRH;
        o->rt = h & 7;
        o->rn = (h >> 3) & 7;
        o->imm = (uint32_t)((h >> 6) & 0x1F) * 2u;
        o->has_imm = 1;
        o->mem_add = 1;
        set(o, load ? A_LOAD : A_STORE, "ldrh/strh");
        return;
    }

    /* 1001 L rt imm8 — SP-relative, scaled by 4 */
    if ((h & 0xF000) == 0x9000) {
        int load = (h & 0x0800) != 0;
        o->op = load ? OP_LDR : OP_STR;
        o->rt = (h >> 8) & 7;
        o->rn = 13;
        o->imm = (uint32_t)(h & 0xFF) * 4u;
        o->has_imm = 1;
        o->mem_add = 1;
        set(o, load ? A_LOAD : A_STORE, "ldr/str sp");
        return;
    }

    /* 1010 SP rd imm8 — ADR, or ADD rd, sp, #imm */
    if ((h & 0xF000) == 0xA000) {
        o->rd = (h >> 8) & 7;
        o->imm = (uint32_t)(h & 0xFF) * 4u;
        o->has_imm = 1;
        if (h & 0x0800) { o->op = OP_ADD; o->rn = 13; set(o, A_ALU, "add sp"); }
        else            { o->op = OP_ADR; o->rn = 15; set(o, A_ALU, "adr"); }
        return;
    }

    /* 1100 L rn rlist — LDM / STM */
    if ((h & 0xF000) == 0xC000) {
        int load = (h & 0x0800) != 0;
        o->rn = (h >> 8) & 7;
        o->reglist = h & 0xFF;
        /* LDM writes back unless the base is in the list (then the loaded
         * value wins); STM always writes back. */
        o->writeback = load ? !(o->reglist & (1u << o->rn)) : 1;
        o->op = load ? OP_LDM : OP_STM;
        set(o, load ? A_LOADM : A_STOREM, "ldm/stm");
        return;
    }

    switch (h >> 12) {
        case 0x0: case 0x1:                     /* shift / add / sub          */
        case 0x2: case 0x3:                     /* mov / cmp / add / sub imm  */
            set(o, A_ALU, "alu");
            return;

        case 0x4:
            if ((h & 0xFC00) == 0x4000) { set(o, A_ALU, "alu"); return; }
            if ((h & 0xFC00) == 0x4400) { set(o, A_ALU, "add/cmp/mov hi"); return; }
            set(o, A_LOAD, "ldr literal");      /* 01001 — LDR Rt,[pc,#imm]   */
            return;

        case 0x5:                               /* load/store register offset */
            set(o, (h & 0x0800) ? A_LOAD : A_STORE, "ldr/str reg");
            return;

        case 0x6: case 0x7: case 0x8: case 0x9: /* load/store immediate       */
            set(o, (h & 0x0800) ? A_LOAD : A_STORE, "ldr/str imm");
            return;

        case 0xA:                               /* adr / add sp               */
            set(o, A_ALU, "adr/add sp");
            return;

        case 0xB:                               /* miscellaneous              */
            if ((h & 0xFF00) == 0xBF00) {
                /* IT and hints share this space. IT is the one that matters:
                 * it makes up to the next four instructions conditional, which
                 * the emitter has to honour. It is identified here so that it
                 * is never silently treated as a NOP. */
                if (h & 0x000F) {
                    o->op = OP_IT;
                    o->cond = (h >> 4) & 0xF;
                    o->it_mask = h & 0xF;
                    set(o, A_SYS, "it");
                } else {
                    o->op = OP_NOP;
                    set(o, A_NOP, "nop");
                }
                return;
            }
            if ((h & 0xFF00) == 0xB000) {       /* ADD/SUB SP, #imm7*4        */
                o->op = (h & 0x0080) ? OP_SUB : OP_ADD;
                o->rd = o->rn = 13;
                o->imm = (uint32_t)(h & 0x7F) * 4u;
                o->has_imm = 1;
                set(o, A_ALU, "add/sub sp");
                return;
            }
            if ((h & 0xFF00) == 0xB200) {       /* SXTH/SXTB/UXTH/UXTB        */
                static const arm_op ex[4] = { OP_SXTH, OP_SXTB, OP_UXTH, OP_UXTB };
                o->op = ex[(h >> 6) & 3];
                o->rd = h & 7;
                o->rm = (h >> 3) & 7;
                set(o, A_ALU, "extend");
                return;
            }
            /* REV / REV16 / REVSH by bits 7:6; 10 is UNDEFINED. This once
             * took everything but REV16 as REV, so REVSH byte-swapped without
             * sign-extending. */
            if ((h & 0xFF00) == 0xBA00 && ((h >> 6) & 3) != 2) {
                static const arm_op rv[4] = { OP_REV, OP_REV16, OP_NONE, OP_REVSH };
                static const char *const nm[4] = { "rev", "rev16", "?", "revsh" };
                o->op = rv[(h >> 6) & 3];
                o->rd = h & 7;
                o->rm = (h >> 3) & 7;
                set(o, A_ALU, nm[(h >> 6) & 3]);
                return;
            }
            if ((h & 0xF600) == 0xB400) {
                int load = (h & 0x0800) != 0;
                /* Bit 8 is the extra register: LR for PUSH, PC for POP. A POP
                 * that restores PC is a return, and must not also be emitted as
                 * an ordinary register load. */
                o->reglist = h & 0xFF;
                o->op = load ? OP_POP : OP_PUSH;
                if (load && (h & 0x0100)) {
                    o->reglist |= 0x8000;       /* PC */
                    set(o, A_RETURN, "pop {..,pc}");
                    return;
                }
                if (!load && (h & 0x0100)) o->reglist |= 0x4000;  /* LR */
                set(o, load ? A_LOADM : A_STOREM, load ? "pop" : "push");
                return;
            }
            if ((h & 0xF500) == 0xB100) {       /* CBZ / CBNZ                 */
                o->op = (h & 0x0800) ? OP_CBNZ : OP_CBZ;
                o->rn = h & 7;
                o->conditional = 1;
                o->has_target = 1;
                o->target = o->addr + 4
                          + ((((uint32_t)(h >> 9) & 1) << 6)
                          |  (((uint32_t)(h >> 3) & 0x1F) << 1));
                set(o, A_BRANCH, "cbz/cbnz");
                return;
            }
            set(o, A_ALU, "misc");
            return;

        case 0xC:                               /* stm / ldm                  */
            set(o, (h & 0x0800) ? A_LOADM : A_STOREM, "ldm/stm");
            return;

        case 0xD: {                             /* conditional branch, svc    */
            int cond = (h >> 8) & 0xF;
            if (cond == 0xF) { o->op = OP_SVC; set(o, A_SYS, "svc"); return; }
            if (cond == 0xE) { set(o, A_UNDEF, "udf"); return; }
            o->op = OP_B;
            o->cond = (uint8_t)cond;
            set(o, A_BRANCH, "b<cond>");
            o->conditional = 1;
            o->has_target = 1;
            o->target = o->addr + 4 + sign_extend(h & 0xFF, 8) * 2;
            return;
        }

        case 0xE:                               /* unconditional branch       */
            o->op = OP_B;
            o->cond = ARM_COND_AL;
            set(o, A_BRANCH, "b");
            o->has_target = 1;
            o->target = o->addr + 4 + sign_extend(h & 0x7FF, 11) * 2;
            return;

        default:
            set(o, A_UNKNOWN, "?");
            return;
    }
}

/* --- scalar VFP ------------------------------------------------------------- */
/*
 * The coprocessor space holds both Advanced SIMD and scalar VFP, and on this
 * platform the split is roughly 18/82 in favour of the scalar side. This
 * function claims the scalar part; anything it does not recognise falls back to
 * being reported as SIMD, so the boundary stays honest rather than optimistic.
 *
 * Register numbering is the fiddly part, and it is fiddly in opposite
 * directions for the two precisions. A single-precision register puts its LOW
 * bit in the separate D/N/M flag and its high four bits in the main field; a
 * double-precision register does the reverse. Getting it backwards yields a
 * valid register number that is simply the wrong one — no error, just a program
 * that reads floats it never wrote.
 */

static int vfp_sreg(int field, int extra) { return (field << 1) | extra; }
static int vfp_dreg(int field, int extra) { return (extra << 4) | field; }

static int decode_vfp(uint16_t h1, uint16_t h2, arm_insn *o) {
    int coproc = (h2 >> 8) & 0xF;
    int dp     = (coproc == 11);
    if (coproc != 10 && coproc != 11) return 0;   /* not scalar VFP */

    o->vfp_dp = (uint8_t)dp;

    /* --- extension register load/store: VLDR / VSTR ----------------------- */
    if ((h1 & 0xFE00) == 0xEC00) {
        int P = (h1 >> 8) & 1, U = (h1 >> 7) & 1;
        int D = (h1 >> 6) & 1, W = (h1 >> 5) & 1, L = (h1 >> 4) & 1;
        int vd = (h2 >> 12) & 0xF;

        /* P=1, W=0 is the plain offset form: VLDR/VSTR. Everything else in
         * this encoding is a register-LIST transfer — VLDM/VSTM, and the
         * SP-relative VPUSH/VPOP that are by far the most common of them.
         *
         * These are worth claiming rather than skipping: they were 293 of the
         * instructions still reported as "SIMD", which is more than a fifth of
         * that bucket, and they do no arithmetic at all. imm8 counts the
         * registers transferred — one per register for single precision, two
         * for double. */
        /* The list forms are exactly W == 1:
         *   P=1 W=0  VLDR/VSTR, a single register at an offset
         *   P=0 W=1  VLDMIA
         *   P=1 W=1  VLDMDB / VPUSH / VPOP
         *   P=0 W=0  64-bit core<->VFP transfer, not claimed here
         *
         * An earlier revision tested `P != W`, which is also true for P=1 W=0 —
         * so it captured the plain VLDR/VSTR forms that were already working
         * and re-decoded them as register lists. Translation went DOWN. Worth
         * recording: a change that claims more encodings can still be a
         * regression if it claims the wrong ones. */
        /* ...and also P=0 U=1 W=0: VLDMIA/VSTMIA with no writeback, which
         * the C++ unwinder uses to save and restore the whole file. P=0 U=0
         * W=0 is the 64-bit transfer below. */
        if (W || (!P && U)) {
            int vd_first = dp ? vfp_dreg(vd, D) : vfp_sreg(vd, D);
            uint32_t n = (uint32_t)(h2 & 0xFF);
            /* An odd count with sz=1 is FLDMX/FSTMX: n doubles plus a format
             * word the base steps over. Without writeback the extra word is
             * invisible; with it the step is wrong, so that form is left. */
            if (dp && (n & 1) && W) return 0;
            if (dp) n /= 2;
            if (n == 0 || n > 32) return 0;
            if (vd_first + (int)n > 32) return 0;

            o->op = L ? OP_VLDM : OP_VSTM;
            o->rn = h1 & 0xF;
            o->vd = (int8_t)vd_first;
            o->imm = n;                       /* register count */
            o->has_imm = 1;
            o->writeback = (uint8_t)W;
            o->mem_add = (uint8_t)U;          /* 0 = decrement before (VPUSH) */
            set(o, L ? A_LOADM : A_STOREM, L ? "vldm/vpop" : "vstm/vpush");
            return 1;
        }
        /* P=0 W=0: two core registers to or from a D register (sz=1) or a
         * pair of singles. GCC moves every double argument this way under
         * the soft-float ABI: `vmov d0, r0, r1` on the way in, `vmov r0, r1,
         * d0` on the way out. */
        if (!P && (h1 & 0xFFE0) == 0xEC40 && (h2 & 0x00D0) == 0x0010) {
            int M = (h2 >> 5) & 1, vm = h2 & 0xF;
            int to_core = (h1 >> 4) & 1;
            o->rt  = (h2 >> 12) & 0xF;
            o->rt2 = h1 & 0xF;
            o->vm  = (int8_t)(dp ? vfp_dreg(vm, M) : vfp_sreg(vm, M));
            if (o->rt == 15 || o->rt2 == 15 || o->rt == 13 || o->rt2 == 13) return 0;
            if (to_core && o->rt == o->rt2) return 0;          /* UNPREDICTABLE */
            if (!dp && o->vm == 31) return 0;
            o->op = to_core ? OP_VMOV_RR_V : OP_VMOV_V_RR;
            set(o, A_ALU, to_core ? "vmov r,r,v" : "vmov v,r,r");
            return 1;
        }
        if (!P || W) return 0;

        o->op  = L ? OP_VLDR : OP_VSTR;
        o->rn  = h1 & 0xF;
        o->vd  = (int8_t)(dp ? vfp_dreg(vd, D) : vfp_sreg(vd, D));
        o->imm = (uint32_t)(h2 & 0xFF) * 4u;
        o->has_imm = 1;
        o->mem_add = (uint8_t)U;
        set(o, L ? A_LOAD : A_STORE, L ? "vldr" : "vstr");
        return 1;
    }

    if ((h1 & 0xEF00) != 0xEE00) return 0;        /* not VFP data processing */

    int D  = (h1 >> 6) & 1;
    int vn = h1 & 0xF;
    int vd = (h2 >> 12) & 0xF;
    int N  = (h2 >> 7) & 1;
    int M  = (h2 >> 5) & 1;
    int vm = h2 & 0xF;
    int op = (h2 >> 6) & 1;
    int opc1 = (h1 >> 4) & 0xB;                   /* bits 7, 5:4 */

    o->vd = (int8_t)(dp ? vfp_dreg(vd, D) : vfp_sreg(vd, D));
    o->vn = (int8_t)(dp ? vfp_dreg(vn, N) : vfp_sreg(vn, N));
    o->vm = (int8_t)(dp ? vfp_dreg(vm, M) : vfp_sreg(vm, M));

    /* --- VMRS: the float flags to the integer ones ------------------------ */
    if ((h1 & 0xFFF0) == 0xEEF0 && (h2 & 0x0F10) == 0x0A10) {
        o->op = OP_VMRS;
        o->rt = (h2 >> 12) & 0xF;
        set(o, A_SYS, "vmrs");
        return 1;
    }

    /* --- VMOV between a core register and a single-precision register ----- */
    if ((h1 & 0xFFE0) == 0xEE00 && (h2 & 0x0F7F) == 0x0A10) {
        int to_vfp = ((h1 >> 4) & 1) == 0;
        o->op = to_vfp ? OP_VMOV_TO_V : OP_VMOV_TO_C;
        o->rt = (h2 >> 12) & 0xF;
        o->vn = (int8_t)vfp_sreg(vn, N);          /* always single here */
        o->vfp_dp = 0;
        set(o, A_ALU, to_vfp ? "vmov s,r" : "vmov r,s");
        return 1;
    }

    /* Bit 4 set is the core<->extension transfer group, never data
     * processing. Without this test `vmov.32 r0, d2[0]` — memchr's result —
     * fell into the switch below and came out as VNMLS: silently, a float
     * multiply-subtract in place of a register move. */
    if (h2 & 0x0010) {
        if (!dp || (h2 & 0x000F)) return 0;
        int rt = (h2 >> 12) & 0xF;
        if (rt == 15 || rt == 13) return 0;
        int reg = vfp_dreg(vn, N);           /* Vd:D or Vn:N, same position */

        /* VDUP from a core register: h1 bit 7 set, bit 4 clear. B:E pick
         * the lane: 00 is 32 bits, 01 16, 10 8. */
        if ((h1 & 0x0090) == 0x0080) {
            int be = (((h1 >> 6) & 1) << 1) | ((h2 >> 5) & 1);
            int Q  = (h1 >> 5) & 1;
            if (be == 3 || (Q && (reg & 1)) || (h2 & 0x0040)) return 0;
            o->op = OP_NEON_DUP;
            o->vd = (int8_t)reg;
            o->rt = (int8_t)rt;
            o->neon_q = (uint8_t)Q;
            o->neon_size = (uint8_t)(be == 0 ? 2 : be == 1 ? 1 : 0);
            set(o, A_SIMD, "vdup");
            return 1;
        }

        /* VMOV to or from one lane. opc1 (h1 6:5) and opc2 (h2 6:5) carry
         * the lane size and index together. */
        int o1 = (h1 >> 5) & 3, o2 = (h2 >> 5) & 3;
        int size, idx;
        if (o1 & 2)      { size = 0; idx = ((o1 & 1) << 2) | o2; }
        else if (o2 & 1) { size = 1; idx = ((o1 & 1) << 1) | (o2 >> 1); }
        else if (o2 == 0){ size = 2; idx = o1 & 1; }
        else return 0;
        int from = (h1 >> 4) & 1, U = (h1 >> 7) & 1;
        if (!from && U) return 0;                       /* that is VDUP's space */
        if (from && size == 2 && U) return 0;
        o->neon_size = (uint8_t)size;
        o->imm = (uint32_t)idx;
        o->has_imm = 1;
        o->rt = (int8_t)rt;
        if (from) {
            o->op = OP_NEON_FROM_SCALAR;
            o->vn = (int8_t)reg;
            o->vfp_unsigned = (uint8_t)U;
            set(o, A_SIMD, "vmov r,d[x]");
        } else {
            o->op = OP_NEON_TO_SCALAR;
            o->vd = (int8_t)reg;
            set(o, A_SIMD, "vmov d[x],r");
        }
        return 1;
    }

    switch (opc1) {
        case 0x0:
            /* Multiply-accumulate. Previously declined on the grounds that
             * translating it as a plain multiply would drop the accumulate —
             * which was right about the hazard and wrong about the conclusion,
             * exactly as with MOVT. It expresses directly as
             * `vd = vd +/- (vn * vm)`, and it was 404 instructions.
             *
             * The only genuine loss is that ARM may fuse the multiply and add
             * without an intermediate rounding step, where C rounds twice. That
             * is a last-bit difference in the mantissa, not a wrong answer, and
             * it is recorded rather than silently accepted. */
            o->op = op ? OP_VMLS : OP_VMLA;
            set(o, A_ALU, op ? "vmls" : "vmla");
            return 1;

        case 0x1:
            /* VNMLA / VNMLS. This case once decoded op == 1 as VNMUL, which
             * lives at opc1 0x2: so every VNMLA lost its accumulate, and
             * every VNMUL (below) lost its negation — both silently. */
            o->op = op ? OP_VNMLA : OP_VNMLS;
            set(o, A_ALU, op ? "vnmla" : "vnmls");
            return 1;

        case 0x2:                                  /* VMUL / VNMUL            */
            o->op = op ? OP_VNMUL : OP_VMUL;
            set(o, A_ALU, op ? "vnmul" : "vmul");
            return 1;

        case 0x3:
            o->op = op ? OP_VSUB : OP_VADD;
            set(o, A_ALU, op ? "vsub" : "vadd");
            return 1;

        case 0x8:
            o->op = OP_VDIV;
            set(o, A_ALU, "vdiv");
            return 1;

        case 0xB: {                                /* the "other" group       */
            int opc2 = h1 & 0xF;
            int opc3 = (h2 >> 6) & 3;

            /* VMOV immediate: an 8-bit float constant, expanded by the
             * architecture's VFPExpandImm. Carried as its final bit pattern
             * so the emitter writes bits rather than re-deriving a float. */
            if ((opc3 & 1) == 0 && (h2 & 0x00B0) == 0) {
                uint32_t i8 = ((uint32_t)opc2 << 4) | (h2 & 0xF);
                uint32_t a = (i8 >> 7) & 1, b = (i8 >> 6) & 1, cdefgh = i8 & 0x3F;
                o->op = OP_VMOV_IMM;
                o->vn = o->vm = ARM_NO_REG;
                o->has_imm = 1;
                if (dp)   /* the high word; the low word of the double is zero */
                    o->imm = (a << 31) | ((b ^ 1) << 30) | (b ? 0x3FC00000u : 0) | (cdefgh << 16);
                else
                    o->imm = (a << 31) | ((b ^ 1) << 30) | (b ? 0x3E000000u : 0) | (cdefgh << 19);
                set(o, A_ALU, "vmov imm");
                return 1;
            }

            if (opc2 == 0x0 && opc3 == 1) { o->op = OP_VMOV;  set(o, A_ALU, "vmov"); return 1; }

            /* VCVT between precisions. The destination is the OTHER size,
             * so its register number is read the other way round. */
            if (opc2 == 0x7 && opc3 == 3) {
                o->vd = (int8_t)(dp ? vfp_sreg(vd, D) : vfp_dreg(vd, D));
                o->op = OP_VCVT_F2F;
                set(o, A_ALU, dp ? "vcvt f32,f64" : "vcvt f64,f32");
                return 1;
            }
            if (opc2 == 0x0 && opc3 == 3) { o->op = OP_VABS;  set(o, A_ALU, "vabs"); return 1; }
            if (opc2 == 0x1 && opc3 == 1) { o->op = OP_VNEG;  set(o, A_ALU, "vneg"); return 1; }
            if (opc2 == 0x1 && opc3 == 3) { o->op = OP_VSQRT; set(o, A_ALU, "vsqrt"); return 1; }

            /* VCMP and VCMPE. The E form differs only in whether a quiet NaN
             * raises an exception, which this runtime does not model. */
            if ((opc2 == 0x4 || opc2 == 0x5) && (opc3 & 1)) {
                o->op = OP_VCMP;
                /* opc2 == 5 compares against zero rather than a register. */
                if (opc2 == 0x5) { o->vm = ARM_NO_REG; o->has_imm = 1; o->imm = 0; }
                set(o, A_ALU, "vcmp");
                return 1;
            }

            /* Integer to float: opc2 1000, with the source signedness in the
             * op bit rather than a separate field. */
            if (opc2 == 0x8 && (opc3 & 1)) {
                o->op = OP_VCVT_I2F;
                o->vfp_unsigned = (uint8_t)(op == 0);
                set(o, A_ALU, "vcvt f,i");
                return 1;
            }

            /* Float to integer: opc2 110x (round to nearest) or 111x (toward
             * zero). Only the toward-zero forms are claimed, because that is
             * what C's conversion does and the other rounding modes would need
             * explicit modelling to be right. */
            if ((opc2 & 0xE) == 0xC && (opc3 & 1)) {
                o->op = OP_VCVT_F2I;
                o->vfp_unsigned = (uint8_t)((opc2 & 1) == 0);
                set(o, A_ALU, "vcvt i,f");
                return 1;
            }
            return 0;
        }

        default:
            return 0;
    }
}

/* --- Thumb, 32-bit ---------------------------------------------------------- */

/* ThumbExpandImm — the 12-bit field that encodes a 32-bit constant.
 *
 * Two entirely different encodings share the field, selected by its top two
 * bits. With them clear, the low byte is replicated into a pattern (once,
 * halfword-spaced, byte-spaced, or all four); otherwise the field is a
 * rotate-right of a value whose top bit is implicit and always set.
 *
 * Reading it as a plain 12-bit integer is the obvious mistake, and it is quiet:
 * small constants happen to be in the first case with pattern 00, where the
 * naive reading is correct. Everything above 0xFF is then wrong. */
static uint32_t thumb_expand_imm(uint32_t imm12) {
    if ((imm12 & 0xC00) == 0) {
        uint32_t b = imm12 & 0xFF;
        switch ((imm12 >> 8) & 3) {
            case 0:  return b;
            case 1:  return (b << 16) | b;
            case 2:  return (b << 24) | (b << 8);
            default: return (b << 24) | (b << 16) | (b << 8) | b;
        }
    }
    /* The 8-bit value always has bit 7 set implicitly, so only 7 bits are
     * stored. Forgetting that leaves every rotated constant short by 0x80. */
    uint32_t v = 0x80u | (imm12 & 0x7F);
    uint32_t r = (imm12 >> 7) & 0x1F;
    return r ? ((v >> r) | (v << (32 - r))) : v;
}

/* --- Advanced SIMD ------------------------------------------------------------
 *
 * Only the data-processing groups that the running programs have asked for so
 * far, each decoded completely rather than sampled. Everything else in the
 * space stays a named SIMD trap. Field layout (T32):
 *
 *   h1  111U 1111 xDss nnnn      h2  dddd oooo NQMB mmmm
 *
 * with register numbers D:Vd, N:Vn, M:Vm (0-31). A Q operation names even
 * registers; an odd one is UNDEFINED, and is left untranslated.
 */

/* AdvSIMDExpandImm: the 64-bit constant a modified-immediate instruction
 * means. cmode picks the lane width and where the byte sits in it. */
static int simd_expand_imm(int op, int cmode, uint32_t imm8, uint64_t *out) {
    uint64_t i = imm8 & 0xFF, r = 0;
    switch (cmode >> 1) {
        case 0: r = i;         r |= r << 32; break;       /* 32-bit lanes, byte 0 */
        case 1: r = i << 8;    r |= r << 32; break;
        case 2: r = i << 16;   r |= r << 32; break;
        case 3: r = i << 24;   r |= r << 32; break;
        case 4: r = i;         r |= r << 16; r |= r << 32; break;   /* 16-bit lanes */
        case 5: r = i << 8;    r |= r << 16; r |= r << 32; break;
        case 6:
            r = (cmode & 1) ? ((i << 16) | 0xFFFF) : ((i << 8) | 0xFF);  /* "ones" forms */
            r |= r << 32;
            break;
        case 7:
            if ((cmode & 1) == 0 && op == 0) {             /* bytes */
                r = i * 0x0101010101010101ull;
            } else if ((cmode & 1) == 0 && op == 1) {      /* each bit a byte of 0xFF */
                for (int b = 0; b < 8; b++)
                    if (i & (1u << b)) r |= 0xFFull << (8 * b);
            } else if (op == 0) {                          /* single-precision float */
                uint64_t a = (i >> 7) & 1, bb = (i >> 6) & 1, cdefgh = i & 0x3F;
                r = (a << 31) | ((bb ^ 1) << 30) | (bb ? 0x3E000000u : 0) | (cdefgh << 19);
                r |= r << 32;
            } else {
                return 0;                                  /* UNDEFINED */
            }
            break;
    }
    *out = r;
    return 1;
}

static int decode_neon(uint16_t h1, uint16_t h2, arm_insn *o) {
    if ((h1 & 0xEF00) != 0xEF00) return 0;          /* not data processing */

    int U = (h1 >> 12) & 1;
    int Q = (h2 >> 6) & 1;
    int d = (((h1 >> 6) & 1) << 4) | ((h2 >> 12) & 0xF);
    int n = (((h2 >> 7) & 1) << 4) | (h1 & 0xF);
    int m = (((h2 >> 5) & 1) << 4) | (h2 & 0xF);

    /* One register and a modified immediate: h1 111a 1111 1D00 0bcd,
     * h2 dddd cccc 0Qo1 efgh. */
    if ((h1 & 0x00B8) == 0x0080 && (h2 & 0x0090) == 0x0010) {
        int cmode = (h2 >> 8) & 0xF, op = (h2 >> 5) & 1;
        uint32_t imm8 = ((uint32_t)U << 7) | ((uint32_t)(h1 & 7) << 4) | (h2 & 0xF);
        if (Q && (d & 1)) return 0;
        if (!simd_expand_imm(op, cmode, imm8, &o->imm64)) return 0;
        int orr_bic = (cmode < 12) && (cmode & 1);         /* 0xx1 and 10x1 */
        if (op == 0)      o->neon_fn = orr_bic ? NL_ORR : NL_MOV;
        else if (cmode == 14) o->neon_fn = NL_MOV;          /* VMOV.I64 */
        else              o->neon_fn = orr_bic ? NL_BIC : NL_MVN;
        o->op = OP_NEON_IMM;
        o->vd = (int8_t)d;
        o->neon_q = (uint8_t)Q;
        static const char *const nm[] = { "vand", "vbic", "vorr", "vorn", "veor",
                                          "vbsl", "vbit", "vbif", "vmov", "vmvn" };
        set(o, A_SIMD, nm[o->neon_fn]);
        return 1;
    }

    /* Three registers of the same length: h1 bit 7 clear. */
    if ((h1 & 0x0080) == 0) {
        int opc = (h2 >> 8) & 0xF, B = (h2 >> 4) & 1, size = (h1 >> 4) & 3;
        if (Q && ((d | n | m) & 1)) return 0;
        o->vd = (int8_t)d; o->vn = (int8_t)n; o->vm = (int8_t)m;
        o->neon_q = (uint8_t)Q;
        o->neon_size = (uint8_t)size;

        if (opc == 0x1 && B == 1) {                        /* the logical group */
            static const uint8_t fn[2][4] = { { NL_AND, NL_BIC, NL_ORR, NL_ORN },
                                              { NL_EOR, NL_BSL, NL_BIT, NL_BIF } };
            static const char *const nm[2][4] = { { "vand", "vbic", "vorr", "vorn" },
                                                  { "veor", "vbsl", "vbit", "vbif" } };
            o->op = OP_NEON_LOGIC;
            o->neon_fn = fn[U][size];
            set(o, A_SIMD, nm[U][size]);
            return 1;
        }
        if (opc == 0x8 && B == 0) {                        /* VADD / VSUB .I<size> */
            o->op = U ? OP_NEON_SUB : OP_NEON_ADD;
            set(o, A_SIMD, U ? "vsub.i" : "vadd.i");
            return 1;
        }
        if (opc == 0x8 && B == 1 && size != 3) {           /* VTST / VCEQ .I<size> */
            o->op = U ? OP_NEON_CEQ : OP_NEON_TST;
            set(o, A_SIMD, U ? "vceq.i" : "vtst");
            return 1;
        }
        if (opc == 0xB && B == 1 && U == 0 && !Q && size != 3) {  /* VPADD.I<size> */
            o->op = OP_NEON_PADD;
            set(o, A_SIMD, "vpadd.i");
            return 1;
        }
        if (opc == 0xD && !(size & 1)) {                   /* float, .F32 only */
            /* For floats the size field is sz (bit 20, 0 for F32) and bit 21
             * (size>>1) is an operation bit, so the operation is picked by U,
             * B and size>>1. Bit 4 (B) separates the adds from the multiplies:
             *   U=0 B=0  VADD / VSUB     (bit 21 clear / set)
             *   U=0 B=1  VMLA / VMLS     (bit 21 clear / set)
             *   U=1 B=1  VMUL            (bit 21 clear)
             * The other combinations are VABD and friends, not claimed. */
            int sub = size >> 1;
            o->neon_size = 2;                              /* 32-bit lanes */
            if (B == 0 && U == 0) {
                o->op = sub ? OP_NEON_FSUB : OP_NEON_FADD;
                set(o, A_SIMD, sub ? "vsub.f32" : "vadd.f32");
                return 1;
            }
            if (B == 1 && U == 0) {
                o->op = sub ? OP_NEON_FMLS : OP_NEON_FMLA;
                set(o, A_SIMD, sub ? "vmls.f32" : "vmla.f32");
                return 1;
            }
            if (B == 1 && U == 1 && !sub) {
                o->op = OP_NEON_FMUL;
                set(o, A_SIMD, "vmul.f32");
                return 1;
            }
        }
        return 0;
    }

    /* Three registers and a scalar: h1 bit 7 set, bits 5:4 = 10 for F32.
     * h2 is dddd opc(4) N 1 M 0 mmmm, so bit 6 and bit 4 are fixed. The scalar
     * is a 32-bit lane: M selects the lane and Vm[3:0] the D register, so unlike
     * the other forms M is NOT the top bit of the register number here. The Q
     * bit is h1 bit 12, the same position as U in the same-length group. */
    if ((h1 & 0x0080) && (h1 & 0x0030) == 0x0020 && (h2 & 0x0050) == 0x0040) {
        int opc = (h2 >> 8) & 0xF;
        int idx = (h2 >> 5) & 1;                           /* M: lane index  */
        int reg = h2 & 0xF;                                /* Vm[3:0] only    */
        int Qs = U;                                        /* Q, see above    */
        if (Qs && ((d | n) & 1)) return 0;
        if (opc == 0x1 || opc == 0x9) {                    /* VMLA / VMUL */
            o->op = opc == 0x1 ? OP_NEON_FMLA_SCALAR : OP_NEON_FMUL_SCALAR;
            o->vd = (int8_t)d; o->vn = (int8_t)n; o->vm = (int8_t)reg;
            o->imm = (uint32_t)idx;
            o->has_imm = 1;
            o->neon_q = (uint8_t)Qs;
            o->neon_size = 2;
            set(o, A_SIMD, opc == 0x1 ? "vmla.f32" : "vmul.f32");
            return 1;
        }
        return 0;
    }

    /* Two registers, miscellaneous: h1 1111 1111 1D11 ss10. Only VMOVN
     * (h2 dddd 0010 00M0 mmmm), which narrows a Q register's lanes to half
     * width into a D register. */
    if ((h1 & 0xFFB3) == 0xFFB2 && (h2 & 0x0FD0) == 0x0200) {
        int size = (h1 >> 2) & 3;
        if (size == 3 || (m & 1)) return 0;
        o->op = OP_NEON_MOVN;
        o->vd = (int8_t)d;
        o->vm = (int8_t)m;
        o->neon_size = (uint8_t)size;                      /* of the result */
        set(o, A_SIMD, "vmovn");
        return 1;
    }
    return 0;
}

/* Element and structure loads/stores: h1 1111 1001 AD L0 nnnn, h2 dddd tttt
 * sszz mmmm. Only VLD1/VST1 of whole registers (A=0, the four "multiple
 * single elements" types), which is what memcpy, struct copies and every
 * vectorised zeroing loop use. Lane size is irrelevant to the bytes moved on
 * a little-endian machine, and the alignment hint only adds a fault the
 * hardware would raise for a misaligned pointer, so neither changes the
 * translation. Rm: 15 no writeback, 13 advance by the transfer size, any
 * other register advance by it. */
static int decode_neon_ls(uint16_t h1, uint16_t h2, arm_insn *o) {
    if ((h1 & 0xFF90) != 0xF900) return 0;
    int L = (h1 >> 5) & 1;
    int d = (((h1 >> 6) & 1) << 4) | ((h2 >> 12) & 0xF);
    int type = (h2 >> 8) & 0xF, align = (h2 >> 4) & 3;
    int n;
    switch (type) {
        case 0x7: n = 1; if (align & 2) return 0; break;
        case 0xA: n = 2; if (align == 3) return 0; break;
        case 0x6: n = 3; if (align & 2) return 0; break;
        case 0x2: n = 4; break;
        default:  return 0;
    }
    if (d + n > 32) return 0;
    o->rn = h1 & 0xF;
    if (o->rn == 15) return 0;
    int rm = h2 & 0xF;
    o->vd  = (int8_t)d;
    o->imm = (uint32_t)n;
    o->has_imm = 1;
    o->writeback = (uint8_t)(rm != 15);
    o->rm = (rm == 15 || rm == 13) ? ARM_NO_REG : (int8_t)rm;
    o->op = L ? OP_NEON_VLD1 : OP_NEON_VST1;
    set(o, A_SIMD, L ? "vld1" : "vst1");
    return 1;
}

static void decode_t32(uint16_t h1, uint16_t h2, arm_insn *o) {
    o->raw = ((uint32_t)h1 << 16) | h2;

    /* Advanced SIMD and VFP occupy the coprocessor space: bits 15:13 are 111
     * and bits 11:10 are 11, which is exactly `(h1 & 0xEC00) == 0xEC00`.
     *
     * An earlier mask of `(h1 & 0xEE00) == 0xEC00` matched only 0xEC and 0xED,
     * missing the whole of 0xEE and 0xEF — which is where VFP's CDP/MCR/MRC
     * forms live, and they are the bulk of it. Those ~2,800 instructions were
     * reported as UNKNOWN rather than SIMD, so every earlier SIMD percentage
     * this toolkit printed was an undercount and every "unknown" figure was
     * correspondingly inflated.
     *
     * Worth noting that this made the tool look better than it was on one axis
     * and worse on another, which is why the trap breakdown by kind was what
     * finally surfaced it: 2.16% of instructions decoding to "?" is a number
     * that demands an explanation, and there wasn't one. */
    if ((h1 & 0xEC00) == 0xEC00) {
        if (decode_vfp(h1, h2, o)) return;
        if (decode_neon(h1, h2, o)) return;
        set(o, A_SIMD, "simd/vfp");
        return;
    }

    /* 11110 ... with bit 15 of the second halfword set: branches and
     * miscellaneous control. */
    if ((h1 & 0xF800) == 0xF000 && (h2 & 0x8000)) {
        /* Four encodings share this group, and they are selected by exactly two
         * bits of h2: bit 14 says call-or-branch, bit 12 picks within the pair.
         *
         *   bit14 bit12
         *     0     0     B<cond>.W (T3), or a system instruction
         *     0     1     B.W       (T4)
         *     1     0     BLX imm   (T2)
         *     1     1     BL        (T1)
         *
         * Masking bits 14:12 together and testing `& 5` conflates them, which
         * is what was here: `== 5` caught only BL, `== 4` caught BLX imm and
         * called it B.W, and real B.W matched neither and fell through to the
         * CONDITIONAL path — where it read a cond field that does not exist in
         * T4 and assembled its offset with T3's 21-bit layout. Every
         * unconditional wide branch was getting a wrong target and a bogus
         * condition, and the one that surfaced it was a tail call into an
         * import stub landing two bytes past the stub. */
        int is_call = (h2 >> 14) & 1;
        int low     = (h2 >> 12) & 1;

        /* The J1/J2 encoding: the two J bits are stored XORed with the sign
         * bit, so they must be un-XORed before assembling the offset. Treating
         * them as plain address bits gives a target that is right for short
         * branches and wrong for long ones. */
        uint32_t s   = (h1 >> 10) & 1;
        uint32_t j1  = (h2 >> 13) & 1;
        uint32_t j2  = (h2 >> 11) & 1;
        uint32_t i1  = !(j1 ^ s);
        uint32_t i2  = !(j2 ^ s);
        uint32_t hi  = (s << 24) | (i1 << 23) | (i2 << 22)
                     | ((uint32_t)(h1 & 0x03FF) << 12);

        if (is_call && low) {                   /* BL */
            set(o, A_CALL, "bl");
            o->has_target = 1;
            o->target = o->addr + 4
                      + sign_extend(hi | ((uint32_t)(h2 & 0x07FF) << 1), 25);
            return;
        }

        if (is_call) {                          /* BLX immediate */
            /* Two things differ from BL and both matter. The offset is a
             * multiple of FOUR — the low encoding field is imm10L, bit 0 is
             * the H bit and is not part of it — and the base is Align(PC,4),
             * not PC, because the destination is ARM code and ARM
             * instructions are word-aligned. Using PC gives a target two bytes
             * high whenever the instruction sits at an odd halfword, which is
             * half the time. */
            set(o, A_CALL, "blx imm");
            o->switches_mode = 1;
            o->has_target = 1;
            o->target = ((o->addr + 4) & ~3u)
                      + sign_extend(hi | ((uint32_t)(h2 & 0x07FE) << 1), 25);
            return;
        }

        if (low) {                              /* B.W, unconditional */
            /* `op` matters as much as the class. The class routes the emitter's
             * fallback, but only `op` reaches the switch that actually
             * translates a branch — leaving it OP_NONE meant every wide branch
             * decoded perfectly, computed its target correctly, and then
             * trapped anyway. 2,359 instructions, purely for want of this
             * line. */
            o->op = OP_B;
            o->cond = ARM_COND_AL;
            set(o, A_BRANCH, "b.w");
            o->has_target = 1;
            o->target = o->addr + 4
                      + sign_extend(hi | ((uint32_t)(h2 & 0x07FF) << 1), 25);
            return;
        }

        /* Conditional B.W, or a system instruction when the condition field
         * (h1 bits 9:6) is 111x. This test once read bits 10:7, so hints and
         * barriers decoded as branches: `nop.w` became a jump to a bogus
         * address and `dmb` vanished into a conditional branch. */
        if (((h1 >> 6) & 0xE) == 0xE) {
            if ((h1 & 0xFFF0) == 0xF3A0 && (h2 & 0xFF00) == 0x8000 && (h2 & 0xFF) <= 4) {
                o->op = OP_NOP;                 /* nop/yield/wfe/wfi/sev */
                set(o, A_ALU, "nop.w");
                return;
            }
            if ((h1 & 0xFFF0) == 0xF3B0 && (h2 & 0xFF00) == 0x8F00) {
                switch ((h2 >> 4) & 0xF) {
                case 0x2: o->op = OP_CLREX; set(o, A_SYS, "clrex"); return;
                case 0x4: case 0x5: case 0x6:
                    o->op = OP_DMB; set(o, A_SYS, "dmb/dsb/isb"); return;
                }
            }
            set(o, A_SYS, "sys");
            return;
        }
        o->op = OP_B;
        o->cond = (uint8_t)((h1 >> 6) & 0xF);
        set(o, A_BRANCH, "b<cond>.w");
        o->conditional = 1;
        /* T3 has its own layout: a 6-bit high field rather than 10, and J1/J2
         * placed directly rather than un-XORed with S. Reusing the raw s/j1/j2
         * read above, since only the assembly differs. */
        {
            uint32_t imm  = (s << 20) | (j2 << 19) | (j1 << 18)
                          | ((uint32_t)(h1 & 0x003F) << 12)
                          | ((uint32_t)(h2 & 0x07FF) << 1);
            o->has_target = 1;
            o->target = o->addr + 4 + sign_extend(imm, 21);
        }
        return;
    }

    /* Exclusive access: LDREX/STREX and their byte, halfword and doubleword
     * forms. These are how every lock-free primitive and every mutex fast path
     * is built, so a threaded game reaches them constantly. STREX writes its
     * status (0 = stored) to rd. */
    if ((h1 & 0xFFF0) == 0xE850 && ((h2 >> 8) & 0xF) == 0xF) {
        o->op = OP_LDREX; o->xsize = 4;
        o->rn = h1 & 0xF; o->rt = (h2 >> 12) & 0xF;
        o->imm = (uint32_t)(h2 & 0xFF) * 4u; o->has_imm = 1;
        set(o, A_LOAD, "ldrex");
        return;
    }
    if ((h1 & 0xFFF0) == 0xE840) {
        o->op = OP_STREX; o->xsize = 4;
        o->rn = h1 & 0xF; o->rt = (h2 >> 12) & 0xF; o->rd = (h2 >> 8) & 0xF;
        o->imm = (uint32_t)(h2 & 0xFF) * 4u; o->has_imm = 1;
        set(o, A_STORE, "strex");
        return;
    }
    if ((h1 & 0xFFE0) == 0xE8C0) {
        static const uint8_t sz[8] = { 0, 0, 0, 0, 1, 2, 0, 8 };
        const uint8_t x = sz[(h2 >> 4) & 7];
        const int load = (h1 & 0x0010) != 0;
        if (x && (h2 & 0x0080) == 0) {
            o->op = load ? OP_LDREX : OP_STREX; o->xsize = x;
            o->rn = h1 & 0xF; o->rt = (h2 >> 12) & 0xF;
            if (x == 8) o->rt2 = (h2 >> 8) & 0xF;
            if (!load)  o->rd  = h2 & 0xF;
            o->imm = 0; o->has_imm = 1;
            set(o, load ? A_LOAD : A_STORE,
                load ? (x == 1 ? "ldrexb" : x == 2 ? "ldrexh" : "ldrexd")
                     : (x == 1 ? "strexb" : x == 2 ? "strexh" : "strexd"));
            return;
        }
    }

    /* Load/store DUAL. Bit 6 separates these from the multiple forms below, and
     * the earlier mask excluded them by keeping that bit — so every LDRD/STRD
     * fell through to unknown. They are common: compilers use them to move
     * 64-bit values and adjacent struct fields in one instruction. */
    if ((h1 & 0xFE40) == 0xE840 && (h1 & 0x0120) != 0x0000) {
        int load = (h1 & 0x0010) != 0;
        o->op  = load ? OP_LDRD : OP_STRD;
        o->rn  = h1 & 0xF;
        o->rt  = (h2 >> 12) & 0xF;
        o->rt2 = (h2 >> 8) & 0xF;
        o->imm = (uint32_t)(h2 & 0xFF) * 4u;   /* scaled by 4 */
        o->has_imm = 1;
        o->mem_add = (h1 & 0x0080) != 0;       /* U bit */
        /* P (bit 8) and W (bit 5): `strd r4, r5, [sp, #-8]!` and
         * `ldrd r4, r5, [sp], #8` move the base. The entry test above
         * guarantees P or W is set. */
        o->writeback = (h1 & 0x0020) != 0;
        o->postindex = (h1 & 0x0100) == 0;
        set(o, load ? A_LOAD : A_STORE, load ? "ldrd" : "strd");
        return;
    }

    /* Load/store multiple. LDM with the PC in the list is a return. */
    if ((h1 & 0xFE40) == 0xE800) {
        int load = (h1 & 0x0010) != 0;
        o->rn = h1 & 0xF;
        o->reglist = h2;
        o->writeback = (h1 & 0x0020) != 0;
        o->decrement = (h1 & 0x0180) == 0x0100;      /* DB; 0x0080 is IA */
        if ((h1 & 0x0180) != 0x0080 && (h1 & 0x0180) != 0x0100) {
            set(o, A_SYS, "srs/rfe");                 /* not LDM/STM at all */
            return;
        }
        /* PUSH.W and POP.W are exactly STMDB sp! and LDMIA sp!. Anything
         * else against SP — STMIA sp without writeback is a common way to
         * store outgoing stack arguments — is a general LDM/STM and must not
         * get the stack-shaped translation, which moves SP. */
        if (o->rn == 13 && o->writeback && load && !o->decrement)      o->op = OP_POP;
        else if (o->rn == 13 && o->writeback && !load && o->decrement) o->op = OP_PUSH;
        else                                                           o->op = load ? OP_LDM : OP_STM;
        /* A load that includes PC is a return — but it is still a load of
         * every other register in the list and an SP adjustment. Classifying
         * it as a return BEFORE filling in the operands made the emitter
         * write a bare `return;` for every `pop.w {r4-r11, pc}`, so callee-
         * saved registers and SP came back unrestored, silently. */
        if (load && (h2 & 0x8000)) { set(o, A_RETURN, "ldm {..,pc}"); return; }
        set(o, load ? A_LOADM : A_STOREM, "ldm/stm.w");
        return;
    }

    /* --- data processing -------------------------------------------------- */
    /*
     * The shifted-register (0xEA/0xEB) and modified-immediate (0xF0/0xF1)
     * groups share an operation field, an S bit, and the rn/rd placement, so
     * they are decoded together and differ only in how operand 2 is formed.
     *
     * Four operations change identity when a register field is r15, and the
     * architecture uses that rather than spending encoding space:
     *   rn == 15 turns ORR into MOV and ORN into MVN
     *   rd == 15 with S set turns AND/EOR/ADD/SUB into TST/TEQ/CMN/CMP
     * Missing either produces an instruction that reads a register which is not
     * an operand, and writes one that is not a destination.
     */
    if ((h1 & 0xFE00) == 0xEA00 || (h1 & 0xFA00) == 0xF000) {
        /* Which of the two groups matched — NOT a single bit. Both 0xEA/0xEB
         * and 0xF0/0xF1 have bit 15 set, so testing it classifies every
         * shifted-register instruction as an immediate one and then reads
         * operand 2 out of the wrong fields. */
        int is_imm = (h1 & 0xFA00) == 0xF000;
        if (is_imm && (h2 & 0x8000)) { set(o, A_UNKNOWN, "?"); return; }

        static const arm_op dp[16] = {
            OP_AND, OP_BIC, OP_ORR, OP_ORN, OP_EOR, OP_NONE, OP_NONE, OP_NONE,
            OP_ADD, OP_NONE, OP_ADC, OP_SBC, OP_NONE, OP_SUB, OP_RSB, OP_NONE
        };
        int opf = (h1 >> 5) & 0xF;
        o->op = dp[opf];
        if (o->op == OP_NONE) { set(o, A_ALU, "alu.w"); return; }

        o->rn = h1 & 0xF;
        o->rd = (h2 >> 8) & 0xF;
        o->sets_flags = (h1 & 0x0010) != 0;

        if (o->rn == 15) {
            if (opf == 0x2) o->op = OP_MOV;       /* orr rn=15 -> mov */
            if (opf == 0x3) o->op = OP_MVN;       /* orn rn=15 -> mvn */
            o->rn = ARM_NO_REG;
        }
        if (o->rd == 15) {
            /* rd == 15 is not a destination here. With S set it selects the
             * compare forms, where the result is discarded and only the flags
             * matter — but ONLY for these four encodings. Every other opf with
             * rd == 15 is UNPREDICTABLE in the architecture, and writing to
             * the PC without S is a branch this path does not model.
             *
             * Clearing rd for those too is how `? = r1 & ~0x18A0000` reached a
             * build: the op stayed BIC, the destination became "no register",
             * and the emitter printed the placeholder as though it were a
             * variable. Leaving op as OP_NONE traps instead, which is what an
             * encoding we cannot translate is supposed to do. */
            int compare = o->sets_flags
                       && (opf == 0x0 || opf == 0x4 || opf == 0x8 || opf == 0xD);
            if (compare) {
                if (opf == 0x0) o->op = OP_TST;
                if (opf == 0x4) o->op = OP_TEQ;
                if (opf == 0x8) o->op = OP_CMN;
                if (opf == 0xD) o->op = OP_CMP;
                o->rd = ARM_NO_REG;
            } else {
                o->op = OP_NONE;
                set(o, A_ALU, "alu.w rd=pc");
                return;
            }
        }

        if (is_imm) {
            uint32_t imm12 = (uint32_t)(((h1 >> 10) & 1) << 11)
                           | (uint32_t)(((h2 >> 12) & 7) << 8)
                           | (uint32_t)(h2 & 0xFF);
            o->imm = thumb_expand_imm(imm12);
            o->imm_carry = (imm12 & 0xC00) != 0;
            o->has_imm = 1;
        } else {
            o->rm = h2 & 0xF;
            o->shift_type = (h2 >> 4) & 3;
            o->shift_amt  = (uint8_t)((((h2 >> 12) & 7) << 2) | ((h2 >> 6) & 3));
            /* DecodeImmShift: an amount of 0 means 32 for LSR/ASR and RRX for
             * ROR. Read literally, `lsr r0, r1, #32` becomes a no-op move. */
            if (o->shift_amt == 0 && o->shift_type == SH_ROR) {
                o->shift_type = SH_RRX;
                o->shift_amt  = 1;
            } else if (o->shift_amt == 0 && o->shift_type != SH_LSL) {
                o->shift_amt = 32;
            }
        }
        set(o, A_ALU, "alu.w");
        return;
    }

    /* Plain binary immediate: MOVW/MOVT and the wide add/sub. These carry a
     * 16-bit literal rather than a modified immediate, and are how compilers
     * build addresses — so they are worth decoding even though they are only
     * two encodings. */
    /* The group is selected by bit 9, which is what separates plain binary
     * immediates from the modified immediates above. Masking any lower bit
     * excludes members of the group: an earlier mask of 0xFB40 kept bit 6,
     * which MOVW sets, so MOVW never matched at all. */
    if ((h1 & 0xFA00) == 0xF200 && !(h2 & 0x8000)) {
        uint32_t imm = (uint32_t)(((h1 >> 10) & 1) << 11)
                     | (uint32_t)(((h2 >> 12) & 7) << 8)
                     | (uint32_t)(h2 & 0xFF);
        o->rd = (h2 >> 8) & 0xF;
        o->has_imm = 1;

        uint16_t form = h1 & 0xFBF0;
        if (form == 0xF240) {                     /* movw: a 16-bit literal   */
            o->op = OP_MOV;
            o->imm = imm | ((uint32_t)(h1 & 0xF) << 12);
            set(o, A_ALU, "movw");
            return;
        }
        if (form == 0xF2C0) {                     /* movt                     */
            /* MOVT writes the top half and preserves the bottom, which is not
             * a move — but it IS trivially expressible, and it is load-bearing:
             * MOVW/MOVT is how ARM builds a 32-bit address, so leaving it
             * untranslated costs every constructed pointer its high 16 bits.
             * A trace of module_start showed the result — indirect branches to
             * 0x00000000 — which is what promoted this from "deliberately
             * trapped" to "the highest-value instruction remaining". */
            o->op = OP_MOVT;
            o->imm = imm | ((uint32_t)(h1 & 0xF) << 12);
            set(o, A_ALU, "movt");
            return;
        }
        if (form == 0xF200 || form == 0xF2A0) {   /* addw / subw              */
            o->op = (form == 0xF2A0) ? OP_SUB : OP_ADD;
            o->rn = h1 & 0xF;
            o->imm = imm;
            set(o, A_ALU, "addw/subw");
            return;
        }
        /* The rest of this group is bitfield and saturating arithmetic, split
         * by the same 5-bit op the forms above use. `imm` is not an immediate
         * here at all — the field holds a width or a bit position — which is
         * why these need their own operands rather than reusing `imm`. */
        uint32_t bop = (h1 >> 4) & 0x1F;
        uint32_t lsb = (uint32_t)(((h2 >> 12) & 7) << 2) | ((h2 >> 6) & 3);
        o->rn = h1 & 0xF;
        o->has_imm = 0;
        o->bf_lsb  = (uint8_t)lsb;

        if (bop == 0x14 || bop == 0x1C) {         /* sbfx / ubfx              */
            uint32_t width = (h2 & 0x1F) + 1;
            /* lsb + width past the end of the register is UNPREDICTABLE, and
             * a linear sweep over literal pools produces it. Refuse rather
             * than clamp: a trap says "this was not an instruction", a clamped
             * extract says nothing. */
            if (lsb + width <= 32) {
                o->op = (bop == 0x14) ? OP_SBFX : OP_UBFX;
                o->bf_width = (uint8_t)width;
            }
            set(o, A_ALU, (bop == 0x14) ? "sbfx" : "ubfx");
            return;
        }
        if (bop == 0x16) {                        /* bfi / bfc                */
            /* BFI names the field by its END bit, not its width — msb is an
             * absolute position, so the width has to be derived. BFC is the
             * same encoding with rn == 15, which here means "no source"
             * rather than "the PC". */
            uint32_t msb = h2 & 0x1F;
            int clear = (o->rn == 15);
            if (msb >= lsb) {
                o->op = clear ? OP_BFC : OP_BFI;
                o->bf_width = (uint8_t)(msb - lsb + 1);
            }
            if (clear) o->rn = ARM_NO_REG;
            set(o, A_ALU, clear ? "bfc" : "bfi");
            return;
        }
        /* SSAT/USAT: saturating arithmetic, not bitfield work. Named
         * separately so the trap report does not claim bitfield instructions
         * remain after these are the only thing left in the group. */
        o->rn = ARM_NO_REG;
        set(o, A_ALU, "sat");
        return;
    }

    /* Single load/store, immediate and register offset. A load into the PC is
     * a return. */
    /* 1111 1001 xxx0: Advanced SIMD element and structure loads and stores
     * (VLD1-4/VST1-4). They sit inside the single load/store space below, and
     * read as one of its forms a `vst1.32 {d16}, [r1]` became an integer
     * store of r0 — no trap, wrong memory. */
    if ((h1 & 0xFF10) == 0xF900) {
        if (!decode_neon_ls(h1, h2, o)) set(o, A_SIMD, "vld/vst");
        return;
    }

    if ((h1 & 0xFE00) == 0xF800) {
        int load = (h1 & 0x0010) != 0;
        int size = (h1 >> 5) & 3;                 /* 0 byte, 1 half, 2 word */
        int sign = (h1 & 0x0100) != 0;

        if (load && ((h2 >> 12) & 0xF) == 0xF) {
            /* A byte or halfword "load" into the PC is a hint: PLD, PLI, or
             * an unallocated hint that executes as a NOP. Classing it as a
             * return made every `pld [r1, #64]` in newlib's memcpy a
             * `return;` halfway through the copy. */
            if (size != 2) { o->op = OP_NOP; set(o, A_NOP, "pld/pli"); return; }
            /* ldr pc, [sp], #4 is POP {pc}, and must still move SP. */
            if ((h1 & 0x000F) == 13 && (h2 & 0x0FFF) == 0x0B04) {
                o->op = OP_POP;
                o->rn = 13;
                o->reglist = 0x8000;
                o->writeback = 1;
                set(o, A_RETURN, "pop {pc}");
                return;
            }
            set(o, A_RETURN, "ldr pc");
            return;
        }

        o->rt = (h2 >> 12) & 0xF;
        o->rn = h1 & 0xF;
        o->mem_add = 1;

        if (load) o->op = size == 0 ? (sign ? OP_LDRSB : OP_LDRB)
                        : size == 1 ? (sign ? OP_LDRSH : OP_LDRH) : OP_LDR;
        else      o->op = size == 0 ? OP_STRB : size == 1 ? OP_STRH : OP_STR;
        if (size == 3 || (!load && sign)) o->op = OP_NONE;   /* UNDEFINED */

        if (o->rn == 15) {                        /* literal: U is bit 7    */
            o->imm = h2 & 0xFFF;
            o->has_imm = 1;
            o->mem_add = (h1 & 0x0080) != 0;
        } else if (h1 & 0x0080) {                 /* 12-bit unsigned offset */
            o->imm = h2 & 0xFFF;
            o->has_imm = 1;
        } else if ((h2 & 0x0FC0) == 0x0000) {     /* register offset */
            o->rm = h2 & 0xF;
            o->shift_type = SH_LSL;
            o->shift_amt = (uint8_t)((h2 >> 4) & 3);
        } else if ((h2 & 0x0800) == 0x0800) {
            /* 1PUW imm8. 1100 is a negative offset and 1110 the unprivileged
             * LDRT/STRT (the same access in user mode); the other four index
             * and write the base back. Reading all of them as [rn, #+imm8]
             * turned `str r0, [sp, #-4]!` into a store ABOVE the stack
             * pointer that never moved it. */
            int P = (h2 >> 10) & 1, U = (h2 >> 9) & 1, W = (h2 >> 8) & 1;
            o->imm = h2 & 0xFF;
            o->has_imm = 1;
            o->mem_add = (uint8_t)U;
            if (!(P && !W)) {
                if (!P && !W) o->op = OP_NONE;    /* 1000/1010: UNDEFINED   */
                o->writeback = 1;
                o->postindex = (uint8_t)!P;
            }
        } else {
            o->op = OP_NONE;                      /* UNDEFINED              */
        }
        set(o, load ? A_LOAD : A_STORE, "ldr/str.w");
        return;
    }

    /* The wide sign/zero extends, op in h1 bits 6:4, with a rotation of
     * the source by 0/8/16/24 and — when rn is not 15 — an add of rn.
     *
     * This once indexed a four-entry table by bits 5:4 alone, so SXTB.W
     * (op 4) became SXTH and UXTB.W (op 5) UXTH, the two B16 forms became
     * SXTB/UXTB, the rotation was dropped and the add forms were decoded as
     * plain extends. Every one silent. */
    if ((h1 & 0xFF80) == 0xFA00 && (h2 & 0xF080) == 0xF080) {
        static const arm_op ex[8] = { OP_SXTH, OP_UXTH, OP_SXTB16, OP_UXTB16,
                                      OP_SXTB, OP_UXTB, OP_NONE, OP_NONE };
        static const char *const nm[8] = { "sxth", "uxth", "sxtb16", "uxtb16",
                                           "sxtb", "uxtb", "?", "?" };
        int k = (h1 >> 4) & 7;
        o->op = ex[k];
        o->rd = (h2 >> 8) & 0xF;
        o->rm = h2 & 0xF;
        o->rn = (h1 & 0xF) == 0xF ? ARM_NO_REG : (int8_t)(h1 & 0xF);
        o->shift_type = SH_ROR;
        o->shift_amt = (uint8_t)(((h2 >> 4) & 3) * 8);
        set(o, A_ALU, nm[k]);
        return;
    }

    /* REV / REV16 / RBIT / REVSH .W. Rm is encoded twice and must agree. */
    if ((h1 & 0xFFF0) == 0xFA90 && (h2 & 0xF0C0) == 0xF080) {
        static const arm_op rv[4] = { OP_REV, OP_REV16, OP_RBIT, OP_REVSH };
        static const char *const nm[4] = { "rev.w", "rev16.w", "rbit", "revsh.w" };
        int k = (h2 >> 4) & 3;
        if ((h1 & 0xF) != (h2 & 0xF)) { set(o, A_ALU, "?"); return; }
        o->op = rv[k];
        o->rd = (h2 >> 8) & 0xF;
        o->rm = h2 & 0xF;
        set(o, A_ALU, nm[k]);
        return;
    }

    /* SMULxy / SMLAxy: a 16x16 signed multiply of chosen halves, plus Ra for
     * SMLA (Ra == 15 is SMUL). The Q flag SMLA sets on overflow is not
     * modelled; nothing in the corpus reads it. */
    if ((h1 & 0xFFF0) == 0xFB10 && (h2 & 0x00C0) == 0x0000) {
        o->rd = (h2 >> 8) & 0xF;
        o->rn = h1 & 0xF;
        o->rm = h2 & 0xF;
        o->rt = (h2 >> 12) & 0xF;
        o->half_sel = (uint8_t)((h2 >> 4) & 3);
        if (o->rt == 0xF) { o->op = OP_SMULXY; o->rt = ARM_NO_REG; set(o, A_ALU, "smulxy"); }
        else              { o->op = OP_SMLAXY; set(o, A_ALU, "smlaxy"); }
        return;
    }

    /* Table branch: a jump table in one instruction, indexing a byte or
     * halfword array to compute a forward branch. Classed indirect here;
     * arm_decode, which can see the bytes around it, bounds the table and
     * makes it followable when the guarding compare proves its size. */
    if ((h1 & 0xFFF0) == 0xE8D0 && (h2 & 0xFFE0) == 0xF000) {
        o->op = (h2 & 0x10) ? OP_TBH : OP_TBB;
        o->rn = h1 & 0xF;
        o->rm = h2 & 0xF;
        set(o, A_INDIRECT, (h2 & 0x10) ? "tbh" : "tbb");
        return;
    }

    /* Shift by register. */
    if ((h1 & 0xFF80) == 0xFA00 && (h2 & 0xF0F0) == 0xF000) {
        static const arm_op sh[4] = { OP_LSL, OP_LSR, OP_ASR, OP_ROR };
        o->op = sh[(h1 >> 5) & 3];
        o->rd = (h2 >> 8) & 0xF;
        o->rn = h1 & 0xF;
        o->rm = h2 & 0xF;
        o->sets_flags = (h1 & 0x0010) != 0;
        set(o, A_ALU, "shift reg.w");
        return;
    }

    /* Count leading zeros. Compilers use it for more than bit scanning:
     * `clz r0, r0; lsrs r0, #5` is the branch-free `x == 0`. */
    if ((h1 & 0xFFF0) == 0xFAB0 && (h2 & 0xF0F0) == 0xF080) {
        o->op = OP_CLZ;
        o->rd = (h2 >> 8) & 0xF;
        o->rm = h2 & 0xF;
        set(o, A_ALU, "clz");
        return;
    }

    /* Parallel add/subtract, unsigned GE-setting forms (op2 == 100). The
     * signed, saturating and halving variants share the group and are left
     * named rather than guessed at. */
    if ((h1 & 0xFF80) == 0xFA80 && (h2 & 0xF080) == 0xF000) {
        int op1 = (h1 >> 4) & 7, op2 = (h2 >> 4) & 7;
        o->rd = (h2 >> 8) & 0xF;
        o->rn = h1 & 0xF;
        o->rm = h2 & 0xF;
        if (op2 == 4) {
            static const arm_op pop[8] = { OP_UADD8, OP_UADD16, OP_NONE, OP_NONE,
                                           OP_USUB8, OP_USUB16, OP_NONE, OP_NONE };
            static const char *const pnm[8] = { "uadd8", "uadd16", "uasx", "?",
                                                "usub8", "usub16", "usax", "?" };
            o->op = pop[op1];
            set(o, A_ALU, pnm[op1]);
            return;
        }
        set(o, A_ALU, "parallel add/sub");
        return;
    }

    /* SEL: bytes from rn or rm by GE. */
    if ((h1 & 0xFFF0) == 0xFAA0 && (h2 & 0xF0F0) == 0xF080) {
        o->op = OP_SEL;
        o->rd = (h2 >> 8) & 0xF;
        o->rn = h1 & 0xF;
        o->rm = h2 & 0xF;
        set(o, A_ALU, "sel");
        return;
    }

    /* Multiply and multiply-accumulate. MLA and MLS carry a third source,
     * the accumulator Ra, held in rt; Ra == 15 is plain MUL. */
    if ((h1 & 0xFFF0) == 0xFB00 && ((h2 & 0x00F0) == 0x0000 || (h2 & 0x00F0) == 0x0010)) {
        o->rd = (h2 >> 8) & 0xF;
        o->rn = h1 & 0xF;
        o->rm = h2 & 0xF;
        o->rt = (h2 >> 12) & 0xF;
        if ((h2 & 0x00F0) == 0x0010) { o->op = OP_MLS; set(o, A_ALU, "mls"); return; }
        if (o->rt == 0xF)            { o->op = OP_MUL; o->rt = ARM_NO_REG; set(o, A_ALU, "mul.w"); return; }
        o->op = OP_MLA;
        set(o, A_ALU, "mla");
        return;
    }

    /* Long multiply: a 64-bit product (or product plus accumulator) in a
     * register pair. The compiler emits UMULL for every division by a
     * constant, so it is far commoner than 64-bit arithmetic in the source.
     * RdLo is held in rt and RdHi in rt2, as for LDRD. */
    if ((h1 & 0xFF90) == 0xFB80 && (h2 & 0x00F0) == 0x0000) {
        static const arm_op lm[4] = { OP_SMULL, OP_UMULL, OP_SMLAL, OP_UMLAL };
        static const char *const nm[4] = { "smull", "umull", "smlal", "umlal" };
        int k = (h1 >> 5) & 3;                   /* 00 smull 01 umull 10 smlal 11 umlal */
        o->op  = lm[k];
        o->rn  = h1 & 0xF;
        o->rm  = h2 & 0xF;
        o->rt  = (h2 >> 12) & 0xF;
        o->rt2 = (h2 >> 8) & 0xF;
        set(o, A_ALU, nm[k]);
        return;
    }

    set(o, A_UNKNOWN, "?");
}

/* --- ARM -------------------------------------------------------------------- */

static void decode_a32(uint32_t w, arm_insn *o) {
    o->raw = w;

    uint32_t cond = w >> 28;
    uint32_t op   = (w >> 25) & 0x7;

    if (cond != 0xF && cond != 0xE) o->conditional = 1;

    /* The unconditional encoding space (cond == 1111) holds BLX immediate,
     * advanced SIMD, and the memory barriers. It is not "condition never" —
     * reading it as a normal conditional instruction misdecodes all of it. */
    if (cond == 0xF) {
        o->conditional = 0;
        if ((w & 0xFE000000) == 0xFA000000) {
            set(o, A_CALL, "blx imm");
            o->switches_mode = 1;
            o->has_target = 1;
            o->target = o->addr + 8 + sign_extend(w & 0x00FFFFFF, 24) * 4
                      + (((w >> 24) & 1) << 1);
            return;
        }
        if ((w & 0x0F000000) == 0x0D000000 || (w & 0x0E000000) == 0x0C000000
            || (w & 0x0F000000) == 0x0E000000) {
            set(o, A_SIMD, "simd/vfp");
            return;
        }
        set(o, A_SYS, "sys");
        return;
    }

    /* BX / BLX register, which live inside the data-processing space and would
     * otherwise decode as a nonsensical MSR. */
    if ((w & 0x0FFFFFF0) == 0x012FFF10) {
        int rm = w & 0xF;
        if (rm == 14) set(o, A_RETURN, "bx lr");
        else        { set(o, A_INDIRECT, "bx"); o->switches_mode = 1; }
        return;
    }
    if ((w & 0x0FFFFFF0) == 0x012FFF30) {
        set(o, A_CALL, "blx reg");
        o->switches_mode = 1;
        return;
    }

    switch (op) {
        case 0: case 1:                             /* data processing        */
            /* MOV pc, lr is a return written as an ALU operation. */
            if (((w >> 12) & 0xF) == 15 && ((w >> 21) & 0xF) == 0xD) {
                set(o, A_RETURN, "mov pc,lr");
                return;
            }
            set(o, A_ALU, "alu");
            return;

        case 2: case 3:                             /* load / store           */
            if (op == 3 && (w & 0x00000010)) { set(o, A_UNDEF, "undef"); return; }
            if ((w & 0x00100000) && ((w >> 12) & 0xF) == 15) {
                set(o, A_RETURN, "ldr pc");
                return;
            }
            set(o, (w & 0x00100000) ? A_LOAD : A_STORE, "ldr/str");
            return;

        case 4:                                     /* ldm / stm              */
            if ((w & 0x00100000) && (w & 0x00008000)) {
                set(o, A_RETURN, "ldm {..,pc}");
                return;
            }
            set(o, (w & 0x00100000) ? A_LOADM : A_STOREM, "ldm/stm");
            return;

        case 5:                                     /* b / bl                 */
            set(o, (w & 0x01000000) ? A_CALL : A_BRANCH,
                   (w & 0x01000000) ? "bl" : "b");
            o->has_target = 1;
            o->target = o->addr + 8 + sign_extend(w & 0x00FFFFFF, 24) * 4;
            return;

        case 6:                                     /* coprocessor / SIMD     */
            set(o, A_SIMD, "simd/vfp");
            return;

        case 7:
            if (w & 0x01000000) { set(o, A_SYS, "svc"); return; }
            set(o, A_SIMD, "cdp/mcr");
            return;

        default:
            set(o, A_UNKNOWN, "?");
            return;
    }
}

/* --- IT blocks ---------------------------------------------------------------- */

void arm_it_step(uint8_t *itstate, arm_insn *in) {
    if (in->mode != ARM_T32) return;
    if (in->op == OP_IT) {
        *itstate = (uint8_t)((in->cond << 4) | (in->it_mask & 0xF));
        return;
    }
    if ((*itstate & 0xF) == 0) return;

    const uint8_t cond = *itstate >> 4;
    if (cond != ARM_COND_AL) {
        in->cond = cond;
        in->conditional = 1;
    }
    if (in->width == 2 && in->sets_flags &&
        in->op != OP_CMP && in->op != OP_CMN && in->op != OP_TST && in->op != OP_TEQ)
        in->sets_flags = 0;

    /* ITAdvance: the low five bits shift left until the mask runs out. */
    if ((*itstate & 0x7) == 0) *itstate = 0;
    else *itstate = (uint8_t)((*itstate & 0xE0) | ((*itstate << 1) & 0x1F));
}

/* --- jump tables ------------------------------------------------------------ */
/*
 * A compiled switch is `cmp rI, #k ; bhi default ; tbb [pc, rI]` with the
 * table immediately after. Nothing in the TBB says how long the table is; the
 * compare does. Reading on until something "looks like code" would be a
 * guess, and a wrong guess translates table bytes as branch targets — so the
 * size is taken only from that exact guard, and a table without one stays an
 * indirect transfer that traps by name.
 *
 * Matching backwards is safe here in a way it is not in general: the guard
 * must decode as a compare of the SAME index register immediately followed by
 * an unsigned-greater (or carry-set) branch, which data does not do by
 * accident often enough to matter, and every resulting target is checked. */
static int t32_bcc(uint16_t h1, uint16_t h2, uint8_t *cond) {
    if ((h1 & 0xF800) != 0xF000 || (h2 & 0xD000) != 0x8000) return 0;
    *cond = (h1 >> 6) & 0xF;
    return *cond < 0xE;
}

static uint32_t table_bound(const uint8_t *code, uint32_t code_addr,
                            uint32_t addr, int rm) {
    uint32_t off = addr - code_addr;
    uint8_t cond = 0xF;
    uint32_t b_at;                         /* offset of the guarding branch */
    if (off >= 2 && (rd16le(code + off - 2) & 0xF000) == 0xD000) {
        cond = (rd16le(code + off - 2) >> 8) & 0xF;
        b_at = off - 2;
    } else if (off >= 4 && t32_bcc(rd16le(code + off - 4), rd16le(code + off - 2), &cond)) {
        b_at = off - 4;
    } else {
        return 0;
    }
    if (cond != 0x8 && cond != 0x2) return 0;      /* HI: n = k+1, CS: n = k */

    int k = -1;
    if (b_at >= 2) {                               /* cmp rI, #imm8 (16-bit) */
        uint16_t c = rd16le(code + b_at - 2);
        if ((c & 0xF800) == 0x2800 && ((c >> 8) & 7) == rm) k = c & 0xFF;
    }
    if (k < 0 && b_at >= 4) {                      /* cmp.w rI, #imm8 */
        uint16_t c1 = rd16le(code + b_at - 4), c2 = rd16le(code + b_at - 2);
        if ((c1 & 0xFBF0) == 0xF1B0 && (c1 & 0xF) == rm &&
            (c2 & 0xFF00) == 0x0F00 && !(c1 & 0x0400))
            k = c2 & 0xFF;
    }
    if (k < 0) return 0;
    return cond == 0x8 ? (uint32_t)k + 1 : (uint32_t)k;
}

uint32_t arm_table_target(const uint8_t *code, uint32_t code_addr,
                          const arm_insn *in, uint32_t i) {
    const uint32_t tab = in->addr + 4 - code_addr;
    const uint32_t e = in->op == OP_TBH ? rd16le(code + tab + 2 * i)
                                        : code[tab + i];
    return in->addr + 4 + 2 * e;
}

static void bound_table(const uint8_t *code, uint32_t code_addr,
                        uint32_t code_len, arm_insn *o) {
    if (o->rn != 15 || o->rm < 0 || o->rm > 14) return;
    uint32_t n = table_bound(code, code_addr, o->addr, o->rm);
    if (n == 0 || n > 4096) return;
    const uint32_t tab_end = o->addr + 4 + n * (o->op == OP_TBH ? 2 : 1);
    if (tab_end - code_addr > code_len) return;
    o->table_n = (uint16_t)n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t = arm_table_target(code, code_addr, o, i);
        if (t < tab_end || t - code_addr >= code_len) { o->table_n = 0; return; }
    }
}

/* --- entry point ------------------------------------------------------------ */

int arm_decode(const uint8_t *code, uint32_t code_addr, uint32_t code_len,
               uint32_t addr, arm_mode mode, arm_insn *out) {
    memset(out, 0, sizeof(*out));
    out->addr = addr;
    out->mode = mode;
    out->mnemonic = "?";
    /* Registers default to "absent" rather than r0, so an emitter that reads a
     * field the decoder never filled produces an obvious error instead of a
     * silent reference to the wrong register. */
    out->rd = out->rn = out->rm = out->rt = out->rt2 = ARM_NO_REG;
    out->vd = out->vn = out->vm = ARM_NO_REG;
    out->cond = ARM_COND_AL;

    if (addr < code_addr) return 0;
    uint32_t off = addr - code_addr;

    if (mode == ARM_A32) {
        if (off + 4 > code_len) return 0;
        out->width = 4;
        decode_a32(rd32le(code + off), out);
        return 4;
    }

    if (off + 2 > code_len) return 0;
    uint16_t h1 = rd16le(code + off);

    /* Thumb width, from the rule rather than a table: the first halfword
     * introduces a 32-bit instruction when its top five bits are 11101, 11110
     * or 11111. Everything else is a complete 16-bit instruction. */
    int is32 = (h1 & 0xF800) == 0xE800
            || (h1 & 0xF800) == 0xF000
            || (h1 & 0xF800) == 0xF800;

    if (!is32) {
        out->width = 2;
        decode_t16(h1, out);
        return 2;
    }

    if (off + 4 > code_len) return 0;
    out->width = 4;
    decode_t32(h1, rd16le(code + off + 2), out);
    if (out->op == OP_TBB || out->op == OP_TBH)
        bound_table(code, code_addr, code_len, out);
    return 4;
}
