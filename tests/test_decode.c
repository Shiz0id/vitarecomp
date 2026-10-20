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
/* What newlib and the VitaSDK samples needed, each with the encoding the
 * toolchain actually emitted. */
static void test_homebrew_forms(void) {
    arm_insn in;

    /* orn: opcode 0011 is MVN only when Rn is r15. Decoding every ORN as MVN
     * dropped Rn, and newlib's strlen then lost strings at 4 mod 8. */
    put16(0, 0xEA62); put16(2, 0x0205);                  /* orn r2, r2, r5 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_ORN && in.rd == 2 && in.rn == 2 && in.rm == 5);
    put16(0, 0xF061); put16(2, 0x00FF);                  /* orn r0, r1, #255 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_ORN && in.rd == 0 && in.rn == 1 && in.has_imm && in.imm == 0xFF);
    put16(0, 0xF06F); put16(2, 0x0C00);                  /* mvn.w ip, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_MVN && in.rd == 12 && in.rn == ARM_NO_REG);

    put16(0, 0xF921); put16(2, 0x223D);                  /* vld1.8 {d2-d5}, [r1 :256]! */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VLD1 && in.vd == 2 && in.imm == 4 && in.writeback && in.rm == ARM_NO_REG);
    put16(0, 0xF943); put16(2, 0x0A8D);                  /* vst1.32 {d16-d17}, [r3]! */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VST1 && in.vd == 16 && in.imm == 2 && in.writeback);
    put16(0, 0xF943); put16(2, 0x0A82);                  /* vst1.32 {d16-d17}, [r3], r2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VST1 && in.writeback && in.rm == 2);

    put16(0, 0xEEE0); put16(2, 0x1B10);                  /* vdup.8 q0, r1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_DUP && in.vd == 0 && in.neon_q && in.neon_size == 0 && in.rt == 1);
    put16(0, 0xEEA0); put16(2, 0x3B90);                  /* vdup.32 q8, r3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_DUP && in.vd == 16 && in.neon_q && in.neon_size == 2 && in.rt == 3);

    /* vmov.32 r0, d2[0]: once decoded as VNMLS. */
    put16(0, 0xEE12); put16(2, 0x0B10);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FROM_SCALAR && in.vn == 2 && in.rt == 0 && in.neon_size == 2 && in.imm == 0);

    put16(0, 0xEEF3); put16(2, 0x1B30);                  /* vmov.u8 r1, d3[5] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FROM_SCALAR && in.vn == 3 && in.neon_size == 0 && in.imm == 5 && in.vfp_unsigned);
    put16(0, 0xEE24); put16(2, 0x2B70);                  /* vmov.16 d4[3], r2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_TO_SCALAR && in.vd == 4 && in.neon_size == 1 && in.imm == 3 && in.rt == 2);

    put16(0, 0xEC4C); put16(2, 0x3B16);                  /* vmov d6, r3, ip */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VMOV_V_RR && in.vm == 6 && in.rt == 3 && in.rt2 == 12 && in.vfp_dp);
    put16(0, 0xEC53); put16(2, 0x0B18);                  /* vmov r0, r3, d8 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VMOV_RR_V && in.vm == 8 && in.rt == 0 && in.rt2 == 3);

    put16(0, 0xFF02); put16(2, 0x2850);                  /* vceq.i8 q1, q1, q0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CEQ && in.vd == 2 && in.vn == 2 && in.vm == 0 && in.neon_q);
    put16(0, 0xEF02); put16(2, 0x2B13);                  /* vpadd.i8 d2, d2, d3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PADD && in.vd == 2 && in.vn == 2 && in.vm == 3 && in.neon_size == 0);
    put16(0, 0xFFF6); put16(2, 0x6220);                  /* vmovn.i32 d22, q8 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MOVN && in.vd == 22 && in.vm == 16 && in.neon_size == 1);

    put16(0, 0xEEF7); put16(2, 0x7BC0);                  /* vcvt.f32.f64 s15, d0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCVT_F2F && in.vfp_dp && in.vd == 15 && in.vm == 0);
    put16(0, 0xEEB7); put16(2, 0x0AC7);                  /* vcvt.f64.f32 d0, s14 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VCVT_F2F && !in.vfp_dp && in.vd == 0 && in.vm == 14);

    put16(0, 0xFA90); put16(2, 0xF0A0);                  /* rbit r0, r0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_RBIT && in.rd == 0 && in.rm == 0);
    put16(0, 0xFA90); put16(2, 0xF890);                  /* rev16.w r8, r0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_REV16 && in.rd == 8);
    put16(0, 0xBA52);                                    /* rev16 r2, r2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_REV16);
    put16(0, 0xBAD1);                                    /* revsh r1, r2: once REV */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_REVSH && in.rd == 1 && in.rm == 2);

    /* The wide extends: once indexed by two bits of a three-bit field. */
    put16(0, 0xFA5F); put16(2, 0xF880);                  /* uxtb.w r8, r0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_UXTB && in.rd == 8 && in.rn == ARM_NO_REG && in.shift_amt == 0);
    put16(0, 0xFA4F); put16(2, 0xF390);                  /* sxtb.w r3, r0, ror #8 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_SXTB && in.shift_amt == 8);
    put16(0, 0xFA13); put16(2, 0xF381);                  /* uxtah r3, r3, r1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_UXTH && in.rn == 3 && in.rm == 1 && in.rd == 3);
    put16(0, 0xFA55); put16(2, 0xF680);                  /* uxtab r6, r5, r0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_UXTB && in.rn == 5 && in.rm == 0 && in.rd == 6);

    put16(0, 0xFB15); put16(2, 0xF204);                  /* smulbb r2, r5, r4 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_SMULXY && in.rd == 2 && in.rn == 5 && in.rm == 4 && in.half_sel == 0);
    put16(0, 0xFB17); put16(2, 0x1202);                  /* smlabb r2, r7, r2, r1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_SMLAXY && in.rt == 1);

    put16(0, 0xEC90); put16(2, 0x0B20);                  /* vldmia r0, {d0-d15}: no writeback */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VLDM && in.vd == 0 && in.imm == 16 && !in.writeback && in.mem_add);
    put16(0, 0xECC0); put16(2, 0x0B20);                  /* vstmia r0, {d16-d31} */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VSTM && in.vd == 16 && in.imm == 16 && !in.writeback);
    put16(0, 0xEC90); put16(2, 0x0B21);                  /* fldmiax r0, {d0-d15} */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_VLDM && in.imm == 16);

    /* Advanced SIMD float, three registers. Encodings from arm-vita-eabi-as.
     * The integer vadd.i32 at the end must still be OP_NEON_ADD. */
    put16(0, 0xEF01); put16(2, 0x0D02);                  /* vadd.f32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FADD && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 2);
    put16(0, 0xEF02); put16(2, 0x0D44);                  /* vadd.f32 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FADD && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q);
    put16(0, 0xEF61); put16(2, 0x0DA2);                  /* vsub.f32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FSUB && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q);
    put16(0, 0xEF62); put16(2, 0x0DE4);                  /* vsub.f32 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FSUB && in.vd == 16 && in.vn == 18 && in.vm == 20 && in.neon_q);
    put16(0, 0xFF01); put16(2, 0x0D12);                  /* vmul.f32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMUL && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q);
    put16(0, 0xFF4A); put16(2, 0x8DFC);                  /* vmul.f32 q12, q13, q14 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMUL && in.vd == 24 && in.vn == 26 && in.vm == 28 && in.neon_q);
    put16(0, 0xEF01); put16(2, 0x0D12);                  /* vmla.f32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMLA && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q);
    put16(0, 0xEF02); put16(2, 0x0D54);                  /* vmla.f32 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMLA && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q);
    put16(0, 0xEF41); put16(2, 0x0DB2);                  /* vmla.f32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMLA && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q);
    put16(0, 0xEF21); put16(2, 0x0D12);                  /* vmls.f32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMLS && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q);
    put16(0, 0xEF22); put16(2, 0x0D54);                  /* vmls.f32 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMLS && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q);
    put16(0, 0xEF62); put16(2, 0x0DF4);                  /* vmls.f32 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMLS && in.vd == 16 && in.vn == 18 && in.vm == 20 && in.neon_q);

    /* Float by scalar: M is the lane (imm), Vm[3:0] the D register. The Q bit
     * is h1 bit 12. d4[1] and d7[1] show the lane bit is not a register bit. */
    put16(0, 0xFFA2); put16(2, 0x0964);                  /* vmul.f32 q0, q1, d4[1] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMUL_SCALAR && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.imm == 1 && in.neon_q);
    put16(0, 0xEFA1); put16(2, 0x0142);                  /* vmla.f32 d0, d1, d2[0] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMLA_SCALAR && in.vd == 0 && in.vn == 1 && in.vm == 2 && in.imm == 0 && !in.neon_q);
    put16(0, 0xEFE1); put16(2, 0x09E7);                  /* vmul.f32 d16, d17, d7[1] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMUL_SCALAR && in.vd == 16 && in.vn == 17 && in.vm == 7 && in.imm == 1 && !in.neon_q);
    put16(0, 0xFFE2); put16(2, 0x01EF);                  /* vmla.f32 q8, q9, d15[1] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMLA_SCALAR && in.vd == 16 && in.vn == 18 && in.vm == 15 && in.imm == 1 && in.neon_q);

    /* Negative: the integer vadd.i32 shares the three-register group and must
     * still be integer, with the float opc (1101) nowhere near it. */
    put16(0, 0xEF21); put16(2, 0x0802);                  /* vadd.i32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ADD && in.neon_size == 2 && in.vd == 0 && in.vn == 1 && in.vm == 2);
    /* The integer by-scalar forms have the same size bits and differ only in
     * opc: VMLA.I32 is 0000 where VMLA.F32 is 0001, VMUL.I32 1000 against
     * 1001. They must not be claimed as float. */
    put16(0, 0xEFA1); put16(2, 0x0042);                  /* vmla.i32 d0, d1, d2[0] */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_FMLA_SCALAR && in.cls == A_SIMD);
    put16(0, 0xEFA1); put16(2, 0x0842);                  /* vmul.i32 d0, d1, d2[0] */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_FMUL_SCALAR && in.cls == A_SIMD);

    /* VMLS by scalar is opc 0101 with the same lane fields as VMLA. The lane
     * index and Vm[3:0] are read the same way, so d2[1] and d7[0] pin them. */
    put16(0, 0xEFA1); put16(2, 0x0562);                  /* vmls.f32 d0, d1, d2[1] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMLS_SCALAR && in.vd == 0 && in.vn == 1 && in.vm == 2 && in.imm == 1 && !in.neon_q);
    put16(0, 0xFFA2); put16(2, 0x0564);                  /* vmls.f32 q0, q1, d4[1] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMLS_SCALAR && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.imm == 1 && in.neon_q);
    put16(0, 0xEFE1); put16(2, 0x05C7);                  /* vmls.f32 d16, d17, d7[0] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMLS_SCALAR && in.vd == 16 && in.vn == 17 && in.vm == 7 && in.imm == 0 && !in.neon_q);
    put16(0, 0xFFE2); put16(2, 0x05EF);                  /* vmls.f32 q8, q9, d15[1] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMLS_SCALAR && in.vd == 16 && in.vn == 18 && in.vm == 15 && in.imm == 1 && in.neon_q);
    /* The integer VMLS.I32 by scalar is opc 0100, one bit away from the float. */
    put16(0, 0xEFA1); put16(2, 0x0442);                  /* vmls.i32 d0, d1, d2[0] */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_FMLS_SCALAR && in.cls == A_SIMD);

    /* Float max and min, three registers: U=0, opc 1111, bit 21 picks min. */
    put16(0, 0xEF01); put16(2, 0x0F02);                  /* vmax.f32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMAX && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 2);
    put16(0, 0xEF42); put16(2, 0x0FE4);                  /* vmax.f32 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMAX && in.vd == 16 && in.vn == 18 && in.vm == 20 && in.neon_q);
    put16(0, 0xEF61); put16(2, 0x0FA2);                  /* vmin.f32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMIN && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q);
    put16(0, 0xEF22); put16(2, 0x0F44);                  /* vmin.f32 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMIN && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q);

    /* Float compares: VCEQ is U=0, VCGE U=1 bit 21 clear, VCGT U=1 bit 21 set. */
    put16(0, 0xEF01); put16(2, 0x0E02);                  /* vceq.f32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCEQ && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 2);
    put16(0, 0xEF42); put16(2, 0x0EE4);                  /* vceq.f32 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCEQ && in.vd == 16 && in.vn == 18 && in.vm == 20 && in.neon_q);
    put16(0, 0xFF41); put16(2, 0x0EA2);                  /* vcge.f32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCGE && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q);
    put16(0, 0xFF02); put16(2, 0x0E44);                  /* vcge.f32 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCGE && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q);
    put16(0, 0xFF21); put16(2, 0x0E02);                  /* vcgt.f32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCGT && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q);
    put16(0, 0xFF62); put16(2, 0x0EE4);                  /* vcgt.f32 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCGT && in.vd == 16 && in.vn == 18 && in.vm == 20 && in.neon_q);

    /* Two registers, miscellaneous: VABS and VNEG. Q is h2 bit 6, and the
     * d16/q8 forms set D (h1 bit 6) and M (h2 bit 5). */
    put16(0, 0xFFB9); put16(2, 0x0701);                  /* vabs.f32 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FABS && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 2);
    put16(0, 0xFFB9); put16(2, 0x0742);                  /* vabs.f32 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FABS && in.vd == 0 && in.vm == 2 && in.neon_q);
    put16(0, 0xFFF9); put16(2, 0x0721);                  /* vabs.f32 d16, d17 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FABS && in.vd == 16 && in.vm == 17 && !in.neon_q);
    put16(0, 0xFFF9); put16(2, 0x0762);                  /* vabs.f32 q8, q9 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FABS && in.vd == 16 && in.vm == 18 && in.neon_q);
    put16(0, 0xFFB9); put16(2, 0x0781);                  /* vneg.f32 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FNEG && in.vd == 0 && in.vm == 1 && !in.neon_q);
    put16(0, 0xFFF9); put16(2, 0x07E2);                  /* vneg.f32 q8, q9 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FNEG && in.vd == 16 && in.vm == 18 && in.neon_q);
    /* The integer VABS.S32 and VNEG.S32 share h1 but have opc 0110 and 0111. */
    put16(0, 0xFFB9); put16(2, 0x0301);                  /* vabs.s32 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_FABS && in.cls == A_SIMD);
    put16(0, 0xFFB9); put16(2, 0x0381);                  /* vneg.s32 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_FNEG && in.cls == A_SIMD);

    /* VCVT between F32 and S32/U32, no fixed-point bits. h1 bit 0 is set and
     * bit 1 separates the conversions from VABS/VNEG; opc 1110 is F32 to S32,
     * 1111 to U32, 1100 S32 to F32 and 1101 U32 to F32. */
    put16(0, 0xFFBB); put16(2, 0x0701);                  /* vcvt.s32.f32 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCVT_TO_INT && in.vd == 0 && in.vm == 1 && !in.vfp_unsigned && !in.neon_q);
    put16(0, 0xFFBB); put16(2, 0x0781);                  /* vcvt.u32.f32 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCVT_TO_INT && in.vd == 0 && in.vm == 1 && in.vfp_unsigned);
    put16(0, 0xFFBB); put16(2, 0x0601);                  /* vcvt.f32.s32 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCVT_FROM_INT && in.vd == 0 && in.vm == 1 && !in.vfp_unsigned && !in.neon_q);
    put16(0, 0xFFBB); put16(2, 0x0681);                  /* vcvt.f32.u32 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCVT_FROM_INT && in.vd == 0 && in.vm == 1 && in.vfp_unsigned);
    put16(0, 0xFFBB); put16(2, 0x0742);                  /* vcvt.s32.f32 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCVT_TO_INT && in.vd == 0 && in.vm == 2 && in.neon_q && !in.vfp_unsigned);
    put16(0, 0xFFBB); put16(2, 0x06C2);                  /* vcvt.f32.u32 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCVT_FROM_INT && in.vd == 0 && in.vm == 2 && in.neon_q && in.vfp_unsigned);
    put16(0, 0xFFFB); put16(2, 0x0721);                  /* vcvt.s32.f32 d16, d17 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCVT_TO_INT && in.vd == 16 && in.vm == 17 && !in.neon_q);
    put16(0, 0xFFFB); put16(2, 0x07E2);                  /* vcvt.u32.f32 q8, q9 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCVT_TO_INT && in.vd == 16 && in.vm == 18 && in.neon_q && in.vfp_unsigned);
    put16(0, 0xFFFB); put16(2, 0x0621);                  /* vcvt.f32.s32 d16, d17 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCVT_FROM_INT && in.vd == 16 && in.vm == 17 && !in.vfp_unsigned);

    /* Negatives. The fixed-point VCVT (#16) is U=0 with a 1 in h2 bit 4, so it
     * is never the plain conversion. The integer max, min and compares share the
     * three-register group with other opc values and stay integer. */
    put16(0, 0xEFB0); put16(2, 0x0F11);                  /* vcvt.s32.f32 d0, d1, #16 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_FCVT_TO_INT && in.op != OP_NEON_FCVT_FROM_INT);
    put16(0, 0xEF21); put16(2, 0x0602);                  /* vmax.s32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_FMAX && in.op != OP_NEON_FMIN);
    put16(0, 0xEF21); put16(2, 0x0612);                  /* vmin.s32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_FMAX && in.op != OP_NEON_FMIN);
    put16(0, 0xEF21); put16(2, 0x0312);                  /* vcge.s32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_FCGE && in.op != OP_NEON_FCEQ && in.op != OP_NEON_FCGT);
    put16(0, 0xFF21); put16(2, 0x0302);                  /* vcgt.u32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_FCGT && in.op != OP_NEON_FCGE);
    put16(0, 0xFF21); put16(2, 0x0812);                  /* vceq.i32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CEQ && in.neon_size == 2 && in.op != OP_NEON_FCEQ);
    put16(0, 0xEF21); put16(2, 0x0802);                  /* vadd.i32 d0, d1, d2 (guard) */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ADD);

    /* Integer VMUL, VMLA, VMLS, every size in D and Q. The opc is 1001 for all
     * three: B set and U clear is VMUL, B clear is VMLA (U=0) or VMLS (U=1). */
    put16(0, 0xEF01); put16(2, 0x0912);                  /* vmul.i8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MUL && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0);
    put16(0, 0xEF52); put16(2, 0x09F4);                  /* vmul.i16 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MUL && in.vd == 16 && in.vn == 18 && in.vm == 20 && in.neon_q && in.neon_size == 1);
    put16(0, 0xEF61); put16(2, 0x09B2);                  /* vmul.i32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MUL && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q && in.neon_size == 2);
    put16(0, 0xEF12); put16(2, 0x0954);                  /* vmul.i16 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MUL && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q && in.neon_size == 1);
    /* VMUL.P8 is the same opc with U set. It is a polynomial multiply, not a
     * wrapping one, so it must not be claimed as OP_NEON_MUL. */
    put16(0, 0xFF01); put16(2, 0x0912);                  /* vmul.p8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_MUL && in.cls == A_SIMD);
    put16(0, 0xFF02); put16(2, 0x0954);                  /* vmul.p8 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_MUL && in.cls == A_SIMD);
    /* The float VMUL and VMLA share the three-register group with opc 1101. */
    put16(0, 0xFF01); put16(2, 0x0D12);                  /* vmul.f32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMUL && in.op != OP_NEON_MUL);
    put16(0, 0xEF01); put16(2, 0x0D12);                  /* vmla.f32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMLA && in.op != OP_NEON_MLA);

    put16(0, 0xEF01); put16(2, 0x0902);                  /* vmla.i8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MLA && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0);
    put16(0, 0xEF12); put16(2, 0x0944);                  /* vmla.i16 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MLA && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q && in.neon_size == 1);
    put16(0, 0xEF61); put16(2, 0x09A2);                  /* vmla.i32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MLA && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q && in.neon_size == 2);
    put16(0, 0xFF01); put16(2, 0x0902);                  /* vmls.i8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MLS && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0);
    put16(0, 0xFF12); put16(2, 0x0944);                  /* vmls.i16 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MLS && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q && in.neon_size == 1);
    put16(0, 0xFF62); put16(2, 0x09E4);                  /* vmls.i32 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MLS && in.vd == 16 && in.vn == 18 && in.vm == 20 && in.neon_q && in.neon_size == 2);
    /* Neighbours: VADD and VSUB are opc 1000, VMLA.F32 is not an integer VMLA. */
    put16(0, 0xEF01); put16(2, 0x0802);                  /* vadd.i8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ADD && in.op != OP_NEON_MLA);
    put16(0, 0xFF61); put16(2, 0x08A2);                  /* vsub.i32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SUB && in.op != OP_NEON_MLS);

    /* Saturating add and subtract, every size in D and Q. U picks the sign:
     * VQADD is opc 0000 and VQSUB opc 0010, both with B set. */
    put16(0, 0xEF01); put16(2, 0x0012);                  /* vqadd.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QADD && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    put16(0, 0xFF01); put16(2, 0x0012);                  /* vqadd.u8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QADD && in.neon_size == 0 && in.vfp_unsigned);
    put16(0, 0xEF12); put16(2, 0x0054);                  /* vqadd.s16 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QADD && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q && in.neon_size == 1 && !in.vfp_unsigned);
    put16(0, 0xFF12); put16(2, 0x0054);                  /* vqadd.u16 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QADD && in.neon_q && in.neon_size == 1 && in.vfp_unsigned);
    put16(0, 0xEF61); put16(2, 0x00B2);                  /* vqadd.s32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QADD && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q && in.neon_size == 2 && !in.vfp_unsigned);
    put16(0, 0xFF62); put16(2, 0x00F4);                  /* vqadd.u32 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QADD && in.vd == 16 && in.vn == 18 && in.vm == 20 && in.neon_q && in.neon_size == 2 && in.vfp_unsigned);
    put16(0, 0xEF32); put16(2, 0x0054);                  /* vqadd.s64 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QADD && in.neon_q && in.neon_size == 3 && !in.vfp_unsigned);
    put16(0, 0xFF71); put16(2, 0x00B2);                  /* vqadd.u64 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QADD && in.vd == 16 && in.neon_size == 3 && in.vfp_unsigned && !in.neon_q);
    put16(0, 0xEF01); put16(2, 0x0212);                  /* vqsub.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QSUB && in.neon_size == 0 && !in.vfp_unsigned && !in.neon_q);
    put16(0, 0xFF11); put16(2, 0x0212);                  /* vqsub.u16 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QSUB && in.neon_size == 1 && in.vfp_unsigned && !in.neon_q);
    put16(0, 0xEF12); put16(2, 0x0254);                  /* vqsub.s16 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QSUB && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q && in.neon_size == 1);
    put16(0, 0xEF61); put16(2, 0x02B2);                  /* vqsub.s32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QSUB && in.vd == 16 && in.neon_size == 2 && !in.vfp_unsigned);
    put16(0, 0xFF62); put16(2, 0x02F4);                  /* vqsub.u32 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QSUB && in.vd == 16 && in.neon_q && in.neon_size == 2 && in.vfp_unsigned);
    put16(0, 0xEF32); put16(2, 0x0254);                  /* vqsub.s64 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QSUB && in.neon_q && in.neon_size == 3 && !in.vfp_unsigned);
    put16(0, 0xFF71); put16(2, 0x02B2);                  /* vqsub.u64 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QSUB && in.vd == 16 && in.neon_size == 3 && in.vfp_unsigned);
    /* The wrapping forms share the opc slots with the other pairs, so VADD and
     * VSUB (opc 1000) must not be claimed as saturating, nor the reverse. */
    put16(0, 0xEF01); put16(2, 0x0802);                  /* vadd.i8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ADD && in.op != OP_NEON_QADD);
    put16(0, 0xEF32); put16(2, 0x0844);                  /* vadd.i64 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ADD && in.op != OP_NEON_QADD);
    put16(0, 0xEF11); put16(2, 0x0B02);                  /* vqdmulh.s16 d0, d1, d2 (not claimed) */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_QADD && in.op != OP_NEON_MUL);

    /* Shifts by immediate. L:imm6 (h2 bit 7 and h1 bits 5:0) gives the size and
     * amount: VSHR is opc 0000 and VSRA 0001 (right, amount 2*esize - L:imm6),
     * VSHL 0101 with U clear (left, amount L:imm6 - esize). Both ends are tested:
     * the full lane width and 1 for right shifts, 0 and esize-1 for VSHL. */
    put16(0, 0xEF8F); put16(2, 0x0011);                  /* vshr.s8 d0, d1, #1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHR && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 0 && in.imm == 1 && !in.vfp_unsigned);
    put16(0, 0xEF88); put16(2, 0x0011);                  /* vshr.s8 d0, d1, #8: full width */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHR && in.neon_size == 0 && in.imm == 8 && !in.vfp_unsigned);
    put16(0, 0xFFC8); put16(2, 0x0031);                  /* vshr.u8 d16, d17, #8 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHR && in.vd == 16 && in.vm == 17 && in.neon_size == 0 && in.imm == 8 && in.vfp_unsigned);
    put16(0, 0xEF90); put16(2, 0x0011);                  /* vshr.s16 d0, d1, #16: full width */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHR && in.neon_size == 1 && in.imm == 16 && !in.vfp_unsigned);
    put16(0, 0xFFDF); put16(2, 0x0072);                  /* vshr.u16 q8, q9, #1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHR && in.vd == 16 && in.vm == 18 && in.neon_q && in.neon_size == 1 && in.imm == 1 && in.vfp_unsigned);
    put16(0, 0xEFA0); put16(2, 0x0052);                  /* vshr.s32 q0, q1, #32: full width */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHR && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 2 && in.imm == 32 && !in.vfp_unsigned);
    put16(0, 0xFFE0); put16(2, 0x0031);                  /* vshr.u32 d16, d17, #32: full width */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHR && in.neon_size == 2 && in.imm == 32 && in.vfp_unsigned);
    put16(0, 0xEFBF); put16(2, 0x00D2);                  /* vshr.s64 q0, q1, #1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHR && in.neon_q && in.neon_size == 3 && in.imm == 1 && !in.vfp_unsigned);
    put16(0, 0xFFC0); put16(2, 0x00F2);                  /* vshr.u64 q8, q9, #64: full width */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHR && in.vd == 16 && in.vm == 18 && in.neon_q && in.neon_size == 3 && in.imm == 64 && in.vfp_unsigned);
    put16(0, 0xEF80); put16(2, 0x0091);                  /* vshr.s64 d0, d1, #64: full width */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHR && !in.neon_q && in.neon_size == 3 && in.imm == 64);

    put16(0, 0xEF8F); put16(2, 0x0511);                  /* vshl.i8 d0, d1, #7: esize-1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL && in.vd == 0 && in.vm == 1 && in.neon_size == 0 && in.imm == 7);
    put16(0, 0xEF88); put16(2, 0x0511);                  /* vshl.i8 d0, d1, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL && in.neon_size == 0 && in.imm == 0);
    put16(0, 0xEFDF); put16(2, 0x0572);                  /* vshl.i16 q8, q9, #15: esize-1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL && in.vd == 16 && in.vm == 18 && in.neon_q && in.neon_size == 1 && in.imm == 15);
    put16(0, 0xEF90); put16(2, 0x0511);                  /* vshl.i16 d0, d1, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL && in.neon_size == 1 && in.imm == 0);
    put16(0, 0xEFFF); put16(2, 0x0531);                  /* vshl.i32 d16, d17, #31: esize-1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL && in.vd == 16 && in.vm == 17 && in.neon_size == 2 && in.imm == 31);
    put16(0, 0xEFA0); put16(2, 0x0552);                  /* vshl.i32 q0, q1, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL && in.neon_q && in.neon_size == 2 && in.imm == 0);
    put16(0, 0xEFBF); put16(2, 0x0591);                  /* vshl.i64 d0, d1, #63: esize-1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL && in.neon_size == 3 && in.imm == 63 && !in.neon_q);

    put16(0, 0xEF88); put16(2, 0x0111);                  /* vsra.s8 d0, d1, #8: full width */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SRA && in.vd == 0 && in.vm == 1 && in.neon_size == 0 && in.imm == 8 && !in.vfp_unsigned);
    put16(0, 0xFFD0); put16(2, 0x0172);                  /* vsra.u16 q8, q9, #16: full width */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SRA && in.vd == 16 && in.vm == 18 && in.neon_q && in.neon_size == 1 && in.imm == 16 && in.vfp_unsigned);
    put16(0, 0xEFE0); put16(2, 0x0131);                  /* vsra.s32 d16, d17, #32: full width */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SRA && in.vd == 16 && in.neon_size == 2 && in.imm == 32 && !in.vfp_unsigned);
    put16(0, 0xFF80); put16(2, 0x01D2);                  /* vsra.u64 q0, q1, #64: full width */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SRA && in.neon_q && in.neon_size == 3 && in.imm == 64 && in.vfp_unsigned);
    put16(0, 0xFF8F); put16(2, 0x0111);                  /* vsra.u8 d0, d1, #1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SRA && in.neon_size == 0 && in.imm == 1 && in.vfp_unsigned);
    put16(0, 0xEFFF); put16(2, 0x01B1);                  /* vsra.s64 d16, d17, #1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SRA && in.vd == 16 && in.vm == 17 && in.neon_size == 3 && in.imm == 1 && !in.vfp_unsigned);

    /* Negatives. The one-register immediate forms share h1 and h2 bit 7 with
     * these, and must still decode as VMOV/VMVN immediate. VSLI (opc 0101 with
     * U set), VSRI (0100, U set) and VRSHR (0010) are other operations, with
     * ops of their own (test_neon_shift_imm_forms), and must not be taken for
     * these. */
    put16(0, 0xEFC0); put16(2, 0x0010);                  /* vmov.i32 d16, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_IMM && in.neon_fn == NL_MOV && in.vd == 16 && in.imm64 == 0 && !in.neon_q);
    put16(0, 0xEF80); put16(2, 0x0030);                  /* vmvn.i32 d0, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_IMM && in.neon_fn == NL_MVN);
    put16(0, 0xFF88); put16(2, 0x0511);                  /* vsli.8 d0, d1, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_SHL && in.op != OP_NEON_SHR && in.op != OP_NEON_SRA && in.cls == A_SIMD);
    put16(0, 0xFF8F); put16(2, 0x0411);                  /* vsri.8 d0, d1, #1 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_SHR && in.op != OP_NEON_SRA && in.cls == A_SIMD);
    put16(0, 0xEF8F); put16(2, 0x0211);                  /* vrshr.s8 d0, d1, #1 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_SHR && in.op != OP_NEON_SRA && in.cls == A_SIMD);
    /* The fixed-point VCVT (#16) has the same h2 bit 4 and bit 7 as a shift.
     * It is decoded in the same group, as its own op, never as a shift. */
    put16(0, 0xEFB0); put16(2, 0x0F11);                  /* vcvt.s32.f32 d0, d1, #16 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_SHR && in.op != OP_NEON_SHL && in.op != OP_NEON_SRA);

    printf("ok   homebrew forms: vld1/vst1, vdup, lane and pair moves, vceq, vpadd, vmovn, vcvt, rev/rbit, extends, smulxy, f32 vector, f32 max/min/cmp/abs/neg/cvt, integer vmul/vmla/vmls, vqadd/vqsub, shifts by immediate\n");
}

/* The shuffles, table lookup and lane duplicate. Each encoding was checked
 * with arm-vita-eabi-as and objdump, except the rejects: vtbl with n=31 and
 * len=2 cannot be assembled, so its halfwords come from the layout in
 * decode.c (N=1 for n=31, len=2 in h2 bits 9:8). */
