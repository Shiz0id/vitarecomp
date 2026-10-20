# Delegating scoped tasks

Much of the remaining work is the same shape again and again: one family of
instructions → decode, a runtime helper, an emitter case and tests. That work
is handed to a smaller, cheaper model as a tightly scoped task, and a reviewer
checks it before anything reaches the branch. Seventeen NEON tasks landed
this way: F32 arithmetic in two parts, the integer multiply/saturate/shift set,
the shuffles, VLD2–4/VST2–4, the widening and narrowing forms, which
were the first pair run in parallel, the integer compares, max/min and
absolute difference, and a second parallel pair: shifts by register and the
compares with zero. With disjoint insertion points that pair
rebased onto each other with no conflicts. Then, in pairs: halving add and
pairwise max/min, the two-register misc forms, shifts by immediate, the
single-lane and all-lanes loads and stores, the narrowing add/sub with
VABAL/VABDL and the polynomial multiplies, the saturating doubling
multiplies, and the remaining F32 forms. VRECPE/VRSQRTE and the last form,
VSHLL by the element size, were done by hand. Each task cost well under a dollar, and the
reviewer rejected none outright. Review did catch errors in the reviewer's own briefs. It also
caught one gap the agent could not have seen, which is how `neontest` came about.

The loop only works because of what the reviewer does on each side of it.
Writing the brief is most of the design work, and reviewing is the
verification. The agent's own report is input to the review, not a
substitute for it.

## 1. Pick a task that fits

Good tasks:
- one instruction family, or one decode group;
- semantics that fit in a paragraph each;
- the same file pattern as an earlier commit to copy.

Keep for yourself anything that needs judgment the brief cannot carry: discovery
heuristics, Vita3K patches, or a semantics question still open (the reciprocal
estimates' exact tables, for instance).

## 2. Give it its own worktree, from the branch tip

```sh
git worktree add -q .claude/worktrees/<task> -b <task> <working-branch>
```

Create it yourself, not through a tool that picks the base. One early task
was based on the default branch and not on the working branch, and its diff was
against the wrong code. `.claude/worktrees/` is gitignored.

## 3. Write the brief

Every brief so far has had these parts, and each was added because leaving
it out cost a round:

- **The working directory, and "do not commit or push".** The reviewer commits.
- **A fixed list of files it may edit.** It is usually six: `decode.h`, `decode.c`, `emit.c`,
  `include/vitarecomp/neon.h`, `tests/test_decode.c` and `tests/test_runtime.c`, plus
  `tests/vita/neontest/main.c` for NEON.
- **Commits to copy** (`git show <sha>`) for style, helper shape and test layout.
- **Real encodings.** Assemble them with `arm-vita-eabi-as` and paste the
  `objdump` lines. Never write them from memory, in the brief or anywhere
  else. For rejects, give the encoding computed from the layout and say that
  it was computed. One brief's "invalid" encoding turned out to be valid
  (`ffbf 0a00` has N=0, so it is `{d15-d17}`). The agent caught it because it
  was told to check.
- **The bit layout and the semantics, stated outright.** Name the traps:
  - VZIP/VUZP of a Q register work across 128 bits, while VTRN/VREV work
    per D half;
  - in the narrowing shifts, bit 6 is the rounding bit, not Q;
  - rounding on a 64-bit lane must not compute `x + (1 << (s-1))`;
  - a destination may overlap a source, so read every input before writing
    any output.
- **Why it does not collide** with the neighbouring decode groups. Ask for the
  mask/value pairs and the argument in the report.
- **Build and test under UBSan with gcc**, because clang here has no UBSan runtime:
  ```sh
  cmake -S . -B build-ubsan -DCMAKE_C_COMPILER=gcc \
        -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-sanitize-recover=all -Wall -Wextra"
  ```
- **Two named mutation checks.** Each is a specific wrong behaviour a test
  must catch, restored and confirmed with `cmp`. Pick the mistake the
  semantics invite: VZIP Q per half, VTBX zeroing like VTBL, a missing
  rounding term.
- **A report format:** the diff stat, the decode conditions, the helper
  signatures, one emitted example, the mutation results and what it is unsure of.
  "Unsure of" is the most useful line. Read it first.

## 4. Running two at once

Two tasks can run in parallel when they touch different decode groups. They
still edit the same files, so:

- **Give each brief exact insertion points** (after enum entry X, before
  emit case Y, before banner Z) that differ between the two, so the merge is
  mechanical.
- **Give shared code one owner.** When both tasks need a line changed (the
  shift group's early Q check, in the widening/narrowing pair), one brief owns
  the change and the other is told not to touch it, and why.
- **Merge one, then rebase the other onto it and rerun its tests** before
  landing it.
- **Expect conflicts in the shared lists anyway.** Insertion points keep the
  logic apart, but both tasks still append to the op enum and to the `main`
  lists of `test_decode.c` and `test_runtime.c`. When both append at the end
  of the same list, the rebase conflicts. The resolution is to keep both sides.
  Check afterwards that each test function is called exactly once.

## 5. Review

Do these in order, and stop at the first failure.

1. **Read the diff** against the ARM ARM: every mask, every reject, every
   helper. Check the agent's "unsure" items yourself. Then answer them in the
   commit or the docs.
2. **Build it yourself** under UBSan and run `ctest`. Look for new warnings
   outside `nids.c`.
3. **Make your own mutation**, different from the agent's two (a wrong lane
   index, a wrong register stride). A test must fail. Restore it and check with `cmp`.
4. **Exercise the emitter.** The unit tests check helpers and decode. Nothing
   in `ctest` compiles the C the emitter prints. For NEON, `neontest` covers
   this (step 5). For anything else, build a small velf that uses each new form,
   `armrecomp emit` it, check that no traps remain and that the operands are right,
   and compile the output.
5. **Run it both ways:** `tests/homebrew/run.sh`. Every sample must pass,
   including `neontest`, whose printed registers must match stock Vita3K
   (dynarmic). That run is the check against the instruction itself, and not
   against anyone's reading of the manual.

   Stock is only a reference for what dynarmic implements. It lacks VQRSHL:
   stock logs "Undefined instruction", skips it and prints the registers
   unchanged, and `run.sh` fails the sample saying so. Check a new form's
   dynarmic decoder line (`external/dynarmic/src/dynarmic/frontend/A32/
   decoder/asimd.inc`) before giving it a `T()` line. A gap in dynarmic is
   never a reason not to port a form: the game runs it on real hardware.
   Port it, and check it instead against a reference written independently
   of the helper (exact arithmetic, every 8-bit input, random wider ones),
   as VQRSHL was.

   The Vita3K build compiles the generated C against the **main checkout's**
   `include/`. A worktree that adds helpers needs its headers in the main
   checkout for this step. Merge first, then run, then push.

## 6. Land it

```sh
git -C <worktree> add -A tools include tests && git -C <worktree> commit
git merge --ff-only <task>               # from the main checkout
tests/homebrew/run.sh                    # with the merged headers
git push -u origin <working-branch>
git worktree remove .claude/worktrees/<task> && git branch -d <task>
```

The commit message says what landed and anything left out on purpose (FPSCR.QC
is not modelled, VMUL.P8 is unclaimed, and so on), so the next task's brief can
build on it.
