# The Vita3K bridge — recompiled code as Vita3K's CPU

The toolkit's largest gaps are all on the firmware side: kernel threads and
sync, `sceGxm` and its shader compiler, audio, input, file I/O, save data.
Vita3K has years of work in every one of them. The bridge lets a recompiled
module run **inside Vita3K, with the JIT replaced by the generated C**, so all
of that HLE serves the recompiled game as it is.

It lives in [`vita3k-bridge/`](../vita3k-bridge) and is **GPLv2**, because it
links Vita3K. The toolkit (`tools/`, `src/`, `include/`) stays MIT and contains
none of it; see [`third_party/VITA3K.md`](../third_party/VITA3K.md).

## How it fits

Vita3K runs every guest thread as a loop around `run(cpu)`: start at PC, stop
when the guest function returns or makes a system call. The CPU behind that
loop is an interface (`CPUInterface`) whose usual implementation is dynarmic.

```
              Vita3K                                    vitarecomp
  ┌───────────────────────────────┐
  │ kernel: threads, sync, memory │  run(cpu)   ┌──────────────────────────┐
  │ thread loop ──────────────────┼────────────►│ RecompCPU::run           │
  │                               │             │   vita_dispatch(pc)      │
  │ HLE modules (by NID):         │             │   → vita_func_<addr>()   │
  │   SceGxm, SceCtrl, SceAudio,  │◄────────────┤   import stub →          │
  │   SceIo, SceAppUtil, ...      │ call_import │   vita_trap_import →     │
  │                               │             │   import handler         │
  │ 4 GB guest reservation ◄──────┼─────────────┤ vita_mem_attach(base 0)  │
  └───────────────────────────────┘             └──────────────────────────┘
```

Three seams, each small:

| Seam | Where | What it does |
|---|---|---|
| CPU backend | `RecompCPU` (`vita3k-bridge/recomp_cpu.*`), installed with `set_cpu_backend_factory` | `run()` looks PC up in the dispatch table and calls the generated function. A function that returns is a guest function that returned, which is Vita3K's halt result |
| Imports | `vita_set_import_handler` (toolkit), `service_import` (bridge) | A generated import stub with no hand-written `vita_hle_*` body is forwarded to Vita3K's `call_import` on the same host thread |
| Memory | `vita_mem_attach` (toolkit) | The runtime reads and writes Vita3K's own 4 GB block at base 0. A pointer either side hands out is valid on the other, with nothing copied |

Three Vita3K-side patches, all marked `vita3krecomp:` in the vendored tree:

- `cpu/src/cpu.cpp` and `cpu/include/cpu/functions.h` gain `set_cpu_backend_factory`;
- `vita3k/CMakeLists.txt` gains `-DVITA3K_RECOMP=ON` and
  `-DVITA3K_RECOMP_GENERATED=<recomp_funcs.c>`. The import defaults are taken
  from `<recomp_funcs.c>_imports/` beside it. `interface.cpp` calls
  `vita_register_functions()` and `vita3k_bridge_install()` just before the
  kernel is created;
- `kernel/thread/thread_state.h` gains a public `ending_requested()`, so the
  bridge can see that a thread was told to end while it was inside an import.
  `sceKernelExitProcess` is an import that returns. Without this check the
  generated code carried on into `__libc_fini_array` after the process had
  exited.

## Who answers an import

In order:

1. **A hand-written `vita_hle_*` in the toolkit.** Linked in place of the
   generated default, so it never reaches the handler. This matters for
   `SceLibc`: 333 of Vita3K's 352 `SceLibc` exports are `UNIMPLEMENTED()`
   stubs, because Vita3K normally runs the game's own `libc.suprx` through the
   JIT, which a recompiled game cannot do.
2. **Vita3K's HLE module for that NID**, through the handler.
3. **The named trap**, for a NID Vita3K has no entry for at all.

Vita3K's `UNIMPLEMENTED()` stubs count as implemented in step 2: the call is
logged by name and returns an error to the game, which is what Vita3K itself
does. If that hides a missing piece, write the function in the toolkit and it
takes precedence.

## Registers and threads

