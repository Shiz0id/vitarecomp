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

Two Vita3K-side patches, both marked `vita3krecomp:` in the vendored tree:

- `cpu/src/cpu.cpp` and `cpu/include/cpu/functions.h` gain `set_cpu_backend_factory`;
- `CMakeLists.txt` gains `-DVITA3K_RECOMP=ON` / `-DVITA3K_RECOMP_TARGET=<game target>`, and
  `interface.cpp` calls `vita_register_functions()` and `vita3k_bridge_install()`
  just before the kernel is created.

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

## Known limits

Each limit below is stated where it bites, rather than left to be found:

- **Built and tested here:** the toolkit (ctest, both threading modes) and the
  bridge against the real Vita3K headers, with `RecompCPU` driven through
  run / live registers / nested callback / stop / two threads / concurrent heap,
  clean under ASan, UBSan and TSan (`vita3k-bridge/check.sh`). **Not yet
  built: the full Vita3K executable with `VITA3K_RECOMP=ON`.** That needs all 28
  of Vita3K's dependencies and a game target, so treat the CMake and
  `interface.cpp` wiring as written-not-run until then.
- **Flat memory only.** The bridge refuses Vita3K's page-table mode, which the
  Vulkan renderer turns on for the `PageTable` and `NativeBuffer` memory
  mapping methods. Pick another mapping.
- **HLE modules only.** A module Vita3K loads as LLE (real firmware or an
  app-bundled `.suprx`) is guest code nobody recompiled, and jumping into it is
  a dispatch miss. Each such module must be recompiled too, or HLE'd.
- **The module must load where it was recompiled.** True for `ET_SCE_EXEC`
  (fixed at `0x81000000`, the whole launch window). A relocatable module that
  Vita3K places elsewhere would need relocation-aware emit.
- **A stop takes effect at the next import**, since recompiled code has no
  instruction boundary to interrupt. A thread spinning with no kernel calls
  cannot be stopped.
- **An HLE call that rewrites PC is not followed.** It is logged by name; the
  generated code continues past the call.
- **`BL` does not set `lr`** in the generated code. Returns do not need it, but
  an HLE function that inspects LR sees a stale value.
- **`errno` is one slot for the process**, in the toolkit's `SceLibc`.
- **`LDREX`/`STREX` are not translated yet.** Once threads run in parallel they
  are not optional.

## Using it

```
# 1. recompile the module (toolkit, MIT)
armrecomp emit eboot.elf generated/recomp_funcs.c <n> path/to/vita-headers/db/360

# 2. a game target holding generated/*.c and the import-default library,
#    compiled with VITARECOMP_THREADED

# 3. build Vita3K from the vendored tree with the bridge
cmake -S third_party/Vita3K -B build-v3k -DVITA3K_RECOMP=ON -DVITA3K_RECOMP_TARGET=<game target>
```

Vita3K's dependencies are submodules upstream and are not vendored; fetch them
at the commits pinned in [`third_party/VITA3K.md`](../third_party/VITA3K.md).