/* The shifts by register: VSHL, VQSHL, VRSHL and VQRSHL, opc 0100 and 0101 with
 * B picking the saturating form. Each encoding was taken from arm-vita-eabi-as
 * and checked by objdump. The assembly is Dd, Dm, Dn: Dm is the value shifted
 * and Dn the amounts, so vn and vm are asserted apart (vshl.s8 d0, d1, d2 has
 * vn = 2 and vm = 1). Size 3 is valid for all four, so 64-bit lanes are covered.
 * The reject is computed from the layout: ef14 1442 has Vd = 1 with Q set, which
 * the register check refuses. VHADD (opc 0000, B clear) and VPMAX (opc 1010) are
 * not claimed by these four. */
static void test_neon_shl_reg_forms(void) {
    arm_insn in;

    put16(0, 0xEF02); put16(2, 0x0401);                  /* vshl.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL_REG && in.vd == 0 && in.vn == 2 && in.vm == 1 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vshl.s") == 0);
    put16(0, 0xFF02); put16(2, 0x0401);                  /* vshl.u8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL_REG && in.vd == 0 && in.vn == 2 && in.vm == 1 && !in.neon_q && in.neon_size == 0 && in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vshl.u") == 0);
    put16(0, 0xEF14); put16(2, 0x0442);                  /* vshl.s16 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL_REG && in.vd == 0 && in.vn == 4 && in.vm == 2 && in.neon_q && in.neon_size == 1 && !in.vfp_unsigned);
    put16(0, 0xFF62); put16(2, 0x04A1);                  /* vshl.u32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL_REG && in.vd == 16 && in.vn == 18 && in.vm == 17 && !in.neon_q && in.neon_size == 2 && in.vfp_unsigned);
    put16(0, 0xEF74); put16(2, 0x04E2);                  /* vshl.s64 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL_REG && in.vd == 16 && in.vn == 20 && in.vm == 18 && in.neon_q && in.neon_size == 3 && !in.vfp_unsigned);
    put16(0, 0xFF35); put16(2, 0x3404);                  /* vshl.u64 d3, d4, d5 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL_REG && in.vd == 3 && in.vn == 5 && in.vm == 4 && !in.neon_q && in.neon_size == 3 && in.vfp_unsigned);
    put16(0, 0xEF11); put16(2, 0x0400);                  /* vshl.s16 d0, d0, d1: Vd = Vm */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHL_REG && in.vd == 0 && in.vn == 1 && in.vm == 0 && !in.neon_q && in.neon_size == 1 && !in.vfp_unsigned);

    put16(0, 0xEF02); put16(2, 0x0411);                  /* vqshl.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QSHL_REG && in.vd == 0 && in.vn == 2 && in.vm == 1 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vqshl.s") == 0);
    put16(0, 0xFF14); put16(2, 0x0452);                  /* vqshl.u16 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QSHL_REG && in.vd == 0 && in.vn == 4 && in.vm == 2 && in.neon_q && in.neon_size == 1 && in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vqshl.u") == 0);
    put16(0, 0xEF62); put16(2, 0x04B1);                  /* vqshl.s32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QSHL_REG && in.vd == 16 && in.vn == 18 && in.vm == 17 && !in.neon_q && in.neon_size == 2 && !in.vfp_unsigned);
    put16(0, 0xFF74); put16(2, 0x04F2);                  /* vqshl.u64 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QSHL_REG && in.vd == 16 && in.vn == 20 && in.vm == 18 && in.neon_q && in.neon_size == 3 && in.vfp_unsigned);

    put16(0, 0xEF02); put16(2, 0x0501);                  /* vrshl.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_RSHL_REG && in.vd == 0 && in.vn == 2 && in.vm == 1 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vrshl.s") == 0);
    put16(0, 0xFF12); put16(2, 0x0501);                  /* vrshl.u16 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_RSHL_REG && in.vd == 0 && in.vn == 2 && in.vm == 1 && !in.neon_q && in.neon_size == 1 && in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vrshl.u") == 0);
    put16(0, 0xEF24); put16(2, 0x0542);                  /* vrshl.s32 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_RSHL_REG && in.vd == 0 && in.vn == 4 && in.vm == 2 && in.neon_q && in.neon_size == 2 && !in.vfp_unsigned);
    put16(0, 0xFF72); put16(2, 0x05A1);                  /* vrshl.u64 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_RSHL_REG && in.vd == 16 && in.vn == 18 && in.vm == 17 && !in.neon_q && in.neon_size == 3 && in.vfp_unsigned);

    put16(0, 0xEF02); put16(2, 0x0511);                  /* vqrshl.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QRSHL_REG && in.vd == 0 && in.vn == 2 && in.vm == 1 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vqrshl.s") == 0);
    put16(0, 0xFF04); put16(2, 0x0552);                  /* vqrshl.u8 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QRSHL_REG && in.vd == 0 && in.vn == 4 && in.vm == 2 && in.neon_q && in.neon_size == 0 && in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vqrshl.u") == 0);
    put16(0, 0xEF72); put16(2, 0x05B1);                  /* vqrshl.s64 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QRSHL_REG && in.vd == 16 && in.vn == 18 && in.vm == 17 && !in.neon_q && in.neon_size == 3 && !in.vfp_unsigned);
    put16(0, 0xFF64); put16(2, 0x05F2);                  /* vqrshl.u32 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QRSHL_REG && in.vd == 16 && in.vn == 20 && in.vm == 18 && in.neon_q && in.neon_size == 2 && in.vfp_unsigned);

    /* Neighbours. VHADD (opc 0000, B clear) and VPMAX (opc 1010, B clear) are
     * not any of these four. Only that is asserted here, so another family can
     * claim them later without this test changing. */
    put16(0, 0xEF01); put16(2, 0x0002);                  /* vhadd.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_SHL_REG && in.op != OP_NEON_QSHL_REG && in.op != OP_NEON_RSHL_REG && in.op != OP_NEON_QRSHL_REG);
    put16(0, 0xEF01); put16(2, 0x0A02);                  /* vpmax.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op != OP_NEON_SHL_REG && in.op != OP_NEON_QSHL_REG && in.op != OP_NEON_RSHL_REG && in.op != OP_NEON_QRSHL_REG);

    /* Reject: a Q register with an odd Vd (d1) is refused by the register check. */
    put16(0, 0xEF14); put16(2, 0x1442);                  /* vshl.s16 with Vd = 1 and Q set */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);

    printf("ok   neon shifts by register: vshl, vqshl, vrshl, vqrshl, every size, and the rejects\n");
}

