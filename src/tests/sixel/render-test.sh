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
# usage: render-test.sh <vte-app> <srcdir> <case> <gtk-arm> [--update-golden]

set -u

APP=${1:?vte app binary}
SRCDIR=${2:?source dir}
CASE=${3:?case name}
ARM=${4:?gtk arm}
UPDATE=${5:-}

SIX="$SRCDIR/$CASE.six"

# One golden per GTK arm, because the compared frame is the WINDOW and the two
# toolkits put the terminal at different offsets inside it: the same fixture
# renders identically - same 96x63 px, same bands, same colours - but GTK4's
# client-side border pushes the content five pixels down and right of where
# GTK3 puts it. A single shared golden would therefore fail on one arm for a
# reason that is not a defect, and papering over it with an offset constant
# would make the compared region a knob that can silently drift off the image.
GOLDEN="$SRCDIR/$CASE.golden-$ARM.png"

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

# Let X pick a free display and TELL us which, rather than guessing a number.
#
# Guessing collides: meson runs these tests in parallel, so two of them can
# choose the same display, one Xvfb then fails to start, and both capture the
# same root window - which shows up as an intermittent golden mismatch in
# whichever test lost the race. That is exactly how this first went flaky.
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

# Emit the image at the home position, then park the cursor well below it so
# the blinking cursor can never land inside the compared region. Without this
# the test is a coin flip on blink phase.
CHILD="printf '\\033[H'; cat '$SIX'; printf '\\033[20;1H'; sleep 30"

# Pin the font, because the golden is a PIXEL comparison and an image's DRAWN
# size is (cells it occupies) x (font cell size). The cells it occupies are
# font-independent by design - that is the fixed emulated cell - but the pixels
# are not, so a machine whose default monospace resolves to different metrics
# renders the same image at a different scale and every golden mismatches by a
# uniform factor. That is not a regression, and a test that cannot tell the
# difference is worse than no test.
#
# Pinning the family and size removes the settings-derived variation. It does
# NOT make this fully portable: fontconfig still resolves "Monospace" to
# whatever the system has. If every render case fails at once and the actual
# frames look right but scaled, that is this, and the fix is --update-golden on
# your machine, not a code change.
FONT=${VTE_TEST_FONT:-Monospace 12}

"$APP" --no-decorations --geometry 80x24 --font "$FONT" -- sh -c "$CHILD" >"$WORK/app.log" 2>&1 &
APID=$!
sleep 6

import -window root "$WORK/shot.png" 2>/dev/null || {
        echo "FAIL: could not capture the window"
        exit 1
}

# Crop a box that is DELIBERATELY WIDER AND TALLER than the image.
#
# The fixture is 96x72 px at the terminal origin. Cropping tightly to that
# would make the test blind to anything drawn OUTSIDE the image's cell
# extent, which is exactly the class of bug worth catching: an image that
# bleeds colour into neighbouring cells is still "drawn correctly" by any
# measure taken inside its own rectangle. So compare a region about three
# times the image's width and include the rows below it.
# A case may need a different region - one testing the right margin has to
# look at the right margin. It lives in <case>.crop next to the fixture, so
# the golden and the region that produced it travel together; a crop passed
# only through the environment silently mismatches whatever regenerated the
# golden, which is exactly how this first failed.
if [ -r "$SRCDIR/$CASE.crop" ]; then
        CROP=$(cat "$SRCDIR/$CASE.crop")
else
        CROP=${VTE_TEST_CROP:-320x130+0+0}
fi
convert "$WORK/shot.png" -crop "$CROP" +repage "$WORK/crop.png" 2>/dev/null

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
