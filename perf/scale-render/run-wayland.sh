#!/usr/bin/env bash
# Render the scale fixture on a headless Wayland output and measure it.
#
# usage: run-wayland.sh <vte-gtk4-app> <out-dir>
#
# X11 cannot do this measurement. GDK_SCALE is an integer there, and the
# GDK_DPI_SCALE trick only shrinks the FONT - the surface stays at 2x, so
# nothing on X11 ever draws an image at 1.5 device pixels per logical pixel.
# A fractional output scale is a Wayland protocol (fractional-scale-v1), so
# proving the fractional path needs a Wayland compositor, and a headless one
# needs no display of its own.
#
# One compositor per scale: sway applies an output scale at startup from its
# config, which keeps each capture a fresh, independent run rather than a
# sequence whose result depends on what the previous scale left behind.
#
# Xwayland is switched off deliberately. It is not needed here, and in a
# sandbox where /tmp/.X11-unix belongs to neither root nor us, wlroots fails
# to start it and takes the compositor down with it.
set -u

APP=${1:?vte gtk4 app binary}
OUT=${2:?output directory}

HERE=$(cd "$(dirname "$0")" && pwd)
SIX=$HERE/../../src/tests/sixel/bands.six
FONT=${VTE_TEST_FONT:-Monospace 12}

for tool in sway grim magick; do
        command -v "$tool" >/dev/null 2>&1 || {
                echo "SKIP: $tool not available"
                exit 77
        }
done
[ -x "$APP" ] || { echo "SKIP: $APP not executable"; exit 77; }

mkdir -p "$OUT"

RUNTIME=$(mktemp -d)
cleanup() {
        [ -n "${SWAY_PID:-}" ] && kill "$SWAY_PID" 2>/dev/null
        wait 2>/dev/null
        rm -rf "$RUNTIME"
}
trap cleanup EXIT

export XDG_RUNTIME_DIR=$RUNTIME
export WLR_BACKENDS=headless
export WLR_LIBINPUT_NO_DEVICES=1
export LIBGL_ALWAYS_SOFTWARE=1
export WLR_RENDERER=pixman
export GDK_BACKEND=wayland
export GSK_RENDERER=${VTE_TEST_RENDERER:-cairo}
export VTE_SIXEL=1

shot() { # <name> <output scale>
        local name=$1 scale=$2 socket=

        cat >"$RUNTIME/config" <<EOF
xwayland disable
default_border none
titlebar_border_thickness 0
gaps inner 0
gaps outer 0
output HEADLESS-1 resolution 2400x1600 scale $scale
exec $APP --no-decorations --no-scrollbar --cursor-blink=off \
        --geometry 80x24 --font '$FONT' \
        -- /bin/sh $HERE/child.sh $SIX
EOF

        sway -c "$RUNTIME/config" >"$OUT/$name.log" 2>&1 &
        SWAY_PID=$!

        for _ in $(seq 1 100); do
                socket=$(cd "$RUNTIME" && ls | grep -m1 '^wayland-[0-9]*$')
                [ -n "$socket" ] && break
                sleep 0.1
        done
        [ -n "$socket" ] || { echo "SKIP: sway did not create a socket"; exit 77; }

        sleep 8
        WAYLAND_DISPLAY=$socket grim "$OUT/$name.png" 2>/dev/null

        kill "$SWAY_PID" 2>/dev/null
        wait "$SWAY_PID" 2>/dev/null
        SWAY_PID=

        echo "captured $OUT/$name.png at output scale $scale"
}

shot wl1 1
shot wl1.5 1.5
shot wl2 2

# Not exec: that would replace this shell and skip the trap, leaving the
# runtime directory behind on every run.
echo
"$HERE/measure.py" "$OUT" wl1 wl1.5 wl2
