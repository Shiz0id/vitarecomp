/* bridgetest — a Vita program whose only job is to be recompiled.
 *
 * Every test in tests/ is synthetic, and that cannot reach the part of the
 * project that matters most: a real module, built by the real toolchain, run
 * through armrecomp and then through a real firmware layer. No game data can
 * be committed, so this is a module that can be: built from source with
 * VitaSDK, small enough to read, and touching each firmware area the bridge
 * forwards — threads, semaphores, the clock, memory blocks, file I/O and the
 * display — with an answer it can check itself.
 *
 * It reports by writing ux0:data/bridgetest.txt and printing through
 * sceClibPrintf, and exits with the number of failed checks.
 */

#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <stdint.h>

static char g_report[2048];
static int  g_len;
static int  g_failed;

static void say(const char *line) {
    sceClibPrintf("bridgetest: %s\n", line);
    int n = sceClibSnprintf(g_report + g_len, sizeof(g_report) - g_len, "%s\n", line);
    if (n > 0 && g_len + n < (int)sizeof(g_report)) g_len += n;
}

static void check(const char *what, int ok) {
    char line[160];
    sceClibSnprintf(line, sizeof(line), "%s %s", ok ? "PASS" : "FAIL", what);
    say(line);
    if (!ok) g_failed++;
}

/* --- integer and float work, so the arithmetic is the recompiler's --------- */

static uint32_t fib(uint32_t n) {
    uint32_t a = 0, b = 1;
    while (n--) { uint32_t t = a + b; a = b; b = t; }
    return a;
}

static float poly(float x) {
    /* volatile: keep the VFP arithmetic out of the constant folder */
    volatile float v = x;
    return v * v * 3.0f - v * 2.0f + 1.0f;
}

/* --- threads and a semaphore --------------------------------------------------
 *
 * Two workers each sum a range and add it to a shared total, holding a
 * semaphore used as a mutex. The main thread waits for both. If guest threads
 * shared one register file, or the semaphore did not block, the total is
 * wrong or the program hangs.
 */

static SceUID   g_lock;
static volatile uint32_t g_total;

typedef struct { uint32_t from, to; } range;

static int worker(SceSize args, void *argp) {
    const range *r = (const range *)argp;
    uint32_t sum = 0;
    for (uint32_t i = r->from; i < r->to; i++) sum += i;
    sceKernelWaitSema(g_lock, 1, NULL);
    g_total += sum;
    sceKernelSignalSema(g_lock, 1);
    return (int)(sum & 0x7FFFFFFF);
}

static void test_threads(void) {
    g_lock = sceKernelCreateSema("bt_lock", 0, 1, 1, NULL);
    check("semaphore created", g_lock >= 0);

    range a = { 0, 50000 }, b = { 50000, 100000 };
    SceUID ta = sceKernelCreateThread("bt_a", worker, 0x10000100, 0x4000, 0, 0, NULL);
    SceUID tb = sceKernelCreateThread("bt_b", worker, 0x10000100, 0x4000, 0, 0, NULL);
    check("threads created", ta >= 0 && tb >= 0);

    sceKernelStartThread(ta, sizeof(a), &a);
    sceKernelStartThread(tb, sizeof(b), &b);
    int sa = -1, sb = -1;
    sceKernelWaitThreadEnd(ta, &sa, NULL);
    sceKernelWaitThreadEnd(tb, &sb, NULL);

    /* sum of 0..99999 = 4999950000, mod 2^32 */
    check("threads summed 0..99999", g_total == (uint32_t)4999950000ull);
    check("thread exit status returned",
          (uint32_t)sa == (1249975000u & 0x7FFFFFFF) && (uint32_t)sb == (3749975000u & 0x7FFFFFFF));

    sceKernelDeleteThread(ta);
    sceKernelDeleteThread(tb);
    sceKernelDeleteSema(g_lock);
}

/* --- the display ------------------------------------------------------------- */

#define FB_W     960
#define FB_H     544
#define FB_PITCH 960
#define FB_SIZE  ((FB_PITCH * FB_H * 4 + 0x3FFFF) & ~0x3FFFF)  /* 256 KiB granule */

static void test_display(void) {
    SceUID block = sceKernelAllocMemBlock("bt_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, FB_SIZE, NULL);
    check("framebuffer block allocated", block >= 0);
    if (block < 0) return;

    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    uint32_t *px = (uint32_t *)base;
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++)
            px[y * FB_PITCH + x] = 0xFF000000u | ((uint32_t)(y * 255 / FB_H) << 8) | (uint32_t)(x * 255 / FB_W);

    SceDisplayFrameBuf fb = {
        .size        = sizeof(fb),
        .base        = base,
        .pitch       = FB_PITCH,
        .pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
        .width       = FB_W,
        .height      = FB_H,
    };
    check("framebuffer set", sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) == 0);
    for (int i = 0; i < 30; i++) sceDisplayWaitVblankStart();
    check("30 vblanks waited", 1);
}

/* --- file I/O ----------------------------------------------------------------- */

static void write_report(void) {
    SceUID fd = sceIoOpen("ux0:data/bridgetest.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) { sceClibPrintf("bridgetest: cannot open report (0x%08X)\n", fd); return; }
    sceIoWrite(fd, g_report, g_len);
    sceIoClose(fd);

    /* Read it back: the write is only real if the bytes come back. */
    char back[64] = {0};
    fd = sceIoOpen("ux0:data/bridgetest.txt", SCE_O_RDONLY, 0);
    int n = fd >= 0 ? sceIoRead(fd, back, sizeof(back) - 1) : -1;
    if (fd >= 0) sceIoClose(fd);
    sceClibPrintf("bridgetest: report read back %d bytes, starts \"%.12s\"\n", n, back);
}

int main(void) {
    say("start");

    check("fib(40) == 102334155", fib(40) == 102334155u);
    check("poly(2.5) == 14.75", poly(2.5f) == 14.75f);

    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    sceKernelDelayThread(20000);
    SceUInt64 t1 = sceKernelGetProcessTimeWide();
    check("20 ms delay advanced the clock", t1 - t0 >= 15000);

    test_threads();
    test_display();

    char line[64];
    sceClibSnprintf(line, sizeof(line), "done, %d failed", g_failed);
    say(line);
    write_report();

    sceKernelExitProcess(g_failed);
    return g_failed;
}
