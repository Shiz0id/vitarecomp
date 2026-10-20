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

#include "recomp_cpu.h"

#include <cpu/functions.h>
#include <cpu/state.h>

#include <vitarecomp/cpu.h>
#include <vitarecomp/dispatch.h>
#include <vitarecomp/vfp.h>

#include <cstring>

#ifndef VITARECOMP_THREADED
#error "vita3k-bridge needs VITARECOMP_THREADED: Vita3K runs each guest thread on its own host thread"
#endif

// The low 8 FPSCR/CPSR bits that are not NZCV stay in ctx; NZCV live in the
// runtime's separate flag ints while guest code runs.
static constexpr uint32_t NZCV = 0xF0000000u;

static uint32_t pack_nzcv(int n, int z, int c, int v) {
    return (uint32_t(n != 0) << 31) | (uint32_t(z != 0) << 30) | (uint32_t(c != 0) << 29) | (uint32_t(v != 0) << 28);
}

RecompCPU::RecompCPU(CPUState *state, std::size_t processor_id)
    : state(state)
    , core(processor_id) {
    ctx.cpsr = 0x20; // Thumb: every Vita entry point this toolkit has met is Thumb
}

bool RecompCPU::live() const {
    return depth > 0 && std::this_thread::get_id() == owner;
}

// --- moving state between the parked context and the live globals ----------

void RecompCPU::globals_from(const CPUContext &c) {
    uint32_t *const r[13] = { &r0, &r1, &r2, &r3, &r4, &r5, &r6, &r7, &r8, &r9, &r10, &r11, &r12 };
    for (int i = 0; i < 13; i++)
        *r[i] = c.cpu_registers[i];
    sp = c.cpu_registers[13];
    lr = c.cpu_registers[14];
    flag_n = (c.cpsr >> 31) & 1;
    flag_z = (c.cpsr >> 30) & 1;
    flag_c = (c.cpsr >> 29) & 1;
    flag_v = (c.cpsr >> 28) & 1;
    // Vita3K's 64 floats and the runtime's 64 words are the same D0-D31 file.
    static_assert(sizeof(c.fpu_registers) == sizeof(vfp_reg) * VFP_WORDS);
    std::memcpy(vfp_s, c.fpu_registers.data(), sizeof(c.fpu_registers));
    fpscr_n = (c.fpscr >> 31) & 1;
    fpscr_z = (c.fpscr >> 30) & 1;
    fpscr_c = (c.fpscr >> 29) & 1;
    fpscr_v = (c.fpscr >> 28) & 1;
}

void RecompCPU::globals_to(CPUContext &c) const {
    const uint32_t r[13] = { r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12 };
    for (int i = 0; i < 13; i++)
        c.cpu_registers[i] = r[i];
    c.cpu_registers[13] = sp;
    c.cpu_registers[14] = lr;
    c.cpsr = (c.cpsr & ~NZCV) | pack_nzcv(flag_n, flag_z, flag_c, flag_v);
    std::memcpy(c.fpu_registers.data(), vfp_s, sizeof(c.fpu_registers));
    c.fpscr = (c.fpscr & ~NZCV) | pack_nzcv(fpscr_n, fpscr_z, fpscr_c, fpscr_v);
}

// --- running ---------------------------------------------------------------

int RecompCPU::run() {
    const bool nested = live();
    if (!nested) {
        owner = std::this_thread::get_id();
        globals_from(ctx);
    }

    std::jmp_buf env;
    unwind.push_back(&env);
    ++depth;
    CPUState *const prev = get_current_cpu_state();
    set_current_cpu_state(state);

    // setjmp, not an exception: the frames between here and the import
    // handler are generated C, which is not built to unwind C++ exceptions.
    // The handler only jumps after call_import has returned, so no C++ frame
    // with a destructor is skipped.
    if (setjmp(env) == 0) {
        const uint32_t pc = ctx.cpu_registers[15] | (ctx.thumb() ? 1u : 0u);
        vita_dispatch(0, pc);
    }

    set_current_cpu_state(prev);
    --depth;
    unwind.pop_back();
    if (!nested)
        globals_to(ctx);

    // A stop ends the outermost level only once every nested level is gone.
    if (depth == 0)
        stop_requested = false;

    // 1 is Vita3K's "the guest function returned" (the halt sentinel). A
    // recompiled function that returns has, by construction, done exactly that.
    return 1;
}

