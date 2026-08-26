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
# that refuses to run does fail loudly on any verdict at all. The frames those
# render cases captured were in fact correct; what was missing was any evidence
# that the verdict could have told if they had not been. The one run that did
# pass against an entirely wrong frame was the deliberate probe: the golden
# compared against its own negative passed.
#
# So drive the real runner once per way the answer can be wrong, and require it
# to answer each one on its own terms:
#
#   - the frame it captured itself passes;
#   - a frame with ONE pixel changed fails;
#   - a frame with EVERY pixel changed fails;
#   - a frame one column narrower than the golden fails;
#   - a comparator that could not RUN fails, and says so rather than blaming
#     the renderer;
#   - a comparator that ran and answered a count of the wrong SHAPE fails, and
#     is named apart from the one above it;
#   - a comparator whose count and exit status CONTRADICT each other fails
#     rather than passing on the count, and is named apart from a rendering
#     difference when it fails;
#   - a failing run keeps its frame where it was told to and never among the
#     fixtures;
#   - an app whose sixel default is off still renders, because the runner asks
#     for --sixel;
#   - a run that timed out waiting for the TERMINAL says so, and is not
#     reported as a difference from the golden;
#   - the case the build documentation points a builder at as its font sentinel
#     really is font-invariant, under fonts the run itself proves reached the
#     terminal.
#
# The first is not a duplicate of the render cases. The failing ones all read
# "the runner said no", and a runner that said no to everything would satisfy
# every one of them; the passing run is what makes their agreement mean
# anything.
#
# usage: render-gate-test.sh <vte-app> <srcdir> <case> <gtk-arm> <font-sensitive-case>

set -u

APP=${1:?vte app binary}
SRCDIR=${2:?source dir}
CASE=${3:?case name}
ARM=${4:?gtk arm}
# Required, like the four above it. This was once read as ${5:-} with the whole
# font scenario inside an `if`, which meant a caller that stopped passing it -
# an edited meson.build, a hand invocation - would lose that scenario in
# silence while the run still printed PASS for the ten around it.
SENSITIVE=${5:?font-sensitive case name}

# Resolved absolutely. One scenario below runs the runner from a different
# working directory - that is the whole of what it measures - and a relative
# path handed in would not survive the cd.
APP=$(cd "$(dirname "$APP")" 2>/dev/null && pwd)/$(basename "$APP")
SRCDIR=$(cd "$SRCDIR" 2>/dev/null && pwd)

RUNNER="$SRCDIR/render-test.sh"
[ -x "$RUNNER" ] || { echo "FAIL: no runner $RUNNER"; exit 1; }

