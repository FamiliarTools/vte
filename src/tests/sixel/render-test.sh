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
# toolkits put the terminal at different offsets inside it: GTK4's client-side
# border pushes the content five pixels down and right of where GTK3 puts it.
# A single shared golden would therefore fail on one arm for a reason that is
# not a defect, and papering over it with an offset constant would make the
# compared region a knob that can silently drift off the image.
#
# How much of the per-arm delta that offset accounts for was measured, case by
# case, by shifting the gtk3 golden five pixels and diffing it against the
# gtk4 one over the overlap. Five of the nine come out at AE=0 - bands,
# bands-gch, bands-truncated, raster-opaque, undefined-registers - so for
# those the offset IS the whole difference, image pixels included. Four do
# not, and the residue is not spread over the frame (boxes are in the shifted
# frame's coordinates):
#
#   cursor-right-off    AE=67.8706   differing box 10x17+1+80
#   cursor-right-on     AE=61.5569   differing box 10x17+1+80
#   raster-transparent  AE=42.6392   differing box 10x14+1+23
#   bands-margin        AE=1163.48   differing box 23x125+771+0
#
# The first three are one text cell each - the marker glyph, greys either side
# of black on both arms, so the two toolkits antialias the same glyph
# differently. bands-margin's is the right-hand end of an 800 px crop that on
# gtk4, whose terminal is inset, has run past the window: magenta all the way
# across on gtk3, window border and desktop on gtk4.
#
# So: the 5 px offset explains every image pixel measured here, and the
# remainder is glyph antialiasing plus one crop that outruns the gtk4 window.
# Neither of those is an image the renderer drew differently, and neither is
# hidden by the per-arm goldens - each arm is still compared byte for byte
# against itself.
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
# reaches down past row 20. No other case does - the crops are 320x130
# (bands, bands-gch, bands-truncated, cursor-right-on, cursor-right-off),
# 800x130 (bands-margin) and 320x80 (undefined-registers), and both of those
# heights stop far above the parked row, which the measurement below puts at
# y=381.
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
#
# The child ends by touching $WORK/fed, which is the first half of the
# readiness handshake below; see wait_until_fed.
CHILD="printf '\\033[?25l\\033[H'; cat '$SIX'; printf '\\033[20;1H'; touch '$WORK/fed'; sleep 30"
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
        CHILD="printf '\\033[?25l\\033[H'; cat '$SIX'; touch '$WORK/fed'"
fi

# Pin the font, because some of the compared regions move with the cell.
#
# Not all of them, and it is worth being exact about which, because the loose
# version of this claim - "a different font renders the image at a different
# scale, so every golden mismatches by a uniform factor" - was measured on
# this arm and is false. Every gtk3 case against six fonts, AE where it
# failed and PASS where the frame was byte-identical:
#
#   case                 Mono 12  Mono 16   Mono 30   Sans 12  Serif 12  LMono 8
#   bands                PASS     PASS      PASS      PASS     PASS      PASS
#   bands-gch            PASS     PASS      PASS      PASS     PASS      PASS
#   bands-truncated      PASS     PASS      PASS      PASS     PASS      PASS
#   undefined-registers  PASS     PASS      PASS      PASS     PASS      PASS
#   cursor-right-on      PASS     126.153   342.424   92.1725  96.451    70.3843
#   cursor-right-off     PASS      90.5216  338.792   78.0706  89.8      49.5451
#   raster-transparent   PASS     129.353   400.094   78.0706  89.8      24873.9
#   raster-opaque        PASS     107.294   107.294   PASS     PASS      7172.14
#   bands-margin         PASS    1764      1764      1764     1764     29273
#
# (Serif 12 is DejaVu Serif 12 and LMono 8 Liberation Mono 8. That the
# alternate fonts took effect at all is what the failures show; a --font that
# was ignored would have left every cell PASS.)
#
# The four invariant cases are not a list to be maintained, they are what the
# rule produces: a case is font-invariant when its compared region holds
# nothing but the image on blank background, because the image is drawn at its
# own pixel size at the terminal origin and neither that size nor that origin
# follows the font. A case moves when the region holds something the terminal
# placed in CELLS. Each of the five was looked at, and it is one of two things
# every time: a marker glyph - the cursor cases and both raster cases end by
# printing one, so their crops contain a character in a cell whose position
# and shape are the font's (measured, raster-transparent at Sans 12: the whole
# difference is the single box 13x12+1+25, one cell) - or the right margin
# bands-margin crops to, which sits at the column count times the cell width.
# The two four-figure counts are the same thing at the frame's edge: the
# window is 80x24 CELLS, so at Liberation Mono 8 it measures 576x338 where at
# Monospace 12 it fills the 800 px screen, and the 800x130 and 400x400 those
# two crops ask for run off it onto the desktop. That is why the amounts share no common factor: they are different things
# moving, not one image rescaled. A case added later needs no edit here - read
# its crop.
#
# So the pin is here for the cases that look outside the image, not for the
# image scale, and an all-cases-fail run is NOT explained by it - bands would
# still be passing.
# It does not make this portable either: fontconfig still resolves "Monospace"
# to whatever the system has, and a system whose Monospace is not the one the
# goldens were captured against moves the same five cases.
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

