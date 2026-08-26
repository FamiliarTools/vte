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

# Starve ONE named readiness wait, leaving the others their real deadline; see
# the four wait sites near the bottom of the file for what each one is.
#
# This exists because render-gate-test.sh could not otherwise see three of the
# four. Driven the way it used to be - VTE_TEST_READY_TIMEOUT=0 - the FIRST
# wait's deadline is already past when it is reached, so the run ends there and
# the other three had never been executed by the gate in their lives, while
# meson_options.txt tells a builder all of them report themselves in the
# terminal's own terms. A gate that cannot reach a wait cannot hold it.
#
# An unknown name is REFUSED rather than silently starving nothing - a typo in
# the gate would otherwise turn its scenario into a normal run asserting the
# wrong words - and it is refused HERE, at the top, with the other argument
# checks and before an X server or an app exists.
#
# Where it is checked is not a matter of taste. This block first sat below,
# past the Xvfb and app launches, and rejecting a name there means exiting
# between starting the background jobs and reaching the waits, which hangs:
# cleanup's `wait` blocks on a job that has been forked and has not yet exec'd,
# and it never returns. Measured - two gate runs in parallel, one of them stuck
# 500 s and still going, `ps` showing the runner in do_wait over a spinning
# bash child with no Xvfb and no app under it - and under meson it came out as
# "sixel-render-gate-gtk3 TIMEOUT 900.08s killed by signal 15 SIGTERM".
STARVE=${VTE_TEST_STARVE_WAIT:-}
case "$STARVE" in
        ''|alive|painted|parsed|drawn) ;;
        *) echo "FAIL: VTE_TEST_STARVE_WAIT names no wait: '$STARVE'"; exit 1 ;;
esac

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
# What this rests on is the direct measurement below, and not on the pair of
# whole-suite reds it was first written from. Those said only that
# raster-opaque and raster-transparent failed under parallel load and passed on
# their own, which is a symptom the mid-paint capture diagnosed at the
# readiness gate below produces just as well; one of the two did differ in
# exactly the parked row, and one is not two. So the anecdote is left here as
# what it is - a symptom, since retired at its own cause - and the reason the
# DECTCEM stays is the measurement:
#
# gtk3, raster-opaque, eight 400x400 crops taken 0.35s
# apart: with the cursor merely parked, consecutive crops differ in exactly
# 10x19+1+381; with the DECTCEM below, all eight crops are byte-identical.
# The park stays, now only to keep the terminal's idea of the cursor away from
# the image.
#
# The terminal's size in cells, in one place because three things below are
# derived from it: the --geometry the app is given, the size the child waits to
# see before it addresses a cell, and the row the paint sentinels are printed
# on.
ROWS=24
COLS=80