REAL_CONVERT=$(command -v convert 2>/dev/null) || REAL_CONVERT=
[ -n "$REAL_CONVERT" ] || { echo "SKIP: convert not available"; exit 77; }
REAL_IMPORT=$(command -v import 2>/dev/null) || REAL_IMPORT=
[ -n "$REAL_IMPORT" ] || { echo "SKIP: import not available"; exit 77; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Hand the runner a scratch copy of the fixtures rather than the source tree.
# This test exists to make the runner fail, repeatedly, on every green run;
# nothing it does should be able to touch the checked-in fixtures. The frames
# those failures keep are steered into $WORK by run_runner below.
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

# Run the real runner with $1 prepended to PATH, against $2 as the app when
# given and the real one otherwise. Leaves the runner's output in $WORK/out and
# its exit status in $STATUS.
run_runner() {
        PATH="$1:$PATH" VTE_TEST_ARTIFACT_DIR="$WORK" \
                "$RUNNER" "${2:-$APP}" "$FIXTURES" "$CASE" "$ARM" >"$WORK/out" 2>&1
        STATUS=$?
}

# An empty directory, for the runs that damage nothing on PATH.
NO_SHIM="$WORK/no-shim"
mkdir "$NO_SHIM"

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

# Where the frame that failure kept was written.
#
# The runner keeps the actual frame so a reader can look at it, and WHERE it
# keeps it is a guard in its own right: $SRCDIR is the checked-in fixture
# directory, so writing there dirties a developer's working copy and is simply
# refused on the read-only srcdir a distributor builds from. That the frame is
# kept at all is deliberate and should stay - what must never happen is it
# being kept in the source tree.
#
# This costs no extra run: the scenario above has just driven the runner to a
# failure, which is exactly the state that writes one. It holds the override -
# run_runner passes VTE_TEST_ARTIFACT_DIR, and a runner that ignored it would
# leave nothing here.
ARTIFACT="$WORK/$CASE-$ARM.actual.png"
[ -s "$ARTIFACT" ] ||
        verdict_failed "the failing frame was not written to the directory VTE_TEST_ARTIFACT_DIR named"

# And nothing was written among the fixtures. These are a scratch copy, so this
# stands in for the checked-in ones the real runs are pointed at.
[ -z "$(find "$FIXTURES" -name '*.actual.png' -print -quit)" ] ||
        verdict_failed "the failing frame was written into the fixture directory"

# The same guard's other half: with no override in the environment the frame
# goes to the working directory - which is the build tree for a test meson
# runs - and still not to the fixtures beside the golden.
ARTIFACT_CWD="$WORK/cwd"
mkdir "$ARTIFACT_CWD"
(
        cd "$ARTIFACT_CWD" &&
                PATH="$ONE_PIXEL:$PATH" "$RUNNER" "$APP" "$FIXTURES" "$CASE" "$ARM"
) >"$WORK/out" 2>&1
STATUS=$?
skip_if_skipped

[ "$STATUS" != 0 ] ||
        verdict_failed "the render test passed on a frame with one pixel changed"
[ -s "$ARTIFACT_CWD/$CASE-$ARM.actual.png" ] ||
        verdict_failed "with no artifact directory given, the failing frame was not written to the working directory"
[ -z "$(find "$FIXTURES" -name '*.actual.png' -print -quit)" ] ||
        verdict_failed "with no artifact directory given, the failing frame was written into the fixture directory"

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

# Scenarios 5 and 6: the comparison answers something that is not a verdict.
#
# The runner reads the comparator TWICE - the exit status, and the count on
# stderr - and refuses to rule on either alone. The two scenarios below drive
# one of those refusals each.
#
# They assert the runner's exact words, not merely that it failed, because the
# words are the whole of what these two mechanisms decide. Measured on gtk3, by
# deleting each from the runner on its own and running this file: with the exit
# status check gone the scenario 5 run still failed, on the count's shape; with
# the count's shape check gone the scenario 6 run still failed, on the exit
# status disagreeing with the count. Neither deletion produced a passing run
# here - what it produced was the other mechanism's message, naming a different
# thing as broken. So each scenario pins the message its own mechanism gives.
#
# Refusing the comparator outright still lets everything before it - the
# capture, the crop, the normalisation - work, so the runner arrives at the
# comparison with a frame worth comparing.

# Scenario 5: the comparator exits as one that could not run.
#
# Exit 2 is what ImageMagick uses for a compare that could not run, as against
# 1 for one that ran and found a difference. Nothing usable is written to
# stderr either, which is the real shape of the failure.
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
        *"the two frames could not be compared"*) ;;
        *) verdict_failed "a comparator that could not run was not reported as one" ;;
esac

# Scenario 6: the comparator succeeds and answers a count that is not a number.
#
# This is the failure the exit status cannot see: compare exits 0, so by that
# channel the run went fine, and the only thing wrong is the count itself. It
# happens whenever the last line of ImageMagick's stderr is not the metric - a
# warning printed after it, a build that words the metric differently - and a
# runner that carried on would be reading a verdict out of that word.
SHAPELESS="$WORK/shapeless-count"
mkdir "$SHAPELESS"
SHAPELESS_MARKER="$WORK/shapeless-reached"

