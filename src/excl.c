/* excl.c — LDREX/STREX, the exclusive monitor, and barriers.
 *
 * The hardware monitor remembers an address after LDREX, and STREX stores only
 * if nothing else wrote there since. A recompiled program has no such monitor,
 * so this keeps one per guest thread — the address and the VALUE loaded — and
 * STREX becomes a compare-and-swap against that value. If another thread
 * changed the word in between, the swap fails and STREX reports failure, which
 * the guest's retry loop already handles.
 *
 * The one thing this cannot see is a write that puts the same value back (ABA).
 * Real exclusives would fail there; this succeeds. Every lock and atomic
 * counter built on LDREX/STREX is correct under that, since what it protects is
 * the value, and that is the universal approach among recompilers and
 * emulators that do not model the monitor per cache line.
 *
 * Single-threaded builds need no atomics, but keep the monitor: CLREX, a
 * mismatched address and a second STREX must still fail.
 */

#include "vitarecomp/cpu.h"
#include "vitarecomp/mem.h"

#include <string.h>

#if defined(VITARECOMP_THREADED) && defined(_MSC_VER)
#include <intrin.h>
#endif

static VITA_TLS uint32_t mon_addr;
static VITA_TLS uint64_t mon_val;
static VITA_TLS uint8_t  mon_size;   /* 0 when the monitor is open */

#if !defined(VITARECOMP_THREADED) || defined(_MSC_VER)
static uint64_t load_n(const void *p, uint32_t size) {
    switch (size) {
    case 1: { uint8_t  v; memcpy(&v, p, 1); return v; }
    case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
    case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
    default: { uint64_t v; memcpy(&v, p, 8); return v; }
    }
}
#endif

/* Store `v` at `p` if it still holds `expect`; returns 1 on success. `p` is
 * naturally aligned: exclusives on unaligned addresses fault on hardware. */
static int cas_n(void *p, uint32_t size, uint64_t expect, uint64_t v) {
#if defined(VITARECOMP_THREADED) && defined(_MSC_VER)
    switch (size) {
    case 1: return (uint8_t)_InterlockedCompareExchange8((volatile char *)p, (char)v, (char)expect) == (uint8_t)expect;
    case 2: return (uint16_t)_InterlockedCompareExchange16((volatile short *)p, (short)v, (short)expect) == (uint16_t)expect;
    case 4: return (uint32_t)_InterlockedCompareExchange((volatile long *)p, (long)v, (long)expect) == (uint32_t)expect;
    default: return (uint64_t)_InterlockedCompareExchange64((volatile long long *)p, (long long)v, (long long)expect) == expect;
    }
#elif defined(VITARECOMP_THREADED)
    switch (size) {
    case 1: { uint8_t  e = (uint8_t)expect;  return __atomic_compare_exchange_n((uint8_t *)p,  &e, (uint8_t)v,  0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    case 2: { uint16_t e = (uint16_t)expect; return __atomic_compare_exchange_n((uint16_t *)p, &e, (uint16_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    case 4: { uint32_t e = (uint32_t)expect; return __atomic_compare_exchange_n((uint32_t *)p, &e, (uint32_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    default: { uint64_t e = expect;          return __atomic_compare_exchange_n((uint64_t *)p, &e, v,           0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
    }
#else
    if (load_n(p, size) != expect) return 0;
    memcpy(p, &v, size);   /* little-endian host: the low `size` bytes */
    return 1;
#endif
}

uint64_t vita_ldrex(uint32_t addr, uint32_t size) {
    const void *p = vita_mem_ptr(addr, size);
    if (!p) { mon_size = 0; return 0; }
#if defined(VITARECOMP_THREADED) && !defined(_MSC_VER)
    uint64_t v;
    switch (size) {
    case 1: v = __atomic_load_n((const uint8_t *)p,  __ATOMIC_SEQ_CST); break;
    case 2: v = __atomic_load_n((const uint16_t *)p, __ATOMIC_SEQ_CST); break;
    case 4: v = __atomic_load_n((const uint32_t *)p, __ATOMIC_SEQ_CST); break;
    default: v = __atomic_load_n((const uint64_t *)p, __ATOMIC_SEQ_CST); break;
    }
#else
    uint64_t v = load_n(p, size);
#endif
    mon_addr = addr;
    mon_size = (uint8_t)size;
    mon_val  = v;
    return v;
}

uint32_t vita_strex(uint32_t addr, uint64_t v, uint32_t size) {
    const int open = mon_size == size && mon_addr == addr;
    mon_size = 0;                        /* any STREX clears the monitor */
    if (!open) return 1;
    void *p = vita_mem_ptr(addr, size);
    if (!p) return 1;
    return cas_n(p, size, mon_val, v) ? 0 : 1;
}

void vita_clrex(void) { mon_size = 0; }

void vita_barrier(void) {
#if defined(VITARECOMP_THREADED) && defined(_MSC_VER)
    _ReadWriteBarrier();
    MemoryBarrier();
#elif defined(VITARECOMP_THREADED)
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
