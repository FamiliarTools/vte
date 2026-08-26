/*
 * Copyright © 2026 Guilherme Fontes
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <https://www.gnu.org/licenses/>.
 */

/* Tests for the image lifetime rules of the ring: an image is resident for
 * exactly as long as the ring still holds at least one of the rows it covers,
 * and the memory it is charged for goes with it.
 *
 * Every path that destroys rows has to say so to the image maps, since the maps
 * are keyed by row number and nothing else notices that a row number has
 * stopped naming a row.
 */

#include "config.h"

#include <glib.h>

#include "ring.hh"
#include "vterowdata.hh"
#include <map>
#include <set>
#include <utility>

#include "cell.hh"
#include "image-ref.hh"
#include "image-pool.hh"

#if WITH_SIXEL

#include <cairo.h>

#include "cairo-glue.hh"

using namespace vte::base;

static int const kCellWidth = 10;
static int const kCellHeight = 20;

/* The invariant the maps are supposed to keep is Ring::validate_images(), and
 * these tests call it directly rather than restating it here.
 *
 * A second copy of an invariant in the test file is worth very little: it can
 * agree with the ring while the ring's own copy is wrong, and it is the ring's
 * copy that every other caller of validate() relies on. Calling it is also the
 * only way it runs at all, since validate() is behind VTE_DEBUG and no shipping
 * build and no default test run turns that on.
 *
 * What it checks:
 *
 * (a) Residency. An image whose every row has left the ring is unreachable: it
 *     cannot be drawn (its rows are not in the viewport and never will be
 *     again, since row numbers only ever grow), it cannot be erased by a verb
 *     (no rectangle in ring coordinates reaches it) and it cannot be moved. It
 *     is pure retention, so it has to be gone.
 * (b) Accounting. The image memory counter is what the GC spends its budget
 *     against, so it has to be the sum over exactly the images that are
 *     resident. A row-destroying path that frees nothing overcounts, and the
 *     overcount is then paid for by evicting images that are still on screen.
 * (c) Indexing. The by-top map holds the same images as the priority map, each
 *     filed under the row it really starts at.
 * (d) Anchoring. Every cell that names an image holds U+FFFC as one whole cell,
 *     and sits exactly where its tile coordinate says that piece of the picture
 *     belongs. The cells are what the draw walks and what every text-moving
 *     operation moves; the rectangle is what every lifetime rule reads. They
 *     have to say the same thing.
 */

/* Append @n rows carrying one cell of text each, so that they are real rows
 * with content rather than untouched array slots.
 */
static void
append_rows(Ring& ring,
            int n)
{
        auto cell = VteCell{};
        cell.c = 'x';
        cell.attr.set_columns(1);

        for (auto i = 0; i < n; i++) {
                auto const row = ring.append(0);
                _vte_row_data_append(row, &cell);
        }

        /* Appending to a full ring discards rows off the front, which is the
         * commonest way an image stops being reachable, so check here rather
         * than leave it to every caller to remember.
         */
        ring.validate_images();
}

/* Widen @row to @n cells of text. append_rows() gives each row a single cell,
 * which is enough to anchor an image but not to erase part of one.
 */
static void
widen_row(Ring& ring,
          long row,
          int n)
{
        auto cell = VteCell{};
        cell.c = 'x';
        cell.attr.set_columns(1);

        auto* const data = ring.index_writable(row);
        while (data->len < n)
                _vte_row_data_append(data, &cell);
}

/* Place an image @rows_tall rows tall and four cells wide, with its top left
 * corner at ring row @top, column @left, then end its emission burst the way
 * the sixel path does.
 *
 * @left has to be the column the caller then stamps at: the image's rectangle
 * and the cells that carry it are two halves of one fact, and a fixture that
 * puts them in different places is not a state the terminal can produce.
 */
static void
place_image(Ring& ring,
            long top,
            int rows_tall,
            long left = 0)
{
        auto const width_px = 4 * kCellWidth;
        auto const height_px = rows_tall * kCellHeight;
        auto surface = vte::take_freeable
                (cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width_px, height_px));

        ring.append_image(std::move(surface),
                          width_px, height_px,
                          left, top,
                          kCellWidth, kCellHeight);
        ring.set_placing_image(nullptr);

        /* Placing runs the image GC, so it both adds to the maps and may evict
         * from them.
         */
        ring.validate_images();
}

/* Ring::resize() lowering the maximum drops rows off the front, exactly as
 * discard_one_row() does one row at a time. With the scrollback off the maximum
 * IS the row count, so making the window shorter drops rows here every time.
 */
static void
test_ring_image_resize_drops(void)
{
        /* The alternate screen's shape: no streams, maximum == visible rows. */
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);

        append_rows(ring, 24);
        g_assert_cmpuint(ring.delta(), ==, 0);
        g_assert_cmpuint(ring.next(), ==, 24);

        /* Rows 2..4, well inside the region the shrink is about to drop. */
        place_image(ring, 2, 3);
        g_assert_cmpuint(ring.image_map().size(), ==, 1);
        g_assert_true(ring.has_images());

        /* The window is made shorter: 24 rows down to 12. Rows 0..11 go. */
        ring.resize(12);
        g_assert_cmpuint(ring.delta(), ==, 12);

        ring.validate_images();
        g_assert_cmpuint(ring.image_map().size(), ==, 0);
        g_assert_cmpuint(ring.image_memory_used(), ==, 0);
        g_assert_false(ring.has_images());
}

/* The same shrink must not take an image that still has a row in the ring:
 * "left the ring" means every row, not the first one.
 */
static void
test_ring_image_resize_keeps_straddling(void)
{
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);

        append_rows(ring, 24);

        /* Rows 10..14: the shrink to 12 cuts through it at row 12. */
        place_image(ring, 10, 5);
        auto const used = ring.image_memory_used();
        g_assert_cmpuint(used, >, 0);

        ring.resize(12);
        g_assert_cmpuint(ring.delta(), ==, 12);

        ring.validate_images();
        g_assert_cmpuint(ring.image_map().size(), ==, 1);
        g_assert_cmpuint(ring.image_memory_used(), ==, used);

        /* And it goes once its last row follows. */
        ring.resize(9);
        g_assert_cmpuint(ring.delta(), ==, 15);
        ring.validate_images();
        g_assert_cmpuint(ring.image_map().size(), ==, 0);
}

/* Raising the maximum destroys no row, so it must not touch the maps. */
static void
test_ring_image_resize_grow(void)
{
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);

        append_rows(ring, 24);
        place_image(ring, 2, 3);
        auto const used = ring.image_memory_used();

        ring.resize(100);
        g_assert_cmpuint(ring.delta(), ==, 0);

        ring.validate_images();
        g_assert_cmpuint(ring.image_map().size(), ==, 1);
        g_assert_cmpuint(ring.image_memory_used(), ==, used);
}

/* The normal screen's shape: a streamed scrollback, with the maximum far above
 * the visible rows, which is what lowering the scrollback setting shrinks.
 */
static void
test_ring_image_scrollback_shrink(void)
{
        auto ring = Ring{200, true};
        ring.set_visible_rows(24);

        append_rows(ring, 200);
        g_assert_cmpuint(ring.delta(), ==, 0);

        place_image(ring, 5, 3);
        place_image(ring, 150, 3);
        g_assert_cmpuint(ring.image_map().size(), ==, 2);
        auto const used_both = ring.image_memory_used();

        /* "scrollback-lines" lowered from 200 to 100: rows 0..99 go. */
        ring.resize(100);
        g_assert_cmpuint(ring.delta(), ==, 100);

        ring.validate_images();
        g_assert_cmpuint(ring.image_map().size(), ==, 1);
        g_assert_cmpuint(ring.image_memory_used(), ==, used_both / 2);
        g_assert_cmpint(ring.image_map().begin()->second->get_top(), ==, 150);
}

/* The rows that leave through the other end are already routed; keep them
 * covered here so that the whole lifetime rule lives in one test.
 */
static void
test_ring_image_shrink_drops_below(void)
{
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);

        append_rows(ring, 24);
        place_image(ring, 20, 3);
        g_assert_cmpuint(ring.image_map().size(), ==, 1);

        ring.shrink(18);
        g_assert_cmpuint(ring.next(), ==, 18);

        ring.validate_images();
        g_assert_cmpuint(ring.image_map().size(), ==, 0);
        g_assert_cmpuint(ring.image_memory_used(), ==, 0);
}

static void
test_ring_image_discard_drops(void)
{
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);

        append_rows(ring, 24);
        place_image(ring, 2, 3);

        /* The ring is full, so each further append discards one row. */
        append_rows(ring, 6);
        g_assert_cmpuint(ring.delta(), ==, 6);

        ring.validate_images();
        g_assert_cmpuint(ring.image_map().size(), ==, 0);
        g_assert_cmpuint(ring.image_memory_used(), ==, 0);
}

static void
test_ring_image_drop_scrollback(void)
{
        auto ring = Ring{200, true};
        ring.set_visible_rows(24);

        append_rows(ring, 200);
        place_image(ring, 5, 3);

        ring.drop_scrollback(190);
        g_assert_cmpuint(ring.delta(), ==, 190);

        ring.validate_images();
        g_assert_cmpuint(ring.image_map().size(), ==, 0);
}

#endif /* WITH_SIXEL */


/* vte::image::Ref packing, and the VteCellAttr union it lives in. */

static void
test_image_ref_roundtrip(void)
{
        /* Every field survives packing, including at its maximum. */
        struct { uint32_t id, row, col; } const cases[] = {
                { 1, 0, 0 },
                { 1, 0, 1 },
                { 1, 1, 0 },
                { 12345, 7, 400 },
                { vte::image::k_ref_pool_id_max, 0, 0 },
                { 1, vte::image::k_ref_tile_row_max, 0 },
                { 1, 0, vte::image::k_ref_tile_col_max },
                { vte::image::k_ref_pool_id_max,
                  vte::image::k_ref_tile_row_max,
                  vte::image::k_ref_tile_col_max },
        };

        for (auto const& c : cases) {
                auto const ref = vte::image::Ref{c.id, c.row, c.col};
                g_assert_cmpuint(ref.pool_id(), ==, c.id);
                g_assert_cmpuint(ref.tile_row(), ==, c.row);
                g_assert_cmpuint(ref.tile_col(), ==, c.col);
                g_assert_true(ref.valid());

                /* Bits survive a trip through the raw 32-bit form. */
                g_assert_true(vte::image::Ref{ref.bits()} == ref);
        }
}

static void
test_image_ref_fields_do_not_alias(void)
{
        /* The three fields must not overlap: a maxed-out coordinate must not
         * bleed into the pool id and alias one image onto another.
         */
        auto const id_only = vte::image::Ref{vte::image::k_ref_pool_id_max, 0, 0};
        auto const row_only = vte::image::Ref{0, vte::image::k_ref_tile_row_max, 0};
        auto const col_only = vte::image::Ref{0, 0, vte::image::k_ref_tile_col_max};

        g_assert_cmpuint(id_only.bits() & row_only.bits(), ==, 0);
        g_assert_cmpuint(id_only.bits() & col_only.bits(), ==, 0);
        g_assert_cmpuint(row_only.bits() & col_only.bits(), ==, 0);

        /* Together they account for all 32 bits. */
        g_assert_cmpuint(id_only.bits() | row_only.bits() | col_only.bits(),
                         ==, 0xffffffffu);

        /* A maximal coordinate leaves the pool id alone. */
        auto const maxed = vte::image::Ref{7,
                                           vte::image::k_ref_tile_row_max,
                                           vte::image::k_ref_tile_col_max};
        g_assert_cmpuint(maxed.pool_id(), ==, 7);
}

