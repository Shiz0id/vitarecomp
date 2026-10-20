/* mem.c — a flat guest address space.
 *
 * One allocation and a subtraction. The Vita's MMU means there is no fixed map
 * to model; what matters for a recompiled module is that the addresses it
 * actually touches are backed and that the ones it should not touch are
 * noticed rather than silently absorbed.
 */

#include "vitarecomp/mem.h"

#include <stdlib.h>
#include <string.h>

uint32_t vita_mem_bad_access;

static uint8_t *g_mem;
static uint32_t g_base;
static size_t   g_size;
static int      g_owned;    /* 0 when attached: the memory is not ours to free */

int vita_mem_init(uint32_t base, size_t size) {
    vita_mem_free();
    g_mem = (uint8_t *)calloc(size, 1);
    if (!g_mem) return 1;
    g_base = base;
    g_size = size;
    g_owned = 1;
    return 0;
}

int vita_mem_attach(void *host, uint32_t base, size_t size) {
    vita_mem_free();
    if (!host || size == 0) return 1;
    g_mem = (uint8_t *)host;
    g_base = base;
    g_size = size;
    g_owned = 0;
    return 0;
}

void vita_mem_free(void) {
    if (g_owned) free(g_mem);
    g_owned = 0;
    g_mem = NULL;
    g_base = 0;
    g_size = 0;
}

void *vita_mem_ptr(uint32_t addr, uint32_t len) {
    if (!g_mem || addr < g_base) { vita_mem_bad_access++; return NULL; }
    uint64_t off = (uint64_t)addr - g_base;
    /* Reject a straddling access rather than reading past the allocation. */
    if (off + len > g_size) { vita_mem_bad_access++; return NULL; }
    return g_mem + off;
}

uint32_t vita_mem_avail(uint32_t addr) {
    if (!g_mem || addr < g_base) return 0;
    uint64_t off = (uint64_t)addr - g_base;
    if (off >= g_size) return 0;
    /* A full 4 GB window at base 0 leaves 2^32 bytes after address 0, which
     * does not fit the return type. Saturate rather than wrap to zero. */
    uint64_t left = (uint64_t)g_size - off;
    return left > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)left;
}

/* Unaligned access is legal on ARMv7 for ordinary loads and stores, so these
 * go through memcpy rather than a cast. A cast would be undefined behaviour on
 * an unaligned address and would fault outright on some hosts. */

uint32_t vita_read32(uint32_t a) {
    void *p = vita_mem_ptr(a, 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

uint16_t vita_read16(uint32_t a) {
    void *p = vita_mem_ptr(a, 2);
    uint16_t v = 0;
    if (p) memcpy(&v, p, 2);
    return v;
}

uint8_t vita_read8(uint32_t a) {
    void *p = vita_mem_ptr(a, 1);
    return p ? *(uint8_t *)p : 0;
}

void vita_write32(uint32_t a, uint32_t v) {
    void *p = vita_mem_ptr(a, 4);
    if (p) memcpy(p, &v, 4);
}

void vita_write16(uint32_t a, uint16_t v) {
    void *p = vita_mem_ptr(a, 2);
    if (p) memcpy(p, &v, 2);
}

void vita_write8(uint32_t a, uint8_t v) {
    void *p = vita_mem_ptr(a, 1);
    if (p) *(uint8_t *)p = v;
}