cat >"$SHAPELESS/compare" <<EOF
#!/usr/bin/env bash
echo reached >"$SHAPELESS_MARKER"
echo "stub compare: no metric here" >&2
exit 0
EOF
chmod +x "$SHAPELESS/compare"

run_runner "$SHAPELESS"
skip_if_skipped

[ -r "$SHAPELESS_MARKER" ] ||
        verdict_failed "the run never reached the comparison, so no count was mangled"

[ "$STATUS" != 0 ] ||
        verdict_failed "the render test passed on a count of differing pixels that is not a number"

case "$(cat "$WORK/out")" in
        *"the count of differing pixels came back as"*) ;;
        *) verdict_failed "a count that is not a number was not reported as one" ;;
esac

# Scenarios 6a and 6b: the comparator's two channels contradict each other.
#
# Scenarios 5 and 6 break one channel each. These two leave both channels
# intact and make them DISAGREE, which is the case neither of the above can
# reach: a count and an exit status that are each well formed on their own and
# cannot both be true.
#
# The two are not worth the same. Only the first can produce a PASS.

# Scenario 6a: the comparator counts no difference and exits as though it found
# one.
#
# This is the one that decides the VERDICT. The count is what the runner reads
# its answer from, and a count of zero is a PASS; the exit status is the only
# thing left saying the comparator did not in fact agree with the golden. Delete
# the check and this run prints "renders identically to the golden" and exits 0
# on a comparator that never agreed with anything - so it is asserted as a
# failure first and by its words second.
#
# Exit 1 rather than 2 on purpose: 2 and above is already refused further up,
# and would make this scenario 5 again.
ZERO_YET_DIFFERENT="$WORK/zero-yet-different"
mkdir "$ZERO_YET_DIFFERENT"
ZERO_MARKER="$WORK/zero-yet-different-reached"

cat >"$ZERO_YET_DIFFERENT/compare" <<EOF
#!/usr/bin/env bash
echo reached >"$ZERO_MARKER"
echo "0" >&2
exit 1
EOF
chmod +x "$ZERO_YET_DIFFERENT/compare"

run_runner "$ZERO_YET_DIFFERENT"
skip_if_skipped

[ -r "$ZERO_MARKER" ] ||
        verdict_failed "the run never reached the comparison, so the two channels were never made to disagree"

[ "$STATUS" != 0 ] ||
        verdict_failed "the render test passed on a comparator that counted no difference yet exited as though it found one"

case "$(cat "$WORK/out")" in
        *"compare found no differing pixel yet exited 1"*) ;;
        *) verdict_failed "a comparator contradicting its own count of zero was not reported as one" ;;
esac

# Scenario 6b: the comparator counts a difference and exits as though it found
# none.
#
# Unlike 6a this one does NOT decide pass or fail - the count is non-zero, so
# the runner fails the run either way. What it decides is WHICH FAILURE IS
# NAMED: without the check the run reads "bands differs from the golden
# (AE=42)", sending a reader to hunt a rendering bug in a frame nobody has
# established anything about. So the assertion here is the exact words, and the
# status is checked only to keep the scenario honest about having failed at all.
ZERO_EXIT_YET_DIFFERING="$WORK/differing-yet-equal-exit"
mkdir "$ZERO_EXIT_YET_DIFFERING"
DIFFERING_MARKER="$WORK/differing-yet-equal-exit-reached"

cat >"$ZERO_EXIT_YET_DIFFERING/compare" <<EOF
#!/usr/bin/env bash
echo reached >"$DIFFERING_MARKER"
echo "42" >&2
exit 0
EOF
chmod +x "$ZERO_EXIT_YET_DIFFERING/compare"

run_runner "$ZERO_EXIT_YET_DIFFERING"
skip_if_skipped

[ -r "$DIFFERING_MARKER" ] ||
        verdict_failed "the run never reached the comparison, so the two channels were never made to disagree"

[ "$STATUS" != 0 ] ||
        verdict_failed "the render test passed on a comparator that counted 42 differing pixels"