static void
test_image_ref_zero_is_not_an_image(void)
{
        /* A zeroed Ref must not name a live image: cells are memset to zero
         * in places, and that must not conjure a reference to image 0.
         */
        g_assert_false(vte::image::Ref{}.valid());
        g_assert_false(vte::image::Ref{0u}.valid());
        auto const no_image = vte::image::Ref{vte::image::k_ref_pool_id_none, 5, 5};
        g_assert_false(no_image.valid());

        /* basic_cell is not an image cell. */
        g_assert_false(basic_cell.attr.image());
}

static void
test_image_ref_stripe_identity(void)
{
        auto const a = vte::image::Ref{42, 3, 0};
        auto const b = vte::image::Ref{42, 3, 100};
        auto const c = vte::image::Ref{42, 4, 0};
        auto const d = vte::image::Ref{43, 3, 0};

        /* Same image, same tile row: one stripe, the unit of lifetime. */
        g_assert_true(a.same_stripe(b));
        g_assert_true(a.same_image(c));
        g_assert_false(a.same_stripe(c));   /* different tile row */
        g_assert_false(a.same_image(d));
        g_assert_false(a.same_stripe(d));
}

static void
test_cell_attr_union_tagging(void)
{
        VteCell cell = basic_cell;

        /* Starts life as a hyperlink cell holding no hyperlink. */
        g_assert_false(cell.attr.image());
        g_assert_cmpuint(cell.attr.hyperlink_idx(), ==, 0);
        g_assert_cmpuint(cell.attr.hyperlink_idx_or_none(), ==, 0);

        cell.attr.set_hyperlink_idx(1234);
        g_assert_false(cell.attr.image());
        g_assert_cmpuint(cell.attr.hyperlink_idx(), ==, 1234);

        /* Storing an image reference flips the tag with it, so the tag and
         * the payload cannot disagree.
         */
        auto const ref = vte::image::Ref{99, 2, 3};
        cell.attr.set_image_ref(ref);
        g_assert_true(cell.attr.image());
        g_assert_true(cell.attr.image_ref() == ref);

        /* An image cell reports no hyperlink, in range, rather than
         * reinterpreting the image bits as an index into the GC bitmap.
         */
        g_assert_cmpuint(cell.attr.hyperlink_idx_or_none(), ==, 0);

        /* And back again. */
        cell.attr.set_hyperlink_idx(7);
        g_assert_false(cell.attr.image());
        g_assert_cmpuint(cell.attr.hyperlink_idx(), ==, 7);
}

static void
test_cell_attr_image_tag_survives_sgr_reset(void)
{
        /* reset_sgr_attributes() must not strip the tag off a cell whose
         * m_link still holds an image reference: that would reinterpret
         * those bits as a hyperlink index.
         */
        VteCell cell = basic_cell;
        auto const ref = vte::image::Ref{1234, 5, 6};
        cell.attr.set_image_ref(ref);

        cell.attr.set_bold(true);
        cell.attr.set_underline(2);
        cell.attr.reset_sgr_attributes();

        g_assert_false(cell.attr.bold());
        g_assert_true(cell.attr.image());
        g_assert_true(cell.attr.image_ref() == ref);
}

static void
test_cell_sizes_unchanged(void)
{
        /* The whole design exists to avoid growing these. */
        g_assert_cmpuint(sizeof(VteCell), ==, 20);
        g_assert_cmpuint(sizeof(VteCellAttr), ==, 16);
}


/* The image id pool. */

using TestPool = vte::image::PoolT<int>;

static void
test_image_pool_allocate_lookup(void)
{
        TestPool pool;
        int a = 1, b = 2;

        auto const ida = pool.allocate(&a);
        auto const idb = pool.allocate(&b);

        /* Never hands out the reserved "no image" id. */
        g_assert_cmpuint(ida, !=, vte::image::k_ref_pool_id_none);
        g_assert_cmpuint(idb, !=, vte::image::k_ref_pool_id_none);
        g_assert_cmpuint(ida, !=, idb);

        g_assert_true(pool.lookup(ida) == &a);
        g_assert_true(pool.lookup(idb) == &b);
        g_assert_cmpuint(pool.live_count(), ==, 2);

        /* Resolvable through a Ref, which is how the draw path will do it. */
        auto const ref = vte::image::Ref{ida, 0, 0};
        g_assert_true(pool.lookup(ref) == &a);

        /* The reserved id resolves to nothing. */
        g_assert_null(pool.lookup(vte::image::k_ref_pool_id_none));
}

static void
test_image_pool_retire_resolves_to_null(void)
{
        /* A cell outliving its image is normal, not an error: it must
         * resolve to nothing and draw as background.
         */
        TestPool pool;
        int a = 1;

        auto const ida = pool.allocate(&a);
        pool.retire(ida);

        g_assert_null(pool.lookup(ida));
        g_assert_cmpuint(pool.live_count(), ==, 0);
        g_assert_cmpuint(pool.retired_count(), ==, 1);
}

static void
test_image_pool_no_reuse_before_sweep(void)
{
        /* THE hazard this pool exists to prevent. If a retired id were
         * handed straight back out, a stale cell still holding it would
         * silently start displaying the NEW image, at the stale cell's own
         * tile coordinates - a slice of one image embedded in another.
         */
        TestPool pool;
        int a = 1, b = 2;

        auto const ida = pool.allocate(&a);
        pool.retire(ida);

        /* Allocating many times must never return the quarantined id. */
        for (int i = 0; i < 64; i++) {
                auto const id = pool.allocate(&b);
                g_assert_cmpuint(id, !=, ida);
        }
}

static void
test_image_pool_sweep_frees_unreferenced(void)
{
        TestPool pool;
        int a = 1, b = 2;

        auto const ida = pool.allocate(&a);
        pool.retire(ida);

        /* A sweep in which nothing referenced the id releases it. */
        pool.sweep_begin();
        auto const freed = pool.sweep_end();
        g_assert_cmpuint(freed, ==, 1);
        g_assert_cmpuint(pool.retired_count(), ==, 0);

        /* Only now may it come back. */
        auto const idb = pool.allocate(&b);
        g_assert_cmpuint(idb, ==, ida);
        g_assert_true(pool.lookup(idb) == &b);
}

static void
test_image_pool_sweep_keeps_referenced(void)
{
        /* A retired id that a surviving cell still names must STAY
         * quarantined across arbitrarily many sweeps.
         */
        TestPool pool;
        int a = 1, b = 2;

        auto const ida = pool.allocate(&a);
        pool.retire(ida);

        for (int round = 0; round < 8; round++) {
                pool.sweep_begin();
                pool.mark(ida);          /* a stale cell still refers to it */
                g_assert_cmpuint(pool.sweep_end(), ==, 0);
                g_assert_cmpuint(pool.retired_count(), ==, 1);

                auto const id = pool.allocate(&b);
                g_assert_cmpuint(id, !=, ida);
        }

        /* Once the last referring cell is gone, the id is reclaimed. */
        pool.sweep_begin();
        g_assert_cmpuint(pool.sweep_end(), ==, 1);
}

static void
test_image_pool_sweep_does_not_touch_live(void)
{
        /* A sweep that nobody marked must not free LIVE ids: an unmarked
         * live image is one whose cells simply were not walked, not a dead
         * one. Only Retired is a sweep's business.
         */
        TestPool pool;
        int a = 1;

        auto const ida = pool.allocate(&a);

        pool.sweep_begin();
        g_assert_cmpuint(pool.sweep_end(), ==, 0);

        g_assert_true(pool.lookup(ida) == &a);
        g_assert_cmpuint(pool.live_count(), ==, 1);
}

static void
test_image_pool_sweep_end_without_begin(void)
{
        /* An unbegun sweep must free nothing rather than everything: the
         * marks are all clear, so a naive implementation would reclaim every
         * retired id while its cells still point at them.
         */
        TestPool pool;
        int a = 1;

        auto const ida = pool.allocate(&a);
        pool.retire(ida);

        g_assert_cmpuint(pool.sweep_end(), ==, 0);
        g_assert_cmpuint(pool.retired_count(), ==, 1);
}

static void
test_image_pool_exhaustion(void)
{
        /* The id space is 14 bits and can genuinely run out. Exhaustion must
         * report failure, not wrap around onto a live id.
         */
        TestPool pool;
        int a = 1;

        std::vector<uint32_t> ids;
        for (;;) {
                auto const id = pool.allocate(&a);
                if (id == vte::image::k_ref_pool_id_none)
                        break;
                ids.push_back(id);
                g_assert_cmpuint(ids.size(), <=, vte::image::k_ref_pool_id_max);
        }

        /* Exactly the ids 1..max, each handed out once. */
        g_assert_cmpuint(ids.size(), ==, vte::image::k_ref_pool_id_max);
        g_assert_cmpuint(pool.available(), ==, 0);

        /* Every id fits the Ref field it has to live in. */
        for (auto const id : ids) {
                auto const ref = vte::image::Ref{id, 0, 0};
                g_assert_cmpuint(ref.pool_id(), ==, id);
        }

        /* Still exhausted while everything is live. */
        g_assert_cmpuint(pool.allocate(&a), ==, vte::image::k_ref_pool_id_none);

        /* Retiring alone does not help; only a completed sweep does. */
        pool.retire(ids[0]);
        g_assert_cmpuint(pool.allocate(&a), ==, vte::image::k_ref_pool_id_none);

        pool.sweep_begin();
        g_assert_cmpuint(pool.sweep_end(), ==, 1);
        g_assert_cmpuint(pool.allocate(&a), ==, ids[0]);
}

static void
test_image_pool_retire_is_idempotent(void)
{
        /* Double retire must not push the id onto the free list twice, which
         * would later hand the same id to two different images at once.
         */
        TestPool pool;
        int a = 1, b = 2, c = 3;

        auto const ida = pool.allocate(&a);
        pool.retire(ida);
        pool.retire(ida);
        pool.retire(ida);

        pool.sweep_begin();
        g_assert_cmpuint(pool.sweep_end(), ==, 1);

        auto const id1 = pool.allocate(&b);
        auto const id2 = pool.allocate(&c);
        g_assert_cmpuint(id1, ==, ida);
        g_assert_cmpuint(id2, !=, id1);
        g_assert_true(pool.lookup(id1) == &b);
        g_assert_true(pool.lookup(id2) == &c);
}