/* Halving add and subtract (opc 0000, 0001, 0010 with B clear) and pairwise
 * max and min (opc 1010). Each encoding was taken from arm-vita-eabi-as and
 * checked by objdump. The rejects: size 3 for the halving ops and for VPMAX,
 * and Q set on VPMAX. The Q reject is the D form vpmax.s8 d0, d2, d4 with h2
 * bit 6 set, since the assembler has no Q form of VPMAX to take it from. */
static void test_neon_halving_forms(void) {
    arm_insn in;

    put16(0, 0xEF01); put16(2, 0x0002);                  /* vhadd.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_HADD && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vhadd.s") == 0);
    put16(0, 0xFF12); put16(2, 0x0044);                  /* vhadd.u16 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_HADD && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q && in.neon_size == 1 && in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vhadd.u") == 0);
    put16(0, 0xEF61); put16(2, 0x00A2);                  /* vhadd.s32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_HADD && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q && in.neon_size == 2 && !in.vfp_unsigned);

    put16(0, 0xFF01); put16(2, 0x0102);                  /* vrhadd.u8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_RHADD && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vrhadd.u") == 0);
    put16(0, 0xEF52); put16(2, 0x01E4);                  /* vrhadd.s16 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_RHADD && in.vd == 16 && in.vn == 18 && in.vm == 20 && in.neon_q && in.neon_size == 1 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vrhadd.s") == 0);
    put16(0, 0xFF24); put16(2, 0x3105);                  /* vrhadd.u32 d3, d4, d5 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_RHADD && in.vd == 3 && in.vn == 4 && in.vm == 5 && !in.neon_q && in.neon_size == 2 && in.vfp_unsigned);

    put16(0, 0xEF01); put16(2, 0x0202);                  /* vhsub.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_HSUB && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vhsub.s") == 0);
    put16(0, 0xFF11); put16(2, 0x0202);                  /* vhsub.u16 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_HSUB && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 1 && in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vhsub.u") == 0);
    put16(0, 0xEF22); put16(2, 0x0244);                  /* vhsub.s32 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_HSUB && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q && in.neon_size == 2 && !in.vfp_unsigned);

    put16(0, 0xEF01); put16(2, 0x0A02);                  /* vpmax.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PMAX && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vpmax.s") == 0);
    put16(0, 0xFF51); put16(2, 0x0AA2);                  /* vpmax.u16 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PMAX && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q && in.neon_size == 1 && in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vpmax.u") == 0);
    put16(0, 0xEF24); put16(2, 0x3A05);                  /* vpmax.s32 d3, d4, d5 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PMAX && in.vd == 3 && in.vn == 4 && in.vm == 5 && !in.neon_q && in.neon_size == 2 && !in.vfp_unsigned);
    put16(0, 0xEF00); put16(2, 0x0A01);                  /* vpmax.s8 d0, d0, d1: Vd = Vn */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PMAX && in.vd == 0 && in.vn == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);

    put16(0, 0xFF01); put16(2, 0x0A12);                  /* vpmin.u8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PMIN && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vpmin.u") == 0);
    put16(0, 0xEF11); put16(2, 0x0A12);                  /* vpmin.s16 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PMIN && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 1 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vpmin.s") == 0);
    put16(0, 0xFF61); put16(2, 0x0AB2);                  /* vpmin.u32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PMIN && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q && in.neon_size == 2 && in.vfp_unsigned);

    /* Neighbours: VQADD (opc 0000 with B set) and VPADD (opc 1011) keep their
     * claims, since the new B-clear cases do not reach them. */
    put16(0, 0xEF01); put16(2, 0x0012);                  /* vqadd.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QADD && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    put16(0, 0xEF01); put16(2, 0x0B12);                  /* vpadd.i8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PADD && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0);

    /* Rejects. Size 3 (h1 bits 5:4 = 11) is UNDEFINED for the halving ops and
     * for VPMAX. Q set on VPMAX is UNDEFINED: h2 bit 6 is the only difference
     * from vpmax.s8 d0, d2, d4 (0A04), and it is refused even though the
     * registers are even. */
    put16(0, 0xEF31); put16(2, 0x0002);                  /* vhadd with size 3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xEF31); put16(2, 0x0A02);                  /* vpmax with size 3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xEF02); put16(2, 0x0A44);                  /* vpmax.s8 with Q set */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);

    printf("ok   neon halving add and subtract, pairwise max and min: every size, the rejects\n");
}

