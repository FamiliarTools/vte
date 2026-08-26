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
# Terminal REPLY conformance, asserted as captured bytes.
#
# XTSMGRAPHICS geometry and register counts, and the DECRQM answer for
# DECSDM.
#
# A wrong reply here is invisible in a rendered frame: it is a lie told to the
# sender, which then sizes its image or its palette against a number we made
# up. So this test asks the running terminal and compares the exact bytes.
#
# Exits 77 (meson "skipped") when Xvfb is unavailable.
#
# A second argument of "geometry" runs only the cross-channel geometry
# assertion at the bottom. That is the one answer here that moves with the
# font, so it is the one xtsmgraphics-isolation-test.sh drives, six times over;
# the fixed replies above it would cost that test four more terminal boots per
# run and tell it nothing it is asking about.
#
# usage: xtsmgraphics-test.sh <vte-app> [geometry]

set -u

APP=${1:?vte app binary}
MODE=${2:-full}

command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: Xvfb not available"; exit 77; }
[ -x "$APP" ] || { echo "SKIP: $APP not executable"; exit 77; }

WORK=$(mktemp -d)
cleanup() {
        [ -n "${APID:-}" ] && kill "$APID" 2>/dev/null
        [ -n "${XPID:-}" ] && kill "$XPID" 2>/dev/null
        wait 2>/dev/null
        rm -rf "$WORK"
}
trap cleanup EXIT

# Ask against a scratch home, not the invoking developer's.
#
# The app reads $XDG_CONFIG_HOME/vteapp.ini - or $HOME/.config/vteapp.ini when
# that is unset - at startup, and Font in that file decides the cell the
# geometry reply is computed in. Measured on this arm: with
# Font=Monospace 40 planted in vteapp.ini, the run before this was added
# reported
#
#   FAIL XTSMGRAPHICS says 2048x1584 but CSI 14t says 2640x1584
#
# because 80 columns of that cell is 2640 px, which XTSMGRAPHICS clamps to
# VTE_SIXEL_MAX_WIDTH while CSI 14t reports the window unclamped. The
# cross-channel assertion below therefore held or failed according to what the
# developer had in their config file.
#
# HOME is set rather than only passing --no-load-config because a config dir
# feeds this beyond vteapp.ini - a user fontconfig under it decides which font
# the default monospace resolves to, and that font is again the cell.
#
# xtsmgraphics-isolation-test.sh probes the two one at a time and goes red for
# either alone. Measured on gtk3 by deleting each from this file on its own:
#
#   - without this block, a fonts.conf in $XDG_CONFIG_HOME reaches the app and
#     the run reports 2048x1152 against CSI 14t's 3280x1152.
#   - without the --no-load-config below, a vteapp.ini in a home the app was
#     given past this block reaches it, and the run reports 2048x1584 against
#     CSI 14t's 2640x1584.
export HOME="$WORK/home"
export XDG_CONFIG_HOME="$HOME/.config"
mkdir -p "$XDG_CONFIG_HOME"

# Pi numbering is xterm's: 1 is colour registers, 2 is sixel geometry, 3 is
# ReGIS. VTE used 0 and 1, so a sender asking for the register count was told
# the geometry and concluded it had 2048 colour registers.
#
# The CURRENT geometry (\033[?2;1S) is deliberately NOT pinned to a constant
# here. It is the window measured in the font's cell, so any machine whose
# monospace font resolves to different metrics reports a different number and a
# hardcoded expectation would fail for a reason that is not a defect. What must
# be true is the invariant that eed2c415 established: XTSMGRAPHICS must agree
# with CSI 14t, because an application sizing an image reads whichever of those
# channels it happens to prefer, and the fork used to tell them different
# stories. That is asserted separately below, against the terminal's own
# answer rather than against a number written down here.
# query sequence | expected reply
CASES=(
        '\033[?1;1S|^[[?1;0;1024S'
        '\033[?2;4S|^[[?2;0;2048;2052S'
        '\033[?3;1S|^[[?3;1S'
        # DECSDM. Nothing reads this mode - images always scroll - so the
        # honest answer is 4, permanently reset. Answering 2 would say
        # "settable", and a sender that then set it would lay out against a
        # rule we do not follow.
        '\033[?80$p|^[[?80;4$y'
)

fail=0