/* The attr_stream run-length trap.
 *
 * freeze_row() decides whether to emit a new CellAttrChange record with
 *
 *      memcmp(&m_last_attr, &attr, sizeof (VteCellAttr))
 *
 * which is all 16 bytes of VteCellAttr - including m_link - while _attrcpy()
 * persists only VTE_CELL_ATTR_COMMON_BYTES, which is 12 and excludes it.
 *
 * So a field that VARIES PER CELL destroys the run-length coding of the attr
 * stream even though it is never written to that stream. This is the single
 * constraint that governs how image references may be encoded on the wire,
 * and it is invisible in the code: nothing near either line mentions the
 * other. Measure it rather than trusting it.
 */

enum class LinkPattern {
        Uniform,        /* no image: every cell identical */
        TileColumn,     /* one image, one stripe, tile column advances per cell */
        DistinctImage,  /* a different image per cell: genuinely different runs */
};

static size_t
freeze_cost_of_row(LinkPattern pattern)
{
        /* A ring WITH streams: freezing is the whole point here. */
        auto ring = Ring{1024, true};
        ring.set_visible_rows(24);

        auto const columns = size_t{500};

        auto const before = ring.attr_stream_head();

        auto const row = ring.append(0);
        for (size_t i = 0; i < columns; i++) {
                auto cell = basic_cell;
                cell.c = 'x';
                switch (pattern) {
                case LinkPattern::Uniform:
                        break;
                case LinkPattern::TileColumn:
                        /* What a real image row looks like: one image, one
                         * stripe, the tile column advancing per cell.
                         */
                        cell.attr.set_image_ref(vte::image::Ref{1, 0, uint32_t(i)});
                        break;
                case LinkPattern::DistinctImage:
                        /* Genuinely different runs: a separate image per cell.
                         * These MUST each cost a record.
                         */
                        cell.attr.set_image_ref(vte::image::Ref{uint32_t(i) + 1, 0, 0});
                        break;
                }
                _vte_row_data_append(row, &cell);
        }

        /* Push it out of the writable window so it is frozen. */
        append_rows(ring, 64);

        return ring.attr_stream_head() - before;
}

static void
test_attr_stream_rle_trap(void)
{
        auto const uniform = freeze_cost_of_row(LinkPattern::Uniform);
        auto const distinct = freeze_cost_of_row(LinkPattern::DistinctImage);

        g_test_message("attr_stream: uniform row %" G_GSIZE_FORMAT " bytes, "
                       "genuinely-distinct-per-cell row %" G_GSIZE_FORMAT " bytes",
                       uniform, distinct);

        /* A row of identical attributes costs a small constant: the coding
         * works. If this ever grows proportional to the row, every other
         * measurement here is meaningless and this is the canary.
         */
        g_assert_cmpuint(uniform, <, 200);

        /* Cells that genuinely belong to different images are different runs
         * and must each cost a record. This is the trap's real cost, and it
         * is what an encoding that varies per cell would pay.
         */
        g_assert_cmpuint(distinct, >, 100 * uniform);
}

static void
test_attr_stream_stripe_is_one_run(void)
{
        /* The payoff. A row of one image's stripe differs in tile column at
         * every cell, but a stripe is contiguous, so the tile column is
         * recoverable by counting from the run's first cell. Excluding it
         * from the run-length key must collapse the whole row to a constant
         * number of records - the same as a row with no image at all.
         *
         * Without this, a single max-size image would cost one 26-byte record
         * per cell in the encrypted append-only scrollback stream.
         */
        auto const uniform = freeze_cost_of_row(LinkPattern::Uniform);
        auto const stripe = freeze_cost_of_row(LinkPattern::TileColumn);

        g_test_message("attr_stream: uniform row %" G_GSIZE_FORMAT " bytes, "
                       "one-stripe image row %" G_GSIZE_FORMAT " bytes",
                       uniform, stripe);

        /* Constant, not proportional to the 500 columns.
         *
         * An image run costs a little MORE than a plain one - the record
         * carries a 4-byte image reference after the hyperlink tail - but the
         * cost is per RUN, not per cell. The bound is deliberately generous
         * about the constant and strict about the growth: 500 columns must
         * not cost anything like 500 records.
         */
        g_assert_cmpuint(stripe, <=, uniform + 64);
}



static void
test_image_ref_out_of_range_cannot_alias(void)
{
        /* A tile coordinate that exceeds its field must NEVER carry into a
         * neighbouring field. If it does, one image silently renders slices
         * of another - the exact failure the pool's id quarantine exists to
         * prevent, reintroduced through the packing instead.
         *
         * This is reachable: the widget clamps cell metrics only to 1x2
         * (vte.cc, set_font_desc sanity check) and inits them to 1x1, while
         * a max-legal image is VTE_SIXEL_MAX_WIDTH x VTE_SIXEL_MAX_HEIGHT.
         * The emulated cell (VTE_SIXEL_CELL_*) is what keeps a legal image
         * inside these fields; the packing is total anyway, so that a bug
         * elsewhere degrades to the wrong tile rather than the wrong image.
         */
        auto const overflow_col = vte::image::Ref{7, 0, vte::image::k_ref_tile_col_max + 1};
        g_assert_cmpuint(overflow_col.pool_id(), ==, 7);
        g_assert_cmpuint(overflow_col.tile_row(), ==, 0);

        auto const overflow_row = vte::image::Ref{7, vte::image::k_ref_tile_row_max + 1, 0};
        g_assert_cmpuint(overflow_row.pool_id(), ==, 7);

        /* And the caller must be able to ASK, rather than find out by
         * corruption, whether a placement fits at all.
         */
        g_assert_true(vte::image::Ref::fits(7, 0, vte::image::k_ref_tile_col_max));
        g_assert_false(vte::image::Ref::fits(7, 0, vte::image::k_ref_tile_col_max + 1));
        g_assert_false(vte::image::Ref::fits(7, vte::image::k_ref_tile_row_max + 1, 0));
        g_assert_false(vte::image::Ref::fits(vte::image::k_ref_pool_id_max + 1, 0, 0));
}


static void
test_image_ref_covers_max_legal_image(void)
{
        /* The whole point of expressing sixel geometry in a fixed emulated
         * cell: the largest image the parser will admit must fit the tile
         * fields, as a property of the constants rather than a hope about
         * what font the user picked.
         */
        g_assert_cmpint(vte::image::k_max_image_tile_cols, <=,
                        int(vte::image::k_ref_tile_col_max) + 1);
        g_assert_cmpint(vte::image::k_max_image_tile_rows, <=,
                        int(vte::image::k_ref_tile_row_max) + 1);

        /* And the extreme corner really does round-trip. */
        auto const corner = vte::image::Ref{vte::image::k_ref_pool_id_max,
                                            uint32_t(vte::image::k_max_image_tile_rows - 1),
                                            uint32_t(vte::image::k_max_image_tile_cols - 1)};
        g_assert_true(vte::image::Ref::fits(vte::image::k_ref_pool_id_max,
                                            vte::image::k_max_image_tile_rows - 1,
                                            vte::image::k_max_image_tile_cols - 1));
        g_assert_cmpuint(corner.tile_col(), ==, uint32_t(vte::image::k_max_image_tile_cols - 1));
        g_assert_cmpuint(corner.tile_row(), ==, uint32_t(vte::image::k_max_image_tile_rows - 1));
        g_assert_cmpuint(corner.pool_id(), ==, vte::image::k_ref_pool_id_max);
}


static void
test_sixel_right_margin_clip(void)
{
        auto const cell = long(VTE_SIXEL_CELL_WIDTH);
        auto const columns = 80L;

        /* Wholly inside the screen: untouched. */
        g_assert_cmpint(vte::image::clipped_width_px(96, 0, columns, cell), ==, 96);
        g_assert_cmpint(vte::image::clipped_width_px(96, 60, columns, cell), ==, 96);

        /* Overhanging: truncated to the columns that exist, and to a whole
         * number of them - a partial trailing column has no cell to live in.
         */
        g_assert_cmpint(vte::image::clipped_width_px(96, 75, columns, cell), ==, 50);
        g_assert_cmpint(vte::image::clipped_width_px(96, 79, columns, cell), ==, 10);

        /* Exactly flush with the margin. */
        g_assert_cmpint(vte::image::clipped_width_px(100, 70, columns, cell), ==, 100);

        /* No room at all, and past the end: refused rather than clamped to
         * something that would erase cells it does not cover.
         */
        g_assert_cmpint(vte::image::clipped_width_px(96, 80, columns, cell), ==, 0);
        g_assert_cmpint(vte::image::clipped_width_px(96, 81, columns, cell), ==, 0);

        /* Degenerate inputs answer zero rather than something negative that
         * would later be used as a length.
         */
        g_assert_cmpint(vte::image::clipped_width_px(0, 0, columns, cell), ==, 0);
        g_assert_cmpint(vte::image::clipped_width_px(96, 0, columns, 0), ==, 0);

        /* The clipped width always fits the columns that remain. */
        for (long left = 0; left < columns; left++) {
                auto const w = vte::image::clipped_width_px(4096, left, columns, cell);
                g_assert_cmpint(w, <=, (columns - left) * cell);
                g_assert_cmpint(w % cell, ==, 0);
        }
}


/* The pool wired into the ring: an id's life is the image's life. */

static void
test_ring_image_pool_allocates(void)
{
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        g_assert_cmpuint(ring.image_pool().live_count(), ==, 0);

        place_image(ring, 2, 3);
        place_image(ring, 8, 3);

        g_assert_cmpuint(ring.image_map().size(), ==, 2);
        g_assert_cmpuint(ring.image_pool().live_count(), ==, 2);

        /* Every resident image has a real id, and the id resolves back to
         * that same image - which is what a cell holding the id will rely on.
         */
        auto seen = std::set<uint32_t>{};
        for (auto const& [priority, image] : ring.image_map()) {
                auto const id = image->get_pool_id();
                g_assert_cmpuint(id, !=, vte::image::k_ref_pool_id_none);
                g_assert_true(ring.image_pool().lookup(id) == image.get());
                g_assert_false(seen.contains(id));
                seen.insert(id);
        }
}

static void
test_ring_image_pool_retires_with_the_image(void)
{
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        place_image(ring, 2, 3);
        auto const id = ring.image_map().begin()->second->get_pool_id();
        g_assert_cmpuint(id, !=, vte::image::k_ref_pool_id_none);

        /* Shrinking drops the rows the image covers, which frees it. */
        ring.resize(2);
        ring.validate_images();

        g_assert_cmpuint(ring.image_map().size(), ==, 0);

        /* The id must NOT resolve to the dead image any more... */
        g_assert_null(ring.image_pool().lookup(id));

        /* ...and must not be free either: it is retired, because a cell may
         * still name it. Reuse here is what would make a stale cell display
         * some future image.
         */
        g_assert_cmpuint(ring.image_pool().live_count(), ==, 0);
        g_assert_cmpuint(ring.image_pool().retired_count(), ==, 1);
}

