// vita3krecomp — wiring recompiled code into Vita3K.
// Copyright (C) 2026 vita3krecomp contributors
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#pragma once

struct EmuEnvState;

// Connect the vitarecomp runtime to a Vita3K instance:
//
//   - guest memory: the runtime reads and writes Vita3K's own 4 GB reservation,
//     so a pointer either side hands out is valid on the other;
//   - the CPU: every thread the kernel creates runs recompiled code through
//     RecompCPU instead of a dynarmic JIT;
//   - imports: a generated import stub with no hand-written vita_hle_* body is
//     serviced by Vita3K's HLE module for that NID. Only a NID Vita3K has no
//     implementation for still traps by name.
//
// Call after Vita3K's memory is initialised and before the kernel creates its
// first thread, with the recompiled dispatch table already registered through
// vita_dispatch_init. Returns false, having changed nothing, if the memory
// cannot be shared (Vita3K's page-table mode remaps addresses per page, which
// a flat window cannot follow).
bool vita3k_bridge_install(EmuEnvState &emuenv);
