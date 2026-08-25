#!/usr/bin/env bash
# usage: run.sh [repeats] [chars] [mode...]
#
# Builds insert-char(1) against the tree's build directory, then runs it under
# perf(1) for each population. VTE_BUILDDIR selects the build directory and
# defaults to _build.
set -eu
export LC_ALL=C
REPEATS=${1:-5}
CHARS=${2:-4000000}
shift 2 2>/dev/null || true
MODES=${*:-cjk combining ascii}
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
BUILD=${VTE_BUILDDIR:-$TOP/_build}

command -v perf >/dev/null || { echo "perf not found"; exit 77; }
command -v Xvfb >/dev/null || { echo "Xvfb not found"; exit 77; }

# The A/B this harness exists for compares two builds of libvte, so a stale
# object would be the whole result. Refuse to measure a build directory that
# has work outstanding.
if ! ninja -C "$BUILD" -n | grep -q "no work to do"; then
        echo "$BUILD is not up to date; run ninja first"
        exit 1
fi

export PKG_CONFIG_PATH=$BUILD/meson-uninstalled:${PKG_CONFIG_PATH:-}
gcc -O2 -o "$HERE/insert-char" "$HERE/insert-char.c" \
    $(pkg-config --cflags --libs vte-2.91)

TMP=$(mktemp -d)
mkfifo "$TMP/ctl" "$TMP/ack"

DISP=:$((90 + RANDOM % 8))
Xvfb $DISP -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 &
X=$!
trap 'kill $X 2>/dev/null; wait 2>/dev/null; rm -rf "$TMP"' EXIT
sleep 2

export DISPLAY=$DISP GDK_BACKEND=x11 LIBGL_ALWAYS_SOFTWARE=1
export LD_LIBRARY_PATH="$BUILD/src:${LD_LIBRARY_PATH:-}"
export VTE_PERF_CTL_FIFO=$TMP/ctl VTE_PERF_ACK_FIFO=$TMP/ack

# -D -1 starts the counters disabled; the workload enables them around the
# flood and disables them again, so what is reported is the parse alone.
for MODE in $MODES; do
        echo "=== $MODE $CHARS ==="
        perf stat -r "$REPEATS" -D -1 --control fifo:"$TMP/ctl","$TMP/ack" \
             -e task-clock:u,instructions:u,cpu-cycles:u \
             "$HERE/insert-char" "$MODE" "$CHARS"
done
