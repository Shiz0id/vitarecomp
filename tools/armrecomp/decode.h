/* decode.h — ARMv7-A instruction decoding, both instruction sets.
 *
 * The Vita runs a Cortex-A9. Its code is predominantly Thumb-2 with a real ARM
 * minority, so a decoder that handles only one of them silently misreads the
 * other — and because both are dense encodings, misreading produces plausible
 * instructions rather than obvious garbage.
 *
 * SCOPE. This decoder determines, for every instruction:
 *
 *   - its WIDTH   (2 or 4 bytes) — required to advance correctly at all
 *   - its CLASS   (ALU / load / store / branch / SIMD / ...)
 *   - its CONTROL FLOW (target, conditionality, call vs return vs jump)
 *
 * It names common instructions but does not yet decode every operand; that
 * lands in phase 5 where the emitter needs it. The distinction matters because
 * width and control flow are what phase 4 needs, and getting those right for
 * 100% of instructions is worth more than getting operands right for 60%.
 *
 * Anything not recognised decodes to A_UNKNOWN rather than being skipped or
 * guessed at, so `cover` can report honestly how much of a module is
 * understood.
 */

#ifndef ARMRECOMP_DECODE_H
#define ARMRECOMP_DECODE_H

#include <stdint.h>

typedef enum {
    ARM_A32 = 0,    /* 4-byte ARM instructions          */
    ARM_T32 = 1,    /* 2- or 4-byte Thumb-2             */
} arm_mode;

typedef enum {
    A_UNKNOWN = 0,  /* not recognised — counted, never guessed at   */
    A_UNDEF,        /* a genuinely UNDEFINED encoding                */
    A_ALU,
    A_LOAD,
    A_STORE,
    A_LOADM,        /* LDM / POP                                     */
    A_STOREM,       /* STM / PUSH                                    */
    A_BRANCH,       /* B, conditional or not                         */
    A_CALL,         /* BL / BLX                                      */
    A_RETURN,       /* BX LR, POP {..,pc}, MOV pc,lr                 */
    A_INDIRECT,     /* BX Rm, other computed transfers               */
    A_SIMD,         /* NEON / VFP — identified, not yet translated   */
    A_SYS,          /* SVC, MRS/MSR, barriers, coprocessor           */
    A_NOP,
} arm_class;

/* The specific operation, for instructions the emitter can translate. Anything
 * left as OP_NONE is decoded well enough to advance and classify but not well
 * enough to emit, and becomes a named run-time trap rather than silence. */