The generated C names registers as globals. The bridge requires
`VITARECOMP_THREADED`, which makes them thread-local, because Vita3K gives every
guest thread its own host thread. While guest code runs, those globals are the
register file and `RecompCPU` reads them live. Otherwise the state is parked in
a `CPUContext`.

Callbacks (`run_callback`) nest naturally: the HLE call is already on the C
stack, and a nested `run()` is another call. A thread the kernel stops, through
exit, delete or shutdown, is unwound with `longjmp` at its next import. Only
generated C frames are skipped, since the jump happens after `call_import` has
returned.

The runtime's shared state (dispatch counters, the trace table, the bump
heap's cursor) is atomic under `VITARECOMP_THREADED`. ThreadSanitizer found
the dispatch counters and the heap racing.

## First run: bridgetest

[`tests/vita/bridgetest`](../tests/vita/bridgetest) is a small VitaSDK homebrew
built to exercise the bridge. It covers:

- 10 self-checks: integer recursion, VFP arithmetic, a timed delay, a semaphore,
  two worker threads summing into shared state and the exit status they return,
  and a framebuffer allocated and shown through `SceDisplay` for 30 vblanks;
- a report written through `SceIo` and read back;
- an exit through `sceKernelExitProcess`.

Run through the full Vita3K build with the bridge, its output matches stock
Vita3K (dynarmic) line for line:

```
bridgetest: PASS ... (10 of 10)
bridgetest: done, 0 failed
bridgetest: report read back 63 bytes, starts "start
The app exited with code 0
```

There were no vitarecomp traps. The same result holds under both working memory
mappings below.

Recompiled coverage of the module is 98.59%. What is left is:

- the filler bodies of import stubs, which are never executed (the stub wins
  in dispatch);
- data at the tail of `.text` that the linear sweep decodes as code;
- one `udf`;
- `switch` defaults that are unreachable by construction.

