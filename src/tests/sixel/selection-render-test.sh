#!/usr/bin/env bash
# An image under a text SELECTION has to stay visible.
#
# Selecting over a sixel used to blank it: the image is painted below the
# cell backgrounds, a selected cell has an explicit background, and the
# background pass filled it opaque. Deselecting brought the picture back, so
# nothing in the ring was ever wrong and no test that reads the ring can see
# this at all. It is a property of PIXELS, and of pixels in a state no
# escape sequence can put the terminal into: a selection comes from the
# pointer.
#
# So this one is not a <case> of render-test.sh. That runner captures ONE
# frame of a terminal driven entirely over its pty; this one has to drive the
# X server as well, and its verdict is a comparison between two frames of the
# same terminal rather than against a golden. A golden would be the wrong
# instrument anyway - what the fix produces is the selection colour composited
# over the image at VTE_IMAGE_CELL_BACKGROUND_ALPHA, and pinning that blend
# into a PNG would make a test of the alpha constant out of a test of whether
# the picture is there.
#
# usage: selection-render-test.sh <vte-app> <srcdir> <gtk-arm>

set -u

APP=${1:?vte app binary}
SRCDIR=${2:?source dir}
ARM=${3:?gtk arm}

SIX="$SRCDIR/bands.six"

# xdotool and xclip are extra requirements over the other runners, and they
# are named separately when they are the ones missing: "SKIP: xdotool not
# available" is actionable where a bare skip is not.
for tool in Xvfb import convert compare xdotool xclip; do
        command -v "$tool" >/dev/null 2>&1 || {
                echo "SKIP: $tool not available"
                exit 77
        }
done
[ -x "$APP" ] || { echo "SKIP: $APP not executable"; exit 77; }
[ -r "$SIX" ] || { echo "FAIL: no fixture $SIX"; exit 1; }

WORK=$(mktemp -d)
cleanup() {
        [ -n "${APID:-}" ] && kill "$APID" 2>/dev/null
        [ -n "${XPID:-}" ] && kill "$XPID" 2>/dev/null
        wait 2>/dev/null
        rm -rf "$WORK"
}
trap cleanup EXIT

# Let X pick the display and tell us which, as the other runners do: meson
# runs these in parallel and a guessed number collides.
Xvfb -displayfd 3 -screen 0 800x600x24 -nolisten tcp 3>"$WORK/display" >/dev/null 2>&1 &
XPID=$!
for _ in $(seq 1 100); do
        [ -s "$WORK/display" ] && break
        sleep 0.1
done
DISP=$(cat "$WORK/display" 2>/dev/null)
[ -n "$DISP" ] || { echo "SKIP: Xvfb did not report a display"; exit 77; }

export DISPLAY=":$DISP"
export GDK_BACKEND=x11
export GSK_RENDERER=${VTE_TEST_RENDERER:-cairo}
export LIBGL_ALWAYS_SOFTWARE=1
export VTE_SIXEL=1

XDG_CONFIG_HOME="$WORK/config"
export XDG_CONFIG_HOME
mkdir -p "$XDG_CONFIG_HOME"

FONT=${VTE_TEST_FONT:-Monospace 12}
READY_TIMEOUT=${VTE_TEST_READY_TIMEOUT:-90}

# The same handshake render-test.sh uses, in its short form: the child asks
# DSR-5 after the fixture and blocks on the reply, so the terminal has
# ANSWERED for every byte of the image before the run goes near the pointer.
CHILD="stty raw -echo; printf '\\033[?25l\\033[H'; cat '$SIX'; printf '\\033[20;1H\\033[5n'; dd bs=1 count=4 of='$WORK/answered' 2>/dev/null; sleep 60"

"$APP" --no-decorations --no-load-config --geometry 80x24 --font "$FONT" --sixel \
       -- sh -c "$CHILD" >"$WORK/app.log" 2>&1 &
APID=$!

DEADLINE=$((SECONDS + READY_TIMEOUT))
while [ ! -s "$WORK/answered" ]; do
        [ $SECONDS -lt $DEADLINE ] || {
                echo "FAIL: the terminal never answered DSR-5 after the fixture"
                exit 1
        }
        kill -0 "$APID" 2>/dev/null || { echo "FAIL: the app exited"; cat "$WORK/app.log"; exit 1; }
        sleep 0.05
