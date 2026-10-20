/* test_runtime.c — the semantic helpers.
 *
 * Every function here exists because the obvious C is wrong, so every test
 * here is a case where the obvious C would pass a casual reading and produce a
 * bad answer. The values are worked out from the architecture's definition, not
 * from what the implementation happens to return.
 */

#include "vitarecomp/recomp_rt.h"
#include "vitarecomp/dispatch.h"
#include "vitarecomp/hle.h"

#include <assert.h>
#include <stdio.h>

#ifdef NDEBUG
#error "tests must be built with asserts enabled (NDEBUG is defined)"
#endif

/* --- shifts ----------------------------------------------------------------- */

static void test_shifts(void) {
    /* The headline case. C leaves a shift by >= the width undefined, and on x86
     * the hardware masks the count to 5 bits — so the naive `v << n` returns
     * the operand UNCHANGED for n == 32, which is the opposite of ARM's answer.
     * This is invisible until a shift amount is computed rather than constant. */
    assert(vita_lsl(1u, 32) == 0u);
    assert(vita_lsl(0xFFFFFFFFu, 32) == 0u);
    assert(vita_lsl(1u, 33) == 0u);
    assert(vita_lsr(0x80000000u, 32) == 0u);

    assert(vita_lsl(1u, 0) == 1u);
    assert(vita_lsl(1u, 31) == 0x80000000u);
    assert(vita_lsr(0x80000000u, 31) == 1u);

    /* ASR saturates to the sign bit rather than reaching zero. */
    assert(vita_asr(0x80000000u, 32) == 0xFFFFFFFFu);
    assert(vita_asr(0x80000000u, 99) == 0xFFFFFFFFu);
    assert(vita_asr(0x7FFFFFFFu, 32) == 0u);
    assert(vita_asr(0x80000000u, 31) == 0xFFFFFFFFu);
    assert(vita_asr(0xFFFFFFF0u, 4)  == 0xFFFFFFFFu);
    assert(vita_asr(0x00000010u, 4)  == 1u);
    assert(vita_asr(0x12345678u, 0)  == 0x12345678u);

    /* ROR by 32 is the identity, not zero. */
    assert(vita_ror(0x12345678u, 32) == 0x12345678u);
    assert(vita_ror(0x00000001u, 1)  == 0x80000000u);
    assert(vita_ror(0x80000000u, 31) == 0x00000001u);

    printf("  shifts (>=32 defined)          ok\n");
}

/* --- flags ------------------------------------------------------------------ */

static void test_flags_add(void) {
    vita_flags_add(1u, 1u, 2u);
    assert(!flag_n && !flag_z && !flag_c && !flag_v);

    /* Unsigned wrap sets carry. */
    vita_flags_add(0xFFFFFFFFu, 1u, 0u);
    assert(flag_z && flag_c);

    /* Signed overflow is a different condition entirely: two positives
     * producing a negative. Carry is NOT set here. */
    vita_flags_add(0x7FFFFFFFu, 1u, 0x80000000u);
    assert(flag_v && !flag_c && flag_n);

    /* Two negatives producing a positive: overflow AND carry. */
    vita_flags_add(0x80000000u, 0x80000000u, 0u);
    assert(flag_v && flag_c && flag_z);

    printf("  flags: add carry vs overflow   ok\n");
}

static void test_flags_sub(void) {
    /* ARM sets carry on a subtraction when there is NO borrow. This is the
     * opposite of the intuitive reading and of several other architectures, and
     * getting it backwards inverts every unsigned comparison in the program. */
    vita_flags_sub(5u, 3u, 2u);
    assert(flag_c);                 /* 5 >= 3, no borrow -> carry SET   */

    vita_flags_sub(3u, 5u, (uint32_t)-2);
    assert(!flag_c);                /* 3 < 5, borrowed   -> carry CLEAR */

    vita_flags_sub(5u, 5u, 0u);
    assert(flag_c && flag_z);       /* equal: no borrow, and zero       */

    /* Signed overflow on subtraction: positive minus negative going negative. */
    vita_flags_sub(0x7FFFFFFFu, 0xFFFFFFFFu, 0x80000000u);
    assert(flag_v);

    printf("  flags: sub carry is NOT borrow ok\n");
}