case "$(cat "$WORK/out")" in
        *"compare found 42 differing yet exited 0"*) ;;
        *) verdict_failed "a comparator contradicting its own non-zero count was blamed on the renderer instead" ;;
esac

# Scenario 7: the --sixel on the runner's command line.
#
# The app defaults sixel on today (app.cc, gboolean sixel{true}), so the flag
# changes nothing as things stand and no render case would notice its deletion.
# What it is there for is the day that default flips, and that day can be
# staged: --sixel and --no-sixel write the same gboolean through GOption, which
# parses argv left to right, so an app wrapper that puts --no-sixel AHEAD of
# the runner's arguments is an app whose effective default is off.
#
# Under that wrapper the runner's own --sixel is the only thing left that can
# put the images back, so deleting it turns this red. The probe above it
# establishes that the slot is live at all: the same flag rewritten to
# --no-sixel, in the place the runner puts it, must change the frame.
SIXEL_OFF="$WORK/app-sixel-rewritten-off"
cat >"$SIXEL_OFF" <<EOF
#!/usr/bin/env bash
args=()
for a; do
        [ "\$a" = --sixel ] && a=--no-sixel
        args+=("\$a")
done
exec "$APP" "\${args[@]}"
EOF
chmod +x "$SIXEL_OFF"

run_runner "$NO_SHIM" "$SIXEL_OFF"
skip_if_skipped

[ "$STATUS" != 0 ] ||
        verdict_failed "turning the runner's own sixel flag off did not change the frame, so the probe below it is vacuous"
case "$(cat "$WORK/out")" in
        *"differs from the golden"*) ;;
        *) verdict_failed "a terminal with sixel turned off was not reported as differing" ;;
esac

DEFAULT_OFF="$WORK/app-sixel-default-off"
cat >"$DEFAULT_OFF" <<EOF
#!/usr/bin/env bash
exec "$APP" --no-sixel "\$@"
EOF
chmod +x "$DEFAULT_OFF"

run_runner "$NO_SHIM" "$DEFAULT_OFF"
skip_if_skipped

[ "$STATUS" = 0 ] ||
        verdict_failed "an app whose sixel default is off rendered no image, so the runner's --sixel did not override it"

