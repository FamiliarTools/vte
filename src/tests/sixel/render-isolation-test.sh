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
# Asserts that the golden-frame render test measures the code, not the machine.
#
# The render runner keeps the invoking developer's settings out of the frame
# with two separate guards, and this test holds ONE of them per run, so that
# deleting either one alone turns this test red with its own message. The
# earlier version of this file held only the pair: it planted a vteapp.ini,
# which both guards stop, so removing either guard on its own still passed and
# either could have been deleted silently.
#
#   guard A - the scratch $HOME/$XDG_CONFIG_HOME the runner exports.
#             Probed here with a user fontconfig, which is the leak
#             --no-load-config cannot touch: fontconfig reads
#             $XDG_CONFIG_HOME/fontconfig/fonts.conf itself, whatever the app
#             does with its own config file.
#
#   guard B - the --no-load-config on the runner's command line.
#             Probed here through a wrapper that re-exports a hostile $HOME
#             and $XDG_CONFIG_HOME over the runner's, which puts a vteapp.ini
#             back on the path the app resolves. Guard A is defeated by
#             construction in that run, so only the flag can keep the frame.
#
# Each probe is preceded by a run establishing that the thing it plants is not
# inert - a test that plants an ignored file and sees green has proven only
# that the file was ignored.
#
# The case must be one whose frame moves with the font, or the fontconfig
# probe has nothing to go red for; see the note on sixel_isolation_case in
# src/app/meson.build.
#
# usage: render-isolation-test.sh <vte-app> <srcdir> <case> <gtk-arm>

set -u

APP=${1:?vte app binary}
SRCDIR=${2:?source dir}
CASE=${3:?case name}
ARM=${4:?gtk arm}

RUNNER="$SRCDIR/render-test.sh"
[ -x "$RUNNER" ] || { echo "FAIL: no runner $RUNNER"; exit 1; }
[ -x "$APP" ] || { echo "SKIP: $APP not executable"; exit 77; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Hand the runner a scratch copy of the fixtures rather than the source tree.
# Two of the runs below are meant to fail, and a failing render case writes the
# frame it actually got next to the golden; pointed at the real directory it
# would leave that artefact behind on every green run.
FIXTURES="$WORK/fixtures"
mkdir "$FIXTURES"
GOLDEN="$SRCDIR/$CASE.golden-$ARM.png"
[ -r "$SRCDIR/$CASE.six" ] || { echo "FAIL: no fixture $SRCDIR/$CASE.six"; exit 1; }
[ -r "$GOLDEN" ] || { echo "FAIL: no golden $GOLDEN"; exit 1; }
cp "$SRCDIR/$CASE.six" "$GOLDEN" "$FIXTURES/" || exit 1
[ -r "$SRCDIR/$CASE.crop" ] && { cp "$SRCDIR/$CASE.crop" "$FIXTURES/" || exit 1; }

# The hostile app configuration, for guard B.
#
# BackgroundColor repaints every cell of the compared region, so an app that
# reads it cannot arrive at the golden by accident, and nothing on the
# runner's command line overrides it - unlike Font, which --font would mask,
# hiding the leak rather than the fix doing so.
INI_BODY=$'[VteApp Configuration]\nBackgroundColor=#ff00ff\n'

# The hostile user fontconfig, for guard A. Reassigns the family the runner
# asks for, which is how a developer's own fontconfig would change the cell
# the frame is laid out in.
FC_BODY='<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <match target="pattern">
    <test name="family"><string>Monospace</string></test>
    <edit name="family" mode="assign" binding="strong"><string>DejaVu Serif</string></edit>
  </match>
</fontconfig>'

plant_fontconfig() {
        mkdir -p "$1/fontconfig" || exit 1
        printf '%s\n' "$FC_BODY" >"$1/fontconfig/fonts.conf" || exit 1
        [ -r "$1/fontconfig/fonts.conf" ] ||
                { echo "FAIL: could not plant $1/fontconfig/fonts.conf"; exit 1; }
}

# Write a wrapper that runs the app with $1 as its home, forcing that home
# past whatever the runner exported. Everything else on the runner's command
# line, --no-load-config included, is passed through untouched.
make_home_wrapper() {
        home=$1
        out=$2
        cat >"$out" <<EOF
#!/usr/bin/env bash
exec env HOME="$home" XDG_CONFIG_HOME="$home/.config" "$APP" "\$@"
EOF
        chmod +x "$out"
}

# Run the real runner against $1, in the environment given by the remaining
# arguments as env(1) takes it - VAR=VALUE, or -u VAR to remove one. Leaves
# its output in $WORK/out and its exit status in $STATUS.
run_runner() {
        app=$1
        shift
        # Assigned on the shell, not handed to env: env stops reading options
        # at its first operand, so a VAR=VALUE ahead of a "-u VAR" in "$@"
        # makes it take the -u as a file to execute.
        VTE_TEST_ARTIFACT_DIR="$WORK" \
                env "$@" "$RUNNER" "$app" "$FIXTURES" "$CASE" "$ARM" >"$WORK/out" 2>&1
        STATUS=$?
}

# A skip is a skip whichever run hit it: the runner decides that from the
# tools and the display, which this test cannot supply.
skip_if_skipped() {
        [ "$STATUS" = 77 ] || return 0
        echo "SKIP: the render test cannot run here"
        sed 's/^/  /' "$WORK/out"
        exit 77
}

# What the last run said about the FRAME, which is the only observable these
# assertions are about. Read from the line the runner prints for each outcome
# and not from its exit status, because the status is shared by every way the
# run can end:
#
#   identical      it compared the frame against the golden and found no
#                  differing pixel
#   different      it compared them and found some
#   inconclusive   it never reached a comparison at all - it could not capture,
#                  the terminal never answered or never drew, ImageMagick
#                  failed, the golden was missing
#
# The third is the point of splitting this out. A run that fell over before
# comparing anything says NOTHING about whether the planted file reached the
# app, and reporting it as though it did sends a reader after a config leak on
# the evidence of a slow machine. It is the same shape as the one require_moved
# in xtsmgraphics-isolation-test.sh was rewritten to avoid: a verdict that
# could not tell its own failure mode from the one it tests for.
frame_verdict() {
        local out
        out=$(cat "$WORK/out")

        case "$out" in
                *"differs from the golden"*)
                        echo different; return ;;
        esac

        case "$out" in
                *"renders identically to the golden"*)
                        # The words and the status have to agree, or this no
                        # longer knows what it is being told.
                        [ "$STATUS" = 0 ] && { echo identical; return; } ;;
        esac

        echo inconclusive
}