static void test_flags_adc(void) {
    /* The edge case that `res < a` alone gets wrong: with a carry in, adding
     * 0xFFFFFFFF leaves the value unchanged while genuinely carrying. A naive
     * test sees res == a, concludes no carry, and silently breaks every
     * multi-word addition in the program. */
    vita_flags_adc(5u, 0xFFFFFFFFu, 1u, 5u);
    assert(flag_c);

    vita_flags_adc(5u, 0u, 0u, 5u);
    assert(!flag_c);

    vita_flags_adc(0xFFFFFFFFu, 0u, 1u, 0u);
    assert(flag_c && flag_z);

    printf("  flags: adc carry-in edge case  ok\n");
}

/* --- condition codes -------------------------------------------------------- */

static void test_conditions(void) {
    /* eq / ne */
    flag_z = 1; assert(vita_cond(0x0)); assert(!vita_cond(0x1));
    flag_z = 0; assert(!vita_cond(0x0)); assert(vita_cond(0x1));

    /* cs / cc */
    flag_c = 1; assert(vita_cond(0x2)); assert(!vita_cond(0x3));
    flag_c = 0; assert(!vita_cond(0x2)); assert(vita_cond(0x3));

    /* The signed conditions depend on N and V agreeing, which is the part that
     * is easy to write as a plain comparison and get wrong. */
    flag_n = 0; flag_v = 0; assert(vita_cond(0xA));  /* ge */
    flag_n = 1; flag_v = 1; assert(vita_cond(0xA));  /* ge: both set */
    flag_n = 1; flag_v = 0; assert(!vita_cond(0xA));
    flag_n = 1; flag_v = 0; assert(vita_cond(0xB));  /* lt */

    /* gt is ge AND not-zero; le is its complement. */
    flag_n = 0; flag_v = 0; flag_z = 0; assert(vita_cond(0xC));
    flag_z = 1;                          assert(!vita_cond(0xC));
    flag_z = 1;                          assert(vita_cond(0xD));

    /* hi is carry AND not-zero — distinct from cs. */
    flag_c = 1; flag_z = 0; assert(vita_cond(0x8));
    flag_c = 1; flag_z = 1; assert(!vita_cond(0x8));
    flag_c = 1; flag_z = 1; assert(vita_cond(0x9));  /* ls */

    assert(vita_cond(0xE));              /* al is always true */

    printf("  condition codes, all 16        ok\n");
}

/* --- sign extension and byte reversal --------------------------------------- */

static void test_extends(void) {
    assert(vita_sxtb(0x7F) == 0x7Fu);
    assert(vita_sxtb(0x80) == 0xFFFFFF80u);
    assert(vita_sxtb(0x12345680u) == 0xFFFFFF80u);   /* high bits ignored */
    assert(vita_sxth(0x7FFF) == 0x7FFFu);
    assert(vita_sxth(0x8000) == 0xFFFF8000u);
    assert(vita_uxtb(0x1234) == 0x34u);
    assert(vita_uxth(0x12345678u) == 0x5678u);
    assert(vita_rev(0x12345678u) == 0x78563412u);
    printf("  extends and rev                ok\n");
}

/* --- bitfields -------------------------------------------------------------- */

/* Expected values are the architecture's definition, not what the code
 * happens to return. The two that matter are the width-32 mask, where the
 * obvious `(1u << 32) - 1` is undefined, and SBFX's sign, which comes from the
 * top bit of the FIELD and not of the register. */