# Wait for the terminal to have CONSUMED the fixture, rather than for a number
# of seconds.
#
# This used to be `sleep 6`. A fixed sleep is not a wait: when it is short of
# what the machine needs, the capture lands before the image is on screen and
# the runner does not skip or say "not ready" - it compares an early frame
# against the golden and reports the mismatch as a VERDICT, which on a pixel
# gate is a flaky red indistinguishable from a real regression. The display
# these tests run on is already handshaken (Xvfb -displayfd), so the harness
# knew how to do this.
#
# Measured, gtk3/bands, this runner against a copy of it whose only difference
# is `sleep 6` in place of this block, both run inside a systemd scope with
# -p CPUQuota=2% -p AllowedCPUs=0 to stand in for a loaded builder, three runs
# each:
#
#   sleep 6        FAIL: bands differs from the golden (AE=38144), all three
#   this block     ready after 23s, 22s, 19s; PASS, all three
#
# That is the whole case for the change: the sleep did not report a slow
# machine, it reported a rendering defect that was not there.
#
# The signal is the child's own word. It touches $WORK/fed as its last act, so
# the flag appearing means the app booted, the pty was spawned, the child ran
# and `cat` returned with the whole fixture written. Nothing about it is a
# guess, and it has no lower bound - unthrottled, the same case reports "ready
# after 0s" where the sleep spent 6.
#
# There is deliberately NO second wait here for the painting to settle. One was
# written and then removed, because it could not be shown to observe anything:
# instrumented to capture the screen at the moment the flag appears and again
# once two consecutive captures agreed, the two frames were identical on all
# twelve runs measured - bands and bands-truncated, unthrottled and at the 2%
# quota above, including the .eof case whose image is only drawn at eos. A
# guard that never observes the state it exists to catch is a guard nothing
# holds, so what is left is the one mechanism with a red run behind it.
#
# The cap is a last resort, not the mechanism: reaching it is reported and
# fails the run, so a capture taken from a terminal that never read the fixture
# is never quietly turned into a verdict about pixels.
READY_TIMEOUT=${VTE_TEST_READY_TIMEOUT:-90}

wait_until_fed() {
        local started=$SECONDS
        local deadline=$((SECONDS + READY_TIMEOUT))

        while [ "$SECONDS" -lt "$deadline" ]; do
                [ -e "$WORK/fed" ] && { echo "ready after $((SECONDS - started))s"; return 0; }
                sleep 0.1
        done

        echo "NOT READY: the child never finished writing $CASE to the terminal"
        return 1
}

# A terminal that never read the fixture is not evidence about the renderer
# either way, so it is reported as the machine being too slow rather than as a
# verdict on the pixels - the same distinction broken_comparison draws below.
wait_until_fed || {
        echo "FAIL: $CASE was never ready to be captured"
        [ -s "$WORK/app.log" ] && sed 's/^/  /' "$WORK/app.log"
        exit 1
}

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
#
# The two cross-checks below are the two ways the count and the exit status can
# contradict each other, and they are not worth the same. The first decides the
# VERDICT: a count of zero is the only thing that prints PASS here, so without
# it a comparator that never agreed with the golden passes the case. The second
# decides only WHICH FAILURE IS NAMED - the count is non-zero either way, so the
# run fails either way; what the check stops is that failure reading as "the
# frame differs from the golden" and sending a reader after a rendering bug
# nothing has established. render-gate-test.sh drives one scenario at each,
# asserting the verdict and the words for the first and the words for the
# second.
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
#
# Keeping the frame at all is the point - a pixel failure is unreadable without
# it - so the build tree accumulating one per failing case is the intended
# outcome, not litter to be suppressed. render-gate-test.sh holds both halves:
# that the override is honoured, and that neither path lands among the
# fixtures.
ARTIFACT_DIR=${VTE_TEST_ARTIFACT_DIR:-$PWD}
ACTUAL="$ARTIFACT_DIR/$CASE-$ARM.actual.png"
if cp "$WORK/crop.png" "$ACTUAL" 2>/dev/null; then
        echo "  actual frame written to $ACTUAL"
else
        echo "  could not write the actual frame to $ACTUAL"
fi
exit 1
