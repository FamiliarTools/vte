#!/usr/bin/env bash
# Process-level memory ceiling for scrolled-away inline images.
#
# The image budget is enforced against an accounting number the ring keeps for
# itself, and the unit tests assert against that same number. That is circular:
# it proves the ring is consistent with its own bookkeeping, not that a
# terminal holding hundreds of scrolled-away previews stops growing. Only the
# kernel can answer the second question, so this asks the kernel:
# /proc/<pid>/smaps_rollup Pss, sampled at four points of a real flood driven
# through a real pty into a real widget on a real display.
#
# The measurement is a set of DIFFERENCES, never an absolute number, because a
# terminal's resident set is mostly fonts, glyph caches and a drawing surface,
# and those swamp the quantity of interest. Four phases, identical in every
# respect but one:
#
#   images-default   the flood at the widget's own budget
#   images-<n>       the same flood at a caller-set budget
#   images-off       the same flood at a budget of zero
#   text             a byte-matched flood of ordinary text
#
# images-off is the control that matters. A budget of zero does not refuse to
# parse a sixel, it evicts the image the moment it is placed (see
# Terminal::set_image_limit), so that phase pushes the SAME bytes through the
# SAME parser, occupies the SAME cells and scrolls the SAME rows away, and
# differs from images-default in exactly one thing: whether the pixels stay
# resident. Its distance from the budgeted phases is therefore the cost of the
# pictures with nothing else in it - no font, no surface, no scrollback.
#
# The text phase is the cruder control, and it is here because the image
# phases share a suspicion the text phase does not: that what is being measured
# is the parser or the wire rather than the pixels.
#
# What the numbers have to show:
#
#   - a PLATEAU. Pss at the last preview minus Pss at the middle checkpoint is
#     about zero, because the budget is a ceiling and not a rate.
#   - a CEILING THAT MOVES. images-default minus images-off is within the
#     default budget; images-<n> minus images-off is within <n>. If the two
#     came out the same the knob would be decorative.
#   - a REUSE. The flood ends with RIS and is then run again. Pss after the
#     second flood is where the first one left it, not a budget higher, which
#     is the only way to tell a released chunk the allocator is sitting on from
#     one the terminal never let go of.
#
# The disk side of the ceiling is reported too. Evicted pixels are appended to
# the image stream rather than dropped, so they cost unlinked temporary file
# instead of RAM, and a bound that only holds because it moved the problem to
# somewhere unmeasured is not a bound. The stream is an O_TMPFILE fd, so it
# shows up in /proc/<pid>/fd as "(deleted)" and can be stat'ed.
#
# Exits 77 when the tools to run it are absent. Deliberately not a meson test:
# a run takes about twenty minutes and writes half a gigabyte of temporary
# files, which is a measurement, not a gate.
#
# usage: run.sh <vte-app> [previews] [mid-checkpoint] [image-limit-bytes]

set -u

HERE=$(cd "$(dirname "$0")" && pwd)

APP=${1:?vte app binary}
PREVIEWS=${2:-300}
MID=${3:-100}
LIMIT=${4:-8388608}

# Seconds to let the widget drain the pty after the last byte is written. cat
# returning only says the bytes left the writer; VTE decodes on its own
# schedule, and sampling before it has caught up measures the queue.
SETTLE=${VTE_MEM_SETTLE:-20}

# Big enough that a 800x480 preview fits with room under it on any plausible
# cell, and small enough that the drawing surface stays a rounding error.
COLS=100
ROWS=40
PAD=48
IMAGE_W=800
IMAGE_H=480

FONT=${VTE_TEST_FONT:-Monospace 12}

for tool in Xvfb img2sixel convert stat awk; do
        command -v "$tool" >/dev/null 2>&1 || {
                echo "SKIP: $tool not available"
                exit 77
        }
done
[ -x "$APP" ] || { echo "SKIP: $APP not executable"; exit 77; }
[ -r /proc/self/smaps_rollup ] || { echo "SKIP: no smaps_rollup"; exit 77; }

WORK=$(mktemp -d)
XPID=
APID=
cleanup() {
        [ -n "$APID" ] && kill "$APID" 2>/dev/null
        [ -n "$XPID" ] && kill "$XPID" 2>/dev/null
        wait 2>/dev/null
        rm -rf "$WORK"
}
trap cleanup EXIT

