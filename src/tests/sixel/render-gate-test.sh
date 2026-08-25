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
# Asserts that the golden-frame render test tells right frames from wrong ones.
#
# The render cases are worth their runtime only if their verdict DISCRIMINATES,
# and that is not the same claim as "the runner can fail". A runner was once
# shipped whose verdict came from a trim box - a number that reads 0x0 both
# when the frames agree and when every pixel of them differs - and a guard that
# only broke the comparator found nothing wrong with it, because a comparator
# that refuses to run does fail loudly on any verdict at all. Every render case
# was passing against a frame that was entirely wrong.
#
# So drive the real runner five times, once per way the answer can be wrong,
# and require it to answer each one on its own terms:
#
#   - the frame it captured itself passes;
#   - a frame with ONE pixel changed fails;
#   - a frame with EVERY pixel changed fails;
#   - a frame one column narrower than the golden fails;
#   - a comparison that could not be MADE fails, and says so rather than
#     blaming the renderer.
#
# The first is not a duplicate of the render cases. The four below it all read
# "the runner said no", and a runner that said no to everything would satisfy
# all four; the passing run is what makes their agreement mean anything.
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

# The region of the capture the runner reads its verdict from, resolved the way
# the runner resolves it. Damage aimed anywhere else would leave the compared
# frame right, and the scenario would be measuring nothing.
#
# The default is stated twice, here and in the runner. Should the two ever
# drift apart, the changed-pixel counts below stop coming out at the intended
# value and every scenario says so by name.
if [ -r "$FIXTURES/$CASE.crop" ]; then
        CROP=$(cat "$FIXTURES/$CASE.crop")
else
        CROP=320x130+0+0
fi
[[ $CROP =~ ^([0-9]+)x([0-9]+)\+([0-9]+)\+([0-9]+)$ ]] ||
        { echo "FAIL: unreadable crop geometry '$CROP'"; exit 1; }
CROP_W=${BASH_REMATCH[1]}
CROP_X=${BASH_REMATCH[3]}
CROP_Y=${BASH_REMATCH[4]}

# How many pixels the runner ends up comparing, taken from the golden itself.
GOLDEN_PIXELS=$("$REAL_CONVERT" "$GOLDEN" -format '%[fx:w*h]' info: 2>/dev/null) ||
        { echo "FAIL: could not measure the golden $GOLDEN"; exit 1; }

# ImageMagick counts through fx and renders whole numbers however it likes, so
# every count here is compared as a number and never as a string.
is_number() { awk -v x="$1" -v y="$2" 'BEGIN { exit !(x == y) }'; }

# Build a PATH directory whose `import` captures the screen for real and then
# damages the capture in a defined way, so the runner reaches its verdict on a
# frame that is wrong by a KNOWN amount.
#
# The shim records what it did, and every scenario reads that back before it
# reads the verdict. Without it a scenario whose damage silently missed - an
# operator ImageMagick declined, a pixel outside the compared region - looks
# exactly like a runner that cannot fail, which is the failure this whole file
# exists to keep out.
#
#   $1 marker file to write
#   $2 directory to build, created here
#   $3 ImageMagick operators to damage the capture with, empty to leave it be
make_capture_shim() {
        local marker=$1 dir=$2 ops=$3
        local damage=:

        [ -z "$ops" ] || damage="\"$REAL_CONVERT\" \"\$shot\" $ops \"\$shot\""

        mkdir -p "$dir"
        cat >"$dir/import" <<EOF
#!/usr/bin/env bash
set -e
"$REAL_IMPORT" "\$@"
# The last argument is the file import was asked to write.
for shot; do :; done
cp "\$shot" "\$shot.as-captured"
$damage
{
        "$REAL_CONVERT" "\$shot" -format 'geometry %wx%h\n' info:
        # A difference is only countable where the two agree on a shape.
        if [ "\$("$REAL_CONVERT" "\$shot" -format '%wx%h' info:)" = \\
             "\$("$REAL_CONVERT" "\$shot.as-captured" -format '%wx%h' info:)" ]; then
                "$REAL_CONVERT" "\$shot.as-captured" "\$shot" \\
                        -compose difference -composite -crop $CROP +repage \\
                        -colorspace Gray -threshold 0 \\
                        -format 'changed %[fx:mean*w*h]\n' info:
        fi
} >"$marker"
EOF
        chmod +x "$dir/import"
}

# What the shim wrote, or nothing at all if it never ran.
shim_geometry() { awk '$1 == "geometry" { print $2 }' "$1" 2>/dev/null; }
shim_changed() { awk '$1 == "changed" { print $2 }' "$1" 2>/dev/null; }

# Run the real runner with $1 prepended to PATH. Leaves the runner's output in
# $WORK/out and its exit status in $STATUS.
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

# Report a scenario that came out wrong, with the runner's own words under it.
verdict_failed() {
        echo "FAIL: $1"
        sed 's/^/  /' "$WORK/out"
        exit 1
}

# Scenario 1: the frame the runner captured itself.
#
# Nothing is damaged, so the only thing that can make this fail is the runner.
# It is the control the four scenarios below stand on: they each require a NO,
# and a runner that answers no unconditionally would give them all one.
UNTOUCHED="$WORK/untouched-capture"
UNTOUCHED_MARKER="$WORK/capture-untouched"
make_capture_shim "$UNTOUCHED_MARKER" "$UNTOUCHED" ""

