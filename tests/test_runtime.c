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
#include <string.h>

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

/* The carry-out of a flag-setting logical op's operand 2. */
static void test_shifter_carry(void) {
    flag_c = 0;
    assert(vita_lsl_c(0x80000001u, 1) == 2 && flag_c == 1);
    assert(vita_lsl_c(0x1u, 32) == 0 && flag_c == 1);       /* bit 0 out last */
    assert(vita_lsl_c(0x1u, 33) == 0 && flag_c == 0);
    flag_c = 1;
    assert(vita_lsl_c(0x5u, 0) == 5 && flag_c == 1);        /* untouched */
    assert(vita_lsr_c(0x2u, 1) == 1 && flag_c == 0);
    assert(vita_lsr_c(0x80000000u, 32) == 0 && flag_c == 1);
    assert(vita_asr_c(0x80000000u, 32) == 0xFFFFFFFFu && flag_c == 1);
    assert(vita_asr_c(0x80000000u, 40) == 0xFFFFFFFFu && flag_c == 1);
    assert(vita_asr_c(0x40000000u, 31) == 0 && flag_c == 1);
    assert(vita_ror_c(0x1u, 1) == 0x80000000u && flag_c == 1);
    flag_c = 0;
    assert(vita_ror_c(0x80000000u, 32) == 0x80000000u && flag_c == 1);

    /* RRX rotates through carry: carry in at the top, bit 0 out. */
    flag_c = 1;
    assert(vita_rrx(0x2u) == 0x80000001u && flag_c == 1);
    assert(vita_rrx_c(0x3u) == 0x80000001u && flag_c == 1);
    flag_c = 0;
    assert(vita_rrx_c(0x2u) == 0x1u && flag_c == 0);
    printf("  shifter carry-out, rrx: ok\n");
}

/* The lane helpers against the obvious per-lane loop, on values chosen to
 * hit every lane boundary: equal, differing only in a lane's top bit, and
 * differing only in its bottom bit. */
static uint64_t ref_ceq(uint64_t a, uint64_t b, int size) {
    int w = 8 << size;
    uint64_t lane = w == 64 ? ~0ull : (1ull << w) - 1, r = 0;
    for (int i = 0; i < 64 / w; i++)
        if (((a >> (i * w)) & lane) == ((b >> (i * w)) & lane)) r |= lane << (i * w);
    return r;
}

static void test_neon_lanes(void) {
    static const uint64_t v[] = {
        0, ~0ull, 0x0123456789ABCDEFull, 0x8000800080008000ull, 0x0100010001000100ull,
        0x8080808080808080ull, 0x0101010101010101ull, 0x00FF00FF00FF00FFull,
        0x7F7F7F7F7F7F7F7Full, 0xFFFFFFFF00000000ull,
    };
    int nv = (int)(sizeof(v) / sizeof(v[0]));
    for (int size = 0; size < 4; size++)
        for (int i = 0; i < nv; i++)
            for (int j = 0; j < nv; j++) {
                assert(neon_ceq(v[i], v[j], size) == ref_ceq(v[i], v[j], size));
                assert(neon_ceq(v[i], v[i] ^ (1ull << ((8 << size) - 1)), size) != ~0ull);   /* a top bit */
                assert(neon_ceq(v[i], v[i] ^ 1ull, size) != ~0ull);                          /* a bottom bit */
                assert(neon_tst(v[i], v[j], size) == ~ref_ceq(v[i] & v[j], 0, size));
            }

    /* memchr's reduction: a byte mask, weighted 1,2,4..128 by a vand, then
     * three vpadds fold 32 bytes into a 32-bit bitmap of matches. */
    assert(neon_padd(0x0807060504030201ull, 0x100F0E0D0C0B0A09ull, 0) == 0x1F1B17130F0B0703ull);
    assert(neon_padd(0x0004000300020001ull, 0x0008000700060005ull, 1) == 0x000F000B00070003ull);
    assert(neon_padd(0x0000000200000001ull, 0x0000000400000003ull, 2) == 0x0000000700000003ull);

    assert(neon_movn(0x0000000200000001ull, 0x0000000400000003ull, 1) == 0x0004000300020001ull);
    assert(neon_movn(0x00AA00BB00CC00DDull, 0x0011002200330044ull, 0) == 0x11223344AABBCCDDull);

    assert(neon_dup(0x1234ABCDu, 0) == 0xCDCDCDCDCDCDCDCDull);
    assert(neon_dup(0x1234ABCDu, 1) == 0xABCDABCDABCDABCDull);
    assert(neon_dup(0x1234ABCDu, 2) == 0x1234ABCD1234ABCDull);

    uint64_t d = 0x0011223344556677ull;
    assert(neon_set_lane(d, 0xEE, 0, 5) == 0x0011EE3344556677ull);
    assert(neon_set_lane(d, 0xBEEF, 1, 3) == 0xBEEF223344556677ull);
    assert(neon_set_lane(d, 0xCAFEF00Du, 2, 0) == 0x00112233CAFEF00Dull);
    assert(neon_get_lane(0x00000000000000F0ull, 0, 0, 0) == 0xFFFFFFF0u);
    assert(neon_get_lane(0x00000000000000F0ull, 0, 0, 1) == 0xF0u);
    assert(neon_get_lane(0x8001000000000000ull, 1, 3, 0) == 0xFFFF8001u);
    assert(neon_get_lane(0x8001000000000000ull, 1, 3, 1) == 0x8001u);
    printf("ok   neon lanes: ceq/tst on every lane size, padd, movn, dup, lane moves\n");
}

/* --- Advanced SIMD single precision ----------------------------------------- */