typedef enum {
    OP_NONE = 0,
    OP_MOV, OP_MOVT, OP_MVN, OP_ADD, OP_ADC, OP_SUB, OP_SBC, OP_RSB,
    OP_AND, OP_ORR, OP_EOR, OP_BIC, OP_ORN,
    OP_CMP, OP_CMN, OP_TST,
    OP_LSL, OP_LSR, OP_ASR, OP_ROR,
    OP_MUL,
    OP_LDR, OP_LDRB, OP_LDRH, OP_LDRSB, OP_LDRSH, OP_LDRD,
    OP_STR, OP_STRB, OP_STRH, OP_STRD,
    OP_PUSH, OP_POP,
    OP_B, OP_BL, OP_BX, OP_BLX,
    OP_ADR,
    OP_SXTB, OP_SXTH, OP_UXTB, OP_UXTH, OP_REV,
    OP_IT, OP_NOP, OP_SVC, OP_CBZ, OP_CBNZ,
    OP_SBFX, OP_UBFX, OP_BFI, OP_BFC,
    OP_CLZ,
    OP_UADD8, OP_USUB8, OP_UADD16, OP_USUB16, OP_SEL,   /* GE-setting / reading */
    OP_TBB, OP_TBH,             /* table branch; table_n entries follow    */
    OP_TEQ,                     /* flags from rn ^ op2; NOT TST's rn & op2   */
    OP_LDREX, OP_STREX,         /* xsize bytes; STREX status in rd          */
    OP_CLREX, OP_DMB,           /* DMB stands for DMB/DSB/ISB: a full fence */
    OP_MLA, OP_MLS,                     /* rd = ra +/- rn * rm; ra in rt          */
    OP_UMULL, OP_SMULL, OP_UMLAL, OP_SMLAL, /* rt = RdLo, rt2 = RdHi            */
    OP_LDM, OP_STM,                     /* general base; `decrement` for DB       */

    /* Scalar VFP. Not vector work — 82% of this platform's coprocessor traffic
     * is ordinary floating point, and it maps onto C almost one to one. */
    OP_VLDR, OP_VSTR,
    OP_VADD, OP_VSUB, OP_VMUL, OP_VDIV,
    OP_VABS, OP_VNEG, OP_VSQRT,
    OP_VMOV,        /* register to register                        */
    OP_VMOV_TO_C,   /* VFP register -> core register               */
    OP_VMOV_TO_V,   /* core register -> VFP register               */
    OP_VCMP,
    OP_VCVT_F2I, OP_VCVT_I2F,
    OP_VMRS,
    OP_VLDM, OP_VSTM,   /* register-list float load/store, incl. VPUSH/VPOP */
    OP_VMLA, OP_VMLS,   /* multiply-accumulate: vd = vd +/- (vn * vm)       */
    OP_VNMUL,
    OP_VNMLA, OP_VNMLS, /* vd = -vd - (vn * vm)  /  vd = -vd + (vn * vm)   */
    OP_VMOV_IMM,        /* vd = VFPExpandImm(imm8), the bits held in imm   */

    /* Advanced SIMD (NEON). Registers are D numbers 0-31 in vd/vn/vm; a Q
     * operation (neon_q) covers the aligned pair d, d+1. */
    OP_NEON_ADD, OP_NEON_SUB,   /* integer, lanes of 8 << neon_size bits   */
    OP_NEON_LOGIC,              /* neon_fn: an arm_neon_logic              */
    OP_NEON_IMM,                /* neon_fn: an arm_neon_logic; imm64 below */
    OP_NEON_CEQ, OP_NEON_TST,   /* lanes all-ones where equal / where n&m != 0 */
    OP_NEON_PADD,               /* pairwise add, D registers only           */
    OP_NEON_MOVN,               /* narrow: Dd lanes = low halves of Qm lanes */
    OP_NEON_DUP,                /* every lane of vd = core rt               */
    OP_NEON_TO_SCALAR,          /* vd[imm] = rt, lane of 8 << neon_size bits */
    OP_NEON_FROM_SCALAR,        /* rt = vn[imm], zero/sign by vfp_unsigned   */
    OP_NEON_VLD1, OP_NEON_VST1, /* imm D registers from vd, at [rn]; rm is the
                                   post-increment register, writeback alone
                                   means "by the transfer size"              */
    OP_NEON_VLDN, OP_NEON_VSTN, /* VLD2-4/VST2-4 (multiple structures): imm
                                   members, each neon_q ? a Q pair : a D,
                                   interleaved in memory; see neon_inc        */
    OP_NEON_NARROW_SHR, OP_NEON_NARROW_RSHR, /* VSHRN, VRSHRN: Qm narrowed to Dd by
                                   imm, truncated to neon_size; RSHR rounds  */
    OP_NEON_NARROW_QSHR, OP_NEON_NARROW_QRSHR, /* VQSHRN, VQRSHRN, and VQMOVN (imm 0):
                                   source and result signed or unsigned by
                                   vfp_unsigned, saturated                    */
    OP_NEON_NARROW_QSHRU, OP_NEON_NARROW_QRSHRU, /* VQSHRUN, VQRSHRUN, and VQMOVUN
                                   (imm 0): signed source, unsigned result;
                                   vfp_unsigned is not read                   */
    OP_NEON_FADD, OP_NEON_FSUB, /* float .F32 lanes, standard FPSCR          */
    OP_NEON_FMUL,               /* float, vd = vn * vm                       */
    OP_NEON_FMLA, OP_NEON_FMLS, /* vd = vd +/- round(vn * vm), not fused     */
    OP_NEON_FMUL_SCALAR,        /* vd = vn * vm[imm], vm a D register        */
    OP_NEON_FMLA_SCALAR,        /* vd = vd + vn * vm[imm], vm a D register   */
    OP_NEON_FMLS_SCALAR,        /* vd = vd - vn * vm[imm], vm a D register   */
    OP_NEON_FMAX, OP_NEON_FMIN, /* float; a NaN operand gives the default NaN */
    OP_NEON_FCEQ, OP_NEON_FCGE, OP_NEON_FCGT, /* lanes all-ones where true  */
    OP_NEON_FABS, OP_NEON_FNEG, /* sign bit only: no flush, NaN payload kept  */
    OP_NEON_FCVT_TO_INT,        /* F32 to S32 or U32 (vfp_unsigned), toward 0 */
    OP_NEON_FCVT_FROM_INT,      /* S32 or U32 (vfp_unsigned) to F32           */
    OP_NEON_CMPZ, OP_NEON_FCMPZ, /* compare with zero: imm is the condition, 0 GT,
                                    1 GE, 2 EQ, 3 LE, 4 LT; FCMPZ is F32 lanes  */
    OP_NEON_ABS, OP_NEON_NEG,   /* integer absolute value and negate, wrapping */
    OP_NEON_MUL, OP_NEON_MLA, OP_NEON_MLS, /* integer, lanes of 8/16/32, wrapping */
    OP_NEON_QADD, OP_NEON_QSUB, /* saturating; vfp_unsigned picks the signedness */
    OP_NEON_CGT, OP_NEON_CGE,   /* integer compares, all-ones where true; vfp_unsigned */
    OP_NEON_MAX, OP_NEON_MIN,   /* integer max and min; vfp_unsigned picks the signedness */
    OP_NEON_ABD, OP_NEON_ABA,   /* absolute difference; ABA adds it to vd, vfp_unsigned */
    OP_NEON_SHL_REG, OP_NEON_QSHL_REG,   /* by register: the value is vm, the amounts
                                            the low byte of each vn lane; vfp_unsigned
                                            picks the signedness; Q saturates left  */
    OP_NEON_RSHL_REG, OP_NEON_QRSHL_REG, /* the same, with a rounding right shift   */
    OP_NEON_HADD, OP_NEON_RHADD, OP_NEON_HSUB, /* halving add, rounding halving add and
                                   halving subtract: floor of the exact result / 2;
                                   vfp_unsigned picks the signedness             */
    OP_NEON_PMAX, OP_NEON_PMIN, /* pairwise max and min, D registers only;
                                   vfp_unsigned picks the signedness             */
    OP_NEON_SHL, OP_NEON_SHR, OP_NEON_SRA, /* by immediate: imm is the amount,
                                   neon_size the lane; SHR and SRA use
                                   vfp_unsigned for the signedness            */
    OP_NEON_ADDL, OP_NEON_SUBL, /* widening: vd (a Q pair) = vn ext +/- vm ext,
                                   vn and vm D registers, neon_size the SOURCE
                                   lane, vfp_unsigned the signedness           */
    OP_NEON_ADDW, OP_NEON_SUBW, /* as ADDL/SUBL, but vn is a Q pair (even)    */
    OP_NEON_MULL,               /* vd = vn ext * vm ext                       */
    OP_NEON_MLAL, OP_NEON_MLSL, /* vd = vd +/- vn ext * vm ext                */
    OP_NEON_SHLL,               /* vd = vm ext << imm, imm 0 is VMOVL. The
                                   widening ops write the pair vd, vd+1 and
                                   do not use neon_q, as MOVN does            */
    OP_NEON_EXT,                /* vd = bytes imm.. of vn:vm, imm a byte offset */
    OP_NEON_REV,                /* reverse elements in each imm-bit container  */
    OP_NEON_SWP, OP_NEON_TRN, OP_NEON_UZP, OP_NEON_ZIP, /* permutes, neon_size lane */
    OP_NEON_DUP_LANE,           /* every lane of vd = lane imm of D register vm */
    OP_NEON_TBL, OP_NEON_TBX,   /* imm D registers from vn as a table; TBX keeps
                                   the vd byte where the index is out of range  */
    OP_VMOV_V_RR,               /* two core regs (rt, rt2) -> a D register or
                                   an S pair, vm naming it                   */
    OP_VMOV_RR_V,               /* the reverse                              */
    OP_VCVT_F2F,                /* between precisions; vfp_dp: source is double */
    OP_REV16, OP_REVSH, OP_RBIT,
    OP_SXTB16, OP_UXTB16,       /* the extends: rotate by shift_amt first,
                                   then add rn when it is a register         */
    OP_SMULXY, OP_SMLAXY,       /* 16x16 multiply, halves by half_sel; rt = Ra */
} arm_op;

