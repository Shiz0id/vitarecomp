/* test_decode.c â€” ARMv7 decoding, on hand-built encodings only.
 *
 * Every encoding here is assembled in this file from the architecture's rules
 * and its expected result worked out by hand, so a wrong decoder disagrees with
 * arithmetic rather than with a blob somebody once observed.
 */

#include "decode.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

/* Lay out little-endian halfwords at a notional base address. */
static uint8_t code[64];

static void put16(int off, uint16_t v) {
    code[off]     = (uint8_t)(v & 0xFF);
    code[off + 1] = (uint8_t)(v >> 8);
}

static void put32(int off, uint32_t v) {
    code[off]     = (uint8_t)(v);
    code[off + 1] = (uint8_t)(v >> 8);
    code[off + 2] = (uint8_t)(v >> 16);
    code[off + 3] = (uint8_t)(v >> 24);
}

#define BASE 0x1000u

static arm_insn dec(arm_mode mode, uint32_t addr) {
    arm_insn in;
    int n = arm_decode(code, BASE, sizeof(code), addr, mode, &in);
    assert(n == in.width);
    return in;
}

/* --- Thumb width: the thing that must never be wrong ------------------------ */
/*
 * A wrong width does not produce one wrong instruction, it desynchronises the
 * rest of the run and produces a long stretch of plausible nonsense. The rule
 * is that first halfwords 0xE800-0xFFFF introduce a 32-bit instruction.
 */

static void test_thumb_width(void) {
    memset(code, 0, sizeof(code));

    /* Just below the boundary: still 16-bit. */
    put16(0, 0xE7FE);                      /* b . (unconditional, 16-bit)    */
    assert(dec(ARM_T32, BASE).width == 2);

    /* At and above the boundary: 32-bit. All three prefixes. */
    put16(0, 0xE800); put16(2, 0x0000);
    assert(dec(ARM_T32, BASE).width == 4);
    put16(0, 0xF000); put16(2, 0xB800);
    assert(dec(ARM_T32, BASE).width == 4);
    put16(0, 0xF800); put16(2, 0x0000);
    assert(dec(ARM_T32, BASE).width == 4);

    /* An ordinary 16-bit data-processing instruction stays 2 bytes. */
    put16(0, 0x4478);                      /* add r0, pc                     */
    assert(dec(ARM_T32, BASE).width == 2);

    printf("  thumb width boundary           ok\n");
}

/* --- returns, in both instruction sets -------------------------------------- */

static void test_returns(void) {
    memset(code, 0, sizeof(code));

    put16(0, 0x4770);                      /* bx lr                          */
    assert(dec(ARM_T32, BASE).cls == A_RETURN);

    put16(0, 0xBD00);                      /* pop {pc}                       */
    assert(dec(ARM_T32, BASE).cls == A_RETURN);

    put16(0, 0xBC00);                      /* pop {} â€” no pc, not a return   */
    assert(dec(ARM_T32, BASE).cls == A_LOADM);

    put32(0, 0xE12FFF1E);                  /* bx lr (ARM)                    */
    assert(dec(ARM_A32, BASE).cls == A_RETURN);

    put32(0, 0xE8BD8000);                  /* ldm sp!, {pc} (ARM)            */
    assert(dec(ARM_A32, BASE).cls == A_RETURN);

    put32(0, 0xE1A0F00E);                  /* mov pc, lr (ARM)               */
    assert(dec(ARM_A32, BASE).cls == A_RETURN);

    printf("  returns, both modes            ok\n");
}

/* --- writing r15 is control flow, not arithmetic ---------------------------- */
/*
 * The high-register ADD/MOV group can name r15 as its destination, and doing so
 * is a BRANCH. Classifying it as an ALU write emits an assignment where a jump
 * belongs, and the recompiled function runs straight past a transfer it should
 * have taken. This was caught only because the runtime has no `pc` variable, so
 * the generated C refused to compile; with a `pc` present it would have built
 * cleanly and been silently wrong.
 */

static void test_pc_destination(void) {
    memset(code, 0, sizeof(code));

    /* rd is (DN << 3) | rd_low, split across bits 7 and 2:0. 0x46B7 is
     * rd = (1<<3)|7 = 15; 0x46B6 is rd = 14 and is an ordinary move to LR.
     * The two differ by one bit, which is why the split field is worth
     * spelling out here. */
    put16(0, 0x46B7);                      /* mov pc, r6  (rd=15, rm=6)      */
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.cls == A_INDIRECT);
    assert(in.cls != A_ALU);

    put16(0, 0x46B6);                      /* mov lr, r6 â€” rd=14, still ALU  */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_ALU && in.op == OP_MOV && in.rd == 14);

    put16(0, 0x46F7);                      /* mov pc, lr                     */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_RETURN);

    put16(0, 0x4487);                      /* add pc, r0  (rd=15)            */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_INDIRECT);

    /* An ordinary high-register move is still arithmetic. */
    put16(0, 0x4630);                      /* mov r0, r6                     */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_ALU && in.op == OP_MOV && in.rd == 0 && in.rm == 6);

    /* CMP never writes its destination, so r15 there is a normal operand. */
    put16(0, 0x45B7);                      /* cmp r15, r6                    */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_ALU && in.op == OP_CMP);

    printf("  r15 destination is a branch    ok\n");
}

/* --- ThumbExpandImm --------------------------------------------------------- */
/*
 * Two encodings share one 12-bit field, and the naive "it is just a 12-bit
 * integer" reading is correct for small constants and wrong for everything
 * else â€” which is exactly the shape of bug that survives casual testing. Each
 * case is checked against a value worked out from the definition.
 */

