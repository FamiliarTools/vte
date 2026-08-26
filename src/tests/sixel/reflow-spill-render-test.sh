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
# Where a SPILLED image lands on screen after a REFLOW, in pixels.
#
# The ring renumbers its rows from zero at every rewrap, and it renumbers the
# spill records with them so that the pixels of a parked image can still be
# reclaimed. Those new row numbers are asserted arithmetically by
# /vte/ring/image/spill-rows-survive-rewrap. What arithmetic cannot say is
# whether the picture then comes back on the row the numbers name: an internally
# consistent mistake satisfies every assertion in the unit tests and puts the
# image somewhere else on the user's screen, which is a worse fault than the
# leak the renumbering was added to fix.
#
# So this runs the whole thing through a real terminal and looks at the frame:
#
#   1. scroll a fixture image well back into the scrollback
#   2. draw another one, so the budget evicts the first and SPILLS its pixels
#   3. resize the window, which reflows and renumbers everything
#   4. scroll back to the image, which has to be faulted in from the spill
#   5. photograph it, and measure where it sits relative to the text around it
#
# and compares that against the identical run WITHOUT step 3, which is the
# known-good control. The verdict is a relationship inside each frame - how far
# the picture sits below the marker above it and above the marker below it - so
# it is read off the photograph rather than computed with the arithmetic under
# test.
#
# Exits 77 (meson's "skipped") when the tools or the build cannot support it.
#
# usage: reflow-spill-render-test.sh <vte-app> <srcdir> <gtk-arm>

set -u

APP=${1:?vte app binary}
SRCDIR=${2:?source dir}
ARM=${3:?gtk arm}

SIX="$SRCDIR/bands.six"

# xdotool is an extra requirement over the plain render runner: the scrollback
# is reached by SCROLLING, and scrolling is a widget action with no escape
# sequence behind it. The wheel is used rather than the keyboard because a
# pointer event goes to the window under the pointer, and a bare Xvfb has no
# window manager to give anything the keyboard focus.
for tool in Xvfb import convert xdotool xclip; do
        command -v "$tool" >/dev/null 2>&1 || {
                echo "SKIP: $tool not available"
                exit 77
        }
done
[ -x "$APP" ] || { echo "SKIP: $APP not executable"; exit 77; }
[ -r "$SIX" ] || { echo "FAIL: no fixture $SIX"; exit 1; }

WORK=$(mktemp -d)
XPID=
cleanup() {
        [ -n "${XPID:-}" ] && kill "$XPID" 2>/dev/null
        [ -n "${APID:-}" ] && kill "$APID" 2>/dev/null
        wait 2>/dev/null
        if [ -n "${VTE_TEST_KEEP_WORK:-}" ]; then
                echo "kept the run at $WORK"
        else
                rm -rf "$WORK"
        fi
}
trap cleanup EXIT

export HOME="$WORK/home"
export XDG_CONFIG_HOME="$HOME/.config"
mkdir -p "$XDG_CONFIG_HOME"

# Let X pick a free display and tell us which, so two of these running in
# parallel cannot photograph each other's window.
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

# The ring announces a spill and a restore under this category. Both are
# REQUIRED below; see confirm_spilled().
export VTE_DEBUG=ring

FONT=${VTE_TEST_FONT:-Monospace 12}
READY_TIMEOUT=${VTE_TEST_READY_TIMEOUT:-90}

COLS=80
ROWS=24

# How much narrower the window is made. Two hundred pixels is twenty cells at
# the pinned font and is comfortably more than one cell on any font, so the
# terminal cannot round the change away.
NARROW_BY_PX=200

# Enough to fit one copy of the fixture and not two.
#
# bands.six is 96x72 px, which is 27648 bytes of ARGB, and the ring charges an
# image at least that. So a budget of 40000 holds the first image while it is
# the only one and cannot hold the second alongside it - which is what makes
# drawing the second image the eviction that parks the first. The number is not
# load-bearing: whether the eviction actually happened is read out of the
# terminal's own debug output rather than inferred from the budget. Measured:
# at 60000 the two fit together, nothing is evicted, and the run stops at
# confirm_spilled() rather than quietly testing the resident path.
IMAGE_LIMIT=40000

