# SIXEL as a property of cells

This branch (`sixel-cellstore`) is a fork of VTE 0.84 that makes inline images
work the way VTE's own architecture says they should: an image is an attribute
of the cells it covers, not an entry in a side table keyed by row.

Everything below is about *why that distinction is the whole problem*.

## The state of SIXEL in upstream VTE

VTE has a SIXEL parser and it works. It is on `master` today. It is also
deleted from every stable branch: commit `3bd4f9a3` removes it, and that commit
carries ten cherry-pick lines - the same deletion re-applied to every stable
branch since `vte-0-62` in 2020. It is a standing policy, not a judgement about
any one release.

The reason is not security, and it is not manpower first. In the maintainer's
words (vte#253, note_1872962):

> the cells don't really contain the image, they are just covered by it

and the deletion commit itself says the feature is "not in a releasable state
with important and fundamental problems still unsolved".

## Why that one sentence is the entire design problem

VTE's ring is the single source of truth, and every mechanism VTE owns operates
on **cells**:

- reflow on resize (rewrap)
- insert-character / delete-character / erase
- freeze and thaw between memory and the attr / text / row streams
- run-length encoding inside the attr stream
- selection, clipboard, accessibility text
- garbage collection

An artifact addressed by *row position* instead of by cell is outside all of
them at once. So upstream's implementation must reimplement each mechanism by
hand for images, and the tree shows exactly that: `m_image_by_top_map`,
`rebuild_image_top_map`, `drop_images_before` / `drop_images_after`,
`drop_images_torn_by_rewrap`, `rewrap_images_in_range`,
`shift_images_for_insert` / `shift_images_for_remove`. One function per
mechanism the images fell out of. Each one is a place where the shadow model and
the real model can drift, and the visible bugs (an image that does not move when
the text under it rewraps, an image that survives the deletion of its own
characters) are that drift surfacing.

The fix is therefore not "handle rewrap". It is: put the image where the truth
already lives, and then delete the shadow.

## The design

The precedent is already in the tree and already accepted upstream:
**hyperlinks**. A cell holds an index into a table; the table is mark-and-swept
(`hyperlink_gc`); the index rides through freeze/thaw in the attr record's tail.
Images get the same shape, in six parts.

### 1. A packed per-cell reference

`vte::image::Ref { pool_id, tile_row, tile_col }`, packed into 32 bits and
carried in the cell attributes (`src/image-ref.hh`).

The coordinate is a position **within the image**, never a position on screen.
That single choice is what makes reflow correct for free: when a cell is wrapped
onto another line, it still names the same piece of the picture, because it
never named a screen location to begin with. Rewrap needs to know nothing about
images beyond carrying the attribute it already carries.

Packing is *total*: every field is masked in the constructor, and `Ref::fits()`
lets the placement path refuse a coordinate rather than store a silently masked
one.

The tile fields are 9 bits per axis, which constrains what the layout cell may
be. Images are laid out against the **font's** cell, floored at 4x8 and
*computed once per session and then locked*:

- Using the font's cell is what makes images render 1:1 and, more importantly,
  undistorted. Scaling x and y independently against a notional 10x20 cell made
  a 200x150 image render 160x127 and turned a 180px circle into an ellipse.
- It is also the only way the terminal tells one story: XTSMGRAPHICS used to
  report a notional cell while CSI 14t and `TIOCGWINSZ` reported the font's, so
  an application sizing an image was wrong by exactly their ratio depending on
  which channel it read.
- The floor is what keeps the 9-bit fields safe - `ceil(2048/4)` is exactly 512
  and `ceil(2052/8)` is 257 - and it is *applied* in the sole producer,
  `Terminal::image_cell_size()`. An earlier version asserted against minimum
  cell constants that were enforced nowhere, which is not an invariant, just
  decoration; the widget clamps font metrics only to 1x2.
- Locking it for the session is what closes the upstream zoom bug (emit image,
  zoom, emit the same image, get a different size). A footprint is written into
  cells and into the frozen attr stream where it cannot be revised, so a layout
  cell that moved under a font change would rescale every image already placed.
  Verified against a real font-scale change: an image placed before a 1.6x zoom
  renders 40px, one placed after renders 65px - they scale together, which is
  what the bug asks for.

### 2. An id pool with a quarantine

`src/image-pool.hh`: a `Free` / `Live` / `Retired` mark-and-sweep pool that
deliberately mirrors `hyperlink_gc`, so images inherit a cost profile the ring
has already accepted.

It exists for one hazard. The pool id is 14 bits with no room for a generation
counter, so if an id were freed while a scrollback cell still held it and then
reused, that stale cell would draw *the new image, at the old cell's tile
coordinates* - a slice of one picture embedded inside another. `retire()` moves
`Live -> Retired`; only a completed sweep moves `Retired -> Free`, and only for
ids nothing marked.

The consequence every caller must accept: **`lookup()` returning `nullptr` is
normal**, not an error. It is what a cell that has outlived its image looks
like. Draw background; never assert.

### 3. Drawing that asks the cells

Images are painted a stripe-run at a time from inside `draw_rows()`, resolved
per cell, instead of as one whole-image blit outside it. Once the cells are the
truth, the drawing path has to consult them - otherwise you have reintroduced
the shadow model in the renderer.

### 4. Pixels in a fourth ring stream

The ring has three streams (attr, text, row). This adds a fourth for image
pixels, created and destroyed with the others and reclaimed at the same trim
site via `_vte_stream_advance_tail`.

This changes eviction from *loss* into *paging*. An image dropped under memory
pressure is spilled, and `thaw_row()` faults it back in and allocates it a fresh
pool id. The budget bounds how much memory images occupy, not what the user is
allowed to have seen.

One policy distinction matters: spill from the **eviction** paths only
(`image_gc`, `drop_images_before`), never from `erase_image`. Eviction is the
ring reclaiming memory behind the user's back and must be invisible; an erase is
the user saying the image should go, and resurrecting it would be a bug.

The identifier written to disk is the image's **priority**, not its pool id.
Priority is monotonic and never reused; pool ids are recycled, so replaying one
from the stream would resolve a thawed cell to a different image - the exact
aliasing the pool exists to prevent, let back in through the stream.

### 5. A real memory bound, as public API

`VteTerminal:image-limit` (bytes; default 35 MiB), with getter, setter and
GObject property, applied to both screens' rings - deliberately parallel to
`vte_terminal_set_scrollback_lines()`. The maintainer asked for exactly this
twice (vte#255 note_968309, vte#2084).

It reframes the resource question. Images are bounded by *resource policy*
rather than by refusing to parse, and zero is meaningful: no image memory means
images are off. Because of the fourth stream, hitting the limit is not data
loss.

### 6. Cells that have something to say

The covered cells hold U+FFFC (OBJECT REPLACEMENT CHARACTER), so selection,
clipboard and accessibility have a representation - the third item on the
maintainer's 2020 ship-blocker list. Copying an image region yields blanks
rather than U+FFFC in the clipboard; the accessibility text keeps the marker.

### What falls out

With cells as the only truth, the row-anchored bookkeeping is **deleted**, and
image lifetime stops being a subsystem: it is the pool sweep. The design is not
complete when images survive rewrap - it is complete when nothing in the
codebase knows where an image is except the cells it covers.

## What is verified, and how

- `test-ring` and `test-sixel` under `meson test`, including a headless
  golden-frame render harness (`src/tests/sixel/`) whose fixtures are generated
  by a checked-in script rather than being magic bytes.
- Caveat on the render tests: a golden is a *pixel* comparison, and while the
  cells an image occupies are font-independent, the pixels are not. The harness
  pins `Monospace 12`, but fontconfig still resolves that to whatever your
  system has. If every render case fails at once and the `.actual.png` frames
  look correct but uniformly scaled, that is this - regenerate with
  `render-test.sh ... --update-golden`. The reply-conformance test avoids the
  problem entirely by asserting that XTSMGRAPHICS *agrees with* CSI 14t rather
  than pinning either to a constant.
- The load-bearing tests are **mutation-verified**: the code was broken on
  purpose to confirm each test actually fails. This caught three tests that
  passed and were worthless, including a memory-budget test whose fixture placed
  every image on the same ring row, so only one image was ever resident and the
  budget was never approached.
- The attr-stream RLE trap is *measured*, not assumed: on a 500-column row,
  identical attributes cost 26 bytes and an image reference per cell costs
  15026. The wire format stores one reference per run and recovers the per-cell
  tile column by counting, which is what keeps it constant.
- A hyperlink-heavy scrollback render is pixel-identical to the pre-refactor
  baseline, as a check that the ring changes did not disturb existing behaviour.
- `perf/fairness/` measures whether a terminal flooding sixels starves a sibling
  terminal in the same process; `perf/a11y-image/` asserts the accessibility and
  clipboard text paths disagree in exactly the intended way.

## What is not done

- Cross-terminal processing fairness is measured and mitigated with a shorter
  decode slice, but a flooding tab is not fully accounted for in
  `process_incoming`.
- Accessibility is a position marker and a U+FFFC convention; no Orca
  announcement work.
- The image dimension caps are held at 1024x1026 on purpose: DECGRA raster
  attributes force an allocation proportional to the *declared* size with zero
  image data present, which the memory budget does not bound.
- Not validated against the full spread of sixel producers; `img2sixel`, `lsix`
  and `chafa` are what has been exercised.

## Building

```sh
meson setup _b
ninja -C _b
meson test -C _b
```

To use it from GNOME Console, see the companion fork
[FamiliarTools/console](https://github.com/FamiliarTools/console) branch
`sixel`, which adds a `sixel-enabled` GSettings key bound to
`VteTerminal:enable-sixel`.

## Provenance, and why this is not an upstream contribution

This branch was written with AI assistance. VTE's `README.md` states:

> You may not contribute any code that was written, whether wholly or partly,
> by using AI in any form.

So this is **not upstreamable as authored**, regardless of its merits, and no
merge request has been or will be opened. It is published as a working fork and
as a reference implementation of a design upstream has described but not built.
Take the design; the code is disqualified by policy.

## Licence

Unchanged from upstream. VTE is LGPL-3.0-or-later (see `COPYING.LGPL3`,
`COPYING.GPL3`, `COPYING.CC-BY-4-0`, `COPYING.XTERM`, `COPYING.README`); new
library files here carry LGPL-3.0-or-later headers and new standalone programs
carry GPL-3.0-or-later.
