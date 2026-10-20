// vita3krecomp — statically recompiled code as a Vita3K CPU backend.
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

// Vita3K runs every guest thread as a loop around run(cpu): start at PC, stop
// when the guest function returns to the halt address or makes a system call.
// The CPU behind that loop is an interface, and its usual implementation is
// dynarmic, a JIT. This is the other implementation: run() looks the PC up in
// the recompiled dispatch table and calls the C function generated from it.
//
// System calls never stop the CPU here. The generated code calls each import
// as a C function, the import handler (bridge.cpp) forwards it to Vita3K's
// call_import on the same host thread, and execution continues in C when it
// returns. That is the same thread Vita3K would have serviced the SVC on, so
// blocking calls block the right thread.
//
// REGISTER STATE. The generated code reads registers as thread-local globals
// (VITARECOMP_THREADED). While guest code runs on this CPU's thread, those
// globals ARE the register file, and the accessors read them live. Otherwise
// the state is parked in `ctx`. The kernel asks for registers from other
// threads too (debugger, thread status), and those reads see `ctx`.

#include <cpu/common.h>
#include <cpu/impl/interface.h>

#include <vitarecomp/mem.h>

#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <thread>
#include <vector>

struct CPUState;

class RecompCPU final : public CPUInterface {
public:
    RecompCPU(CPUState *state, std::size_t processor_id);

    int run() override;
    int step() override { return run(); }  // no instruction granularity: a step is a call
    void stop() override;

    uint32_t get_reg(uint8_t idx) override;
    void set_reg(uint8_t idx, uint32_t val) override;
    uint32_t get_sp() override { return get_reg(13); }
    void set_sp(uint32_t val) override { set_reg(13, val); }
    uint32_t get_pc() override { return ctx.cpu_registers[15]; }
    void set_pc(uint32_t val) override { ctx.set_pc(val); }
    uint32_t get_lr() override { return get_reg(14); }
    void set_lr(uint32_t val) override { set_reg(14, val); }

    uint32_t get_cpsr() override;
    void set_cpsr(uint32_t val) override;
    uint32_t get_tpidruro() override { return tpidruro; }
    void set_tpidruro(uint32_t val) override { tpidruro = val; }
    uint32_t get_tpidrurw() override { return tpidrurw; }
    void set_tpidrurw(uint32_t val) override { tpidrurw = val; }

    float get_float_reg(uint8_t idx) override;
    void set_float_reg(uint8_t idx, float val) override;
    uint32_t get_fpscr() override;
    void set_fpscr(uint32_t val) override;

    CPUContext save_context() override;
    void load_context(const CPUContext &c) override;
    void invalidate_jit_cache(Address, size_t) override {} // nothing is generated at run time

    bool is_thumb_mode() override { return ctx.thumb(); }

    bool hit_breakpoint() override { return false; }
    void trigger_breakpoint() override {}
    void set_log_code(bool) override {}
    void set_log_mem(bool) override {}
    bool get_log_code() override { return false; }
    bool get_log_mem() override { return false; }

    // The monitor is the runtime's, one per host thread (src/excl.c). Vita3K
    // clears it after every kernel call, as a real exception return does; it
    // does so on the thread that owns it, so the thread-local one is the
    // right one to clear.
    void clear_exclusive() override { vita_clrex(); }

    std::size_t processor_id() const override { return core; }

    // Called by the import handler after every serviced call. If the kernel
    // asked this CPU to stop (thread exit, delete, shutdown), unwind to run()
    // instead of returning into guest code that must not continue.
    void unwind_if_stopped();

private:
    // True when the thread-local globals hold this CPU's live registers.
    bool live() const;
    void globals_from(const CPUContext &c);
    void globals_to(CPUContext &c) const;

    CPUState *state;
    std::size_t core;
    CPUContext ctx;
    uint32_t tpidruro = 0;
    uint32_t tpidrurw = 0;

    std::thread::id owner;
    int depth = 0;                  // nested run() levels active on `owner`
    // Set by stop(), which the kernel may call from another host thread (a
    // thread deleted while it waits), so it is atomic.
    std::atomic<bool> stop_requested{ false };
    std::vector<std::jmp_buf *> unwind; // one per active run() level
};