/* Widening: the three-registers-of-different-lengths group and VSHLL/VMOVL.
 * Each encoding was taken from arm-vita-eabi-as and checked by objdump. The
 * rejects are computed from the layout, and each comment gives the bit that
 * refuses it: an odd Qd or Qn, opc 0100, ss = 11 (which is VEXT here), h2 bit 6
 * set on a VSHLL, and L (h2 bit 7) set on a VSHLL. */
static void test_neon_widen_forms(void) {
    arm_insn in;

    put16(0, 0xEF81); put16(2, 0x0002);                  /* vaddl.s8 q0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ADDL && in.vd == 0 && in.vn == 1 && in.vm == 2);
    assert(in.neon_size == 0 && !in.vfp_unsigned && strcmp(in.mnemonic, "vaddl.s8") == 0);
    put16(0, 0xFF92); put16(2, 0x2003);                  /* vaddl.u16 q1, d2, d3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ADDL && in.vd == 2 && in.vn == 2 && in.vm == 3);
    assert(in.neon_size == 1 && in.vfp_unsigned && strcmp(in.mnemonic, "vaddl.u16") == 0);
    put16(0, 0xEFA2); put16(2, 0x0104);                  /* vaddw.s32 q0, q1, d4 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ADDW && in.vd == 0 && in.vn == 2 && in.vm == 4);
    assert(in.neon_size == 2 && !in.vfp_unsigned && strcmp(in.mnemonic, "vaddw.s32") == 0);
    put16(0, 0xEF90); put16(2, 0x0302);                  /* vsubw.s16 q0, q0, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SUBW && in.vd == 0 && in.vn == 0 && in.vm == 2);
    assert(in.neon_size == 1 && strcmp(in.mnemonic, "vsubw.s16") == 0);
    put16(0, 0xFF80); put16(2, 0x4201);                  /* vsubl.u8 q2, d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SUBL && in.vd == 4 && in.vn == 0 && in.vm == 1);
    assert(in.neon_size == 0 && in.vfp_unsigned && strcmp(in.mnemonic, "vsubl.u8") == 0);
    put16(0, 0xEF91); put16(2, 0x0C02);                  /* vmull.s16 q0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MULL && in.vd == 0 && in.vn == 1 && in.vm == 2);
    assert(in.neon_size == 1 && !in.vfp_unsigned && strcmp(in.mnemonic, "vmull.s16") == 0);
    put16(0, 0xFFE0); put16(2, 0x0C2F);                  /* vmull.u32 q8, d0, d31: d = 16 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MULL && in.vd == 16 && in.vn == 0 && in.vm == 31);
    assert(in.neon_size == 2 && in.vfp_unsigned && strcmp(in.mnemonic, "vmull.u32") == 0);
    put16(0, 0xFF84); put16(2, 0x2C05);                  /* vmull.u8 q1, d4, d5 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MULL && in.vd == 2 && in.vn == 4 && in.vm == 5);
    assert(in.neon_size == 0 && in.vfp_unsigned && strcmp(in.mnemonic, "vmull.u8") == 0);
    put16(0, 0xEF81); put16(2, 0x0802);                  /* vmlal.s8 q0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MLAL && in.vd == 0 && in.vn == 1 && in.vm == 2);
    assert(in.neon_size == 0 && !in.vfp_unsigned && strcmp(in.mnemonic, "vmlal.s8") == 0);
    put16(0, 0xFFA6); put16(2, 0x6807);                  /* vmlal.u32 q3, d6, d7 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MLAL && in.vd == 6 && in.vn == 6 && in.vm == 7);
    assert(in.neon_size == 2 && in.vfp_unsigned && strcmp(in.mnemonic, "vmlal.u32") == 0);
    put16(0, 0xFF92); put16(2, 0x2A03);                  /* vmlsl.u16 q1, d2, d3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MLSL && in.vd == 2 && in.vn == 2 && in.vm == 3);
    assert(in.neon_size == 1 && in.vfp_unsigned && strcmp(in.mnemonic, "vmlsl.u16") == 0);

    /* VSHLL and VMOVL: the shift group, with opc 1010 and L clear. The shift
     * is L:imm6 - esize, and the size is the top set bit of L:imm6. */
    put16(0, 0xFF8B); put16(2, 0x0A11);                  /* vshll.u8 q0, d1, #3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHLL && in.vd == 0 && in.vm == 1 && in.neon_size == 0);
    assert(in.vfp_unsigned && in.imm == 3 && in.has_imm && strcmp(in.mnemonic, "vshll.u8") == 0);
    put16(0, 0xEF88); put16(2, 0x0A11);                  /* vmovl.s8 q0, d1: shift 0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHLL && in.vd == 0 && in.vm == 1 && in.neon_size == 0);
    assert(!in.vfp_unsigned && in.imm == 0 && strcmp(in.mnemonic, "vmovl.s8") == 0);
    put16(0, 0xFF90); put16(2, 0x2A14);                  /* vmovl.u16 q1, d4 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHLL && in.vd == 2 && in.vm == 4 && in.neon_size == 1);
    assert(in.vfp_unsigned && in.imm == 0 && strcmp(in.mnemonic, "vmovl.u16") == 0);
    put16(0, 0xEFA0); put16(2, 0x4A15);                  /* vmovl.s32 q2, d5 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHLL && in.vd == 4 && in.vm == 5 && in.neon_size == 2);
    assert(!in.vfp_unsigned && in.imm == 0 && strcmp(in.mnemonic, "vmovl.s32") == 0);
    put16(0, 0xEF9F); put16(2, 0x0A11);                  /* vshll.s16 q0, d1, #15 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHLL && in.vd == 0 && in.vm == 1 && in.neon_size == 1);
    assert(!in.vfp_unsigned && in.imm == 15 && strcmp(in.mnemonic, "vshll.s16") == 0);
    put16(0, 0xFFA1); put16(2, 0x2A12);                  /* vshll.u32 q1, d2, #1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SHLL && in.vd == 2 && in.vm == 2 && in.neon_size == 2);
    assert(in.vfp_unsigned && in.imm == 1 && strcmp(in.mnemonic, "vshll.u32") == 0);

    /* Rejects. */
    put16(0, 0xEF91); put16(2, 0x1C02);                  /* vmull: d = 1, Qd odd */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xEFA3); put16(2, 0x0104);                  /* vaddw: n = 3, Qn odd */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xEF81); put16(2, 0x0402);                  /* opc 0100, VADDHN: not claimed */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xEFB1); put16(2, 0x0C02);                  /* ss = 11, VEXT with imm 12 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xEFB1); put16(2, 0x0002);                  /* ss = 11, opc 0: VEXT, not VADDL */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_EXT && in.vd == 0 && in.vn == 1 && in.vm == 2 && in.imm == 0);
    put16(0, 0xEF9F); put16(2, 0x1A11);                  /* VSHLL: d = 1, Qd odd */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xEF9F); put16(2, 0x0A50);                  /* VSHLL with h2 bit 6 set: not VSHLL */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xEF9F); put16(2, 0x0A91);                  /* VSHLL with L (h2 bit 7) set: 64-bit source */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);

    printf("ok   neon widening: vaddl/vsubl/vaddw/vsubw/vmull/vmlal/vmlsl, vshll/vmovl, and their rejects\n");
}

