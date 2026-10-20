/* neontest — NEON instructions run on known bytes, results printed.
 *
 * A unit test of a helper checks the helper against what its author thought
 * the instruction does. This checks it against dynarmic: tests/homebrew/run.sh
 * runs it under stock Vita3K and recompiled, and fails if any "NT" line
 * differs. Each line is d0-d7 after the op (or the 64 bytes a store wrote,
 * with 0xEE where it wrote nothing) and how far r0 moved, which catches a
 * wrong writeback amount. Add a T() line with each new NEON form that stock
 * Vita3K can run; run.sh fails the sample if stock logs one as undefined.
 */
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

static unsigned char src[64] __attribute__((aligned(32)));
static unsigned char dst[64] __attribute__((aligned(32)));
static unsigned delta;

static void dump(const char *name) {
    static const char hx[] = "0123456789abcdef";
    char line[140]; int p = 0;
    for (int i = 0; i < 64; i++) { line[p++] = hx[dst[i] >> 4]; line[p++] = hx[dst[i] & 15]; }
    line[p] = 0;
    sceClibPrintf("NT %-28s %s %u\n", name, line, delta);
}

/* Set d0-d7 from src (bytes 0..63), run `op` with r0 = src, r1 = dst,
 * then store d0-d7 to dst; delta is how far r0 moved. */
#define T(name, op) do { \
    sceClibMemset(dst, 0xEE, 64); \
    __asm__ volatile("mov r0, %1\n mov r1, %2\n vld1.8 {d0-d3}, [r0]\n add r2, r0, #32\n vld1.8 {d4-d7}, [r2]\n" \
                     op "\n" "sub r2, r0, %1\n mov %0, r2\n" \
                     : "=r"(delta) : "r"(src), "r"(dst) \
                     : "r0", "r1", "r2", "r3", "d0","d1","d2","d3","d4","d5","d6","d7", "memory"); \
    if (sceClibStrncmp(name, "vst", 3) != 0) \
        __asm__ volatile("vst1.8 {d0-d3}, [%0]\n add r2, %0, #32\n vst1.8 {d4-d7}, [r2]\n" \
                         :: "r"(dst) : "r2", "memory"); \
    dump(name); } while (0)

