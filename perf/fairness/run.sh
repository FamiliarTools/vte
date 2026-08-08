#!/usr/bin/env bash
# usage: run.sh <idle|textflood|flood> <:display>
#
# Builds nothing; expects fairness(1) built against the tree's libvte, e.g.
#
#   gcc -O2 -o fairness fairness.c -I ../../src -I ../../_b/src \
#       $(pkg-config --cflags gtk4) -L ../../_b/src -lvte-2.91-gtk4 \
#       $(pkg-config --libs gtk4)
set -u
MODE=$1; DISP=$2
HERE=$(cd "$(dirname "$0")" && pwd)
DONE=$HERE/done_$MODE
rm -f "$DONE"
cleanup(){ [ -n "${A:-}" ] && kill $A 2>/dev/null; [ -n "${X:-}" ] && kill $X 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT
Xvfb $DISP -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & X=$!
sleep 2
export DISPLAY=$DISP GDK_BACKEND=x11 GSK_RENDERER=cairo LIBGL_ALWAYS_SOFTWARE=1
export LD_LIBRARY_PATH="$HERE/../../_b/src:${LD_LIBRARY_PATH:-}"
START=$(date +%s.%N)
"$HERE/fairness" "$MODE" "$DONE" >/dev/null 2>&1 & A=$!
for i in $(seq 1 120); do [ -f "$DONE" ] && break; sleep 0.5; done
if [ -f "$DONE" ]; then
  END=$(cat "$DONE")
  echo "$MODE: victim finished in $(python3 -c "print(f'{$END-$START:.2f}')")s"
else
  echo "$MODE: victim DID NOT FINISH within 60s"
fi