static uint32_t f32_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static float f32_from(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static uint64_t f32_pair(uint32_t hi, uint32_t lo) {
    return ((uint64_t)hi << 32) | lo;
}

/* The two-step reference: the product is rounded to float first, in its own
 * volatile so it cannot be fused with the add. */
static float ref_mla(float d, float n, float m, int sub) {
    volatile float p = n * m;
    return sub ? d - p : d + p;
}

static void test_neon_f32(void) {
    /* Ordinary values: the lanes are plain float arithmetic. */
    float a0 = 1.5f, a1 = -3.25f, b0 = 0.5f, b1 = 2.0f, c0 = 4.0f, c1 = 0.125f;
    uint64_t A = f32_pair(f32_bits(a1), f32_bits(a0));
    uint64_t B = f32_pair(f32_bits(b1), f32_bits(b0));
    uint64_t C = f32_pair(f32_bits(c1), f32_bits(c0));
    assert(neon_fadd(A, B) == f32_pair(f32_bits(a1 + b1), f32_bits(a0 + b0)));
    assert(neon_fsub(A, B) == f32_pair(f32_bits(a1 - b1), f32_bits(a0 - b0)));
    assert(neon_fmul(A, B) == f32_pair(f32_bits(a1 * b1), f32_bits(a0 * b0)));
    assert(neon_fmla(A, B, C) == f32_pair(f32_bits(ref_mla(a1, b1, c1, 0)), f32_bits(ref_mla(a0, b0, c0, 0))));
    assert(neon_fmls(A, B, C) == f32_pair(f32_bits(ref_mla(a1, b1, c1, 1)), f32_bits(ref_mla(a0, b0, c0, 1))));

    /* By scalar: the one lane, in every lane of the product. */
    assert(neon_fmul_s(f32_pair(f32_bits(3.0f), f32_bits(2.0f)), f32_bits(4.0f))
           == f32_pair(f32_bits(12.0f), f32_bits(8.0f)));
    assert(neon_fmla_s(f32_pair(f32_bits(1.0f), f32_bits(1.0f)), f32_pair(f32_bits(3.0f), f32_bits(2.0f)), f32_bits(0.5f))
           == f32_pair(f32_bits(2.5f), f32_bits(2.0f)));

    /* Flush-to-zero on the input: 0x00400000 is 2^-127, a denormal. Read as
     * zero, 2^-127 * 2^126 is +0; read as itself it would be 0.5. */
    assert(neon_fmul(f32_pair(0x80400000u, 0x00400000u), f32_pair(0x7E800000u, 0x7E800000u))
           == f32_pair(0x80000000u, 0x00000000u));
    /* A denormal input plus zero: -denormal is a -0, and -0 + +0 is +0. */
    assert(neon_fadd(f32_pair(0x00000001u, 0x80000001u), f32_pair(0x80000000u, 0x00000000u))
           == f32_pair(0x00000000u, 0x00000000u));

    /* Flush-to-zero on the result: 2^-126 * 2^-23 = 2^-149, the smallest
     * denormal. It becomes +0, and -2^-149 becomes -0. */
    assert(neon_fmul(f32_pair(0x80800000u, 0x00800000u), f32_pair(0x34000000u, 0x34000000u))
           == f32_pair(0x80000000u, 0x00000000u));

    /* NaN: the default NaN whatever the operands, for every operation. */
    uint64_t nan_in = f32_pair(0xFFC00000u, 0x7F800001u);   /* qNaN negative, sNaN */
    uint64_t one = f32_pair(f32_bits(1.0f), f32_bits(1.0f));
    assert(neon_fadd(nan_in, one) == f32_pair(0x7FC00000u, 0x7FC00000u));
    assert(neon_fmul(nan_in, one) == f32_pair(0x7FC00000u, 0x7FC00000u));
    uint64_t inf = f32_pair(0x7F800000u, 0x7F800000u);
    assert(neon_fsub(inf, inf) == f32_pair(0x7FC00000u, 0x7FC00000u));          /* inf - inf */
    assert(neon_fmul(inf, f32_pair(0, 0)) == f32_pair(0x7FC00000u, 0x7FC00000u)); /* inf * 0 */
    assert(neon_fmla(nan_in, one, one) == f32_pair(0x7FC00000u, 0x7FC00000u));  /* NaN accumulator */

    /* Fused vs unfused. n = m = 1 + 2^-23, d = -(1 + 2^-22).
     *   exact n*m + d = 2^-46             (what a fused multiply-add gives)
     *   round(n*m) = 1 + 2^-22, + d = 0   (what VMLA gives: the product first)
     * The exact result is computed in double, where it is exact, so the fused
     * reference is not itself a rounded guess. */
    uint32_t n = 0x3F800001u, d = 0xBF800002u;
    double exact = (double)f32_from(n) * (double)f32_from(n) + (double)f32_from(d);
    assert(f32_bits((float)exact) == 0x28800000u);              /* the fused answer */
    assert(neon_f32_mla(d, n, n, 0) == 0x00000000u);            /* VMLA: two roundings */
    assert(neon_f32_mla(d, n, n, 0) != f32_bits((float)exact)); /* a fused one would fail */
    assert(neon_fmla(f32_pair(d, d), f32_pair(n, n), f32_pair(n, n)) == f32_pair(0, 0));
    /* VMLS of the same numbers: d' = 1 + 2^-22, so the answer is 0 unfused and
     * -2^-46 fused. */
    assert(neon_f32_mla(0x3F800002u, n, n, 1) == 0x00000000u);

    printf("ok   neon f32: add/sub/mul/mla/mls, scalar forms, denormals, default NaN, unfused mla\n");
}

/* The second batch: max/min, compares, abs/neg, the conversions, and the VMLS
 * by scalar. Each rule from the architecture gets its own explicit bit pattern. */
static void test_neon_f32_b(void) {
    /* VABS and VNEG touch the sign bit and nothing else: a denormal stays a
     * denormal, a NaN keeps its payload, and -0 and +0 swap with the sign. */
    assert(neon_fabs(f32_pair(0x80000001u, 0xFFA00001u)) == f32_pair(0x00000001u, 0x7FA00001u));
    assert(neon_fabs(f32_pair(0xFF800000u, 0x80000000u)) == f32_pair(0x7F800000u, 0x00000000u));
    assert(neon_fneg(f32_pair(0x00000001u, 0x7FA00001u)) == f32_pair(0x80000001u, 0xFFA00001u));
    assert(neon_fneg(f32_pair(0x00000000u, 0x7F800000u)) == f32_pair(0x80000000u, 0xFF800000u));
    assert(neon_fneg(f32_pair(0x80000000u, 0x7FC00000u)) == f32_pair(0x00000000u, 0xFFC00000u));

    /* VMAX and VMIN on lanes that are ordinary values. */
    uint64_t X = f32_pair(f32_bits(2.0f), f32_bits(1.5f));
    uint64_t Y = f32_pair(f32_bits(-3.25f), f32_bits(-7.0f));
    assert(neon_fmax(X, Y) == f32_pair(f32_bits(2.0f), f32_bits(1.5f)));
    assert(neon_fmin(X, Y) == f32_pair(f32_bits(-3.25f), f32_bits(-7.0f)));

    /* Signed zeros: max(+0, -0) is +0 and min(+0, -0) is -0, in either order. */
    assert(neon_f32_minmax(0x80000000u, 0x00000000u, 1) == 0x00000000u);
    assert(neon_f32_minmax(0x00000000u, 0x80000000u, 1) == 0x00000000u);
    assert(neon_f32_minmax(0x80000000u, 0x00000000u, 0) == 0x80000000u);
    assert(neon_f32_minmax(0x00000000u, 0x80000000u, 0) == 0x80000000u);

    /* A denormal input is a zero of its sign, so it takes part in the signed
     * zero rule: max(-denormal, +0) is +0, min(+denormal, -0) is -0. */
    assert(neon_f32_minmax(0x80000001u, 0x00000000u, 1) == 0x00000000u);
    assert(neon_f32_minmax(0x00000001u, 0x80000000u, 0) == 0x80000000u);
    assert(neon_f32_minmax(0x00000001u, 0x00000002u, 0) == 0x00000000u);

    /* A NaN on either side gives the default NaN, not the operand's sign or
     * payload. */
    assert(neon_f32_minmax(0x7FA00001u, 0x3F800000u, 1) == 0x7FC00000u);
    assert(neon_f32_minmax(0x3F800000u, 0xFFC00000u, 0) == 0x7FC00000u);
    assert(neon_fmax(f32_pair(0x7FA00001u, 0x3F800000u), f32_pair(0x3F800000u, 0x3F800000u))
           == f32_pair(0x7FC00000u, 0x3F800000u));

    /* Infinities order as numbers. */
    assert(neon_f32_minmax(0xFF800000u, f32_bits(5.0f), 1) == f32_bits(5.0f));
    assert(neon_f32_minmax(0x7F800000u, f32_bits(5.0f), 0) == f32_bits(5.0f));
    assert(neon_f32_minmax(0xFF800000u, 0x7F800000u, 1) == 0x7F800000u);

    /* VCEQ, VCGE, VCGT: all-ones or zero per lane. +0 == -0 is true. */
    assert(neon_fceq(f32_pair(0x00000000u, 0x80000000u), f32_pair(0x80000000u, 0x00000000u))
           == f32_pair(0xFFFFFFFFu, 0xFFFFFFFFu));
    assert(neon_f32_cmp(0x80000000u, 0x00000000u, 1) == 0xFFFFFFFFu);
    assert(neon_f32_cmp(0x80000000u, 0x00000000u, 2) == 0);

    /* Denormal inputs flush first, so a denormal compares equal to a zero. */
    assert(neon_f32_cmp(0x00000001u, 0x00000000u, 0) == 0xFFFFFFFFu);
    assert(neon_f32_cmp(0x80000001u, 0x00000000u, 0) == 0xFFFFFFFFu);
    assert(neon_f32_cmp(0x00000001u, 0x00000000u, 2) == 0);
    assert(neon_f32_cmp(0x80000001u, 0x00000001u, 1) == 0xFFFFFFFFu);

    /* A NaN on either side makes every compare false, including NaN == NaN. */
    assert(neon_f32_cmp(0x7FC00000u, 0x7FC00000u, 0) == 0);
    assert(neon_f32_cmp(0x7FA00001u, 0x3F800000u, 0) == 0);
    assert(neon_f32_cmp(0x7FA00001u, 0x3F800000u, 1) == 0);
    assert(neon_f32_cmp(0x3F800000u, 0xFFC00000u, 2) == 0);
    assert(neon_f32_cmp(0xFFC00000u, 0xFF800000u, 1) == 0);

    /* Infinities and ordinary negatives compare as numbers. */
    assert(neon_f32_cmp(0x7F800000u, 0x7F7FFFFFu, 2) == 0xFFFFFFFFu);
    assert(neon_f32_cmp(0xFF800000u, 0xFF800000u, 0) == 0xFFFFFFFFu);
    assert(neon_f32_cmp(0xBF800000u, 0xC0000000u, 1) == 0xFFFFFFFFu);
    assert(neon_f32_cmp(0xC0000000u, 0xBF800000u, 2) == 0);
    assert(neon_fcgt(f32_pair(f32_bits(1.0f), f32_bits(0.0f)), f32_pair(f32_bits(0.0f), f32_bits(1.0f)))
           == f32_pair(0xFFFFFFFFu, 0x00000000u));

    /* VCVT F32 to S32, toward zero, saturating, and never undefined. */
    assert(neon_f32_to_int(f32_bits(1.9f), 0) == 1);
    assert(neon_f32_to_int(f32_bits(-1.9f), 0) == 0xFFFFFFFFu);
    assert(neon_f32_to_int(f32_bits(0.99f), 0) == 0);
    assert(neon_f32_to_int(f32_bits(-0.99f), 0) == 0);
    assert(neon_f32_to_int(0x80000000u, 0) == 0);                /* -0 */
    assert(neon_f32_to_int(0x00000001u, 0) == 0);                /* denormal */
    assert(neon_f32_to_int(0x80000001u, 0) == 0);                /* -denormal */
    assert(neon_f32_to_int(0x7FC00000u, 0) == 0);                /* NaN */
    assert(neon_f32_to_int(0x7FA00001u, 0) == 0);                /* sNaN payload */
    assert(neon_f32_to_int(0xFFC00000u, 0) == 0);                /* -NaN */
    assert(neon_f32_to_int(0x4EFFFFFFu, 0) == 0x7FFFFF80u);      /* 2147483520 exact */
    assert(neon_f32_to_int(0x4F000000u, 0) == 0x7FFFFFFFu);      /* 2147483648 saturates */
    assert(neon_f32_to_int(0x4F000001u, 0) == 0x7FFFFFFFu);      /* 2147483904 saturates */
    assert(neon_f32_to_int(0x4F800000u, 0) == 0x7FFFFFFFu);      /* 4294967296 saturates */
    assert(neon_f32_to_int(0x7F800000u, 0) == 0x7FFFFFFFu);      /* +inf */
    assert(neon_f32_to_int(0xCF000000u, 0) == 0x80000000u);      /* -2147483648 in range */
    assert(neon_f32_to_int(0xCF000001u, 0) == 0x80000000u);      /* -2147483904 saturates */
    assert(neon_f32_to_int(0xFF800000u, 0) == 0x80000000u);      /* -inf */

    /* VCVT F32 to U32: negatives and NaN give 0, large values saturate. */
    assert(neon_f32_to_int(f32_bits(3.9f), 1) == 3);
    assert(neon_f32_to_int(f32_bits(0.99f), 1) == 0);
    assert(neon_f32_to_int(f32_bits(-0.99f), 1) == 0);
    assert(neon_f32_to_int(0xBF800000u, 1) == 0);                /* -1.0 */
    assert(neon_f32_to_int(0x80000000u, 1) == 0);                /* -0 */
    assert(neon_f32_to_int(0x80000001u, 1) == 0);                /* -denormal */
    assert(neon_f32_to_int(0xFF800000u, 1) == 0);                /* -inf */
    assert(neon_f32_to_int(0x7FC00000u, 1) == 0);                /* NaN */
    assert(neon_f32_to_int(0x4F000000u, 1) == 0x80000000u);      /* 2147483648 */
    assert(neon_f32_to_int(0x4F7FFFFFu, 1) == 0xFFFFFF00u);      /* largest below 2^32 */
    assert(neon_f32_to_int(0x4F800000u, 1) == 0xFFFFFFFFu);      /* 4294967296 saturates */
    assert(neon_f32_to_int(0x7F800000u, 1) == 0xFFFFFFFFu);      /* +inf */
    assert(neon_f2i(f32_pair(f32_bits(-1.0f), f32_bits(2.5f)), 0) == f32_pair(0xFFFFFFFFu, 2u));

    /* VCVT S32 and U32 to F32: round to nearest, ties to even, never denormal. */
    assert(neon_i32_to_f32(0x7FFFFFFFu, 0) == 0x4F000000u);      /* 2147483647 rounds up to 2^31 */
    assert(neon_i32_to_f32(16777217u, 0) == 0x4B800000u);       /* tie: even is 16777216 */
    assert(neon_i32_to_f32(16777219u, 0) == 0x4B800002u);       /* tie: even is 16777220 */
    assert(neon_i32_to_f32(0xFFFFFFFFu, 0) == 0xBF800000u);     /* -1 */
    assert(neon_i32_to_f32(0x80000000u, 0) == 0xCF000000u);     /* -2^31 */
    assert(neon_i32_to_f32(0x80000000u, 1) == 0x4F000000u);     /* 2^31 unsigned */
    assert(neon_i32_to_f32(0xFFFFFFFFu, 1) == 0x4F800000u);     /* 4294967295 rounds to 2^32 */
    assert(neon_i32_to_f32(0u, 0) == 0x00000000u);
    assert(neon_i2f(f32_pair(0xFFFFFFFFu, 3u), 0) == f32_pair(0xBF800000u, 0x40400000u));

    /* VMLS by scalar: d - n * s per lane, with the product rounded first. */
    assert(neon_fmls_s(f32_pair(f32_bits(1.0f), f32_bits(1.0f)), f32_pair(f32_bits(2.0f), f32_bits(3.0f)), f32_bits(0.5f))
           == f32_pair(0x00000000u, 0xBF000000u));

    printf("ok   neon f32 b: max/min signed zeros and NaN, compares, abs/neg, vcvt saturation, vmls scalar\n");
}

/* A lane value repeated across the whole D word, for lanes of 8 << size bits. */
static uint64_t rep_lane(uint64_t v, int size) {
    int w = 8 << size;
    uint64_t r = 0;
    for (int i = 0; i < 64 / w; i++)
        r |= (v & neon_lmask(size)) << (i * w);
    return r;
}

static void test_neon_int_c(void) {
    /* VMUL wraps to the lane. 0xFF*0xFF = 0xFE01 keeps 0x01 in 8-bit lanes,
     * and 0x8000*2 = 0x10000 keeps 0 in 16-bit lanes. */
    assert(neon_mul(~0ull, ~0ull, 0) == 0x0101010101010101ull);
    assert(neon_mul(0x8000800080008000ull, 0x0002000200020002ull, 1) == 0);
    assert(neon_mul(0xFFFFull, 0xFFFFull, 1) == 0x0001ull);                  /* (-1)*(-1) */
    assert(neon_mul(0x80000000FFFFFFFFull, 0x0000000200000002ull, 2) == 0x00000000FFFFFFFEull);
    assert(neon_mul(~0ull, ~0ull, 2) == 0x0000000100000001ull);              /* (2^32-1)^2 low word */
    assert(neon_mul(0x8000000180000001ull, 0x0000000200000002ull, 2) == 0x0000000200000002ull);

    /* VMLA and VMLS add and subtract the wrapped product, wrapping too. */
    assert(neon_mla(~0ull, ~0ull, 0x0202020202020202ull, 0) == 0xFDFDFDFDFDFDFDFDull);  /* 0xFF + 0xFE */
    assert(neon_mls(0, 0x0303030303030303ull, 0x0505050505050505ull, 0) == 0xF1F1F1F1F1F1F1F1ull);
    assert(neon_mla(0, 0x8001800180018001ull, 0x0003000300030003ull, 1) == 0x8003800380038003ull);
    assert(neon_mls(0x0000000700000005ull, 0x0000000200000003ull, 0x0000000000000002ull, 2)
           == 0x00000007FFFFFFFFull);                                /* 5 - 3*2 = -1; 7 - 2*0 = 7 */

    /* VQADD and VQSUB, signed at both ends for every size up to 32 bits. */
    assert(neon_qop(rep_lane(0x7F, 0), rep_lane(0x01, 0), 0, 0, 0) == rep_lane(0x7F, 0));
    assert(neon_qop(rep_lane(0x80, 0), rep_lane(0xFF, 0), 0, 0, 0) == rep_lane(0x80, 0));
    assert(neon_qop(rep_lane(0x80, 0), rep_lane(0x01, 0), 0, 0, 1) == rep_lane(0x80, 0));
    assert(neon_qop(rep_lane(0x7F, 0), rep_lane(0xFF, 0), 0, 0, 1) == rep_lane(0x7F, 0));
    assert(neon_qop(rep_lane(0x7F, 0), rep_lane(0x7E, 0), 0, 0, 0) == rep_lane(0x7F, 0));  /* 127+126 clamps */
    assert(neon_qop(rep_lane(0x7FFF, 1), rep_lane(0x0001, 1), 1, 0, 0) == rep_lane(0x7FFF, 1));
    assert(neon_qop(rep_lane(0x8000, 1), rep_lane(0x8000, 1), 1, 0, 0) == rep_lane(0x8000, 1));
    assert(neon_qop(rep_lane(0x8000, 1), rep_lane(0x0001, 1), 1, 0, 1) == rep_lane(0x8000, 1));
    assert(neon_qop(rep_lane(0x7FFF, 1), rep_lane(0xFFFF, 1), 1, 0, 1) == rep_lane(0x7FFF, 1));
    assert(neon_qop(rep_lane(0x7FFFFFFF, 2), rep_lane(1, 2), 2, 0, 0) == rep_lane(0x7FFFFFFF, 2));
    assert(neon_qop(rep_lane(0x80000000u, 2), rep_lane(0x80000000u, 2), 2, 0, 0) == rep_lane(0x80000000u, 2));
    assert(neon_qop(rep_lane(0x80000000u, 2), rep_lane(1, 2), 2, 0, 1) == rep_lane(0x80000000u, 2));
    assert(neon_qop(rep_lane(0x7FFFFFFF, 2), rep_lane(0xFFFFFFFF, 2), 2, 0, 1) == rep_lane(0x7FFFFFFF, 2));

    /* 64-bit signed: INT64_MAX+1 and INT64_MIN-1 clamp, with no overflow in C. */
    assert(neon_qop(0x7FFFFFFFFFFFFFFFull, 1, 3, 0, 0) == 0x7FFFFFFFFFFFFFFFull);
    assert(neon_qop(0x8000000000000000ull, ~0ull, 3, 0, 0) == 0x8000000000000000ull);  /* INT64_MIN + -1 */
    assert(neon_qop(0x8000000000000000ull, 1, 3, 0, 1) == 0x8000000000000000ull);      /* INT64_MIN - 1 */
    assert(neon_qop(0x7FFFFFFFFFFFFFFFull, ~0ull, 3, 0, 1) == 0x7FFFFFFFFFFFFFFFull);  /* INT64_MAX - -1 */
    assert(neon_qop(0x8000000000000000ull, 0x8000000000000000ull, 3, 0, 1) == 0);   /* MIN - MIN: no clamp */
    assert(neon_qop(0x8000000000000000ull, 0x8000000000000000ull, 3, 0, 0) == 0x8000000000000000ull);
    assert(neon_qop(0x7FFFFFFFFFFFFFFFull, 0x7FFFFFFFFFFFFFFFull, 3, 0, 0) == 0x7FFFFFFFFFFFFFFFull);
    assert(neon_qop(0x0000000000000003ull, 0xFFFFFFFFFFFFFFFFull, 3, 0, 0) == 0x0000000000000002ull); /* 3 + -1 */
    assert(neon_qop(0x7FFFFFFFFFFFFFFEull, 0x0000000000000001ull, 3, 0, 0) == 0x7FFFFFFFFFFFFFFFull); /* no overflow */
    assert(neon_qop(0x0000000000000001ull, 0x0000000000000003ull, 3, 0, 1) == 0xFFFFFFFFFFFFFFFEull); /* 1 - 3 */

    /* Unsigned, 0 and the top value at every size. */
    assert(neon_qop(rep_lane(0xFF, 0), rep_lane(0x01, 0), 0, 1, 0) == rep_lane(0xFF, 0));
    assert(neon_qop(rep_lane(0x00, 0), rep_lane(0x01, 0), 0, 1, 1) == 0);          /* 0 - 1 clamps at 0 */
    assert(neon_qop(rep_lane(0x80, 0), rep_lane(0x7F, 0), 0, 1, 1) == rep_lane(0x01, 0));
    assert(neon_qop(rep_lane(0xFFFF, 1), rep_lane(0x0001, 1), 1, 1, 0) == rep_lane(0xFFFF, 1));
    assert(neon_qop(rep_lane(0x0000, 1), rep_lane(0x0001, 1), 1, 1, 1) == 0);
    assert(neon_qop(rep_lane(0xFFFFFFFFu, 2), rep_lane(1, 2), 2, 1, 0) == rep_lane(0xFFFFFFFFu, 2));
    assert(neon_qop(rep_lane(0, 2), rep_lane(1, 2), 2, 1, 1) == 0);
    assert(neon_qop(~0ull, 1, 3, 1, 0) == ~0ull);                                  /* UINT64_MAX + 1 */
    assert(neon_qop(0, 1, 3, 1, 1) == 0);                                          /* 0 - 1 */
    assert(neon_qop(1, 2, 3, 1, 1) == 0);
    assert(neon_qop(5, 3, 3, 1, 1) == 2);                                          /* not saturating */
    assert(neon_qop(0x0102030405060708ull, 0x0101010101010101ull, 0, 1, 1) == 0x0001020304050607ull);

    /* VSHR by the full lane width: signed gives the sign, unsigned gives 0. */
    assert(neon_shr(rep_lane(0x80, 0), 8, 0, 0) == rep_lane(0xFF, 0));
    assert(neon_shr(rep_lane(0x80, 0), 8, 0, 1) == 0);
    assert(neon_shr(rep_lane(0x7F, 0), 8, 0, 0) == 0);
    assert(neon_shr(rep_lane(0x8000, 1), 16, 1, 0) == rep_lane(0xFFFF, 1));
    assert(neon_shr(rep_lane(0x8000, 1), 16, 1, 1) == 0);
    assert(neon_shr(rep_lane(0x7FFF, 1), 16, 1, 0) == 0);
    assert(neon_shr(rep_lane(0x80000000u, 2), 32, 2, 0) == rep_lane(0xFFFFFFFFu, 2));
    assert(neon_shr(rep_lane(0x80000000u, 2), 32, 2, 1) == 0);
    assert(neon_shr(rep_lane(0x7FFFFFFFu, 2), 32, 2, 0) == 0);
    assert(neon_shr(0x8000000000000000ull, 64, 3, 0) == ~0ull);
    assert(neon_shr(0x8000000000000000ull, 64, 3, 1) == 0);
    assert(neon_shr(0x7FFFFFFFFFFFFFFFull, 64, 3, 0) == 0);
    assert(neon_shr(~0ull, 64, 3, 0) == ~0ull);
    assert(neon_shr(~0ull, 64, 3, 1) == 0);

    /* One-bit right shifts of negative and positive lanes, every size. */
    assert(neon_shr(rep_lane(0x80, 0), 1, 0, 0) == rep_lane(0xC0, 0));              /* -128 >> 1 = -64 */
    assert(neon_shr(rep_lane(0xFF, 0), 1, 0, 0) == rep_lane(0xFF, 0));              /* -1 >> 1 = -1 */
    assert(neon_shr(rep_lane(0x7F, 0), 1, 0, 0) == rep_lane(0x3F, 0));
    assert(neon_shr(rep_lane(0xFF, 0), 1, 0, 1) == rep_lane(0x7F, 0));
    assert(neon_shr(rep_lane(0x8000, 1), 1, 1, 0) == rep_lane(0xC000, 1));
    assert(neon_shr(rep_lane(0xFFFE, 1), 1, 1, 0) == rep_lane(0xFFFF, 1));           /* -2 >> 1 = -1 */
    assert(neon_shr(rep_lane(0x7FFF, 1), 1, 1, 0) == rep_lane(0x3FFF, 1));
    assert(neon_shr(rep_lane(0x8000, 1), 1, 1, 1) == rep_lane(0x4000, 1));
    assert(neon_shr(rep_lane(0x80000000u, 2), 1, 2, 0) == rep_lane(0xC0000000u, 2));
    assert(neon_shr(rep_lane(0xFFFFFFFEu, 2), 1, 2, 0) == rep_lane(0xFFFFFFFFu, 2));
    assert(neon_shr(rep_lane(0x7FFFFFFFu, 2), 1, 2, 0) == rep_lane(0x3FFFFFFFu, 2));
    assert(neon_shr(rep_lane(0x80000000u, 2), 1, 2, 1) == rep_lane(0x40000000u, 2));
    assert(neon_shr(0x8000000000000000ull, 1, 3, 0) == 0xC000000000000000ull);
    assert(neon_shr(0x8000000000000001ull, 1, 3, 0) == 0xC000000000000000ull);      /* floor, not toward zero */
    assert(neon_shr(0x8000000000000001ull, 63, 3, 0) == ~0ull);
    assert(neon_shr(0x7FFFFFFFFFFFFFFFull, 1, 3, 0) == 0x3FFFFFFFFFFFFFFFull);
    assert(neon_shr(0x7FFFFFFFFFFFFFFFull, 1, 3, 1) == 0x3FFFFFFFFFFFFFFFull);
    /* Lanes of mixed sign in one word: each lane shifts on its own sign. */
    assert(neon_shr(0x80017FFF0002FFFEull, 1, 1, 0) == 0xC0003FFF0001FFFFull);

    /* VSHL: amount esize-1 is the top bit, and bits leaving a lane are dropped. */
    assert(neon_shl(rep_lane(0x01, 0), 7, 0) == rep_lane(0x80, 0));
    assert(neon_shl(rep_lane(0x81, 0), 7, 0) == rep_lane(0x80, 0));                /* bit 8 is dropped */
    assert(neon_shl(rep_lane(0x0001, 1), 15, 1) == rep_lane(0x8000, 1));
    assert(neon_shl(rep_lane(0x00000001u, 2), 31, 2) == rep_lane(0x80000000u, 2));
    assert(neon_shl(0x0000000000000001ull, 63, 3) == 0x8000000000000000ull);
    assert(neon_shl(0x0000000000000003ull, 63, 3) == 0x8000000000000000ull);       /* bit 64 dropped */
    assert(neon_shl(0x0123456789ABCDEFull, 0, 3) == 0x0123456789ABCDEFull);
    assert(neon_shl(0x8001ull, 1, 0) == 0x0002ull);     /* lane 1's top bit leaves the lane, not carried */

    /* VSRA: d + (m >> amount), wrapping. */
    assert(neon_sra(rep_lane(0x10, 0), rep_lane(0x10, 0), 1, 0, 0) == rep_lane(0x18, 0));
    assert(neon_sra(rep_lane(0xFF, 0), rep_lane(0x80, 0), 8, 0, 0) == rep_lane(0xFE, 0)); /* +(-1) wraps */
    assert(neon_sra(rep_lane(0x80, 0), rep_lane(0x80, 0), 1, 0, 1) == rep_lane(0xC0, 0));
    assert(neon_sra(rep_lane(0xFF, 0), rep_lane(0xFE, 0), 1, 0, 0) == rep_lane(0xFE, 0)); /* -2>>1 = -1; 0xFF+0xFF wraps */
    assert(neon_sra(rep_lane(0xFFFF, 1), rep_lane(0xFFFF, 1), 16, 1, 0) == rep_lane(0xFFFE, 1));
    assert(neon_sra(0, 0x8000000000000000ull, 64, 3, 0) == ~0ull);
    assert(neon_sra(1, 0x8000000000000000ull, 64, 3, 0) == 0);                       /* 1 + -1 wraps to 0 */
    assert(neon_sra(0x8000000000000000ull, 0x8000000000000000ull, 64, 3, 1) == 0x8000000000000000ull); /* unsigned: +0 */

    printf("ok   neon int: wrapping mul/mla/mls, vqadd/vqsub clamp at both ends, shifts by immediate, vsra wrap\n");
}

/* Pack n lanes of w bits, lane 0 lowest, from v. */
static uint64_t pack_lanes(const uint64_t *v, int w, int n) {
    uint64_t r = 0;
    for (int i = 0; i < n; i++) {
        assert(w == 64 || (v[i] >> w) == 0);
        r |= v[i] << (i * w);
    }
    return r;
}

/* Check a Q result against its lanes, lane 0 first: 128 / w of them, w the
 * result lane width. */
static void check_q(const uint64_t *r, const uint64_t *lanes, int w) {
    int per = 64 / w;
    assert(r[0] == pack_lanes(lanes, w, per));
    assert(r[1] == pack_lanes(lanes + per, w, per));
}

/* The compares, max and min, and absolute difference, against values worked
 * out by hand. The same bits are read signed and unsigned: 0x80 is -128 and
 * 128, and each pair of results must differ, so a helper that ignores U fails
 * here. The mixed-lane words put a different value in every lane, so a lane
 * shifted or masked wrongly shows up. */
static void test_neon_cmp_c(void) {
    /* Signed and unsigned disagree on 0x80 against 0x7F, at each size. */
    assert(neon_cmp(rep_lane(0x80, 0), rep_lane(0x7F, 0), 0, 0, 0) == 0);          /* -128 > 127: no */
    assert(neon_cmp(rep_lane(0x80, 0), rep_lane(0x7F, 0), 0, 1, 0) == ~0ull);      /* 128 > 127: yes */
    assert(neon_minmax(rep_lane(0x80, 0), rep_lane(0x7F, 0), 0, 0, 0) == rep_lane(0x7F, 0));
    assert(neon_minmax(rep_lane(0x80, 0), rep_lane(0x7F, 0), 0, 1, 0) == rep_lane(0x80, 0));
    assert(neon_cmp(rep_lane(0x8000, 1), rep_lane(0x7FFF, 1), 1, 0, 0) == 0);
    assert(neon_cmp(rep_lane(0x8000, 1), rep_lane(0x7FFF, 1), 1, 1, 0) == ~0ull);
    assert(neon_minmax(rep_lane(0x8000, 1), rep_lane(0x7FFF, 1), 1, 0, 0) == rep_lane(0x7FFF, 1));
    assert(neon_minmax(rep_lane(0x8000, 1), rep_lane(0x7FFF, 1), 1, 1, 0) == rep_lane(0x8000, 1));
    assert(neon_cmp(rep_lane(0x80000000, 2), rep_lane(0x7FFFFFFF, 2), 2, 0, 0) == 0);
    assert(neon_cmp(rep_lane(0x80000000, 2), rep_lane(0x7FFFFFFF, 2), 2, 1, 0) == ~0ull);
    assert(neon_minmax(rep_lane(0x80000000, 2), rep_lane(0x7FFFFFFF, 2), 2, 0, 0) == rep_lane(0x7FFFFFFF, 2));
    assert(neon_minmax(rep_lane(0x80000000, 2), rep_lane(0x7FFFFFFF, 2), 2, 1, 0) == rep_lane(0x80000000, 2));
    /* -1 against 0: signed is less, unsigned is greater. */
    assert(neon_cmp(rep_lane(0xFFFF, 1), 0, 1, 0, 0) == 0);
    assert(neon_cmp(rep_lane(0xFFFF, 1), 0, 1, 1, 0) == ~0ull);

    /* Equal lanes: VCGT gives 0 and VCGE all ones, and the absolute difference
     * is 0, at every size and sign. */
    for (int s = 0; s < 3; s++)
        for (int u = 0; u < 2; u++) {
            uint64_t x = 0x80FF7F01C3A5D26Eull;
            assert(neon_cmp(x, x, s, u, 0) == 0);
            assert(neon_cmp(x, x, s, u, 1) == ~0ull);
            assert(neon_abd(x, x, s, u) == 0);
        }

    /* Absolute difference is exact, then truncated. |-128 - 127| = 255 and
     * |0 - 255| = 255 fill the lane. The same bits 1 and 0xFF differ signed and
     * unsigned: |1 - (-1)| = 2 and |1 - 255| = 254. */
    assert(neon_abd(rep_lane(0x80, 0), rep_lane(0x7F, 0), 0, 0) == ~0ull);
    assert(neon_abd(rep_lane(0x00, 0), rep_lane(0xFF, 0), 0, 1) == ~0ull);
    assert(neon_abd(rep_lane(0x80000000, 2), rep_lane(0x7FFFFFFF, 2), 2, 0) == ~0ull);
    assert(neon_abd(rep_lane(0x8000, 1), rep_lane(0x7FFF, 1), 1, 0) == ~0ull);
    assert(neon_abd(rep_lane(0x01, 0), rep_lane(0xFF, 0), 0, 0) == rep_lane(0x02, 0));
    assert(neon_abd(rep_lane(0x01, 0), rep_lane(0xFF, 0), 0, 1) == rep_lane(0xFE, 0));

    /* VABA wraps in the lane: 0xFF + 2 is 0x01, and 1 + 255 is 0. */
    assert(neon_add(rep_lane(0xFF, 0), neon_abd(rep_lane(3, 0), rep_lane(1, 0), 0, 1), 0) == rep_lane(0x01, 0));
    assert(neon_add(rep_lane(0x01, 0), neon_abd(rep_lane(0x80, 0), rep_lane(0x7F, 0), 0, 0), 0) == rep_lane(0x00, 0));
    assert(neon_add(rep_lane(0xFFFF, 1), neon_abd(rep_lane(3, 1), rep_lane(1, 1), 1, 1), 1) == rep_lane(0x0001, 1));
    assert(neon_add(rep_lane(0xFFFFFFFF, 2), neon_abd(rep_lane(3, 2), rep_lane(1, 2), 2, 1), 2) == rep_lane(1, 2));

    /* Mixed lanes, 8-bit. Lanes 0, 1 and 6 differ between the signed and the
     * unsigned results. */
    uint64_t n8[8] = { 0x80, 0x7F, 0x01, 0xFF, 0x10, 0x20, 0x90, 0x00 };
    uint64_t m8[8] = { 0x7F, 0x80, 0x02, 0xFE, 0x10, 0x1F, 0x00, 0x00 };
    uint64_t N8 = pack_lanes(n8, 8, 8), M8 = pack_lanes(m8, 8, 8);
    uint64_t e_gt_s[8]  = { 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00 };
    uint64_t e_gt_u[8]  = { 0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0xFF, 0x00 };
    uint64_t e_ge_s[8]  = { 0x00, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0xFF };
    uint64_t e_ge_u[8]  = { 0xFF, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint64_t e_max_s[8] = { 0x7F, 0x7F, 0x02, 0xFF, 0x10, 0x20, 0x00, 0x00 };
    uint64_t e_min_s[8] = { 0x80, 0x80, 0x01, 0xFE, 0x10, 0x1F, 0x90, 0x00 };
    uint64_t e_max_u[8] = { 0x80, 0x80, 0x02, 0xFF, 0x10, 0x20, 0x90, 0x00 };
    uint64_t e_min_u[8] = { 0x7F, 0x7F, 0x01, 0xFE, 0x10, 0x1F, 0x00, 0x00 };
    uint64_t e_abd_s[8] = { 0xFF, 0xFF, 0x01, 0x01, 0x00, 0x01, 0x70, 0x00 };
    uint64_t e_abd_u[8] = { 0x01, 0x01, 0x01, 0x01, 0x00, 0x01, 0x90, 0x00 };
    uint64_t d8[8]      = { 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0xFF };
    uint64_t e_aba_s[8] = { 0x0F, 0x1F, 0x31, 0x41, 0x50, 0x61, 0xE0, 0xFF };
    assert(neon_cmp(N8, M8, 0, 0, 0) == pack_lanes(e_gt_s, 8, 8));
    assert(neon_cmp(N8, M8, 0, 1, 0) == pack_lanes(e_gt_u, 8, 8));
    assert(neon_cmp(N8, M8, 0, 0, 1) == pack_lanes(e_ge_s, 8, 8));
    assert(neon_cmp(N8, M8, 0, 1, 1) == pack_lanes(e_ge_u, 8, 8));
    assert(neon_minmax(N8, M8, 0, 0, 0) == pack_lanes(e_max_s, 8, 8));
    assert(neon_minmax(N8, M8, 0, 0, 1) == pack_lanes(e_min_s, 8, 8));
    assert(neon_minmax(N8, M8, 0, 1, 0) == pack_lanes(e_max_u, 8, 8));
    assert(neon_minmax(N8, M8, 0, 1, 1) == pack_lanes(e_min_u, 8, 8));
    assert(neon_abd(N8, M8, 0, 0) == pack_lanes(e_abd_s, 8, 8));
    assert(neon_abd(N8, M8, 0, 1) == pack_lanes(e_abd_u, 8, 8));
    assert(neon_add(pack_lanes(d8, 8, 8), neon_abd(N8, M8, 0, 0), 0) == pack_lanes(e_aba_s, 8, 8));

    /* Mixed lanes, 16-bit: the sign bit is set in two lanes of each word. */
    uint64_t n16[4] = { 0x8000, 0x7FFF, 0x0001, 0xFFFF };
    uint64_t m16[4] = { 0x7FFF, 0x8000, 0x0002, 0xFFFE };
    uint64_t N16 = pack_lanes(n16, 16, 4), M16 = pack_lanes(m16, 16, 4);
    uint64_t c16_gt_s[4] = { 0x0000, 0xFFFF, 0x0000, 0xFFFF };
    uint64_t c16_gt_u[4] = { 0xFFFF, 0x0000, 0x0000, 0xFFFF };
    uint64_t x16_max_s[4] = { 0x7FFF, 0x7FFF, 0x0002, 0xFFFF };
    uint64_t x16_min_s[4] = { 0x8000, 0x8000, 0x0001, 0xFFFE };
    uint64_t x16_max_u[4] = { 0x8000, 0x8000, 0x0002, 0xFFFF };
    uint64_t x16_min_u[4] = { 0x7FFF, 0x7FFF, 0x0001, 0xFFFE };
    uint64_t a16_s[4] = { 0xFFFF, 0xFFFF, 0x0001, 0x0001 };
    uint64_t a16_u[4] = { 0x0001, 0x0001, 0x0001, 0x0001 };
    assert(neon_cmp(N16, M16, 1, 0, 0) == pack_lanes(c16_gt_s, 16, 4));
    assert(neon_cmp(N16, M16, 1, 1, 0) == pack_lanes(c16_gt_u, 16, 4));
    assert(neon_minmax(N16, M16, 1, 0, 0) == pack_lanes(x16_max_s, 16, 4));
    assert(neon_minmax(N16, M16, 1, 0, 1) == pack_lanes(x16_min_s, 16, 4));
    assert(neon_minmax(N16, M16, 1, 1, 0) == pack_lanes(x16_max_u, 16, 4));
    assert(neon_minmax(N16, M16, 1, 1, 1) == pack_lanes(x16_min_u, 16, 4));
    assert(neon_abd(N16, M16, 1, 0) == pack_lanes(a16_s, 16, 4));
    assert(neon_abd(N16, M16, 1, 1) == pack_lanes(a16_u, 16, 4));

    /* Mixed lanes, 32-bit: -2^31 and 5 against 2^31 - 1 and -5. */
    uint64_t n32[2] = { 0x80000000, 0x00000005 };
    uint64_t m32[2] = { 0x7FFFFFFF, 0xFFFFFFFB };
    uint64_t N32 = pack_lanes(n32, 32, 2), M32 = pack_lanes(m32, 32, 2);
    uint64_t c32_gt_s[2] = { 0x00000000, 0xFFFFFFFF };
    uint64_t c32_gt_u[2] = { 0xFFFFFFFF, 0x00000000 };
    uint64_t x32_max_s[2] = { 0x7FFFFFFF, 0x00000005 };
    uint64_t x32_min_s[2] = { 0x80000000, 0xFFFFFFFB };
    uint64_t x32_max_u[2] = { 0x80000000, 0xFFFFFFFB };
    uint64_t x32_min_u[2] = { 0x7FFFFFFF, 0x00000005 };
    uint64_t a32_s[2] = { 0xFFFFFFFF, 0x0000000A };
    uint64_t a32_u[2] = { 0x00000001, 0xFFFFFFF6 };
    assert(neon_cmp(N32, M32, 2, 0, 0) == pack_lanes(c32_gt_s, 32, 2));
    assert(neon_cmp(N32, M32, 2, 1, 0) == pack_lanes(c32_gt_u, 32, 2));
    assert(neon_minmax(N32, M32, 2, 0, 0) == pack_lanes(x32_max_s, 32, 2));
    assert(neon_minmax(N32, M32, 2, 0, 1) == pack_lanes(x32_min_s, 32, 2));
    assert(neon_minmax(N32, M32, 2, 1, 0) == pack_lanes(x32_max_u, 32, 2));
    assert(neon_minmax(N32, M32, 2, 1, 1) == pack_lanes(x32_min_u, 32, 2));
    assert(neon_abd(N32, M32, 2, 0) == pack_lanes(a32_s, 32, 2));
    assert(neon_abd(N32, M32, 2, 1) == pack_lanes(a32_u, 32, 2));

    printf("ok   neon cmp: vcgt/vcge, vmax/vmin, vabd/vaba at the lane edges, signed and unsigned\n");
}

/* The shifts by register, against values worked out by hand. The value is the
 * first argument and the amounts the second, as in vshl Dd, Dm, Dn. The mixed
 * words give each lane its own amount, so a lane read from the wrong place
 * shows up, and the signed and unsigned results differ where the top bit is
 * set. The rounding words set the bits a right shift drops. */
static void test_neon_shl_reg_c(void) {
    /* Operand order: the amount is the second argument. 1 << 3 and 3 << 1. */
    assert(neon_shl_reg(rep_lane(1, 0), rep_lane(3, 0), 0, 0, 0, 0) == rep_lane(8, 0));
    assert(neon_shl_reg(rep_lane(3, 0), rep_lane(1, 0), 0, 0, 0, 0) == rep_lane(6, 0));

    /* Mixed lanes, 8-bit, VSHL. Amounts +1, -1, +2, -2, 0, +3, -3, +1. Lane 3
     * (-128 or 128) and lane 6 (-16 or 240) differ signed and unsigned. */
    uint64_t v8[8] = { 0x01, 0x02, 0x10, 0x80, 0xFF, 0x7F, 0xF0, 0x03 };
    uint64_t s8[8] = { 0x01, 0xFF, 0x02, 0xFE, 0x00, 0x03, 0xFD, 0x01 };
    uint64_t V8 = pack_lanes(v8, 8, 8), S8 = pack_lanes(s8, 8, 8);
    uint64_t e_shl_s[8] = { 0x02, 0x01, 0x40, 0xE0, 0xFF, 0xF8, 0xFE, 0x06 };
    uint64_t e_shl_u[8] = { 0x02, 0x01, 0x40, 0x20, 0xFF, 0xF8, 0x1E, 0x06 };
    assert(neon_shl_reg(V8, S8, 0, 0, 0, 0) == pack_lanes(e_shl_s, 8, 8));
    assert(neon_shl_reg(V8, S8, 0, 1, 0, 0) == pack_lanes(e_shl_u, 8, 8));

    /* VRSHL on a second word, with the bits a right shift drops set, so the
     * rounding shows. Amounts -1, -3, -4, -1, -2, -3, -1, 0. 3 by 1 is 2, and
     * 0x81 (-127 or 129) by 1 is -63 or 65. 0xF5 (-11 or 245) by 3 is -1 or 31. */
    uint64_t vr[8] = { 0x03, 0x07, 0x0F, 0x81, 0x7F, 0xF5, 0x05, 0xFF };
    uint64_t sr[8] = { 0xFF, 0xFD, 0xFC, 0xFF, 0xFE, 0xFD, 0xFF, 0x00 };
    uint64_t VR = pack_lanes(vr, 8, 8), SR = pack_lanes(sr, 8, 8);
    uint64_t e_rs[8] = { 0x02, 0x01, 0x01, 0xC1, 0x20, 0xFF, 0x03, 0xFF };
    uint64_t e_ru[8] = { 0x02, 0x01, 0x01, 0x41, 0x20, 0x1F, 0x03, 0xFF };
    assert(neon_shl_reg(VR, SR, 0, 0, 1, 0) == pack_lanes(e_rs, 8, 8));
    assert(neon_shl_reg(VR, SR, 0, 1, 1, 0) == pack_lanes(e_ru, 8, 8));

    /* Only the low byte of the amount lane counts. 0x0301 is +1 and 0x01FF is -1
     * in a 16-bit lane, and 0x0080 (-128) and 0x007F (+127) are the extremes. */
    uint64_t v16[4] = { 0x0003, 0x0006, 0x8000, 0x0001 };
    uint64_t s16[4] = { 0x0301, 0x01FF, 0x0080, 0x007F };
    uint64_t V16 = pack_lanes(v16, 16, 4), S16 = pack_lanes(s16, 16, 4);
    uint64_t e16_s[4] = { 0x0006, 0x0003, 0xFFFF, 0x0000 };
    uint64_t e16_u[4] = { 0x0006, 0x0003, 0x0000, 0x0000 };
    assert(neon_shl_reg(V16, S16, 1, 0, 0, 0) == pack_lanes(e16_s, 16, 4));
    assert(neon_shl_reg(V16, S16, 1, 1, 0, 0) == pack_lanes(e16_u, 16, 4));
    assert(neon_shl_reg(rep_lane(0x0003, 1), rep_lane(0x0301, 1), 1, 0, 0, 0) == rep_lane(0x0006, 1));
    assert(neon_shl_reg(rep_lane(0x0010, 1), rep_lane(0x01FF, 1), 1, 0, 0, 0) == rep_lane(0x0008, 1));

    /* Mixed lanes, 32-bit. The amounts have bits above the low byte set. */
    uint64_t v32[2] = { 0x40000001, 0x80000010 };
    uint64_t s32[2] = { 0x12345602, 0xABCDEFFD };
    uint64_t V32 = pack_lanes(v32, 32, 2), S32 = pack_lanes(s32, 32, 2);
    uint64_t e32_s[2] = { 0x00000004, 0xF0000002 };
    uint64_t e32_u[2] = { 0x00000004, 0x10000002 };
    assert(neon_shl_reg(V32, S32, 2, 0, 0, 0) == pack_lanes(e32_s, 32, 2));
    assert(neon_shl_reg(V32, S32, 2, 1, 0, 0) == pack_lanes(e32_u, 32, 2));

    /* 64-bit lanes, one per D register. Left 1 truncates to 64 bits. Right by 3
     * is arithmetic for the signed lane and logical for the unsigned one. */
    assert(neon_shl_reg(0x4000000000000001ull, 0x01, 3, 0, 0, 0) == 0x8000000000000002ull);
    assert(neon_shl_reg(0x8000000000000010ull, 0xAABBCCDDEEFFFFFDull, 3, 0, 0, 0) == 0xF000000000000002ull);
    assert(neon_shl_reg(0x8000000000000010ull, 0xAABBCCDDEEFFFFFDull, 3, 1, 0, 0) == 0x1000000000000002ull);

    /* Amounts at and past the lane width, 8-bit, with -128 and 127. A left
     * shift by 8 or more is 0 and a right shift by 8 or more is the sign. */
    assert(neon_shl_reg(rep_lane(0x01, 0), rep_lane(0x08, 0), 0, 0, 0, 0) == 0);
    assert(neon_shl_reg(rep_lane(0x81, 0), rep_lane(0x08, 0), 0, 0, 0, 0) == 0);
    assert(neon_shl_reg(rep_lane(0xFF, 0), rep_lane(0x7F, 0), 0, 0, 0, 0) == 0);     /* -1 << 127 */
    assert(neon_shl_reg(rep_lane(0x01, 0), rep_lane(0x7F, 0), 0, 1, 0, 0) == 0);
    assert(neon_shl_reg(rep_lane(0x81, 0), rep_lane(0xF8, 0), 0, 0, 0, 0) == rep_lane(0xFF, 0));   /* -127 >> 8 */
    assert(neon_shl_reg(rep_lane(0x81, 0), rep_lane(0xF8, 0), 0, 1, 0, 0) == 0);                   /* 129 >> 8 */
    assert(neon_shl_reg(rep_lane(0x80, 0), rep_lane(0xF7, 0), 0, 0, 0, 0) == rep_lane(0xFF, 0));   /* -128 >> 9 */
    assert(neon_shl_reg(rep_lane(0x80, 0), rep_lane(0xF7, 0), 0, 1, 0, 0) == 0);
    assert(neon_shl_reg(rep_lane(0x80, 0), rep_lane(0x80, 0), 0, 0, 0, 0) == rep_lane(0xFF, 0));   /* -128 >> 128 */
    assert(neon_shl_reg(rep_lane(0x7F, 0), rep_lane(0x80, 0), 0, 0, 0, 0) == 0);                   /* 127 >> 128 */
    assert(neon_shl_reg(rep_lane(0x7F, 0), rep_lane(0xF8, 0), 0, 0, 0, 0) == 0);                   /* 127 >> 8 */

    /* VRSHL at the width. A right shift by w rounds on the top bit alone: 0x80
     * by 8 is 0.5, which rounds to 1 unsigned and to 0 signed (-0.5 rounds up).
     * Past the width it is 0: 0x80 by 9 is 0.25 unsigned, which rounds to 0. */
    assert(neon_shl_reg(rep_lane(0x80, 0), rep_lane(0xF8, 0), 0, 1, 1, 0) == rep_lane(0x01, 0));
    assert(neon_shl_reg(rep_lane(0x80, 0), rep_lane(0xF8, 0), 0, 0, 1, 0) == 0);
    assert(neon_shl_reg(rep_lane(0x7F, 0), rep_lane(0xF8, 0), 0, 0, 1, 0) == 0);
    assert(neon_shl_reg(rep_lane(0x80, 0), rep_lane(0xF7, 0), 0, 1, 1, 0) == 0);

    /* VRSHL rounding, the values the spec names. 0x7F by 1 is 0x40, and -1 by 1
     * is 0 (signed). Unsigned 0xFF by 1 is 128, which x + 1 would wrap in 8 bits
     * to 0 (0xFF + 1 = 0x100), so the rounding must not overflow the lane. */
    assert(neon_shl_reg(rep_lane(0x7F, 0), rep_lane(0xFF, 0), 0, 0, 1, 0) == rep_lane(0x40, 0));
    assert(neon_shl_reg(rep_lane(0xFF, 0), rep_lane(0xFF, 0), 0, 0, 1, 0) == 0);
    assert(neon_shl_reg(rep_lane(0xFF, 0), rep_lane(0xFF, 0), 0, 1, 1, 0) == rep_lane(0x80, 0));
    assert(neon_shl_reg(rep_lane(0x81, 0), rep_lane(0xFF, 0), 0, 0, 1, 0) == rep_lane(0xC1, 0));
    assert(neon_shl_reg(rep_lane(0xFFFFFFFF, 2), rep_lane(0xFF, 2), 2, 1, 1, 0) == rep_lane(0x80000000, 2));
    assert(neon_shl_reg(~0ull, 0xFF, 3, 1, 1, 0) == 0x8000000000000000ull);
    assert(neon_shl_reg(~0ull, 0xFF, 3, 0, 1, 0) == 0);                    /* -1 rounded by 1 */
    assert(neon_shl_reg(0x8000000000000001ull, 0xFF, 3, 1, 1, 0) == 0x4000000000000001ull);

    /* Rounding by 64 on 64-bit lanes. Unsigned all-ones is 2^64 - 1, and
     * (2^64 - 1 + 2^63) >> 64 is 1. The signed values here are -2^63 and
     * 2^63 - 1, and both round to 0. */
    assert(neon_shl_reg(~0ull, 0xC0, 3, 1, 1, 0) == 1);
    assert(neon_shl_reg(0x8000000000000000ull, 0xC0, 3, 0, 1, 0) == 0);
    assert(neon_shl_reg(0x7FFFFFFFFFFFFFFFull, 0xC0, 3, 0, 1, 0) == 0);

    /* VQSHL, the saturation the spec names. 0x40 << 1 clamps to 0x7F, and 0xC0
     * (-64) << 1 is exactly -128, which fits. 0xBF (-65) << 1 does not and
     * clamps to 0x80, where the plain shift gives 0x7E. */
    assert(neon_shl_reg(rep_lane(0x40, 0), rep_lane(0x01, 0), 0, 0, 0, 1) == rep_lane(0x7F, 0));
    assert(neon_shl_reg(rep_lane(0xC0, 0), rep_lane(0x01, 0), 0, 0, 0, 1) == rep_lane(0x80, 0));
    assert(neon_shl_reg(rep_lane(0xBF, 0), rep_lane(0x01, 0), 0, 0, 0, 1) == rep_lane(0x80, 0));
    assert(neon_shl_reg(rep_lane(0xBF, 0), rep_lane(0x01, 0), 0, 0, 0, 0) == rep_lane(0x7E, 0));
    /* Unsigned 0x80 << 1 is 256, clamped to 0xFF. */
    assert(neon_shl_reg(rep_lane(0x80, 0), rep_lane(0x01, 0), 0, 1, 0, 1) == rep_lane(0xFF, 0));
    /* Zero never saturates, however far it is shifted left. */
    assert(neon_shl_reg(0, rep_lane(0x7F, 0), 0, 0, 0, 1) == 0);
    assert(neon_shl_reg(0, rep_lane(0x7F, 0), 0, 1, 0, 1) == 0);
    /* 1 << 64 on u64 is past the lane, so it clamps to all-ones. -1 << 63 is
     * -2^63, which fits, and 1 << 63 is 2^63, which clamps to 2^63 - 1. */
    assert(neon_shl_reg(1, 0x40, 3, 1, 0, 1) == ~0ull);
    assert(neon_shl_reg(~0ull, 0x3F, 3, 0, 0, 1) == 0x8000000000000000ull);
    assert(neon_shl_reg(1, 0x3F, 3, 0, 0, 1) == 0x7FFFFFFFFFFFFFFFull);
    /* Wider lanes: 0x4000 << 1 clamps, and -2^30 << 1 is -2^31, which fits. */
    assert(neon_shl_reg(rep_lane(0x4000, 1), rep_lane(0x01, 1), 1, 0, 0, 1) == rep_lane(0x7FFF, 1));
    assert(neon_shl_reg(rep_lane(0xC0000000, 2), rep_lane(0x01, 2), 2, 0, 0, 1) == rep_lane(0x80000000, 2));

    /* VQRSHL: a left shift that clamps, and a right shift that rounds. */
    assert(neon_shl_reg(rep_lane(0x40, 0), rep_lane(0x01, 0), 0, 0, 1, 1) == rep_lane(0x7F, 0));
    assert(neon_shl_reg(rep_lane(0x7F, 0), rep_lane(0xFF, 0), 0, 0, 1, 1) == rep_lane(0x40, 0));
    assert(neon_shl_reg(rep_lane(0xFFFF, 1), rep_lane(0xFF, 1), 1, 1, 1, 1) == rep_lane(0x8000, 1));

    printf("ok   neon shl reg: vshl, vqshl, vrshl, vqrshl by register, amounts, rounding and saturation\n");
}

/* The widening helpers, against values worked out by hand from the lanes.
 * Every source has a lane with its sign bit set, so each signed result differs
 * from the unsigned one. The 32-bit products 0x80000000 * 0x80000000 are 2^62
 * either way; the lane 0xFFFFFFFF * 2 is not, and it tells them apart. The
 * helpers take the D words as the emitter passes them, so these tests cover
 * the operand handling as well as the arithmetic. */
static void test_neon_widen(void) {
    uint64_t r[2];

    /* 8-bit source, 16-bit results. N is -128, -1, 1, 127, 0, 16, -16, 5 and M
     * is 1, -128, -1, 127, 2, 1, -128, 16 signed; the unsigned reading of the
     * same bytes is 128, 255, 1, 127, 0, 16, 240, 5 and 1, 128, 255, 127, 2, 1,
     * 128, 16. */
    const uint64_t N8[8] = { 0x80, 0xFF, 0x01, 0x7F, 0x00, 0x10, 0xF0, 0x05 };
    const uint64_t M8[8] = { 0x01, 0x80, 0xFF, 0x7F, 0x02, 0x01, 0x80, 0x10 };
    const uint64_t dn8 = pack_lanes(N8, 8, 8), dm8 = pack_lanes(M8, 8, 8);
    const uint64_t nl8[2] = { dn8, 0 };                 /* L form: n is one D word */
    const uint64_t add_s[8] = { 0xFF81, 0xFF7F, 0x0000, 0x00FE, 0x0002, 0x0011, 0xFF70, 0x0015 };
    const uint64_t add_u[8] = { 0x0081, 0x017F, 0x0100, 0x00FE, 0x0002, 0x0011, 0x0170, 0x0015 };
    const uint64_t sub_s[8] = { 0xFF7F, 0x007F, 0x0002, 0x0000, 0xFFFE, 0x000F, 0x0070, 0xFFF5 };
    const uint64_t sub_u[8] = { 0x007F, 0x007F, 0xFF02, 0x0000, 0xFFFE, 0x000F, 0x0070, 0xFFF5 };
    neon_wide_addsub(r, nl8, dm8, 0, 0, 0, 0); check_q(r, add_s, 16);
    neon_wide_addsub(r, nl8, dm8, 0, 1, 0, 0); check_q(r, add_u, 16);
    neon_wide_addsub(r, nl8, dm8, 0, 0, 0, 1); check_q(r, sub_s, 16);
    neon_wide_addsub(r, nl8, dm8, 0, 1, 0, 1); check_q(r, sub_u, 16);

    /* VMULL, and VMLAL/VMLSL. VMLAL adds the products to 100 in every lane.
     * VMLSL subtracts them from 0, so the lanes wrap below zero. */
    const uint64_t mul_s[8] = { 0xFF80, 0x0080, 0xFFFF, 0x3F01, 0x0000, 0x0010, 0x0800, 0x0050 };
    const uint64_t mul_u[8] = { 0x0080, 0x7F80, 0x00FF, 0x3F01, 0x0000, 0x0010, 0x7800, 0x0050 };
    const uint64_t acc_s[8] = { 0xFFE4, 0x00E4, 0x0063, 0x3F65, 0x0064, 0x0074, 0x0864, 0x00B4 };
    const uint64_t mls_u[8] = { 0xFF80, 0x8080, 0xFF01, 0xC0FF, 0x0000, 0xFFF0, 0x8800, 0xFFB0 };
    const uint64_t hundred[2] = { 0x0064006400640064ull, 0x0064006400640064ull };
    neon_wide_mul(r, dn8, dm8, 0, 0); check_q(r, mul_s, 16);
    neon_wide_mul(r, dn8, dm8, 0, 1); check_q(r, mul_u, 16);
    r[0] = hundred[0]; r[1] = hundred[1];
    neon_wide_mac(r, dn8, dm8, 0, 0, 0); check_q(r, acc_s, 16);
    r[0] = 0; r[1] = 0;
    neon_wide_mac(r, dn8, dm8, 0, 1, 1); check_q(r, mls_u, 16);

    /* VADDW and VSUBW: the wide operand is the Q pair, taken as it is, and
     * only M is widened. W is 0x0100, 0xFFFF, 0, 0x8000, 0x7FFF, 1, 0xFF00,
     * 0x1234 as 16-bit lanes. */
    const uint64_t W8[8] = { 0x0100, 0xFFFF, 0x0000, 0x8000, 0x7FFF, 0x0001, 0xFF00, 0x1234 };
    const uint64_t nq8[2] = { pack_lanes(W8, 16, 4), pack_lanes(W8 + 4, 16, 4) };
    const uint64_t addw_s[8] = { 0x0101, 0xFF7F, 0xFFFF, 0x807F, 0x8001, 0x0002, 0xFE80, 0x1244 };
    const uint64_t addw_u[8] = { 0x0101, 0x007F, 0x00FF, 0x807F, 0x8001, 0x0002, 0xFF80, 0x1244 };
    const uint64_t subw_s[8] = { 0x00FF, 0x007F, 0x0001, 0x7F81, 0x7FFD, 0x0000, 0xFF80, 0x1224 };
    const uint64_t subw_u[8] = { 0x00FF, 0xFF7F, 0xFF01, 0x7F81, 0x7FFD, 0x0000, 0xFE80, 0x1224 };
    neon_wide_addsub(r, nq8, dm8, 0, 0, 1, 0); check_q(r, addw_s, 16);
    neon_wide_addsub(r, nq8, dm8, 0, 1, 1, 0); check_q(r, addw_u, 16);
    neon_wide_addsub(r, nq8, dm8, 0, 0, 1, 1); check_q(r, subw_s, 16);
    neon_wide_addsub(r, nq8, dm8, 0, 1, 1, 1); check_q(r, subw_u, 16);

    /* 16-bit source, 32-bit results. N is -32768, -1, 1, 32767 and M is 1,
     * -32768, -1, 32767 signed; unsigned, N is 32768, 65535, 1, 32767 and M
     * is 1, 32768, 65535, 32767. */
    const uint64_t N16[4] = { 0x8000, 0xFFFF, 0x0001, 0x7FFF };
    const uint64_t M16[4] = { 0x0001, 0x8000, 0xFFFF, 0x7FFF };
    const uint64_t dn16 = pack_lanes(N16, 16, 4), dm16 = pack_lanes(M16, 16, 4);
    const uint64_t nl16[2] = { dn16, 0 };
    const uint64_t addl16_s[4] = { 0xFFFF8001, 0xFFFF7FFF, 0x00000000, 0x0000FFFE };
    const uint64_t addl16_u[4] = { 0x00008001, 0x00017FFF, 0x00010000, 0x0000FFFE };
    const uint64_t subl16_s[4] = { 0xFFFF7FFF, 0x00007FFF, 0x00000002, 0x00000000 };
    const uint64_t subl16_u[4] = { 0x00007FFF, 0x00007FFF, 0xFFFF0002, 0x00000000 };
    const uint64_t mull16_s[4] = { 0xFFFF8000, 0x00008000, 0xFFFFFFFF, 0x3FFF0001 };
    const uint64_t mull16_u[4] = { 0x00008000, 0x7FFF8000, 0x0000FFFF, 0x3FFF0001 };
    const uint64_t mlsl16_s[4] = { 0x00008000, 0xFFFF8000, 0x00000001, 0xC000FFFF };
    neon_wide_addsub(r, nl16, dm16, 1, 0, 0, 0); check_q(r, addl16_s, 32);
    neon_wide_addsub(r, nl16, dm16, 1, 0, 0, 1); check_q(r, subl16_s, 32);
    neon_wide_mul(r, dn16, dm16, 1, 0);          check_q(r, mull16_s, 32);
    r[0] = 0; r[1] = 0;
    neon_wide_mac(r, dn16, dm16, 1, 0, 1);       check_q(r, mlsl16_s, 32);
    neon_wide_addsub(r, nl16, dm16, 1, 1, 0, 0); check_q(r, addl16_u, 32);
    neon_wide_addsub(r, nl16, dm16, 1, 1, 0, 1); check_q(r, subl16_u, 32);
    neon_wide_mul(r, dn16, dm16, 1, 1);          check_q(r, mull16_u, 32);

    /* 16-bit W form: the wide operand is 32-bit lanes. Lane 0 0x00010000 + 1,
     * lane 1 5 - 32768 (wraps below zero), lane 2 0xFFFF0000 - 1, lane 3
     * 0x7FFFFFFF + 32767 (carries into bit 31). */
    const uint64_t W16[4] = { 0x00010000, 0x00000005, 0xFFFF0000, 0x7FFFFFFF };
    const uint64_t nq16[2] = { pack_lanes(W16, 32, 2), pack_lanes(W16 + 2, 32, 2) };
    const uint64_t addw16_s[4] = { 0x00010001, 0xFFFF8005, 0xFFFEFFFF, 0x80007FFE };
    neon_wide_addsub(r, nq16, dm16, 1, 0, 1, 0); check_q(r, addw16_s, 32);

    /* 32-bit source, 64-bit results. N is 0x80000000 and 0xFFFFFFFF, M is
     * 0x80000000 and 2. */
    const uint64_t N32[2] = { 0x80000000, 0xFFFFFFFF };
    const uint64_t M32[2] = { 0x80000000, 0x00000002 };
    const uint64_t dn32 = pack_lanes(N32, 32, 2), dm32 = pack_lanes(M32, 32, 2);
    const uint64_t nl32[2] = { dn32, 0 };
    const uint64_t addl32_s[2] = { 0xFFFFFFFF00000000ull, 0x0000000000000001ull };
    const uint64_t addl32_u[2] = { 0x0000000100000000ull, 0x0000000100000001ull };
    const uint64_t mull32_s[2] = { 0x4000000000000000ull, 0xFFFFFFFFFFFFFFFEull };
    const uint64_t mull32_u[2] = { 0x4000000000000000ull, 0x00000001FFFFFFFEull };
    const uint64_t mlal32_s[2] = { 0x4000000000000001ull, 0x0000000000000000ull };
    const uint64_t mlsl32_u[2] = { 0xC000000000000000ull, 0xFFFFFFFE00000002ull };
    neon_wide_addsub(r, nl32, dm32, 2, 0, 0, 0); check_q(r, addl32_s, 64);
    neon_wide_mul(r, dn32, dm32, 2, 0);          check_q(r, mull32_s, 64);
    r[0] = 1; r[1] = 2;
    neon_wide_mac(r, dn32, dm32, 2, 0, 0);       check_q(r, mlal32_s, 64);
    neon_wide_addsub(r, nl32, dm32, 2, 1, 0, 0); check_q(r, addl32_u, 64);
    neon_wide_mul(r, dn32, dm32, 2, 1);          check_q(r, mull32_u, 64);
    r[0] = 0; r[1] = 0;
    neon_wide_mac(r, dn32, dm32, 2, 1, 1);       check_q(r, mlsl32_u, 64);

    /* 32-bit W form. W is 2^63 and 5; M is -2^31 and 2 signed. The signed
     * subtract gives 2^63 + 2^31 in lane 0 and the unsigned one 2^63 - 2^31. */
    const uint64_t nq32[2] = { 0x8000000000000000ull, 0x0000000000000005ull };
    const uint64_t addw32_s[2] = { 0x7FFFFFFF80000000ull, 0x0000000000000007ull };
    const uint64_t subw32_s[2] = { 0x8000000080000000ull, 0x0000000000000003ull };
    const uint64_t subw32_u[2] = { 0x7FFFFFFF80000000ull, 0x0000000000000003ull };
    neon_wide_addsub(r, nq32, dm32, 2, 0, 1, 0); check_q(r, addw32_s, 64);
    neon_wide_addsub(r, nq32, dm32, 2, 0, 1, 1); check_q(r, subw32_s, 64);
    neon_wide_addsub(r, nq32, dm32, 2, 1, 1, 1); check_q(r, subw32_u, 64);

    /* VSHLL by esize - 1, and VMOVL (shift 0). Each shifted lane keeps its
     * wide width: the sign bits shifted out of a source lane are masked. */
    const uint64_t shll8_u7[8] = { 0x4000, 0x7F80, 0x0080, 0x3F80, 0x0000, 0x0800, 0x7800, 0x0280 };
    const uint64_t shll8_s7[8] = { 0xC000, 0xFF80, 0x0080, 0x3F80, 0x0000, 0x0800, 0xF800, 0x0280 };
    const uint64_t shll16_s15[4] = { 0xC0000000, 0xFFFF8000, 0x00008000, 0x3FFF8000 };
    const uint64_t movl16_s[4] = { 0xFFFF8000, 0xFFFFFFFF, 0x00000001, 0x00007FFF };
    const uint64_t movl16_u[4] = { 0x00008000, 0x0000FFFF, 0x00000001, 0x00007FFF };
    const uint64_t shll32_u31[2] = { 0x4000000000000000ull, 0x7FFFFFFF80000000ull };
    const uint64_t shll32_s31[2] = { 0xC000000000000000ull, 0xFFFFFFFF80000000ull };
    neon_widen(r, dn8, 0, 7, 1);   check_q(r, shll8_u7, 16);
    neon_widen(r, dn8, 0, 7, 0);   check_q(r, shll8_s7, 16);
    neon_widen(r, dn16, 1, 15, 0); check_q(r, shll16_s15, 32);
    neon_widen(r, dn16, 1, 0, 0);  check_q(r, movl16_s, 32);
    neon_widen(r, dn16, 1, 0, 1);  check_q(r, movl16_u, 32);
    neon_widen(r, dn32, 2, 31, 1); check_q(r, shll32_u31, 64);
    neon_widen(r, dn32, 2, 31, 0); check_q(r, shll32_s31, 64);

    printf("ok   neon widening: vaddl/vsubl/vaddw/vsubw/vmull/vmlal/vmlsl at 8/16/32, vshll/vmovl\n");
}

/* The compares with zero and the integer VABS and VNEG, against values worked
 * out by hand. The emitter prints the expressions these asserts call: GT, GE
 * and EQ put the zero second (neon_cmp(x, 0, ...), neon_ceq(x, 0, ...)) and LE
 * and LT put it first (neon_cmp(0, x, ...)), and the F32 forms use neon_fcgt,
 * neon_fcge and neon_fceq the same way. The lanes are -1, 0, 1, the minimum and
 * the maximum, and 0x80 and 0xFF read as negative or large, so the signed and
 * unsigned answers differ and an unsigned compare fails here. */
static void test_neon_cmpz_c(void) {
    /* Integer compares, 8-bit lanes. Word A holds -1, 0, 1, min, max, -1, 0, 1. */
    uint64_t na8[8] = { 0xFF, 0x00, 0x01, 0x80, 0x7F, 0xFF, 0x00, 0x01 };
    uint64_t NA8 = pack_lanes(na8, 8, 8);
    uint64_t gt_a8[8] = { 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0xFF };
    uint64_t ge_a8[8] = { 0x00, 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xFF };
    uint64_t eq_a8[8] = { 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00 };
    uint64_t le_a8[8] = { 0xFF, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xFF, 0x00 };
    uint64_t lt_a8[8] = { 0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00 };
    assert(neon_cmp(NA8, 0, 0, 0, 0) == pack_lanes(gt_a8, 8, 8));
    assert(neon_cmp(NA8, 0, 0, 0, 1) == pack_lanes(ge_a8, 8, 8));
    assert(neon_ceq(NA8, 0, 0) == pack_lanes(eq_a8, 8, 8));
    assert(neon_cmp(0, NA8, 0, 0, 1) == pack_lanes(le_a8, 8, 8));
    assert(neon_cmp(0, NA8, 0, 0, 0) == pack_lanes(lt_a8, 8, 8));

    /* Word B: -128, 127, 0, 1, -2, -127, 64, -64. */
    uint64_t nb8[8] = { 0x80, 0x7F, 0x00, 0x01, 0xFE, 0x81, 0x40, 0xC0 };
    uint64_t NB8 = pack_lanes(nb8, 8, 8);
    uint64_t gt_b8[8] = { 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00 };
    uint64_t ge_b8[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF, 0x00 };
    uint64_t eq_b8[8] = { 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint64_t le_b8[8] = { 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xFF, 0x00, 0xFF };
    uint64_t lt_b8[8] = { 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0xFF };
    assert(neon_cmp(NB8, 0, 0, 0, 0) == pack_lanes(gt_b8, 8, 8));
    assert(neon_cmp(NB8, 0, 0, 0, 1) == pack_lanes(ge_b8, 8, 8));
    assert(neon_ceq(NB8, 0, 0) == pack_lanes(eq_b8, 8, 8));
    assert(neon_cmp(0, NB8, 0, 0, 1) == pack_lanes(le_b8, 8, 8));
    assert(neon_cmp(0, NB8, 0, 0, 0) == pack_lanes(lt_b8, 8, 8));

    /* 16-bit lanes: -1, 0, 1, min in word A; max, 0, -1, 1 in word B. */
    uint64_t na16[4] = { 0xFFFF, 0x0000, 0x0001, 0x8000 };
    uint64_t NA16 = pack_lanes(na16, 16, 4);
    uint64_t gt_a16[4] = { 0x0000, 0x0000, 0xFFFF, 0x0000 };
    uint64_t ge_a16[4] = { 0x0000, 0xFFFF, 0xFFFF, 0x0000 };
    uint64_t eq_a16[4] = { 0x0000, 0xFFFF, 0x0000, 0x0000 };
    uint64_t le_a16[4] = { 0xFFFF, 0xFFFF, 0x0000, 0xFFFF };
    uint64_t lt_a16[4] = { 0xFFFF, 0x0000, 0x0000, 0xFFFF };
    assert(neon_cmp(NA16, 0, 1, 0, 0) == pack_lanes(gt_a16, 16, 4));
    assert(neon_cmp(NA16, 0, 1, 0, 1) == pack_lanes(ge_a16, 16, 4));
    assert(neon_ceq(NA16, 0, 1) == pack_lanes(eq_a16, 16, 4));
    assert(neon_cmp(0, NA16, 1, 0, 1) == pack_lanes(le_a16, 16, 4));
    assert(neon_cmp(0, NA16, 1, 0, 0) == pack_lanes(lt_a16, 16, 4));

    uint64_t nb16[4] = { 0x7FFF, 0x0000, 0xFFFF, 0x0001 };
    uint64_t NB16 = pack_lanes(nb16, 16, 4);
    uint64_t gt_b16[4] = { 0xFFFF, 0x0000, 0x0000, 0xFFFF };
    uint64_t ge_b16[4] = { 0xFFFF, 0xFFFF, 0x0000, 0xFFFF };
    uint64_t eq_b16[4] = { 0x0000, 0xFFFF, 0x0000, 0x0000 };
    uint64_t le_b16[4] = { 0x0000, 0xFFFF, 0xFFFF, 0x0000 };
    uint64_t lt_b16[4] = { 0x0000, 0x0000, 0xFFFF, 0x0000 };
    assert(neon_cmp(NB16, 0, 1, 0, 0) == pack_lanes(gt_b16, 16, 4));
    assert(neon_cmp(NB16, 0, 1, 0, 1) == pack_lanes(ge_b16, 16, 4));
    assert(neon_ceq(NB16, 0, 1) == pack_lanes(eq_b16, 16, 4));
    assert(neon_cmp(0, NB16, 1, 0, 1) == pack_lanes(le_b16, 16, 4));
    assert(neon_cmp(0, NB16, 1, 0, 0) == pack_lanes(lt_b16, 16, 4));

    /* 32-bit lanes: -1 and 0; min and max; 1 and 0. */
    uint64_t na32[2] = { 0xFFFFFFFF, 0x00000000 };
    uint64_t NA32 = pack_lanes(na32, 32, 2);
    uint64_t gt_a32[2] = { 0x00000000, 0x00000000 };
    uint64_t ge_a32[2] = { 0x00000000, 0xFFFFFFFF };
    uint64_t eq_a32[2] = { 0x00000000, 0xFFFFFFFF };
    uint64_t le_a32[2] = { 0xFFFFFFFF, 0xFFFFFFFF };
    uint64_t lt_a32[2] = { 0xFFFFFFFF, 0x00000000 };
    assert(neon_cmp(NA32, 0, 2, 0, 0) == pack_lanes(gt_a32, 32, 2));
    assert(neon_cmp(NA32, 0, 2, 0, 1) == pack_lanes(ge_a32, 32, 2));
    assert(neon_ceq(NA32, 0, 2) == pack_lanes(eq_a32, 32, 2));
    assert(neon_cmp(0, NA32, 2, 0, 1) == pack_lanes(le_a32, 32, 2));
    assert(neon_cmp(0, NA32, 2, 0, 0) == pack_lanes(lt_a32, 32, 2));

    uint64_t nb32[2] = { 0x80000000, 0x7FFFFFFF };
    uint64_t NB32 = pack_lanes(nb32, 32, 2);
    uint64_t gt_b32[2] = { 0x00000000, 0xFFFFFFFF };
    uint64_t ge_b32[2] = { 0x00000000, 0xFFFFFFFF };
    uint64_t eq_b32[2] = { 0x00000000, 0x00000000 };
    uint64_t le_b32[2] = { 0xFFFFFFFF, 0x00000000 };
    uint64_t lt_b32[2] = { 0xFFFFFFFF, 0x00000000 };
    assert(neon_cmp(NB32, 0, 2, 0, 0) == pack_lanes(gt_b32, 32, 2));
    assert(neon_cmp(NB32, 0, 2, 0, 1) == pack_lanes(ge_b32, 32, 2));
    assert(neon_ceq(NB32, 0, 2) == pack_lanes(eq_b32, 32, 2));
    assert(neon_cmp(0, NB32, 2, 0, 1) == pack_lanes(le_b32, 32, 2));
    assert(neon_cmp(0, NB32, 2, 0, 0) == pack_lanes(lt_b32, 32, 2));

    /* Integer VABS, wrapping. The minimum stays, since -(-2^(w-1)) does not
     * fit, and -1 becomes 1. */
    assert(neon_abs(rep_lane(0x80, 0), 0) == rep_lane(0x80, 0));
    assert(neon_abs(rep_lane(0x8000, 1), 1) == rep_lane(0x8000, 1));
    assert(neon_abs(rep_lane(0x80000000, 2), 2) == rep_lane(0x80000000, 2));
    assert(neon_abs(rep_lane(0xFF, 0), 0) == rep_lane(0x01, 0));
    assert(neon_abs(rep_lane(0xFFFF, 1), 1) == rep_lane(0x0001, 1));
    assert(neon_abs(rep_lane(0xFFFFFFFF, 2), 2) == rep_lane(0x00000001, 2));
    assert(neon_abs(rep_lane(0x7F, 0), 0) == rep_lane(0x7F, 0));
    assert(neon_abs(rep_lane(0x00, 0), 0) == 0);

    /* Mixed lanes for VABS and VNEG, 8-bit: -1, 0, 1, min, max, -2, -127, -112. */
    uint64_t va8[8] = { 0xFF, 0x00, 0x01, 0x80, 0x7F, 0xFE, 0x81, 0x90 };
    uint64_t VA8 = pack_lanes(va8, 8, 8);
    uint64_t abs8[8] = { 0x01, 0x00, 0x01, 0x80, 0x7F, 0x02, 0x7F, 0x70 };
    uint64_t neg8[8] = { 0x01, 0x00, 0xFF, 0x80, 0x81, 0x02, 0x7F, 0x70 };
    assert(neon_abs(VA8, 0) == pack_lanes(abs8, 8, 8));
    assert(neon_sub(0, VA8, 0) == pack_lanes(neg8, 8, 8));

    uint64_t va16[4] = { 0xFFFF, 0x0000, 0x0001, 0x8000 };
    uint64_t VA16 = pack_lanes(va16, 16, 4);
    uint64_t abs16[4] = { 0x0001, 0x0000, 0x0001, 0x8000 };
    uint64_t neg16[4] = { 0x0001, 0x0000, 0xFFFF, 0x8000 };
    assert(neon_abs(VA16, 1) == pack_lanes(abs16, 16, 4));
    assert(neon_sub(0, VA16, 1) == pack_lanes(neg16, 16, 4));

    uint64_t va32[2] = { 0x80000000, 0xFFFFFFFF };
    uint64_t VA32 = pack_lanes(va32, 32, 2);
    uint64_t abs32[2] = { 0x80000000, 0x00000001 };
    uint64_t neg32[2] = { 0x80000000, 0x00000001 };
    assert(neon_abs(VA32, 2) == pack_lanes(abs32, 32, 2));
    assert(neon_sub(0, VA32, 2) == pack_lanes(neg32, 32, 2));

    /* VNEG of 0 and of the minimum, at 8 bits: 0 stays and -128 stays. */
    assert(neon_sub(0, rep_lane(0x00, 0), 0) == 0);
    assert(neon_sub(0, rep_lane(0x80, 0), 0) == rep_lane(0x80, 0));

    /* F32 compares against +0 and -0, with 1.5, -2, the infinities, a NaN of
     * each sign, and a denormal of each sign. neon_f32_in flushes a denormal to
     * its signed zero before the compare, so a positive denormal compares as
     * +0 and a negative one as -0: equal to zero, neither above nor below. Every
     * compare with a NaN is false. Ten values, each in both lanes of a word:
     * +0, -0, 1.5, -2, qNaN, +inf, -inf, +denormal, -denormal, -qNaN. */
    uint32_t fv[10] = { 0x00000000u, 0x80000000u, 0x3FC00000u, 0xC0000000u,
                        0x7FC00000u, 0x7F800000u, 0xFF800000u,
                        0x00000001u, 0x80000001u, 0xFFC00001u };
    uint32_t fgt[10] = { 0, 0, 0xFFFFFFFFu, 0, 0, 0xFFFFFFFFu, 0, 0, 0, 0 };
    uint32_t fge[10] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0, 0xFFFFFFFFu, 0,
                         0xFFFFFFFFu, 0xFFFFFFFFu, 0 };
    uint32_t feq[10] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0, 0, 0, 0,
                         0xFFFFFFFFu, 0xFFFFFFFFu, 0 };
    uint32_t fle[10] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0xFFFFFFFFu, 0, 0, 0xFFFFFFFFu,
                         0xFFFFFFFFu, 0xFFFFFFFFu, 0 };
    uint32_t flt[10] = { 0, 0, 0, 0xFFFFFFFFu, 0, 0, 0xFFFFFFFFu, 0, 0, 0 };
    for (int i = 0; i < 10; i++) {
        uint64_t x = f32_pair(fv[i], fv[i]);
        assert(neon_fcgt(x, 0) == f32_pair(fgt[i], fgt[i]));
        assert(neon_fcge(x, 0) == f32_pair(fge[i], fge[i]));
        assert(neon_fceq(x, 0) == f32_pair(feq[i], feq[i]));
        assert(neon_fcge(0, x) == f32_pair(fle[i], fle[i]));
        assert(neon_fcgt(0, x) == f32_pair(flt[i], flt[i]));
    }
    /* The lanes are independent: 1.5 above zero and -0 not, in one word. */
    assert(neon_fcgt(f32_pair(0x3FC00000u, 0x80000000u), 0) == f32_pair(0xFFFFFFFFu, 0));
    assert(neon_fcge(0, f32_pair(0x3FC00000u, 0x80000000u)) == f32_pair(0, 0xFFFFFFFFu));

    printf("ok   neon cmpz: integer compares with zero at 8/16/32 bits, vabs/vneg wrap, f32 compares with zero\n");
}

