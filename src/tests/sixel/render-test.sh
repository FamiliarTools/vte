#!/usr/bin/env bash
#
# Copyright © 2026 Guilherme Fontes
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
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
# renders identically - same 96x72 px, same bands, same colours - but GTK4's
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

# A comparison that could not be MADE is a failure of this test, and is
# reported apart from a frame that genuinely differs: one says the machine is
# broken, the other says the renderer is. What neither may do is agree with
# the golden by default. This test used to read any ImageMagick failure as an
# empty difference and print PASS, so every bug it exists to catch was
# invisible on a host where ImageMagick could not run at all.
#
# Not a skip: the tools were all found on PATH at the top of the run, so
# failing here means one of them broke on real input, which is a result.
broken_comparison() {
        echo "FAIL: $CASE could not be compared: $1"
        [ -s "$WORK/im.log" ] && sed 's/^/  /' "$WORK/im.log"
        exit 1
}

# Run against a scratch home, not the invoking developer's.
#
# The app reads $XDG_CONFIG_HOME/vteapp.ini - or $HOME/.config/vteapp.ini when
# that is unset - at startup, and that file sets the very inputs the golden was
# captured against: the colours, the margins, the font, whether sixel is
# enabled at all. A developer who has one was not being told about the
# renderer, they were being told about their own terminal settings, and a
# golden regenerated on that machine carried the settings into the tree.
#
# The two guards - this scratch home, and the --no-load-config passed further
# down - overlap on vteapp.ini, and each also holds something the other does
# not. render-isolation-test.sh probes them one at a time and goes red for
# either alone:
#
#   - the scratch home is the only one that stops a user FONTCONFIG, which
#     fontconfig reads from $XDG_CONFIG_HOME/fontconfig/fonts.conf without
#     asking the app. Measured: a fonts.conf reassigning Monospace to DejaVu
#     Serif moves bands-margin by AE=1764 once this block is deleted.
#   - --no-load-config is the only one that stops a vteapp.ini reached by a
#     home the app was given past this block. Measured: AE=33490.7 under such
#     a home once the flag is deleted.
export HOME="$WORK/home"
export XDG_CONFIG_HOME="$HOME/.config"
mkdir -p "$XDG_CONFIG_HOME"

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

# Turn the cursor OFF, then emit the image at the home position and park what
# is now an invisible cursor below it.
#
# A blinking cursor inside the compared region makes the verdict a coin flip
# on blink phase, and parking it at row 20 does not put it outside every
# compared region: raster-opaque and raster-transparent crop 400x400, which
# reaches down past row 20, where the other cases crop 320x130 and do not.
#
# It bit: raster-opaque and raster-transparent failed together on two full
# gtk3 suite runs and passed when re-run on their own, unchanged, and the
# pixels that differed on one of those failures were the single box
# 10x19+1+381 - the parked row, nowhere near the image.
#
# Then measured directly, gtk3, raster-opaque, eight 400x400 crops taken 0.35s
# apart: with the cursor merely parked, consecutive crops differ in exactly
# 10x19+1+381; with the DECTCEM below, all eight crops are byte-identical.
# The park stays, now only to keep the terminal's idea of the cursor away from
# the image.
CHILD="printf '\\033[?25l\\033[H'; cat '$SIX'; printf '\\033[20;1H'; sleep 30"
KEEP=()

# A case whose fixture is a sixel with NO TERMINATOR has to be run
# differently, and an empty <case>.eof next to the fixture says so - the flag
# travelling with the fixture the way <case>.crop does.
#
# Such an image is drawn in exactly one place: process_incoming_decsixel
# reads a parser ABORT as a truncated image rather than a cancelled one only
# when the chunk is at eos (vte.cc), and eos is the child closing the pty. So
# the child must EXIT, and the app is asked to --keep the window that is then
# captured. Measured: the same fixture under the sleep above renders nothing.
#
# Nothing may follow the fixture on the way out either. The cursor park is an
# ESC, and an ESC reaching the sixel parser mid-sequence is a CANCEL, which
# deliberately does not draw. Measured with the park still in place: three
# runs, two blank frames and one image. So this case drops the park and keeps
# only the DECTCEM the child above already begins with.
if [ -r "$SRCDIR/$CASE.eof" ]; then
        KEEP=(--keep)
        CHILD="printf '\\033[?25l\\033[H'; cat '$SIX'"
