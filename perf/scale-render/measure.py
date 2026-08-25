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
"""Measure the drawn size of a SIXEL image in a captured frame.

usage: measure.py <shot-dir> <config> [<config>...]

The law being checked, in device pixels of the captured frame:

    drawn width  = image width in sixel pixels x device scale x font zoom

Both factors have to be there and neither may be counted twice. The image's
grid is the cell VTE told the producer about over the PTY, which is the
UNSCALED cell, while the draw sizes against the scaled one - so a sixel pixel
is a logical pixel that the toolkit then blows up by the device scale, and the
zoom scales the image exactly as it scales the text around it.

The font zoom is not read from the command line, because a font does not
promise that twice the size is twice the metric: Monospace 12 doubled measures
20x39 here, not 20x40. It is measured, as the ratio of this frame's cell to
the cell of the same configuration at zoom 1, and 39/20 is then what the image
must scale by vertically. A harness that assumed 2.0 would report a defect
that is really a font rounding to a half pixel.
"""

import subprocess
import sys

# Cell geometry of the fixture in child.sh: a 20x3 cell marker block whose
# top-left is at row 6, column 1, and a second copy of the image at row 10,
# column 6.
MARKER_COLS = 20
MARKER_ROWS = 3
MARKER_ROW = 6
IMAGE2_ROW = 10
IMAGE2_COL = 6

MARKER_COLOUR = 'srgb(0,255,128)'
BAND_COLOUR = 'srgb(255,0,255)'

# bands.six: 96x72 sixel pixels, eight 9 px bands, the magenta one seventh.
IMAGE_W = 96
BAND_Y = 54
BAND_H = 9

# One device pixel. The scaled draw resamples, so a band's outermost row of
# pixels is blended with its neighbour and drops out of an exact-colour box.
TOLERANCE = 1.0

# name -> (nominal device scale, the config that gives this one's UNSCALED
# cell). A configuration at font zoom 1 is its own reference.
CONFIGS = {
    'gdk1': (1.0, 'gdk1'),
    'gdk2': (2.0, 'gdk2'),
    'gdk2-dpi0.75': (2.0, 'gdk2-dpi0.75'),
    'zoom1.5': (1.0, 'gdk1'),
    'zoom2': (1.0, 'gdk1'),
    'wl1': (1.0, 'wl1'),
    'wl1.5': (1.5, 'wl1.5'),
    'wl2': (2.0, 'wl2'),
}


def box(png, colour, crop=None):
    """Bounding box of the pixels of one colour, as (x, y, w, h).

    Everything else is painted black and the colour white, so ImageMagick's
    trim box is the region wanted. A small fuzz is deliberate: a resampled
    edge is no longer the exact colour, and the alternative - an exact match -
    measures the fully saturated interior and reports every scaled image as
    two pixels too small.
    """
    argv = ['magick', png]
    if crop is not None:
        argv += ['-crop', crop, '+repage']
    argv += ['-fuzz', '20%',
             '-fill', 'black', '+opaque', colour,
             '-fill', 'white', '-opaque', colour,
             '-format', '%@', 'info:']
    out = subprocess.run(argv, capture_output=True, text=True).stdout.strip()
    if not out or out.startswith('0x0'):
        return None

    size, x, y = out.split('+')
    w, h = size.split('x')
    return int(x), int(y), int(w), int(h)


class Frame:
    """What one captured frame says about the cell and the image."""

    def __init__(self, png):
        self.png = png

        size = subprocess.run(['magick', 'identify', '-format', '%w %h', png],
                              capture_output=True, text=True)
        if size.returncode != 0:
            raise RuntimeError('%s: %s' % (png, size.stderr.strip()))
        width, height = (int(v) for v in size.stdout.split())

        marker = box(png, MARKER_COLOUR)
        if marker is None:
            raise RuntimeError('%s: no marker block - the fixture never '
                               'reached the state the measurement describes'
                               % png)

        mx, my, mw, mh = marker
        self.cell_w = mw / MARKER_COLS
        self.cell_h = mh / MARKER_ROWS
        self.origin_x = mx
        self.origin_y = my - (MARKER_ROW - 1) * self.cell_h

        # Row 10 splits the frame between the two images. Below it there is
        # only the second, above it only the first.
        split = int(round(self.origin_y + (IMAGE2_ROW - 1) * self.cell_h))

        first = box(png, BAND_COLOUR, '%dx%d+0+0' % (width, split))
        second = box(png, BAND_COLOUR,
                     '%dx%d+0+%d' % (width, height - split, split))
        if first is None or second is None:
            raise RuntimeError('%s: %s image band is missing - the fixture '
                               'never reached the state the measurement '
                               'describes'
                               % (png, 'first' if first is None else 'second'))

        self.band_x, self.band_y, self.band_w, self.band_h = first
        self.band_y -= self.origin_y
        self.second_x = second[0]


def check(name, frame, ref, failures):
    """Compare one frame against the law, and record what does not hold."""
    device, _ = CONFIGS[name]
    zoom_x = frame.cell_w / ref.cell_w
    zoom_y = frame.cell_h / ref.cell_h

    def want(value, expected, what):
        if abs(value - expected) > TOLERANCE:
            failures.append('%s: %s is %g, want %g' % (name, what, value,
                                                       expected))

    want(frame.band_w, IMAGE_W * device * zoom_x, 'drawn width')
    want(frame.band_y, BAND_Y * device * zoom_y, 'band top')
    want(frame.band_h, BAND_H * device * zoom_y, 'band height')

    # The image is placed by cell, so the copy five cells right of the first
    # has to be drawn exactly five cells right of it. An image sized against
    # one cell and positioned against another still lands on the origin, so
    # the second copy is the only one of the two that can tell.
    want(frame.band_x, frame.origin_x, 'first image left edge')
    want(frame.second_x - frame.origin_x, (IMAGE2_COL - 1) * frame.cell_w,
         'second image left edge')

    return zoom_x, zoom_y


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2

    directory = argv[1]
    names = argv[2:]

    frames = {}
    for name in names:
        if name not in CONFIGS:
            sys.stderr.write('unknown configuration %s\n' % name)
            return 2
        frames[name] = Frame('%s/%s.png' % (directory, name))

    print('%-14s %-11s %-7s %-7s %-9s %-9s %s'
          % ('config', 'cell', 'device', 'zoom', 'width', 'band top',
             'band height'))

    failures = []
    for name in names:
        frame = frames[name]
        ref_name = CONFIGS[name][1]
        if ref_name not in frames:
            failures.append('%s: needs %s in the same run to know its '
                            'unscaled cell' % (name, ref_name))
            continue

        zoom_x, zoom_y = check(name, frame, frames[ref_name], failures)
        print('%-14s %-11s %-7s %-7s %-9s %-9s %s'
              % (name,
                 '%gx%g' % (frame.cell_w, frame.cell_h),
                 '%g' % CONFIGS[name][0],
                 '%.2fx%.2f' % (zoom_x, zoom_y),
                 '%g px' % frame.band_w,
                 '%g px' % frame.band_y,
                 '%g px' % frame.band_h))

    if failures:
        print()
        for failure in failures:
            print('FAIL: %s' % failure)
        return 1

    print('\nPASS: every image scales by its device scale times its zoom, '
          'and lands on its cell')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