# Two PAINT SENTINELS: a cell of a colour that appears in no golden, printed at
# a known place, and waited for as a PIXEL.
#
# This is what the readiness gate's "the picture changed" step became, and the
# reason it had to is a measurement. That step said: the only thing sent since
# the empty frame is the fixture, so a picture that differs from it is the
# fixture on screen. That sentence is false, and here is the frame that
# falsifies it. gtk3, raster-opaque, the runner given an $APP that execs the
# real one inside `systemd-run --user --scope -p CPUQuota=1% -p AllowedCPUs=0`,
# every capture the run made kept, two reds out of six:
#
#   empty.png   800x600, 385400 px white and 94600 px black
#   shot.png    800x600, 385600 px white and 94400 px black
#   difference  200 px, trim box 10x20+1+1
#   that cell   in empty.png 200 px of #000000; in shot.png 200 px of #FFFFFF
#
# 10x20 at +1+1 is ONE CELL at the terminal origin, and it went from solid
# black to solid white. That is the BLOCK CURSOR being erased. The child's
# first act is DECTCEM off, and the terminal answers DSR-5 for it long before
# the repaint that takes the cursor off the screen; under load that repaint
# lands after the empty frame has been captured and has settled. So the picture
# changed, the change was accepted as the fixture having been drawn, and the
# frame handed to the comparison was the whole 400x400 crop white - 160000 px
# of #FFFFFF, no image anywhere in it - reported as AE=119245 against a golden
# that is a solid red square.
#
# A difference proves only that SOMETHING was painted. So do not infer; look
# for a pixel that can only have got there one way:
#
#   pre-sentinel   printed by the prologue. The empty frame is not taken until
#                  this is on screen, so the empty frame is one the terminal
#                  painted AFTER the prologue - the cursor already gone.
#   post-sentinel  printed after the fixture. Its arrival proves the paint
#                  reached PAST the fixture in the byte stream.
#
# Why the post-sentinel carries the image with it, rather than merely following
# it: the child writes the fixture and then the sentinel down ONE pty, and the
# parser consumes a byte stream in order, so insert_image() has run before the
# sentinel cell exists in the model. A frame is painted from the model, so any
# frame holding the sentinel is painted from a model that already holds the
# image.
#
# What that leaves open is a frame whose damage region covers the sentinel's
# cell and not the image's. The reason to think it cannot happen is that the
# image's invalidation is still outstanding when the sentinel's arrives, so the
# region GTK hands the next draw is the union of the two, and Terminal::draw()
# repaints every image intersecting the region it is given (vte.cc, the
# `if (m_images_enabled)` block). But that is an argument about GTK's damage
# accumulation and NOTHING HERE PROVES IT. It was surveyed instead, on this
# arm, with every capture of every run kept:
#
#   12 runs of raster-opaque under the 1% scope above, 62 captured frames.
#   24 of the 62 held the post-sentinel. All 24 also held the image
#   (#C00000 present). Frames holding the sentinel and not the image: 0.
#
# Zero counterexamples in 24 opportunities is a survey, not a demonstration.
# So the settle wait below is KEPT as belt-and-braces rather than retired on
# the strength of it: what is PROVED here is the ordering - the sentinel cannot
# be in the model before the image is - and the union is only observed.
#
# The colours are checked against the goldens rather than chosen and hoped for.
# Run in this directory, this enumerates every colour in every golden of every
# arm and looks for the two:
#
#   $ export LC_ALL=C
#   $ for f in *.golden-*.png; do
#         magick "$f" txt:- | sed 1d | awk '{print $3}'
#     done | sort -u >/tmp/allcolours
#   $ wc -l </tmp/allcolours; ls *.golden-*.png | wc -l
#   85
#   18
#   $ grep -E '#(12FE34|DE12FE)' /tmp/allcolours; echo "exit: $?"
#   exit: 1
#
# 85 distinct colours across 18 goldens and neither sentinel among them, so a
# sentinel pixel inside a compared crop could not be mistaken for a golden
# pixel - it would fail the case.
#
# Both halves of that are held rather than left to decay. A golden regenerated
# tomorrow could introduce the colour, so render-gate-test.sh re-runs this
# enumeration over the real goldens on every green run; and that a sentinel
# never reaches a crop in the first place is asserted per run rather than
# assumed, by assert_sentinel_clear below, which measures each sentinel's
# bounding box in the captured frame and refuses to reach a verdict if any of
# it is inside the crop.
PRE_SENTINEL=$'\033[38;2;18;254;52m\033[48;2;18;254;52m \033[m'
PRE_SENTINEL_HEX='#12FE34'
POST_SENTINEL=$'\033[38;2;222;18;254m\033[48;2;222;18;254m \033[m'
POST_SENTINEL_HEX='#DE12FE'