static void
test_ring_image_pool_sweep_reclaims(void)
{
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        place_image(ring, 2, 3);
        auto const id = ring.image_map().begin()->second->get_pool_id();

        ring.resize(2);
        ring.validate_images();
        g_assert_cmpuint(ring.image_pool().retired_count(), ==, 1);

        /* No cell names it - none are stamped yet - so a sweep reclaims it.
         * The sweep must not touch anything else.
         */
        ring.sweep_image_pool_for_test();
        g_assert_cmpuint(ring.image_pool().retired_count(), ==, 0);

        /* And a live image keeps its id across a sweep. */
        auto ring2 = Ring{24, false};
        ring2.set_visible_rows(24);
        append_rows(ring2, 24);
        place_image(ring2, 2, 3);
        auto const live_id = ring2.image_map().begin()->second->get_pool_id();
        ring2.sweep_image_pool_for_test();
        g_assert_true(ring2.image_pool().lookup(live_id) ==
                      ring2.image_map().begin()->second.get());
}


static void
test_ring_image_cells_carry_the_reference(void)
{
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        place_image(ring, 2, 3);
        auto* image = ring.image_map().begin()->second.get();
        auto const id = image->get_pool_id();

        /* place_image() only appends the image; stamping is what
         * erase_image_rect() does, so drive it directly here.
         */
        ring.set_placing_image(image);
        for (auto r = 0u; r < 3u; r++)
                ring.stamp_image_row(2 + r, 0, 1, r);
        ring.set_placing_image(nullptr);

        /* Each stamped cell names the image AND its own place in it. */
        for (auto r = 0u; r < 3u; r++) {
                auto const* row = ring.index(2 + r);
                g_assert_nonnull(row);
                g_assert_cmpint(row->len, >, 0);

                auto const& attr = row->cells[0].attr;
                g_assert_true(attr.image());

                auto const ref = attr.image_ref();
                g_assert_cmpuint(ref.pool_id(), ==, id);
                g_assert_cmpuint(ref.tile_row(), ==, r);
                g_assert_cmpuint(ref.tile_col(), ==, 0);

                /* And the reference resolves back to the image itself. */
                g_assert_true(ring.image_pool().lookup(ref) == image);
        }

        /* Cells of different tile rows are different stripes, cells of the
         * same row are one stripe - the unit of lifetime.
         */
        auto const a = ring.index(2)->cells[0].attr.image_ref();
        auto const b = ring.index(3)->cells[0].attr.image_ref();
        g_assert_true(a.same_image(b));
        g_assert_false(a.same_stripe(b));
}

static void
test_ring_image_sweep_sees_cell_references(void)
{
        /* The sweep's marking loop is only worth anything if a stamped cell
         * keeps an id alive by itself.
         *
         * Use an id with NO resident image, because sweep_image_pool() also
         * marks every resident image's id - so with an image still in the map
         * the cell's contribution would be invisible, and the test would pass
         * whether or not cells were walked at all.
         */
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        auto const id = ring.image_pool().allocate(nullptr);
        g_assert_cmpuint(id, !=, vte::image::k_ref_pool_id_none);

        auto* row = ring.index_writable(2);
        g_assert_cmpint(row->len, >, 0);
        row->cells[0].attr.set_image_ref(vte::image::Ref{id, 0, 0});

        ring.image_pool().retire(id);
        g_assert_cmpuint(ring.image_pool().retired_count(), ==, 1);

        /* A cell still names it: the sweep must not reclaim it. */
        ring.sweep_image_pool_for_test();
        g_assert_cmpuint(ring.image_pool().retired_count(), ==, 1);

        /* Clear that cell, and nothing names it any more. */
        row->cells[0].attr.set_hyperlink_idx(0);
        ring.sweep_image_pool_for_test();
        g_assert_cmpuint(ring.image_pool().retired_count(), ==, 0);
}



static void
test_ring_image_anchor_follows_the_cells(void)
{
        /* The whole reason for anchoring images to cells: an operation that
         * moves TEXT must move the image, without that operation knowing
         * images exist.
         */
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        place_image(ring, 5, 3);
        auto* image = ring.image_map().begin()->second.get();
        auto const id = image->get_pool_id();

        ring.set_placing_image(image);
        for (auto r = 0u; r < 3u; r++)
                ring.stamp_image_row(5 + r, 0, 1, r);
        ring.set_placing_image(nullptr);

        auto row = Ring::row_t{};
        auto col = Ring::column_t{};
        g_assert_true(ring.find_image_anchor(id, &row, &col));
        g_assert_cmpuint(row, ==, 5);
        g_assert_cmpuint(col, ==, 0);

        /* Insert a row above it: every row below shifts down by one, and the
         * anchoring cell goes with them.
         */
        ring.insert(5, 0);
        ring.validate_images();

        g_assert_true(ring.find_image_anchor(id, &row, &col));
        g_assert_cmpuint(row, ==, 6);

        /* An id nothing names has no anchor, rather than a wrong one. */
        auto const unused = ring.image_pool().allocate(nullptr);
        g_assert_false(ring.find_image_anchor(unused, &row, &col));
        g_assert_false(ring.find_image_anchor(vte::image::k_ref_pool_id_none, &row, &col));
}

/* Collect the screen positions of the cells that name @id, as tile coordinate
 * to position, so a test can say where each piece of a picture ended up.
 */
static std::map<std::pair<uint32_t, uint32_t>, std::pair<long, long>>
image_cell_positions(Ring& ring,
                     uint32_t id)
{
        auto found = std::map<std::pair<uint32_t, uint32_t>, std::pair<long, long>>{};

        for (auto r = long(ring.delta()); r < long(ring.next()); r++) {
                auto const* const row = ring.index(r);
                if (row == nullptr)
                        continue;

                for (auto c = 0; c < row->len; c++) {
                        auto const& attr = row->cells[c].attr;
                        if (!attr.image())
                                continue;

                        auto const ref = attr.image_ref();
                        if (ref.pool_id() != id)
                                continue;

                        found[{ref.tile_row(), ref.tile_col()}] = {r, c};
                }
        }

        return found;
}

/* Move the cells of @row in the inclusive column range [@left, @right] by
 * @amount, exactly as Terminal::scroll_text_right() and scroll_text_left()
 * memmove them for ICH, DCH, SL, SR and insert mode. Positive is rightwards.
 *
 * The ring does not do this itself - the terminal does, and the ring is only
 * told about it - so a test of what the ring owes that move has to perform the
 * move.
 */
static void
scroll_row_cells(Ring& ring,
                 long row,
                 long left,
                 long right,
                 long amount)
{
        auto* const data = ring.index_writable(row);
        g_assert_cmpint(long(data->len), >=, right + 1);

        auto const span = right - left + 1;
        auto const n = span - std::abs(amount);

        if (amount > 0) {
                memmove(data->cells + left + amount, data->cells + left,
                        n * sizeof(VteCell));
                std::fill_n(&data->cells[left], amount, basic_cell);
        } else if (amount < 0) {
                memmove(data->cells + left, data->cells + left - amount,
                        n * sizeof(VteCell));
                std::fill_n(&data->cells[right + amount + 1], -amount, basic_cell);
        }
}

/* Place a @rows_tall x 4 image at (@top, @left) and stamp its cells, leaving
 * the ring in the state the sixel path leaves it in. Returns the image.
 */
static vte::image::Image*
place_and_stamp(Ring& ring,
                long top,
                int rows_tall,
                long left,
                long row_width)
{
        for (auto r = top; r < top + rows_tall; r++)
                widen_row(ring, r, row_width);

        place_image(ring, top, rows_tall, left);
        auto* const image = ring.image_map().begin()->second.get();

        ring.set_placing_image(image);
        for (auto r = 0; r < rows_tall; r++)
                ring.stamp_image_row(top + r, left, 4, r);
        ring.set_placing_image(nullptr);

        ring.validate_images();
        return image;
}

/* ICH, DCH, SL, SR and insert mode move cells sideways inside one row without
 * moving the row. A cell carries its own piece of the picture, so the picture
 * can go with them - but only if the image's own left edge is moved to match,
 * because that edge is what every lifetime rule reads and what
 * validate_images() requires to agree with the cells.
 *
 * Before this, the ring had no way to say "these cells and this bitmap move
 * together" and every such move deleted the image outright.
 */
static void
test_ring_image_follows_horizontal_scroll(void)
{
        auto const top = long{5};
        auto const left = long{2};
        auto const rows_tall = 2;
        auto const cols_wide = 4;
        auto const width = long{40};

        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        auto* const image = place_and_stamp(ring, top, rows_tall, left, width);
        auto const id = image->get_pool_id();

        /* Push one cell in at the left margin, over both of the image's rows. */
        auto damage_top = long{};
        auto damage_bottom = long{};
        g_assert_true(ring.shift_images_for_scroll(top, top + rows_tall - 1,
                                                   0, width - 1, 1,
                                                   &damage_top, &damage_bottom));
        for (auto r = top; r < top + rows_tall; r++)
                scroll_row_cells(ring, r, 0, width - 1, 1);

        ring.validate_images();

        /* The picture is still here, and it went with its cells. */
        g_assert_cmpuint(ring.image_map().size(), ==, 1);
        g_assert_cmpint(image->get_left(), ==, left + 1);
        g_assert_cmpint(image->get_top(), ==, top);

        auto const found = image_cell_positions(ring, id);
        g_assert_cmpuint(found.size(), ==, rows_tall * cols_wide);
        for (auto r = 0; r < rows_tall; r++) {
                for (auto c = 0; c < cols_wide; c++) {
                        auto const it = found.find({uint32_t(r), uint32_t(c)});
                        g_assert_true(it != found.end());
                        g_assert_cmpint(it->second.first, ==, top + r);
                        g_assert_cmpint(it->second.second, ==, left + 1 + c);
                }
            }

        /* And back the other way, which is DCH's direction. */
        g_assert_true(ring.shift_images_for_scroll(top, top + rows_tall - 1,
                                                   0, width - 1, -1,
                                                   &damage_top, &damage_bottom));
        for (auto r = top; r < top + rows_tall; r++)
                scroll_row_cells(ring, r, 0, width - 1, -1);

        ring.validate_images();
        g_assert_cmpuint(ring.image_map().size(), ==, 1);
        g_assert_cmpint(image->get_left(), ==, left);
        g_assert_cmpuint(image_cell_positions(ring, id).size(), ==, rows_tall * cols_wide);
}

/* A cell that the move pushes off the end of the region is lost exactly as a
 * cell of text there would be, and the rest of the picture still follows. This
 * is what DCH on the image's own first column does.
 */
static void
test_ring_image_clipped_by_horizontal_scroll(void)
{
        auto const top = long{5};
        auto const left = long{0};
        auto const rows_tall = 1;
        auto const cols_wide = 4;
        auto const width = long{40};

        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        auto* const image = place_and_stamp(ring, top, rows_tall, left, width);
        auto const id = image->get_pool_id();

        auto damage_top = long{};
        auto damage_bottom = long{};
        g_assert_true(ring.shift_images_for_scroll(top, top, 0, width - 1, -1,
                                                   &damage_top, &damage_bottom));
        scroll_row_cells(ring, top, 0, width - 1, -1);

        ring.validate_images();

        /* The leftmost tile column is gone; the other three moved left. */
        g_assert_cmpuint(ring.image_map().size(), ==, 1);
        g_assert_cmpint(image->get_left(), ==, left - 1);

        auto const found = image_cell_positions(ring, id);
        g_assert_cmpuint(found.size(), ==, cols_wide - 1);
        for (auto c = 1; c < cols_wide; c++) {
                auto const it = found.find({0u, uint32_t(c)});
                g_assert_true(it != found.end());
                g_assert_cmpint(it->second.first, ==, top);
                g_assert_cmpint(it->second.second, ==, left - 1 + c);
        }
}