void RecompCPU::stop() {
    // Honoured at the next import call. Recompiled code has no instruction
    // boundary to interrupt at, but a thread being stopped by the kernel is,
    // almost always, inside a kernel call already.
    stop_requested = true;
}

void RecompCPU::unwind_if_stopped() {
    if (stop_requested.load() && !unwind.empty())
        std::longjmp(*unwind.back(), 1);
}

// --- registers -------------------------------------------------------------

uint32_t RecompCPU::get_reg(uint8_t idx) {
    if (idx == 15)
        return ctx.cpu_registers[15];
    if (!live())
        return ctx.cpu_registers[idx];
    switch (idx) {
    case 0: return r0;
    case 1: return r1;
    case 2: return r2;
    case 3: return r3;
    case 4: return r4;
    case 5: return r5;
    case 6: return r6;
    case 7: return r7;
    case 8: return r8;
    case 9: return r9;
    case 10: return r10;
    case 11: return r11;
    case 12: return r12;
    case 13: return sp;
    case 14: return lr;
    default: return 0;
    }
}

void RecompCPU::set_reg(uint8_t idx, uint32_t val) {
    if (idx == 15) {
        ctx.set_pc(val);
        return;
    }
    if (!live()) {
        ctx.cpu_registers[idx] = val;
        return;
    }
    switch (idx) {
    case 0: r0 = val; break;
    case 1: r1 = val; break;
    case 2: r2 = val; break;
    case 3: r3 = val; break;
    case 4: r4 = val; break;
    case 5: r5 = val; break;
    case 6: r6 = val; break;
    case 7: r7 = val; break;
    case 8: r8 = val; break;
    case 9: r9 = val; break;
    case 10: r10 = val; break;
    case 11: r11 = val; break;
    case 12: r12 = val; break;
    case 13: sp = val; break;
    case 14: lr = val; break;
    default: break;
    }
}

uint32_t RecompCPU::get_cpsr() {
    if (!live())
        return ctx.cpsr;
    return (ctx.cpsr & ~NZCV) | pack_nzcv(flag_n, flag_z, flag_c, flag_v);
}

void RecompCPU::set_cpsr(uint32_t val) {
    ctx.cpsr = val;
    if (live()) {
        flag_n = (val >> 31) & 1;
        flag_z = (val >> 30) & 1;
        flag_c = (val >> 29) & 1;
        flag_v = (val >> 28) & 1;
    }
}

float RecompCPU::get_float_reg(uint8_t idx) {
    return live() ? vfp_s[idx & 63].f : ctx.fpu_registers[idx & 63];
}

void RecompCPU::set_float_reg(uint8_t idx, float val) {
    if (live())
        vfp_s[idx & 63].f = val;
    else
        ctx.fpu_registers[idx & 63] = val;
}

uint32_t RecompCPU::get_fpscr() {
    if (!live())
        return ctx.fpscr;
    return (ctx.fpscr & ~NZCV) | pack_nzcv(fpscr_n, fpscr_z, fpscr_c, fpscr_v);
}

void RecompCPU::set_fpscr(uint32_t val) {
    ctx.fpscr = val;
    if (live()) {
        fpscr_n = (val >> 31) & 1;
        fpscr_z = (val >> 30) & 1;
        fpscr_c = (val >> 29) & 1;
        fpscr_v = (val >> 28) & 1;
    }
}

CPUContext RecompCPU::save_context() {
    CPUContext c = ctx;
    if (live())
        globals_to(c);
    return c;
}

void RecompCPU::load_context(const CPUContext &c) {
    ctx = c;
    if (live())
        globals_from(c);
}