# Both are printed on the LAST row at the LEFT-HAND end, two columns apart so
# neither overwrites the other.
#
# Left rather than right, because the right-hand end of a row is not reliably
# on the screen. The capture is `import -window root` of an 800x600 display,
# and 80 columns only fit in it while the cell is narrow enough. Measured, one
# cell printed at row 24 column 1 and one at row 24 column 77, then located in
# the captured root:
#
#   font                column 1         column 77
#   Monospace 12        10x20+1+461      10x20+761+461
#   DejaVu Serif 12     17x20+1+461      NOT IN THE FRAME - 0 pixels
#   Liberation Mono 8    7x14+1+323       7x14+533+323
#
# DejaVu Serif 12 puts the cell at 17 px, so 80 of them is 1360 px and column
# 77 is off the right of the screen entirely. A sentinel there is one the
# runner can never see, and render-gate-test.sh runs that very font: the whole
# font scenario came out as "bands-margin was never captured: the terminal
# never painted its window" while column 77 was where the sentinels stood.
# Column 1 is in the frame at all three.
#
# The other thing the position has to survive is SCROLLING, and the pre-sentinel
# does not - it is on screen before the fixture, and a fixture can scroll it.
# Measured, gtk3/raster-opaque, three runs identical: a pre-sentinel printed on
# row 24 ended up on row 17, because raster-opaque declares "1;1;600;600 - a
# 600 px raster, 30 rows at the pinned 20 px cell - into a 24-row screen. Seven
# rows up from the bottom-left is inside the 400x400 crop that case compares.
#
# So the pre-sentinel is not left there to be scrolled: the child ERASES the
# screen as its first act after the fixture is released, before a byte of the
# fixture is written. That is an ESC, but it is ahead of the fixture rather
# than after it, so even the .eof case can take it. The frame under test
# therefore holds the post-sentinel only, which is printed after everything
# that could scroll it.
#
# What remains font-dependent is whether row 24 is above a crop's bottom edge.
# At the pinned font it is not close - y=461 against crops at most 400 tall. At
# Liberation Mono 8 the terminal is only 338 px tall, so row 24 is at y=323 and
# the two 400x400 crops would reach it. That is an honest limit of an
# overriding VTE_TEST_FONT, not a silent one: assert_sentinel_clear measures it
# per run and stops, naming the sentinel, rather than comparing a crop with a
# colour this test put in it.
PRE_AT=$'\033['"$ROWS;3"'H'
POST_AT=$'\033['"$ROWS;1"'H'

# The child runs in three parts, and the runner drives the joins between them;
# see the handshake below for what each one establishes.
#
#   prologue  turn the cursor off, wait for the terminal to have taken the
#             geometry it was asked for, print the pre-sentinel, ask DSR-5, and
#             then BLOCK on $WORK/go. The block is what lets the runner learn
#             the terminal's own empty screen before a single byte of the
#             fixture has been sent.
#   fixture   released by $WORK/go: ERASE THE SCREEN, taking the pre-sentinel
#             with it before anything can scroll it into a compared crop, then
#             home the cursor, write the sixel, park.
#   epilogue  print the post-sentinel, re-park, ask DSR-5 again so the runner
#             has the TERMINAL's word that the fixture was parsed, and then
#             stay alive to be photographed.
#
# The `stty size` loop is what makes addressing a cell mean anything. Measured
# without it: the pre-sentinel printed at row 24 landed at y=321, which is row
# 17 - the child had reached its first printf before the widget's allocation
# had reached the pty, so row 24 clamped to the bottom of a 17-row screen. A
# terminal that never reaches $ROWS x $COLS never answers DSR-5, and wait 1
# below reports that in the terminal's own terms.
#
# The child reads its replies rather than leaving them in the pty, and `stty
# raw -echo` is what makes that possible: without -echo the reply would be
# echoed back and drawn into the frame under test.
CHILD_PROLOGUE="stty raw -echo; printf '\\033[?25l'; while [ \"\$(stty size)\" != '$ROWS $COLS' ]; do sleep 0.02; done; printf '%s%s\\033[H\\033[5n' '$PRE_AT' '$PRE_SENTINEL'; dd bs=1 count=4 of='$WORK/alive' 2>/dev/null; while [ ! -e '$WORK/go' ]; do sleep 0.02; done"

CHILD="$CHILD_PROLOGUE; printf '\\033[H\\033[2J'; cat '$SIX'; printf '\\033[20;1H%s%s\\033[20;1H\\033[5n' '$POST_AT' '$POST_SENTINEL'; dd bs=1 count=4 of='$WORK/answered' 2>/dev/null; sleep 30"
KEEP=()
# Whether the terminal is asked to speak again AFTER the fixture, and whether a
# post-sentinel is printed after it. The .eof case below can take neither, so it
# says so here rather than the waits guessing.
ANSWER_AFTER=1
SENTINEL_AFTER=1

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
# runs, two blank frames and one image. So this case drops the park - and the
# DSR-5 that follows it, which is an ESC too, so this is the one case whose
# terminal cannot be asked to confirm the parse.
#
# The post-sentinel is an ESC as well, so this case does not get one either,
# and it is the one case whose readiness is still an inference rather than a
# pixel. Say exactly what it is left with, because it is weaker: the picture
# having CHANGED from the empty frame, and then repeated. What that no longer
# admits is the cursor erasure documented at the sentinels above - the empty
# frame is not taken until the PRE-sentinel is on screen, and the pre-sentinel
# is printed after the DECTCEM, so a frame carrying it was painted with the
# cursor already gone. What it still admits in principle is any OTHER paint the
# terminal makes on its own between the empty frame and the fixture's. None is
# known; none was seen in the throttled runs; and unlike the other eight cases
# that is a survey rather than a proof.
if [ -r "$SRCDIR/$CASE.eof" ]; then
        KEEP=(--keep)
        CHILD="$CHILD_PROLOGUE; printf '\\033[H\\033[2J'; cat '$SIX'"
        ANSWER_AFTER=
        SENTINEL_AFTER=
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
#
capture() {
        import -window root "$1" 2>"$WORK/im.log"
}

