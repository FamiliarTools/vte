#!/usr/bin/env bash
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
# usage: xtsmgraphics-test.sh <vte-app>

set -u

APP=${1:?vte app binary}

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
        export VTE_SIXEL=1

        : > "$WORK/out"
        "$APP" --no-decorations --geometry 80x24 -- /bin/sh -c \
                "stty raw -echo; printf '${q}'; dd bs=1 count=${n} 2>/dev/null | cat -v > $WORK/out; stty sane; sleep 1" \
                >/dev/null 2>&1 &
        APID=$!
        sleep 7
        kill "$APID" 2>/dev/null; APID=
        kill "$XPID" 2>/dev/null; XPID=
        wait 2>/dev/null

        cat "$WORK/out"
}

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