static void test_thumb_expand_imm(void) {
    memset(code, 0, sizeof(code));

    /* mov.w rd, #imm â€” 0xF04F is ORR with rn=15, which is MOV. */

    /* Pattern 00: the byte, unchanged. */
    put16(0, 0xF04F); put16(2, 0x0042);
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOV && in.has_imm && in.imm == 0x42);

    /* Pattern 01: byte replicated into halves -> 0x00XY00XY. */
    put16(0, 0xF04F); put16(2, 0x1042);
    in = dec(ARM_T32, BASE);
    assert(in.imm == 0x00420042u);

    /* Pattern 10: byte replicated, shifted up -> 0xXY00XY00. */
    put16(0, 0xF04F); put16(2, 0x2042);
    in = dec(ARM_T32, BASE);
    assert(in.imm == 0x42004200u);

    /* Pattern 11: byte in all four positions. */
    put16(0, 0xF04F); put16(2, 0x3042);
    in = dec(ARM_T32, BASE);
    assert(in.imm == 0x42424242u);

    /* The rotate form. The stored 7 bits carry an implicit leading 1, so the
     * value rotated is 0x80|imm7, not imm7 â€” the single easiest thing here to
     * get wrong, and it leaves every such constant short by 0x80.
     *
     * i:imm3:a = 01000 gives a rotate of 8 applied to 0x80: 0x80000000. */
    put16(0, 0xF04F); put16(2, 0x4000);
    in = dec(ARM_T32, BASE);
    assert(in.imm == 0x80000000u);

    printf("  thumb expand immediate         ok\n");
}

/* --- 32-bit Thumb operands -------------------------------------------------- */

static void test_t32_operands(void) {
    memset(code, 0, sizeof(code));

    /* add.w r0, r1, r2 */
    put16(0, 0xEB01); put16(2, 0x0002);
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.width == 4 && in.op == OP_ADD);
    assert(in.rd == 0 && in.rn == 1 && in.rm == 2 && !in.sets_flags);

    /* adds.w r0, r1, r2 â€” the S bit is bit 4 of the first halfword. */
    put16(0, 0xEB11); put16(2, 0x0002);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_ADD && in.sets_flags);

    /* orr.w with rn == 15 is MOV, not a real ORR against r15. */
    put16(0, 0xEA4F); put16(2, 0x0001);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOV && in.rn == ARM_NO_REG && in.rm == 1);

    /* sub.w with rd == 15 and S set is CMP: flags only, no destination. */
    put16(0, 0xEBB1); put16(2, 0x0F02);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_CMP && in.rd == ARM_NO_REG);

    /* movw r0, #0x1234 â€” a plain 16-bit literal, not a modified immediate. */
    put16(0, 0xF241); put16(2, 0x2034);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOV && in.imm == 0x1234);

    /* ldr.w r0, [r1, #0x100] â€” 12-bit unsigned offset form. */
    put16(0, 0xF8D1); put16(2, 0x0100);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDR && in.rt == 0 && in.rn == 1 && in.imm == 0x100);

    /* str.w r0, [r1, #0x100] */
    put16(0, 0xF8C1); put16(2, 0x0100);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STR && in.rt == 0 && in.rn == 1);

    /* movt r0, #0x8100 â€” writes the TOP half only.
     *
     * MOVW/MOVT is how ARM builds a 32-bit address. Leaving MOVT untranslated
     * costs every constructed pointer its high 16 bits, which a trace of
     * module_start showed as indirect branches to 0x00000000. */
    /* rd is bits 11:8 of the SECOND halfword, so 0x1000 means r0. The 16-bit
     * literal is assembled from imm4:i:imm3:imm8 spread across both halfwords:
     * 8:0:1:0x00 gives 0x8100. */
    put16(0, 0xF2C8); put16(2, 0x1000);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOVT && in.rd == 0 && in.imm == 0x8100);

    /* strd r1, r0, [sp] â€” bit 6 separates dual from multiple. An earlier mask
     * kept that bit and so excluded every LDRD/STRD from decoding at all. */
    put16(0, 0xE9CD); put16(2, 0x1000);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STRD && in.rn == 13 && in.rt == 1 && in.rt2 == 0);

    put16(0, 0xE9DD); put16(2, 0x1200);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDRD && in.rn == 13 && in.rt == 1 && in.rt2 == 2);

    /* mul.w r0, r1, r2 â€” ra == 15 distinguishes it from MLA. */
    put16(0, 0xFB01); put16(2, 0xF002);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MUL && in.rd == 0 && in.rn == 1 && in.rm == 2);

    /* push.w {r4,lr} / pop.w â€” only the SP-relative forms translate as stack
     * operations; an LDM against another base must not. */
    put16(0, 0xE92D); put16(2, 0x4010);
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_STOREM && in.op == OP_PUSH && in.rn == 13);

    put16(0, 0xE8B1); put16(2, 0x0030);      /* ldm r1!, {r4,r5}: base r1 */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_LOADM && in.op == OP_LDM && in.rn == 1 &&
           in.writeback && !in.decrement && in.reglist == 0x0030);

    printf("  32-bit thumb operands          ok\n");
}

/* --- operand decoding ------------------------------------------------------- */

