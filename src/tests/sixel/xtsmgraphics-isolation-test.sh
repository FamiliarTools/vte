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
# Asserts that the XTSMGRAPHICS test asks the code, not the machine.
#
# The one answer xtsmgraphics-test.sh asserts that is not a constant is the
# agreement between XTSMGRAPHICS current geometry and CSI 14t. That geometry is
# the window measured in the font's cell, so anything that changes the cell
# changes it - and a developer's own config is full of such things. The test
# keeps them out with two guards, and until this file existed neither was held
# by anything:
#
#   guard A - the scratch $HOME/$XDG_CONFIG_HOME it exports.
#             Probed here with a user fontconfig, which is the leak
#             --no-load-config cannot touch: fontconfig reads
#             $XDG_CONFIG_HOME/fontconfig/fonts.conf itself, whatever the app
#             does with its own config file. Probed on BOTH paths the config
#             dir resolves from, $XDG_CONFIG_HOME and $HOME/.config, since
#             those are separate resolutions.
#
#   guard B - the --no-load-config on its app command line.
#             Probed here through a wrapper that re-exports a hostile $HOME and
#             $XDG_CONFIG_HOME over the ones the test exported, which puts a
#             vteapp.ini back on the path the app resolves. Guard A is defeated
#             by construction in that run, so only the flag can save it.
#
# Each probe is preceded by a run establishing that what it plants is not
# inert. A test that plants an ignored file and sees green has proven only that
# the file was ignored.
#
# The verdict is the reported geometry itself, compared against the geometry a
# clean run reports, rather than the tested script's own pass or fail. A leak
# that moves the cell without moving it far enough to cross
# VTE_SIXEL_MAX_WIDTH leaves that script passing while it is no longer
# measuring what it thinks it is, and this file is here to see exactly that.
#
# usage: xtsmgraphics-isolation-test.sh <vte-app> <srcdir>

set -u

APP=${1:?vte app binary}
SRCDIR=${2:?source dir}