done

# The image's own box. bands is 96x72 px drawn at the terminal origin; the
# two toolkits put that origin at different places inside the window, which
# is the same offset the goldens carry one per arm for.
case "$ARM" in
        gtk3) CROP=96x72+0+0 ;;
        gtk4) CROP=96x72+5+5 ;;
        *) echo "FAIL: unknown arm $ARM"; exit 1 ;;
esac

shoot() {
        import -window root "$WORK/$1.png" 2>/dev/null ||
                { echo "FAIL: could not capture the window"; exit 1; }
        convert "$WORK/$1.png" -crop "$CROP" +repage "$WORK/$1-crop.png" 2>/dev/null ||
                { echo "FAIL: could not crop $CROP out of the captured frame"; exit 1; }
}

# How many distinct colours the crop holds. bands is eight bands, so an
# intact image reads eight whatever the selection does to it: compositing a
# flat colour over eight distinct colours at a fixed alpha leaves eight
# distinct colours. A blanked image reads ONE. That is why the metric is the
# colour count and not a pixel count of any particular colour - the fix
# deliberately CHANGES every colour in the crop, and a test that demanded
# the old ones back would fail on the fix.
colours() {
        convert "$WORK/$1-crop.png" -format '%k' info: 2>/dev/null
}

shoot before
BEFORE=$(colours before)

# The crop landing on the image is not assumed. If it were off the picture
# this whole test would compare two frames of blank terminal and pass.
[ "${BEFORE:-0}" -ge 8 ] || {
        echo "FAIL: the crop $CROP holds $BEFORE colours before any selection;"
        echo "  it is supposed to be sitting on an eight-band image"
        exit 1
}

# Drag across the image's rows, from left of it to well past its right edge.
# The window is the only one on this display and is undecorated at 0,0, so
# root coordinates are window coordinates.
IFS='x+' read -r IW IH IX IY <<EOF
$CROP
EOF
xdotool mousemove $((IX + 1)) $((IY + 1)) mousedown 1 \
        mousemove $((IX + IW + 40)) $((IY + IH - 1)) \
        mousemove $((IX + IW + 60)) $((IY + IH - 1)) mouseup 1 2>>"$WORK/app.log" || {
        echo "FAIL: xdotool could not drive the pointer"
        exit 1
}

# The drag having HAPPENED is not the drag having SELECTED. vte owns PRIMARY
# only once it has a selection, so an empty PRIMARY means the frame below
# shows an unselected terminal and says nothing about the question.
SEL=
for _ in $(seq 1 100); do
        SEL=$(xclip -o -selection primary 2>/dev/null)
        [ -n "$SEL" ] && break
        sleep 0.1
done
[ -n "$SEL" ] || {
        echo "FAIL: the drag left PRIMARY empty, so nothing was selected"
        exit 1
}

shoot after
AFTER=$(colours after)

# The selection has to have REACHED these pixels. Without this a drag that
# selected somewhere else entirely leaves the image untouched, the colour
# count stays at eight, and the run passes while testing nothing.
convert "$WORK/before-crop.png" -depth 8 -colorspace sRGB "$WORK/a.ppm" 2>/dev/null
convert "$WORK/after-crop.png" -depth 8 -colorspace sRGB "$WORK/b.ppm" 2>/dev/null
DIFF=$(compare -metric AE "$WORK/a.ppm" "$WORK/b.ppm" null: 2>&1 | tr -d '\n')
[ "${DIFF%%.*}" != "0" ] || {
        echo "FAIL: the frame under the selection is pixel-identical to the one"
        echo "  before it, so the selection never reached the image's cells"
        exit 1
}

if [ "${AFTER:-0}" -ge 8 ]; then
        echo "PASS: the image survives the selection ($BEFORE colours before, $AFTER under it)"
        exit 0
fi

echo "FAIL: the selection left $AFTER colour(s) where the image's $BEFORE were"
cp "$WORK/after-crop.png" "./selection-$ARM.actual.png" 2>/dev/null &&
        echo "  frame under the selection written to $PWD/selection-$ARM.actual.png"
exit 1
