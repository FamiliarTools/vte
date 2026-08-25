#!/usr/bin/env bash
# Render the scale fixture under Xvfb and measure what came out.
#
# usage: run.sh <vte-app> <out-dir>
#
# One Xvfb for the whole matrix; the app is relaunched per configuration
# because GDK_SCALE, GDK_DPI_SCALE and the font zoom are all fixed for the
# lifetime of a process. Every run captures the root window, so the numbers
# are read off the pixels a display would have shown.
#
# The configurations, and what each is here to say:
#
#   gdk1           the reference frame
#   gdk2           integer HiDPI: everything twice the size, image included
#   gdk2-dpi0.75   X11's only approximation of a fractional scale - a 2x
#                  surface with the font at three quarters. NOTE that this
#                  does NOT make a 1.5x image: the emulated sixel cell shrinks
#                  with the font, so the image keeps its 2x pixel size and
#                  covers 12 columns where it covered 9.6. A true 1.5 device
#                  scale is Wayland only; see run-wayland.sh.
#   zoom1.5        the zoom keys, which is the path GNOME/vte#253 is about
#   zoom2
set -u

APP=${1:?vte app binary}
OUT=${2:?output directory}

HERE=$(cd "$(dirname "$0")" && pwd)
SIX=$HERE/../../src/tests/sixel/bands.six
FONT=${VTE_TEST_FONT:-Monospace 12}

for tool in Xvfb import magick; do
        command -v "$tool" >/dev/null 2>&1 || {
                echo "SKIP: $tool not available"
                exit 77
        }
done
[ -x "$APP" ] || { echo "SKIP: $APP not executable"; exit 77; }

mkdir -p "$OUT"

cleanup() {
        [ -n "${XPID:-}" ] && kill "$XPID" 2>/dev/null
        wait 2>/dev/null
        rm -f "$OUT/.display"
}
trap cleanup EXIT

# Big enough for 80x24 cells at twice the size, and let X pick the display
# and say which rather than guessing a number that another run may hold.
Xvfb -displayfd 3 -screen 0 2400x1600x24 -nolisten tcp \
        3>"$OUT/.display" >/dev/null 2>&1 &
XPID=$!

for _ in $(seq 1 100); do
        [ -s "$OUT/.display" ] && break
        sleep 0.1
done
DISP=$(cat "$OUT/.display" 2>/dev/null)
[ -n "$DISP" ] || { echo "SKIP: Xvfb did not report a display"; exit 77; }

export DISPLAY=":$DISP"
export GDK_BACKEND=x11
export GSK_RENDERER=${VTE_TEST_RENDERER:-cairo}
export LIBGL_ALWAYS_SOFTWARE=1
export VTE_SIXEL=1

shot() { # <name> <font-scale>
        local name=$1 zoom=$2 pid

        "$APP" --no-decorations --no-scrollbar --cursor-blink=off \
                --geometry 80x24 --font "$FONT" --font-scale "$zoom" \
                -- /bin/sh "$HERE/child.sh" "$SIX" \
                >"$OUT/$name.log" 2>&1 &
        pid=$!

        sleep 6
        import -window root "$OUT/$name.png" 2>/dev/null
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null

        echo "captured $OUT/$name.png"
}

GDK_SCALE=1 shot gdk1 1.0
GDK_SCALE=2 shot gdk2 1.0
GDK_SCALE=2 GDK_DPI_SCALE=0.75 shot gdk2-dpi0.75 1.0
GDK_SCALE=1 shot zoom1.5 1.5
GDK_SCALE=1 shot zoom2 2.0

# Not exec: that would replace this shell and skip the trap, leaving an Xvfb
# behind on every run.
echo
"$HERE/measure.py" "$OUT" gdk1 gdk2 gdk2-dpi0.75 zoom1.5 zoom2