# Scenario 8: the font sentinel the builder is pointed at.
#
# meson_options.txt tells a builder whose Monospace is not the goldens' one to
# read the suite by this rule: cases whose compared region holds something
# placed in cells move with the font, and a case that compares nothing but the
# image does not - so if some cases fail while THIS one passes, the cause is
# font resolution and not a rendering regression. That advice is only as good
# as its sentinel, and nothing was holding the sentinel: it was measured once,
# in a comment, and a case whose crop grew to take in a cell would go on being
# quoted as font-invariant.
#
# So run the sentinel under an alternate font and require it byte-identical.
# What makes that PASS mean anything is the harder half, and it is NOT "the
# font-sensitive case handed in beside it also fails", which is what stood here
# before. That case fails under any font that is not the goldens' Monospace 12,
# the app's own built-in default included, so it goes red exactly the same on a
# --font that never reached the terminal at all - measured on gtk3 through an
# app shim that strips --font from the argv, bands-margin still FAILs, at
# AE=19053. The half meant to rule out an ignored font was satisfied by one.
#
# What an ignored font cannot fake is a DIFFERENT amount per font. So drive the
# sensitive case under TWO alternate fonts and require the two counts to
# disagree: a --font that is dropped renders the same frame twice and answers
# the same number twice. Measured on gtk3, same shim:
#
#   alternate font       --font honoured   --font stripped
#   DejaVu Serif 12      AE=1764           AE=19053
#   Liberation Mono 8    AE=29273          AE=19053
#
# Neither count is pinned, only their disagreement, so a host resolving these
# families elsewhere still measures the same thing. Two families that collapse
# to ONE font on such a host make this scenario fail loudly instead of passing
# emptily, which is the right way round.
#
# The sentinel is then required invariant under BOTH of them, since the claim
# meson_options.txt makes is about any foreign Monospace and not about one.
#
# Every guard here is held, one red run each, measured on gtk3:
#
#   - the sensitive case moving: hand the sentinel in as the sensitive case, so
#     the run required to move compares nothing but the image. "FAIL: bands
#     under DejaVu Serif 12 rendered identically to its golden, so the
#     font-sensitive case does not move", under it "PASS: bands renders
#     identically to the golden".
#   - moving because the FRAME differs: drop the golden from the sensitive
#     fixtures, so the runner exits non-zero without ever comparing. "FAIL:
#     bands-margin under DejaVu Serif 12 failed for something other than a
#     difference from its golden", under it the runner's "FAIL: no golden
#     .../bands-margin.golden-gtk3.png". The bare status check this pairs with
#     was satisfied by that run and said nothing, which is the whole reason the
#     words are matched - as they are at every other non-zero status in this
#     file. The check is applied to both fonts; it is one guard read twice, not
#     a second guard.
#   - the two counts disagreeing: run the gate against an app shim that strips
#     --font from the argv. "FAIL: DejaVu Serif 12 and Liberation Mono 8 moved
#     bands-margin by the same 19053 pixels, so the alternate font is not
#     reaching the terminal".
#   - the sentinel's invariance: hand the sensitive case in as the sentinel, so
#     the run required to be invariant compares a crop holding the right
#     margin. "FAIL: the font sentinel bands-margin is not font-invariant under
#     DejaVu Serif 12 after all", under it "FAIL: bands-margin differs from the
#     golden (AE=1764)".
#   - the argument being required: invoke with four arguments.
#     "render-gate-test.sh: line 70: 5: font-sensitive case name", exit 1,
#     before any scenario runs.
SENSITIVE_FIXTURES="$WORK/sensitive"
mkdir "$SENSITIVE_FIXTURES"
cp "$SRCDIR/$SENSITIVE.six" \
   "$SRCDIR/$SENSITIVE.golden-$ARM.png" "$SENSITIVE_FIXTURES/" || exit 1
[ -r "$SRCDIR/$SENSITIVE.crop" ] &&
        { cp "$SRCDIR/$SENSITIVE.crop" "$SENSITIVE_FIXTURES/" || exit 1; }
[ -r "$SRCDIR/$SENSITIVE.eof" ] &&
        { cp "$SRCDIR/$SENSITIVE.eof" "$SENSITIVE_FIXTURES/" || exit 1; }

# Not "Monospace something": the point is font families fontconfig resolves
# differently from the goldens' Monospace, which is what a foreign build host
# has. Two of them, for the reason above, and they have to differ from each
# other in CELL METRICS and not merely in glyph shape, or the amount they move
# the sensitive case by could legitimately coincide.
ALT_FONT_A='DejaVu Serif 12'
ALT_FONT_B='Liberation Mono 8'

# Run the real runner under $1 as the font, over $2 as the fixture directory and
# $3 as the case. Same contract as run_runner: output in $WORK/out, status in
# $STATUS.
run_runner_with_font() {
        VTE_TEST_FONT="$1" VTE_TEST_ARTIFACT_DIR="$WORK" \
                "$RUNNER" "$APP" "$2" "$3" "$ARM" >"$WORK/out" 2>&1
        STATUS=$?
}

# The count the runner reported, or nothing when it never got as far as one.
runner_ae() { sed -n 's/.*(AE=\([^)]*\)).*/\1/p' "$WORK/out"; }

# The sensitive case under each font: it has to move, and to move because the
# frame differed rather than because the run fell over.
SENSITIVE_AE=()
for ALT_FONT in "$ALT_FONT_A" "$ALT_FONT_B"; do
        run_runner_with_font "$ALT_FONT" "$SENSITIVE_FIXTURES" "$SENSITIVE"
        skip_if_skipped

        [ "$STATUS" != 0 ] ||
                verdict_failed "$SENSITIVE under $ALT_FONT rendered identically to its golden, so the font-sensitive case does not move"

        case "$(cat "$WORK/out")" in
                *"differs from the golden"*) ;;
                *) verdict_failed "$SENSITIVE under $ALT_FONT failed for something other than a difference from its golden" ;;
        esac

        AE=$(runner_ae)
        [ -n "$AE" ] ||
                verdict_failed "$SENSITIVE under $ALT_FONT reported a difference without a count to compare"
        SENSITIVE_AE+=("$AE")