static void test_operands(void) {
    memset(code, 0, sizeof(code));

    /* 16-bit data processing sets flags implicitly â€” there is no S bit. */
    put16(0, 0x1C41);                      /* adds r1, r0, #1                */
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.op == OP_ADD && in.rd == 1 && in.rn == 0);
    assert(in.has_imm && in.imm == 1 && in.sets_flags);

    put16(0, 0x2042);                      /* movs r0, #0x42                 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOV && in.rd == 0 && in.imm == 0x42 && in.sets_flags);

    /* Load/store immediates are scaled by the ACCESS SIZE, so the byte form is
     * not the word form with a smaller range. */
    put16(0, 0x6841);                      /* ldr r1, [r0, #4]               */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDR && in.rt == 1 && in.rn == 0 && in.imm == 4);

    put16(0, 0x7841);                      /* ldrb r1, [r0, #1]              */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDRB && in.imm == 1);

    put16(0, 0x8841);                      /* ldrh r1, [r0, #2]              */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDRH && in.imm == 2);

    /* PUSH/POP register lists, including the extra bit for LR and PC. */
    put16(0, 0xB570);                      /* push {r4,r5,r6,lr}             */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_PUSH && in.reglist == (0x70 | 0x4000));

    put16(0, 0xBD70);                      /* pop {r4,r5,r6,pc}              */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_POP && (in.reglist & 0x8000) && in.cls == A_RETURN);

    printf("  operands                       ok\n");
}

/* --- the J1/J2 branch encoding ---------------------------------------------- */
/*
 * The two J bits are stored XORed with the sign bit. A decoder that treats them
 * as plain address bits is correct for short forward branches and wrong for
 * long or backward ones, which is exactly the bug that survives casual testing.
 * Both directions are checked, with the encodings derived by hand.
 */

/* The bitfield group shares its 5-bit op field with MOVW/MOVT/ADDW, and the
 * position of the field is encoded three different ways across four
 * instructions: SBFX/UBFX give a width, BFI gives an end bit, BFC gives an end
 * bit and no source. Reading any of them as another silently shifts data. */
static void test_bitfield(void) {
    memset(code, 0, sizeof(code));

    /* ubfx r0, r1, #4, #8 */
    put16(0, 0xF3C1); put16(2, 0x1007);
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.width == 4 && in.op == OP_UBFX);
    assert(in.rd == 0 && in.rn == 1 && in.bf_lsb == 4 && in.bf_width == 8);

    /* sbfx r0, r1, #4, #8 — same operands, one bit of op apart. */
    put16(0, 0xF341); put16(2, 0x1007);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_SBFX && in.bf_lsb == 4 && in.bf_width == 8);

    /* bfi r0, r1, #4, #8 — encoded as msb = 11, so width must be derived. */
    put16(0, 0xF361); put16(2, 0x100B);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_BFI && in.rn == 1 && in.bf_lsb == 4 && in.bf_width == 8);

    /* bfc r0, #4, #8 — the same encoding with rn == 15, which is "no source"
     * and not a reference to the PC. */
    put16(0, 0xF36F); put16(2, 0x100B);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_BFC && in.rn == ARM_NO_REG && in.bf_width == 8);

    /* ubfx running off the end of the register (lsb 28, width 8) is
     * UNPREDICTABLE, and a linear sweep over a literal pool produces it. It
     * must trap rather than emit a clamped extract. */
    put16(0, 0xF3C1); put16(2, 0x7007);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && strcmp(in.mnemonic, "ubfx") == 0);

    /* ssat lives in the same group and is not bitfield work; it is named
     * separately so the trap report does not overstate what is left. */
    put16(0, 0xF300); put16(2, 0x0000);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && strcmp(in.mnemonic, "sat") == 0);

    /* movw still matches: it sets bit 6, which the group's op field shares. */
    put16(0, 0xF241); put16(2, 0x2034);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOV && in.imm == 0x1234);

    printf("  bitfield                       ok\n");
}

/* The four encodings sharing the 32-bit branch group are selected by two bits
 * of the second halfword: bit 14 (call or branch) and bit 12 (which of the
 * pair). Masking bits 14:12 together and testing `& 5` conflates them, which
 * decoded BLX-immediate as B.W and sent real B.W down the CONDITIONAL path,
 * where it read a cond field that does not exist in its encoding.
 *
 * Encodings below are assembled from the architecture's rules, and the
 * expected targets worked out by hand.
 */
