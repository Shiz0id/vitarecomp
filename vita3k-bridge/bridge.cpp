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

#include "bridge.h"
#include "recomp_cpu.h"

#include <cpu/functions.h>
#include <cpu/state.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <modules/module_parent.h>
#include <util/log.h>

#include <vitarecomp/mem.h>
#include <vitarecomp/recomp_rt.h>

#include <memory>

static EmuEnvState *bridge_env;

// The generated default for an import, offered to Vita3K before it traps.
//
// Only NIDs Vita3K actually implements are claimed. call_import would also
// accept an unknown one, log it, and return 0 to the guest; declining instead
// lets the runtime's trap name the function, which is the more useful result
// while bringing a title up.
static int service_import(uint32_t stub, uint32_t nid, const char *name) {
    CPUState *const cpu = get_current_cpu_state();
    if (!cpu || !bridge_env || !has_hle_implementation(nid))
        return 0;

    // Recompiled code has no PC. Park the stub address in it for the duration,
    // which is what an HLE function that logs or inspects PC would have seen
    // on the JIT, and notice one that tries to redirect execution. The parked
    // value is restored afterwards: set_pc also rewrites the Thumb bit, and
    // the stub's instruction set is not the caller's.
    const uint32_t saved_pc = read_pc(*cpu) | (is_thumb_mode(*cpu) ? 1u : 0u);
    write_pc(*cpu, stub);
    call_import(*bridge_env, *cpu, nid, cpu->thread_id);
    const uint32_t after_pc = read_pc(*cpu);
    if ((after_pc & ~1u) != (stub & ~3u))
        LOG_ERROR("vita3krecomp: {} (NID {}) redirected PC to {}; recompiled code cannot follow a PC write and continued past it",
            name ? name : "import", log_hex(nid), log_hex(after_pc));
    write_pc(*cpu, saved_pc);

    // An import can end its own thread — sceKernelExitProcess, sceKernelExitThread,
    // sceKernelExitDeleteThread. The JIT notices at its run loop; here the import returns into
    // guest code, so leave instead of running on past an exit.
    RecompCPU &rc = static_cast<RecompCPU &>(*cpu->cpu);
    if (const ThreadStatePtr thread = bridge_env->kernel.get_thread(cpu->thread_id);
        thread && thread->ending_requested())
        rc.stop();
    rc.unwind_if_stopped();
    return 1;
}

bool vita3k_bridge_install(EmuEnvState &emuenv) {
    MemState &mem = emuenv.mem;
    if (!mem.memory || mem.use_page_table) {
        LOG_ERROR("vita3krecomp: guest memory {}; the recompiled runtime needs Vita3K's flat 4 GB reservation",
            mem.memory ? "is in page-table mode" : "is not initialised");
        return false;
    }

    // Base 0, the whole 32-bit space: guest address == offset into Vita3K's block.
    if (vita_mem_attach(mem.memory.get(), 0, size_t(1) << 32) != 0)
        return false;

    bridge_env = &emuenv;
    vita_set_import_handler(service_import);
    set_cpu_backend_factory([](CPUState *state, std::size_t processor_id) -> CPUInterfacePtr {
        return std::make_unique<RecompCPU>(state, processor_id);
    });
    return true;
}