static void test_neon_shuffle(void) {
    /* A holds bytes 0x00..0x07 and B bytes 0x08..0x0F, so byte j of the pair
     * is j. X and Y continue the count at 0x10 and 0x18. Every expected value
     * below is worked out from that, by hand. */
    const uint64_t A = 0x0706050403020100ull, B = 0x0F0E0D0C0B0A0908ull;
    const uint64_t X = 0x1716151413121110ull, Y = 0x1F1E1D1C1B1A1918ull;
    uint64_t a[2], b[2];

    /* VEXT: the bytes of a:b from imm on. Q takes the second D word of b from
     * the second half of the result, so bytes cross the halves. */
    a[0] = A; b[0] = B;
    neon_ext(a, b, 1, 3);
    assert(a[0] == 0x0A09080706050403ull);
    a[0] = A; b[0] = B;
    neon_ext(a, b, 1, 7);
    assert(a[0] == 0x0E0D0C0B0A090807ull);
    a[0] = A; a[1] = B; b[0] = X; b[1] = Y;
    neon_ext(a, b, 2, 0);
    assert(a[0] == A && a[1] == B);                      /* imm 0 is a */
    a[0] = A; a[1] = B; b[0] = X; b[1] = Y;
    neon_ext(a, b, 2, 11);
    assert(a[0] == 0x1211100F0E0D0C0Bull && a[1] == 0x1A19181716151413ull);

    /* VREV: reverse the elements of each container. */
    a[0] = A;
    neon_rev(a, 1, 64, 0);
    assert(a[0] == 0x0001020304050607ull);                /* VREV64.8 d */
    a[0] = A;
    neon_rev(a, 1, 32, 0);
    assert(a[0] == 0x0405060700010203ull);                /* VREV32.8 d */
    a[0] = A;
    neon_rev(a, 1, 64, 1);
    assert(a[0] == 0x0100030205040706ull);                /* VREV64.16 d */
    a[0] = A;
    neon_rev(a, 1, 32, 1);
    assert(a[0] == 0x0504070601000302ull);                /* VREV32.16 d */
    a[0] = A; a[1] = B;
    neon_rev(a, 2, 16, 0);
    assert(a[0] == 0x0607040502030001ull && a[1] == 0x0E0F0C0D0A0B0809ull); /* VREV16.8 q */
    a[0] = A; a[1] = B;
    neon_rev(a, 2, 64, 2);
    assert(a[0] == 0x0302010007060504ull && a[1] == 0x0B0A09080F0E0D0Cull); /* VREV64.32 q */

    /* VSWP exchanges the words. */
    a[0] = A; a[1] = B; b[0] = X; b[1] = Y;
    neon_swp(a, b, 2);
    assert(a[0] == X && a[1] == Y && b[0] == A && b[1] == B);

    /* VTRN: lane 2i+1 of a with lane 2i of b. */
    a[0] = A; b[0] = B;
    neon_trn(a, b, 1, 1);                                 /* 16-bit, d */
    assert(a[0] == 0x0D0C050409080100ull && b[0] == 0x0F0E07060B0A0302ull);
    a[0] = A; a[1] = B; b[0] = X; b[1] = Y;
    neon_trn(a, b, 2, 2);                                 /* 32-bit, q: per half */
    assert(a[0] == 0x1312111003020100ull && a[1] == 0x1B1A19180B0A0908ull);
    assert(b[0] == 0x1716151407060504ull && b[1] == 0x1F1E1D1C0F0E0D0Cull);

    /* VUZP: a gets the even elements of a:b, b the odd. The Q case takes its
     * elements from both words, so word 0 of a holds elements of a's word 1. */
    a[0] = A; b[0] = B;
    neon_uzp(a, b, 1, 0);                                 /* 8-bit, d */
    assert(a[0] == 0x0E0C0A0806040200ull && b[0] == 0x0F0D0B0907050301ull);
    a[0] = A; a[1] = B; b[0] = X; b[1] = Y;
    neon_uzp(a, b, 2, 1);                                 /* 16-bit, q */
    assert(a[0] == 0x0D0C090805040100ull && a[1] == 0x1D1C191815141110ull);
    assert(b[0] == 0x0F0E0B0A07060302ull && b[1] == 0x1F1E1B1A17161312ull);

    /* VZIP: a0,b0,a1,b1,... ; a takes the first half of the interleave. */
    a[0] = A; b[0] = B;
    neon_zip(a, b, 1, 0);                                 /* 8-bit, d */
    assert(a[0] == 0x0B030A0209010800ull && b[0] == 0x0F070E060D050C04ull);
    a[0] = A; a[1] = B; b[0] = X; b[1] = Y;
    neon_zip(a, b, 2, 1);                                 /* 16-bit, q: crosses */
    assert(a[0] == 0x1312030211100100ull && a[1] == 0x1716070615140504ull);
    assert(b[0] == 0x1B1A0B0A19180908ull && b[1] == 0x1F1E0F0E1D1C0D0Cull);
    a[0] = A; a[1] = B; b[0] = X; b[1] = Y;
    neon_zip(a, b, 2, 2);                                 /* 32-bit, q */
    assert(a[0] == 0x1312111003020100ull && a[1] == 0x1716151407060504ull);
    assert(b[0] == 0x1B1A19180B0A0908ull && b[1] == 0x1F1E1D1C0F0E0D0Cull);

    /* VDUP (scalar): the lane is every lane of the result. */
    assert(neon_dup_lane(A, 0, 5) == 0x0505050505050505ull);          /* .8 */
    assert(neon_dup_lane(B, 1, 2) == 0x0D0C0D0C0D0C0D0Cull);          /* .16 */
    assert(neon_dup_lane(A, 2, 1) == 0x0706050407060504ull);          /* .32 */

    /* VTBL: an index past the table gives zero. Table of one D word: the
     * index 8 is exactly 8 * len and 0xFF is far past it. */
    uint64_t t[4] = { A, B, 0, 0 };
    assert(neon_tbl(t, 1, 0x01070C080700FF03ull, 0) == 0x0107000007000003ull);
    /* Table of two words, indices 16 (= 8 * 2) and 0xFF out of range. */
    assert(neon_tbl(t, 2, 0x07FF100A000F0810ull, 0) == 0x0700000A000F0800ull);

    /* VTBX: an out-of-range byte keeps the old destination byte. The old value
     * is 0xEEDDCCBBAA998877, byte j being 0x77 + 0x11 * j. */
    const uint64_t old = 0xEEDDCCBBAA998877ull;
    assert(neon_tbl(t, 2, 0x07FF100A000F0810ull, old) == 0x07DDCC0A000F0877ull);
    assert(neon_tbl(t, 1, 0x01070C080700FF03ull, old) == 0x0107CCBB07008803ull);

    printf("ok   neon shuffles: vext, vrev, vswp, vtrn, vuzp, vzip, vdup scalar, vtbl/vtbx\n");
}