static void test_wide_branch_group(void) {
    memset(code, 0, sizeof(code));

    /* BLX immediate. h2: bit15=1, bit14=1, bit13=J1, bit12=0, bit11=J2,
     * bits10:1=imm10L, bit0=H(must be 0). With S=0, J1=J2=1 gives I1=I2=0, so
     * the offset is imm10L*4 = 4 here.
     *
     * The property that matters: the base is Align(PC,4), NOT PC, because the
     * destination is ARM code. Placed at an ODD halfword the two differ, and
     * using PC lands two bytes high — which is how a tail call into an import
     * stub arrived two bytes past the stub. */
    put16(2, 0xF000); put16(4, 0xE802);
    arm_insn in = dec(ARM_T32, BASE + 2);
    assert(in.width == 4);
    assert(in.cls == A_CALL);          /* a call, not a branch */
    assert(in.switches_mode);          /* and it changes instruction set */
    assert(in.has_target);
    /* pc = 0x1006, Align(pc,4) = 0x1004, +4 => 0x1008. Using pc would give
     * 0x100A, and an ARM target is never at an odd halfword. */
    assert(in.target == BASE + 8);
    assert((in.target & 3) == 0);

    /* The SAME encoding one halfword later, where PC is already aligned. The
     * offset is identical, so the target must move by 4 — not by 2. That is
     * the assertion the old code could not pass. */
    put16(4, 0xF000); put16(6, 0xE802);
    in = dec(ARM_T32, BASE + 4);
    assert(in.cls == A_CALL && in.target == BASE + 12);

    /* B.W: bit14=0, bit12=1. Unconditional, a branch, and NOT a call. */
    put16(8, 0xF000); put16(10, 0xB802);
    in = dec(ARM_T32, BASE + 8);
    assert(in.cls == A_BRANCH && in.op == OP_B);
    assert(!in.conditional && in.cond == ARM_COND_AL);
    assert(!in.switches_mode);
    assert(in.target == BASE + 16);    /* pc(0x100C) + 4 */

    /* BL: bit14=1, bit12=1. A call that does not change instruction set, and
     * unlike BLX its offset is a multiple of two, not four. */
    put16(16, 0xF000); put16(18, 0xF802);
    in = dec(ARM_T32, BASE + 16);
    assert(in.cls == A_CALL && !in.switches_mode);
    assert(in.target == BASE + 24);    /* pc(0x1014) + 4 */

    /* B<cond>.W: bit14=0, bit12=0. Keeps T3's own layout and a real
     * condition, which is exactly what B.W must NOT be given. */
    put16(24, 0xF000); put16(26, 0x8000);
    in = dec(ARM_T32, BASE + 24);
    assert(in.cls == A_BRANCH && in.conditional);

    printf("  wide branch group, all four    ok\n");
}

static void test_bl_target(void) {
    memset(code, 0, sizeof(code));

    /* BL at 0x1000 -> 0x1100. offset = 0x1100 - 0x1004 = +0xFC.
     * S=0, so I1=I2=0, which means J1=J2=1. imm11 = 0xFC>>1 = 0x7E. */
    put16(0, 0xF000);
    put16(2, 0xF87E);
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.width == 4);
    assert(in.cls == A_CALL);
    assert(in.has_target);
    assert(in.target == 0x1100);

    /* BL at 0x1000 -> 0x0F00. offset = 0x0F00 - 0x1004 = -0x104.
     * S=1, I1=I2=1 so J1=J2=1; imm10=0x3FF, imm11=0x77E. */
    put16(0, 0xF7FF);
    put16(2, 0xFF7E);
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_CALL);
    assert(in.target == 0x0F00);

    printf("  bl target, both directions     ok\n");
}

static void test_branch_targets(void) {
    memset(code, 0, sizeof(code));

    /* 16-bit conditional branch: b<cond> +4 -> 0x1000+4+4 = 0x1008. */
    put16(0, 0xD002);                      /* cond=0 (eq), imm8=2            */
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.cls == A_BRANCH && in.conditional && in.target == 0x1008);

    /* 16-bit unconditional: b . -> 0x1000+4-2 = 0x1000 (a self-loop). */
    put16(0, 0xE7FE);
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_BRANCH && !in.conditional && in.target == BASE);

    /* ARM bl: pc is addr+8 on ARM, not addr+4. bl +0 -> 0x1008. */
    put32(0, 0xEB000000);
    in = dec(ARM_A32, BASE);
    assert(in.cls == A_CALL && in.target == 0x1008);

    /* ARM b backwards: imm24 = -2 -> 0x1000+8-8 = 0x1000. */
    put32(0, 0xEAFFFFFE);
    in = dec(ARM_A32, BASE);
    assert(in.cls == A_BRANCH && in.target == BASE);

    printf("  branch targets                 ok\n");
}

/* --- interworking ----------------------------------------------------------- */

static void test_mode_switches(void) {
    memset(code, 0, sizeof(code));

    put16(0, 0x4778);                      /* bx pc â€” switches to ARM        */
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.cls == A_INDIRECT && in.switches_mode);

    put16(0, 0x4780);                      /* blx r0                         */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_CALL && in.switches_mode);

    put32(0, 0xE12FFF30);                  /* blx r0 (ARM)                   */
    in = dec(ARM_A32, BASE);
    assert(in.cls == A_CALL && in.switches_mode);

    /* bx lr must NOT be flagged as a mode switch â€” it is a return, and
     * treating it as interworking would corrupt the decoder's mode state for
     * everything after it. */
    put16(0, 0x4770);
    assert(dec(ARM_T32, BASE).switches_mode == 0);

    printf("  interworking                   ok\n");
}

/* --- the ARM unconditional encoding space ----------------------------------- */

static void test_arm_cond_f(void) {
    memset(code, 0, sizeof(code));

    /* cond == 1111 is not "never" â€” it is a separate encoding space. Reading it
     * as a normal conditional instruction misdecodes all of it. */
    put32(0, 0xFA000000);                  /* blx imm                        */
    arm_insn in = dec(ARM_A32, BASE);
    assert(in.cls == A_CALL && in.switches_mode && !in.conditional);

    /* A genuinely conditional instruction still reports as conditional. */
    put32(0, 0x0A000000);                  /* beq                            */
    in = dec(ARM_A32, BASE);
    assert(in.cls == A_BRANCH && in.conditional);

    /* AL (1110) is unconditional in effect and must not be flagged. */
    put32(0, 0xEA000000);                  /* b                              */
    assert(dec(ARM_A32, BASE).conditional == 0);

    printf("  arm unconditional space        ok\n");
}

/* --- SIMD is identified, not decoded ---------------------------------------- */