static void test_bitfields(void) {
    assert(vita_bf_mask(0)  == 0x00000000u);
    assert(vita_bf_mask(8)  == 0x000000FFu);
    assert(vita_bf_mask(32) == 0xFFFFFFFFu);

    assert(vita_ubfx(0xDEADBEEFu, 4, 8) == 0xEEu);
    assert(vita_ubfx(0xDEADBEEFu, 0, 32) == 0xDEADBEEFu);   /* the UB case */
    assert(vita_ubfx(0x80000000u, 31, 1) == 1u);

    /* Field 0x8 with width 4: its top bit is set, so it sign-extends — even
     * though the register itself is positive. */
    assert(vita_sbfx(0x00000080u, 4, 4) == 0xFFFFFFF8u);
    assert(vita_sbfx(0x00000070u, 4, 4) == 0x00000007u);
    assert(vita_sbfx(0xDEADBEEFu, 0, 32) == 0xDEADBEEFu);

    /* BFI preserves everything outside the field, which is what makes it a
     * read-modify-write of the destination rather than an assignment. */
    assert(vita_bfi(0xFFFFFFFFu, 0x0u, 4, 8) == 0xFFFFF00Fu);   /* bfc */
    assert(vita_bfi(0x00000000u, 0xFFu, 4, 8) == 0x00000FF0u);
    /* Source bits above the field width are discarded, not spilled. */
    assert(vita_bfi(0x00000000u, 0xFFFFu, 4, 8) == 0x00000FF0u);
    assert(vita_bfi(0x12345678u, 0x0u, 0, 32) == 0x00000000u);

    printf("  bitfields                      ok\n");
}

/* --- dispatch ----------------------------------------------------------------
 *
 * The table now carries import stubs alongside recompiled functions, merged
 * from two sorted lists. Both properties the search depends on are checked
 * here: that the Thumb bit is masked, and that an unsorted table is refused
 * rather than searched — an unsorted one does not crash, it silently fails to
 * find entries that are present, which looks exactly like missing coverage.
 */

static int g_called;
static void mark_a(void) { g_called = 1; }
static void mark_b(void) { g_called = 2; }
static void mark_c(void) { g_called = 3; }

static void test_dispatch(void) {
    static const vita_dispatch_entry sorted[] = {
        { 0x81000100u, mark_a },
        { 0x81000200u, mark_b },   /* an import stub, as far as the table knows */
        { 0x81000300u, mark_c },
    };
    vita_dispatch_init(sorted, 3);

    g_called = 0; vita_dispatch(0, 0x81000100u); assert(g_called == 1);
    g_called = 0; vita_dispatch(0, 0x81000300u); assert(g_called == 3);

    /* Every pointer to Thumb code carries bit 0 as an instruction-set marker.
     * A lookup that does not mask it misses EVERY entry, by one. */
    g_called = 0; vita_dispatch(0, 0x81000201u); assert(g_called == 2);

    assert(vita_dispatch_hits() == 3);
    assert(vita_dispatch_misses() == 0);

    /* An unsorted table is refused outright: count goes to zero, so every
     * lookup misses loudly instead of some lookups missing quietly. */
    static const vita_dispatch_entry unsorted[] = {
        { 0x81000300u, mark_c },
        { 0x81000100u, mark_a },
    };
    vita_dispatch_init(unsorted, 2);
    g_called = 0;
    assert(vita_dispatch_hits() == 0);

    printf("  dispatch, thumb bit and order  ok\n");
}

/* --- HLE: SceLibc over guest memory ------------------------------------------
 *
 * These forward to the host C library, so the host's correctness is not what
 * is in question here. The boundary is: every pointer argument is a GUEST
 * address into a flat allocation, and the length of a C string is in the data
 * rather than in the arguments. Both are checked.
 */

#define GBASE 0x81000000u

/* Declared here rather than in hle.h. Every one of these is `void(void)` and
 * the generated code declares the whole set already; a second list in a
 * shipped header would only be something to keep in sync. */
void vita_hle_strlen(void);
void vita_hle_strcpy(void);
void vita_hle_strncpy(void);
void vita_hle_strchr(void);
void vita_hle_memcpy(void);
void vita_hle_memcmp(void);
void vita_hle_malloc(void);
void vita_hle__sceLibcErrnoLoc(void);
void vita_hle___cxa_guard_acquire(void);
void vita_hle___cxa_guard_release(void);