/* VLD2-4 / VST2-4 element order. Memory holds bytes 0x00, 0x01, ... in order,
 * as little-endian D words, so each expected value below is the byte index
 * of each element, worked out by hand: member k takes memory elements k, k+n,
 * k+2n, ... Lanes are listed from the top down in the hex literals. */
static void test_neon_ldst(void) {
    const uint64_t M[4] = { 0x0706050403020100ull, 0x0F0E0D0C0B0A0908ull,
                            0x1716151413121110ull, 0x1F1E1D1C1B1A1918ull };
    uint64_t r[8];

    /* VLD2.8 {d0-d1}: even bytes to d0, odd bytes to d1. */
    neon_deinterleave(M, r, 2, 1, 0);
    assert(r[0] == 0x0E0C0A0806040200ull && r[1] == 0x0F0D0B0907050301ull);
    /* VLD2.16 {d0,d2}: every other halfword. */
    neon_deinterleave(M, r, 2, 1, 1);
    assert(r[0] == 0x0D0C090805040100ull && r[1] == 0x0F0E0B0A07060302ull);
    /* VLD2.32 {d0,d2}: every other word. */
    neon_deinterleave(M, r, 2, 1, 2);
    assert(r[0] == 0x0B0A090803020100ull && r[1] == 0x0F0E0D0C07060504ull);

    /* VLD2.8 {d0-d3}, the Q-pair form. Member 0 is the even bytes of all 32,
     * so its second D word (r[1]) holds bytes 16..30, past the first 16. */
    neon_deinterleave(M, r, 2, 2, 0);
    assert(r[0] == 0x0E0C0A0806040200ull && r[1] == 0x1E1C1A1816141210ull);
    assert(r[2] == 0x0F0D0B0907050301ull && r[3] == 0x1F1D1B1917151311ull);

    /* VLD3.8 {d0-d2}: byte 3j+k to member k. */
    neon_deinterleave(M, r, 3, 1, 0);
    assert(r[0] == 0x15120F0C09060300ull);
    assert(r[1] == 0x1613100D0A070401ull);
    assert(r[2] == 0x1714110E0B080502ull);
    /* VLD3.32 {d0,d1,d2}: 32-bit element 3j+k to member k, two elements each. */
    neon_deinterleave(M, r, 3, 1, 2);
    assert(r[0] == 0x0F0E0D0C03020100ull);
    assert(r[1] == 0x1312111007060504ull);
    assert(r[2] == 0x171615140B0A0908ull);

    /* VLD4.8 {d0-d3}: byte 4j+k to member k. */
    neon_deinterleave(M, r, 4, 1, 0);
    assert(r[0] == 0x1C1814100C080400ull);
    assert(r[1] == 0x1D1915110D090501ull);
    assert(r[2] == 0x1E1A16120E0A0602ull);
    assert(r[3] == 0x1F1B17130F0B0703ull);
    /* VLD4.16 {d0,d2,d4,d6}: halfword 4j+k to member k. */
    neon_deinterleave(M, r, 4, 1, 1);
    assert(r[0] == 0x1918111009080100ull);
    assert(r[1] == 0x1B1A13120B0A0302ull);
    assert(r[2] == 0x1D1C15140D0C0504ull);
    assert(r[3] == 0x1F1E17160F0E0706ull);

    /* VST is the inverse: interleaving the VLD2.8 members gives back the
     * first sixteen bytes exactly. */
    neon_deinterleave(M, r, 2, 1, 0);
    neon_interleave(r, r + 4, 2, 1, 0);
    assert(r[4] == M[0] && r[5] == M[1]);

    /* interleave(deinterleave(x)) == x and deinterleave(interleave(x)) == x,
     * for every member count, width and element size. The pattern makes each
     * word distinct, so a misplaced element shows. */
    for (int n = 2; n <= 4; n++)
        for (int words = 1; words <= 2; words++)
            for (int size = 0; size <= 3; size++) {
                uint64_t x[8], m[8], back[8];
                for (int i = 0; i < n * words; i++)
                    x[i] = 0x9E3779B97F4A7C15ull * (uint64_t)(i + 1) + 0x0123456789ABCDEFull * (uint64_t)i;
                neon_deinterleave(x, m, n, words, size);
                neon_interleave(m, back, n, words, size);
                for (int i = 0; i < n * words; i++) assert(back[i] == x[i]);
                neon_interleave(x, m, n, words, size);
                neon_deinterleave(m, back, n, words, size);
                for (int i = 0; i < n * words; i++) assert(back[i] == x[i]);
            }

    printf("ok   neon structures: vld2/3/4 element order, Q pairs, interleave round trips\n");
}

