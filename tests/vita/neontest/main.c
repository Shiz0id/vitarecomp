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
    T("vabs.s8 q",           "vabs.s8 q0, q2");
    T("vabs.s8 min d",       "vmov.i8 d2, #0x80\n vabs.s8 d0, d2");
    T("vabs.s16 d",          "vabs.s16 d0, d1");
    T("vabs.s32 q",          "vabs.s32 q0, q1");
    T("vneg.s8 d",           "vneg.s8 d0, d3");
    T("vneg.s16 min q",      "vmov.i16 q2, #0x8000\n vneg.s16 q0, q2");
    T("vneg.s16 q",          "vneg.s16 q0, q1");
    T("vneg.s32 d",          "vneg.s32 d0, d3");
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
    sceClibPrintf("NT done\n");
    sceKernelExitProcess(0);
    return 0;
}