static void test_hle_libc(void) {
    assert(vita_mem_init(GBASE, 64 * 1024) == 0);
    vita_heap_init(GBASE + 32 * 1024, 16 * 1024);
    vita_mem_bad_access = 0;

    /* strlen measures inside guest memory. */
    const char *msg = "cardgame";
    for (uint32_t i = 0; i <= 8; i++) vita_write8(GBASE + 0x100 + i, (uint8_t)msg[i]);
    r0 = GBASE + 0x100; vita_hle_strlen();
    assert(r0 == 8);

    /* strcpy copies the terminator too, which is why it needs len + 1. */
    r0 = GBASE + 0x200; r1 = GBASE + 0x100; vita_hle_strcpy();
    r0 = GBASE + 0x200; vita_hle_strlen();
    assert(r0 == 8);
    assert(vita_read8(GBASE + 0x208) == 0);

    /* memcpy returns nothing we check, but must move the bytes. */
    r0 = GBASE + 0x300; r1 = GBASE + 0x100; r2 = 9; vita_hle_memcpy();
    r0 = GBASE + 0x300; r1 = GBASE + 0x100; r2 = 9; vita_hle_memcmp();
    assert(r0 == 0);

    /* strncpy pads the WHOLE remainder with NULs, not just one. */
    for (uint32_t i = 0; i < 16; i++) vita_write8(GBASE + 0x400 + i, 0xAA);
    r0 = GBASE + 0x400; r1 = GBASE + 0x100; r2 = 12; vita_hle_strncpy();
    assert(vita_read8(GBASE + 0x408) == 0);
    assert(vita_read8(GBASE + 0x40B) == 0);
    assert(vita_read8(GBASE + 0x40C) == 0xAA);   /* and no further */

    /* strchr returns a GUEST pointer, not a host one. */
    r0 = GBASE + 0x100; r1 = 'g'; vita_hle_strchr();
    assert(r0 == GBASE + 0x104);
    r0 = GBASE + 0x100; r1 = 'z'; vita_hle_strchr();
    assert(r0 == 0);

    /* The heap hands out guest addresses inside its region, 8-aligned. */
    uint32_t used0 = vita_heap_used();
    r0 = 10; vita_hle_malloc();
    uint32_t a = r0;
    assert(a >= GBASE + 32 * 1024 && a < GBASE + 48 * 1024);
    assert((a & 7u) == 0);
    r0 = 10; vita_hle_malloc();
    assert(r0 != a);                              /* distinct allocations */
    assert(vita_heap_used() > used0);
    r0 = 0; vita_hle_malloc();
    assert(r0 == 0);                              /* malloc(0) is NULL here */

    /* errno needs an address the guest can write through. */
    vita_hle__sceLibcErrnoLoc();
    assert(r0 != 0);
    vita_write32(r0, 22);
    vita_hle__sceLibcErrnoLoc();
    assert(vita_read32(r0) == 22);

    /* A static-init guard reports "run the constructor" exactly once. */
    uint32_t guard = GBASE + 0x500;
    vita_write8(guard, 0);
    r0 = guard; vita_hle___cxa_guard_acquire(); assert(r0 == 1);
    r0 = guard; vita_hle___cxa_guard_release();
    r0 = guard; vita_hle___cxa_guard_acquire(); assert(r0 == 0);

    /* None of the above should have touched anything unbacked. */
    assert(vita_mem_bad_access == 0);

    vita_mem_free();
    printf("  hle: SceLibc over guest memory ok\n");
}

/* --- memory ----------------------------------------------------------------- */

