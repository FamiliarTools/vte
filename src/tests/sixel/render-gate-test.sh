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
# Asserts that the golden-frame render test can fail.
#
# Two different things have to be true of that runner, and only one of them
# used to be checked here:
#
#   - a comparison that could not be MADE is reported as such, rather than
#     read as an empty difference and printed as PASS;
#   - the verdict DISCRIMINATES, so a frame that does not match the golden
#     fails even though the comparison ran fine.
#
# The second is the one that matters, and leaving it unchecked is what let the
# runner ship a verdict taken from a trim box - a number that reads 0x0 both
# when the frames agree and when every pixel of them differs. Every render
# case passed against a frame that was entirely wrong. So drive the runner
# with a deliberately wrong capture and require it to say so.
#
# Both scenarios run the real runner end to end and corrupt one step of it
# from PATH, so the run reaches the verdict with a real captured frame; a
# marker file proves it got that far.
#
# usage: render-gate-test.sh <vte-app> <srcdir> <case> <gtk-arm>

set -u

APP=${1:?vte app binary}
SRCDIR=${2:?source dir}
CASE=${3:?case name}
ARM=${4:?gtk arm}

RUNNER="$SRCDIR/render-test.sh"
[ -x "$RUNNER" ] || { echo "FAIL: no runner $RUNNER"; exit 1; }

REAL_CONVERT=$(command -v convert 2>/dev/null) || REAL_CONVERT=
[ -n "$REAL_CONVERT" ] || { echo "SKIP: convert not available"; exit 77; }
REAL_IMPORT=$(command -v import 2>/dev/null) || REAL_IMPORT=
[ -n "$REAL_IMPORT" ] || { echo "SKIP: import not available"; exit 77; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Hand the runner a scratch copy of the fixtures rather than the source tree.
# A failing render case writes the frame it actually got next to the golden,
# and this test exists to make it fail: pointed at the real directory it would
# leave that artefact behind on every green run.
FIXTURES="$WORK/fixtures"
mkdir "$FIXTURES"
GOLDEN="$SRCDIR/$CASE.golden-$ARM.png"
[ -r "$SRCDIR/$CASE.six" ] || { echo "FAIL: no fixture $SRCDIR/$CASE.six"; exit 1; }
[ -r "$GOLDEN" ] || { echo "FAIL: no golden $GOLDEN"; exit 1; }
cp "$SRCDIR/$CASE.six" "$GOLDEN" "$FIXTURES/" || exit 1
[ -r "$SRCDIR/$CASE.crop" ] && { cp "$SRCDIR/$CASE.crop" "$FIXTURES/" || exit 1; }

# Run the real runner with $1 prepended to PATH. Leaves the runner's output in
# $OUT and its exit status in $STATUS.
run_runner() {
        PATH="$1:$PATH" "$RUNNER" "$APP" "$FIXTURES" "$CASE" "$ARM" >"$WORK/out" 2>&1
        STATUS=$?
}

# A skip is a skip whichever scenario hit it: the runner decides that from the
# tools and the display, which this test cannot supply.
skip_if_skipped() {
        [ "$STATUS" = 77 ] || return 0
        echo "SKIP: the render test cannot run here"
        sed 's/^/  /' "$WORK/out"
        exit 77
}

# Scenario 1: the comparison cannot run.
#
# Refuse the one command the verdict is read from, so everything before it -
# the capture, the crop, the normalisation - still works and the runner
# arrives at the comparison with a frame worth comparing. Exit 2 is what
# ImageMagick uses for a compare that could not run, as against 1 for one that
# ran and found a difference.
BROKEN="$WORK/broken-comparator"
mkdir "$BROKEN"
BROKEN_MARKER="$WORK/comparison-reached"

cat >"$BROKEN/compare" <<EOF
#!/usr/bin/env bash
echo reached >"$BROKEN_MARKER"
echo "stub compare: refusing to compare" >&2
exit 2
EOF
chmod +x "$BROKEN/compare"

run_runner "$BROKEN"
skip_if_skipped

[ -r "$BROKEN_MARKER" ] || {
        echo "FAIL: the run never reached the comparison, so nothing was broken"
        sed 's/^/  /' "$WORK/out"
        exit 1
}

[ "$STATUS" != 0 ] || {
        echo "FAIL: the render test passed with its comparison broken"
        sed 's/^/  /' "$WORK/out"
        exit 1
}

# And it has to say which of the two things went wrong. "Differs from the
# golden" would send a reader looking for a rendering bug that is not there.
case "$(cat "$WORK/out")" in
        *"could not be compared"*) ;;
        *)
                echo "FAIL: a broken comparison was not reported as one"
                sed 's/^/  /' "$WORK/out"
                exit 1
                ;;
esac

# Scenario 2: the comparison runs, on a frame that is wrong everywhere.
#
# Negating the capture is the bluntest possible wrong frame - same geometry,
# every pixel differs - so a verdict that lets this through is not measuring
# pixels at all. It is also precisely what the trim-box verdict let through.
WRONG="$WORK/wrong-capture"
mkdir "$WRONG"
WRONG_MARKER="$WORK/capture-negated"

cat >"$WRONG/import" <<EOF
#!/usr/bin/env bash
# Capture for real, then invert what was captured. The last argument is the
# file import was asked to write.
set -e
"$REAL_IMPORT" "\$@"
for shot; do :; done
"$REAL_CONVERT" "\$shot" -negate "\$shot"
echo "\$shot" >"$WRONG_MARKER"
EOF
chmod +x "$WRONG/import"

run_runner "$WRONG"
skip_if_skipped

[ -s "$WRONG_MARKER" ] || {
        echo "FAIL: the capture was never negated, so the frame compared was not wrong"
        sed 's/^/  /' "$WORK/out"
        exit 1
}

[ "$STATUS" != 0 ] || {
        echo "FAIL: the render test passed on a frame with every pixel inverted"
        sed 's/^/  /' "$WORK/out"
        exit 1
}

# The right reason, again: a wrong frame must be reported as a difference, not
# as a comparison that could not be made.
case "$(cat "$WORK/out")" in
        *"differs from the golden"*) ;;
        *)
                echo "FAIL: an inverted frame was not reported as differing"
                sed 's/^/  /' "$WORK/out"
                exit 1
                ;;
esac

echo "PASS: a comparison that cannot run fails the render test"
echo "PASS: a frame with every pixel inverted fails the render test"
exit 0