/* --- narrowing shifts and saturating narrows -------------------------------- */

static void test_neon_narrow(void) {
    /* Truncating shift, 16-bit lanes to 8, by 8. The lanes of lo, low first,
     * are 0x0123 0x4567 0x89AB 0xCDEF; those of hi are 0x1000 0x0FFF 0x7FFF
     * 0x8000. */
    uint64_t lo = 0xCDEF89AB45670123ull, hi = 0x80007FFF0FFF1000ull;
    assert(neon_shrn(lo, hi, 8, 0, 0) == 0x807F0F10CD894501ull);
    /* Rounded: bit 7 of a lane adds one to its result. 0x89AB and 0xCDEF have
     * it set, and so do 0x0FFF and 0x7FFF, giving 0x8A, 0xCE, 0x10, 0x80. */
    assert(neon_shrn(lo, hi, 8, 0, 1) == 0x80801010CE8A4501ull);
    /* VMOVN is the zero-amount truncation, so the two must agree. */
    for (int size = 0; size <= 2; size++)
        assert(neon_shrn(lo, hi, 0, size, 0) == neon_movn(lo, hi, size));
    /* No 128-bit source: size 3 is not a form. */
    assert(neon_shrn(lo, hi, 1, 3, 0) == 0);

    /* Signed saturation, 16 to 8 (VQMOVN.S16). Lanes of lo: 0x007F 0x0080 0xFF80
     * 0x8000 give 0x7F 0x7F 0x80 0x80; lanes of hi: 0x1234 0xFFFF 0x0000 0x7FFF
     * give 0x7F 0xFF 0x00 0x7F. -128 is in range, so 0xFF80 keeps 0x80. */
    assert(neon_qshrn(0x8000FF800080007Full, 0x7FFF0000FFFF1234ull, 0, 0, 0, 1, 0) == 0x7F00FF7F80807F7Full);

    /* VQMOVUN.S16: a signed source clamps to [0, 255]. 0x0100 gives 0xFF, the
     * negative 0xFF80 and 0x8000 give 0. */
    assert(neon_qshrn(0x8000FF800100007Full, 0x7FFFFFFF000100FFull, 0, 0, 0, 1, 1) == 0xFF0001FF0000FF7Full);

    /* VQMOVN.U16: unsigned lanes saturate at 255 from above only. */
    assert(neon_qshrn(0xFFFF8000010000FFull, 0x00807FFF00420000ull, 0, 0, 0, 0, 1) == 0x80FF4200FFFFFFFFull);

    /* Rounding at exactly .5, shift 1. 3 5 7 9 give 2 3 4 5 when rounded and
     * 1 2 3 4 when truncated. Signed, -3 and -5 round half up to -1 and -2. */
    assert(neon_shrn(0x0009000700050003ull, 0, 1, 0, 1) == 0x05040302ull);
    assert(neon_shrn(0x0009000700050003ull, 0, 1, 0, 0) == 0x04030201ull);
    assert(neon_qshrn(0x00050003FFFBFFFDull, 0, 1, 0, 1, 1, 0) == 0x0302FEFFull);

    /* Signed saturation, 32 to 16 (size 1): 0x00008000 gives 0x7FFF and
     * 0xFFFF7FFF (-32769) gives 0x8000. */
    assert(neon_qshrn(0xFFFF7FFF00008000ull, 0, 0, 1, 0, 1, 0) == 0x80007FFFull);
    /* Truncating and rounding at 16 bits (size 1, shift 16). Lane 0x01234567
     * gives 0x0123 with bit 15 clear; lane 0x89ABCDEF gives 0x89AB, which rounds
     * to 0x89AC since bit 15 of 0xCDEF is set. */
    assert(neon_shrn(0x89ABCDEF01234567ull, 0, 16, 1, 0) == 0x89AB0123ull);
    assert(neon_shrn(0x89ABCDEF01234567ull, 0, 16, 1, 1) == 0x89AC0123ull);

    /* VQSHRUN and VQRSHRUN, 16 to 8, by 8. 0x0180 gives 1 without rounding and
     * 2 with it (bit 7 is set). 0x8000 and 0xFF00 are negative and give 0.
     * 0x7F00 gives 0x7F. */
    assert(neon_qshrn(0xFF007F0080000180ull, 0, 8, 0, 0, 1, 1) == 0x007F0001ull);
    assert(neon_qshrn(0xFF007F0080000180ull, 0, 8, 0, 1, 1, 1) == 0x007F0002ull);
    /* 32 to 16 (size 1): 0x00010000 saturates to 0xFFFF and -1 gives 0. */
    assert(neon_qshrn(0xFFFFFFFF00010000ull, 0, 0, 1, 0, 1, 1) == 0x0000FFFFull);

    /* 64-bit source, size 2, with the top bit set and rounding by 32. The
     * unsigned lo lane 0x8000000080000001 gives 0x80000001: bit 31 rounds up,
     * and the sum still fits. The all-ones hi lane rounds to 2^32 and
     * saturates. A naive x + (1 << (amt-1)) would wrap here, not saturate. */
    assert(neon_qshrn(0x8000000080000001ull, 0xFFFFFFFFFFFFFFFFull, 32, 2, 1, 0, 1) == 0xFFFFFFFF80000001ull);
    /* Signed, shift 1, rounding. INT64_MIN gives -2^62 and saturates to
     * 0x80000000; 2^63-1 rounds to 2^62 and saturates to 0x7FFFFFFF. */
    assert(neon_qshrn(0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, 1, 2, 1, 1, 0) == 0x7FFFFFFF80000000ull);
    /* VQSHRUN.S64 by 0: the negative lane gives 0; 2^32 saturates to 2^32-1. */
    assert(neon_qshrn(0x8000000000000000ull, 0x0000000100000000ull, 0, 2, 0, 1, 1) == 0xFFFFFFFF00000000ull);
    /* VRSHRN.I64 by 32 truncates, and a rounding carry out of the 32 bits
     * wraps: 0xFFFFFFFF + 1 is 0. */
    assert(neon_shrn(0xFFFFFFFFFFFFFFFFull, 0, 32, 2, 0) == 0x00000000FFFFFFFFull);
    assert(neon_shrn(0xFFFFFFFFFFFFFFFFull, 0, 32, 2, 1) == 0);

    printf("ok   neon narrowing: vshrn/vrshrn, vqmovn/vqmovun, vqshrn/vqrshrn, vqshrun/vqrshrun at 8, 16, 32, and 64-bit sources\n");
}