typedef enum {
    NL_AND, NL_BIC, NL_ORR, NL_ORN, NL_EOR, NL_BSL, NL_BIT, NL_BIF,
    NL_MOV, NL_MVN,             /* immediate forms only */
} arm_neon_logic;

/* Shift types, in the architecture's encoding order. SH_RRX is not an encoding
 * of its own: it is ROR with an amount of zero, which the decoder rewrites so
 * nothing downstream has to remember that a rotate by 0 is a rotate by 1
 * through carry. LSR/ASR #0 likewise arrive as shift_amt 32, their meaning. */
typedef enum { SH_LSL = 0, SH_LSR = 1, SH_ASR = 2, SH_ROR = 3, SH_RRX = 4 } arm_shift;

#define ARM_COND_AL 0xE
#define ARM_NO_REG  (-1)

typedef struct {
    uint32_t   addr;
    uint32_t   raw;         /* 16-bit instructions occupy the low half */
    uint8_t    width;       /* 2 or 4                                  */
    arm_mode   mode;
    arm_class  cls;
    const char *mnemonic;   /* never NULL; "?" when unknown            */

    /* Control flow, filled in when the class is a branch form. */
    int        has_target;
    uint32_t   target;
    int        conditional;
    int        switches_mode;   /* BLX / BX to the other instruction set */

    /* --- operands (phase 5) ------------------------------------------------ */
    arm_op     op;
    int8_t     rd, rn, rm, rt;  /* ARM_NO_REG when unused                */
    int8_t     rt2;             /* second transfer register: LDRD/STRD    */

    /* VFP register numbers. Single-precision registers are numbered 0-31;
     * double-precision 0-15, aliased onto the same storage. */
    int8_t     vd, vn, vm;
    uint8_t    vfp_dp;          /* 1 = double precision                   */
    uint8_t    vfp_unsigned;    /* VCVT: unsigned integer form            */
    uint32_t   imm;
    uint8_t    has_imm;
    /* The immediate came from a rotation, and so has a shifter carry-out: a
     * flag-setting logical op sets C to its bit 31. A replicated or plain
     * immediate leaves C alone. */
    uint8_t    imm_carry;
    uint8_t    sets_flags;      /* writes NZCV                           */
    uint8_t    shift_type;      /* arm_shift                             */
    uint8_t    shift_amt;
    uint8_t    shift_is_reg;
    uint16_t   reglist;         /* PUSH/POP/LDM/STM                      */
    uint8_t    cond;            /* ARM_COND_AL when unconditional        */
    uint8_t    writeback;       /* LDR/STR/LDRD/STRD: rn is updated by imm */
    uint8_t    postindex;       /* ...after the access, which uses rn alone */
    uint8_t    decrement;       /* LDM/STM: DB (decrement before) vs IA   */
    uint8_t    neon_q;          /* NEON: quadword (two D registers)       */
    uint8_t    neon_size;       /* NEON: lane size, 0..3 = 8..64 bits      */
    uint8_t    neon_fn;         /* NEON: arm_neon_logic                    */
    uint64_t   imm64;           /* NEON: expanded modified immediate       */
    uint8_t    mem_add;         /* U bit: offset added rather than subtracted */
    uint8_t    it_mask;         /* IT: which of the next 4 are conditional */
    uint16_t   table_n;         /* TBB/TBH: entries, 0 when not bounded    */
    uint8_t    xsize;           /* LDREX/STREX: 1, 2, 4, or 8 (the D form)  */

    /* VLDN/VSTN: the register step from one member to the next, 1 or 2. Kept
     * out of `imm` (the member count) and out of neon_q/neon_size, which give
     * the words per member and the element size; no one of those fields can
     * carry a third value without the others changing meaning. */
    uint8_t    neon_inc;

    /* Bitfield ops. Kept out of `imm`/`shift_amt` because a field named for
     * something else is how a reader ends up trusting the wrong one. */
    uint8_t    bf_lsb;
    uint8_t    bf_width;

    /* SMULxy/SMLAxy: bit 1 takes rn's top half, bit 0 rm's. */
    uint8_t    half_sel;
} arm_insn;