static void test_neon_shuffle_forms(void) {
    arm_insn in;

    put16(0, 0xEFB1); put16(2, 0x0302);                  /* vext.8 d0, d1, d2, #3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_EXT && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q);
    assert(in.imm == 3 && in.has_imm && strcmp(in.mnemonic, "vext.8") == 0);
    put16(0, 0xEFB2); put16(2, 0x0B44);                  /* vext.8 q0, q1, q2, #11 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_EXT && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q);
    assert(in.imm == 11 && strcmp(in.mnemonic, "vext.8") == 0);
    put16(0, 0xEFB1); put16(2, 0x0402);                  /* vext.32 d0, d1, d2, #1: byte 4 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_EXT && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.imm == 4);
    put16(0, 0xEFB1); put16(2, 0x0802);                  /* D form, imm4 8: not encoded */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD && strcmp(in.mnemonic, "simd/vfp") == 0);
    put16(0, 0xEFB2); put16(2, 0x1B44);                  /* Q form, vd = d1: odd */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);

    put16(0, 0xFFB0); put16(2, 0x0001);                  /* vrev64.8 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_REV && in.vd == 0 && in.vm == 1 && !in.neon_q);
    assert(in.neon_size == 0 && in.imm == 64 && strcmp(in.mnemonic, "vrev64.8") == 0);
    put16(0, 0xFFB8); put16(2, 0x0042);                  /* vrev64.32 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_REV && in.vd == 0 && in.vm == 2 && in.neon_q);
    assert(in.neon_size == 2 && in.imm == 64 && strcmp(in.mnemonic, "vrev64.32") == 0);
    put16(0, 0xFFB4); put16(2, 0x0081);                  /* vrev32.16 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_REV && in.vd == 0 && in.vm == 1 && !in.neon_q);
    assert(in.neon_size == 1 && in.imm == 32 && strcmp(in.mnemonic, "vrev32.16") == 0);
    put16(0, 0xFFB0); put16(2, 0x0142);                  /* vrev16.8 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_REV && in.vd == 0 && in.vm == 2 && in.neon_q);
    assert(in.neon_size == 0 && in.imm == 16 && strcmp(in.mnemonic, "vrev16.8") == 0);
    put16(0, 0xFFB4); put16(2, 0x0101);                  /* vrev16.16: no such size */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);

    put16(0, 0xFFB2); put16(2, 0x0001);                  /* vswp d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SWP && in.vd == 0 && in.vm == 1 && !in.neon_q && strcmp(in.mnemonic, "vswp") == 0);
    put16(0, 0xFFB2); put16(2, 0x0042);                  /* vswp q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SWP && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 0);
    put16(0, 0xFFB6); put16(2, 0x0042);                  /* VSWP with size 16: undefined */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB6); put16(2, 0x0081);                  /* vtrn.16 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_TRN && in.vd == 0 && in.vm == 1 && !in.neon_q);
    assert(in.neon_size == 1 && strcmp(in.mnemonic, "vtrn.16") == 0);
    put16(0, 0xFFBA); put16(2, 0x00C2);                  /* vtrn.32 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_TRN && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 2);
    assert(strcmp(in.mnemonic, "vtrn.32") == 0);
    put16(0, 0xFFB2); put16(2, 0x0101);                  /* vuzp.8 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_UZP && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 0);
    assert(strcmp(in.mnemonic, "vuzp.8") == 0);
    put16(0, 0xFFB6); put16(2, 0x0142);                  /* vuzp.16 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_UZP && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 1);
    assert(strcmp(in.mnemonic, "vuzp.16") == 0);
    put16(0, 0xFFB2); put16(2, 0x0181);                  /* vzip.8 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ZIP && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 0);
    assert(strcmp(in.mnemonic, "vzip.8") == 0);
    put16(0, 0xFFBA); put16(2, 0x01C2);                  /* vzip.32 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ZIP && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 2);
    assert(strcmp(in.mnemonic, "vzip.32") == 0);
    put16(0, 0xFFBA); put16(2, 0x0181);                  /* vzip.32 d0, d1: undefined */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);

    put16(0, 0xFFBB); put16(2, 0x0C01);                  /* vdup.8 d0, d1[5] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_DUP_LANE && in.vd == 0 && in.vm == 1 && !in.neon_q);
    assert(in.neon_size == 0 && in.imm == 5 && strcmp(in.mnemonic, "vdup.8") == 0);
    put16(0, 0xFFBA); put16(2, 0x0C41);                  /* vdup.16 q0, d1[2] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_DUP_LANE && in.vd == 0 && in.vm == 1 && in.neon_q);
    assert(in.neon_size == 1 && in.imm == 2 && strcmp(in.mnemonic, "vdup.16") == 0);
    put16(0, 0xFFBC); put16(2, 0x0C2F);                  /* vdup.32 d0, d31[1] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_DUP_LANE && in.vd == 0 && in.vm == 31 && !in.neon_q);
    assert(in.neon_size == 2 && in.imm == 1 && strcmp(in.mnemonic, "vdup.32") == 0);
    put16(0, 0xFFB8); put16(2, 0x0C01);                  /* imm4 = x000: no lane size */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);

    put16(0, 0xFFB1); put16(2, 0x0802);                  /* vtbl.8 d0, {d1}, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_TBL && in.vd == 0 && in.vn == 1 && in.vm == 2 && in.imm == 1);
    assert(strcmp(in.mnemonic, "vtbl.8") == 0);
    put16(0, 0xFFB1); put16(2, 0x0B05);                  /* vtbl.8 d0, {d1-d4}, d5 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_TBL && in.vn == 1 && in.vm == 5 && in.imm == 4);
    put16(0, 0xFFB1); put16(2, 0x0943);                  /* vtbx.8 d0, {d1-d2}, d3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_TBX && in.vd == 0 && in.vn == 1 && in.vm == 3 && in.imm == 2);
    assert(strcmp(in.mnemonic, "vtbx.8") == 0);
    put16(0, 0xFFBF); put16(2, 0x0A00);                  /* vtbl.8 d0, {d15-d17}, d0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_TBL && in.vn == 15 && in.vm == 0 && in.imm == 3);
    put16(0, 0xFFBF); put16(2, 0x0A80);                  /* n = 31, len 2: runs past d31 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);

    printf("ok   neon shuffles: vext, vrev, vswp, vtrn, vuzp, vzip, vdup scalar, vtbl/vtbx, and their rejects\n");
}

/* VLD2-4 / VST2-4, multiple structures. Each encoding was taken from
 * arm-vita-eabi-as and checked by objdump. neon_inc is the register step
 * between members, and neon_q marks the type 0011 form, whose members are Q
 * pairs. The rejects are computed from the encoding tables. */
static void test_neon_ldst_multi(void) {
    arm_insn in;

    put16(0, 0xF920); put16(2, 0x080F);                  /* vld2.8 {d0-d1}, [r0] */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_SIMD && in.op == OP_NEON_VLDN && strcmp(in.mnemonic, "vld2") == 0);
    assert(in.vd == 0 && in.rn == 0 && in.imm == 2 && in.neon_size == 0);
    assert(!in.neon_q && in.neon_inc == 1 && !in.writeback && in.rm == ARM_NO_REG);

    put16(0, 0xF921); put16(2, 0x094D);                  /* vld2.16 {d0,d2}, [r1]! */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VLDN && in.vd == 0 && in.rn == 1 && in.imm == 2);
    assert(in.neon_size == 1 && !in.neon_q && in.neon_inc == 2);
    assert(in.writeback && in.rm == ARM_NO_REG);         /* rm 13: by transfer size */

    put16(0, 0xF922); put16(2, 0x0383);                  /* vld2.32 {d0-d3}, [r2], r3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VLDN && in.vd == 0 && in.rn == 2 && in.imm == 2);
    assert(in.neon_size == 2 && in.neon_q && in.neon_inc == 2);
    assert(in.writeback && in.rm == 3);

    put16(0, 0xF960); put16(2, 0x032F);                  /* vld2.8 {d16-d19}, [r0 :128] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VLDN && in.vd == 16 && in.rn == 0 && in.imm == 2);
    assert(in.neon_size == 0 && in.neon_q && in.neon_inc == 2 && !in.writeback);

    put16(0, 0xF906); put16(2, 0x484F);                  /* vst2.16 {d4-d5}, [r6] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VSTN && in.vd == 4 && in.rn == 6 && in.imm == 2);
    assert(in.neon_size == 1 && !in.neon_q && in.neon_inc == 1 && !in.writeback);
    assert(strcmp(in.mnemonic, "vst2") == 0);

    put16(0, 0xF901); put16(2, 0x038D);                  /* vst2.32 {d0-d3}, [r1]! */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VSTN && in.vd == 0 && in.rn == 1 && in.imm == 2);
    assert(in.neon_size == 2 && in.neon_q && in.neon_inc == 2 && in.writeback);
    assert(in.rm == ARM_NO_REG);

    put16(0, 0xF920); put16(2, 0x040F);                  /* vld3.8 {d0-d2}, [r0] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VLDN && in.vd == 0 && in.rn == 0 && in.imm == 3);
    assert(in.neon_size == 0 && !in.neon_q && in.neon_inc == 1 && !in.writeback);
    assert(strcmp(in.mnemonic, "vld3") == 0);

    put16(0, 0xF921); put16(2, 0x054D);                  /* vld3.16 {d0,d2,d4}, [r1]! */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VLDN && in.vd == 0 && in.rn == 1 && in.imm == 3);
    assert(in.neon_size == 1 && !in.neon_q && in.neon_inc == 2 && in.writeback);

    put16(0, 0xF902); put16(2, 0x1484);                  /* vst3.32 {d1-d3}, [r2], r4 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VSTN && in.vd == 1 && in.rn == 2 && in.imm == 3);
    assert(in.neon_size == 2 && !in.neon_q && in.neon_inc == 1 && in.writeback && in.rm == 4);

    put16(0, 0xF920); put16(2, 0x000F);                  /* vld4.8 {d0-d3}, [r0] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VLDN && in.vd == 0 && in.rn == 0 && in.imm == 4);
    assert(in.neon_size == 0 && !in.neon_q && in.neon_inc == 1 && !in.writeback);
    assert(strcmp(in.mnemonic, "vld4") == 0);

    put16(0, 0xF921); put16(2, 0x018D);                  /* vld4.32 {d0,d2,d4,d6}, [r1]! */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VLDN && in.vd == 0 && in.rn == 1 && in.imm == 4);
    assert(in.neon_size == 2 && !in.neon_q && in.neon_inc == 2 && in.writeback);

    put16(0, 0xF945); put16(2, 0xC041);                  /* vst4.16 {d28-d31}, [r5], r1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VSTN && in.vd == 28 && in.rn == 5 && in.imm == 4);
    assert(in.neon_size == 1 && !in.neon_q && in.neon_inc == 1 && in.writeback && in.rm == 1);

    put16(0, 0xF900); put16(2, 0x013F);                  /* vst4.8 {d0,d2,d4,d6}, [r0 :256] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VSTN && in.vd == 0 && in.rn == 0 && in.imm == 4);
    assert(in.neon_size == 0 && !in.neon_q && in.neon_inc == 2 && !in.writeback);

    /* VLD1 shares the space and must still decode as VLD1 with neon_inc unused. */
    put16(0, 0xF920); put16(2, 0x0A8F);                  /* vld1.32 {d0-d1}, [r0] */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_VLD1 && in.vd == 0 && in.imm == 2 && !in.writeback);

    /* Rejects: size 3, VLD2 align 3 (D form), VLD3 align bit 1, a register past
     * d31, and rn = pc. Each decodes as opaque SIMD, not as a VLDN/VSTN. */
    put16(0, 0xF920); put16(2, 0x08CF);                  /* vld2, size 3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xF920); put16(2, 0x083F);                  /* vld2 type 1000, align 3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xF920); put16(2, 0x042F);                  /* vld3, align 2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xF960); put16(2, 0xA10F);                  /* vld4 type 0001 at d26: d32 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xF960); put16(2, 0xD30F);                  /* vld2 type 0011 at d29: d32 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xF92F); put16(2, 0x080F);                  /* vld2, rn = pc */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);

    printf("ok   neon structures: vld2-4/vst2-4 forms, Q pairs, writeback, and their rejects\n");
}

/* The narrowing forms. VQMOVN and VQMOVUN are in the two-register group, with
 * op = h2 bits 7:6. VSHRN to VQRSHRUN are in the shift group, with opc 1000
 * or 1001, and there h2 bit 6 is the rounding flag, not Q. Each encoding was
 * taken from arm-vita-eabi-as and checked by objdump. The rejects are computed
 * from the layout, and each says which field makes it invalid. */