# The scrollback is deliberately SHORT and the run deliberately long, because a
# ring that never dropped a row would renumber nothing.
#
# rewrap() sets the ring's first row to zero. If no row has fallen off the front
# yet, the first row already IS zero, the old and new numbering coincide, and an
# image restored at a stale row number lands exactly where a correct one would.
# The preamble below overflows the scrollback before the fixture is ever drawn,
# so by the time the reflow happens the numbering has a real offset to lose, and
# a picture placed by the old numbers misses by that whole offset.
SCROLLBACK=200
PREAMBLE=300
TRAILER=60

# Two marker cells of colours that appear in no fixture, immediately above and
# immediately below the image. They are what "the right row" is read against:
# the verdict is the picture's distance from THEM in the same photograph, not a
# pixel coordinate this script worked out for itself.
TOP_HEX='#12FE34'
BOT_HEX='#DE12FE'
TOP_MARK=$'\033[38;2;18;254;52m\033[48;2;18;254;52m        \033[m'
BOT_MARK=$'\033[38;2;222;18;254m\033[48;2;222;18;254m        \033[m'

# The image is found by SATURATION, not by any particular colour.
#
# bands.six is eight flat 9 px bands over 72 px: black, red, green, blue,
# yellow, cyan, magenta, white. The six in the middle are the only saturated
# thing in the frame once the two markers are painted out - text antialiases to
# greys, and the terminal's own background is white - so thresholding the
# saturation channel isolates exactly the band block that runs from 9 px below
# the top of the image to 62 px below it.
#
# Colour is deliberately not the handle, and that is a measurement and not
# caution. An image drawn in the SCROLLBACK does not come out at the intensity
# it has on screen: the same fixture that renders #FF0000 while it is on the
# writable rows renders #7F0000 once it has been scrolled back to, whether or
# not it was ever evicted. Hunting for #FF0000 therefore finds nothing in any
# frame this test takes, and pinning #7F0000 instead would make this test fail
# on the day that difference is fixed. Saturation and the band COUNT below
# survive both.
SAT_TOP_OFS=9                     # first saturated band, below the image top
SAT_HEIGHT=54                     # six 9 px bands
IMAGE_W=96
IMAGE_H=72

# How many pixels of colour $2 the frame $1 holds. Exactly the colour: the
# markers are filled cells, so there is no glyph edge inside one to antialias
# away.
colour_pixels() {
        convert "$1" -fuzz 0 -fill '#000000' +opaque "$2" \
                -fill '#FFFFFF' -opaque "$2" \
                -format '%[fx:mean*w*h]' info: 2>"$WORK/im.log"
}

# The bounding box of colour $2 in frame $1, as WxH+X+Y, or empty if the colour
# is not there at all.
colour_box() {
        local n
        n=$(colour_pixels "$1" "$2") || return 1
        awk -v n="$n" 'BEGIN { exit !(n > 0) }' || { echo ""; return 0; }

        convert "$1" -fuzz 0 -fill '#000000' +opaque "$2" \
                -fill '#FFFFFF' -opaque "$2" \
                -trim -format '%wx%h%X%Y' info: 2>"$WORK/im.log"
}

# The bounding box of the saturated pixels in frame $1, with the markers - which
# are saturated too - painted out first.
saturated_box() {
        convert "$1" -fuzz 0 \
                -fill '#000000' -opaque "$TOP_HEX" \
                -fill '#000000' -opaque "$BOT_HEX" \
                -colorspace HSL -channel G -separate +channel \
                -threshold 25% \
                -trim -format '%wx%h%X%Y' info: 2>"$WORK/im.log"
}

# The distinct colours of the whole image rectangle at $2,$3 in frame $1, one
# per line. Eight of them is the fixture intact; anything else is a picture that
# came back torn, rescaled, or half drawn - and comparing the two arms' lists
# says the reflow did not repaint it differently either.
image_colours() {
        convert "$1" -crop "${IMAGE_W}x${IMAGE_H}+$2+$3" +repage \
                -unique-colors -depth 8 txt:- 2>"$WORK/im.log" |
                sed -n 's/.*\(#[0-9A-Fa-f]\{6\}\).*/\1/p' | sort
}

box_field() {
        local box=$1 which=$2
        local w h x y
        IFS='x+' read -r w h x y <<EOF
$box
EOF
        case "$which" in
                w) echo "$w" ;; h) echo "$h" ;; x) echo "$x" ;; y) echo "$y" ;;
        esac
}