static void test_bit_ops(void) {
    assert(vita_rbit(1u) == 0x80000000u);
    assert(vita_rbit(0x12345678u) == 0x1E6A2C48u);
    assert(vita_rev16(0x11223344u) == 0x22114433u);
    assert(vita_revsh(0x000080FFu) == 0xFFFFFF80u);
    assert(vita_revsh(0x12340180u) == 0xFFFF8001u);
    assert(vita_uxtb16(0x12345678u) == 0x00340078u);
    assert(vita_sxtb16(0x0080007Fu) == 0xFF80007Fu);
    assert(vita_add16(0x0001FFFFu, 0x00010001u) == 0x00020000u);   /* no carry across */
    assert(vita_smulxy(0xFFFF0003u, 0x0002FFFEu, 0) == (uint32_t)-6);
    assert(vita_smulxy(0xFFFF0003u, 0x0002FFFEu, 3) == (uint32_t)-2);
    assert(vita_smulxy(0x80000000u, 0x80000000u, 3) == 0x40000000u);
    printf("ok   rbit, rev16, revsh, b16 extends, smulxy\n");
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
    test_shifter_carry();
    test_neon_lanes();
    test_neon_f32();
    test_neon_f32_b();
    test_neon_int_c();
    test_neon_cmp_c();
    test_neon_shl_reg_c();
    test_neon_widen();
    test_neon_shuffle();
    test_neon_ldst();
    test_neon_narrow();
    test_neon_cmpz_c();
    test_bit_ops();
    test_parallel_and_lanes();
    test_vfp_d32();
    test_mem_attach();
    test_import_handler();
    printf("all runtime tests passed\n");
    return 0;
}