static void test_neon_narrow_forms(void) {
    arm_insn in;

    /* VMOVN (op 00) is handled before the VQMOVN block, and must not change. */
    put16(0, 0xFFB2); put16(2, 0x0202);                  /* vmovn.i16 d0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MOVN && in.vd == 0 && in.vm == 2 && in.neon_size == 0);
    assert(strcmp(in.mnemonic, "vmovn") == 0);

    put16(0, 0xFFB2); put16(2, 0x0282);                  /* vqmovn.s16 d0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_QSHR && in.vd == 0 && in.vm == 2);
    assert(in.neon_size == 0 && in.imm == 0 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vqmovn.s16") == 0);
    put16(0, 0xFFB6); put16(2, 0x12C4);                  /* vqmovn.u32 d1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_QSHR && in.vd == 1 && in.vm == 4);
    assert(in.neon_size == 1 && in.vfp_unsigned && in.imm == 0);
    assert(strcmp(in.mnemonic, "vqmovn.u32") == 0);
    put16(0, 0xFFBA); put16(2, 0x2286);                  /* vqmovn.s64 d2, q3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_QSHR && in.vd == 2 && in.vm == 6);
    assert(in.neon_size == 2 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vqmovn.s64") == 0);

    put16(0, 0xFFB2); put16(2, 0x0242);                  /* vqmovun.s16 d0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_QSHRU && in.vd == 0 && in.vm == 2);
    assert(in.neon_size == 0 && in.imm == 0);
    assert(strcmp(in.mnemonic, "vqmovun.s16") == 0);
    put16(0, 0xFFBA); put16(2, 0x0242);                  /* vqmovun.s64 d0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_QSHRU && in.vd == 0 && in.vm == 2 && in.neon_size == 2);
    assert(strcmp(in.mnemonic, "vqmovun.s64") == 0);

    /* The shift group. The result lane is L:imm6's size, and the amount is
     * 2*esize - L:imm6, so each form is checked at both ends of its range. */
    put16(0, 0xEF8D); put16(2, 0x0812);                  /* vshrn.i16 d0, q1, #3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_SHR && in.vd == 0 && in.vm == 2);
    assert(in.neon_size == 0 && in.imm == 3 && in.has_imm && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vshrn.i16") == 0);
    put16(0, 0xEF90); put16(2, 0x0812);                  /* vshrn.i32 d0, q1, #16 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_SHR && in.neon_size == 1 && in.imm == 16);
    assert(strcmp(in.mnemonic, "vshrn.i32") == 0);
    put16(0, 0xEFBF); put16(2, 0x0812);                  /* vshrn.i64 d0, q1, #1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_SHR && in.neon_size == 2 && in.imm == 1);
    assert(strcmp(in.mnemonic, "vshrn.i64") == 0);
    put16(0, 0xEF88); put16(2, 0x0852);                  /* vrshrn.i16 d0, q1, #8 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_RSHR && in.neon_size == 0 && in.imm == 8);
    assert(strcmp(in.mnemonic, "vrshrn.i16") == 0);
    /* Odd Dd, and the full width: Q is not checked here, because h2 bit 6 is
     * the rounding flag. */
    put16(0, 0xEFA0); put16(2, 0x3854);                  /* vrshrn.i64 d3, q2, #32 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_RSHR && in.vd == 3 && in.vm == 4);
    assert(in.neon_size == 2 && in.imm == 32 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vrshrn.i64") == 0);

    put16(0, 0xEF8F); put16(2, 0x0912);                  /* vqshrn.s16 d0, q1, #1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_QSHR && in.neon_size == 0 && in.imm == 1 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vqshrn.s16") == 0);
    put16(0, 0xFF9B); put16(2, 0x0912);                  /* vqshrn.u32 d0, q1, #5 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_QSHR && in.neon_size == 1 && in.imm == 5 && in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vqshrn.u32") == 0);
    put16(0, 0xEF9C); put16(2, 0x0952);                  /* vqrshrn.s32 d0, q1, #4 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_QRSHR && in.neon_size == 1 && in.imm == 4 && !in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vqrshrn.s32") == 0);
    put16(0, 0xFFA1); put16(2, 0x0952);                  /* vqrshrn.u64 d0, q1, #31 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_QRSHR && in.neon_size == 2 && in.imm == 31 && in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vqrshrn.u64") == 0);

    put16(0, 0xFFA0); put16(2, 0x0812);                  /* vqshrun.s64 d0, q1, #32 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_QSHRU && in.neon_size == 2 && in.imm == 32);
    assert(strcmp(in.mnemonic, "vqshrun.s64") == 0);
    put16(0, 0xFF8E); put16(2, 0x0852);                  /* vqrshrun.s16 d0, q1, #2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NARROW_QRSHRU && in.neon_size == 0 && in.imm == 2);
    assert(strcmp(in.mnemonic, "vqrshrun.s16") == 0);

    /* Rejects. ef8d 0813: Qm = M:mmmm = 00011 = 3, odd. ef8d 0892: h2 bit 7 is
     * L and is set, so L:imm6 = 64 + 13 is past the 64-bit range. ffbe 0282:
     * h1 bits 3:2 (ss) are 11, which would need a 128-bit source. ffb2 0243:
     * VQMOVUN with Qm = 3, odd. */
    put16(0, 0xEF8D); put16(2, 0x0813);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xEF8D); put16(2, 0x0892);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFBE); put16(2, 0x0282);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB2); put16(2, 0x0243);
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);

    printf("ok   neon narrowing: vmovn, vqmovn/vqmovun, vshrn/vrshrn, vqshrn/vqrshrn, vqshrun/vqrshrun, and their rejects\n");
}

/* The shifts by immediate beyond VSHR, VSRA and VSHL: the rounding forms, the
 * inserts, the saturating left shifts and the fixed-point VCVT. Each encoding
 * was taken from arm-vita-eabi-as and objdump. The fields are checked against
 * the lane and the amount the encoding gives (R = 2*esize - L:imm6 for right
 * shifts, L:imm6 - esize for left shifts, fbits = 64 - L:imm6 for VCVT).
 * The rejects are the UNDEFINED cases the comment names. */
static void expect_shift(uint16_t h1, uint16_t h2, arm_op op, int vd, int vm,
                         int q, int size, int imm, int u) {
    put16(0, h1); put16(2, h2);
    arm_insn in = dec(ARM_T32, BASE);
    if (in.op != op || in.vd != vd || in.vm != vm || in.neon_q != q ||
        in.neon_size != size || in.imm != (uint32_t)imm || in.vfp_unsigned != u) {
        fprintf(stderr, "shift %04X %04X: op %d vd %d vm %d q %d size %d imm %u u %d\n",
                h1, h2, (int)in.op, in.vd, in.vm, in.neon_q, in.neon_size,
                (unsigned)in.imm, in.vfp_unsigned);
        assert(0);
    }
}

static void expect_none(uint16_t h1, uint16_t h2) {
    put16(0, h1); put16(2, h2);
    arm_insn in = dec(ARM_T32, BASE);
    if (in.op != OP_NONE || in.cls != A_SIMD) {
        fprintf(stderr, "reject %04X %04X decoded as op %d\n", h1, h2, (int)in.op);
        assert(0);
    }
}

static void test_neon_shift_imm_forms(void) {
    arm_insn in;

    /* VRSHR and VRSRA. Right shifts: L:imm6 = 8..15 is 8-bit lanes, and the
     * amount runs 1..8 there. L set is 64-bit lanes. */
    expect_shift(0xEF8F, 0x0211, OP_NEON_RSHR, 0, 1, 0, 0, 1, 0);     /* vrshr.s8 d0, d1, #1 */
    expect_shift(0xFF90, 0x0252, OP_NEON_RSHR, 0, 2, 1, 1, 16, 1);    /* vrshr.u16 q0, q1, #16: R = esize */
    expect_shift(0xEFF9, 0x0231, OP_NEON_RSHR, 16, 17, 0, 2, 7, 0);   /* vrshr.s32 d16, d17, #7 */
    expect_shift(0xFFC0, 0x02F2, OP_NEON_RSHR, 16, 18, 1, 3, 64, 1);  /* vrshr.u64 q8, q9, #64 */
    expect_shift(0xEF8F, 0x0291, OP_NEON_RSHR, 0, 1, 0, 3, 49, 0);    /* vrshr.s64 d0, d1, #49: a valid 64-bit form */
    expect_shift(0xEF8E, 0x1211, OP_NEON_RSHR, 1, 1, 0, 0, 2, 0);     /* vrshr.s8 d1, d1, #2 */
    expect_shift(0xEF88, 0x0311, OP_NEON_RSRA, 0, 1, 0, 0, 8, 0);     /* vrsra.s8 d0, d1, #8: R = esize */
    expect_shift(0xFFBD, 0x0352, OP_NEON_RSRA, 0, 2, 1, 2, 3, 1);     /* vrsra.u32 q0, q1, #3 */
    expect_shift(0xEFBF, 0x3394, OP_NEON_RSRA, 3, 4, 0, 3, 1, 0);     /* vrsra.s64 d3, d4, #1 */

    /* VSRI and VSLI. VSRI is U set (U clear is not a form), and VSLI is U set
     * opc 0101 where VSHL is U clear. R = esize and Lft = 0 are both legal. */
    expect_shift(0xFF8F, 0x0411, OP_NEON_SRI, 0, 1, 0, 0, 1, 1);      /* vsri.8 d0, d1, #1 */
    expect_shift(0xFF90, 0x0452, OP_NEON_SRI, 0, 2, 1, 1, 16, 1);     /* vsri.16 q0, q1, #16 */
    expect_shift(0xFFFB, 0x0431, OP_NEON_SRI, 16, 17, 0, 2, 5, 1);    /* vsri.32 d16, d17, #5 */
    expect_shift(0xFF80, 0x3494, OP_NEON_SRI, 3, 4, 0, 3, 64, 1);     /* vsri.64 d3, d4, #64 */
    expect_shift(0xFF88, 0x0511, OP_NEON_SLI, 0, 1, 0, 0, 0, 1);      /* vsli.8 d0, d1, #0 */
    expect_shift(0xFF9F, 0x0552, OP_NEON_SLI, 0, 2, 1, 1, 15, 1);     /* vsli.16 q0, q1, #15 */
    expect_shift(0xFFFF, 0x0531, OP_NEON_SLI, 16, 17, 0, 2, 31, 1);   /* vsli.32 d16, d17, #31 */
    expect_shift(0xFFC1, 0x05F2, OP_NEON_SLI, 16, 18, 1, 3, 1, 1);    /* vsli.64 q8, q9, #1 */

    /* VQSHL and VQSHLU. VQSHLU is U set only. */
    expect_shift(0xEF89, 0x0711, OP_NEON_QSHL_IMM, 0, 1, 0, 0, 1, 0); /* vqshl.s8 d0, d1, #1 */
    expect_shift(0xFF9F, 0x0752, OP_NEON_QSHL_IMM, 0, 2, 1, 1, 15, 1);/* vqshl.u16 q0, q1, #15 */
    expect_shift(0xEFE0, 0x0731, OP_NEON_QSHL_IMM, 16, 17, 0, 2, 0, 0);/* vqshl.s32 d16, d17, #0 */
    expect_shift(0xFFBF, 0x3794, OP_NEON_QSHL_IMM, 3, 4, 0, 3, 63, 1);/* vqshl.u64 d3, d4, #63 */
    expect_shift(0xFF8F, 0x0611, OP_NEON_QSHLU, 0, 1, 0, 0, 7, 1);    /* vqshlu.s8 d0, d1, #7 */
    expect_shift(0xFF91, 0x0652, OP_NEON_QSHLU, 0, 2, 1, 1, 1, 1);    /* vqshlu.s16 q0, q1, #1 */
    expect_shift(0xFFE0, 0x0631, OP_NEON_QSHLU, 16, 17, 0, 2, 0, 1);  /* vqshlu.s32 d16, d17, #0 */
    expect_shift(0xFFCA, 0x06F2, OP_NEON_QSHLU, 16, 18, 1, 3, 10, 1); /* vqshlu.s64 q8, q9, #10 */

    /* Fixed-point VCVT: imm is fbits, 64 - L:imm6. Only 32-bit lanes, so
     * L:imm6 is 32..63 with L clear, and fbits runs 1..32. */
    expect_shift(0xEFBF, 0x0F11, OP_NEON_CVT_F_TO_FIXED, 0, 1, 0, 2, 1, 0);    /* vcvt.s32.f32 d0, d1, #1 */
    expect_shift(0xFFA0, 0x0F52, OP_NEON_CVT_F_TO_FIXED, 0, 2, 1, 2, 32, 1);   /* vcvt.u32.f32 q0, q1, #32 */
    expect_shift(0xEFF0, 0x0E31, OP_NEON_CVT_FIXED_TO_F, 16, 17, 0, 2, 16, 0); /* vcvt.f32.s32 d16, d17, #16 */
    expect_shift(0xFFFF, 0x0E72, OP_NEON_CVT_FIXED_TO_F, 16, 18, 1, 2, 1, 1);  /* vcvt.f32.u32 q8, q9, #1 */

    /* Mnemonics, in the style of the other shifts. */
    put16(0, 0xEF8F); put16(2, 0x0211); in = dec(ARM_T32, BASE);
    assert(strcmp(in.mnemonic, "vrshr.s") == 0);
    put16(0, 0xFF8F); put16(2, 0x0411); in = dec(ARM_T32, BASE);
    assert(strcmp(in.mnemonic, "vsri.i") == 0);
    put16(0, 0xFF8F); put16(2, 0x0611); in = dec(ARM_T32, BASE);
    assert(strcmp(in.mnemonic, "vqshlu.s") == 0);
    put16(0, 0xEFBF); put16(2, 0x0F11); in = dec(ARM_T32, BASE);
    assert(strcmp(in.mnemonic, "vcvt.s32.f32") == 0);

    /* Rejects. 0100 with U clear and 0110 with U clear are not forms. With
     * L:imm6 = 011111 (31) the fixed-point VCVT is UNDEFINED, as it is with
     * L set (127 here). With Q set, an odd Vm (3) or Vd (1) is UNDEFINED. The
     * last two differ from the valid vrshr.s8 q0, q1 (0252) only in Vm or Vd. */
    expect_none(0xEF8F, 0x0411);     /* 0100 U clear */
    expect_none(0xEF8F, 0x0611);     /* 0110 U clear */
    expect_none(0xEF9F, 0x0F11);     /* VCVT, L:imm6 = 31 < 32 */
    expect_none(0xEFBF, 0x0F91);     /* VCVT, L set: 64-bit lanes */
    expect_none(0xEF8F, 0x0253);     /* vrshr.s8 q0, q1 with Vm = 3 (odd) */
    expect_none(0xEF8F, 0x1252);     /* vrshr.s8 q with Vd = 1 (odd) */

    /* The forms that share this group and were already claimed. */
    expect_shift(0xEF8F, 0x0011, OP_NEON_SHR, 0, 1, 0, 0, 1, 0);      /* vshr.s8 d0, d1, #1 */
    expect_shift(0xFF9D, 0x0111, OP_NEON_SRA, 0, 1, 0, 1, 3, 1);      /* vsra.u16 d0, d1, #3 */
    expect_shift(0xEFA3, 0x0511, OP_NEON_SHL, 0, 1, 0, 2, 3, 0);      /* vshl.s32 d0, d1, #3 */
    expect_shift(0xEF89, 0x0A11, OP_NEON_SHLL, 0, 1, 0, 0, 1, 0);     /* vshll.s8 q0, d1, #1 */
    put16(0, 0xFFBB); put16(2, 0x0701); in = dec(ARM_T32, BASE);     /* vcvt.s32.f32 d0, d1 */
    assert(in.op == OP_NEON_FCVT_TO_INT);

    printf("ok   neon shifts by immediate: vrshr/vrsra, vsri/vsli, vqshl/vqshlu, fixed VCVT, and their rejects\n");
}

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
    assert(in.cls == A_SIMD && in.op == OP_NEON_VST1 && in.vd == 16 && in.imm == 1 && !in.writeback);
    put16(0, 0xF961); put16(2, 0x078F);                  /* vld1.32 {d16}, [r1] */
    in = dec(ARM_T32, BASE);
    assert(in.cls == A_SIMD && in.op == OP_NEON_VLD1 && in.rn == 1);
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