/* --- scalar VFP ------------------------------------------------------------- */
/*
 * Register numbering is fiddly in opposite directions for the two precisions: a
 * single-precision register keeps its LOW bit in the separate D flag and its
 * high four in the main field, while a double-precision register does the
 * reverse. Getting it backwards yields a valid register number that is simply
 * the wrong one â€” no error, just a program reading floats it never wrote.
 */

static void test_vfp(void) {
    memset(code, 0, sizeof(code));

    /* vldr s0, [r1, #8] */
    put16(0, 0xED91); put16(2, 0x0A02);
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.op == OP_VLDR && in.rn == 1 && in.vd == 0);
    assert(in.imm == 8 && in.mem_add && !in.vfp_dp);

    /* vldr d0, [r1, #8] â€” coproc 11 selects double precision */
    put16(0, 0xED91); put16(2, 0x0B02);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VLDR && in.vfp_dp && in.vd == 0);

    /* The D bit is the LOW bit of a single-precision register number. With
     * D set, Vd=0 means s1 rather than s0. */
    put16(0, 0xEDD1); put16(2, 0x0A02);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VLDR && in.vd == 1 && !in.vfp_dp);

    /* ...but the HIGH bit of a double-precision one: D set, Vd=0 means d16. */
    put16(0, 0xEDD1); put16(2, 0x0B02);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VLDR && in.vfp_dp && in.vd == 16);

    /* vadd.f32 s0, s1, s2 / vsub.f32 â€” the op bit of the second halfword
     * separates them within one encoding. */
    put16(0, 0xEE30); put16(2, 0x0A81);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VADD && !in.vfp_dp);

    put16(0, 0xEE30); put16(2, 0x0AC1);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VSUB);

    /* vmul.f32 and vdiv.f32 */
    put16(0, 0xEE20); put16(2, 0x0A81);
    assert(dec(ARM_T32, BASE).op == OP_VMUL);
    put16(0, 0xEE80); put16(2, 0x0A81);
    assert(dec(ARM_T32, BASE).op == OP_VDIV);

    /* vcmp.f32 s0, s1 */
    put16(0, 0xEEB4); put16(2, 0x0A41);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCMP && in.vm != ARM_NO_REG);

    /* vcmp.f32 s0, #0 â€” compares against zero, so there is no second
     * register at all. */
    put16(0, 0xEEB5); put16(2, 0x0A40);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCMP && in.vm == ARM_NO_REG && in.has_imm);

    /* vmrs APSR_nzcv, fpscr â€” VFP comparisons write FPSCR, and only this
     * moves them where a conditional branch can see them. */
    put16(0, 0xEEF1); put16(2, 0xFA10);
    assert(dec(ARM_T32, BASE).op == OP_VMRS);

    /* vabs / vneg / vsqrt share an encoding, split by opc2 and opc3. */
    put16(0, 0xEEB0); put16(2, 0x0AC0);
    assert(dec(ARM_T32, BASE).op == OP_VABS);
    put16(0, 0xEEB1); put16(2, 0x0A40);
    assert(dec(ARM_T32, BASE).op == OP_VNEG);
    put16(0, 0xEEB1); put16(2, 0x0AC0);
    assert(dec(ARM_T32, BASE).op == OP_VSQRT);

    /* Advanced SIMD proper is still reported as SIMD, not claimed. */
    put16(0, 0xEF00); put16(2, 0x0000);
    assert(dec(ARM_T32, BASE).cls == A_SIMD);

    printf("  scalar vfp                     ok\n");
}

static void test_simd_identified(void) {
    memset(code, 0, sizeof(code));

    arm_insn in;
    put16(0, 0xEF00); put16(2, 0x0000);    /* advanced SIMD (Thumb)          */
    assert(dec(ARM_T32, BASE).cls == A_SIMD);

    /* VSTR shares the coprocessor space but is scalar VFP, not vector work, so
     * it is now claimed and translated rather than reported as opaque SIMD.
     * Most of this space is like that: 82% of it on a real module is scalar
     * floating point. */
    put16(0, 0xED00); put16(2, 0x0A00);    /* vstr s0, [r0]                  */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_STORE && in.op == OP_VSTR);
    assert(in.rn == 0 && in.vd == 0 && !in.vfp_dp);

    put32(0, 0xEE000A10);                  /* vmov (ARM)                     */
    assert(dec(ARM_A32, BASE).cls == A_SIMD);

    printf("  simd identified as a class     ok\n");
}

/* --- IT blocks -------------------------------------------------------------- */

static void test_it_block(void) {
    memset(code, 0, sizeof(code));

    /* IT makes up to the next four instructions conditional. It must be
     * distinguishable from the NOP-family hints that share its encoding
     * space, because phase 5 has to honour it. */
    put16(0, 0xBF08);                      /* it eq â€” mask nonzero           */
    arm_insn in = dec(ARM_T32, BASE);
    assert(in.cls == A_SYS && strcmp(in.mnemonic, "it") == 0);

    put16(0, 0xBF00);                      /* nop â€” mask zero                */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_NOP);

    printf("  it block vs nop                ok\n");
}

/* --- refusing to run off the end -------------------------------------------- */

static void test_bounds(void) {
    arm_insn in;
    /* A 32-bit Thumb instruction whose second halfword is past the end must
     * report "no instruction", not decode from uninitialised memory. */
    memset(code, 0, sizeof(code));
    put16(sizeof(code) - 2, 0xF000);
    assert(arm_decode(code, BASE, sizeof(code),
                      BASE + sizeof(code) - 2, ARM_T32, &in) == 0);

    /* An address before the start of the buffer. */
    assert(arm_decode(code, BASE, sizeof(code), BASE - 4, ARM_T32, &in) == 0);

    printf("  bounds                         ok\n");
}

