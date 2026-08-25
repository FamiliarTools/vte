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
#include "cell.hh"
#include "image-ref.hh"
#include "image-pool.hh"

#if WITH_SIXEL

#include <cairo.h>

#include "cairo-glue.hh"

using namespace vte::base;

static int const kCellWidth = 10;
static int const kCellHeight = 20;

/* The invariant the maps are supposed to keep, checked from the outside.
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
 */
static void
assert_image_invariants(Ring const& ring,
                        char const* where)
{
        auto sum = size_t{0};

        for (auto const& [priority, image] : ring.image_map()) {
                if (long(image->get_bottom()) < long(ring.delta())) {
                        g_error("%s: image at rows %ld..%ld is resident but the "
                                "ring starts at row %lu - every row it covers is gone",
                                where,
                                long(image->get_top()),
                                long(image->get_bottom()),
                                ring.delta());
                }

                sum += image->resource_size();
        }

        if (sum != ring.image_memory_used()) {
                g_error("%s: image memory accounted %lu, resident images hold %lu",
                        where,
                        (unsigned long)ring.image_memory_used(),
                        (unsigned long)sum);
        }
}

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
}

/* Place an image @rows_tall rows tall with its top at ring row @top, then end
 * its emission burst the way the sixel path does.
 */
static void
place_image(Ring& ring,
            long top,
            int rows_tall)
{
        auto const width_px = 4 * kCellWidth;
        auto const height_px = rows_tall * kCellHeight;
        auto surface = vte::take_freeable
                (cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width_px, height_px));

        ring.append_image(std::move(surface),
                          width_px, height_px,
                          0, top,
                          kCellWidth, kCellHeight);
        ring.set_placing_image(nullptr);
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
        assert_image_invariants(ring, "after placing");

        /* The window is made shorter: 24 rows down to 12. Rows 0..11 go. */
        ring.resize(12);
        g_assert_cmpuint(ring.delta(), ==, 12);

        assert_image_invariants(ring, "after resize");
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

        assert_image_invariants(ring, "after resize");
        g_assert_cmpuint(ring.image_map().size(), ==, 1);
        g_assert_cmpuint(ring.image_memory_used(), ==, used);

        /* And it goes once its last row follows. */
        ring.resize(9);
        g_assert_cmpuint(ring.delta(), ==, 15);
        assert_image_invariants(ring, "after second resize");
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

        assert_image_invariants(ring, "after grow");
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

        assert_image_invariants(ring, "after scrollback shrink");
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

        assert_image_invariants(ring, "after shrink");
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

        assert_image_invariants(ring, "after discards");
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

        assert_image_invariants(ring, "after drop_scrollback");
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

        /* Constant, not proportional to the 500 columns. */
        g_assert_cmpuint(stripe, <=, 2 * uniform);
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
         * The k_min_cell_* floor the static asserts are written against is
         * not enforced anywhere, so the packing must be total on its own.
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

int
main(int argc,
     char* argv[])
{
        g_test_init(&argc, &argv, nullptr);

#if WITH_SIXEL
        g_test_add_func("/vte/image/ref/roundtrip", test_image_ref_roundtrip);
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
