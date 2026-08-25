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
# render-test.sh decides its verdict from what ImageMagick says about the
# difference of two frames. It used to read a failure of that command as an
# empty difference and print PASS, so on any host where ImageMagick broke -
# and for any input it choked on - the whole render suite reported success
# without having looked at a single pixel. A test that cannot fail is worse
# than no test, because it is counted.
#
# So break the comparison on purpose and require the runner to say so: put a
# convert on PATH that refuses the one invocation the verdict rests on and
# passes every other through to the real thing. The run therefore reaches the
# comparison with a real captured frame - the marker file proves it got that
# far - and then cannot make it.
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

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

MARKER="$WORK/reached"
mkdir "$WORK/bin"

cat >"$WORK/bin/convert" <<EOF
#!/usr/bin/env bash
# Refuse only the difference composite the verdict is read from, so
# everything before it - the crop, the normalisation - still works and the
# runner arrives at the comparison with a frame worth comparing.
for arg in "\$@"; do
        if [ "\$arg" = "-compose" ]; then
                echo reached >"$MARKER"
                echo "stub convert: refusing to take the difference" >&2
                exit 1
        fi
done
exec "$REAL_CONVERT" "\$@"
EOF
chmod +x "$WORK/bin/convert"

PATH="$WORK/bin:$PATH" "$RUNNER" "$APP" "$SRCDIR" "$CASE" "$ARM" >"$WORK/out" 2>&1
STATUS=$?

if [ "$STATUS" = 77 ]; then
        echo "SKIP: the render test cannot run here"
        sed 's/^/  /' "$WORK/out"
        exit 77
fi

# The fixture first: without this the test would pass for the wrong reason on
# a run that never got as far as comparing anything.
[ -r "$MARKER" ] || {
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

echo "PASS: a comparison that cannot run fails the render test"
exit 0