/* --- forms the first run of a real module found missing ---------------------
 *
 * Encodings taken from GCC's own output for tests/vita/bridgetest, or worked
 * out from the architecture manual. Three of these were translating WRONGLY
 * rather than trapping, which is why each has an exact operand check. */

/* Found running bridgetest inside Vita3K. */
static void test_tables_hints_exclusives(void) {
    memset(code, 0, sizeof(code));
    arm_insn in;

    /* nop.w and dmb sit in the conditional-branch encoding space with cond
     * 111x. The test that separated them read the wrong four bits, and both
     * decoded as branches. */
    put16(0, 0xF3AF); put16(2, 0x8000);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NOP && in.cls != A_BRANCH && !in.has_target);
    put16(0, 0xF3BF); put16(2, 0x8F5F);                  /* dmb sy */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_DMB && !in.has_target);
    put16(0, 0xF3BF); put16(2, 0x8F2F);                  /* clrex */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_CLREX);
    put16(0, 0xF040); put16(2, 0x8000);                  /* bne.w: still a branch */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_B && in.cond == 1 && in.has_target);

    /* ldrex r2, [r0, #4]; strex r3, r1, [r0]; ldrexb; strexh; ldrexd; strexd */
    put16(0, 0xE850); put16(2, 0x2F01);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDREX && in.xsize == 4 && in.rn == 0 && in.rt == 2 && in.imm == 4);
    put16(0, 0xE840); put16(2, 0x1300);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STREX && in.xsize == 4 && in.rn == 0 && in.rt == 1 && in.rd == 3);
    put16(0, 0xE8D0); put16(2, 0x2F4F);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDREX && in.xsize == 1 && in.rt == 2);
    put16(0, 0xE8C0); put16(2, 0x1F53);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STREX && in.xsize == 2 && in.rt == 1 && in.rd == 3);
    put16(0, 0xE8D0); put16(2, 0x237F);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDREX && in.xsize == 8 && in.rt == 2 && in.rt2 == 3);
    put16(0, 0xE8C0); put16(2, 0x2374);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STREX && in.xsize == 8 && in.rt == 2 && in.rt2 == 3 && in.rd == 4);

    /* A switch: cmp r2, #4; bhi; tbb [pc, r2]; five byte entries. The size
     * comes only from the guard; without it the table is not followed. */
    memset(code, 0, sizeof(code));
    put16(0, 0x2A04);                                    /* cmp r2, #4  */
    put16(2, 0xD808);                                    /* bhi.n       */
    put16(4, 0xE8DF); put16(6, 0xF002);                  /* tbb [pc,r2] */
    code[8] = 3; code[9] = 4; code[10] = 5; code[11] = 3; code[12] = 6;
    in = dec(ARM_T32, BASE + 4);
    assert(in.op == OP_TBB && in.table_n == 5);
    assert(arm_table_target(code, BASE, &in, 0) == BASE + 8 + 6);
    assert(arm_table_target(code, BASE, &in, 4) == BASE + 8 + 12);
    put16(0, 0x2B04);                                    /* cmp r3: wrong register */
    in = dec(ARM_T32, BASE + 4);
    assert(in.op == OP_TBB && in.table_n == 0 && in.cls == A_INDIRECT);
    put16(0, 0x2A04); put16(2, 0xD008);                  /* beq: not a bound */
    in = dec(ARM_T32, BASE + 4);
    assert(in.table_n == 0);
    /* An entry pointing back into the table is data, not a switch. */
    put16(2, 0xD808); code[10] = 0;
    in = dec(ARM_T32, BASE + 4);
    assert(in.table_n == 0);

    /* IT blocks. `ite eq; moveq r1,#1; movne r1,#0` from bridgetest's main:
     * both moves were emitted unconditionally, and both set flags. */
    {
        uint8_t it = 0;
        arm_insn seq[3];
        memset(code, 0, sizeof(code));
        put16(0, 0xBF0C); put16(2, 0x2101); put16(4, 0x2100); put16(6, 0x2101);
        for (int i = 0; i < 3; i++) {
            seq[i] = dec(ARM_T32, BASE + 2u * (unsigned)i);
            arm_it_step(&it, &seq[i]);
        }
        assert(seq[1].cond == 0x0 && seq[1].conditional && !seq[1].sets_flags);
        assert(seq[2].cond == 0x1 && seq[2].conditional && !seq[2].sets_flags);
        in = dec(ARM_T32, BASE + 6);
        arm_it_step(&it, &in);
        assert(in.cond == ARM_COND_AL && !in.conditional && in.sets_flags);

        /* itte ne: NE, NE, EQ; a CMP inside still sets flags. */
        it = 0;
        put16(0, 0xBF1A); put16(2, 0x2A01); put16(4, 0x2101); put16(6, 0x2100);
        for (int i = 0; i < 4; i++) {
            in = dec(ARM_T32, BASE + 2u * (unsigned)i);
            arm_it_step(&it, &in);
            if (i == 1) assert(in.cond == 0x1 && in.op == OP_CMP && in.sets_flags);
            if (i == 2) assert(in.cond == 0x1);
            if (i == 3) assert(in.cond == 0x0);
        }
        assert(it == 0);
    }

    /* rsb.w r1, r1, #2048 keeps its immediate (only 16-bit NEG has zero), and
     * teq is its own op: XOR, not TST's AND. */
    put16(0, 0xF5C1); put16(2, 0x6100);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_RSB && in.has_imm && in.imm == 2048 && in.rn == 1 && in.rd == 1);
    put16(0, 0xF090); put16(2, 0x0F01);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_TEQ && in.imm == 1 && in.sets_flags);

    printf("  tables, hints and exclusives  ok\n");
}

