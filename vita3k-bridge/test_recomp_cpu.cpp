// vita3krecomp — RecompCPU driven the way Vita3K's thread loop drives a CPU.
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
//
// Needs Vita3K's headers but none of its libraries: the two cpu.cpp functions
// RecompCPU calls are defined here, and the "recompiled module" is a handful
// of hand-written functions in the shape armrecomp emits. Build with
// VITARECOMP_THREADED and link the vitarecomp runtime.

#include "recomp_cpu.h"

#include <cpu/state.h>

#include <vitarecomp/cpu.h>
#include <vitarecomp/dispatch.h>
#include <vitarecomp/hle.h>
#include <vitarecomp/mem.h>
#include <vitarecomp/recomp_rt.h>
#include <vitarecomp/vfp.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#ifdef NDEBUG
#error "tests must be built with asserts enabled"
#endif

// --- the two cpu.cpp functions RecompCPU uses ---------------------------------

static thread_local CPUState *current_cpu_state = nullptr;
void set_current_cpu_state(CPUState *state) { current_cpu_state = state; }
CPUState *get_current_cpu_state() { return current_cpu_state; }

// --- a fake recompiled module --------------------------------------------------

static RecompCPU *cpu_of_this_thread() {
    return static_cast<RecompCPU *>(get_current_cpu_state()->cpu.get());
}

// r0 = r0 + r1, and set Z: the simplest function with a result and a flag.
extern "C" void vita_func_81000000(void) {
    r0 = r0 + r1;
    flag_z = (r0 == 0);
}

// Calls an "import" that inspects registers through the CPU interface, the way
// a Vita3K HLE function reads its arguments, and writes its result back.
static uint32_t g_seen_arg;
extern "C" void vita_func_81000100(void) {
    r0 = 41;
    RecompCPU *cpu = cpu_of_this_thread();
    g_seen_arg = cpu->get_reg(0);   // live read
    cpu->set_reg(0, g_seen_arg + 1); // live write: the "return value"
    vfp_setd(17, 2.5);               // D17 must survive into the saved context
}

// An import that runs a guest callback before returning, as run_callback does.
extern "C" void vita_func_81000200(void) {
    r4 = 0xAAAA;
    RecompCPU *cpu = cpu_of_this_thread();
    const CPUContext saved = cpu->save_context();
    cpu->set_pc(0x81000001);         // the adder, Thumb
    cpu->set_reg(0, 5);
    cpu->set_reg(1, 6);
    assert(cpu->run() == 1);         // nested level
    const uint32_t cb_result = cpu->get_reg(0);
    cpu->load_context(saved);
    assert(r4 == 0xAAAA);            // the caller's state came back
    r0 = cb_result;
}

// Guest code that must not continue past a kernel call that stopped it.
static int g_ran_past_stop;
extern "C" void vita_func_81000300(void) {
    RecompCPU *cpu = cpu_of_this_thread();
    cpu->stop();                     // e.g. sceKernelExitThread
    cpu->unwind_if_stopped();        // what the import handler does after any call
    g_ran_past_stop = 1;
}

static const vita_dispatch_entry table[] = {
    { 0x81000000u, vita_func_81000000 },
    { 0x81000100u, vita_func_81000100 },
    { 0x81000200u, vita_func_81000200 },
    { 0x81000300u, vita_func_81000300 },
};

// --- the tests -----------------------------------------------------------------

struct Thread {
    CPUState state;
    RecompCPU *cpu;
    Thread() {
        state.cpu = std::make_unique<RecompCPU>(&state, 0);
        cpu = static_cast<RecompCPU *>(state.cpu.get());
    }
};

static void test_run_and_result() {
    Thread t;
    t.cpu->set_pc(0x81000001);
    t.cpu->set_reg(0, 2);
    t.cpu->set_reg(1, 3);
    t.cpu->set_reg(13, 0x81800000);
    assert(t.cpu->run() == 1);
    // After run() the state is parked: read without the live globals.
    assert(t.cpu->get_reg(0) == 5);
    assert(t.cpu->get_sp() == 0x81800000);
    assert((t.cpu->get_cpsr() & 0x40000000u) == 0); // Z clear
    std::printf("  run, result parked in context ok\n");
}

static void test_live_access() {
    Thread t;
    t.cpu->set_pc(0x81000101);
    t.cpu->run();
    assert(g_seen_arg == 41);
    assert(t.cpu->get_reg(0) == 42);
    // D17 lives at float slots 34-35 in Vita3K's context.
    const CPUContext c = t.cpu->save_context();
    double d;
    std::memcpy(&d, &c.fpu_registers[34], sizeof(d));
    assert(d == 2.5);
    std::printf("  live registers and D17        ok\n");
}

static void test_nested_callback() {
    Thread t;
    t.cpu->set_pc(0x81000201);
    t.cpu->run();
    assert(t.cpu->get_reg(0) == 11);
    std::printf("  nested callback restores      ok\n");
}

static void test_stop_unwinds() {
    Thread t;
    t.cpu->set_pc(0x81000301);
    assert(t.cpu->run() == 1);
    assert(g_ran_past_stop == 0);
    // The stop is spent: the next run is a normal one.
    t.cpu->set_pc(0x81000001);
    t.cpu->set_reg(0, 1);
    t.cpu->set_reg(1, 1);
    t.cpu->run();
    assert(t.cpu->get_reg(0) == 2);
    std::printf("  stop unwinds, then clears     ok\n");
}

static void test_threads_independent() {
    // Two guest threads on two host threads, each with its own register file.
    uint32_t results[2] = {};
    std::thread a([&] {
        Thread t;
        t.cpu->set_pc(0x81000001);
        t.cpu->set_reg(0, 100);
        t.cpu->set_reg(1, 1);
        t.cpu->run();
        results[0] = t.cpu->get_reg(0);
    });
    std::thread b([&] {
        Thread t;
        t.cpu->set_pc(0x81000001);
        t.cpu->set_reg(0, 200);
        t.cpu->set_reg(1, 2);
        t.cpu->run();
        results[1] = t.cpu->get_reg(0);
    });
    a.join();
    b.join();
    assert(results[0] == 101 && results[1] == 202);
    std::printf("  per-thread register files     ok\n");
}

// SceLibc's malloc, hand-written in the runtime, called from two guest threads
// at once. Every block must be distinct: a racy cursor hands both the same one.
extern "C" void vita_hle_malloc(void);

static void test_heap_concurrent() {
    static uint8_t backing[1 << 20];
    assert(vita_mem_attach(backing, 0x82000000u, sizeof(backing)) == 0);
    vita_heap_init(0x82000000u, sizeof(backing));
    constexpr int N = 2000;
    std::vector<uint32_t> got[2];
    auto worker = [&](int w) {
        for (int i = 0; i < N; i++) {
            r0 = 16;
            vita_hle_malloc();
            got[w].push_back(r0);
        }
    };
    std::thread a(worker, 0), b(worker, 1);
    a.join();
    b.join();
    std::set<uint32_t> all(got[0].begin(), got[0].end());
    all.insert(got[1].begin(), got[1].end());
    assert(all.size() == 2 * N && !all.count(0));
    vita_mem_free();
    std::printf("  heap, concurrent malloc       ok\n");
}

int main() {
    std::printf("recomp_cpu:\n");
    vita_dispatch_init(table, sizeof(table) / sizeof(table[0]));
    test_run_and_result();
    test_live_access();
    test_nested_callback();
    test_stop_unwinds();
    test_threads_independent();
    test_heap_concurrent();
    std::printf("all recomp_cpu tests passed\n");
    return 0;
}