"$APP" --sixel "${KEEP[@]}" --no-load-config --no-decorations --geometry "${COLS}x${ROWS}" --font "$FONT" \
        -- sh -c "$CHILD" >"$WORK/app.log" 2>&1 &
APID=$!

# Wait for the TERMINAL, rather than for the child or for a number of seconds.
#
# What this replaced was a flag the CHILD touched after `cat` returned. That is
# a fact about the child: `cat` returning means the fixture is in the pty
# buffer, and says nothing about whether vte has read it, parsed it or drawn
# it. Its own failure message admitted as much - it spoke about the child never
# finishing writing. So the capture could and did land mid-paint, and the
# runner reported the resulting frame as a VERDICT on the pixels.
#
# Reproduced, gtk3/raster-opaque, this runner unchanged but given an $APP that
# execs the real one inside `systemd-run --user --scope -p CPUQuota=1%
# -p AllowedCPUs=0` - the asymmetry a loaded parallel builder produces, where
# the process taking the photograph is scheduled and the terminal is not. Six
# runs, six reds, none of them a rendering difference:
#
#   FAIL: raster-opaque differs from the golden (AE=40755.3)   x5
#   FAIL: raster-opaque differs from the golden (AE=119145)    x1
#
# The kept frames say what they were. AE=40755.3 is the crop entirely BLACK -
# the app had not painted its window at all. AE=119145 is the terminal's empty
# white screen with the cursor cell in it and NO IMAGE - parsed or not, not yet
# drawn. The golden is a solid red square, so neither is a near miss the way a
# font difference is a near miss; they are frames from before the paint.
#
# So the readiness signal has to come from the terminal, and it takes two
# different kinds of signal, because a terminal can answer for bytes it has not
# yet drawn:
#
#   PARSED   the terminal's own reply to DSR-5. A reply cannot be emitted
#            before everything queued ahead of the query has been parsed, so
#            the reply arriving is the fixture having reached the model.
#
#   DRAWN    a paint SENTINEL - a cell of a colour no golden contains, printed
#            after the fixture - being on the screen at all.
#
# The second is not redundant, and this is the measurement that says so rather
# than an argument. The same 1% scope, the same case, the readiness flag moved
# to the DSR reply and the screen then sampled every ~90ms, six runs: the reply
# arrived after 12.8s to 14.3s, and in ALL SIX the first capture after it was
# still AE=119145 - the empty screen. In three of the six the SECOND capture was
# too. Parsed is not painted, and a gate that stopped at the reply would have
# been a mechanism named for what it was wanted to do.
#
# What DRAWN must not be is "the picture changed". That was the previous
# version of this gate and the sentinels above carry the frame that falsified
# it: a 400x400 crop of 160000 white pixels, no image, admitted because the
# block cursor had been erased between the empty frame and this one. A
# difference says something was painted; it does not say WHAT.
#
# Settling is kept under both, and it is not the mechanism either. By the same
# six runs, two consecutive equal captures is satisfied by the empty screen,
# which held across two samples half the time. Stability is only a signal where
# the input stream is QUIESCENT, and the handshake is arranged so that it is
# only ever asked at such a point: the child blocks on $WORK/go, and the
# terminal has already answered for everything sent before it, so once a frame
# repeats there is nothing left in flight that could change it. That is a
# structural quiescence, not a duration.
#
# The cap is a last resort and not the mechanism. Reaching any of the four
# waits is reported in the terminal's terms - it never answered, it never
# painted, it never finished parsing, it never drew - and never as a difference
# from the golden, so a machine too slow to photograph can never be read as a
# renderer that draws the wrong thing.
READY_TIMEOUT=${VTE_TEST_READY_TIMEOUT:-90}