# The child, in one string, parameterised by the resize.
#
# The handshake is render-test.sh's short form: every stage ends with DSR-5 and
# a blocking read of the reply, so the terminal has ANSWERED for the bytes
# before the runner does anything that depends on them. A reply cannot be
# emitted before everything queued ahead of the query has been parsed.
child_script() {
        local resize=$1 work=$2
        local s=

        s+="stty raw -echo; "
        s+="printf '\\033[?25l\\033[2J\\033[H'; "

        # Overflow the scrollback before the fixture exists.
        s+="i=0; while [ \$i -lt $PREAMBLE ]; do printf 'preamble\\r\\n'; i=\$((i+1)); done; "

        # Marker, image, marker. The image is emitted at the cursor, so the
        # marker above it is the row above it; the one below is placed by
        # cursor address so that nothing depends on where a sixel leaves the
        # cursor.
        s+="printf '%s\\r\\n' '$TOP_MARK'; "
        s+="cat '$SIX'; "
        s+="printf '\\033[999;1H\\r\\n%s' '$BOT_MARK'; "

        # Scroll it up out of the screen.
        s+="printf '\\033[999;1H'; "
        s+="i=0; while [ \$i -lt $TRAILER ]; do printf 'trailer\\r\\n'; i=\$((i+1)); done; "

        # A second image. This is the eviction: the budget holds one.
        s+="cat '$SIX'; "
        s+="printf '\\033[999;1H\\033[5n'; dd bs=1 count=4 of='$work/drawn' 2>/dev/null; "

        # A live readout of the size the TERMINAL believes it has.
        #
        # This is how the runner learns that its resize landed, and it has to
        # come from inside the terminal rather than from the X server: an
        # XResizeWindow that the toolkit never turned into a new grid would
        # still change the window's geometry, and a run that read the geometry
        # would call that a reflow.
        s+="while :; do stty size > '$work/size.tmp'; mv '$work/size.tmp' '$work/size'; sleep 0.2; done"

        echo "$s"
}

wait_for_reply() {
        local flag=$1 what=$2
        local deadline=$((SECONDS + READY_TIMEOUT))

        while [ "$SECONDS" -lt "$deadline" ]; do
                [ -e "$flag" ] && [ "$(wc -c <"$flag")" = 4 ] && return 0
                kill -0 "$APID" 2>/dev/null || {
                        echo "FAIL: the app exited before $what"
                        [ -s "$LOG" ] && sed 's/^/  /' "$LOG"
                        exit 1
                }
                sleep 0.05
        done

        echo "FAIL: $what: the terminal never answered DSR-5"
        [ -s "$LOG" ] && sed 's/^/  /' "$LOG"
        exit 1
}

# The image really was evicted and its pixels really were parked on disk.
#
# Without this the run is free to be testing the RESIDENT path while claiming to
# test the spill: an image that was never evicted draws from memory and produces
# exactly the same photograph. The budget alone does not settle it - a cost
# model that charged less than expected, or a build with no image stream, would
# leave the picture in RAM - so this reads the terminal's own account of what it
# did.
#
# A build that cannot say is SKIPPED rather than passed. The debug prints are
# compiled out unless the library was built with debugging enabled, and a run
# that cannot see them cannot tell the two paths apart.
confirm_spilled() {
        grep -q 'New ring' "$LOG" || {
                echo "SKIP: this build prints no ring debug output, so whether the"
                echo "  image was actually spilled cannot be established"
                exit 77
        }

        grep -q 'Spilled image' "$LOG" || {
                echo "FAIL: the terminal never spilled an image, so this run exercised"
                echo "  the resident path and says nothing about the spill"
                sed -n 's/^/  /p' "$LOG" | tail -20
                exit 1
        }
}