Getting there took six fixes, all of them silent miscompiles that the unit
tests had not caught. They are listed in
[`LESSONS.md`](LESSONS.md#a-real-emulator-is-a-better-test-than-a-test).

## VitaSDK samples

Five of the six VitaSDK samples tried run recompiled with output identical to stock
Vita3K. The json sample stops where stock does, in a firmware library Vita3K
does not implement. The table and the four miscompiles they found are in
[`TRANSLATION.md`](TRANSLATION.md#vitasdk-samples).

They also found an upstream race. debugScreen homebrew frees its
framebuffer's memblock on exit, and the renderer thread could still be
copying the last frame out of it: a SIGSEGV in `render_frame`'s `memcpy`. The
window is small. Stock Vita3K hits it too once a debugger slows it down. It
is fixed in the vendored tree:
- `ksceKernelFreeMemBlock` clears any displayed or predicted frame inside
  the freed block, under `display_info_mutex`.
- Both renderers hold that mutex across the raw copy and re-check the base.

## Memory mapping and the GPU

Vita3K's Vulkan renderer offers several ways to map guest memory, set in
`config.yml` as `memory-mapping:`. The bridge needs guest memory to be one
flat block, because generated code turns a guest address into
`base + addr`. Here is how each mode behaves:

| Mode | With the bridge |
|---|---|
| `double-buffer` (default) | Works. Tested with bridgetest |
| `external-host` | Works. Tested with bridgetest. **Recommended** |
| `page-table` (and `native-buffer`, Android only) | Refused at start with "guest memory is in page-table mode" |

**Use `external-host`.** Vulkan imports Vita3K's guest block as host memory
(`VK_EXT_external_memory_host`), so a buffer the recompiled game writes is
already in memory the GPU can read. Nothing is copied between the game and the
renderer. That is the zero-copy path, and it costs nothing on the
recompiled side: the generated code still writes to the same flat block.
`double-buffer` allocates a separate Vulkan buffer for each mapped range and
copies between it and guest memory. Use it only when the driver lacks the
extension.

Giving the game real GPU allocations (device-local VRAM in place of guest
memory) is a renderer change, not a CPU-side one. If it is ever done, it
belongs in Vita3K's `gxm`/renderer layer, and the bridge does not need to
change.

## Known limits

Each limit below is stated where it bites, rather than left to be found:

- **Built and run here:**
  - the toolkit (ctest, both threading modes);
  - `RecompCPU`, against the real Vita3K headers, clean under ASan, UBSan and
    TSan (`vita3k-bridge/check.sh`);
  - **the full Vita3K executable with `VITA3K_RECOMP=ON`, running bridgetest
    and the VitaSDK samples** (above).

  No commercial title has been run through it yet.
- **Flat memory only.** See the mapping table above.
- **HLE modules only.** A module Vita3K loads as LLE (real firmware or an
  app-bundled `.suprx`) is guest code nobody recompiled, and jumping into it is
  a dispatch miss. Each such module must be recompiled too, or HLE'd.
- **The module must load where it was recompiled.** That holds for
  `ET_SCE_EXEC`, which is fixed at `0x81000000` and covers the whole launch
  window. A relocatable module that Vita3K places elsewhere would need
  relocation-aware emit.
- **A stop takes effect at the next import**, since recompiled code has no
  instruction boundary to interrupt. A thread spinning with no kernel calls
  cannot be stopped.
- **An HLE call that rewrites PC is not followed.** It is logged by name; the
  generated code continues past the call.
- **`BL` does not set `lr`** in the generated code. Returns do not need it, but
  an HLE function that inspects LR sees a stale value.
- **`errno` is one slot for the process**, in the toolkit's `SceLibc`.
- **Exclusives are Thumb-only so far.** Thumb `LDREX`/`STREX` (all four sizes),
  `CLREX` and `DMB`/`DSB`/`ISB` are translated (`src/excl.c`). The monitor
  compares and swaps the value, so it misses an ABA change, which a spinlock
  never depends on. The A32 encodings still trap.

## Using it

The recipe below is the one that was run, on Ubuntu 24.04 x86-64. The pins are
the ones that worked: Vita3K needs C++23 `<expected>`/`<ranges>` from
libstdc++ 14, and Qt 6 with Multimedia and ShaderTools.

```sh
# 0. dependencies
apt-get install clang-19 clang-tools-19 lld-19 libstdc++-14-dev g++-14 ninja-build \
    libgtk-3-dev xdg-desktop-portal libx11-dev libxext-dev libxrandr-dev \
    libxcursor-dev libxi-dev libxss-dev libxkbcommon-dev libwayland-dev \
    libdbus-1-dev libpulse-dev libasound2-dev libgl-dev libegl-dev \
    libvulkan-dev libudev-dev
pip install aqtinstall
aqt install-qt linux desktop 6.11.3 linux_gcc_64 -m qtmultimedia qtshadertools -O /opt/qt
# Vita3K's external/ submodules, at the commits pinned in third_party/VITA3K.md

# 1. recompile the module (toolkit, MIT). Writes recomp_funcs.c and
#    recomp_funcs.c_imports/; 0 = every function
armrecomp emit eboot.velf gen/recomp_funcs.c 0 <nid db: vita-headers db/360 or nid_db_classic.yml>

# 2. build Vita3K with the bridge (GPLv2)
cmake -S third_party/Vita3K -B build-v3k -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
    -DCMAKE_CXX_COMPILER_CLANG_SCAN_DEPS=/usr/bin/clang-scan-deps-19 \
    -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld \
    -DUSE_LTO=NEVER -DUSE_DISCORD_RICH_PRESENCE=OFF \
    -DCMAKE_PREFIX_PATH=/opt/qt/6.11.3/gcc_64 \
    -DVITA3K_RECOMP=ON -DVITA3K_RECOMP_GENERATED=$PWD/gen/recomp_funcs.c
cmake --build build-v3k

# 3. install and run. Run from build-v3k/bin: Vita3K finds its shaders beside
#    the binary, and a copy elsewhere fails in createGraphicsPipeline
cd build-v3k/bin
./Vita3K --frontend sdl -B Vulkan path/to/app.vpk     # first time: installs
./Vita3K --frontend sdl -B Vulkan -r <TITLEID>        # later runs
```

The generated code runs as whatever title the build was made for. Any other
title launched from that binary still dispatches into it, so build one Vita3K
per recompiled game. Headless works too: run Xvfb, and use
`mesa-vulkan-drivers` (lavapipe) for Vulkan. That is how bridgetest was run
here.