# The deadline a wait gets: none at all when it is the starved one. $STARVE is
# read and validated at the top of the file, before anything is launched.
deadline_for() {
        if [ "$1" = "$STARVE" ]; then
                echo "$SECONDS"
        else
                echo "$((SECONDS + READY_TIMEOUT))"
        fi
}

# DSR-5 is asked rather than DSR-6/CPR because its reply has NO variable field:
# ECMA-48 makes the operating-status answer CSI 0 n, four bytes, whatever the
# terminal is doing. CPR carries the cursor row and column, so its length moves
# with them, and a fixed-count read of one CPR would leave the tail of it in the
# pty for the next read to pick up as the next reply. Two reads are made here,
# so that mattered.
DSR_REPLY=$'\033[0n'

# All four bytes, not merely a file with something in it: `dd bs=1` writes them
# one at a time, so a length test is the only way to tell a whole reply from a
# reply the runner has caught halfway.
reply_complete() { [ -e "$1" ] && [ "$(wc -c <"$1")" = 4 ]; }

# Wait for those four bytes to have come back, and require them to BE those
# bytes. A reply of some other shape means this no longer knows what it is
# being told, which is not a thing to photograph and report on.
wait_for_reply() {
        local name=$1 flag=$2 what=$3
        local deadline
        deadline=$(deadline_for "$name")
        local got=

        while [ "$SECONDS" -lt "$deadline" ]; do
                reply_complete "$flag" && break
                sleep 0.05
        done

        reply_complete "$flag" || {
                echo "FAIL: $CASE was never captured: $what"
                [ -s "$WORK/app.log" ] && sed 's/^/  /' "$WORK/app.log"
                exit 1
        }

        got=$(cat "$flag")
        [ "$got" = "$DSR_REPLY" ] || {
                echo "FAIL: $CASE was never captured: the terminal answered DSR-5 with $(cat -v "$flag"), not ESC[0n"
                exit 1
        }
}

# How many pixels of colour $2 the frame $1 holds.
#
# Every pixel that is not the colour is painted black, then every pixel that IS
# it is painted white, and the mean of the result times the pixel count is the
# tally. -fuzz 0 makes it an exact match, so a cell antialiased against its
# neighbours contributes only the pixels that really are the colour - and the
# sentinel is a filled cell whose foreground and background are both set to it,
# so there is no glyph edge inside it to antialias.
sentinel_pixels() {
        convert "$1" -fuzz 0 -fill '#000000' +opaque "$2" \
                -fill '#FFFFFF' -opaque "$2" \
                -format '%[fx:mean*w*h]' info: 2>"$WORK/im.log"
}

# Wait until the sentinel colour $2 is ON the screen and the frame has then
# stayed put for one further capture, and leave that settled frame in $3.
#
# The sentinel is the signal; the repeat is belt-and-braces. See the sentinel
# block near the top for which of the two is proved and which is measured.
wait_for_sentinel() {
        local name=$1 hex=$2 into=$3 what=$4
        local deadline seen
        deadline=$(deadline_for "$name")

        while [ "$SECONDS" -lt "$deadline" ]; do
                capture "$into" || { echo "FAIL: could not capture the window"; exit 1; }

                seen=$(sentinel_pixels "$into" "$hex") ||
                        broken_comparison "could not look for the paint sentinel $hex"
                if awk -v n="$seen" 'BEGIN { exit !(n > 0) }'; then
                        cp "$into" "$WORK/settling.png"
                        capture "$into" || { echo "FAIL: could not capture the window"; exit 1; }
                        cmp -s "$WORK/settling.png" "$into" && return 0
                        continue
                fi

                sleep 0.05
        done

        echo "FAIL: $CASE was never captured: $what"
        [ -s "$WORK/app.log" ] && sed 's/^/  /' "$WORK/app.log"
        exit 1
}