/* The integer compares, max and min, and absolute difference, all in the
 * same-length group. VCGT and VCGE are opc 0011 with B clear and set, VMAX and
 * VMIN opc 0110, VABD and VABA opc 0111, and U picks the signedness. Each
 * encoding was taken from arm-vita-eabi-as and checked by objdump. The rejects
 * are computed from the layout: size 3 (h1 bits 5:4 = 11) is UNDEFINED for
 * these, and Q with an odd Vd is refused by the existing register check. */
static void test_neon_cmp_forms(void) {
    arm_insn in;

    put16(0, 0xEF01); put16(2, 0x0302);                  /* vcgt.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CGT && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    put16(0, 0xFF01); put16(2, 0x0302);                  /* vcgt.u8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CGT && in.neon_size == 0 && in.vfp_unsigned && !in.neon_q);
    assert(strcmp(in.mnemonic, "vcgt.u") == 0);
    put16(0, 0xEF12); put16(2, 0x0344);                  /* vcgt.s16 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CGT && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q && in.neon_size == 1 && !in.vfp_unsigned);
    put16(0, 0xFF62); put16(2, 0x03E4);                  /* vcgt.u32 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CGT && in.vd == 16 && in.vn == 18 && in.vm == 20 && in.neon_q && in.neon_size == 2 && in.vfp_unsigned);
    put16(0, 0xEF01); put16(2, 0x1302);                  /* vcgt.s8 d1, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CGT && in.vd == 1 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);

    put16(0, 0xEF01); put16(2, 0x0312);                  /* vcge.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CGE && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    put16(0, 0xFF14); put16(2, 0x3315);                  /* vcge.u16 d3, d4, d5 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CGE && in.vd == 3 && in.vn == 4 && in.vm == 5 && !in.neon_q && in.neon_size == 1 && in.vfp_unsigned);
    put16(0, 0xEF62); put16(2, 0x03F4);                  /* vcge.s32 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CGE && in.vd == 16 && in.vn == 18 && in.vm == 20 && in.neon_q && in.neon_size == 2 && !in.vfp_unsigned);
    put16(0, 0xFF02); put16(2, 0x0354);                  /* vcge.u8 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CGE && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q && in.neon_size == 0 && in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vcge.u") == 0);

    put16(0, 0xEF01); put16(2, 0x0602);                  /* vmax.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MAX && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    put16(0, 0xFF01); put16(2, 0x0602);                  /* vmax.u8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MAX && in.neon_size == 0 && in.vfp_unsigned && !in.neon_q);
    put16(0, 0xEF14); put16(2, 0x2646);                  /* vmax.s16 q1, q2, q3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MAX && in.vd == 2 && in.vn == 4 && in.vm == 6 && in.neon_q && in.neon_size == 1 && !in.vfp_unsigned);
    put16(0, 0xFF61); put16(2, 0x06A2);                  /* vmax.u32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MAX && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q && in.neon_size == 2 && in.vfp_unsigned);

    put16(0, 0xEF01); put16(2, 0x0612);                  /* vmin.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MIN && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    put16(0, 0xFF12); put16(2, 0x0654);                  /* vmin.u16 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MIN && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q && in.neon_size == 1 && in.vfp_unsigned);
    put16(0, 0xEF61); put16(2, 0x06B2);                  /* vmin.s32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MIN && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q && in.neon_size == 2 && !in.vfp_unsigned);
    put16(0, 0xFF62); put16(2, 0x06F4);                  /* vmin.u32 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MIN && in.vd == 16 && in.vn == 18 && in.vm == 20 && in.neon_q && in.neon_size == 2 && in.vfp_unsigned);
    assert(strcmp(in.mnemonic, "vmin.u") == 0);

    put16(0, 0xEF01); put16(2, 0x0702);                  /* vabd.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ABD && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    put16(0, 0xFF01); put16(2, 0x0702);                  /* vabd.u8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ABD && in.neon_size == 0 && in.vfp_unsigned && !in.neon_q);
    put16(0, 0xEF12); put16(2, 0x0744);                  /* vabd.s16 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ABD && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q && in.neon_size == 1 && !in.vfp_unsigned);
    put16(0, 0xFF61); put16(2, 0x07A2);                  /* vabd.u32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ABD && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q && in.neon_size == 2 && in.vfp_unsigned);
    put16(0, 0xEF6E); put16(2, 0xF7AD);                  /* vabd.s32 d31, d30, d29 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ABD && in.vd == 31 && in.vn == 30 && in.vm == 29 && !in.neon_q && in.neon_size == 2 && !in.vfp_unsigned);

    /* VABA is the accumulating form: the same opc 0111 with B set. */
    put16(0, 0xEF01); put16(2, 0x0712);                  /* vaba.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ABA && in.vd == 0 && in.vn == 1 && in.vm == 2 && !in.neon_q && in.neon_size == 0 && !in.vfp_unsigned);
    put16(0, 0xFF12); put16(2, 0x0754);                  /* vaba.u16 q0, q1, q2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ABA && in.vd == 0 && in.vn == 2 && in.vm == 4 && in.neon_q && in.neon_size == 1 && in.vfp_unsigned);
    put16(0, 0xEF61); put16(2, 0x07B2);                  /* vaba.s32 d16, d17, d18 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ABA && in.vd == 16 && in.vn == 17 && in.vm == 18 && !in.neon_q && in.neon_size == 2 && !in.vfp_unsigned);
    put16(0, 0xFF62); put16(2, 0x07F4);                  /* vaba.u32 q8, q9, q10 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ABA && in.vd == 16 && in.vn == 18 && in.vm == 20 && in.neon_q && in.neon_size == 2 && in.vfp_unsigned);

    /* Neighbours. The saturating VQSUB (opc 0010, B set) and the float compare
     * VCGT.F32 (opc 1110) must not become an integer compare, and the float
     * VMAX.F32 (opc 1111) must not become OP_NEON_MAX. */
    put16(0, 0xEF01); put16(2, 0x0212);                  /* vqsub.s8 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QSUB && in.op != OP_NEON_CGT);
    put16(0, 0xFF21); put16(2, 0x0E02);                  /* vcgt.f32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCGT && in.op != OP_NEON_CGT);
    put16(0, 0xFF01); put16(2, 0x0E02);                  /* vcge.f32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCGE && in.op != OP_NEON_CGE);
    put16(0, 0xEF01); put16(2, 0x0F02);                  /* vmax.f32 d0, d1, d2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FMAX && in.op != OP_NEON_MAX);

    /* Rejects. Each one decodes to OP_NONE, the trap, as the other refused
     * three-register forms do. */
    put16(0, 0xEF31); put16(2, 0x0602);                  /* vmax.s64 (size 3): UNDEFINED */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFF31); put16(2, 0x0712);                  /* vaba.u64 (size 3): UNDEFINED */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xEF02); put16(2, 0x1644);                  /* vmax.s8 q with Vd = d1: odd */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);

    printf("ok   neon compares, max/min, absolute difference, vaba, and their rejects\n");
}

/* The compares with zero, integer VABS and VNEG, all in the two-registers-
 * miscellaneous group with A = 01. Each encoding was taken from
 * arm-vita-eabi-as and checked by objdump. F (h2 bit 10) with op (h2 bits
 * 9:7) gives the form: 000 GT, 001 GE, 010 EQ, 011 LE, 100 LT, and 110/111
 * VABS/VNEG for integers. F=1 with 110/111 is the float VABS and VNEG, which
 * this block must leave to the block after it. The rejects are computed from
 * the layout: integer size 3 (h1 bits 3:2 = 11), F32 at a size other than 2,
 * op 101, and Q with an odd Vd or Vm. */