int main(void) {
    for (int i = 0; i < 64; i++) src[i] = (unsigned char)(i * 7 + 1);
    T("vld2.8 d0-d1",        "vld2.8 {d0-d1}, [r0]!");
    T("vld2.16 d0,d2",       "vld2.16 {d0,d2}, [r0]!");
    T("vld2.32 d0-d3 rm",    "mov r3, #40\n vld2.32 {d0-d3}, [r0], r3");
    T("vld2.8 d0-d3",        "vld2.8 {d0-d3}, [r0]!");
    T("vld3.8 d0-d2",        "vld3.8 {d0-d2}, [r0]!");
    T("vld3.16 d0,d2,d4",    "vld3.16 {d0,d2,d4}, [r0]!");
    T("vld3.32 d1-d3",       "vld3.32 {d1-d3}, [r0]");
    T("vld4.8 d0-d3",        "vld4.8 {d0-d3}, [r0]!");
    T("vld4.16 d0,d2,d4,d6", "vld4.16 {d0,d2,d4,d6}, [r0]!");
    T("vld4.32 d4-d7",       "vld4.32 {d4-d7}, [r0]");
    T("vst2.8 d0-d1",        "vst2.8 {d0-d1}, [r1]");
    T("vst2.16 d0-d3",       "vst2.16 {d0-d3}, [r1]");
    T("vst3.8 d0,d2,d4",     "vst3.8 {d0,d2,d4}, [r1]");
    T("vst3.32 d1-d3",       "vst3.32 {d1-d3}, [r1]");
    T("vst4.8 d0-d3",        "vst4.8 {d0-d3}, [r1]");
    T("vst4.16 d0,d2,d4,d6", "vst4.16 {d0,d2,d4,d6}, [r1]");
    T("vld1.8 d0[5] !",      "vld1.8 {d0[5]}, [r0]!");
    T("vld1.16 d1[2]",       "vld1.16 {d1[2]}, [r0]");
    T("vld1.32 d1[1] rm",    "mov r3, #40\n vld1.32 {d1[1]}, [r0], r3");
    T("vld2.8 d0[3] d1[3]",  "vld2.8 {d0[3],d1[3]}, [r0]");
    T("vld2.16 d0,d2[1] !",  "vld2.16 {d0[1],d2[1]}, [r0]!");
    T("vld2.16 d0,d2[1] rm", "mov r3, #40\n vld2.16 {d0[1],d2[1]}, [r0], r3");
    T("vld2.32 d1,d2[1]",    "vld2.32 {d1[1],d2[1]}, [r0]");
    T("vld2.32 d0,d2[1] !",  "vld2.32 {d0[1],d2[1]}, [r0]!");
    T("vld3.8 d0,d1,d2[7]",  "vld3.8 {d0[7],d1[7],d2[7]}, [r0]");
    T("vld3.16 d0,d2,d4[1] !", "vld3.16 {d0[1],d2[1],d4[1]}, [r0]!");
    T("vld3.32 d1-d3[1]",    "vld3.32 {d1[1],d2[1],d3[1]}, [r0]");
    T("vld4.8 d0-d3[1]",     "vld4.8 {d0[1],d1[1],d2[1],d3[1]}, [r0]");
    T("vld4.16 d0,d2,d4,d6[3] !", "vld4.16 {d0[3],d2[3],d4[3],d6[3]}, [r0]!");
    T("vld4.32 d0-d3[0]",    "vld4.32 {d0[0],d1[0],d2[0],d3[0]}, [r0]");
    T("vld4.32 d0,d2,d4,d6[1] rm", "mov r3, #40\n vld4.32 {d0[1],d2[1],d4[1],d6[1]}, [r0], r3");
    T("vst1.8 d0[7]",        "vst1.8 {d0[7]}, [r1]");
    T("vst1.16 d1[0] !",     "vst1.16 {d1[0]}, [r1]!");
    T("vst1.32 d1[1] rm",    "mov r3, #12\n vst1.32 {d1[1]}, [r1], r3");
    T("vst2.8 d0[2] d1[2]",  "vst2.8 {d0[2],d1[2]}, [r1]");
    T("vst2.16 d0,d2[1] !",  "vst2.16 {d0[1],d2[1]}, [r1]!");
    T("vst2.32 d0,d1[1]",    "vst2.32 {d0[1],d1[1]}, [r1]");
    T("vst3.8 d0-d2[6]",     "vst3.8 {d0[6],d1[6],d2[6]}, [r1]");
    T("vst3.16 d0-d2[2]",    "vst3.16 {d0[2],d1[2],d2[2]}, [r1]");
    T("vst3.32 d0,d2,d4[1] !", "vst3.32 {d0[1],d2[1],d4[1]}, [r1]!");
    T("vst4.8 d0-d3[5]",     "vst4.8 {d0[5],d1[5],d2[5],d3[5]}, [r1]");
    T("vst4.16 d0,d2,d4,d6[3]", "vst4.16 {d0[3],d2[3],d4[3],d6[3]}, [r1]");
    T("vst4.32 d0,d2,d4,d6[1] !", "vst4.32 {d0[1],d2[1],d4[1],d6[1]}, [r1]!");
    T("vst4.32 d1-d4[0] rm", "mov r3, #16\n vst4.32 {d1[0],d2[0],d3[0],d4[0]}, [r1], r3");
    T("vld1.8 d0[] !",       "vld1.8 {d0[]}, [r0]!");
    T("vld1.16 d0-d1[] !",   "vld1.16 {d0[], d1[]}, [r0]!");
    T("vld1.32 d2-d3[]",     "vld1.32 {d2[], d3[]}, [r0]");
    T("vld1.32 d1[] rm",     "mov r3, #40\n vld1.32 {d1[]}, [r0], r3");
    T("vld2.8 d0-d1[] !",    "vld2.8 {d0[], d1[]}, [r0]!");
    T("vld2.16 d0,d2[] !",   "vld2.16 {d0[],d2[]}, [r0]!");
    T("vld2.32 d2-d3[]",     "vld2.32 {d2[], d3[]}, [r0]");
    T("vld3.8 d0-d2[] !",    "vld3.8 {d0[], d1[], d2[]}, [r0]!");
    T("vld3.16 d0,d2,d4[]",  "vld3.16 {d0[],d2[],d4[]}, [r0]");
    T("vld3.32 d1-d3[] rm",  "mov r3, #40\n vld3.32 {d1[], d2[], d3[]}, [r0], r3");
    T("vld4.8 d0-d3[] !",    "vld4.8 {d0[], d1[], d2[], d3[]}, [r0]!");
    T("vld4.16 d0,d2,d4,d6[] !", "vld4.16 {d0[],d2[],d4[],d6[]}, [r0]!");
    T("vld4.32 d0-d3[] align", "vld4.32 {d0[], d1[], d2[], d3[]}, [r0 :128]");
    T("vaddl.s8 q",          "vaddl.s8 q0, d1, d2");
    T("vaddl.u16 q",         "vaddl.u16 q1, d2, d3");
    T("vaddw.s32 q",         "vaddw.s32 q0, q1, d4");
    T("vsubl.u8 q",          "vsubl.u8 q2, d0, d1");
    T("vsubw.s16 q overlap", "vsubw.s16 q0, q0, d2");
    T("vmull.s16 q",         "vmull.s16 q0, d1, d2");
    T("vmull.s16 q overlap", "vmull.s16 q0, d0, d1");
    T("vmull.u32 q",         "vmull.u32 q1, d0, d3");
    T("vmull.u8 q",          "vmull.u8 q1, d4, d5");
    T("vmlal.s8 q",          "vmlal.s8 q0, d1, d2");
    T("vmlal.u32 q",         "vmlal.u32 q3, d6, d7");
    T("vmlsl.u16 q",         "vmlsl.u16 q1, d2, d3");
    T("vmovl.s8 q",          "vmovl.s8 q0, d1");
    T("vmovl.u16 q",         "vmovl.u16 q1, d4");
    T("vmovl.s32 q",         "vmovl.s32 q2, d5");
    T("vshll.u8 q #3",       "vshll.u8 q0, d1, #3");
    T("vshll.s16 q #15",     "vshll.s16 q0, d1, #15");
    T("vshll.u32 q #1",      "vshll.u32 q1, d2, #1");
    T("vshll.i8 q #8",       "vshll.i8 q0, d1, #8");
    T("vshll.i16 q #16",     "vshll.i16 q1, d4, #16");
    T("vshll.i32 q #32",     "vshll.i32 q2, d5, #32");
    T("vrshr.s8 d #1",        "vrshr.s8 d0, d1, #1");
    T("vrshr.u16 q #16",      "vrshr.u16 q0, q1, #16");
    T("vrshr.s32 d #7",       "vrshr.s32 d2, d3, #7");
    T("vrshr.u64 q #64",      "vrshr.u64 q0, q1, #64");
    T("vrshr.s64 d #1",       "vrshr.s64 d2, d3, #1");
    T("vrsra.s8 d #8",        "vrsra.s8 d0, d1, #8");
    T("vrsra.u32 q #3",       "vrsra.u32 q0, q1, #3");
    T("vrsra.s64 d #1",       "vrsra.s64 d2, d3, #1");
    T("vrsra.u16 q overlap",  "vrsra.u16 q0, q0, #2");
    T("vsri.8 d #1",          "vsri.8 d0, d1, #1");
    T("vsri.16 q #16",        "vsri.16 q0, q1, #16");
    T("vsri.32 d #5",         "vsri.32 d2, d3, #5");
    T("vsri.64 d #64",        "vsri.64 d2, d3, #64");
    T("vsri.8 d overlap #3",  "vsri.8 d0, d0, #3");
    T("vsli.8 d #0",          "vsli.8 d0, d1, #0");
    T("vsli.16 q #15",        "vsli.16 q0, q1, #15");
    T("vsli.32 d #31",        "vsli.32 d2, d3, #31");
    T("vsli.64 q #1",         "vsli.64 q0, q1, #1");
    T("vsli.16 d overlap #5", "vsli.16 d0, d0, #5");
    T("vqshl.s8 d #1",        "vqshl.s8 d0, d1, #1");
    T("vqshl.u16 q #15",      "vqshl.u16 q0, q1, #15");
    T("vqshl.s32 d #0",       "vqshl.s32 d2, d3, #0");
    T("vqshl.u64 d #63",      "vqshl.u64 d2, d3, #63");
    T("vqshlu.s8 d #7",       "vqshlu.s8 d0, d1, #7");
    T("vqshlu.s16 q #1",      "vqshlu.s16 q0, q1, #1");
    T("vqshlu.s32 d #0",      "vqshlu.s32 d2, d3, #0");
    T("vqshlu.s64 q #10",     "vqshlu.s64 q0, q1, #10");
    T("vcvt.s32.f32 d #1",    "vcvt.s32.f32 d0, d1, #1");
    T("vcvt.s32.f32 d #8",    "vcvt.s32.f32 d0, d1, #8");
    T("vcvt.u32.f32 q #32",   "vcvt.u32.f32 q0, q1, #32");
    T("vcvt.f32.s32 d #16",   "vcvt.f32.s32 d2, d3, #16");
    T("vcvt.f32.u32 q #1",    "vcvt.f32.u32 q0, q1, #1");
    T("vcvt.f32.s32 q #32",   "vcvt.f32.s32 q0, q1, #32");
    T("vext.8 d",            "vext.8 d0, d1, d2, #3");
    T("vext.8 q",            "vext.8 q0, q1, q2, #11");
    T("vrev64.8",            "vrev64.8 d0, d1");
    T("vrev64.32 q",         "vrev64.32 q0, q1");
    T("vrev32.16",           "vrev32.16 d0, d1");
    T("vrev16.8 q",          "vrev16.8 q0, q1");
    T("vswp q",              "vswp q0, q1");
    T("vtrn.8",              "vtrn.8 d0, d1");
    T("vtrn.16 q",           "vtrn.16 q0, q1");
    T("vtrn.32 q",           "vtrn.32 q0, q1");
    T("vuzp.8",              "vuzp.8 d0, d1");
    T("vuzp.16 q",           "vuzp.16 q0, q1");
    T("vuzp.32 q",           "vuzp.32 q0, q1");
    T("vzip.8",              "vzip.8 d0, d1");
    T("vzip.16 q",           "vzip.16 q0, q1");
    T("vzip.32 q",           "vzip.32 q0, q1");
    T("vdup.8 lane",         "vdup.8 d0, d1[5]");
    T("vdup.16 q lane",      "vdup.16 q0, d3[2]");
    T("vdup.32 lane",        "vdup.32 d0, d7[1]");
    T("vqdmulh.s16 d",       "vqdmulh.s16 d0, d1, d2");
    T("vqdmulh.s32 d",       "vqdmulh.s32 d0, d1, d2");
    T("vqdmulh.s16 q",       "vqdmulh.s16 q0, q1, q2");
    T("vqdmulh.s32 q=n",     "vqdmulh.s32 q0, q0, q1");
    T("vqrdmulh.s16 q",      "vqrdmulh.s16 q0, q1, q2");
    T("vqrdmulh.s32 d",      "vqrdmulh.s32 d0, d1, d2");
    T("vqrdmulh.s16 d=m",    "vqrdmulh.s16 d0, d1, d0");
    T("vqrdmulh.s32 q",      "vqrdmulh.s32 q0, q1, q2");
    T("vmla.i16 d lane",     "vmla.i16 d0, d1, d2[1]");
    T("vmla.i32 q lane",     "vmla.i32 q0, q1, d2[1]");
    T("vmls.i16 q lane",     "vmls.i16 q0, q1, d3[3]");
    T("vmls.i32 d lane",     "vmls.i32 d0, d1, d7[0]");
    T("vmul.i16 d lane",     "vmul.i16 d0, d1, d2[3]");
    T("vmul.i32 q scalar in Qd", "vmul.i32 q0, q1, d1[1]");
    T("vmlal.s16 lane",      "vmlal.s16 q0, d1, d2[0]");
    T("vmlal.u32 lane",      "vmlal.u32 q0, d1, d3[1]");
    T("vmlsl.u16 lane",      "vmlsl.u16 q1, d2, d7[3]");
    T("vmlsl.s32 lane",      "vmlsl.s32 q0, d1, d2[0]");
    T("vmull.s16 lane",      "vmull.s16 q1, d2, d3[2]");
    T("vmull.s16 Dn in Qd",  "vmull.s16 q0, d0, d1[2]");
    T("vmull.u32 lane",      "vmull.u32 q1, d2, d3[1]");
    T("vqdmull.s16 lane",    "vqdmull.s16 q0, d1, d2[1]");
    T("vqdmull.s32 lane",    "vqdmull.s32 q1, d2, d3[1]");
    /* No VQDMULL/VQDMLAL/VQDMLSL (vector) nor VQDMLAL/VQDMLSL (scalar): dynarmic
     * does not implement them, so stock has no result to compare; they are
     * checked against an independent reference instead. */
    T("vqdmulh.s16 d lane",  "vqdmulh.s16 d0, d1, d2[2]");
    T("vqdmulh.s32 q lane",  "vqdmulh.s32 q0, q1, d3[1]");
    T("vqrdmulh.s16 q lane", "vqrdmulh.s16 q0, q1, d7[2]");
    T("vqrdmulh.s32 d lane", "vqrdmulh.s32 d0, d1, d7[1]");
    T("vtbl1",               "vmov.i8 d2, #0x05\n vtbl.8 d0, {d1}, d2");
    T("vtbl4",               "vmov.i8 d6, #30\n vtbl.8 d0, {d1-d4}, d6");
    T("vtbl range",          "vmov.i8 d6, #33\n vtbl.8 d0, {d1-d4}, d6");
    T("vtbx range",          "vmov.i8 d6, #40\n vtbx.8 d0, {d1-d4}, d6");
    T("vtbx mixed",          "vtbx.8 d0, {d1-d2}, d0");
    T("vtbl overlap",        "vtbl.8 d1, {d1-d2}, d1");
    T("vqmovn.s16",          "vqmovn.s16 d0, q1");
    T("vqmovn.u32",          "vqmovn.u32 d1, q2");
    T("vqmovn.s64",          "vqmovn.s64 d2, q3");
    T("vqmovun.s16",         "vqmovun.s16 d0, q1");
    T("vqmovun.s64",         "vqmovun.s64 d0, q1");
    T("vshrn.i16 #3",        "vshrn.i16 d0, q1, #3");
    T("vshrn.i32 #16",       "vshrn.i32 d0, q1, #16");
    T("vshrn.i64 #1",        "vshrn.i64 d0, q1, #1");
    T("vrshrn.i16 #8",       "vrshrn.i16 d0, q1, #8");
    T("vrshrn.i64 d3 #32",   "vrshrn.i64 d3, q2, #32");
    T("vqshrn.s16 #1",       "vqshrn.s16 d0, q1, #1");
    T("vqshrn.u32 #5",       "vqshrn.u32 d0, q1, #5");
    T("vqrshrn.s32 #4",      "vqrshrn.s32 d0, q1, #4");
    T("vqrshrn.u64 #31",     "vqrshrn.u64 d0, q1, #31");
    T("vqshrun.s64 #32",     "vqshrun.s64 d0, q1, #32");
    T("vqrshrun.s16 #2",     "vqrshrun.s16 d0, q1, #2");
    T("vcgt.s8 #0 d",        "vcgt.s8 d0, d1, #0");
    T("vcge.s16 #0 q",       "vcge.s16 q0, q1, #0");
    T("vcle.s32 #0 d",       "vcle.s32 d4, d5, #0");
    T("vclt.s8 #0 q",        "vclt.s8 q2, q3, #0");
    T("vclt.s16 #0 same",    "vclt.s16 d1, d1, #0");
    T("vceq.i8 #0 mixed d",  "vmov.i64 d2, #0xff00ff00ff00ff00\n vceq.i8 d0, d2, #0");
    T("vceq.i16 #0 mixed d", "vmov.i64 d2, #0xffff0000ffff0000\n vceq.i16 d0, d2, #0");
    T("vceq.i32 #0 mixed d", "vmov.i64 d2, #0xffffffff00000000\n vceq.i32 d0, d2, #0");
    T("vceq.i32 #0 q",       "vmov.i32 q2, #0\n vceq.i32 q0, q2, #0");
    T("vcgt.f32 #0 d",       "vcgt.f32 d0, d1, #0");
    T("vcge.f32 #0 q",       "vcge.f32 q0, q1, #0");
    T("vceq.f32 #0 zero q",  "vmov.i32 q2, #0\n vceq.f32 q0, q2, #0");
    T("vcle.f32 #0 d",       "vcle.f32 d4, d5, #0");
    T("vclt.f32 #0 q",       "vclt.f32 q2, q3, #0");
    /* The float forms of the rest of the same-length group, and the F16/F32
     * conversions. Special values are built inside the op string: +Inf is
     * 0xff000000 shifted right one, NaN is +Inf with 0x00400000 set. */
    T("vpadd.f32 d",         "vpadd.f32 d0, d1, d2");
    T("vpadd.f32 d=n",       "vpadd.f32 d0, d0, d1");
    T("vpadd.f32 inf 0",     "vmov.i32 d1, #0xff000000\n vshr.u32 d1, d1, #1\n vmov.i32 d2, #0\n vpadd.f32 d0, d1, d2");
    T("vpmax.f32 d",         "vpmax.f32 d0, d1, d2");
    T("vpmax.f32 d=n",       "vpmax.f32 d0, d0, d1");
    T("vpmin.f32 d",         "vpmin.f32 d3, d4, d5");
    T("vpmin.f32 -0 +0",     "vmov.i32 d4, #0x80000000\n vmov.i32 d5, #0\n vpmin.f32 d3, d4, d5");
    T("vabd.f32 d",          "vabd.f32 d0, d1, d2");
    T("vabd.f32 q",          "vabd.f32 q0, q1, q2");
    T("vabd.f32 inf d",      "vmov.i32 d1, #0xff000000\n vshr.u32 d1, d1, #1\n vabd.f32 d0, d1, d2");
    T("vacge.f32 d",         "vacge.f32 d0, d1, d2");
    T("vacgt.f32 q",         "vacgt.f32 q0, q1, q2");
    T("vacge.f32 -0 +0",     "vmov.i32 d1, #0x80000000\n vmov.i32 d2, #0\n vacge.f32 d0, d1, d2");
    T("vacgt.f32 nan",       "vmov.i32 d1, #0xff000000\n vshr.u32 d1, d1, #1\n vmov.i32 d2, #0xff000000\n vshr.u32 d2, d2, #1\n vorr.i32 d2, #0x00400000\n vacgt.f32 d0, d1, d2");
    T("vrecps.f32 d",        "vrecps.f32 d0, d1, d2");
    T("vrecps.f32 q",        "vrecps.f32 q0, q1, q2");
    T("vrecps inf*0",        "vmov.i32 d1, #0xff000000\n vshr.u32 d1, d1, #1\n vmov.i32 d2, #0\n vrecps.f32 d0, d1, d2");
    T("vrsqrts.f32 d",       "vrsqrts.f32 d0, d1, d2");
    T("vrsqrts.f32 q",       "vrsqrts.f32 q0, q1, q2");
    T("vrsqrts inf*0",       "vmov.i32 d1, #0xff000000\n vshr.u32 d1, d1, #1\n vmov.i32 d2, #0\n vrsqrts.f32 d0, d1, d2");
    T("vrsqrts 1*1",         "vmov.f32 d1, #1.0\n vmov.f32 d2, #1.0\n vrsqrts.f32 d0, d1, d2");
    T("vcvt.f16.f32 d2,q1",  "vcvt.f16.f32 d2, q1");
    T("vcvt.f16.f32 d0,q1",  "vcvt.f16.f32 d0, q1");
    T("vcvt.f16.f32 1.0",    "vmov.f32 q1, #1.0\n vcvt.f16.f32 d0, q1");
    T("vcvt.f16.f32 inf q",  "vmov.i32 q2, #0xff000000\n vshr.u32 q2, q2, #1\n vcvt.f16.f32 d0, q2");
    T("vcvt.f32.f16 q0,d1",  "vcvt.f32.f16 q0, d1");
    T("vcvt.f32.f16 q1,d0",  "vcvt.f32.f16 q1, d0");
    T("vcvt.f32.f16 denorm", "vmov.i16 d1, #1\n vcvt.f32.f16 q0, d1");
    T("vcvt.f32.f16 inf",    "vmov.i16 d1, #0x7c00\n vcvt.f32.f16 q0, d1");
    T("vabs.s8 q",           "vabs.s8 q0, q2");
    T("vabs.s8 min d",       "vmov.i8 d2, #0x80\n vabs.s8 d0, d2");
    T("vabs.s16 d",          "vabs.s16 d0, d1");
    T("vabs.s32 q",          "vabs.s32 q0, q1");
    T("vneg.s8 d",           "vneg.s8 d0, d3");
    T("vneg.s16 min q",      "vmov.i16 q2, #0x8000\n vneg.s16 q0, q2");
    T("vneg.s16 q",          "vneg.s16 q0, q1");
    T("vneg.s32 d",          "vneg.s32 d0, d3");
    T("vpaddl.u8 d",         "vpaddl.u8 d0, d1");
    T("vpaddl.s16 q",        "vpaddl.s16 q0, q2");
    T("vpaddl.u32 d",        "vpaddl.u32 d0, d3");
    T("vpadal.s8 d",         "vpadal.s8 d0, d1");
    T("vpadal.s16 q",        "vpadal.s16 q0, q1");
    T("vpadal.u32 d overlap", "vpadal.u32 d0, d0");
    T("vcls.s8 d",           "vcls.s8 d0, d1");
    T("vcls.s16 q",          "vcls.s16 q0, q1");
    T("vcls.s32 d",          "vcls.s32 d0, d3");
    T("vclz.i8 q",           "vclz.i8 q0, q1");
    T("vclz.i16 d",          "vclz.i16 d0, d1");
    T("vclz.i32 q",          "vclz.i32 q0, q2");
    T("vcnt.8 d",            "vcnt.8 d0, d3");
    T("vcnt.8 q",            "vcnt.8 q0, q2");
    T("vmvn d",              "vmvn d0, d3");
    T("vmvn q",              "vmvn q0, q1");
    T("vqabs.s8 min",        "vmov.i8 d2, #0x80\n vqabs.s8 d0, d2");
    T("vqabs.s16 q",         "vqabs.s16 q0, q1");
    T("vqabs.s32 d",         "vqabs.s32 d0, d1");
    T("vqneg.s8 q",          "vqneg.s8 q0, q1");
    T("vqneg.s16 min d",     "vmov.i16 d2, #0x8000\n vqneg.s16 d0, d2");
    T("vqneg.s32 q",         "vqneg.s32 q0, q1");
    T("vcgt.s16 #0 q",       "vcgt.s16 q1, q2, #0");
    T("vcgt.s32 #0 d",       "vcgt.s32 d5, d6, #0");
    T("vcge.s8 #0 d",        "vcge.s8 d2, d3, #0");
    T("vcge.s32 #0 q",       "vcge.s32 q0, q3, #0");
    T("vcle.s8 #0 q",        "vcle.s8 q0, q1, #0");
    T("vcle.s16 #0 d",       "vcle.s16 d6, d7, #0");
    T("vclt.s32 #0 d",       "vclt.s32 d0, d1, #0");
    T("vcgt.s8 d",           "vcgt.s8 d0, d1, d2");
    T("vcgt.u8 d",           "vcgt.u8 d0, d1, d2");
    T("vcgt.s16 q",          "vcgt.s16 q0, q1, q2");
    T("vcgt.u32 d",          "vcgt.u32 d2, d3, d4");
    T("vcge.s8 q",           "vcge.s8 q0, q1, q2");
    T("vcge.u16 d",          "vcge.u16 d0, d1, d2");
    T("vcge.s32 q",          "vcge.s32 q0, q1, q2");
    T("vcge.u8 q",           "vcge.u8 q0, q1, q2");
    T("vmax.s8 d",           "vmax.s8 d0, d1, d2");
    T("vmax.u16 q",          "vmax.u16 q0, q1, q2");
    T("vmax.s32 d",          "vmax.s32 d0, d1, d2");
    T("vmin.s16 d",          "vmin.s16 d0, d1, d2");
    T("vmin.u32 q",          "vmin.u32 q0, q1, q2");
    T("vabd.s8 d",           "vabd.s8 d0, d1, d2");
    T("vabd.u8 q",           "vabd.u8 q0, q1, q2");
    T("vabd.s32 d",          "vabd.s32 d0, d1, d2");
    T("vaba.s8 d",           "vaba.s8 d0, d1, d2");
    T("vaba.u16 q",          "vaba.u16 q0, q1, q2");
    T("vaba.s32 q",          "vaba.s32 q0, q1, q2");
    T("vaba.u32 d",          "vaba.u32 d0, d1, d2");
    T("vshl.s8 d, -3",       "vmov.i8 d2, #0xfd\n vshl.s8 d0, d1, d2");
    T("vshl.u16 d, byte 0",  "vmov.i16 d2, #0x0300\n vshl.u16 d0, d1, d2");
    T("vshl.u32 q, -2",      "vmov.i8 q2, #0xfe\n vshl.u32 q0, q1, q2");
    T("vshl.s64 d, -1",      "vmov.i64 d2, #0xff\n vshl.s64 d0, d1, d2");
    T("vshl.s16 d, vd=vm",   "vshl.s16 d0, d0, d1");
    T("vshl.u64 d, raw",     "vshl.u64 d3, d4, d5");
    T("vqshl.s8 d, 3",       "vmov.i8 d2, #3\n vqshl.s8 d0, d1, d2");
    T("vqshl.u16 q, 127",    "vmov.i8 q2, #0x7f\n vqshl.u16 q0, q1, q2");
    T("vqshl.s64 d, raw",    "vqshl.s64 d0, d1, d2");
    T("vrshl.s8 d, -1",      "vmov.i8 d2, #0xff\n vrshl.s8 d0, d1, d2");
    T("vrshl.u32 q, -1",     "vmov.i8 q2, #0xff\n vrshl.u32 q0, q1, q2");
    T("vrshl.u64 d, -1",     "vmov.i64 d2, #0xff\n vrshl.u64 d0, d1, d2");
    /* No VQRSHL: dynarmic does not implement it (its decoder line is
     * commented out), so stock skips it and has no result to compare. */
    T("vhadd.s8 d",          "vhadd.s8 d0, d1, d2");
    T("vhadd.u16 q",         "vhadd.u16 q0, q1, q2");
    T("vhadd.s32 d=n",       "vhadd.s32 d0, d0, d1");
    T("vrhadd.u8 d",         "vrhadd.u8 d0, d1, d2");
    T("vrhadd.s16 q",        "vrhadd.s16 q0, q1, q2");
    T("vrhadd.u32 d",        "vrhadd.u32 d3, d2, d1");
    T("vrhadd.u8 q=n",       "vrhadd.u8 q0, q0, q1");
    T("vhsub.s8 d",          "vhsub.s8 d0, d1, d2");
    T("vhsub.u16 d",         "vhsub.u16 d0, d1, d2");
    T("vhsub.s32 q",         "vhsub.s32 q0, q1, q2");
    T("vhsub.u8 q",          "vhsub.u8 q0, q1, q2");
    T("vpmax.s8 d",          "vpmax.s8 d0, d1, d2");
    T("vpmax.u16 d",         "vpmax.u16 d0, d1, d2");
    T("vpmax.s32 d=n",       "vpmax.s32 d0, d0, d3");
    T("vpmin.u8 d",          "vpmin.u8 d0, d1, d2");
    T("vpmin.s16 d",         "vpmin.s16 d0, d1, d2");
    T("vpmin.u32 d=n",       "vpmin.u32 d0, d0, d1");
    /* No VADDHN/VRADDHN/VSUBHN/VRSUBHN: dynarmic does not implement them, so
     * stock has no result to compare. They are checked against an
     * independent reference instead. */
    T("vabal.s8 q",          "vabal.s8 q0, d2, d3");
    T("vabal.u8 overlap",    "vabal.u8 q0, d0, d1");
    T("vabal.u16 q",         "vabal.u16 q2, d6, d7");
    T("vabal.s32 q",         "vabal.s32 q1, d4, d5");
    T("vabdl.u8 q",          "vabdl.u8 q0, d1, d2");
    T("vabdl.u8 overlap",    "vabdl.u8 q0, d0, d1");
    T("vabdl.s8 q",          "vabdl.s8 q2, d6, d7");
    T("vabdl.s16 q",         "vabdl.s16 q1, d4, d5");
    T("vabdl.u32 q",         "vabdl.u32 q2, d0, d1");
    T("vmull.p8 q",          "vmull.p8 q0, d1, d2");
    T("vmull.p8 overlap",    "vmull.p8 q1, d2, d3");
    T("vmul.p8 d",           "vmul.p8 d0, d1, d2");
    T("vmul.p8 q",           "vmul.p8 q0, q1, q2");
    T("vmul.p8 q overlap",   "vmul.p8 q0, q0, q1");
    /* VRECPE and VRSQRTE on the src bytes, then on special values set
     * through r2 and r3: d2 gets r3 in its low lane and r2 in its high one. */
    T("vrecpe.u32 d",        "vrecpe.u32 d0, d1");
    T("vrecpe.u32 q",        "vrecpe.u32 q0, q2");
    T("vrsqrte.u32 d",       "vrsqrte.u32 d0, d5");
    T("vrsqrte.u32 q=m",     "vrsqrte.u32 q1, q1");
    T("vrecpe.f32 d",        "vrecpe.f32 d0, d1");
    T("vrecpe.f32 q",        "vrecpe.f32 q0, q3");
    T("vrsqrte.f32 d",       "vrsqrte.f32 d0, d4");
    T("vrsqrte.f32 q=m",     "vrsqrte.f32 q2, q2");
    T("vrecpe.u32 edges",    "movw r3, #0\n movt r3, #0x8000\n movw r2, #0xffff\n movt r2, #0x7fff\n vmov d2, r3, r2\n vrecpe.u32 d0, d2");
    T("vrsqrte.u32 edges",   "movw r3, #0\n movt r3, #0x4000\n movw r2, #0xffff\n movt r2, #0x3fff\n vmov d2, r3, r2\n vrsqrte.u32 d0, d2");
    T("vrecpe.f32 1,-0",     "vmov.f32 d2, #1.0\n vmov.i32 d3, #0x80000000\n vrecpe.f32 q0, q1");
    T("vrecpe.f32 -inf,nan", "movw r3, #0\n movt r3, #0xff80\n movw r2, #1\n movt r2, #0x7f80\n vmov d2, r3, r2\n vrecpe.f32 d0, d2");
    T("vrecpe.f32 2^126,den", "movw r3, #0\n movt r3, #0x7e80\n movw r2, #1\n movt r2, #0x8000\n vmov d2, r3, r2\n vrecpe.f32 d0, d2");
    T("vrecpe.f32 max,min",  "movw r3, #0xffff\n movt r3, #0x7e7f\n movw r2, #0\n movt r2, #0x0080\n vmov d2, r3, r2\n vrecpe.f32 d0, d2");
    T("vrsqrte.f32 1,-0",    "vmov.f32 d2, #1.0\n vmov.i32 d3, #0x80000000\n vrsqrte.f32 q0, q1");
    T("vrsqrte.f32 -inf,nan", "movw r3, #0\n movt r3, #0xff80\n movw r2, #1\n movt r2, #0x7f80\n vmov d2, r3, r2\n vrsqrte.f32 d0, d2");
    T("vrsqrte.f32 inf,-den", "movw r3, #0\n movt r3, #0x7f80\n movw r2, #1\n movt r2, #0x8000\n vmov d2, r3, r2\n vrsqrte.f32 d0, d2");
    T("vrsqrte.f32 2,-1",    "vmov.f32 d2, #2.0\n vmov.f32 d3, #-1.0\n vrsqrte.f32 q0, q1");
    sceClibPrintf("NT done\n");
    sceKernelExitProcess(0);
    return 0;
}