static void test_memory(void) {
    assert(vita_mem_init(0x81000000u, 64 * 1024) == 0);
    vita_mem_bad_access = 0;

    vita_write32(0x81000000u, 0xDEADBEEFu);
    assert(vita_read32(0x81000000u) == 0xDEADBEEFu);
    assert(vita_read8(0x81000000u) == 0xEFu);        /* little-endian */
    assert(vita_read16(0x81000002u) == 0xDEADu);

    /* Unaligned access is legal on ARMv7 and must not fault or be reordered. */
    vita_write32(0x81000005u, 0x11223344u);
    assert(vita_read32(0x81000005u) == 0x11223344u);
    assert(vita_mem_bad_access == 0);

    /* Out of range is COUNTED, not silently absorbed and not a wild write. */
    vita_read32(0x90000000u);
    assert(vita_mem_bad_access == 1);
    vita_write32(0x80000000u, 1u);                   /* below the base */
    assert(vita_mem_bad_access == 2);

    /* An access straddling the end of the region is refused rather than
     * reading past the allocation. */
    vita_read32(0x81000000u + 64 * 1024 - 2);
    assert(vita_mem_bad_access == 3);

    vita_mem_free();
    printf("  memory, unaligned and bounds   ok\n");
}

/* --- CLZ ----------------------------------------------------------------------- */

/* STREX succeeds only against the monitor its own LDREX opened, and only
 * while the word still holds what was loaded. */
static void test_exclusive(void) {
    assert(vita_mem_init(0x81000000u, 4096) == 0);
    const uint32_t a = 0x81000100u;
    vita_write32(a, 41);

    assert(vita_ldrex(a, 4) == 41);
    assert(vita_strex(a, 42, 4) == 0 && vita_read32(a) == 42);
    assert(vita_strex(a, 43, 4) == 1 && vita_read32(a) == 42);   /* monitor spent */

    (void)vita_ldrex(a, 4);
    vita_write32(a, 7);                                         /* another writer */
    assert(vita_strex(a, 99, 4) == 1 && vita_read32(a) == 7);

    (void)vita_ldrex(a, 4);
    vita_clrex();
    assert(vita_strex(a, 99, 4) == 1);

    (void)vita_ldrex(a, 4);
    assert(vita_strex(a + 4, 99, 4) == 1);                      /* other address */

    vita_write8(a + 8, 0xAB);
    assert(vita_ldrex(a + 8, 1) == 0xAB);
    assert(vita_strex(a + 8, 0x1CD, 1) == 0 && vita_read8(a + 8) == 0xCD && vita_read8(a + 9) == 0);

    vita_write32(a + 16, 1); vita_write32(a + 20, 2);
    assert(vita_ldrex(a + 16, 8) == 0x0000000200000001ull);
    assert(vita_strex(a + 16, 0x0000000400000003ull, 8) == 0);
    assert(vita_read32(a + 16) == 3 && vita_read32(a + 20) == 4);

    vita_barrier();
    vita_mem_free();
    printf("  exclusive monitor             ok\n");
}

static void test_clz(void) {
    /* Zero is 32 by ARM's definition; __builtin_clz(0) is undefined. */
    assert(vita_clz(0u) == 32u);
    assert(vita_clz(1u) == 31u);
    assert(vita_clz(0x80000000u) == 0u);
    assert(vita_clz(0x00010000u) == 15u);
    /* The `x == 0` idiom compilers build from it: clz, then lsr #5. */
    assert((vita_clz(0u) >> 5) == 1u && (vita_clz(7u) >> 5) == 0u);
    printf("  clz, including zero            ok\n");
}

/* --- GE flags and NEON lanes ---------------------------------------------------- */

static void test_parallel_and_lanes(void) {
    /* uadd8: carry-out per byte sets GE for that byte only. */
    assert(vita_uadd8(0x01FF80FFu, 0x01010180u) == 0x02008100u + 0x7Fu);
    assert(flag_ge == 0x5u);                 /* bytes 0 and 2 carried */
    /* sel picks rn's byte where GE is set */
    assert(vita_sel(0xAABBCCDDu, 0x11223344u) == 0x11BB33DDu);
    /* usub8: GE means no borrow */
    vita_usub8(0x10101010u, 0x20101000u);
    assert(flag_ge == 0x7u);                 /* byte 3 (0x10 - 0x20) borrowed */

    /* NEON lanes: no carry may cross a lane boundary. */
    assert(neon_add(0x00000000FFFFFFFFull, 0x0000000000000001ull, 2) == 0);
    assert(neon_add(0x00000000FFFFFFFFull, 0x0000000000000001ull, 3) == 0x0000000100000000ull);
    assert(neon_add(0xFF01ull, 0x0101ull, 0) == 0x0002ull);
    assert(neon_sub(0x0000000000000000ull, 0x0000000000000001ull, 2) == 0x00000000FFFFFFFFull);
    assert(neon_sub(0x0100ull, 0x0001ull, 0) == 0x01FFull);
    assert(neon_sub(0x0100ull, 0x0001ull, 1) == 0x00FFull);
    neon_setd(17, 0x0123456789ABCDEFull);
    assert(neon_getd(17) == 0x0123456789ABCDEFull);
    assert(vfp_s[34].u == 0x89ABCDEFu);      /* D17 low word: word 34, no S name */
    printf("  GE/sel and NEON lanes          ok\n");
}

