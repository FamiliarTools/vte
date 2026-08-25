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
# The app reads a user's vteapp.ini at startup, and that file sets the colours,
# the margins, the font and whether sixel is on - every input the golden was
# captured against. Left alone, the render suite therefore reports on whoever
# ran it: a developer with a config file sees failures that are not defects,
# and a golden regenerated on that machine writes their settings into the tree.
#
# Three runs of the real runner, because the claim has two halves and neither
# is worth anything without the other:
#
#   - the configuration planted here DOES change the frame, established by
#     making the app load it on purpose and requiring the runner to notice;
#   - the runner ignores that same configuration on each of the two paths the
#     app resolves it from, $XDG_CONFIG_HOME and $HOME/.config.
#
# Without the first, a test that plants an inert file and sees green proves
# only that the file was inert.
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
# The first of the three runs is meant to fail, and a failing render case
# writes the frame it actually got next to the golden; pointed at the real
# directory it would leave that artefact behind on every green run.
FIXTURES="$WORK/fixtures"
mkdir "$FIXTURES"
GOLDEN="$SRCDIR/$CASE.golden-$ARM.png"
[ -r "$SRCDIR/$CASE.six" ] || { echo "FAIL: no fixture $SRCDIR/$CASE.six"; exit 1; }
[ -r "$GOLDEN" ] || { echo "FAIL: no golden $GOLDEN"; exit 1; }
cp "$SRCDIR/$CASE.six" "$GOLDEN" "$FIXTURES/" || exit 1
[ -r "$SRCDIR/$CASE.crop" ] && { cp "$SRCDIR/$CASE.crop" "$FIXTURES/" || exit 1; }

# The hostile configuration.
#
# BackgroundColor repaints every cell of the compared region, so an app that
# reads it cannot arrive at the golden by accident, and nothing on the
# runner's command line overrides it - unlike Font, which --font would mask,
# hiding the leak rather than the fix doing so.
INI_BODY=$'[VteApp Configuration]\nBackgroundColor=#ff00ff\n'

# Run the real runner against $1, in the environment given by the remaining
# arguments as env(1) takes it - VAR=VALUE, or -u VAR to remove one. Leaves
# its output in $WORK/out and its exit status in $STATUS.
run_runner() {
        app=$1
        shift
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

# Run 1: is the planted configuration one that changes anything?
#
# Force it in by the one route the isolation deliberately leaves open - an
# explicit --load-config, which the app honours whether or not the default
# load was suppressed - and require the runner to report a difference. If this
# comes back green the file is inert and the two runs below are vacuous.
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

[ "$STATUS" != 0 ] || {
        echo "FAIL: the planted configuration does not change the frame, so nothing below is proven"
        sed 's/^/  /' "$WORK/out"
        exit 1
}

case "$(cat "$WORK/out")" in
        *"differs from the golden"*) ;;
        *)
                echo "FAIL: loading the planted configuration did not produce a frame difference"
                sed 's/^/  /' "$WORK/out"
                exit 1
                ;;
esac

# Run 2: reachable as $XDG_CONFIG_HOME/vteapp.ini.
#
# $HOME goes to an empty directory of its own so this run tests the one path
# it names: whatever the invoking developer has under their real ~/.config
# must not be what decides the verdict either way.
XDG="$WORK/xdg"
EMPTY_HOME="$WORK/xdg-home"
mkdir -p "$XDG" "$EMPTY_HOME"
printf '%s' "$INI_BODY" >"$XDG/vteapp.ini"
[ -r "$XDG/vteapp.ini" ] || { echo "FAIL: could not plant $XDG/vteapp.ini"; exit 1; }

run_runner "$APP" "HOME=$EMPTY_HOME" "XDG_CONFIG_HOME=$XDG"
skip_if_skipped

[ "$STATUS" = 0 ] || {
        echo "FAIL: a vteapp.ini in \$XDG_CONFIG_HOME changed the rendered frame"
        sed 's/^/  /' "$WORK/out"
        exit 1
}

# Run 3: reachable as $HOME/.config/vteapp.ini, with $XDG_CONFIG_HOME unset.
# The two are separate resolutions inside glib, and covering one says nothing
# about the other.
FAKE_HOME="$WORK/home"
mkdir -p "$FAKE_HOME/.config"
printf '%s' "$INI_BODY" >"$FAKE_HOME/.config/vteapp.ini"
[ -r "$FAKE_HOME/.config/vteapp.ini" ] ||
        { echo "FAIL: could not plant $FAKE_HOME/.config/vteapp.ini"; exit 1; }

run_runner "$APP" -u XDG_CONFIG_HOME "HOME=$FAKE_HOME"
skip_if_skipped

[ "$STATUS" = 0 ] || {
        echo "FAIL: a vteapp.ini in \$HOME/.config changed the rendered frame"
        sed 's/^/  /' "$WORK/out"
        exit 1
}

echo "PASS: the planted configuration does change the frame when the app loads it"
echo "PASS: a vteapp.ini in \$XDG_CONFIG_HOME does not reach the render test"
echo "PASS: a vteapp.ini in \$HOME/.config does not reach the render test"
exit 0
