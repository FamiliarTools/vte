#!/usr/bin/env bash
# Headless SIXEL render test with golden-frame diffing.
#
# Boots the vte demo widget offscreen, feeds it a sixel file, captures the
# window, crops the region the image occupies, and compares that crop against
# a checked-in golden.
#
# Why a rendered frame and not a unit test: every interesting property of an
# image - that it is drawn at all, at the right size, in the right place, with
# the right colours, clipped where it should be - is a property of PIXELS. The
# unit tests can only see the data structures, and the drawing arm was
# missing entirely for two years without a single unit test noticing.
#
# Exits 77 (meson's "skipped") when the tools to run it are absent, so the
# suite stays green in a sandbox without X or ImageMagick rather than
# reporting a failure it cannot distinguish from a real one.
#
# usage: render-test.sh <vte-app> <srcdir> <case> [--update-golden]

set -u

APP=${1:?vte app binary}
SRCDIR=${2:?source dir}
CASE=${3:?case name}
UPDATE=${4:-}

SIX="$SRCDIR/$CASE.six"
GOLDEN="$SRCDIR/$CASE.golden.png"

for tool in Xvfb import compare convert; do
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

# A display number nobody else in a parallel test run is using.
DISP=$((90 + (RANDOM % 60)))
Xvfb ":$DISP" -screen 0 800x600x24 -nolisten tcp >/dev/null 2>&1 &
XPID=$!
sleep 2

export DISPLAY=":$DISP"
export GDK_BACKEND=x11
export GSK_RENDERER=${VTE_TEST_RENDERER:-cairo}
export LIBGL_ALWAYS_SOFTWARE=1
export VTE_SIXEL=1

# Emit the image at the home position, then park the cursor well below it so
# the blinking cursor can never land inside the compared region. Without this
# the test is a coin flip on blink phase.
CHILD="printf '\\033[H'; cat '$SIX'; printf '\\033[20;1H'; sleep 30"

"$APP" --no-decorations --geometry 80x24 -- sh -c "$CHILD" >"$WORK/app.log" 2>&1 &
APID=$!
sleep 6

import -window root "$WORK/shot.png" 2>/dev/null || {
        echo "FAIL: could not capture the window"
        exit 1
}

# Crop the image's own rectangle. The fixture is 96x72 px at the terminal
# origin; the widget insets its content, so allow a generous box and compare
# the same box on both sides.
convert "$WORK/shot.png" -crop 120x96+0+0 +repage "$WORK/crop.png" 2>/dev/null

if [ "$UPDATE" = "--update-golden" ]; then
        cp "$WORK/crop.png" "$GOLDEN"
        echo "updated golden: $GOLDEN"
        exit 0
fi

[ -r "$GOLDEN" ] || { echo "FAIL: no golden $GOLDEN (run with --update-golden)"; exit 1; }

# Normalise both through the same pixel format before comparing, so a PNG
# encoder difference cannot masquerade as a rendering difference.
convert "$GOLDEN" -depth 8 -colorspace sRGB "$WORK/a.ppm"
convert "$WORK/crop.png" -depth 8 -colorspace sRGB "$WORK/b.ppm"

DIFF=$(compare -metric AE "$WORK/a.ppm" "$WORK/b.ppm" null: 2>&1 | tr -d '\n')
BBOX=$(convert "$WORK/a.ppm" "$WORK/b.ppm" -compose difference -composite \
        -threshold 0 -format '%@' info: 2>/dev/null)

# An empty difference bounding box is ImageMagick's way of saying "no pixel
# differs". Trust the bbox rather than the AE scalar, which is scaled oddly
# for palette PNGs.
case "$BBOX" in
        ""|"0x0+0+0"|*"+"[0-9]*"+"[0-9]*)
                if [ "${BBOX%%x*}" = "0" ] || [ -z "$BBOX" ]; then
                        echo "PASS: $CASE renders identically to the golden"
                        exit 0
                fi
                ;;
esac

echo "FAIL: $CASE differs from the golden (AE=$DIFF bbox=$BBOX)"
cp "$WORK/crop.png" "${GOLDEN%.png}.actual.png" 2>/dev/null
echo "  actual frame written to ${GOLDEN%.png}.actual.png"
exit 1
