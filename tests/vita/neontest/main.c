/* neontest — NEON instructions run on known bytes, results printed.
 *
 * A unit test of a helper checks the helper against what its author thought
 * the instruction does. This checks it against dynarmic: tests/homebrew/run.sh
 * runs it under stock Vita3K and recompiled, and fails if any "NT" line
 * differs. Each line is d0-d7 after the op (or the 64 bytes a store wrote,
 * with 0xEE where it wrote nothing) and how far r0 moved, which catches a
 * wrong writeback amount. Add a T() line with each new NEON form.
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
    sceClibPrintf("NT done\n");
    sceKernelExitProcess(0);
    return 0;
}