done

# The whole non-vacuity of the sentinel runs below: a font that never arrived
# moves the case by the same amount whichever font was asked for.
! is_number "${SENSITIVE_AE[0]}" "${SENSITIVE_AE[1]}" ||
        verdict_failed "$ALT_FONT_A and $ALT_FONT_B moved $SENSITIVE by the same ${SENSITIVE_AE[0]} pixels, so the alternate font is not reaching the terminal"

# The sentinel itself. Only the status is read: render-test.sh prints its PASS
# on the one path that exits zero, and scenario 1 above already reads the two
# together on an undamaged run, so matching the words here as well would be a
# second copy of that guard with no red run of its own.
for ALT_FONT in "$ALT_FONT_A" "$ALT_FONT_B"; do
        run_runner_with_font "$ALT_FONT" "$FIXTURES" "$CASE"
        skip_if_skipped

        [ "$STATUS" = 0 ] ||
                verdict_failed "the font sentinel $CASE is not font-invariant under $ALT_FONT after all"
done

# Scenario: a run that TIMED OUT waiting for the terminal is never reported as
# a rendering difference.
#
# The runner's readiness gate is three waits on the terminal - that it answered
# DSR-5 at all, that it painted its window, that it drew the fixture. Each can
# be reached by a machine too slow rather than by a renderer drawing the wrong
# thing, and the whole point of the gate is that those two are told apart. A
# runner that let a timeout fall through to the comparison would report the
# frame it happened to hold, which is the mid-paint red this gate exists to
# stop.
#
# Driven with VTE_TEST_READY_TIMEOUT=0, so the first wait's deadline is already
# past when it is reached. No shim is needed and no terminal has to be
# starved: the run reaches the same code an exhausted deadline reaches.
#
# Asserted in both directions, because only the pair is the claim: the words
# have to name the TERMINAL, and they must not be the words a wrong frame gets.
VTE_TEST_READY_TIMEOUT=0 VTE_TEST_ARTIFACT_DIR="$WORK" \
        "$RUNNER" "$APP" "$FIXTURES" "$CASE" "$ARM" >"$WORK/out" 2>&1
STATUS=$?
skip_if_skipped

[ "$STATUS" != 0 ] ||
        verdict_failed "the render test passed without ever waiting for the terminal"

case "$(cat "$WORK/out")" in
        *"differs from the golden"*)
                verdict_failed "a run that never waited for the terminal was reported as a rendering difference" ;;
esac

case "$(cat "$WORK/out")" in
        *"the terminal never answered DSR-5"*) ;;
        *) verdict_failed "a run that timed out waiting for the terminal did not say so" ;;
esac

echo "PASS: the frame the render test captures itself passes it"
echo "PASS: a frame with one changed pixel fails the render test"
echo "PASS: a frame with every pixel inverted fails the render test"
echo "PASS: a frame one column narrower than the golden fails the render test"
echo "PASS: a comparator that could not run fails the render test, and is named as one"
echo "PASS: a count of differing pixels that is not a number fails the render test, and is named as one"
echo "PASS: a comparator counting no difference while exiting as though it found one cannot produce a PASS"
echo "PASS: a comparator contradicting its own non-zero count is named as one, not blamed on the renderer"
echo "PASS: a failing frame is kept out of the fixture directory, in the artifact directory or the working one"
echo "PASS: the runner's --sixel turns images back on for an app whose default is off"
echo "PASS: $CASE is font-invariant under two fonts that move $SENSITIVE by different amounts, so the font sentinel holds"
echo "PASS: a run that timed out waiting for the terminal says so, and is not reported as a rendering difference"
exit 0