# Wait until the picture has CHANGED from $1 and then stayed put for one
# further capture, and leave that settled frame in $2.
#
# Only the .eof case is left on this, and only because nothing may follow its
# fixture; see the .eof block above for what that costs it.
wait_for_frame() {
        local name=$1 from=$2 into=$3 what=$4
        local deadline
        deadline=$(deadline_for "$name")

        while [ "$SECONDS" -lt "$deadline" ]; do
                capture "$into" || { echo "FAIL: could not capture the window"; exit 1; }

                if cmp -s "$from" "$into"; then
                        sleep 0.05
                        continue
                fi

                cp "$into" "$WORK/settling.png"
                capture "$into" || { echo "FAIL: could not capture the window"; exit 1; }
                cmp -s "$WORK/settling.png" "$into" && return 0
        done

        echo "FAIL: $CASE was never captured: $what"
        [ -s "$WORK/app.log" ] && sed 's/^/  /' "$WORK/app.log"
        exit 1
}

# 1. The terminal is alive, has taken its geometry, and has parsed the
#    cursor-off and the pre-sentinel the child opens with. Nothing of the
#    FIXTURE has been sent yet, so nothing that follows can be mistaken for it.
wait_for_reply alive "$WORK/alive" "the terminal never answered DSR-5, so it never read anything the child wrote"

# 2. The terminal's own empty screen, and the pre-sentinel is what says this
#    frame is one the terminal painted after the prologue rather than before
#    it - which is where the erased block cursor used to hide. The child is
#    blocked on $WORK/go and has answered for everything before it, so this
#    frame is the whole of what the terminal has to show until the fixture is
#    released.
wait_for_sentinel painted "$PRE_SENTINEL_HEX" "$WORK/empty.png" "the terminal never painted its window"

# 3. Release the fixture.
touch "$WORK/go"

# 4. The terminal has parsed it. Not asked of the .eof case, whose fixture
#    nothing may follow; see the .eof note above.
[ -n "$ANSWER_AFTER" ] &&
        wait_for_reply parsed "$WORK/answered" "the terminal never answered DSR-5 after $CASE, so it never finished parsing the fixture"

# 5. And has drawn it: the post-sentinel, printed after the fixture on the same
#    pty, is on the screen. The .eof case cannot have one and falls back to the
#    picture having changed; see the .eof note above for what that is worth.
if [ -n "$SENTINEL_AFTER" ]; then
        wait_for_sentinel drawn "$POST_SENTINEL_HEX" "$WORK/shot.png" "the terminal parsed $CASE but never drew it"
else
        wait_for_frame drawn "$WORK/empty.png" "$WORK/shot.png" "the terminal parsed $CASE but never drew it"
fi

# The sentinels must be nowhere near the region the verdict comes from, and
# that is asserted on the frame in hand rather than reasoned about. A font
# other than the pinned one moves every cell, so the row they sit on is not a
# fixed pixel coordinate; if one ever lands inside the crop this stops, in the
# runner's own terms, instead of failing the case for a colour the test put
# there itself.
assert_sentinel_clear() {
        local hex=$1 what=$2 box=

        convert "$WORK/shot.png" -fuzz 0 -fill '#000000' +opaque "$hex" \
                "$WORK/sentinel.png" 2>"$WORK/im.log" ||
                broken_comparison "could not locate the $what paint sentinel"

        box=$(convert "$WORK/sentinel.png" -trim -format '%wx%h+%X+%Y' info: 2>"$WORK/im.log") ||
                broken_comparison "could not measure the $what paint sentinel"

        convert "$WORK/shot.png" -crop "$CROP" +repage "$WORK/croptest.png" 2>"$WORK/im.log" ||
                broken_comparison "could not crop $CROP out of the captured frame"

        local inside
        inside=$(sentinel_pixels "$WORK/croptest.png" "$hex") ||
                broken_comparison "could not look for the $what paint sentinel in the crop"

        awk -v n="$inside" 'BEGIN { exit !(n > 0) }' && {
                echo "FAIL: $CASE could not be compared: the $what paint sentinel $hex is at $box, inside the compared crop $CROP"
                exit 1
        }
        return 0
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

# Only the post-sentinel, because only it is still on the screen. The child
# erased the pre-sentinel before writing the fixture, so on a real run there is
# none to find; asking after it would be a check that cannot fail.
[ -n "$SENTINEL_AFTER" ] && assert_sentinel_clear "$POST_SENTINEL_HEX" post

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