static void test_bringup_forms(void) {
    memset(code, 0, sizeof(code));
    arm_insn in;

    /* pop.w {r4-r11, pc}: a return that still restores eight registers and
     * SP. It was classified as a return before its operands were filled in. */
    put16(0, 0xE8BD); put16(2, 0x8FF0);
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_RETURN && in.op == OP_POP && in.reglist == 0x8FF0);

    /* stmia.w r6, {r0,r1}: no writeback, a struct copy. */
    put16(0, 0xE886); put16(2, 0x0003);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STM && in.rn == 6 && !in.writeback && !in.decrement);

    /* stmia.w sp, {r0}: against SP but NOT a push — it must not move SP. */
    put16(0, 0xE88D); put16(2, 0x0001);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STM && in.rn == 13 && !in.writeback);

    /* ldmdb r0!, {r1,r2} */
    put16(0, 0xE930); put16(2, 0x0006);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDM && in.rn == 0 && in.writeback && in.decrement);

    /* 16-bit: ldmia r2!, {r0,r1} writes back; ldmia r0, {r0,r1} does not,
     * because the base is in the list. */
    put16(0, 0xCA03);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDM && in.rn == 2 && in.writeback && in.reglist == 0x03);
    put16(0, 0xC803);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDM && in.rn == 0 && !in.writeback);

    /* clz r1, r1 */
    put16(0, 0xFAB1); put16(2, 0xF181);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_CLZ && in.rd == 1 && in.rm == 1);

    /* umull r3, r2, ip, r4 — division by a constant, from bridgetest's main */
    put16(0, 0xFBAC); put16(2, 0x3204);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_UMULL && in.rn == 12 && in.rm == 4 && in.rt == 3 && in.rt2 == 2);
    put16(0, 0xFB81); put16(2, 0x2003);       /* smull r2, r0, r1, r3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_SMULL && in.rt == 2 && in.rt2 == 0);
    put16(0, 0xFBC1); put16(2, 0x2003);
    assert(dec(ARM_T32, BASE).op == OP_SMLAL);
    put16(0, 0xFBE1); put16(2, 0x2003);
    assert(dec(ARM_T32, BASE).op == OP_UMLAL);

    /* mla r0, r1, r2, r3 / mls r0, r1, r2, r3 — Ra in rt */
    put16(0, 0xFB01); put16(2, 0x3002);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MLA && in.rd == 0 && in.rn == 1 && in.rm == 2 && in.rt == 3);
    put16(0, 0xFB01); put16(2, 0x3012);
    assert(dec(ARM_T32, BASE).op == OP_MLS);

    /* vmov.f32 s12, #1.0 and vmov.f32 s11, #3.0 — both from bridgetest. The
     * second has D set, and 0x08 is 3.0 by VFPExpandImm. */
    put16(0, 0xEEB7); put16(2, 0x6A00);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VMOV_IMM && in.vd == 12 && in.imm == 0x3F800000u);
    put16(0, 0xEEF0); put16(2, 0x5A08);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VMOV_IMM && in.vd == 11 && in.imm == 0x40400000u);
    /* vmov.f64 d16, #1.0: high word only, D16 needs the D bit */
    put16(0, 0xEEF7); put16(2, 0x0B00);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VMOV_IMM && in.vfp_dp && in.vd == 16 && in.imm == 0x3FF00000u);

    /* The multiply family by opc1 and op. vmul/vnmul share opc1 0x2;
     * vnmls/vnmla share 0x1. Two of these four were decoded as each other. */
    put16(0, 0xEE20); put16(2, 0x0A81);       /* vmul.f32  s0, s1, s2 */
    assert(dec(ARM_T32, BASE).op == OP_VMUL);
    put16(0, 0xEE20); put16(2, 0x0AC1);       /* vnmul.f32 s0, s1, s2 */
    assert(dec(ARM_T32, BASE).op == OP_VNMUL);
    put16(0, 0xEE10); put16(2, 0x0A81);       /* vnmls.f32 s0, s1, s2 */
    assert(dec(ARM_T32, BASE).op == OP_VNMLS);
    put16(0, 0xEE10); put16(2, 0x0AC1);       /* vnmla.f32 s0, s1, s2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VNMLA && in.vd == 0 && in.vn == 1 && in.vm == 2);

    /* uadd8 r5, r2, ip and sel r4, r4, ip: newlib's strcmp */
    put16(0, 0xFA82); put16(2, 0xF54C);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_UADD8 && in.rd == 5 && in.rn == 2 && in.rm == 12);
    put16(0, 0xFAA4); put16(2, 0xF48C);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_SEL && in.rd == 4 && in.rn == 4 && in.rm == 12);

    /* NEON, from newlib's allocator. vmov.i32 d16, #0 and #5. */
    put16(0, 0xEFC0); put16(2, 0x0010);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_IMM && in.neon_fn == NL_MOV && in.vd == 16 && in.imm64 == 0 && !in.neon_q);
    put16(0, 0xEFC0); put16(2, 0x0015);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_IMM && in.imm64 == 0x0000000500000005ull);
    /* vadd.i32 d16, d16, d17 */
    put16(0, 0xEF60); put16(2, 0x08A1);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ADD && in.neon_size == 2 && in.vd == 16 && in.vn == 16 && in.vm == 17);
    /* vorr d8, d16, d16 (the vmov alias) */
    put16(0, 0xEF20); put16(2, 0x81B0);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_LOGIC && in.neon_fn == NL_ORR && in.vd == 8 && in.vn == 16 && in.vm == 16);
    /* vmov.i8 q8, #0xFF: cmode 1110, Q set, a=1 bcd=111 efgh=1111 */
    put16(0, 0xFFC7); put16(2, 0x0E5F);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_IMM && in.neon_q && in.vd == 16 && in.imm64 == ~0ull);

    printf("  bring-up forms (ldm/stm, clz, long mul, vmov imm, vnm*)  ok\n");
}

