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
# The child runs in three parts, and the runner drives the joins between them;
# see the handshake below for what each one establishes.
#
#   prologue  turn the cursor off, ask DSR-5, and then BLOCK on $WORK/go.
#             The block is what lets the runner learn the terminal's own empty
#             screen before a single byte of the fixture has been sent.
#   fixture   released by $WORK/go: home the cursor, write the sixel, park.
#   epilogue  ask DSR-5 again, so the runner has the TERMINAL's word that the
#             fixture was parsed, and then stay alive to be photographed.
#
# The child reads its replies rather than leaving them in the pty, and `stty
# raw -echo` is what makes that possible: without -echo the reply would be
# echoed back and drawn into the frame under test.
CHILD_PROLOGUE="stty raw -echo; printf '\\033[?25l\\033[5n'; dd bs=1 count=4 of='$WORK/alive' 2>/dev/null; while [ ! -e '$WORK/go' ]; do sleep 0.02; done"

CHILD="$CHILD_PROLOGUE; printf '\\033[H'; cat '$SIX'; printf '\\033[20;1H\\033[5n'; dd bs=1 count=4 of='$WORK/answered' 2>/dev/null; sleep 30"
KEEP=()
# Whether the terminal is asked to speak again AFTER the fixture. The .eof
# case below cannot be, so it says so here rather than the wait guessing.
ANSWER_AFTER=1

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
# terminal cannot be asked to confirm the parse. It is still gated on the
# terminal rather than on the child: the whole of the prologue handshake below
# runs BEFORE the fixture, and what follows it is the wait for the picture to
# change, which only the terminal can satisfy.
if [ -r "$SRCDIR/$CASE.eof" ]; then
        KEEP=(--keep)
        CHILD="$CHILD_PROLOGUE; printf '\\033[H'; cat '$SIX'"
        ANSWER_AFTER=
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
# Photograph the BARE ROOT first, before the app exists. Nothing else draws on
# this display - it was booted here, for this run, with no window manager and
# no other client - so the first frame that differs from this one is the app's
# own first paint and cannot be anything else. Every wait below is anchored on
# a frame the terminal itself produced, and this is the first link in the
# chain.
capture() {
        import -window root "$1" 2>"$WORK/im.log"
}

capture "$WORK/bare.png" || { echo "FAIL: could not capture the window"; exit 1; }

"$APP" --sixel "${KEEP[@]}" --no-load-config --no-decorations --geometry 80x24 --font "$FONT" \
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
#   DRAWN    the picture on the screen having CHANGED from the frame the
#            terminal drew before the fixture was released to it.
#
# The second is not redundant, and this is the measurement that says so rather
# than an argument. The same 1% scope, the same case, the readiness flag moved
# to the DSR reply and the screen then sampled every ~90ms, six runs: the reply
# arrived after 12.8s to 14.3s, and in ALL SIX the first capture after it was
# still AE=119145 - the empty screen. In three of the six the SECOND capture was
# too. Parsed is not painted, and a gate that stopped at the reply would have
# been a mechanism named for what it was wanted to do.
#
# That also rules out settling alone, by the same six runs: two consecutive
# equal captures is satisfied by the empty screen, which held across two
# samples half the time. Stability is only a signal where the input stream is
# QUIESCENT, and the handshake below is arranged so that it only ever is asked
# at such a point: the child blocks on $WORK/go, and the terminal has already
# answered for everything sent before it, so once a frame repeats there is
# nothing left in flight that could change it. That is a structural quiescence,
# not a duration.
#
# The cap is a last resort and not the mechanism. Reaching any of the three
# waits is reported in the terminal's terms - it never painted, it never
# answered, it never drew - and never as a difference from the golden, so a
# machine too slow to photograph can never be read as a renderer that draws the
# wrong thing.
READY_TIMEOUT=${VTE_TEST_READY_TIMEOUT:-90}

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
        local flag=$1 what=$2
        local deadline=$((SECONDS + READY_TIMEOUT))
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

# Wait until the picture has CHANGED from $1 and then stayed put for one
# further capture, and leave that settled frame in $2.
#
# Both halves are needed and neither substitutes for the other: the change is
# what makes it the terminal's signal rather than a duration, and the repeat is
# what keeps a frame caught between two paints from being the one compared.
wait_for_frame() {
        local from=$1 into=$2 what=$3
        local deadline=$((SECONDS + READY_TIMEOUT))

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

# 1. The terminal is alive and has parsed the cursor-off the child opens with.
#    Nothing visible has been sent yet, so nothing that follows can be mistaken
#    for the fixture.
wait_for_reply "$WORK/alive" "the terminal never answered DSR-5, so it never read anything the child wrote"

# 2. The terminal's own empty screen. The child is blocked on $WORK/go and has
#    answered for everything before it, so this frame is the whole of what the
#    terminal has to show until the fixture is released.
wait_for_frame "$WORK/bare.png" "$WORK/empty.png" "the terminal never painted its window"

# 3. Release the fixture.
touch "$WORK/go"

# 4. The terminal has parsed it. Not asked of the .eof case, whose fixture
#    nothing may follow; see the .eof note above.
[ -n "$ANSWER_AFTER" ] &&
        wait_for_reply "$WORK/answered" "the terminal never answered DSR-5 after $CASE, so it never finished parsing the fixture"

# 5. And has drawn it. The only thing sent since the empty frame is the
#    fixture, so a picture that differs from it is the fixture on screen.
wait_for_frame "$WORK/empty.png" "$WORK/shot.png" "the terminal parsed $CASE but never drew it"

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