/* An image with a cell OUTSIDE the moving region cannot follow: part of it
 * would move and part would stand still, and no single left edge describes
 * that. Those cells are taken, as any other write to them would take them.
 */
static void
test_ring_image_torn_by_horizontal_scroll(void)
{
        auto const top = long{5};
        auto const left = long{2};
        auto const width = long{40};

        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        /* Two rows tall, but the region is only the first of them. */
        auto* const image = place_and_stamp(ring, top, 2, left, width);
        auto const id = image->get_pool_id();
        g_assert_cmpuint(image_cell_positions(ring, id).size(), ==, 8);

        auto damage_top = long{};
        auto damage_bottom = long{};
        g_assert_true(ring.shift_images_for_scroll(top, top, 0, width - 1, 1,
                                                   &damage_top, &damage_bottom));
        scroll_row_cells(ring, top, 0, width - 1, 1);

        ring.validate_images();

        /* The image did not move, and the row inside the region lost its
         * cells rather than being dragged out from under the other one.
         */
        g_assert_cmpint(image->get_left(), ==, left);
        auto const found = image_cell_positions(ring, id);
        g_assert_cmpuint(found.size(), ==, 4);
        for (auto c = 0; c < 4; c++) {
                auto const it = found.find({1u, uint32_t(c)});
                g_assert_true(it != found.end());
                g_assert_cmpint(it->second.first, ==, top + 1);
                g_assert_cmpint(it->second.second, ==, left + c);
        }
}

static void
test_ring_image_cells_stay_with_their_image(void)
{
        /* The other half of anchoring: the cells and the image's rectangle
         * have to keep saying the same thing.
         *
         * find_image_anchor() asks the cells where the picture is, while every
         * lifetime rule - which images an erase reaches, which the scrollback
         * has taken, which a reflow tears apart - asks the rectangle. Let the
         * two drift apart and a picture is drawn in one place and reasoned
         * about in another: it survives an erase that covered it, and dies of
         * one that did not. Ring::validate_images() is where that is stated;
         * this is the state that gives the statement something to say.
         */
        auto const top = long{5};
        auto const left = long{2};
        auto const rows_tall = 3;
        auto const cols_wide = 4;    /* place_image() makes the image this wide. */

        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        for (auto r = top; r < top + rows_tall; r++)
                widen_row(ring, r, left + cols_wide);

        place_image(ring, top, rows_tall, left);
        auto* const image = ring.image_map().begin()->second.get();
        auto const id = image->get_pool_id();

        ring.set_placing_image(image);
        for (auto r = 0; r < rows_tall; r++)
                ring.stamp_image_row(top + r, left, cols_wide, r);
        ring.set_placing_image(nullptr);

        /* The fixture before the assertion it exists for: every tile of a
         * footprint several rows tall and several columns wide is really
         * carried by a cell, and the picture starts at a column that is NOT
         * zero - at column zero a rectangle with the wrong left still agrees
         * with a cell whose tile column is zero, and the check would pass
         * without ever comparing anything.
         */
        auto const expect_footprint = [&](auto const& found,
                                          long first_row) {
                g_assert_cmpuint(found.size(), ==, rows_tall * cols_wide);

                for (auto r = 0; r < rows_tall; r++) {
                        for (auto c = 0; c < cols_wide; c++) {
                                auto const it = found.find({uint32_t(r), uint32_t(c)});
                                g_assert_true(it != found.end());
                                g_assert_cmpint(it->second.first, ==, first_row + r);
                                g_assert_cmpint(it->second.second, ==, left + c);
                        }
                }
        };

        expect_footprint(image_cell_positions(ring, id), top);
        g_assert_cmpint(image->get_top(), ==, top);
        g_assert_cmpint(image->get_left(), ==, left);

        ring.validate_images();

        /* Now push a row in above it. Ring::insert() knows nothing about
         * images: it moves rows, the cells go with them, and the image has to
         * end up describing where they went.
         */
        ring.insert(top, 0);

        expect_footprint(image_cell_positions(ring, id), top + 1);

        ring.validate_images();
        g_assert_cmpint(image->get_top(), ==, top + 1);
        g_assert_cmpint(image->get_left(), ==, left);
}


static void
test_ring_image_reference_survives_freeze(void)
{
        /* A cell must still know it is part of an image after its row has
         * been frozen into the scrollback and thawed back out - and must know
         * WHICH part, since the tile coordinates are what position it.
         *
         * Here the pool entry has no image behind it, so there is nothing
         * to resolve on the way back and the cell correctly comes back
         * naming no image. The sibling test below covers the case where the
         * image IS still resident and must be re-bound.
         */
        auto ring = Ring{1024, true};   /* with streams: freezing is the point */
        ring.set_visible_rows(24);
        append_rows(ring, 4);

        auto const id = ring.image_pool().allocate(nullptr);
        auto* row = ring.index_writable(1);
        g_assert_cmpint(row->len, >, 0);
        row->cells[0].attr.set_image_ref(vte::image::Ref{id, 3, 0});
        g_assert_true(ring.index(1)->cells[0].attr.image());

        /* Push it far out of the writable window, so it is frozen. */
        append_rows(ring, 200);

        /* Reading it back thaws it. */
        auto const* thawed = ring.index(1);
        g_assert_nonnull(thawed);
        g_assert_cmpint(thawed->len, >, 0);

        auto const& attr = thawed->cells[0].attr;

        /* Still an image cell, and still the same piece of the image. */
        g_assert_true(attr.image());

        auto const ref = attr.image_ref();
        g_assert_cmpuint(ref.tile_row(), ==, 3);
        g_assert_cmpuint(ref.tile_col(), ==, 0);

        /* No image was behind this id, so nothing was written down that
         * could resolve it, and it comes back naming nothing.
         *
         * That is the correct outcome and not merely an accident of the
         * fixture: the pool id itself is never what survives, because it
         * indexes an in-memory table whose quarantine only tracks cells in
         * the writable rows. Replaying a stale id could name a different
         * image - exactly the aliasing the pool exists to prevent - so the
         * id is always re-resolved, and resolves to nothing when the image
         * is gone.
         */
        g_assert_cmpuint(ref.pool_id(), ==, vte::image::k_ref_pool_id_none);
        g_assert_false(ref.valid());
        g_assert_null(ring.image_pool().lookup(ref));

        /* And it must not be mistaken for a hyperlink: the union tag decides,
         * and an image cell answers "no hyperlink" in range.
         */
        g_assert_cmpuint(attr.hyperlink_idx_or_none(), ==, 0);
}



static void
test_ring_image_reference_rebinds_after_thaw(void)
{
        /* A row carrying a REAL, still-resident image must come back out of
         * the scrollback naming that same image again.
         *
         * This is the case the sibling test above does not reach: it uses a
         * pool entry with no image behind it, so nothing is written down to
         * resolve and the cell correctly comes back naming nothing. Here the
         * image is alive the whole time, so coming back as "no image" would
         * be a silent loss - a picture that vanishes when you scroll past it
         * and back.
         *
         * What makes it safe is that the stream records the image's PRIORITY,
         * which is allocated from a monotonic counter and never reused, and
         * not its pool id, which is an index that a sweep can recycle. The
         * pool id on the way back out is looked up fresh, so it may legally
         * differ from the one that went in; the IMAGE must not.
         */
        auto ring = Ring{1024, true};   /* with streams: freezing is the point */
        ring.set_visible_rows(24);
        append_rows(ring, 4);

        place_image(ring, 1, 1);
        auto* image = ring.image_map().begin()->second.get();
        auto const id_before = image->get_pool_id();

        ring.set_placing_image(image);
        ring.stamp_image_row(1, 0, 1, 0);
        ring.set_placing_image(nullptr);

        g_assert_true(ring.index(1)->cells[0].attr.image());
        g_assert_true(ring.image_pool().lookup(ring.index(1)->cells[0].attr.image_ref())
                      == image);

        /* Push the row far out of the writable window, so it is frozen, then
         * read it back, which thaws it.
         */
        append_rows(ring, 200);

        auto const* thawed = ring.index(1);
        g_assert_nonnull(thawed);
        g_assert_cmpint(thawed->len, >, 0);

        auto const& attr = thawed->cells[0].attr;
        g_assert_true(attr.image());

        auto const ref = attr.image_ref();

        /* The tile coordinates place it within the image. */
        g_assert_cmpuint(ref.tile_row(), ==, 0);
        g_assert_cmpuint(ref.tile_col(), ==, 0);

        /* And it resolves to THE SAME image object, not to nothing and not
         * to some other image that inherited the id.
         */
        g_assert_true(ref.valid());
        g_assert_true(ring.image_pool().lookup(ref) == image);

        /* The id is re-resolved rather than replayed: whatever it is now, it
         * is the id the image actually holds.
         */
        g_assert_cmpuint(ref.pool_id(), ==, image->get_pool_id());
        (void)id_before;
}


static void
test_ring_image_cells_hold_object_replacement(void)
{
        /* A cell an image covers must not keep the text that was there, and
         * must not be left empty either.
         *
         * chpe's answer to whether a cell may hold both text and an image is
         * that placing an image erases its area and makes the cells contain
         * U+FFFC, not drawn as a character (vte#253). Empty would be wrong in
         * a way that matters: an empty cell is indistinguishable from one the
         * image never covered.
         */
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        /* append_rows() puts real text in every cell. */
        g_assert_cmpuint(ring.index(2)->cells[0].c, ==, 'x');

        place_image(ring, 2, 2);
        auto* image = ring.image_map().begin()->second.get();

        ring.set_placing_image(image);
        for (auto r = 0u; r < 2u; r++)
                ring.stamp_image_row(2 + r, 0, 1, r);
        ring.set_placing_image(nullptr);

        for (auto r = 0u; r < 2u; r++) {
                auto const& cell = ring.index(2 + r)->cells[0];

                g_assert_true(cell.attr.image());
                g_assert_cmpuint(cell.c, ==, VTE_OBJECT_REPLACEMENT_CHARACTER);

                /* Not empty: the ring must be able to tell "image here" from
                 * "nothing here".
                 */
                g_assert_cmpuint(cell.c, !=, 0);
        }

        /* An untouched neighbouring row still holds its text. */
        g_assert_cmpuint(ring.index(5)->cells[0].c, ==, 'x');
        g_assert_false(ring.index(5)->cells[0].attr.image());
}


/* The contract text extraction relies on: EVERY cell an image covers, on every
 * row it covers, holds U+FFFC, and no cell outside its footprint is touched.
 *
 * Terminal::get_text() has no image case at all. It appends each non-fragment
 * cell's own c, which is what makes the clipboard, the search buffer, the regex
 * match buffer and the a11y snapshot all mark an image's position, and mark it
 * the same way (vte#309). That only holds while the cell store keeps this
 * shape, so pin it over a footprint that is several rows tall AND several
 * columns wide: a one-row, one-column fixture would pass with every other
 * covered cell left as text.
 */