# Send one query to a freshly booted terminal and echo back what it replied.
#
# A fresh display per query, because letting X pick the number is what stops
# these from colliding with the render tests meson runs in parallel; guessing a
# display number is how this first went flaky.
ask() {
        q=$1
        n=$2

        : > "$WORK/display"
        Xvfb -displayfd 3 -screen 0 900x700x24 -nolisten tcp 3>"$WORK/display" >/dev/null 2>&1 &
        XPID=$!
        for _ in $(seq 1 100); do
                [ -s "$WORK/display" ] && break
                sleep 0.1
        done
        DISP=$(cat "$WORK/display" 2>/dev/null)
        [ -n "$DISP" ] || { echo "SKIP: Xvfb did not report a display"; exit 77; }

        export DISPLAY=":$DISP"
        export GDK_BACKEND=x11 GSK_RENDERER=cairo LIBGL_ALWAYS_SOFTWARE=1
        # No sixel switch is set here, and none is needed: every reply this
        # file asserts is emitted from inside #if WITH_SIXEL in vteseq.cc's
        # XTSMGRAPHICS handler, which is a build-time condition, not the
        # runtime VteTerminal:enable-sixel that --sixel/--no-sixel moves. (The
        # runtime property gates the DA1 attribute at vteseq.cc:3291, which
        # this file does not ask for.) The export of VTE_SIXEL=1 that stood
        # here read as a precondition and was read by nothing in the tree.

        : > "$WORK/out"
        "$APP" --no-load-config --no-decorations --geometry 80x24 -- /bin/sh -c \
                "stty raw -echo; printf '${q}'; dd bs=1 count=${n} 2>/dev/null | cat -v > $WORK/out; stty sane; sleep 1" \
                >/dev/null 2>&1 &
        APID=$!

        # Wait for the reply to have ARRIVED, rather than for a number of
        # seconds.
        #
        # This used to be `sleep 7`, and a fixed sleep is the wrong shape for
        # the question. Short of what the machine needs it reads $WORK/out
        # before the terminal has answered, and the empty file comes back as a
        # conformance FAILURE - the terminal blamed for a wait nobody made.
        # Measured on gtk3 inside a systemd scope with -p CPUQuota=2%
        # -p AllowedCPUs=0, geometry mode:
        #
        #   sleep 7     FAIL geometry agreement: unparseable replies
        #               (XTSMGRAPHICS '', CSI 14t '')
        #   this wait   ok   XTSMGRAPHICS geometry agrees with CSI 14t at 640x408
        #
        # Longer than needed it is just as wrong in the other direction: the
        # full run took 42.8s under the sleep and 3.2s under this wait, with
        # every reply byte-identical between the two.
        #
        # ${n} is a ceiling and not a length - the replies here are 7 to 17
        # bytes, so `dd bs=1 count=${n}` never returns on its own and cannot be
        # the signal. The file is: `cat -v` writes the reply through as it
        # reads it, so out being non-empty is the answer having arrived.
        #
        # Nothing further is waited for. A settle check was written on top of
        # this - out the same size on two consecutive samples - and removed,
        # because sampling out every 20ms showed it has nothing to do: both
        # unthrottled and at the 2% quota above, out went straight from 0 bytes
        # to the full 16 in a single sample and was never once observed
        # part-written.
        #
        # Reaching the cap is reported rather than treated as a reply, and the
        # caller then compares whatever did arrive - a terminal that answered
        # nothing is a result this file should show, not hide.
        deadline=$((SECONDS + ${VTE_TEST_READY_TIMEOUT:-90}))
        replied=
        while [ "$SECONDS" -lt "$deadline" ]; do
                [ -s "$WORK/out" ] && { replied=1; break; }
                sleep 0.1
        done
        [ -n "$replied" ] ||
                echo "NOT READY: no reply to ${q} arrived in time" >&2

        kill "$APID" 2>/dev/null; APID=
        kill "$XPID" 2>/dev/null; XPID=
        wait 2>/dev/null

        cat "$WORK/out"
}

if [ "$MODE" != geometry ]; then
        for c in "${CASES[@]}"; do
                q=${c%%|*}
                want=${c##*|}

                got=$(ask "$q" 18)
                if [ "$got" = "$want" ]; then
                        echo "ok   ${q} -> $got"
                else
                        echo "FAIL ${q} -> '$got' (want '$want')"
                        fail=1
                fi
        done
fi

# The cross-channel invariant. XTSMGRAPHICS current geometry (width, height)
# must be the same window CSI 14t reports (height, width) - note the opposite
# order, which is itself worth pinning, since swapping them is a plausible and
# silent bug on a non-square cell.
#
# Asserted as agreement rather than as a constant so it holds on any font.
xtsm=$(ask '\033[?2;1S' 18)
csi14=$(ask '\033[14t' 16)

xw=$(printf '%s' "$xtsm" | sed -n 's/^\^\[\[?2;0;\([0-9]*\);\([0-9]*\)S$/\1/p')
xh=$(printf '%s' "$xtsm" | sed -n 's/^\^\[\[?2;0;\([0-9]*\);\([0-9]*\)S$/\2/p')
cw=$(printf '%s' "$csi14" | sed -n 's/^\^\[\[4;\([0-9]*\);\([0-9]*\)t$/\2/p')
ch=$(printf '%s' "$csi14" | sed -n 's/^\^\[\[4;\([0-9]*\);\([0-9]*\)t$/\1/p')

if [ -z "$xw" ] || [ -z "$cw" ]; then
        echo "FAIL geometry agreement: unparseable replies (XTSMGRAPHICS '$xtsm', CSI 14t '$csi14')"
        fail=1
elif [ "$xw" = "$cw" ] && [ "$xh" = "$ch" ]; then
        echo "ok   XTSMGRAPHICS geometry agrees with CSI 14t at ${xw}x${xh}"
else
        echo "FAIL XTSMGRAPHICS says ${xw}x${xh} but CSI 14t says ${cw}x${ch}"
        fail=1
fi

exit $fail