fi

# Pin the font, because some of the compared regions move with the cell.
#
# Not all of them, and it is worth being exact about which, because the loose
# version of this claim - "a different font renders the image at a different
# scale, so every golden mismatches by a uniform factor" - was measured on
# this arm and is false. Running the gtk3 cases against three fonts:
#
#   case              Monospace 12   Monospace 30      DejaVu Serif 12
#   bands             PASS           PASS              PASS
#   raster-opaque     PASS           FAIL AE=107.294   PASS
#   cursor-right-off  PASS           FAIL AE=338.792   FAIL AE=89.8
#   bands-margin      PASS           FAIL AE=1764      FAIL AE=1764
#
# bands is byte-identical across all three: the image is drawn at its own
# pixel size at the terminal origin, and neither that size nor that origin
# follows the font. What follows the font is WHERE ELSE in the window a case
# looks - the right margin bands-margin crops to sits at the column count
# times the cell width, and the cursor and the raster rows below the image are
# placed in cells - so those three move, by different amounts, and not by any
# single factor. (That the alternate fonts took effect at all is what those
# failures show; a --font that was ignored would have left all four PASS.)
#
# So the pin is here for the three, not for the image scale, and an
# all-cases-fail run is NOT explained by it - bands would still be passing.
# It does not make this portable either: fontconfig still resolves "Monospace"
# to whatever the system has, and a system whose Monospace is not the one the
# goldens were captured against moves the same three cases.
FONT=${VTE_TEST_FONT:-Monospace 12}

# --sixel is asked for explicitly rather than relied on. The app defaults it
# on (app.cc, gboolean sixel{true}), which is why the option is hidden from
# --help-all, but that default is a thing a patch can change and this suite
# would then be capturing a terminal with no images in it. It replaces an
# exported VTE_SIXEL=1 that nothing in the tree ever read.
#
# No render case holds it while the default is on, so render-gate-test.sh
# stages the day it flips: it runs this runner against a wrapper that puts
# --no-sixel AHEAD of these arguments, which - GOption parsing argv left to
# right into the one gboolean - is an app whose effective default is off. The
# flag below is then the only thing putting the images back, and deleting it
# turns that scenario red.
"$APP" --sixel "${KEEP[@]}" --no-load-config --no-decorations --geometry 80x24 --font "$FONT" \
        -- sh -c "$CHILD" >"$WORK/app.log" 2>&1 &
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
# Every ImageMagick step from here on reports its own failure and stops.
# A step that did not run has produced no evidence about the rendering, and
# the one thing it must never do is let the run continue to a verdict; see
# broken_comparison below.
convert "$WORK/shot.png" -crop "$CROP" +repage "$WORK/crop.png" 2>"$WORK/im.log" ||
        broken_comparison "could not crop $CROP out of the captured frame"

if [ "$UPDATE" = "--update-golden" ]; then
        cp "$WORK/crop.png" "$GOLDEN"
        echo "updated golden: $GOLDEN"
        exit 0
fi

[ -r "$GOLDEN" ] || { echo "FAIL: no golden $GOLDEN (run with --update-golden)"; exit 1; }

# Normalise both through the same pixel format before comparing, so a PNG
# encoder difference cannot masquerade as a rendering difference.
convert "$GOLDEN" -depth 8 -colorspace sRGB "$WORK/a.ppm" 2>"$WORK/im.log" ||
        broken_comparison "could not read the golden $GOLDEN"
