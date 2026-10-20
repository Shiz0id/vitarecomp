#!/bin/bash
# Run homebrew through stock Vita3K and through the recompiled bridge, and
# compare. Usage: tests/homebrew/run.sh [name...]   (default: every sample)
#
# A miscompile does not trap; it prints the wrong thing. This harness is what
# found the ORN, VNMLS, REVSH and extend bugs: stock Vita3K runs dynarmic,
# which shares none of our assumptions, so a difference is a lead.
#
# Needs (see docs/VITA3K_BRIDGE.md#using-it for building the two Vita3Ks):
#   V3K_BUILD  Vita3K build dir configured with -DVITA3K_RECOMP=ON. Its
#              VITA3K_RECOMP_GENERATED is where this script writes the C.
#   V3K_STOCK  a stock Vita3K binary  (default: $V3K_BUILD/bin/Vita3K.stock)
#   ARMRECOMP  the armrecomp binary   (default: build/tools/armrecomp/armrecomp)
#   VITASDK    (default /opt/vitasdk), an X display in DISPLAY (Xvfb is fine),
#              ImageMagick `import`, python3 with Pillow.
#   WORK       scratch dir            (default: build-homebrew)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
: "${VITASDK:=/opt/vitasdk}" "${WORK:=$ROOT/build-homebrew}"
: "${V3K_BUILD:?set V3K_BUILD to a Vita3K build dir configured with VITA3K_RECOMP=ON}"
: "${V3K_STOCK:=$V3K_BUILD/bin/Vita3K.stock}"
: "${ARMRECOMP:=$ROOT/build/tools/armrecomp/armrecomp}"
: "${DISPLAY:?needs an X display (e.g. Xvfb :99 and DISPLAY=:99)}"
export VITASDK PATH="$VITASDK/bin:$PATH"

# Pinned so a result means the same thing next month.
SAMPLES_REPO=https://github.com/vitasdk/samples.git
SAMPLES_REV=fe8fbef570f3280586c0c20157146e3faefb2181

GEN=$(sed -n 's/^VITA3K_RECOMP_GENERATED:[A-Z]*=//p' "$V3K_BUILD/CMakeCache.txt")
[ -n "$GEN" ] || { echo "no VITA3K_RECOMP_GENERATED in $V3K_BUILD/CMakeCache.txt"; exit 2; }
for x in "$V3K_STOCK" "$ARMRECOMP"; do [ -x "$x" ] || { echo "missing: $x"; exit 2; }; done

mkdir -p "$WORK"
if [ ! -d "$WORK/samples/.git" ]; then
    git clone -q "$SAMPLES_REPO" "$WORK/samples" && git -C "$WORK/samples" checkout -q "$SAMPLES_REV" \
        || { echo "cannot fetch vitasdk/samples"; exit 2; }
fi

# Run one binary for `secs`, screenshot near the end, keep the log.
run_one() { # out tag secs args...
    local out=$1 tag=$2 secs=$3; shift 3
    (cd "$V3K_BUILD/bin" && timeout "$secs" "$@" --frontend sdl -B Vulkan > "$out/$tag.log" 2>&1) &
    local pid=$!
    sleep $((secs - 3)); import -window root "$out/$tag.png" 2>/dev/null
    wait $pid 2>/dev/null
}

fails=0; ran=0
while read -r name src velf tid secs expect <&3; do
    case "$name" in ''|'#'*) continue ;; esac
    [ $# -gt 0 ] && ! printf '%s\n' "$@" | grep -qx "$name" && continue
    [ "$expect" = skip ] && { echo "SKIP  $name (see samples.list)"; continue; }
    out=$WORK/out/$name; bld=$WORK/build/$name; mkdir -p "$out" "$bld"

    case "$src" in repo:*) srcdir=$ROOT/${src#repo:} ;; *) srcdir=$WORK/samples/$src ;; esac
    if ! { cmake -S "$srcdir" -B "$bld" -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
           && cmake --build "$bld" -j8; } > "$out/sample-build.log" 2>&1; then
        echo "FAIL  $name: sample does not build ($out/sample-build.log)"; fails=$((fails+1)); continue
    fi
    vpk=$(ls "$bld"/*.vpk | head -1)

    rm -rf "${GEN%.c}"_imports "$GEN"
    if ! "$ARMRECOMP" emit "$bld/$velf" "$GEN" > "$out/emit.log" 2>&1 ||
       ! (cd "$V3K_BUILD" && cmake --build . -j8) > "$out/v3k-build.log" 2>&1; then
        echo "FAIL  $name: emit or Vita3K build ($out)"; fails=$((fails+1)); continue
    fi

    run_one "$out" stock "$secs" "$V3K_STOCK" "$vpk"          # also installs
    run_one "$out" recomp "$secs" ./Vita3K -r "$tid"
    ran=$((ran+1))

    pct=$(sed -n 's/^ *translated .*(\(.*\))/\1/p' "$out/emit.log")
    # A vitarecomp trap, or the host process dying under the recompiled code.
    # (Stock is not judged: a stock binary built before the framebuffer-free
    # fix in VITA3K_BRIDGE.md can crash on exit through no fault of ours.)
    trap_line=$(grep -m1 -E '^vitarecomp: |Unhandled (SIG|access)' "$out/recomp.log")
    diff=$(python3 - "$out" <<'PY'
import sys
from PIL import Image, ImageChops
d = sys.argv[1]
try:
    a = Image.open(d + '/stock.png').convert('RGB'); b = Image.open(d + '/recomp.png').convert('RGB')
    print(ImageChops.difference(a, b).getbbox() is not None and 'differs' or 'same')
except Exception as e:
    print('noshot')
PY
)
    if [ -n "$trap_line" ]; then
        echo "FAIL  $name ($pct): $trap_line"; fails=$((fails+1))
    elif [ "$expect" = same ] && [ "$diff" != same ]; then
        echo "FAIL  $name ($pct): screen $diff from stock (compare $out/stock.png and recomp.png)"
        fails=$((fails+1))
    else
        echo "PASS  $name ($pct translated; screen $diff)"
    fi
done 3< "$(dirname "$0")/samples.list"

echo "$ran run, $fails failed"
[ $fails -eq 0 ]