# Scroll back until both markers are on the screen, and say so when they never
# come. Three wheel clicks at a time, because a click is a few lines and the
# distance is tens of rows.
scroll_back_to_the_image() {
        local into=$1
        local deadline=$((SECONDS + READY_TIMEOUT))

        # The pointer goes inside the terminal. The window is the only one on
        # this display, undecorated at the origin, so root coordinates are
        # window coordinates; the point chosen is well inside even the narrowed
        # window.
        xdotool mousemove 40 200 2>>"$LOG" || {
                echo "FAIL: xdotool could not place the pointer"
                exit 1
        }

        while [ "$SECONDS" -lt "$deadline" ]; do
                import -window root "$into" 2>"$WORK/im.log" || {
                        echo "FAIL: could not capture the window"
                        exit 1
                }

                local t b
                t=$(colour_pixels "$into" "$TOP_HEX") || return 1
                b=$(colour_pixels "$into" "$BOT_HEX") || return 1
                if awk -v t="$t" -v b="$b" 'BEGIN { exit !(t > 0 && b > 0) }'; then
                        # Settle: capture again and require the same frame, so
                        # the verdict is not taken from a half-painted scroll.
                        cp "$into" "$WORK/settling.png"
                        import -window root "$into" 2>"$WORK/im.log"
                        cmp -s "$WORK/settling.png" "$into" && return 0
                        continue
                fi

                # One click at a time, well spaced.
                #
                # `xdotool click --repeat 3 4` is what stood here first, and its
                # clicks land ~100 ms apart - inside GTK's double-click time. The
                # terminal read the burst as a multiple click and SELECTED, and a
                # selection is not a neutral thing to photograph through: it
                # inverts the text under it and composites a tint over the image.
                # Measured on the frame that produced it: every band of the
                # fixture came out at exactly half its intensity - #7F0000 where
                # the resident image draws #FF0000 - and the text rows came back
                # white-on-black. The verdict would have been taken from a
                # picture the selection had recoloured.
                #
                # That this stays true is not left to the spacing; see the
                # PRIMARY check below.
                local i
                for i in 1 2 3; do
                        xdotool click 4 2>>"$LOG" || {
                                echo "FAIL: xdotool could not drive the wheel"
                                exit 1
                        }
                        sleep 0.35
                done
        done

        echo "FAIL: scrolling back never brought both markers onto the screen"
        return 1
}

# The terminal's own column count, once it has one to report.
wait_for_size() {
        local dir=$1
        local deadline=$((SECONDS + READY_TIMEOUT))

        while [ "$SECONDS" -lt "$deadline" ]; do
                if [ -s "$dir/size" ]; then
                        local line
                        line=$(cat "$dir/size" 2>/dev/null)
                        case "$line" in
                                *' '*) echo "${line##* }"; return 0 ;;
                        esac
                fi
                sleep 0.05
        done

        echo "FAIL: the terminal never reported its size" >&2
        return 1
}

# Narrow the WINDOW from outside, and return the column count the terminal ends
# up with.
#
# The resize is done with XResizeWindow rather than with the terminal's own
# window operation, and that is not a preference: the demo app's resize is a
# no-op on gtk4 - vteapp_window_resize() has the whole body under
# `#if VTE_GTK == 3` behind a FIXMEgtk4 - so CSI 8 t leaves the gtk4 window
# exactly the size it was, and this test could only ever have run on one arm.
# Sizing the toplevel directly goes through the toolkit's ordinary
# configure handling on both.
#
# Whether it worked is read from the CHILD's `stty size`, which is the terminal
# saying it has a new grid. A bare Xvfb has no window manager, so a resize
# request is a thing that can simply be dropped, and a run that assumed it
# landed would compare two identical arms and pass.
narrow_the_window() {
        local dir=$1 before=$2
        local wid geom w h

        # By NAME, not by taking whatever the search happens to list last.
        #
        # The app puts more than one X window on the display: the toplevel, and
        # a 1x1 helper at -1,-1. `xdotool search --onlyvisible --name '' | tail
        # -1` picked the helper, resized THAT, and the terminal went on
        # reporting its original width - which this function then correctly
        # refused to call a reflow.
        wid=$(xdotool search --onlyvisible --name 'Terminal' 2>/dev/null | head -1)
        [ -n "$wid" ] || {
                echo "FAIL: no terminal window to resize on this display" >&2
                return 1
        }

        geom=$(xdotool getwindowgeometry --shell "$wid" 2>/dev/null)
        w=$(echo "$geom" | sed -n 's/^WIDTH=//p')
        h=$(echo "$geom" | sed -n 's/^HEIGHT=//p')
        [ -n "$w" ] && [ -n "$h" ] || {
                echo "FAIL: could not read the window geometry" >&2
                return 1
        }

        xdotool windowsize "$wid" "$((w - NARROW_BY_PX))" "$h" 2>>"$LOG" || {
                echo "FAIL: xdotool could not resize the window" >&2
                return 1
        }

        local deadline=$((SECONDS + READY_TIMEOUT))
        while [ "$SECONDS" -lt "$deadline" ]; do
                local now
                now=$(wait_for_size "$dir") || return 1
                if [ "$now" != "$before" ]; then
                        echo "$now"
                        return 0
                fi
                sleep 0.1
        done

        echo "FAIL: the window was resized but the terminal is still $before" >&2
        echo "  columns wide, so nothing reflowed and nothing here was tested" >&2
        return 1
}