# Complain that the run never got as far as a frame, naming $1 as what is left
# unestablished. Deliberately worded so it can never be misread as the planted
# file having leaked.
inconclusive() {
        echo "FAIL: the render runner never reached a comparison, so nothing was established about $1"
        sed 's/^/  /' "$WORK/out"
        exit 1
}

# Require the last run to have reported a frame DIFFERENCE - used for the two
# probes that establish a planted file is not inert. $1 names the probe; $2 is
# what to say if the frame came back identical, i.e. the plant did nothing.
require_difference() {
        case "$(frame_verdict)" in
                different) ;;
                identical) echo "FAIL: $2"; sed 's/^/  /' "$WORK/out"; exit 1 ;;
                *) inconclusive "$1" ;;
        esac
}

# Require the last run to have reported the frame IDENTICAL to the golden -
# used for the guard assertions themselves. $1 names the guard; $2 is what to
# say if the frame differed, i.e. the planted file did reach the app.
require_identical() {
        case "$(frame_verdict)" in
                identical) ;;
                different) echo "FAIL: $2"; sed 's/^/  /' "$WORK/out"; exit 1 ;;
                *) inconclusive "$1" ;;
        esac
}

# ------------------------------------------------- the verdict reader itself ---
#
# frame_verdict() is only worth anything if it really does separate the three,
# and it keys on words the RUNNER prints, in another file. So both halves are
# checked before any of the assertions above are trusted.
#
# First that the words still exist where they are read from. A rename in the
# runner would otherwise silently turn every outcome into "inconclusive", and
# this test would go red for the wrong reason or, worse, stop distinguishing
# anything.
for PHRASE in 'renders identically to the golden' 'differs from the golden'; do
        grep -qF "$PHRASE" "$RUNNER" ||
                { echo "FAIL: the runner no longer says \"$PHRASE\", so frame_verdict cannot read it"; exit 1; }
done

# Then that each of the three is actually reached. Driven over $WORK/out and
# $STATUS directly, which is the whole of frame_verdict's input.
self_check() {
        printf '%s\n' "$2" >"$WORK/out"
        STATUS=$3
        [ "$(frame_verdict)" = "$1" ] ||
                { echo "FAIL: frame_verdict read $4 as $(frame_verdict), not $1"; exit 1; }
}