static void
test_ring_image_covers_every_cell_it_claims(void)
{
        auto const top = long{5};
        auto const rows_tall = 3;
        auto const left = 2;
        auto const cols_wide = 4;    /* place_image() makes the image this wide. */
        auto const width = left + cols_wide + 3;

        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        for (auto r = top; r < top + rows_tall; r++)
                widen_row(ring, r, width);

        place_image(ring, top, rows_tall, left);
        auto* image = ring.image_map().begin()->second.get();

        ring.set_placing_image(image);
        for (auto r = 0; r < rows_tall; r++)
                ring.stamp_image_row(top + r, left, cols_wide, r);
        ring.set_placing_image(nullptr);
        ring.validate_images();

        /* The fixture before the assertion it is there to support: the stamped
         * footprint really does span several distinct rows and several distinct
         * columns.
         */
        auto rows_seen = std::set<long>{};
        auto cols_seen = std::set<long>{};
        for (auto r = top; r < top + rows_tall; r++) {
                auto const* row = ring.index(r);
                for (auto col = 0; col < row->len; col++) {
                        if (!row->cells[col].attr.image())
                                continue;

                        rows_seen.insert(r);
                        cols_seen.insert(col);
                }
        }
        g_assert_cmpuint(rows_seen.size(), ==, rows_tall);
        g_assert_cmpuint(cols_seen.size(), ==, cols_wide);

        for (auto r = top; r < top + rows_tall; r++) {
                auto const* row = ring.index(r);
                g_assert_cmpint(row->len, ==, width);

                for (auto col = 0; col < row->len; col++) {
                        auto const& cell = row->cells[col];
                        auto const covered = col >= left && col < left + cols_wide;

                        g_assert_cmpint(cell.attr.image(), ==, covered);

                        /* Fragments are skipped by text extraction, so a covered
                         * cell that was one would contribute nothing.
                         */
                        g_assert_false(cell.attr.fragment());

                        g_assert_cmpuint(cell.c, ==,
                                         covered ? VTE_OBJECT_REPLACEMENT_CHARACTER
                                                 : gunichar('x'));
                }
        }

        /* The row just below the footprint still holds its text. */
        auto const* below = ring.index(top + rows_tall);
        g_assert_false(below->cells[0].attr.image());
        g_assert_cmpuint(below->cells[0].c, ==, 'x');
}


static void
test_ring_image_partial_erase_keeps_the_rest(void)
{
        /* Writing over PART of an image must cost only the cells written.
         *
         * This was previously all-or-nothing: any write whose rectangle
         * touched an image's bounding box deleted the whole image, because a
         * whole-image blit could not draw an image missing some of its cells.
         * The draw walks the cells now, so it can.
         */
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        for (auto r = 2; r <= 4; r++)
                widen_row(ring, r, 4);

        place_image(ring, 2, 3);
        auto* image = ring.image_map().begin()->second.get();
        auto const id = image->get_pool_id();

        ring.set_placing_image(image);
        for (auto r = 0u; r < 3u; r++)
                ring.stamp_image_row(2 + r, 0, 4, r);
        ring.set_placing_image(nullptr);

        /* Erase a single cell in the middle row. */
        auto damage_top = long{}, damage_bottom = long{};
        g_assert_true(ring.erase_images_in_rect(3, 3, 1, 1,
                                                &damage_top, &damage_bottom));
        ring.validate_images();

        /* The image is still here. */
        g_assert_cmpuint(ring.image_map().size(), ==, 1);
        g_assert_true(ring.image_pool().lookup(id) == image);

        /* The erased cell no longer names it... */
        g_assert_false(ring.index(3)->cells[1].attr.image());

        /* ...and every other cell still does. */
        g_assert_true(ring.index(3)->cells[0].attr.image());
        g_assert_true(ring.index(3)->cells[2].attr.image());
        g_assert_true(ring.index(2)->cells[0].attr.image());
        g_assert_true(ring.index(4)->cells[0].attr.image());
}

static void
test_ring_image_full_erase_frees_it(void)
{
        /* The other half: once NO cell names the image, it must actually be
         * freed. Keeping it would be retention with nothing able to draw it
         * and nothing able to erase it.
         */
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        for (auto r = 2; r <= 4; r++)
                widen_row(ring, r, 4);

        place_image(ring, 2, 3);
        auto* image = ring.image_map().begin()->second.get();
        auto const id = image->get_pool_id();

        ring.set_placing_image(image);
        for (auto r = 0u; r < 3u; r++)
                ring.stamp_image_row(2 + r, 0, 4, r);
        ring.set_placing_image(nullptr);

        g_assert_cmpuint(ring.image_map().size(), ==, 1);

        /* Erase the whole area the image covers. */
        auto damage_top = long{}, damage_bottom = long{};
        g_assert_true(ring.erase_images_in_rect(2, 4, 0, 79,
                                                &damage_top, &damage_bottom));
        ring.validate_images();

        g_assert_cmpuint(ring.image_map().size(), ==, 0);

        /* The damage covers the rows the image occupied, so the caller
         * repaints all of them and not just the erased rectangle.
         */
        g_assert_cmpint(damage_top, <=, 2);
        g_assert_cmpint(damage_bottom, >=, 4);

        /* Its id resolves to nothing rather than to a recycled image. */
        g_assert_null(ring.image_pool().lookup(id));
}


static void
test_ring_image_pixels_survive_eviction(void)
{
        /* The scrollback half of image support.
         *
         * Until now only the image REFERENCE survived freezing: a row could
         * come back knowing it was part of an image, while the image itself
         * had been evicted, so the cells resolved to nothing and drew as
         * background. The picture silently disappeared from the scrollback.
         *
         * The pixels are now spilled to a fourth stream when the image is
         * evicted, and restored when a row that names it is thawed.
         */
        auto ring = Ring{1024, true};   /* with streams: this is about them */
        ring.set_visible_rows(24);
        append_rows(ring, 4);
        widen_row(ring, 1, 4);

        place_image(ring, 1, 1);
        auto* const image = ring.image_map().begin()->second.get();
        auto const priority = image->get_priority();
        auto const width = image->get_width_px();
        auto const height = image->get_height_px();

        ring.set_placing_image(image);
        ring.stamp_image_row(1, 0, 4, 0);
        ring.set_placing_image(nullptr);

        /* Freeze the row, then evict the image outright - the cells still
         * name it, but the pixels are gone from memory.
         */
        append_rows(ring, 200);
        ring.evict_all_images_for_test();
        ring.validate_images();
        g_assert_cmpuint(ring.image_map().size(), ==, 0);

        /* Thawing the row must bring the image back. */
        auto const* const thawed = ring.index(1);
        g_assert_nonnull(thawed);
        g_assert_cmpint(thawed->len, >, 0);

        auto const& attr = thawed->cells[0].attr;
        g_assert_true(attr.image());

        auto const ref = attr.image_ref();
        g_assert_true(ref.valid());

        auto* const restored = ring.image_pool().lookup(ref);
        g_assert_nonnull(restored);

        /* Same image, and the same picture: identity is the priority, and the
         * geometry has to come back intact or it would draw at the wrong
         * scale.
         */
        g_assert_cmpuint(restored->get_priority(), ==, priority);
        g_assert_cmpint(restored->get_width_px(), ==, width);
        g_assert_cmpint(restored->get_height_px(), ==, height);
        g_assert_nonnull(restored->get_surface());
}

static void
test_ring_image_spill_is_reclaimed(void)
{
        /* A spill is only needed while a row naming it can still be thawed.
         * Once those rows are gone the pixels are unreachable, and keeping
         * them would make this stream a pure leak - the very failure a fourth
         * stream risks introducing.
         */
        auto ring = Ring{64, true};
        ring.set_visible_rows(24);
        append_rows(ring, 4);
        widen_row(ring, 1, 4);

        place_image(ring, 1, 1);
        auto* const image = ring.image_map().begin()->second.get();

        ring.set_placing_image(image);
        ring.stamp_image_row(1, 0, 4, 0);
        ring.set_placing_image(nullptr);

        append_rows(ring, 40);
        ring.evict_all_images_for_test();
        ring.validate_images();
        g_assert_cmpuint(ring.image_spill_count_for_test(), ==, 1);

        /* Push the image's row out of the ring entirely. */
        append_rows(ring, 4096);

        g_assert_cmpuint(ring.image_spill_count_for_test(), ==, 0);
}


static void
test_ring_image_limit_is_enforced(void)
{
        /* The image memory budget is real API now, not a #define. chpe asked
         * for it twice: "some API to set the hard resource limit (like we have
         * the number-of-scrollback-lines API)" (vte#255).
         *
         * Each image here is 4 cells wide by 1 tall at 10x20, so 40x20 px,
         * 3200 bytes. A 20 KiB budget therefore holds a handful and must
         * evict the rest.
         */
        auto ring = Ring{1024, true};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        auto const budget = size_t{20 * 1024};
        ring.set_image_memory_max(budget);
        g_assert_cmpuint(ring.image_memory_max(), ==, budget);

        /* Different rows: images stacked on the same row replace each other,
         * so only one would ever be resident and the budget would never bind.
         */
        for (auto i = 0; i < 100; i++)
                place_image(ring, 2 + (i % 20), 1);

        /* Bounded, and actually holding several - a bound that only ever
         * holds one image is not testing a bound.
         */
        g_assert_cmpuint(ring.image_map().size(), >, 1);
        g_assert_cmpuint(ring.image_memory_used(), <=, budget);

        /* The counter still agrees with what is resident - the budget is only
         * meaningful if the number it is compared against is true.
         */
        ring.validate_images();

        /* The survivors are the NEWEST: eviction takes the oldest first, so
         * what is on screen now outlives what scrolled past.
         */
        auto const newest = ring.image_map().rbegin()->first;
        for (auto const& [priority, image] : ring.image_map())
                g_assert_cmpuint(priority, >, newest - ring.image_map().size());
}

static void
test_ring_image_limit_zero_disables(void)
{
        /* Zero is a meaningful setting, not a degenerate one: it is how a
         * caller turns images off by resource policy rather than by refusing
         * to parse them.
         */
        auto ring = Ring{1024, true};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        ring.set_image_memory_max(0);

        for (auto i = 0; i < 10; i++)
                place_image(ring, 2 + i, 1);

        g_assert_cmpuint(ring.image_map().size(), ==, 0);
        g_assert_cmpuint(ring.image_memory_used(), ==, 0);
}

static void
test_ring_image_limit_shrinks_immediately(void)
{
        /* Lowering the budget must take effect at once, not at the next
         * image: a caller reducing it is reclaiming memory now.
         */
        auto ring = Ring{1024, true};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        for (auto i = 0; i < 20; i++)
                place_image(ring, 2 + i, 1);

        /* Several resident, so lowering the budget has something to reclaim. */
        g_assert_cmpuint(ring.image_map().size(), >, 1);
        g_assert_cmpuint(ring.image_memory_used(), >, 3200);

        ring.set_image_memory_max(3200);
        ring.validate_images();
        g_assert_cmpuint(ring.image_memory_used(), <=, 3200);
}


