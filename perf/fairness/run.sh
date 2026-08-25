#!/usr/bin/env bash
# usage: run.sh <idle|textflood|flood> <:display>
#
# Builds fairness(1) against the tree's build directory and runs one mode.
# VTE_BUILDDIR selects the build directory and defaults to _build. The
# uninstalled pkg-config file it carries is what names the library, so this
# follows the tree from a gtk3 configuration to a gtk4 one without editing.
set -u
MODE=$1; DISP=$2
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
BUILD=${VTE_BUILDDIR:-$TOP/_build}
DONE=$HERE/done_$MODE
rm -f "$DONE"

export PKG_CONFIG_PATH=$BUILD/meson-uninstalled:${PKG_CONFIG_PATH:-}
gcc -O2 -o "$HERE/fairness" "$HERE/fairness.c" \
    $(pkg-config --cflags --libs vte-2.91) || exit 1
cleanup(){ [ -n "${A:-}" ] && kill $A 2>/dev/null; [ -n "${X:-}" ] && kill $X 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT
Xvfb $DISP -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & X=$!
sleep 2
export DISPLAY=$DISP GDK_BACKEND=x11 GSK_RENDERER=cairo LIBGL_ALWAYS_SOFTWARE=1
export LD_LIBRARY_PATH="$BUILD/src:${LD_LIBRARY_PATH:-}"
START=$(date +%s.%N)
"$HERE/fairness" "$MODE" "$DONE" >/dev/null 2>&1 & A=$!
for i in $(seq 1 120); do [ -f "$DONE" ] && break; sleep 0.5; done
if [ -f "$DONE" ]; then
  END=$(cat "$DONE")
  echo "$MODE: victim finished in $(python3 -c "print(f'{$END-$START:.2f}')")s"
else
  echo "$MODE: victim DID NOT FINISH within 60s"
fi