static void test_neon_cmpz_forms(void) {
    arm_insn in;

    put16(0, 0xFFB1); put16(2, 0x0001);                  /* vcgt.s8 d0, d1, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CMPZ && in.imm == 0 && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 0);
    assert(strcmp(in.mnemonic, "vcgt.s") == 0);
    put16(0, 0xFFB5); put16(2, 0x00C2);                  /* vcge.s16 q0, q1, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CMPZ && in.imm == 1 && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 1);
    assert(strcmp(in.mnemonic, "vcge.s") == 0);
    put16(0, 0xFFF9); put16(2, 0x0121);                  /* vceq.i32 d16, d17, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CMPZ && in.imm == 2 && in.vd == 16 && in.vm == 17 && !in.neon_q && in.neon_size == 2);
    assert(strcmp(in.mnemonic, "vceq.i") == 0);
    put16(0, 0xFFF1); put16(2, 0x01E2);                  /* vcle.s8 q8, q9, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CMPZ && in.imm == 3 && in.vd == 16 && in.vm == 18 && in.neon_q && in.neon_size == 0);
    assert(strcmp(in.mnemonic, "vcle.s") == 0);
    put16(0, 0xFFB9); put16(2, 0x0201);                  /* vclt.s32 d0, d1, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CMPZ && in.imm == 4 && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 2);
    assert(strcmp(in.mnemonic, "vclt.s") == 0);
    put16(0, 0xFFB5); put16(2, 0x1201);                  /* vclt.s16 d1, d1, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CMPZ && in.imm == 4 && in.vd == 1 && in.vm == 1 && in.neon_size == 1);

    /* F32: the same five kinds, size 2, with the float mnemonics. */
    put16(0, 0xFFB9); put16(2, 0x0401);                  /* vcgt.f32 d0, d1, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCMPZ && in.imm == 0 && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 2);
    assert(strcmp(in.mnemonic, "vcgt.f32") == 0);
    put16(0, 0xFFB9); put16(2, 0x04C2);                  /* vcge.f32 q0, q1, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCMPZ && in.imm == 1 && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 2);
    assert(strcmp(in.mnemonic, "vcge.f32") == 0);
    put16(0, 0xFFF9); put16(2, 0x0521);                  /* vceq.f32 d16, d17, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCMPZ && in.imm == 2 && in.vd == 16 && in.vm == 17 && !in.neon_q);
    assert(strcmp(in.mnemonic, "vceq.f32") == 0);
    put16(0, 0xFFF9); put16(2, 0x05E2);                  /* vcle.f32 q8, q9, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCMPZ && in.imm == 3 && in.vd == 16 && in.vm == 18 && in.neon_q);
    assert(strcmp(in.mnemonic, "vcle.f32") == 0);
    put16(0, 0xFFB9); put16(2, 0x0601);                  /* vclt.f32 d0, d1, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCMPZ && in.imm == 4 && in.vd == 0 && in.vm == 1 && !in.neon_q);
    assert(strcmp(in.mnemonic, "vclt.f32") == 0);

    /* Integer VABS and VNEG, sizes 0-2, D and Q. */
    put16(0, 0xFFB1); put16(2, 0x0301);                  /* vabs.s8 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ABS && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 0);
    assert(strcmp(in.mnemonic, "vabs.s") == 0);
    put16(0, 0xFFB5); put16(2, 0x0342);                  /* vabs.s16 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ABS && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 1);
    put16(0, 0xFFF9); put16(2, 0x0321);                  /* vabs.s32 d16, d17 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_ABS && in.vd == 16 && in.vm == 17 && !in.neon_q && in.neon_size == 2);
    put16(0, 0xFFF1); put16(2, 0x03E2);                  /* vneg.s8 q8, q9 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NEG && in.vd == 16 && in.vm == 18 && in.neon_q && in.neon_size == 0);
    assert(strcmp(in.mnemonic, "vneg.s") == 0);
    put16(0, 0xFFB5); put16(2, 0x0381);                  /* vneg.s16 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NEG && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 1);
    put16(0, 0xFFB9); put16(2, 0x03C2);                  /* vneg.s32 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_NEG && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 2);

    /* The float VABS and VNEG of the block after this one keep their ops: the
     * F=1 op 110/111 encodings must not come out as the integer forms, and
     * the F32 compares must not come out as VABS. */
    put16(0, 0xFFB9); put16(2, 0x0701);                  /* vabs.f32 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FABS && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 2);
    assert(strcmp(in.mnemonic, "vabs.f32") == 0);
    put16(0, 0xFFB9); put16(2, 0x07C2);                  /* vneg.f32 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FNEG && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 2);
    put16(0, 0xFFBB); put16(2, 0x0701);                  /* vcvt.s32.f32 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCVT_TO_INT && in.vd == 0 && in.vm == 1);

    /* Rejects. Each decodes to OP_NONE, the trap. */
    put16(0, 0xFFBD); put16(2, 0x0001);                  /* vcgt.s64 #0: size 3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB1); put16(2, 0x0401);                  /* vcgt.f32 with size 0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB1); put16(2, 0x0281);                  /* op 101: UNDEFINED */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB5); put16(2, 0x1342);                  /* vabs.s16 q1: odd Vd */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFBD); put16(2, 0x0701);                  /* vabs.f32 encoding with size 3: UNDEFINED */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);

    /* Neighbour. The VCVT encodings have A = 11 (h1 bit 1 set), so the mask
     * above never sees them, and vcvt.s32.f32 must keep its op. */
    put16(0, 0xFFBB); put16(2, 0x0701);                  /* vcvt.s32.f32 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_FCVT_TO_INT);

    printf("ok   neon compares with zero, integer vabs/vneg, float vabs/vneg kept, and their rejects\n");
}

/* Two registers, miscellaneous, A = 00 beyond VREV: the pairwise adds
 * (opc 0100 and 0101, and 1100 and 1101 with accumulate), VCLS (1000), VCLZ
 * (1001), VCNT (1010, size 0), VMVN (1011, size 0), VQABS (1110) and VQNEG
 * (1111). h2 bit 7 is the unsigned flag for the pairwise adds. Each encoding
 * was taken from arm-vita-eabi-as and checked by objdump. The rejects are
 * computed from the layout: size 3 for the forms that have one, VCNT and VMVN
 * at sizes 1 and 2, opc 0011, 0110 and 0111, and Q with an odd Vd or Vm. The
 * VREV and VSWP words and the A = 01 compare with zero keep their ops. */
static void test_neon_misc_forms(void) {
    arm_insn in;

    put16(0, 0xFFB0); put16(2, 0x0401);                  /* vcls.s8 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CLS && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 0);
    assert(strcmp(in.mnemonic, "vcls.s") == 0);
    put16(0, 0xFFB4); put16(2, 0x0442);                  /* vcls.s16 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CLS && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 1);
    put16(0, 0xFFF8); put16(2, 0x0421);                  /* vcls.s32 d16, d17 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CLS && in.vd == 16 && in.vm == 17 && !in.neon_q && in.neon_size == 2);
    put16(0, 0xFFF0); put16(2, 0x04E2);                  /* vclz.i8 q8, q9 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CLZ && in.vd == 16 && in.vm == 18 && in.neon_q && in.neon_size == 0);
    assert(strcmp(in.mnemonic, "vclz.i") == 0);
    put16(0, 0xFFB4); put16(2, 0x0481);                  /* vclz.i16 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CLZ && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 1);
    put16(0, 0xFFB8); put16(2, 0x04C2);                  /* vclz.i32 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CLZ && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 2);

    put16(0, 0xFFB0); put16(2, 0x0501);                  /* vcnt.8 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CNT && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 0);
    assert(strcmp(in.mnemonic, "vcnt.8") == 0);
    put16(0, 0xFFB0); put16(2, 0x0542);                  /* vcnt.8 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CNT && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 0);
    put16(0, 0xFFB0); put16(2, 0x0581);                  /* vmvn d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MVN && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 0);
    assert(strcmp(in.mnemonic, "vmvn") == 0);
    put16(0, 0xFFF0); put16(2, 0x05E2);                  /* vmvn q8, q9 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_MVN && in.vd == 16 && in.vm == 18 && in.neon_q && in.neon_size == 0);

    put16(0, 0xFFB0); put16(2, 0x0701);                  /* vqabs.s8 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QABS && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 0);
    assert(strcmp(in.mnemonic, "vqabs.s") == 0);
    put16(0, 0xFFB4); put16(2, 0x0742);                  /* vqabs.s16 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QABS && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 1);
    put16(0, 0xFFF8); put16(2, 0x0721);                  /* vqabs.s32 d16, d17 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QABS && in.vd == 16 && in.vm == 17 && !in.neon_q && in.neon_size == 2);
    put16(0, 0xFFB0); put16(2, 0x07C2);                  /* vqneg.s8 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QNEG && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 0);
    assert(strcmp(in.mnemonic, "vqneg.s") == 0);
    put16(0, 0xFFB4); put16(2, 0x0781);                  /* vqneg.s16 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QNEG && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 1);
    put16(0, 0xFFF8); put16(2, 0x07E2);                  /* vqneg.s32 q8, q9 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_QNEG && in.vd == 16 && in.vm == 18 && in.neon_q && in.neon_size == 2);

    put16(0, 0xFFB0); put16(2, 0x0201);                  /* vpaddl.s8 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PADDL && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 0);
    assert(in.vfp_unsigned == 0 && strcmp(in.mnemonic, "vpaddl.s") == 0);
    put16(0, 0xFFB0); put16(2, 0x02C2);                  /* vpaddl.u8 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PADDL && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 0);
    assert(in.vfp_unsigned == 1 && strcmp(in.mnemonic, "vpaddl.u") == 0);
    put16(0, 0xFFF4); put16(2, 0x0221);                  /* vpaddl.s16 d16, d17 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PADDL && in.vd == 16 && in.vm == 17 && !in.neon_q && in.neon_size == 1);
    assert(in.vfp_unsigned == 0);
    put16(0, 0xFFF8); put16(2, 0x02E2);                  /* vpaddl.u32 q8, q9 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PADDL && in.vd == 16 && in.vm == 18 && in.neon_q && in.neon_size == 2);
    assert(in.vfp_unsigned == 1);
    put16(0, 0xFFB0); put16(2, 0x0601);                  /* vpadal.s8 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PADAL && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 0);
    assert(in.vfp_unsigned == 0 && strcmp(in.mnemonic, "vpadal.s") == 0);
    put16(0, 0xFFB4); put16(2, 0x06C2);                  /* vpadal.u16 q0, q1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PADAL && in.vd == 0 && in.vm == 2 && in.neon_q && in.neon_size == 1);
    assert(in.vfp_unsigned == 1 && strcmp(in.mnemonic, "vpadal.u") == 0);
    put16(0, 0xFFF8); put16(2, 0x0621);                  /* vpadal.s32 d16, d17 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PADAL && in.vd == 16 && in.vm == 17 && !in.neon_q && in.neon_size == 2);
    put16(0, 0xFFB8); put16(2, 0x0680);                  /* vpadal.u32 d0, d0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_PADAL && in.vd == 0 && in.vm == 0 && !in.neon_q && in.neon_size == 2);
    assert(in.vfp_unsigned == 1);

    put16(0, 0xFFB0); put16(2, 0x1501);                  /* vcnt.8 d1, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CNT && in.vd == 1 && in.vm == 1 && !in.neon_q && in.neon_size == 0);

    /* Neighbours: VREV and VSWP keep their ops, and the A = 01 compare with
     * zero is untouched. */
    put16(0, 0xFFB0); put16(2, 0x0001);                  /* vrev64.8 d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_REV && in.vd == 0 && in.vm == 1);
    put16(0, 0xFFB2); put16(2, 0x0001);                  /* vswp d0, d1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_SWP && in.vd == 0 && in.vm == 1 && !in.neon_q && in.neon_size == 0);
    put16(0, 0xFFB1); put16(2, 0x0001);                  /* vcgt.s8 d0, d1, #0 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NEON_CMPZ && in.imm == 0 && in.vd == 0 && in.vm == 1 && in.neon_size == 0);

    /* Rejects. Each decodes to OP_NONE, the trap. */
    put16(0, 0xFFBC); put16(2, 0x0401);                  /* vcls size 3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFBC); put16(2, 0x0481);                  /* vclz size 3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFBC); put16(2, 0x0221);                  /* vpaddl size 3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFBC); put16(2, 0x0621);                  /* vpadal size 3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFBC); put16(2, 0x0701);                  /* vqabs size 3 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFBC); put16(2, 0x07C2);                  /* vqneg size 3, Q */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB4); put16(2, 0x0501);                  /* vcnt.8 with size 1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB4); put16(2, 0x0581);                  /* vmvn with size 1 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB8); put16(2, 0x0501);                  /* vcnt.8 with size 2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB8); put16(2, 0x0581);                  /* vmvn with size 2 */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB0); put16(2, 0x1442);                  /* vcls.s8 q1: odd Vd */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB0); put16(2, 0x1242);                  /* vpaddl.s8 q1: odd Vd */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB0); put16(2, 0x0743);                  /* vqabs.s8 q0, q3: odd Vm */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB0); put16(2, 0x0181);                  /* opc 0011: not claimed */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB0); put16(2, 0x0301);                  /* opc 0110: not claimed */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);
    put16(0, 0xFFB0); put16(2, 0x0381);                  /* opc 0111: not claimed */
    in = dec(ARM_T32, BASE);
    assert(in.op == OP_NONE && in.cls == A_SIMD);

    printf("ok   neon misc A=00: pairwise adds, vcls/vclz/vcnt/vmvn, vqabs/vqneg, and their rejects\n");
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
    test_homebrew_forms();
    test_neon_cmpz_forms();
    test_neon_misc_forms();
    test_neon_widen_forms();
    test_neon_shuffle_forms();
    test_neon_shift_imm_forms();
    test_neon_ldst_multi();
    test_neon_narrow_forms();
    test_neon_cmp_forms();
    test_neon_shl_reg_forms();
    test_neon_halving_forms();
    test_tables_hints_exclusives();
    printf("all decode tests passed\n");
    return 0;
}