say() { printf '%s\n' "$*"; }

# Pss in KiB. One line, first match: smaps_rollup already sums the mappings.
pss() { awk '/^Pss:/ { print $2; exit }' "/proc/$1/smaps_rollup" 2>/dev/null; }

# Total bytes held in unlinked temporary files the process still has open.
# _vte_mkstemp() opens the ring's streams with O_TMPFILE, so this is the whole
# of what the terminal has pushed out of RAM and onto disk.
deleted_fd_bytes() {
        local pid=$1 fd target total=0 size
        for fd in "/proc/$pid/fd"/*; do
                target=$(readlink "$fd" 2>/dev/null) || continue
                case "$target" in
                *'(deleted)') ;;
                *) continue ;;
                esac
                size=$(stat -Lc %s "$fd" 2>/dev/null) || continue
                total=$((total + size))
        done
        printf '%s' "$total"
}

deleted_fd_largest() {
        local pid=$1 fd target largest=0 size
        for fd in "/proc/$pid/fd"/*; do
                target=$(readlink "$fd" 2>/dev/null) || continue
                case "$target" in
                *'(deleted)') ;;
                *) continue ;;
                esac
                size=$(stat -Lc %s "$fd" 2>/dev/null) || continue
                [ "$size" -gt "$largest" ] && largest=$size
        done
        printf '%s' "$largest"
}

mib() { awk -v k="$1" 'BEGIN { printf "%.1f", k / 1024 }'; }
mib_b() { awk -v b="$1" 'BEGIN { printf "%.1f", b / 1048576 }'; }

wait_for() {
        local path=$1 limit=$2 waited=0
        while [ ! -e "$path" ]; do
                sleep 1
                waited=$((waited + 1))
                if [ "$waited" -ge "$limit" ]; then
                        return 1
                fi
                if [ $((waited % 15)) = 0 ]; then
                        say "    ... ${waited}s, frame $(cat "$WORK/run/progress" 2>/dev/null || echo 0)/$PREVIEWS, Pss $(mib "$(pss "$APID")") MiB"
                fi
        done
        return 0
}

# One frame is a preview followed by enough blank lines to push it entirely
# above the viewport.
#
# The padding is what makes this a test of SCROLLED-AWAY images rather than of
# one image being replaced over and over. A flood that leaves each preview
# where the last one was would keep exactly one image resident and never
# approach any budget, and would still look like a 300-image flood from the
# outside. Every frame therefore lands on rows no other frame ever occupies,
# and the run asserts afterwards that the spill stream grew accordingly.
build_image_corpus() {
        local dir=$WORK/corpus-image i r g b
        mkdir -p "$dir"
        say "building $PREVIEWS distinct ${IMAGE_W}x${IMAGE_H} previews"

        local pad
        pad=$(awk -v n="$PAD" 'BEGIN { while (n-- > 0) printf "\n" }')

        for ((i = 1; i <= PREVIEWS; i++)); do
                # Distinct content per frame. Nothing in the tree hashes image
                # data, so identical previews would account identically - but a
                # measurement that would not notice if they did is worth less
                # than one that would.
                r=$(((i * 37) % 256))
                g=$(((i * 91) % 256))
                b=$(((i * 149) % 256))
                convert -size "${IMAGE_W}x${IMAGE_H}" plasma:fractal \
                        -modulate 100,120,"$((r % 200))" \
                        -fill "rgb($r,$g,$b)" -colorize 25% \
                        -depth 8 ppm:- 2>/dev/null |
                        img2sixel -p 64 >"$dir/frame-$i" 2>/dev/null
                [ -s "$dir/frame-$i" ] || { echo "FAIL: could not build preview $i"; exit 1; }
                printf '%s\n' "$pad" >>"$dir/frame-$i"
                if [ $((i % 25)) = 0 ]; then
                        say "  ... $i/$PREVIEWS"
                fi
        done
}

# The text control is matched frame for frame on BYTES, not on rows. Matching
# rows instead would need the font's cell height, which is a property of
# whatever fontconfig resolved on the machine, and pinning the comparison to
# that would make the control drift with the font. Bytes are what both floods
# actually put through the pty.
build_text_corpus() {
        local dir=$WORK/corpus-text i size
        mkdir -p "$dir"
        say "building the byte-matched text control"

        for ((i = 1; i <= PREVIEWS; i++)); do
                size=$(stat -c %s "$WORK/corpus-image/frame-$i")
                LC_ALL=C tr -dc 'a-zA-Z0-9 ' </dev/urandom 2>/dev/null |
                        head -c "$size" | fold -w $((COLS - 1)) >"$dir/frame-$i"
        done
}

# Results, one line per phase:
# label baseline mid full reset reuse spill largest
RESULTS=$WORK/results
: >"$RESULTS"

run_phase() {
        local label=$1 corpus=$2 limit=$3
        local limit_arg=()
        [ -n "$limit" ] && limit_arg=(--image-limit "$limit")

        say ""
        say "phase $label (corpus $(basename "$corpus"), limit ${limit:-default})"

        rm -rf "$WORK/run"
        mkdir -p "$WORK/run"

        # Let X pick a free display and tell us which, the way the render
        # tests do: guessing a number is how those first went flaky.
        Xvfb -displayfd 3 -screen 0 1600x1200x24 -nolisten tcp 3>"$WORK/run/display" >/dev/null 2>&1 &
        XPID=$!
        local _
        for _ in $(seq 1 100); do
                [ -s "$WORK/run/display" ] && break
                sleep 0.1
        done
        local disp
        disp=$(cat "$WORK/run/display" 2>/dev/null)
        [ -n "$disp" ] || { echo "SKIP: Xvfb did not report a display"; exit 77; }

        export DISPLAY=":$disp"
        export GDK_BACKEND=x11
        export GSK_RENDERER=${VTE_TEST_RENDERER:-cairo}
        export LIBGL_ALWAYS_SOFTWARE=1
        export VTE_SIXEL=1
        export VTE_MEM_WORK=$WORK/run
        export VTE_MEM_CORPUS=$corpus
        export VTE_MEM_PREVIEWS=$PREVIEWS
        export VTE_MEM_MID=$MID
        export VTE_MEM_SETTLE=$SETTLE

        "$APP" --no-decorations --geometry "${COLS}x${ROWS}" --font "$FONT" \
                "${limit_arg[@]}" -- /bin/sh "$HERE/child.sh" \
                >"$WORK/run/app.log" 2>&1 &
        APID=$!

        wait_for "$WORK/run/boot" 60 || { echo "FAIL: the child never started"; exit 1; }

        # Settle before the baseline. The first frames realise the font, the
        # glyph cache and the drawing surface, and charging those to the images
        # would inflate every delta below by the same amount.
        sleep "$SETTLE"
        local baseline
        baseline=$(pss "$APID")
        say "  baseline Pss $(mib "$baseline") MiB"

        : >"$WORK/run/go"

        wait_for "$WORK/run/mark-mid" 5400 || { echo "FAIL: never reached preview $MID"; exit 1; }
        local at_mid spill_mid
        at_mid=$(pss "$APID")
        spill_mid=$(deleted_fd_bytes "$APID")
        say "  at $MID: Pss $(mib "$at_mid") MiB, unlinked temporaries $(mib_b "$spill_mid") MiB"
        : >"$WORK/run/go-mid"

        wait_for "$WORK/run/mark-full" 5400 || { echo "FAIL: never reached preview $PREVIEWS"; exit 1; }
        local at_full spill_full spill_largest
        at_full=$(pss "$APID")
        spill_full=$(deleted_fd_bytes "$APID")
        spill_largest=$(deleted_fd_largest "$APID")
        say "  at $PREVIEWS: Pss $(mib "$at_full") MiB, unlinked temporaries $(mib_b "$spill_full") MiB (largest $(mib_b "$spill_largest") MiB)"
        : >"$WORK/run/go-full"

        wait_for "$WORK/run/mark-reset" 600 || { echo "FAIL: never came back from RIS"; exit 1; }
        local at_reset
        at_reset=$(pss "$APID")
        say "  after RIS: Pss $(mib "$at_reset") MiB"
        : >"$WORK/run/go-reset"

        wait_for "$WORK/run/mark-reuse" 5400 || { echo "FAIL: never finished the second flood"; exit 1; }
        local at_reuse
        at_reuse=$(pss "$APID")
        say "  reflooded to $MID: Pss $(mib "$at_reuse") MiB"

        printf '%s %s %s %s %s %s %s %s\n' \
                "$label" "$baseline" "$at_mid" "$at_full" "$at_reset" "$at_reuse" \
                "$spill_full" "$spill_largest" >>"$RESULTS"

        kill "$APID" 2>/dev/null; APID=
        kill "$XPID" 2>/dev/null; XPID=
        wait 2>/dev/null
}

build_image_corpus
build_text_corpus

IMAGE_BYTES=$(cat "$WORK/corpus-image"/frame-* | wc -c)
TEXT_BYTES=$(cat "$WORK/corpus-text"/frame-* | wc -c)
say "image corpus $(mib_b "$IMAGE_BYTES") MiB, text corpus $(mib_b "$TEXT_BYTES") MiB"

run_phase "images-default" "$WORK/corpus-image" ""
run_phase "images-$LIMIT" "$WORK/corpus-image" "$LIMIT"
run_phase "images-off" "$WORK/corpus-image" "0"
run_phase "text" "$WORK/corpus-text" ""

say ""
say "phase             baseline  at-$MID  at-$PREVIEWS  post-RIS  reflood  on-disk  biggest"
awk '
{
        printf "%-18s %7.1f %8.1f %8.1f %8.1f %8.1f %9.1f %8.1f\n",
               $1, $2/1024, $3/1024, $4/1024, $5/1024, $6/1024,
               $7/1048576, $8/1048576
}' "$RESULTS"
say "                  (MiB Pss; then all unlinked temporaries, and the largest"
say "                   single one, which in an image phase is the image stream)"

# The other end of the same bound. What is NOT on disk is what is in the
# budget: pixels are spilled on eviction, so the shortfall between the image
# stream and the raw pixels the flood contained is what was still resident.
# It is an independent path to the ceiling - kernel Pss on one side, a file
# size on the other - and the two have to agree.
say ""
awk -v raw="$((PREVIEWS * IMAGE_W * IMAGE_H * 4))" '
$1 ~ /^images/ {
        printf "  %-18s image stream %7.1f MiB, so %5.1f MiB of pixels never left RAM\n",
               $1, $8/1048576, (raw - $8) / 1048576
}' "$RESULTS"

say ""
say "derived:"
awk -v previews="$PREVIEWS" -v mid="$MID" -v limit="$LIMIT" '
{
        order[++n] = $1
        m[$1] = $3; f[$1] = $4; r[$1] = $5; u[$1] = $6
}
END {
        for (i = 1; i <= n; i++) {
                p = order[i]
                printf "  %-18s plateau (at-%d minus at-%d)  %+8.1f MiB\n",
                       p, previews, mid, (f[p] - m[p]) / 1024
        }
        off = f["images-off"]
        printf "  resident pixels at the default budget   %+8.1f MiB\n",
               (f["images-default"] - off) / 1024
        printf "  resident pixels at a %.0f MiB budget      %+8.1f MiB\n",
               limit / 1048576, (f["images-" limit] - off) / 1024
        printf "  images-off minus the text control       %+8.1f MiB\n",
               (off - f["text"]) / 1024
        for (i = 1; i <= n; i++) {
                p = order[i]
                printf "  %-18s returned by RIS  %+8.1f MiB, second flood to %d ends %+8.1f MiB above the first\n",
                       p, (f[p] - r[p]) / 1024, mid, (u[p] - m[p]) / 1024
        }
}' "$RESULTS"

# The fixture assertion, and it comes first because everything above is
# meaningless without it. If the previews had piled onto the same rows, one
# image would have been resident throughout, nothing would ever have been
# evicted, and the image stream would be about the size of a single preview.
# It has to be within reach of previews x width x height x 4 instead.
EXPECT_SPILL=$((PREVIEWS * IMAGE_W * IMAGE_H * 4))
GOT_SPILL=$(awk '$1 == "images-off" { print $8 }' "$RESULTS")
say ""
say "fixture: the image stream holds $(mib_b "$GOT_SPILL") MiB against $(mib_b "$EXPECT_SPILL") MiB of raw pixels for $PREVIEWS previews"
if [ "$GOT_SPILL" -lt $((EXPECT_SPILL / 2)) ]; then
        say "FAIL: too little was spilled for $PREVIEWS previews to have been resident and evicted"
        exit 1
fi

exit 0
