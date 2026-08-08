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

# Query, expected reply. The terminal is 80x24 and the emulated sixel cell is
# 10x20, so the CURRENT geometry is 800x480 - which is the point of asking for
# it separately from the maximum.
#
# Pi numbering is xterm's: 1 is colour registers, 2 is sixel geometry, 3 is
# ReGIS. VTE used 0 and 1, so a sender asking for the register count was told
# the geometry and concluded it had 2048 colour registers.
# query sequence | expected reply
CASES=(
        '\033[?1;1S|^[[?1;0;1024S'
        '\033[?2;1S|^[[?2;0;800;480S'
        '\033[?2;4S|^[[?2;0;2048;2052S'
        '\033[?3;1S|^[[?3;1S'
        # DECSDM. Nothing reads this mode - images always scroll - so the
        # honest answer is 4, permanently reset. Answering 2 would say
        # "settable", and a sender that then set it would lay out against a
        # rule we do not follow.
        '\033[?80$p|^[[?80;4$y'
)

fail=0

for c in "${CASES[@]}"; do
        q=${c%%|*}
        want=${c##*|}

        # Let X pick a free display and report it; guessing a number collides
        # with the other tests meson runs in parallel.
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
                "stty raw -echo; printf '${q}'; dd bs=1 count=18 2>/dev/null | cat -v > $WORK/out; stty sane; sleep 1" \
                >/dev/null 2>&1 &
        APID=$!
        sleep 7
        kill "$APID" 2>/dev/null; APID=
        kill "$XPID" 2>/dev/null; XPID=
        wait 2>/dev/null

        got=$(cat "$WORK/out")
        if [ "$got" = "$want" ]; then
                echo "ok   ${q} -> $got"
        else
                echo "FAIL ${q} -> '$got' (want '$want')"
                fail=1
        fi
done

exit $fail
