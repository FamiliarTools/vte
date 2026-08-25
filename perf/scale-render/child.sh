#!/bin/sh
# The fixture the scale harness photographs. usage: child.sh <file.six> [seconds]
#
# Three things, and each of them is load-bearing:
#
#   row 1, column 1   the image, so its left and top edge are the terminal's
#   rows 6..8         a 20x3 cell block in a colour the image does not contain,
#                     which is how the harness learns the cell size IN THE
#                     PIXELS IT CAPTURED rather than assuming a font metric
#   row 10, column 6  the same image again, five cells right, so a run can
#                     show the image lands on a cell boundary and not merely
#                     at the origin, where every wrong answer also lands
#
# The cursor parks at row 20 so it can never sit inside a measured region.
set -u

SIX=${1:?sixel file}
SECONDS_=${2:-30}

printf '\033[H'
cat "$SIX"

for row in 6 7 8; do
        printf "\033[%d;1H\033[48;2;0;255;128m%20s\033[m" "$row" ''
done

printf '\033[10;6H'
cat "$SIX"

printf '\033[20;1H'
sleep "$SECONDS_"
