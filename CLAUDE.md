# vitarecomp — notes for working in this repo

Static recompiler for PlayStation Vita code: Thumb-2 (ARMv7) → C. Two halves:

- **The toolkit (MIT):** `tools/armrecomp`, `src`, `include`, `tests`.
  - `decode.c` turns an encoding into an `arm_insn`.
  - `analyze.c` discovers functions.
  - `emit.c` writes C.
  - `include/vitarecomp/*.h` holds the runtime helpers the C calls (`vita_*`, `neon_*`).
- **The bridge (GPLv2):**
  - `vita3k-bridge/` runs the generated C as Vita3K's CPU and forwards imports
    to Vita3K's HLE by NID.
  - `third_party/Vita3K` is a full vendored copy we patch. Keep GPL code
    there; the toolkit must not depend on it.

Start with `docs/ROADMAP.md` for status, `docs/VITA3K_BRIDGE.md` for the
bridge, `docs/TRANSLATION.md` for what translates, and `docs/LESSONS.md` for
why things are the way they are.

## Build and test

```sh
cmake -S . -B build && cmake --build build -j8 && (cd build && ctest)
VITA3K_EXTERNAL=$PWD/third_party/Vita3K/external CC=gcc CXX=g++ \
    vita3k-bridge/check.sh                         # bridge under ASan/UBSan/TSan
```

The full Vita3K build is in `docs/VITA3K_BRIDGE.md#using-it`. It takes about
30 minutes cold and needs clang-19, libstdc++ 14 and Qt 6.11. Keep two builds:
- one with `-DVITA3K_RECOMP=ON`, rebuilt for each recompiled module;
- one stock (`VITA3K_RECOMP` off), used as the reference. The harness expects
  it at `$V3K_BUILD/bin/Vita3K.stock` unless `V3K_STOCK` is set.

## The test that matters: run it both ways

```sh
Xvfb :99 & DISPLAY=:99 V3K_BUILD=build-v3k tests/homebrew/run.sh [sample...]
```

For each sample in `tests/homebrew/samples.list`, the script:
1. builds it with VitaSDK (`/opt/vitasdk`);
2. recompiles it and rebuilds Vita3K around it;
3. runs stock and recompiled, then compares screenshots and checks for traps
   or crashes.

Every serious bug so far was a **silent miscompile**: it translated, did not
trap, and printed the wrong thing. Passing unit tests did not catch any of
them; diffing against stock (dynarmic) did. So:

- Every decoder or emitter change needs `ctest` and `tests/homebrew/run.sh`
  green before it is pushed.
- Every new NEON form gets a `T()` line in `tests/vita/neontest/main.c`. That
  sample prints register contents, and the harness fails if they differ from
  stock, so it checks semantics against dynarmic and not against our own reading.
- Every decode fix gets a test in `tests/test_decode.c`. Take the encoding from
  `arm-vita-eabi-as` + `objdump`, never from memory.
- A test failure means debug it. Never weaken or skip the test.

## Things that bite

- **Register fields of r15 change the instruction.** ORN with Rn=15 is MVN;
  AND/EOR/ADD/SUB with Rd=15 and S set are TST/TEQ/CMN/CMP. Bit 4 of a VFP
  second halfword separates core↔extension transfers from data processing.
- **IT blocks change decode:** 16-bit ALU ops do not set flags inside one.
- **Discovery's `seen` map is shared across functions.** A walk stops at code
  another walk decoded first. Anything that must hold per function (thunk
  detection, tail-call targets) cannot rely on walk-local state; see
  `thunk_tail` in `analyze.c`.
- **`code_end`** is the end of the last import stub, so `.rodata` lies past it.
  Seeds past it are data.
- **A32 is classified but not decoded**, and discovery refuses ARM seeds.
  A32 exclusives wait on that.
- The Vita3K build compiles the generated C against the **main checkout's**
  `include/`, even when run.sh runs from a worktree. A worktree that adds
  runtime helpers fails to build there ("undeclared function neon_...") until
  its headers reach the main checkout.
- Run Vita3K from its `bin/` directory, or it cannot find its shaders.
- `armrecomp emit x.velf /dev/null` gives statistics only (no imports dir).
  The import defaults go to `<out minus .c>_imports/`.

## Conventions

- Comments explain *why*, at the density of the surrounding code. Many
  record a bug that was really hit; keep them accurate when changing the code.
- No model identifiers in commits, code or docs.
- Commit in logical pieces (discovery / translation / Vita3K patch / docs).