/* --- the VFP register file ---------------------------------------------------- */

static void test_vfp_d32(void) {
    /* D16-D31 are real registers with no single-precision names. Masking the
     * double index by the singles' range folded them onto D0-D15, so writing
     * D16 silently clobbered D0. */
    vfp_setd(0, 1.5);
    vfp_setd(16, -2.25);
    assert(vfp_getd(0) == 1.5);
    assert(vfp_getd(16) == -2.25);
    vfp_setd(15, 4.0);
    vfp_setd(31, 3.0);
    assert(vfp_getd(31) == 3.0);
    assert(vfp_getd(15) == 4.0);                    /* D31 is not D15 */

    /* Dn still aliases S(2n), S(2n+1) in the low bank, low word first. */
    vfp_setu(2, 0x00000000u);
    vfp_setu(3, 0x3FF00000u);                       /* 1.0 as a double */
    assert(vfp_getd(1) == 1.0);
    printf("  vfp, D0-D31 distinct           ok\n");
}

/* --- attached memory and the import handler ----------------------------------- */

static void test_mem_attach(void) {
    /* Memory the runtime does not own: it must be addressable, and freeing the
     * runtime's view of it must leave the owner's buffer alone. */
    static uint8_t host[256];
    host[0x10] = 0x7F;
    assert(vita_mem_attach(host, 0x60000000u, sizeof(host)) == 0);
    assert(vita_read8(0x60000010u) == 0x7F);
    vita_write32(0x60000020u, 0xDEADBEEFu);
    assert(host[0x20] == 0xEF && host[0x23] == 0xDE);
    vita_mem_free();
    assert(host[0x10] == 0x7F);                     /* not freed, not zeroed */
    assert(vita_mem_attach(NULL, 0, 16) != 0);
    printf("  memory, attach without owning  ok\n");
}

static uint32_t g_seen_nid;
static int handle_even(uint32_t stub, uint32_t nid, const char *name) {
    (void)stub; (void)name;
    g_seen_nid = nid;
    if (nid & 1) return 0;                          /* decline odd NIDs */
    r0 = nid ^ 0xFFFFFFFFu;                         /* the call's result */
    return 1;
}

static void test_import_handler(void) {
    uint32_t before = vita_trap_count();
    vita_set_import_handler(handle_even);
    r0 = 0;
    vita_trap_import(0x81000000u, 0x12345678u, "Lib::fn");
    assert(g_seen_nid == 0x12345678u);
    assert(r0 == (0x12345678u ^ 0xFFFFFFFFu));
    /* Serviced calls are not gaps. */
    assert(vita_trap_count() == before);
    vita_set_import_handler(NULL);
    printf("  import handler services NIDs   ok\n");
}

int main(void) {
    printf("runtime:\n");
    test_shifts();
    test_flags_add();
    test_flags_sub();
    test_flags_adc();
    test_conditions();
    test_extends();
    test_bitfields();
    test_dispatch();
    test_hle_libc();
    test_memory();
    test_clz();
    test_exclusive();
    test_parallel_and_lanes();
    test_vfp_d32();
    test_mem_attach();
    test_import_handler();
    printf("all runtime tests passed\n");
    return 0;
}