# Run one arm and leave its measurements in the named file, one "key value" per
# line.
run_arm() {
        local name=$1 resize=$2
        local dir="$WORK/$name"
        mkdir -p "$dir"

        LOG="$dir/app.log"

        "$APP" --sixel --no-load-config --no-decorations \
               --geometry "${COLS}x${ROWS}" --font "$FONT" \
               --scrollback-lines "$SCROLLBACK" --image-limit "$IMAGE_LIMIT" \
               -- sh -c "$(child_script "$resize" "$dir")" >"$LOG" 2>&1 &
        APID=$!

        wait_for_reply "$dir/drawn" "$name: the fixture and the second image"

        confirm_spilled

        local cols
        cols=$(wait_for_size "$dir") || return 1

        if [ -n "$resize" ]; then
                cols=$(narrow_the_window "$dir" "$cols") || return 1
        fi

        echo "cols $cols" >>"$dir/measure"

        import -window root "$dir/pre-scroll.png" 2>"$WORK/im.log" || {
                echo "FAIL: could not capture the window"
                kill "$APID" 2>/dev/null
                return 1
        }

        scroll_back_to_the_image "$dir/frame.png" || {
                kill "$APID" 2>/dev/null
                return 1
            }

        # Nothing is SELECTED in the frame the verdict comes from. vte owns
        # PRIMARY only once it has a selection, so an empty PRIMARY is the
        # terminal's own word that the pixels below are the terminal's and not a
        # selection's; see the wheel loop above for the run where they were not.
        local sel
        sel=$(xclip -o -selection primary 2>/dev/null)
        [ -z "$sel" ] || {
                echo "FAIL: $name: the scroll left a selection, which recolours both"
                echo "  the text and the image this verdict is read from"
                kill "$APID" 2>/dev/null
                return 1
        }

        # The picture had to be faulted back in for this frame to be what it
        # is. If it was still in memory the scroll proved nothing about the
        # spill.
        grep -q 'Restored image' "$LOG" || {
                echo "FAIL: $name: scrolling back never restored a spilled image,"
                echo "  so this frame did not come from the spill"
                kill "$APID" 2>/dev/null
                return 1
        }

        cp "$dir/frame.png" "$WORK/$name-frame.png"

        local topbox botbox
        topbox=$(colour_box "$dir/frame.png" "$TOP_HEX")
        botbox=$(colour_box "$dir/frame.png" "$BOT_HEX")
        echo "top_y $(box_field "$topbox" y)" >>"$dir/measure"
        echo "top_h $(box_field "$topbox" h)" >>"$dir/measure"
        echo "bot_y $(box_field "$botbox" y)" >>"$dir/measure"

        local box w h x y
        box=$(saturated_box "$dir/frame.png")
        [ -n "$box" ] || {
                echo "FAIL: $name: there is no saturated pixel anywhere in the frame,"
                echo "  so the picture did not come back at all"
                kill "$APID" 2>/dev/null
                return 1
        }
        w=$(box_field "$box" w)
        h=$(box_field "$box" h)
        x=$(box_field "$box" x)
        y=$(box_field "$box" y)

        # The band block, whole and alone: a second copy of the fixture in the
        # frame, a torn picture or a rescaled one all break this.
        [ "$w" = "$IMAGE_W" ] && [ "$h" = "$SAT_HEIGHT" ] || {
                echo "FAIL: $name: the fixture's colour bands measure ${w}x${h},"
                echo "  not ${IMAGE_W}x${SAT_HEIGHT}, so this frame does not hold one intact picture"
                kill "$APID" 2>/dev/null
                return 1
        }

        echo "image_x $x" >>"$dir/measure"
        echo "image_top $((y - SAT_TOP_OFS))" >>"$dir/measure"
        image_colours "$dir/frame.png" "$x" "$((y - SAT_TOP_OFS))" >"$dir/colours"

        kill "$APID" 2>/dev/null
        wait "$APID" 2>/dev/null
        APID=
        return 0
}

