#!/usr/bin/env bash
# Build and run the bridge's checks without building Vita3K.
#
# The bridge needs Vita3K's HEADERS, and those pull in three of its submodule
# dependencies (fmt, spdlog, boost). Point VITA3K_EXTERNAL at a Vita3K
# checkout's external/ with at least those initialised:
#
#   git clone https://github.com/Vita3K/Vita3K.git
#   git -C Vita3K submodule update --init --depth 1 external/fmt external/spdlog external/boost
#   VITA3K_EXTERNAL=$PWD/Vita3K/external vita3k-bridge/check.sh
#
# Runs the RecompCPU test plain, under ASan+UBSan, and under TSan, and
# syntax-checks bridge.cpp against the vendored (patched) Vita3K tree.
set -euo pipefail

: "${VITA3K_EXTERNAL:?set VITA3K_EXTERNAL to the external/ directory of a Vita3K checkout}"
root="$(cd "$(dirname "$0")/.." && pwd)"
out="${OUT:-$(mktemp -d)}"
cd "$root"

inc=()
for d in third_party/Vita3K/vita3k/*/include; do inc+=("-I$d"); done
inc+=(-Ithird_party/Vita3K/vita3k "-I$VITA3K_EXTERNAL/fmt/include"
      "-I$VITA3K_EXTERNAL/spdlog/include" "-I$VITA3K_EXTERNAL/boost"
      -Iinclude -Ivita3k-bridge)
rt=(src/cpu.c src/mem.c src/trap.c src/loader.c src/dispatch.c src/vfp.c src/hle_libc.c)
warn=(-Wall -Wextra -Wno-unused-parameter)

echo "== syntax: bridge.cpp"
g++ -std=c++20 -fsyntax-only "${warn[@]}" -DVITARECOMP_THREADED "${inc[@]}" vita3k-bridge/bridge.cpp

for san in none address,undefined thread; do
    obj="$out/obj-${san//,/-}"
    mkdir -p "$obj"
    flags=(-O2 -g -DVITARECOMP_THREADED)
    [ "$san" != none ] && flags+=("-fsanitize=$san")
    for c in "${rt[@]}"; do
        gcc -std=c11 "${flags[@]}" "${warn[@]}" -Iinclude -c "$c" -o "$obj/$(basename "$c").o"
    done
    g++ -std=c++20 "${flags[@]}" "${warn[@]}" -UNDEBUG "${inc[@]}" \
        vita3k-bridge/test_recomp_cpu.cpp vita3k-bridge/recomp_cpu.cpp "$obj"/*.o \
        -lpthread -o "$obj/test_recomp_cpu"
    echo "== test_recomp_cpu (sanitizer: $san)"
    # TSan reports races on stderr without failing the run; make them fail it.
    TSAN_OPTIONS=halt_on_error=1 "$obj/test_recomp_cpu"
done
