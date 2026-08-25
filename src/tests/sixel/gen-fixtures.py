#!/usr/bin/env python3
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
"""Generate the SIXEL render-test fixtures.

Checked in alongside its output so the fixtures are reproducible rather than
magic bytes nobody can regenerate or reason about.

The one subtlety worth stating, because getting it wrong produces an image
that looks plausible and is 8x too wide: a colour introducer (#N) does NOT
return the active position to the left margin. Within one sixel row, every
colour pass after the first must be preceded by DECGCR ($), or each pass
starts where the previous one stopped.
"""

W, H = 96, 72
BANDS = [(0, 0, 0), (100, 0, 0), (0, 100, 0), (0, 0, 100),
         (100, 100, 0), (0, 100, 100), (100, 0, 100), (100, 100, 100)]
BAND_H = H // len(BANDS)          # 9 px
ROWS = H // 6                     # 12 sixel rows


def bands_six():
    out = ['\x1bPq']
    for i, (r, g, b) in enumerate(BANDS):
        out.append('#%d;2;%d;%d;%d' % (i, r, g, b))

    for row in range(ROWS):
        passes = []
        for i in range(len(BANDS)):
            mask = 0
            for bit in range(6):
                if (row * 6 + bit) // BAND_H == i:
                    mask |= 1 << bit
            if mask:
                passes.append('#%d%s' % (i, chr(63 + mask) * W))
        # DECGCR between colour passes: back to the left margin.
        out.append('$'.join(passes))
        if row != ROWS - 1:
            out.append('-')
    out.append('\x1b\\')
    return ''.join(out)


def splice_gch(s):
    """bands.six with a DECGCH spliced in.

    DECGCH is a VT240 command a VT340 does not implement. Per DEC STD 070
    8.1 an unsupported command is parsed and ignored, so this must render
    pixel-identically to bands.six - which is why the two share a golden.
    """
    parts = s.split('-')
    mid = len(parts) // 2
    parts[mid] = '+' + parts[mid]
    return '-'.join(parts)


def at_right_margin(s):
    """bands.six emitted so that it overhangs the right margin.

    DEC STD 070 11.2.2: "Sixels defined to be printed past the right margin
    are not printed." The image is 96 px wide, which is several columns on
    any cell the terminal can have, so starting it at column 76 of an 80
    column screen leaves it overhanging: the part past the margin must be
    discarded - not wrapped, not scaled to fit, not drawn over the edge.
    """
    return '\x1b[1;76H' + s


def undefined_registers():
    """An image that SELECTS colour registers without ever DEFINING them.

    DECRST 1070 first, so the terminal uses shared colour registers - the
    path on which nothing initialises the palette unless it is done at
    construction. The registers selected are VT340 palette entries, so a
    conformant terminal draws blue/red/green/magenta and an unconfigured
    one draws whatever happened to be in memory.
    """
    W, H = 96, 24
    out = ['\x1b[?1070l', '\x1bPq']
    rows = H // 6
    for row in range(rows):
        # Registers 1..4 of the VT340 palette: blue, red, green, magenta.
        out.append('#%d%s' % (row + 1, chr(63 + 0b111111) * W))
        if row != rows - 1:
            out.append('-')
    out.append('\x1b\\')
    return ''.join(out)


def truncated(s):
    """bands.six cut off partway through, with NO terminator.

    What a dropped connection or `head -c` produces. xterm has rendered the
    part that arrived since patch #323; discarding it means a user who
    cats a partially-downloaded image sees nothing rather than the top of
    it. Cut at 60% so several complete bands have arrived.
    """
    return s[:int(len(s) * 0.6)]


if __name__ == '__main__':
    b = bands_six()
    open('bands.six', 'w').write(b)
    open('bands-gch.six', 'w').write(splice_gch(b))
    open('bands-margin.six', 'w').write(at_right_margin(b))
    open('undefined-registers.six', 'w').write(undefined_registers())
    open('bands-truncated.six', 'w').write(truncated(b))
    print('wrote bands.six (%d bytes), bands-gch.six, bands-margin.six' % len(b))