/* Shift amounts of zero that do not mean zero, the immediate carry-out, and
 * load/store indexing. Each of these decoded to a plausible instruction that
 * did something else. */
static void test_shifts_and_indexing(void) {
    memset(code, 0, sizeof(code));
    arm_insn in;

    put16(0, 0x0808);                                    /* lsrs r0, r1, #32 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LSR && in.has_imm && in.imm == 32);
    put16(0, 0x1008);                                    /* asrs r0, r1, #32 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_ASR && in.imm == 32);
    put16(0, 0x0008);                                    /* movs r0, r1      */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LSL && in.imm == 0);

    put16(0, 0xEA4F); put16(2, 0x0011);                  /* mov.w r0, r1, lsr #32 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOV && in.shift_type == SH_LSR && in.shift_amt == 32);
    put16(0, 0xEA4F); put16(2, 0x0031);                  /* mov.w r0, r1, rrx */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MOV && in.shift_type == SH_RRX && in.shift_amt == 1);
    put16(0, 0xEA4F); put16(2, 0x0001);                  /* mov.w r0, r1 */
    in = dec(ARM_T32, BASE);
    assert(in.shift_type == SH_LSL && in.shift_amt == 0);

    put16(0, 0xF011); put16(2, 0x407F);                  /* ands r0, r1, #0xFF000000 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_AND && in.sets_flags && in.imm == 0xFF000000u && in.imm_carry);
    put16(0, 0xF011); put16(2, 0x00FF);                  /* ands r0, r1, #0xFF */
    in = dec(ARM_T32, BASE);
    assert(in.imm == 0xFF && !in.imm_carry);

    put16(0, 0xF84D); put16(2, 0x0D04);                  /* str r0, [sp, #-4]! */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STR && in.rn == 13 && in.imm == 4 && !in.mem_add
           && in.writeback && !in.postindex);
    put16(0, 0xF851); put16(2, 0x0B04);                  /* ldr r0, [r1], #4 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDR && in.imm == 4 && in.mem_add && in.writeback && in.postindex);
    put16(0, 0xF851); put16(2, 0x0C08);                  /* ldr r0, [r1, #-8] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDR && in.imm == 8 && !in.mem_add && !in.writeback);
    put16(0, 0xF851); put16(2, 0x0022);                  /* ldr r0, [r1, r2, lsl #2] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDR && in.rm == 2 && in.shift_amt == 2 && !in.has_imm);
    put16(0, 0xF85F); put16(2, 0x0008);                  /* ldr r0, [pc, #-8] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDR && in.rn == 15 && in.imm == 8 && !in.mem_add);

    put16(0, 0xF941); put16(2, 0x078F);                  /* vst1.32 {d16}, [r1] */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_SIMD && in.op == OP_NONE);
    put16(0, 0xF961); put16(2, 0x078F);                  /* vld1.32 {d16}, [r1] */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_SIMD && in.op == OP_NONE);
    put16(0, 0xF911); put16(2, 0x0004);                  /* ldrsb.w r0, [r1, r4] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDRSB);
    put16(0, 0xF891); put16(2, 0xF040);                  /* pld [r1, #64] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NOP && in.cls == A_NOP);
    put16(0, 0xF85D); put16(2, 0xFB04);                  /* ldr pc, [sp], #4 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_POP && in.cls == A_RETURN && in.reglist == 0x8000);

    put16(0, 0xE96D); put16(2, 0x4502);                  /* strd r4, r5, [sp, #-8]! */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_STRD && in.imm == 8 && !in.mem_add && in.writeback && !in.postindex);
    put16(0, 0xE8FD); put16(2, 0x4502);                  /* ldrd r4, r5, [sp], #8 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDRD && in.imm == 8 && in.mem_add && in.writeback && in.postindex);
    put16(0, 0xE9D0); put16(2, 0x0100);                  /* ldrd r0, r1, [r0] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LDRD && !in.writeback && !in.postindex);

    put16(0, 0xFA21); put16(2, 0xF002);                  /* lsr.w r0, r1, r2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_LSR && in.rd == 0 && in.rn == 1 && in.rm == 2 && !in.has_imm);
    printf("  shift-by-0 meanings, immediate carry, indexing, hints: ok\n");
}

int main(void) {
    printf("decode:\n");
    test_thumb_width();
    test_returns();
    test_pc_destination();
    test_operands();
    test_thumb_expand_imm();
    test_t32_operands();
    test_bitfield();
    test_wide_branch_group();
    test_bl_target();
    test_branch_targets();
    test_mode_switches();
    test_arm_cond_f();
    test_simd_identified();
    test_vfp();
    test_it_block();
    test_bounds();
    test_bringup_forms();
    test_shifts_and_indexing();
    test_tables_hints_exclusives();
    printf("all decode tests passed\n");
    return 0;
}