self_check identical    "PASS: c renders identically to the golden"       0 "a passing run"
self_check different    "FAIL: c differs from the golden (AE=1234)"       1 "a frame difference"
self_check inconclusive "FAIL: c was never captured: the terminal never painted its window" 1 "a readiness timeout"
self_check inconclusive "FAIL: could not capture the window"              1 "a capture failure"
self_check inconclusive "FAIL: c could not be compared: could not read the golden" 1 "a broken comparison"
# A pass line the exit status contradicts is not a pass.
self_check inconclusive "PASS: c renders identically to the golden"       1 "a pass line with a non-zero status"
rm -f "$WORK/out"
unset STATUS

# ---------------------------------------------------------------- guard B ---

# Is the planted vteapp.ini one that changes anything? Force it in by the one
# route the isolation deliberately leaves open - an explicit --load-config,
# which the app honours whether or not the default load was suppressed.
FORCED="$WORK/app-with-config"
FORCED_INI="$WORK/forced.ini"
printf '%s' "$INI_BODY" >"$FORCED_INI"
cat >"$FORCED" <<EOF
#!/usr/bin/env bash
exec "$APP" --load-config "$FORCED_INI" "\$@"
EOF
chmod +x "$FORCED"

run_runner "$FORCED"
skip_if_skipped
require_difference "the guard B probe's premise" \
        "the planted vteapp.ini does not change the frame, so the guard B probe is vacuous"

# Guard B itself. The wrapper puts the vteapp.ini back where the app looks for
# it, so the scratch home cannot be what saves this run; --no-load-config is
# the only thing left. Nothing under this home touches fontconfig, so removing
# the scratch home from the runner leaves this run green - the two probes fail
# for one guard each.
INI_HOME="$WORK/ini-home"
mkdir -p "$INI_HOME/.config"
printf '%s' "$INI_BODY" >"$INI_HOME/.config/vteapp.ini"
[ -r "$INI_HOME/.config/vteapp.ini" ] ||
        { echo "FAIL: could not plant $INI_HOME/.config/vteapp.ini"; exit 1; }
make_home_wrapper "$INI_HOME" "$WORK/app-in-ini-home"

run_runner "$WORK/app-in-ini-home"
skip_if_skipped
require_identical "guard B, --no-load-config" \
        "a vteapp.ini in the app's own config dir changed the rendered frame"

# ---------------------------------------------------------------- guard A ---

# Is the planted fontconfig one that changes anything? Same wrapper trick, so
# the file is reached no matter what the runner exports.
FC_HOME="$WORK/fc-home"
mkdir -p "$FC_HOME/.config"
plant_fontconfig "$FC_HOME/.config"
make_home_wrapper "$FC_HOME" "$WORK/app-in-fc-home"

run_runner "$WORK/app-in-fc-home"
skip_if_skipped
require_difference "the guard A probes' premise" \
        "the planted fontconfig does not change the frame, so the guard A probes are vacuous"

# Guard A, on each of the two paths the config dir resolves from. They are
# separate resolutions - $XDG_CONFIG_HOME when set, $HOME/.config otherwise -
# and covering one says nothing about the other. No vteapp.ini is planted
# under either, so removing --no-load-config from the runner leaves both
# green.
XDG="$WORK/xdg"
EMPTY_HOME="$WORK/xdg-home"
mkdir -p "$XDG" "$EMPTY_HOME"
plant_fontconfig "$XDG"

run_runner "$APP" "HOME=$EMPTY_HOME" "XDG_CONFIG_HOME=$XDG"
skip_if_skipped
require_identical "guard A, via \$XDG_CONFIG_HOME" \
        "a fontconfig in \$XDG_CONFIG_HOME changed the rendered frame"

FC_ONLY_HOME="$WORK/home"
mkdir -p "$FC_ONLY_HOME/.config"
plant_fontconfig "$FC_ONLY_HOME/.config"

run_runner "$APP" -u XDG_CONFIG_HOME "HOME=$FC_ONLY_HOME"
skip_if_skipped
require_identical "guard A, via \$HOME/.config" \
        "a fontconfig in \$HOME/.config changed the rendered frame"

echo "PASS: the planted vteapp.ini and fontconfig each do change the frame when they reach the app"
echo "PASS: a vteapp.ini in the app's own config dir does not reach the render test"
echo "PASS: a fontconfig in \$XDG_CONFIG_HOME does not reach the render test"
echo "PASS: a fontconfig in \$HOME/.config does not reach the render test"
exit 0