TESTED="$SRCDIR/xtsmgraphics-test.sh"
[ -x "$TESTED" ] || { echo "FAIL: no xtsmgraphics test $TESTED"; exit 1; }
command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: Xvfb not available"; exit 77; }
[ -x "$APP" ] || { echo "SKIP: $APP not executable"; exit 77; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The hostile app configuration, for guard B. Font is what decides the cell,
# and 40 is far enough from any default that the geometry cannot land back on
# the clean one by chance. Nothing on the tested script's command line
# overrides it - it passes no --font.
INI_BODY=$'[VteApp Configuration]\nFont=Monospace 40\n'

# The hostile user fontconfig, for guard A. Assigns both the family and the
# size for every pattern, which is the blunt version of what a developer's own
# fonts.conf does to the cell the geometry is computed in. It is deliberately
# not conditioned on a family: the tested script asks for no font at all, so
# there is no family name to key on.
FC_BODY='<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
  <match target="pattern">
    <edit name="family" mode="assign" binding="strong"><string>DejaVu Serif</string></edit>
    <edit name="pixelsize" mode="assign" binding="strong"><double>40</double></edit>
  </match>
</fontconfig>'

plant_fontconfig() {
        mkdir -p "$1/fontconfig" || exit 1
        printf '%s\n' "$FC_BODY" >"$1/fontconfig/fonts.conf" || exit 1
        [ -r "$1/fontconfig/fonts.conf" ] ||
                { echo "FAIL: could not plant $1/fontconfig/fonts.conf"; exit 1; }
}

plant_ini() {
        mkdir -p "$1" || exit 1
        printf '%s' "$INI_BODY" >"$1/vteapp.ini" || exit 1
        [ -r "$1/vteapp.ini" ] ||
                { echo "FAIL: could not plant $1/vteapp.ini"; exit 1; }
}

# Write a wrapper that runs the app with $1 as its home, forcing that home past
# whatever the tested script exported. Everything else on its command line,
# --no-load-config included, is passed through untouched.
make_home_wrapper() {
        cat >"$2" <<EOF
#!/usr/bin/env bash
exec env HOME="$1" XDG_CONFIG_HOME="$1/.config" "$APP" "\$@"
EOF
        chmod +x "$2"
}

# Run the tested script against $1, in the environment given by the remaining
# arguments as env(1) takes it - VAR=VALUE, or -u VAR to remove one. Leaves its
# output in $WORK/out and its exit status in $STATUS.
run_tested() {
        app=$1
        shift
        # Assigned on the shell, not handed to env: env stops reading options
        # at its first operand, so a VAR=VALUE ahead of a "-u VAR" in "$@"
        # makes it take the -u as a file to execute.
        env "$@" "$TESTED" "$app" geometry >"$WORK/out" 2>&1
        STATUS=$?
}

# The geometry the last run reported, or nothing when it reported none.
reported_geometry() {
        awk '/agrees with CSI 14t at/ { print $NF }' "$WORK/out"
}

skip_if_skipped() {
        [ "$STATUS" = 77 ] || return 0
        echo "SKIP: the XTSMGRAPHICS test cannot run here"
        sed 's/^/  /' "$WORK/out"
        exit 77
}

bail() { echo "FAIL: $1"; sed 's/^/  /' "$WORK/out"; exit 1; }

# The last run must have reported the same geometry the clean run did. Both the
# status and the number are checked: a script that fell over reports no
# geometry at all, and an empty string must never read as agreement.
require_same_geometry() {
        [ "$STATUS" = 0 ] || bail "$1"
        [ "$(reported_geometry)" = "$CLEAN" ] || bail "$1"
}

# The last run must NOT have come out where the clean run did - either it
# reported a different geometry, or the leak moved the cell far enough that the
# two channels stopped agreeing and it failed outright. Both mean the planted
# file reached the app.
require_moved() {
        [ "$STATUS" = 0 ] && [ "$(reported_geometry)" = "$CLEAN" ] && bail "$1"
        return 0
}

# ------------------------------------------------------------------ clean ---

run_tested "$APP"
skip_if_skipped
[ "$STATUS" = 0 ] || bail "the XTSMGRAPHICS test does not pass on its own"
CLEAN=$(reported_geometry)
[ -n "$CLEAN" ] ||
        bail "the clean run reported no geometry, so there is nothing to compare against"

# ---------------------------------------------------------------- guard B ---

# Is the planted vteapp.ini one that changes anything? Force it in by the one
# route the isolation deliberately leaves open - an explicit --load-config,
# which the app honours whether or not the default load was suppressed
# (app.cc: the --load-config path is read after, and apart from, the
# !no_load_config default load).
FORCED_INI="$WORK/forced.ini"
printf '%s' "$INI_BODY" >"$FORCED_INI"
cat >"$WORK/app-with-config" <<EOF
#!/usr/bin/env bash
exec "$APP" --load-config "$FORCED_INI" "\$@"
EOF
chmod +x "$WORK/app-with-config"

run_tested "$WORK/app-with-config"
skip_if_skipped
require_moved "the planted vteapp.ini does not change the geometry, so the guard B probe is vacuous"

# Guard B itself. The wrapper puts the vteapp.ini back where the app looks for
# it, so the scratch home cannot be what saves this run; --no-load-config is
# the only thing left. Nothing under this home touches fontconfig, so removing
# the scratch home from the tested script leaves this run green - the probes
# fail for one guard each.
INI_HOME="$WORK/ini-home"
plant_ini "$INI_HOME/.config"
make_home_wrapper "$INI_HOME" "$WORK/app-in-ini-home"

run_tested "$WORK/app-in-ini-home"
skip_if_skipped
require_same_geometry "a vteapp.ini in the app's own config dir changed the reported geometry"

# ---------------------------------------------------------------- guard A ---

# Is the planted fontconfig one that changes anything? Same wrapper trick, so
# the file is reached no matter what the tested script exports.
FC_HOME="$WORK/fc-home"
mkdir -p "$FC_HOME/.config"
plant_fontconfig "$FC_HOME/.config"
make_home_wrapper "$FC_HOME" "$WORK/app-in-fc-home"

run_tested "$WORK/app-in-fc-home"
skip_if_skipped
require_moved "the planted fontconfig does not change the geometry, so the guard A probes are vacuous"

# Guard A, on each of the two paths the config dir resolves from. No vteapp.ini
# is planted under either, so removing --no-load-config from the tested script
# leaves both green.
XDG="$WORK/xdg"
EMPTY_HOME="$WORK/xdg-home"
mkdir -p "$XDG" "$EMPTY_HOME"
plant_fontconfig "$XDG"

run_tested "$APP" "HOME=$EMPTY_HOME" "XDG_CONFIG_HOME=$XDG"
skip_if_skipped
require_same_geometry "a fontconfig in \$XDG_CONFIG_HOME changed the reported geometry"

FC_ONLY_HOME="$WORK/home"
mkdir -p "$FC_ONLY_HOME/.config"
plant_fontconfig "$FC_ONLY_HOME/.config"

run_tested "$APP" -u XDG_CONFIG_HOME "HOME=$FC_ONLY_HOME"
skip_if_skipped
require_same_geometry "a fontconfig in \$HOME/.config changed the reported geometry"

echo "PASS: the planted vteapp.ini and fontconfig each do change the geometry when they reach the app"
echo "PASS: a vteapp.ini in the app's own config dir does not reach the XTSMGRAPHICS test"
echo "PASS: a fontconfig in \$XDG_CONFIG_HOME does not reach the XTSMGRAPHICS test"
echo "PASS: a fontconfig in \$HOME/.config does not reach the XTSMGRAPHICS test"
exit 0