static void
test_image_footprint_is_font_independent(void)
{
        /* The cell footprint of an image must not depend on the font.
         *
         * This is what closes the zoom bug chpe reported and left open in
         * vte#253: "output some image, increase zoom and the image zooms with
         * it (fine so far); output the same image again, and the new image is
         * smaller than the zoomed one." That happened because the footprint
         * was computed from the font's cell at placement time, so two
         * placements of the SAME file at two zoom levels disagreed.
         *
         * Geometry is expressed in the fixed emulated cell now, so the
         * footprint is a property of the image alone. Verified live as well -
         * the same sixel emitted before and after two zoom steps renders at
         * the same width - but asserted here so it cannot regress silently.
         */
        auto const width_px = 95;
        auto const height_px = 45;

        auto make = [&](int cell_w, int cell_h) {
                auto surface = vte::take_freeable
                        (cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                    width_px, height_px));
                return std::make_unique<vte::image::Image>(std::move(surface),
                                                           1, width_px, height_px,
                                                           0, 0, cell_w, cell_h);
        };

        /* The emulated cell is what geometry is expressed in, whatever the
         * font happens to be.
         */
        auto const image = make(VTE_SIXEL_CELL_WIDTH, VTE_SIXEL_CELL_HEIGHT);

        auto const expect_cols = (width_px + VTE_SIXEL_CELL_WIDTH - 1) / VTE_SIXEL_CELL_WIDTH;
        auto const expect_rows = (height_px + VTE_SIXEL_CELL_HEIGHT - 1) / VTE_SIXEL_CELL_HEIGHT;

        g_assert_cmpint(image->get_width(), ==, expect_cols);
        g_assert_cmpint(image->get_height(), ==, expect_rows);

        /* A partial trailing cell still occupies a whole cell - it has to,
         * because a cell is the smallest thing that can carry a reference.
         */
        g_assert_cmpint(expect_cols * VTE_SIXEL_CELL_WIDTH, >=, width_px);
        g_assert_cmpint(expect_rows * VTE_SIXEL_CELL_HEIGHT, >=, height_px);

        /* Drawing scales into the font's cell, and only there. Two different
         * font sizes give two different pixel sizes for the same unchanged
         * footprint - which is exactly the property the zoom bug violated.
         */
        auto const small = image->get_width_pixels(8);
        auto const large = image->get_width_pixels(16);
        g_assert_cmpfloat(large, >, small);
        g_assert_cmpint(image->get_width(), ==, expect_cols);
}

/* The cached thawed row holds image references, and the pool must count them.
 *
 * index() thaws a frozen row into one cache and then serves that same copy
 * again without re-thawing, so its cells go on naming images by the ids they
 * resolved to at thaw time. If the sweep does not see those cells it frees an
 * id that is still named, the next image allocated takes it, and the cached
 * row draws a picture that is not its own - precisely the reuse the pool
 * exists to prevent, arrived at without a single cell being corrupted.
 */
static void
test_ring_cached_row_holds_its_image_id(void)
{
        auto ring = Ring{1024, true};
        ring.set_visible_rows(24);
        append_rows(ring, 4);

        place_image(ring, 1, 1);
        auto* const image = ring.image_map().begin()->second.get();
        ring.set_placing_image(image);
        ring.stamp_image_row(1, 0, 1, 0);
        ring.set_placing_image(nullptr);

        /* Freeze it into the scrollback. */
        append_rows(ring, 200);

        /* Thaw it into the cached row: this is what the draw loop does. */
        auto const* const thawed = ring.index(1);
        g_assert_nonnull(thawed);
        g_assert_cmpint(thawed->len, >, 0);
        g_assert_true(thawed->cells[0].attr.image());

        auto const ref = thawed->cells[0].attr.image_ref();
        auto const cached_id = ref.pool_id();
        g_assert_cmpuint(cached_id, !=, vte::image::k_ref_pool_id_none);
        g_assert_true(ring.image_pool().lookup(ref) == image);

        /* Memory pressure evicts the image: the id retires, and the cached row
         * is left naming it.
         */
        ring.evict_all_images_for_test();
        ring.validate_images();
        g_assert_cmpuint(ring.image_pool().retired_count(), ==, 1);

        /* A sweep, as append_image() runs on id exhaustion. The cached row is
         * a reference, so the id must not come back to the free list.
         */
        ring.sweep_image_pool_for_test();
        g_assert_cmpuint(ring.image_pool().retired_count(), ==, 1);

        /* So the next image cannot be given that id. */
        place_image(ring, 300, 1);
        auto* const newimg = ring.image_map().rbegin()->second.get();
        g_assert_cmpuint(newimg->get_pool_id(), !=, cached_id);

        /* And the cached row, served again without re-thawing, still resolves
         * to nothing rather than to the new picture.
         */
        auto const* const again = ring.index(1);
        g_assert_nonnull(again);
        g_assert_true(again->cells[0].attr.image());
        g_assert_cmpuint(again->cells[0].attr.image_ref().pool_id(), ==, cached_id);
        g_assert_null(ring.image_pool().lookup(again->cells[0].attr.image_ref()));
}


static void
test_ring_rewrap_with_images(void)
{
        /* Ring::rewrap had NO test at all, which is why a stride bug that
         * corrupts every frozen row after the first image shipped.
         *
         * rewrap walks the attr stream record by record. An image record has a
         * 12 byte tail, so a walker that does not account for it lands 12 bytes
         * short on every subsequent record and reads garbage - in practice a
         * mangled scrollback, and at the stream layer an abort.
         */
        auto ring = Ring{1024, true};
        ring.set_visible_rows(24);
        append_rows(ring, 4);
        widen_row(ring, 1, 4);

        place_image(ring, 1, 1);
        auto* const image = ring.image_map().begin()->second.get();
        ring.set_placing_image(image);
        ring.stamp_image_row(1, 0, 4, 0);
        ring.set_placing_image(nullptr);

        /* Rows after the image, so the walk has to cross the image record. */
        append_rows(ring, 300);

        /* Freeze everything, then rewrap to a different width. This is what a
         * horizontal window resize does.
         */
        ring.rewrap_for_test(40);
        ring.validate_images();

        /* The ring must still be readable and self-consistent afterwards. */
        for (auto r = ring.delta(); r < ring.next(); r++) {
                auto const* row = ring.index(r);
                g_assert_nonnull(row);
                g_assert_cmpint(row->len, >=, 0);
        }

        /* And again at another width, since the second pass reads records the
         * first pass wrote.
         */
        ring.rewrap_for_test(100);
        ring.validate_images();
        for (auto r = ring.delta(); r < ring.next(); r++) {
                g_assert_nonnull(ring.index(r));
        }
}

/* Build what an image emitted in the middle of a wrapped paragraph leaves
 * behind: a paragraph too wide for the reflow below, then the row above the
 * image, then the image itself on row 2, one row tall and four cells wide.
 *
 * @tear_above is the only difference between the two arms - whether that row
 * above is hard wrapped, which is precisely what
 * Terminal::erase_image_rect()'s set_hard_wrapped(top - 1) does when the image
 * is placed.
 *
 * Returns the image's pool id.
 */
static uint32_t
place_image_below_a_boundary(Ring& ring,
                             bool tear_above)
{
        ring.set_visible_rows(24);
        append_rows(ring, 8);

        /* Wider than the width rewrapped to below, so the paragraph splits in
         * two and every row under it shifts down. An image whose top merely
         * stays put would not exercise the re-anchoring at all.
         */
        widen_row(ring, 0, 12);

        widen_row(ring, 1, 4);
        ring.index_writable(1)->attr.soft_wrapped = !tear_above;

        widen_row(ring, 2, 4);
        place_image(ring, 2, 1);
        auto* const image = ring.image_map().rbegin()->second.get();

        ring.set_placing_image(image);
        ring.stamp_image_row(2, 0, 4, 0);
        ring.set_placing_image(nullptr);
        ring.validate_images();

        return image->get_pool_id();
}

static void
test_ring_rewrap_needs_the_boundary_above_torn(void)
{
        /* Placing an image tears the paragraph apart above it, and that tear is
         * what carries the picture through a reflow.
         *
         * An image is a rectangle of PHYSICAL rows, so it survives a rewrap
         * only if each of its rows comes out of the reflow the same way it went
         * in - and a paragraph that continues INTO the image's top row rewrites
         * that row as thoroughly as anything below it does.
         * drop_images_torn_by_rewrap() therefore holds the row above the top to
         * the same rule as the covered rows, and an image that fails it is
         * deleted rather than left claiming rows that now hold something else.
         *
         * So the row above being hard wrapped is not a detail of how the image
         * is drawn: it is the difference between the picture surviving the
         * first window resize and being destroyed by it.
         */
        auto const columns = Ring::column_t{6};

        /* Torn: the state erase_image_rect() leaves behind. */
        {
                auto ring = Ring{1024, true};
                auto const id = place_image_below_a_boundary(ring, true);

                /* The fixture really is in the state the assertions describe:
                 * the boundary is torn, and the cells carry the picture.
                 */
                g_assert_false(ring.is_soft_wrapped(1));
                auto const before = image_cell_positions(ring, id);
                g_assert_cmpuint(before.size(), ==, 4);
                for (auto c = 0u; c < 4u; c++)
                        g_assert_true((before.at({0, c}) == std::pair<long, long>{2, c}));

                ring.rewrap_for_test(columns);
                ring.validate_images();

                /* The reflow really did split the paragraph above, so the rows
                 * under it moved and the image had to be re-anchored rather
                 * than left where it was.
                 */
                g_assert_cmpint(long(ring.next() - ring.delta()), ==, 9);

                g_assert_cmpuint(ring.image_map().size(), ==, 1);
                auto const* const image = ring.image_map().begin()->second.get();
                g_assert_cmpint(long(image->get_top()), ==, 3);

                auto const after = image_cell_positions(ring, id);
                g_assert_cmpuint(after.size(), ==, 4);
                for (auto c = 0u; c < 4u; c++)
                        g_assert_true((after.at({0, c}) == std::pair<long, long>{3, c}));
        }

        /* Glued: the state without the tear, which the terminal produces
         * whenever an image is emitted part way through a wrapped paragraph.
         */
        {
                auto ring = Ring{1024, true};
                auto const id = place_image_below_a_boundary(ring, false);

                g_assert_true(ring.is_soft_wrapped(1));
                g_assert_cmpuint(image_cell_positions(ring, id).size(), ==, 4);

                ring.rewrap_for_test(columns);
                ring.validate_images();

                /* The image is gone, and its memory with it. */
                g_assert_cmpuint(ring.image_map().size(), ==, 0);
                g_assert_cmpuint(ring.image_memory_used(), ==, 0);
                g_assert_false(ring.has_images());

                /* And gone for a reason, not by accident: the paragraph above
                 * really did reflow across the boundary, so that the row the
                 * image's stripe used to have to itself now begins with that
                 * paragraph's text. Keeping the image would have drawn it
                 * under text that is not where it was emitted.
                 */
                auto const* const row = ring.index(2);
                g_assert_nonnull(row);
                g_assert_cmpint(row->len, ==, long(columns));
                for (auto c = 0; c < 4; c++) {
                        g_assert_false(row->cells[c].attr.image());
                        g_assert_cmpuint(row->cells[c].c, ==, 'x');
                }
                for (auto c = 4; c < long(columns); c++)
                        g_assert_true(row->cells[c].attr.image());
        }
}


