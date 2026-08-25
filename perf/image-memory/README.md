# Does the image budget bound the PROCESS?

`VteTerminal:image-limit` is enforced against `m_image_fast_memory_used`, a
number the ring keeps for itself, and the unit tests assert against that same
number. That is circular. It proves the ring agrees with its own bookkeeping,
which is worth having, but it cannot answer the question a user actually asks -
does a terminal that has scrolled a few hundred previews away stop growing -
because a budget honoured perfectly against an undercount is still unbounded.

Only the kernel can answer that, so this asks the kernel: Pss from
`/proc/<pid>/smaps_rollup`, sampled through a real flood driven over a real pty
into a real widget on a real display.

## The shape of the measurement

Every number here is a DIFFERENCE. A terminal's resident set is mostly fonts,
glyph caches and a drawing surface, and those swamp the quantity of interest,
so four phases run identically and differ in exactly one thing:

    images-default   the flood at the widget's own budget
    images-<n>       the same flood at a caller-set budget
    images-off       the same flood at a budget of ZERO
    text             a byte-matched flood of ordinary text

`images-off` is the control that matters, and it exists because of a deliberate
property of the API: a budget of zero does not refuse to parse a sixel, it
evicts the image the instant it is placed. So that phase pushes the same bytes
through the same parser, occupies the same cells, and scrolls the same rows
away. It differs from a budgeted phase in whether the PIXELS stay resident, and
in nothing else. The `text` phase is the cruder control, and it is here to rule
out the one suspicion the image phases share: that the parser or the wire is
what is being measured.

Three things have to come out of it:

- a **plateau** - the last preview costs what the hundredth did, because a
  budget is a ceiling and not a rate;
- a **ceiling that moves** - halving the knob has to halve the ceiling, or the
  knob is decorative;
- a **reuse** - Pss after a reset cannot distinguish memory the terminal
  released from memory the allocator is sitting on, so the flood is run a
  second time. Released memory means the second flood ends where the first one
  did rather than a budget higher.

## Running it

```sh
perf/image-memory/run.sh _build/src/app/vte-2.91 300 100 8388608
```

Arguments are the app, the preview count, the frame to take the middle sample
at, and the second budget to try. It needs `Xvfb`, `img2sixel` and
ImageMagick's `convert`, and exits 77 without them. It takes about twenty
minutes and writes half a gigabyte of temporaries, which is why it is a
measurement here rather than a meson test.

The fixture is `child.sh`: each preview is a distinct 800x480 plasma at 64
colours followed by 48 blank lines, so every frame lands on rows no other frame
ever occupies. That padding is the whole difference between measuring 300
scrolled-away images and measuring one image replaced 300 times, and the second
of those is a mistake this branch has already made once - a budget test that put
every image on the same ring row kept exactly one resident, never approached
the limit, and passed. So the run asserts its fixture reached the intended state
before it reports anything: the image stream has to hold roughly
`previews x width x height x 4` bytes, which only happens if every preview was
resident and then evicted.

## Measured on this tree

GTK3, X11 under Xvfb, `Monospace 12`, cairo renderer, glibc malloc. 300
previews of 800x480, each 1.46 MiB of pixels, each scrolled entirely away.

    phase             baseline  at-100  at-300  post-RIS  reflood  on-disk  biggest
    images-default        27.7    67.3    67.2      33.8     68.8    407.9    405.9
    images-8388608        27.6    40.6    42.2      31.9     41.3    434.3    432.2
    images-off            27.7    31.4    31.3      31.3     31.4    440.2    439.6
    text                  27.1    29.3    28.8      29.2     29.3    129.3    104.3

MiB of Pss, except the last two columns: all unlinked temporaries the process
still holds open, and the largest single one, which in an image phase is the
image stream.

The fixture check comes first, because nothing above it means anything without
it. The image stream in `images-off` holds 439.6 MiB against the 439.5 MiB of
raw pixels that 300 previews of this size contain, so all 300 really were
resident and all 300 really were evicted - not one preview overwritten 300
times.

**The plateau holds.** Previews 100 to 300 - two hundred more pictures, 293 MiB
more raw pixels - cost -0.1 MiB. Across three runs of this harness that term
ranged from -0.1 to +1.6 MiB, and so did it in the two phases that hold NO
images, which is what says the spread is the noise floor rather than growth.

**The ceiling moves with the knob**, both figures against `images-off`, so both
are pixels and nothing else:

    resident pixels at the 35 MiB default    36.0 MiB   (36.0 to 37.3 over three runs)
    resident pixels at an 8 MiB budget       10.9 MiB   ( 9.0 to 10.9 over three runs)

Each sits one to three MiB above its budget, which is what an allocator that
does not hand back every freed 1.46 MiB chunk looks like, not a bound that
leaks: the overshoot does not grow with the number of previews, only the
budget does.

Against the cruder control, the whole image flood costs 67.2 - 28.8 = **38.4
MiB over a byte-matched text flood** (38.4 to 39.8 over three runs) for a 35
MiB budget. The gap splits into the allocator overshoot above and 2.5 MiB of
per-image structure that is not pixels at all - the cell references, the pool,
the spill index - which is exactly what `images-off` minus `text` measures.

**The memory is reused, not retained.** The post-RIS column is allocator
behaviour and must not be read as anything else: the same phase returned 0.2,
17.5 and 33.4 MiB on three runs of the same harness. The reflood column is what
decides it. After RIS, flooding a hundred more previews ends at 68.8 MiB - what
the first flood cost, within 1.6 MiB, rather than a budget above it. A terminal
that had held on to the first flood's pixels would have finished near 104.

## What this costs, said plainly

The RAM bound is real and it is bought with disk. 35 MiB resident cost 406 MiB
of unlinked temporary file, because an evicted image is spilled rather than
dropped so that scrolling back to it still shows the picture.

The two ends agree, and that is the cross-check worth having, because they come
from different places entirely - kernel Pss on one side, a file size on the
other. 300 previews contain 439.5 MiB of pixels; what the image stream does NOT
hold is what stayed in RAM:

    images-default   image stream 405.9 MiB, so 33.6 MiB never left RAM   (budget 35)
    images-8388608   image stream 432.2 MiB, so  7.2 MiB never left RAM   (budget  8)
    images-off       image stream 439.6 MiB, so -0.1 MiB never left RAM   (budget  0)

Those three file sizes were byte-identical across runs.

The disk side is bounded by the SCROLLBACK, not by `image-limit`.
`reclaim_image_spill()` advances the stream's tail as rows leave the ring, so a
terminal with a finite scrollback has a finite image stream - arithmetic, not
measurement: at 72 rows per preview here, a 10000-line scrollback retains about
139 previews, or roughly 200 MiB. This harness runs the app at its default of
INFINITE scrollback, which is the worst case on purpose.

## What it does not measure

- One process, one terminal. Several image-heavy tabs in one `kgx` each carry
  their own budget, and 35 MiB is a per-terminal number.
- The declared-raster allocation. DECGRA sizes a buffer from the declared
  raster before any image data arrives, so the dimension caps and not this
  budget are what bound it; see the note in `SIXEL.md`.
- Anything about a real image producer's working set. These are synthetic
  plasmas at a fixed size, chosen so the arithmetic is checkable.