const char *arm_cond_name(uint8_t cond);

const char *arm_class_name(arm_class c);

/* Decode one instruction at `addr` from `code` (which begins at `code_addr`).
 * Returns the width consumed, or 0 if there is not enough input left.
 *
 * `mode` is the current instruction set. The decoder never guesses it: mode is
 * caller state, changed only by an explicit interworking branch, because there
 * is no reliable way to infer it from a single instruction. */
int arm_decode(const uint8_t *code, uint32_t code_addr, uint32_t code_len,
               uint32_t addr, arm_mode mode, arm_insn *out);

/* Apply an IT block to the instructions it covers.
 *
 * IT makes up to the next four instructions conditional, but each of them
 * decodes on its own as unconditional — the condition lives in the processor's
 * ITSTATE, not in the instruction. So anything that decodes a straight run of
 * Thumb code keeps one `itstate` byte, zeroed at the start of each block, and
 * passes every decoded instruction through here in order. A covered
 * instruction gets its condition and `conditional` set, and the 16-bit ALU
 * forms lose their flag update: outside an IT block `movs`/`adds` set flags,
 * inside one the same encodings do not (CMP, CMN and TST still do). */
void arm_it_step(uint8_t *itstate, arm_insn *in);

/* Entry `i` of a TBB/TBH's table: the address it branches to. Only valid
 * for i < in->table_n, which the decoder sets only when the table's size is
 * proven by the bounds check guarding it and every entry lands inside `code`. */
uint32_t arm_table_target(const uint8_t *code, uint32_t code_addr,
                          const arm_insn *in, uint32_t i);

#endif /* ARMRECOMP_DECODE_H */