run_runner "$UNTOUCHED"
skip_if_skipped

[ -n "$(shim_geometry "$UNTOUCHED_MARKER")" ] ||
        verdict_failed "no frame was captured, so the run below it proves nothing"
is_number "$(shim_changed "$UNTOUCHED_MARKER")" 0 ||
        verdict_failed "the capture was altered on the run that was meant to leave it alone"

[ "$STATUS" = 0 ] ||
        verdict_failed "the render test rejects the frame it captured itself"

case "$(cat "$WORK/out")" in
        *"renders identically to the golden"*) ;;
        *) verdict_failed "a matching frame was not reported as matching" ;;
esac

# Scenario 2: the comparison runs, on a frame wrong in a single pixel.
#
# The smallest difference there is, and the one a verdict drifts into
# tolerating: a fuzz factor, a threshold, a metric that answers "close enough".
# An image is drawn one pixel out of place by exactly this much.
ONE_PIXEL="$WORK/one-pixel-wrong"
ONE_PIXEL_MARKER="$WORK/capture-one-pixel"
make_capture_shim "$ONE_PIXEL_MARKER" "$ONE_PIXEL" \
        "-region 1x1+$((CROP_X + 1))+$((CROP_Y + 1)) -negate"

run_runner "$ONE_PIXEL"
skip_if_skipped

is_number "$(shim_changed "$ONE_PIXEL_MARKER")" 1 ||
        verdict_failed "the capture differs in $(shim_changed "$ONE_PIXEL_MARKER") pixels of the compared region, not the one intended"

[ "$STATUS" != 0 ] ||
        verdict_failed "the render test passed on a frame with one pixel changed"

case "$(cat "$WORK/out")" in
        *"differs from the golden"*) ;;
        *) verdict_failed "a frame with one pixel changed was not reported as differing" ;;
esac

# Scenario 3: the comparison runs, on a frame that is wrong everywhere.
#
# Negating the capture is the bluntest possible wrong frame - same geometry,
# every pixel differs - so a verdict that lets this through is not measuring
# pixels at all. It is also precisely what the trim-box verdict let through.
NEGATED="$WORK/negated-capture"
NEGATED_MARKER="$WORK/capture-negated"
make_capture_shim "$NEGATED_MARKER" "$NEGATED" "-negate"

run_runner "$NEGATED"
skip_if_skipped

is_number "$(shim_changed "$NEGATED_MARKER")" "$GOLDEN_PIXELS" ||
        verdict_failed "negating the capture changed $(shim_changed "$NEGATED_MARKER") of the $GOLDEN_PIXELS compared pixels, not all of them"

[ "$STATUS" != 0 ] ||
        verdict_failed "the render test passed on a frame with every pixel inverted"

case "$(cat "$WORK/out")" in
        *"differs from the golden"*) ;;
        *) verdict_failed "an inverted frame was not reported as differing" ;;
esac

# Scenario 4: the comparison would run, on a frame of the wrong size.
#
# Take one column off the capture, so the runner's crop comes out one column
# narrower than the golden. This is the wrong frame a difference metric cannot
# see: it measures the overlap of the two, and calls the narrower frame equal
# to the golden it is a prefix of. Only a size check catches it, and a size
# check nothing exercises is a size check that can be deleted.
NARROW="$WORK/narrow-capture"
NARROW_MARKER="$WORK/capture-narrowed"
make_capture_shim "$NARROW_MARKER" "$NARROW" \
        "-crop $((CROP_X + CROP_W - 1))x+0+0 +repage"

run_runner "$NARROW"
skip_if_skipped

NARROW_GEOMETRY=$(shim_geometry "$NARROW_MARKER")
case "$NARROW_GEOMETRY" in
        "$((CROP_X + CROP_W - 1))x"*) ;;
        *) verdict_failed "the capture came out $NARROW_GEOMETRY, so it was not narrowed to $((CROP_X + CROP_W - 1)) columns" ;;
esac

[ "$STATUS" != 0 ] ||
        verdict_failed "the render test passed on a frame one column narrower than the golden"

# A size mismatch is not a rendering difference: the compared region is fixed,
# so a frame that does not fill it says the capture went wrong.
case "$(cat "$WORK/out")" in
        *"where the golden is"*) ;;
        *) verdict_failed "a frame of the wrong size was not reported as one" ;;
esac

# Scenario 5: the comparison cannot run.
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

[ -r "$BROKEN_MARKER" ] ||
        verdict_failed "the run never reached the comparison, so nothing was broken"

[ "$STATUS" != 0 ] ||
        verdict_failed "the render test passed with its comparison broken"

# And it has to say which of the two things went wrong. "Differs from the
# golden" would send a reader looking for a rendering bug that is not there.
case "$(cat "$WORK/out")" in
        *"could not be compared"*) ;;
        *) verdict_failed "a broken comparison was not reported as one" ;;
esac

echo "PASS: the frame the render test captures itself passes it"
echo "PASS: a frame with one changed pixel fails the render test"
echo "PASS: a frame with every pixel inverted fails the render test"
echo "PASS: a frame one column narrower than the golden fails the render test"
echo "PASS: a comparison that cannot run fails the render test"
exit 0
