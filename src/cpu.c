/* cpu.c — the machine state the generated C operates on. */

#include "vitarecomp/cpu.h"

VITA_TLS uint32_t r0, r1, r2, r3, r4, r5, r6, r7;
VITA_TLS uint32_t r8, r9, r10, r11, r12;
VITA_TLS uint32_t sp, lr;

VITA_TLS int flag_n, flag_z, flag_c, flag_v;
VITA_TLS uint32_t flag_ge;