static void
test_ring_scrollback_restore_respects_the_budget(void)
{
        /* Scrolling back through history that held images must not blow the
         * memory budget, and must still show the pictures.
         *
         * Faulting an image in from the scrollback ADDS to the accounting, so
         * it has to be collected against like any other addition. It was not:
         * indexing rows read-only, which is all Page Up does, pulled every
         * image back into RAM and nothing evicted them. Measured at 8.3 times
         * the configured budget before the fix.
         */
        auto const image_count = 12;

        auto ring = Ring{1024, true};
        ring.set_visible_rows(24);

        /* Enough rows for every image to have one to itself. A row that does
         * not exist yet cannot be widened and cannot be stamped, so a ring too
         * short here leaves most of the images anchored to nothing.
         */
        append_rows(ring, 2 + image_count + 2);

        /* One image per row, on distinct rows so they coexist. */
        auto row_of = std::map<size_t, long>{};
        for (auto i = 0; i < image_count; i++) {
                auto const row = long(2 + i);

                widen_row(ring, row, 4);
                place_image(ring, row, 1);
                auto* const img = ring.image_map().rbegin()->second.get();
                ring.set_placing_image(img);
                ring.stamp_image_row(row, 0, 4, 0);
                ring.set_placing_image(nullptr);

                row_of[img->get_priority()] = row;
        }

        /* All of them resident at once, which is what a row each buys. Stacked
         * on one row they would replace each other, only ever one would be
         * resident, and a budget nothing approaches bounds nothing.
         */
        g_assert_cmpuint(ring.image_map().size(), ==, size_t(image_count));

        /* Freeze them all, then set a budget that only a couple can fit. */
        append_rows(ring, 300);
        g_assert_cmpint(long(ring.writable_start_for_test()), >, long(2 + image_count));

        auto const budget = size_t{9600};
        ring.set_image_memory_max(budget);
        ring.validate_images();
        g_assert_cmpuint(ring.image_memory_used(), <=, budget);

        /* Most of them have left RAM for the spill, so the rows read below
         * really are rows whose picture is no longer in memory.
         */
        auto evicted = std::set<size_t>{};
        for (auto const& [priority, row] : row_of) {
                if (ring.image_map().find(priority) == ring.image_map().end())
                        evicted.insert(priority);
        }
        g_assert_cmpuint(ring.image_map().size(), >, 0);
        g_assert_cmpuint(evicted.size(), >, 0);
        g_assert_cmpuint(ring.image_spill_count_for_test(), ==, evicted.size());

        /* One of them on its own first. Reading the row it covers has to put
         * the image back in the map and back on the accounting: a bound on the
         * memory says nothing by itself, since faulting nothing in at all
         * satisfies any bound.
         */
        auto const target = *evicted.begin();
        g_assert_nonnull(ring.index(row_of[target]));

        auto const restored = ring.image_map().find(target);
        g_assert_true(restored != ring.image_map().end());
        g_assert_cmpuint(ring.image_memory_used(), >=, restored->second->resource_size());

        /* Now scroll back over all of them, read only. */
        auto faulted_in = std::set<size_t>{};
        for (auto const& [priority, row] : row_of) {
                auto const* const data = ring.index(row);
                g_assert_nonnull(data);
                g_assert_cmpint(data->len, >, 0);

                /* The row still has its picture, and it is its own: the cell
                 * names an image, and that image is here to be drawn.
                 */
                auto const& cell = data->cells[0];
                g_assert_true(cell.attr.image());

                auto const* const image = ring.image_pool().lookup(cell.attr.image_ref());
                g_assert_nonnull(image);
                g_assert_cmpuint(image->get_priority(), ==, priority);

                if (evicted.count(priority))
                        faulted_in.insert(priority);

                /* And the addition is collected against as it is made, not at
                 * some later placement that may never come.
                 */
                g_assert_cmpuint(ring.image_memory_used(), <=, budget);
        }

        /* Every one that had been evicted came back. */
        g_assert_cmpuint(faulted_in.size(), ==, evicted.size());
        g_assert_cmpuint(ring.image_memory_used(), <=, budget);

        /* And the accounting still matches what is actually resident, so the
         * bound is a real one rather than a stale counter.
         */
        ring.validate_images();
}

int
main(int argc,
     char* argv[])
{
        g_test_init(&argc, &argv, nullptr);

#if WITH_SIXEL
        g_test_add_func("/vte/sixel/right-margin-clip", test_sixel_right_margin_clip);
        g_test_add_func("/vte/image/ref/roundtrip", test_image_ref_roundtrip);
        g_test_add_func("/vte/image/footprint-is-font-independent", test_image_footprint_is_font_independent);
        g_test_add_func("/vte/image/ref/covers-max-legal-image", test_image_ref_covers_max_legal_image);
        g_test_add_func("/vte/image/ref/out-of-range-cannot-alias", test_image_ref_out_of_range_cannot_alias);
        g_test_add_func("/vte/image/ref/fields-do-not-alias", test_image_ref_fields_do_not_alias);
        g_test_add_func("/vte/image/ref/zero-is-not-an-image", test_image_ref_zero_is_not_an_image);
        g_test_add_func("/vte/image/ref/stripe-identity", test_image_ref_stripe_identity);
        g_test_add_func("/vte/cell/attr/union-tagging", test_cell_attr_union_tagging);
        g_test_add_func("/vte/cell/attr/image-tag-survives-sgr-reset", test_cell_attr_image_tag_survives_sgr_reset);
        g_test_add_func("/vte/cell/sizes-unchanged", test_cell_sizes_unchanged);

        g_test_add_func("/vte/image/pool/allocate-lookup", test_image_pool_allocate_lookup);
        g_test_add_func("/vte/image/pool/retire-resolves-to-null", test_image_pool_retire_resolves_to_null);
        g_test_add_func("/vte/image/pool/no-reuse-before-sweep", test_image_pool_no_reuse_before_sweep);
        g_test_add_func("/vte/image/pool/sweep-frees-unreferenced", test_image_pool_sweep_frees_unreferenced);
        g_test_add_func("/vte/image/pool/sweep-keeps-referenced", test_image_pool_sweep_keeps_referenced);
        g_test_add_func("/vte/image/pool/sweep-does-not-touch-live", test_image_pool_sweep_does_not_touch_live);
        g_test_add_func("/vte/image/pool/sweep-end-without-begin", test_image_pool_sweep_end_without_begin);
        g_test_add_func("/vte/image/pool/exhaustion", test_image_pool_exhaustion);
        g_test_add_func("/vte/image/pool/retire-is-idempotent", test_image_pool_retire_is_idempotent);

        g_test_add_func("/vte/ring/attr-stream/rle-trap", test_attr_stream_rle_trap);
        g_test_add_func("/vte/ring/attr-stream/stripe-is-one-run", test_attr_stream_stripe_is_one_run);

        g_test_add_func("/vte/ring/image-pool/allocates", test_ring_image_pool_allocates);
        g_test_add_func("/vte/ring/image-pool/retires-with-the-image", test_ring_image_pool_retires_with_the_image);
        g_test_add_func("/vte/ring/image-pool/sweep-reclaims", test_ring_image_pool_sweep_reclaims);

        g_test_add_func("/vte/ring/image-pool/cells-hold-object-replacement", test_ring_image_cells_hold_object_replacement);
        g_test_add_func("/vte/ring/image-pool/covers-every-cell-it-claims", test_ring_image_covers_every_cell_it_claims);
        g_test_add_func("/vte/ring/image-pool/cells-carry-the-reference", test_ring_image_cells_carry_the_reference);
        g_test_add_func("/vte/ring/image-pool/sweep-sees-cell-references", test_ring_image_sweep_sees_cell_references);

        g_test_add_func("/vte/ring/image-pool/anchor-follows-the-cells", test_ring_image_anchor_follows_the_cells);
        g_test_add_func("/vte/ring/image-pool/cells-stay-with-their-image", test_ring_image_cells_stay_with_their_image);
        g_test_add_func("/vte/ring/image-pool/follows-horizontal-scroll", test_ring_image_follows_horizontal_scroll);
        g_test_add_func("/vte/ring/image-pool/clipped-by-horizontal-scroll", test_ring_image_clipped_by_horizontal_scroll);
        g_test_add_func("/vte/ring/image-pool/torn-by-horizontal-scroll", test_ring_image_torn_by_horizontal_scroll);

        g_test_add_func("/vte/ring/image-pool/reference-survives-freeze", test_ring_image_reference_survives_freeze);
        g_test_add_func("/vte/ring/image-pool/reference-rebinds-after-thaw", test_ring_image_reference_rebinds_after_thaw);

        g_test_add_func("/vte/ring/image/partial-erase-keeps-the-rest", test_ring_image_partial_erase_keeps_the_rest);
        g_test_add_func("/vte/ring/image/full-erase-frees-it", test_ring_image_full_erase_frees_it);

        g_test_add_func("/vte/ring/image/pixels-survive-eviction", test_ring_image_pixels_survive_eviction);
        g_test_add_func("/vte/ring/image/spill-is-reclaimed", test_ring_image_spill_is_reclaimed);

        g_test_add_func("/vte/ring/image/limit-is-enforced", test_ring_image_limit_is_enforced);
        g_test_add_func("/vte/ring/image/limit-zero-disables", test_ring_image_limit_zero_disables);
        g_test_add_func("/vte/ring/image/limit-shrinks-immediately", test_ring_image_limit_shrinks_immediately);

        g_test_add_func("/vte/ring/scrollback-restore-respects-the-budget", test_ring_scrollback_restore_respects_the_budget);
        g_test_add_func("/vte/ring/cached-row-holds-its-image-id", test_ring_cached_row_holds_its_image_id);
        g_test_add_func("/vte/ring/rewrap-with-images", test_ring_rewrap_with_images);
        g_test_add_func("/vte/ring/rewrap-needs-the-boundary-above-torn",
                        test_ring_rewrap_needs_the_boundary_above_torn);

        g_test_add_func("/vte/ring/image/resize-drops", test_ring_image_resize_drops);
        g_test_add_func("/vte/ring/image/resize-keeps-straddling", test_ring_image_resize_keeps_straddling);
        g_test_add_func("/vte/ring/image/resize-grow", test_ring_image_resize_grow);
        g_test_add_func("/vte/ring/image/scrollback-shrink", test_ring_image_scrollback_shrink);
        g_test_add_func("/vte/ring/image/shrink-drops-below", test_ring_image_shrink_drops_below);
        g_test_add_func("/vte/ring/image/discard-drops", test_ring_image_discard_drops);
        g_test_add_func("/vte/ring/image/drop-scrollback", test_ring_image_drop_scrollback);
#endif

        return g_test_run();
}