get() { awk -v k="$2" '$1 == k { print $2 }' "$1"; }

run_arm control "" || exit 1
run_arm resized 1 || exit 1

CTL="$WORK/control/measure"
RSZ="$WORK/resized/measure"

# The insult landed: the resize really did change the terminal's width. A
# window operation a bare X server quietly dropped would leave both arms
# identical and this test green while it had reflowed nothing.
#
# The two arms are compared against EACH OTHER rather than against the numbers
# asked for, because the width a terminal takes is not the width it is given:
# gtk4's client-side border eats three columns, so an 80 column request comes up
# 77 and a 60 column one comes up 57. Requiring the requested numbers turned
# this test red on gtk4 for a reason that is not a defect. What has to be true
# is that the resized arm ended up NARROWER than the control, which is the
# reflow having happened at all.
CTL_COLS=$(get "$CTL" cols)
RSZ_COLS=$(get "$RSZ" cols)
[ -n "$CTL_COLS" ] && [ -n "$RSZ_COLS" ] || {
        echo "FAIL: the terminal never reported its size"
        exit 1
}
[ "$RSZ_COLS" -lt "$CTL_COLS" ] || {
        echo "FAIL: the resize was not honoured: the resized arm is $RSZ_COLS columns"
        echo "  wide and the control is $CTL_COLS, so no reflow happened and nothing"
        echo "  here was tested"
        exit 1
}

status=0

for arm in control resized; do
        m="$WORK/$arm/measure"
        r=$(get "$m" image_top)

        # Eight colours in the image's rectangle: the fixture's eight bands and
        # nothing else. A picture half restored, torn, or overdrawn by the text
        # around it does not read eight.
        n=$(wc -l <"$WORK/$arm/colours")
        [ "$n" = 8 ] || {
                echo "FAIL: $arm: the image rectangle holds $n colours, not the"
                echo "  fixture's eight, so the picture that came back is not the fixture"
                status=1
        }

        top_y=$(get "$m" top_y)
        top_h=$(get "$m" top_h)
        bot_y=$(get "$m" bot_y)

        # The ordering, before any distance: marker, picture, marker.
        [ "$((top_y + top_h))" -le "$r" ] && [ "$r" -lt "$bot_y" ] || {
                echo "FAIL: $arm: the image top is at $r, which is not between the"
                echo "  marker above (ending $((top_y + top_h))) and the marker below ($bot_y)"
                status=1
        }

        echo "$arm: image top $r, marker above ends $((top_y + top_h)), marker below at $bot_y"
        echo "above $((r - top_y - top_h))" >>"$m"
        echo "below $((bot_y - r))" >>"$m"
done

[ "$status" = 0 ] || exit 1

# The verdict. The picture's distance from the text around it is what a user
# sees, and the reflow must not change it.
for k in above below; do
        a=$(get "$CTL" "$k")
        z=$(get "$RSZ" "$k")
        [ "$a" = "$z" ] || {
                echo "FAIL: after the reflow the restored image sits $z px $k the"
                echo "  marker, where the control has it $a px $k"
                echo "  the picture came back on the wrong row"
                status=1
        }
done

# And it is the same picture, not merely one the same distance down. The reflow
# repaints everything, so a restored image drawn at a different scale or with a
# different palette would otherwise pass on its position alone.
cmp -s "$WORK/control/colours" "$WORK/resized/colours" || {
        echo "FAIL: the reflowed frame's image holds different colours than the"
        echo "  control's:"
        diff "$WORK/control/colours" "$WORK/resized/colours" | sed 's/^/    /'
        status=1
}

[ "$status" = 0 ] || {
        for arm in control resized; do
                out="$(dirname "$SIX")/../../../reflow-spill-$ARM-$arm.png"
                cp "$WORK/$arm-frame.png" "$out" 2>/dev/null &&
                        echo "  kept the $arm frame at $out"
        done
        exit 1
}

echo "PASS: the spilled image comes back on the same row relative to its text,"
echo "  with and without the reflow ($ARM)"
exit 0
