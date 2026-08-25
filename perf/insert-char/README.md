# What does the image choke point cost text that has no image?

Every write to a cell has to tell the ring about it, because overwriting cells
is the only way a producer can take a sixel back. `insert_char()` therefore
calls `Terminal::erase_images_in_rect()` for the character it is about to
write, and that is once PER CHARACTER.

The bulk text path does not: `insert_single_width_chars()` calls it once per
RUN, over the whole extent it is about to fill. So only the characters that
fall back to `insert_char()` can pay per character at all - wide ones, and
combining marks. This measures what they pay.

The number this replaces was 6.6%, carried as a claim with no harness behind
it. It does not reproduce.

## The method

`insert-char(1)` feeds one `VteTerminal` a large buffer and reports the CPU the
parse took. Two properties make it a measurement rather than a timing:

- `process_incoming()` drains the whole incoming queue in a single call, so the
  entire payload is parsed inside one frame-clock tick and the process burns no
  CPU waiting for frames;
- the counters are gated. Counting the whole process would charge the parse for
  GTK startup and for building the payload, both of which grow with the
  character count and so do not cancel out of a per-character figure, so `perf
  stat` is started with `-D -1` and the workload enables the counters around
  the flood and disables them after.

Three populations, and the third is the control:

    cjk         CJK ideographs, every one of them double width
    combining   a base letter plus U+0301, the other insert_char() call site
    ascii       plain letters, which go through insert_single_width_chars()

`ascii` is what says the A/B is measuring the call and not the weather: the
call being removed is not on that path, so its numbers have to be unchanged.

The fixture is checked before any number is reported. A payload that turned out
not to be double width would be routed through `insert_single_width_chars()`
and the run would silently measure the wrong path, so the harness feeds ten
characters and asks the widget where the cursor ended up. If `cjk` does not
reach column 20 it prints `FIXTURE FAILED` and exits non-zero.

The check earns that description: replacing the ideographs in `append_char()`
with plain letters produces

    FIXTURE FAILED: cjk payload put the cursor at column 10 after 10
    characters, expected 20

with the counters reading `<not counted>` and the runner exiting 1, so a
mismeasured arm cannot come back as a number.

## Running it

```sh
perf/insert-char/run.sh 5 10000000            # repeats, characters
perf/insert-char/run.sh 5 10000000 cjk        # one population
```

It needs `perf` and `Xvfb`, and exits 77 without them. `VTE_BUILDDIR` selects
the build directory, default `_build`. The script refuses to run against a
build directory with work outstanding: the whole result here is a difference
between two builds of `libvte`, so a stale object would BE the result.

## Measured on this tree

Arm A is the tree. Arm B is the tree with both `erase_images_in_rect()` calls
in `insert_char()` compiled out, `ninja` run to "no work to do" in between.
10,000,000 characters, `perf stat -r 5`, parse-only window.

    cjk                instructions:u          cpu-cycles:u        task-clock:u
      A            5 650 791 496 +-0.00%   925 500 956 +-0.51%   207.79 ms +-2.85%
      B            5 610 788 980 +-0.00%   889 283 618 +-0.34%   210.02 ms +-2.38%
      the call          +40 002 516            +36 217 338          not resolvable
                             +0.71%                 +4.07%

    combining          instructions:u          cpu-cycles:u        task-clock:u
      A           16 000 203 846 +-0.00%  3 622 427 391 +-0.16%   812.31 ms +-0.26%
      B           15 910 409 309 +-0.00%  3 594 903 600 +-0.16%   807.15 ms +-0.17%
      the call          +89 794 537            +27 523 791
                             +0.56%                 +0.76%

    ascii (control)    instructions:u          cpu-cycles:u        task-clock:u
      A            1 189 400 508 +-0.00%   172 536 736 +-0.35%    42.42 ms +-2.72%
      B            1 188 993 850 +-0.00%   173 210 746 +-0.88%    42.18 ms +-2.08%
      the call             +406 658              -674 010
                             +0.03%                 -0.39%

The control is flat, in both directions, so the arms differ in the call and in
nothing else.

Per wide character the call is **4.00 instructions and about 3.3 cycles**,
against a CJK parse that costs 565 instructions and 93 cycles per character.
The instruction figure is the reliable one: it reproduced to seven digits
across three independent A/B pairs, including one taken with the counters
ungated (+40 136 944 there, +40 002 516 and +40 001 815 gated). The cycle
figure moved between 2.9 and 3.6 cycles per character across those same pairs.
The arm A `cjk` count came back as 5 650 791 496, 5 650 792 894 and
5 650 791 564 across three separate relinks of arm A, which agree to eight
significant figures.

`task-clock` cannot see it. The two arms landed within each other's error bars
every time, in both directions, because run-to-run spread there is +-2.5% and
the effect is smaller than that.

## The verdict

**Measured at 4.07% of the cycles of a stream made entirely of wide characters,
0.03% of ordinary text, and below the resolution of wall CPU time. Below the
action threshold; the guard is already the idiomatic minimum.**

The guard is one inline test - `if (!m_screen->row_data->has_images())
[[likely]] return;` - on a flag the ring keeps in step for exactly this
purpose, with everything else out of line. Four instructions is what that
costs, and three of them are the pointer chase to reach the flag.

Removing the remaining cost means giving wide characters the run batching that
`insert_single_width_chars()` has, which means a THIRD copy of the insertion
logic beside a function whose own comment reads "much of this method is
duplicated below in insert_single_width_chars(). Make sure to keep the two in
sync!". That is the most expensive change anyone could make to this file, and
what it buys is at most four percent of a workload that is 100% CJK with no
image on screen. It is not a trade worth making, and the 6.6% that would have
argued for it was never there.
