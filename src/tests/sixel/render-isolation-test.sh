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

# Require the last run to have reported a frame difference, with $1 said if it
# did not. Both halves are checked: a non-zero status alone would also be
# satisfied by the runner failing to run at all.
require_difference() {
        [ "$STATUS" != 0 ] || { echo "FAIL: $1"; sed 's/^/  /' "$WORK/out"; exit 1; }
        case "$(cat "$WORK/out")" in
                *"differs from the golden"*) ;;
                *) echo "FAIL: $1"; sed 's/^/  /' "$WORK/out"; exit 1 ;;
        esac
}

require_identical() {
        [ "$STATUS" = 0 ] || { echo "FAIL: $1"; sed 's/^/  /' "$WORK/out"; exit 1; }
}

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
require_difference "the planted vteapp.ini does not change the frame, so the guard B probe is vacuous"

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
require_identical "a vteapp.ini in the app's own config dir changed the rendered frame"

# ---------------------------------------------------------------- guard A ---

# Is the planted fontconfig one that changes anything? Same wrapper trick, so
# the file is reached no matter what the runner exports.
FC_HOME="$WORK/fc-home"
mkdir -p "$FC_HOME/.config"
plant_fontconfig "$FC_HOME/.config"
make_home_wrapper "$FC_HOME" "$WORK/app-in-fc-home"

run_runner "$WORK/app-in-fc-home"
skip_if_skipped
require_difference "the planted fontconfig does not change the frame, so the guard A probes are vacuous"

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
require_identical "a fontconfig in \$XDG_CONFIG_HOME changed the rendered frame"

FC_ONLY_HOME="$WORK/home"
mkdir -p "$FC_ONLY_HOME/.config"
plant_fontconfig "$FC_ONLY_HOME/.config"

run_runner "$APP" -u XDG_CONFIG_HOME "HOME=$FC_ONLY_HOME"
skip_if_skipped
require_identical "a fontconfig in \$HOME/.config changed the rendered frame"

echo "PASS: the planted vteapp.ini and fontconfig each do change the frame when they reach the app"
echo "PASS: a vteapp.ini in the app's own config dir does not reach the render test"
echo "PASS: a fontconfig in \$XDG_CONFIG_HOME does not reach the render test"
echo "PASS: a fontconfig in \$HOME/.config does not reach the render test"
exit 0
