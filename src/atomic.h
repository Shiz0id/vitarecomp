/* atomic.h — the runtime's few shared counters, safe under VITARECOMP_THREADED.
 *
 * With one register file per host thread, guest threads really do run in
 * parallel, and anything the runtime keeps process-wide — dispatch counters,
 * the trace's seen-table, the heap cursor — is shared between them. A plain
 * `++` there is a data race: counts come out short, and the heap hands two
 * threads the same block.
 *
 * Single-threaded builds keep plain operations; the atomic forms cost a locked
 * instruction on the hottest path in the program (every indirect transfer).
 * Private to src/: nothing here is API.
 */

#ifndef VITARECOMP_ATOMIC_H
#define VITARECOMP_ATOMIC_H

#include <stdint.h>

#if defined(VITARECOMP_THREADED) && defined(_MSC_VER)
#include <intrin.h>
static inline void vita_atomic_inc(volatile uint32_t *p) {
    _InterlockedIncrement((volatile long *)p);
}
static inline uint32_t vita_atomic_load(volatile uint32_t *p) {
    return (uint32_t)_InterlockedCompareExchange((volatile long *)p, 0, 0);
}
/* Returns 1 if *p held `expected` and now holds `desired`. */
static inline int vita_atomic_cas(volatile uint32_t *p, uint32_t expected, uint32_t desired) {
    return (uint32_t)_InterlockedCompareExchange((volatile long *)p, (long)desired, (long)expected) == expected;
}
#elif defined(VITARECOMP_THREADED)
static inline void vita_atomic_inc(volatile uint32_t *p) {
    __atomic_fetch_add(p, 1u, __ATOMIC_RELAXED);
}
static inline uint32_t vita_atomic_load(volatile uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static inline int vita_atomic_cas(volatile uint32_t *p, uint32_t expected, uint32_t desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
#else
static inline void vita_atomic_inc(volatile uint32_t *p) { (*p)++; }
static inline uint32_t vita_atomic_load(volatile uint32_t *p) { return *p; }
static inline int vita_atomic_cas(volatile uint32_t *p, uint32_t expected, uint32_t desired) {
    if (*p != expected) return 0;
    *p = desired;
    return 1;
}
#endif

#endif /* VITARECOMP_ATOMIC_H */
