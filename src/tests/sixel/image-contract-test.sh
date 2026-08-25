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
# Runner for the image lifetime contract test.
#
# The test realizes a terminal widget, so it needs a display: an image's
# footprint is measured in the font's cell, and there is no cell until the
# widget has been given a font on a display. This boots a throwaway X server
# for it, exactly as the render tests do, and never touches an existing one.
#
# Exits 77 (meson's "skipped") when Xvfb is absent, so a sandboxed build
# reports "skipped" rather than a failure it could not tell apart from a real
# one. The test binary uses the same convention for itself.
#
# usage: image-contract-test.sh <test-binary>

set -u

BIN=${1:?test binary}

command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: Xvfb not available"; exit 77; }
[ -x "$BIN" ] || { echo "SKIP: $BIN not executable"; exit 77; }

WORK=$(mktemp -d)
cleanup() {
        [ -n "${XPID:-}" ] && kill "$XPID" 2>/dev/null
        wait 2>/dev/null
        rm -rf "$WORK"
}
trap cleanup EXIT

# Let X pick a free display and TELL us which. Guessing the number collides
# with the render tests meson runs in parallel, which is how those first went
# flaky: two servers race for one number, one loses, and both tests then talk
# to whichever won.
Xvfb -displayfd 3 -screen 0 800x600x24 -nolisten tcp 3>"$WORK/display" >/dev/null 2>&1 &
XPID=$!

for _ in $(seq 1 100); do
        [ -s "$WORK/display" ] && break
        sleep 0.1
done
DISP=$(cat "$WORK/display" 2>/dev/null)
[ -n "$DISP" ] || { echo "SKIP: Xvfb did not report a display"; exit 77; }

export DISPLAY=":$DISP"
export GDK_BACKEND=x11
export GSK_RENDERER=${VTE_TEST_RENDERER:-cairo}
export LIBGL_ALWAYS_SOFTWARE=1

# A failed assertion aborts, and a core file left behind is this test's litter
# to clean, not the developer's to discover.
ulimit -c 0

"$BIN"