convert "$WORK/crop.png" -depth 8 -colorspace sRGB "$WORK/b.ppm" 2>"$WORK/im.log" ||
        broken_comparison "could not read the captured frame"

# Establish that the two frames are the same shape before believing anything
# said about their contents. The difference composite below only covers the
# overlap of the two, so a golden and a crop of different sizes come out of it
# looking identical.
SIZE_A=$(convert "$WORK/a.ppm" -format '%wx%h' info: 2>"$WORK/im.log") ||
        broken_comparison "could not measure the golden"
SIZE_B=$(convert "$WORK/b.ppm" -format '%wx%h' info: 2>"$WORK/im.log") ||
        broken_comparison "could not measure the captured frame"
# Nothing failed here, so do not quote ImageMagick's last words at it.
: >"$WORK/im.log"
[ "$SIZE_A" = "$SIZE_B" ] ||
        broken_comparison "the frame is $SIZE_B where the golden is $SIZE_A"

# Count the pixels that DIFFER. Nothing short of that answers the question
# this test asks, and the near miss is instructive: the verdict used to be
# read from the '%@' of a thresholded difference, described as the bounding
# box of the differing pixels. It is not. '%@' is a TRIM box, computed against
# the corner pixel, so a difference in which every pixel is set trims away to
# nothing and reports 0x0 - byte for byte what a difference in which none of
# them are set reports. The golden compared against its own negative passed.
#
# compare -metric AE writes the count to STDERR and answers again in its exit
# status: 0 equal, 1 different, 2 and above the run itself failed. Both are
# read, and checked against each other, because a status that disagrees with
# the count means this no longer understands what it is being told and has no
# business issuing a verdict. The count is not always a whole number - a Q16
# HDRI build reports the differing CHANNELS, as a fraction of a pixel - so it
# is only ever tested against zero, never parsed as a pixel total.
#
# What it does not do is notice a size mismatch: compare measures the overlap,
# and calls a 320x130 frame equal to the same frame one column narrower. That
# is what the size check above is for, and why it has to come first.
compare -metric AE "$WORK/a.ppm" "$WORK/b.ppm" null: 2>"$WORK/im.log"
CMP=$?
AE=$(tail -n 1 "$WORK/im.log" | awk '{print $1}')

[ "$CMP" -le 1 ] ||
        broken_comparison "the two frames could not be compared"

[[ $AE =~ ^[0-9]+(\.[0-9]+)?([eE][-+]?[0-9]+)?$ ]] ||
        broken_comparison "the count of differing pixels came back as '$AE'"

# Compared as numbers: the count arrives in whatever notation the build uses.
if awk -v ae="$AE" 'BEGIN { exit !(ae == 0) }'; then
        [ "$CMP" = 0 ] ||
                broken_comparison "compare found no differing pixel yet exited $CMP"
        echo "PASS: $CASE renders identically to the golden"
        exit 0
fi

[ "$CMP" = 1 ] ||
        broken_comparison "compare found $AE differing yet exited $CMP"

echo "FAIL: $CASE differs from the golden (AE=$AE)"
# Keep the failing frame OUT of the source tree. $SRCDIR is the checked-in
# fixture directory: writing there dirties the working copy of anyone whose
# tree is writable, and is simply refused on the out-of-tree, read-only-srcdir
# builds distributors use. The working directory meson gives the test is in
# the build tree; VTE_TEST_ARTIFACT_DIR overrides it.
ARTIFACT_DIR=${VTE_TEST_ARTIFACT_DIR:-$PWD}
ACTUAL="$ARTIFACT_DIR/$CASE-$ARM.actual.png"
if cp "$WORK/crop.png" "$ACTUAL" 2>/dev/null; then
        echo "  actual frame written to $ACTUAL"
else
        echo "  could not write the actual frame to $ACTUAL"
fi
exit 1
