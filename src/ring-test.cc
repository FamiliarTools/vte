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
#include <vector>

#include "cell.hh"
#include "image-ref.hh"
#include "image-pool.hh"

/* The image coordinate spaces, spelled out. Each is its own type, so these
 * tests have to name the space they mean too, and a transposed pair of them
 * does not build.
 */
using pool_id_t = vte::image::pool_id_t;
using tile_row_t = vte::image::tile_row_t;
using tile_col_t = vte::image::tile_col_t;

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
 * (d) Naming. The pool calls an id live for exactly the images the ring holds,
 *     and each of those ids resolves back to its own image. The pool does not
 *     own the images, so a live id whose image is gone reads freed memory, and
 *     no sweep can ever take it back.
 * (e) Anchoring. Every cell that names an image holds U+FFFC as one whole cell,
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
 * corner at ring row @top, column @left: append it, anchor it to the cells it
 * covers, and end its emission burst, the way the sixel path does.
 *
 * The stamping is part of placing rather than left to the caller, because the
 * image's rectangle and the cells that carry it are two halves of one fact and
 * an image with neither half is not a state the terminal can produce - it would
 * be a picture nothing draws, nothing moves and nothing can erase. A fixture
 * that reaches only the first half never reaches the state the assertions are
 * about.
 *
 * Only cells that already exist are stamped, which is also what the real path's
 * stamp does over a row too short to carry the whole stripe: widen the rows
 * first if the test needs the picture wider than one cell.
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

        /* append_image() left the new image marked as the one being placed,
         * which is what stamp_image_row() writes the reference of.
         */
        for (auto r = 0; r < rows_tall; r++)
                ring.stamp_image_row(vte::grid::coords(top + r, left),
                                     4,
                                     tile_row_t(uint32_t(r)));

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

/* Why an Image carries a rectangle when the cells already say where the picture
 * is.
 *
 * The rectangle is not a second opinion about the cells. It is the ring's index
 * of which ROWS an image occupies, and it cannot be derived from the cells
 * because the ring's rows are not all cells: at m_writable a row stops being
 * memory and becomes bytes in the streams. The rules that decide when an
 * image's last row has left the ring have to be exact for exactly those rows.
 *
 * So freeze every row an image covers and ask both sides. find_image_anchor(),
 * which is the cell-derived position, has nothing left to answer with, while
 * the rectangle still has to place the image to the row - and does, one row
 * either side of the drop.
 */
static void
test_ring_image_rectangle_answers_for_frozen_rows(void)
{
        auto ring = Ring{40, true};     /* with streams: freezing is the point */
        ring.set_visible_rows(24);
        append_rows(ring, 4);

        place_image(ring, 1, 3);
        g_assert_cmpuint(ring.image_map().size(), ==, 1);

        auto const* const image = ring.image_map().begin()->second.get();
        auto const id = image->get_pool_id();
        auto const bottom = long(image->get_bottom());
        g_assert_cmpint(long(image->get_top()), ==, 1);
        g_assert_cmpint(bottom, ==, 3);

        /* The fixture: fill the ring, so that the image's rows are frozen and
         * every cell naming it is in the streams rather than in memory. The
         * ring is not full yet, so no row has been dropped.
         */
        append_rows(ring, 36);
        g_assert_cmpuint(ring.delta(), ==, 0);
        g_assert_cmpuint(ring.image_map().size(), ==, 1);
        g_assert_cmpint(long(ring.writable_start_for_test()), >, bottom);

        /* Which is the state the whole question turns on: the cells can no
         * longer say where the image is. Both halves of the cell-side
         * invariant hold vacuously here, so neither of them is what keeps the
         * image placed.
         */
        g_assert_false(ring.find_image_anchor(id).has_value());
        g_assert_true(ring.image_cells_are_anchored());
        g_assert_null(ring.image_invariant_violation());

        /* And the rows are real rows the user can still scroll back to, each
         * naming the image, so this is a picture that has to be kept and not a
         * leak that has to be collected.
         */
        for (auto r = 1; r <= 3; r++) {
                auto const* const frozen = ring.index(Ring::row_t(r));
                g_assert_nonnull(frozen);
                g_assert_cmpint(frozen->len, >, 0);
                g_assert_true(frozen->cells[0].attr.image());
                g_assert_true(ring.image_pool().lookup(frozen->cells[0].attr.image_ref()) == image);
        }

        /* The behaviour: the rectangle decides, and it decides to the row. The
         * ring is full now, so each further append drops one row off the front.
         * While the image's LAST row is still in the ring, the image stays.
         */
        while (long(ring.delta()) < bottom) {
                append_rows(ring, 1);
                ring.validate_images();
                g_assert_cmpuint(ring.image_map().size(), ==, 1);
        }
        g_assert_cmpuint(ring.delta(), ==, Ring::row_t(bottom));

        /* One more row leaves, taking the last row the image covers, and the
         * image goes with it, budget included.
         */
        append_rows(ring, 1);
        g_assert_cmpuint(ring.delta(), ==, Ring::row_t(bottom + 1));

        ring.validate_images();
        g_assert_cmpuint(ring.image_map().size(), ==, 0);
        g_assert_cmpuint(ring.image_memory_used(), ==, 0);
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
                auto const ref = vte::image::Ref{pool_id_t{c.id},
                                                 tile_row_t{c.row},
                                                 tile_col_t{c.col}};
                g_assert_cmpuint(ref.pool_id().value(), ==, c.id);
                g_assert_cmpuint(ref.tile_row().value(), ==, c.row);
                g_assert_cmpuint(ref.tile_col().value(), ==, c.col);
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
        auto const id_only = vte::image::Ref{pool_id_t{vte::image::k_ref_pool_id_max},
                                             tile_row_t{0}, tile_col_t{0}};
        auto const row_only = vte::image::Ref{pool_id_t{0},
                                              tile_row_t{vte::image::k_ref_tile_row_max},
                                              tile_col_t{0}};
        auto const col_only = vte::image::Ref{pool_id_t{0}, tile_row_t{0},
                                              tile_col_t{vte::image::k_ref_tile_col_max}};

        g_assert_cmpuint(id_only.bits() & row_only.bits(), ==, 0);
        g_assert_cmpuint(id_only.bits() & col_only.bits(), ==, 0);
        g_assert_cmpuint(row_only.bits() & col_only.bits(), ==, 0);

        /* Together they account for all 32 bits. */
        g_assert_cmpuint(id_only.bits() | row_only.bits() | col_only.bits(),
                         ==, 0xffffffffu);

        /* A maximal coordinate leaves the pool id alone. */
        auto const maxed = vte::image::Ref{pool_id_t{7},
                                           tile_row_t{vte::image::k_ref_tile_row_max},
                                           tile_col_t{vte::image::k_ref_tile_col_max}};
        g_assert_cmpuint(maxed.pool_id().value(), ==, 7);
}

static void
test_image_ref_zero_is_not_an_image(void)
{
        /* A zeroed Ref must not name a live image: cells are memset to zero
         * in places, and that must not conjure a reference to image 0.
         */
        g_assert_false(vte::image::Ref{}.valid());
        g_assert_false(vte::image::Ref{0u}.valid());
        auto const no_image = vte::image::Ref{vte::image::k_ref_pool_id_none,
                                             tile_row_t{5}, tile_col_t{5}};
        g_assert_false(no_image.valid());

        /* basic_cell is not an image cell. */
        g_assert_false(basic_cell.attr.image());
}

static void
test_image_ref_stripe_identity(void)
{
        auto const a = vte::image::Ref{pool_id_t{42}, tile_row_t{3}, tile_col_t{0}};
        auto const b = vte::image::Ref{pool_id_t{42}, tile_row_t{3}, tile_col_t{100}};
        auto const c = vte::image::Ref{pool_id_t{42}, tile_row_t{4}, tile_col_t{0}};
        auto const d = vte::image::Ref{pool_id_t{43}, tile_row_t{3}, tile_col_t{0}};

        /* Same image, same tile row: one stripe, the unit of lifetime. */
        g_assert_true(a.same_stripe(b));
        g_assert_true(a.same_image(c));
        g_assert_false(a.same_stripe(c));   /* different tile row */
        g_assert_false(a.same_image(d));
        g_assert_false(a.same_stripe(d));
}

/* Whether a Ref can be built from these argument types, in this order, and
 * whether the ring's one stamping call can be made with them. Both are asked
 * of the type system alone: nothing is constructed or called.
 */
template<typename... Args>
concept ref_buildable_from = requires (Args... args) {
        vte::image::Ref{args...};
};

template<typename... Args>
concept stamp_callable_with = requires (Ring& ring, Args... args) {
        ring.stamp_image_row(args...);
};

static void
test_image_coordinate_spaces_are_distinct(void)
{
        /* An image reference mixes three spaces - which picture, and which
         * row and column OF that picture - and the ring's stamping call adds
         * two more, a ring row and a screen column. They are all small
         * numbers, so a transposition of any two is a well-formed value that
         * draws the wrong tile, or the right tile in the wrong place.
         *
         * These assertions ask the compiler to refuse the wrong orders.
         *
         * The fixture first: the well-formed order IS accepted, so a refusal
         * below is about the order and not about the expression being
         * malformed for some other reason.
         */
        g_assert_true((ref_buildable_from<pool_id_t, tile_row_t, tile_col_t>));

        /* Every other order of the same three values is refused. */
        g_assert_false((ref_buildable_from<tile_row_t, pool_id_t, tile_col_t>));
        g_assert_false((ref_buildable_from<pool_id_t, tile_col_t, tile_row_t>));
        g_assert_false((ref_buildable_from<tile_col_t, tile_row_t, pool_id_t>));
        g_assert_false((ref_buildable_from<tile_row_t, tile_col_t, pool_id_t>));
        g_assert_false((ref_buildable_from<tile_col_t, pool_id_t, tile_row_t>));

        /* And so is the shape all of them used to have: three bare numbers,
         * which every order above satisfies equally well.
         */
        g_assert_false((ref_buildable_from<uint32_t, uint32_t, uint32_t>));

        /* The raw 32-bit form stays: it is how the bits stored in a cell and
         * in the attr stream become a Ref again, and one number cannot be
         * mistaken for a coordinate.
         */
        g_assert_true((ref_buildable_from<uint32_t>));

        /* The same question of the call that mixes a ring row, a screen
         * column, a count of columns and a tile row.
         */
        g_assert_true((stamp_callable_with<vte::grid::coords,
                                           Ring::column_t,
                                           tile_row_t>));

        g_assert_false((stamp_callable_with<vte::grid::coords,
                                            tile_row_t,
                                            Ring::column_t>));
        g_assert_false((stamp_callable_with<tile_row_t,
                                            Ring::column_t,
                                            vte::grid::coords>));

        /* Including the shape it used to have, where the position, the count
         * and the tile row were four numbers in a row.
         */
        g_assert_false((stamp_callable_with<Ring::row_t,
                                            Ring::column_t,
                                            Ring::column_t,
                                            uint32_t>));
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
        auto const ref = vte::image::Ref{pool_id_t{99}, tile_row_t{2}, tile_col_t{3}};
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
        auto const ref = vte::image::Ref{pool_id_t{1234}, tile_row_t{5}, tile_col_t{6}};
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
        g_assert_cmpuint(ida.value(), !=, vte::image::k_ref_pool_id_none.value());
        g_assert_cmpuint(idb.value(), !=, vte::image::k_ref_pool_id_none.value());
        g_assert_cmpuint(ida.value(), !=, idb.value());

        g_assert_true(pool.lookup(ida) == &a);
        g_assert_true(pool.lookup(idb) == &b);
        g_assert_cmpuint(pool.live_count(), ==, 2);

        /* Resolvable through a Ref, which is how the draw path will do it. */
        auto const ref = vte::image::Ref{ida, tile_row_t{0}, tile_col_t{0}};
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
                g_assert_cmpuint(id.value(), !=, ida.value());
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
        g_assert_cmpuint(idb.value(), ==, ida.value());
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
                g_assert_cmpuint(id.value(), !=, ida.value());
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

        std::vector<pool_id_t> ids;
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
                auto const ref = vte::image::Ref{id, tile_row_t{0}, tile_col_t{0}};
                g_assert_cmpuint(ref.pool_id().value(), ==, id.value());
        }

        /* Still exhausted while everything is live. */
        g_assert_cmpuint(pool.allocate(&a).value(), ==, vte::image::k_ref_pool_id_none.value());

        /* Retiring alone does not help; only a completed sweep does. */
        pool.retire(ids[0]);
        g_assert_cmpuint(pool.allocate(&a).value(), ==, vte::image::k_ref_pool_id_none.value());

        pool.sweep_begin();
        g_assert_cmpuint(pool.sweep_end(), ==, 1);
        g_assert_cmpuint(pool.allocate(&a).value(), ==, ids[0].value());
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
        g_assert_cmpuint(id1.value(), ==, ida.value());
        g_assert_cmpuint(id2.value(), !=, id1.value());
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
                        cell.attr.set_image_ref(vte::image::Ref{pool_id_t{1},
                                                                tile_row_t{0},
                                                                tile_col_t{uint32_t(i)}});
                        break;
                case LinkPattern::DistinctImage:
                        /* Genuinely different runs: a separate image per cell.
                         * These MUST each cost a record.
                         */
                        cell.attr.set_image_ref(vte::image::Ref{pool_id_t{uint32_t(i) + 1},
                                                                tile_row_t{0},
                                                                tile_col_t{0}});
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
         * The layout cell's floor (VTE_SIXEL_CELL_MIN_*) is what keeps a legal
         * image inside these fields; the packing is total anyway, so that a
         * bug elsewhere degrades to the wrong tile rather than the wrong
         * image.
         */
        auto const overflow_col = vte::image::Ref{pool_id_t{7}, tile_row_t{0},
                                                  tile_col_t{vte::image::k_ref_tile_col_max + 1}};
        g_assert_cmpuint(overflow_col.pool_id().value(), ==, 7);
        g_assert_cmpuint(overflow_col.tile_row().value(), ==, 0);

        auto const overflow_row = vte::image::Ref{pool_id_t{7},
                                                  tile_row_t{vte::image::k_ref_tile_row_max + 1},
                                                  tile_col_t{0}};
        g_assert_cmpuint(overflow_row.pool_id().value(), ==, 7);

        /* And the caller must be able to ASK, rather than find out by
         * corruption, whether a placement fits at all.
         */
        g_assert_true(vte::image::Ref::fits(pool_id_t{7}, tile_row_t{0},
                                            tile_col_t{vte::image::k_ref_tile_col_max}));
        g_assert_false(vte::image::Ref::fits(pool_id_t{7}, tile_row_t{0},
                                             tile_col_t{vte::image::k_ref_tile_col_max + 1}));
        g_assert_false(vte::image::Ref::fits(pool_id_t{7},
                                             tile_row_t{vte::image::k_ref_tile_row_max + 1},
                                             tile_col_t{0}));
        g_assert_false(vte::image::Ref::fits(pool_id_t{vte::image::k_ref_pool_id_max + 1},
                                             tile_row_t{0}, tile_col_t{0}));
}


static void
test_image_ref_covers_max_legal_image(void)
{
        /* What the layout cell's floor buys: the largest image the parser will
         * admit must fit the tile fields, as a property of the constants
         * rather than a hope about what font the user picked.
         */
        g_assert_cmpint(vte::image::k_max_image_tile_cols, <=,
                        int(vte::image::k_ref_tile_col_max) + 1);
        g_assert_cmpint(vte::image::k_max_image_tile_rows, <=,
                        int(vte::image::k_ref_tile_row_max) + 1);

        /* And the extreme corner really does round-trip. */
        auto const corner = vte::image::Ref{pool_id_t{vte::image::k_ref_pool_id_max},
                                            tile_row_t{uint32_t(vte::image::k_max_image_tile_rows - 1)},
                                            tile_col_t{uint32_t(vte::image::k_max_image_tile_cols - 1)}};
        g_assert_true(vte::image::Ref::fits(pool_id_t{vte::image::k_ref_pool_id_max},
                                            tile_row_t{uint32_t(vte::image::k_max_image_tile_rows - 1)},
                                            tile_col_t{uint32_t(vte::image::k_max_image_tile_cols - 1)}));
        g_assert_cmpuint(corner.tile_col().value(), ==, uint32_t(vte::image::k_max_image_tile_cols - 1));
        g_assert_cmpuint(corner.tile_row().value(), ==, uint32_t(vte::image::k_max_image_tile_rows - 1));
        g_assert_cmpuint(corner.pool_id().value(), ==, vte::image::k_ref_pool_id_max);
}


static void
test_sixel_right_margin_clip(void)
{
        /* A round layout cell, so that the expectations below are legible;
         * the function takes it as an argument and nothing here depends on
         * which font would have produced it.
         */
        auto const cell = 10L;
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
                g_assert_cmpuint(id.value(), !=, vte::image::k_ref_pool_id_none.value());
                g_assert_true(ring.image_pool().lookup(id) == image.get());
                g_assert_false(seen.contains(id.value()));
                seen.insert(id.value());
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
        g_assert_cmpuint(id.value(), !=, vte::image::k_ref_pool_id_none.value());

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
        g_assert_true(ring.image_pool().lookup(id) != nullptr);

        ring.resize(2);
        ring.validate_images();
        g_assert_cmpuint(ring.image_pool().retired_count(), ==, 1);

        /* Its rows are gone, so no cell names it and a sweep reclaims its id.
         * The sweep must not touch anything else.
         */
        ring.sweep_image_pool_for_test();
        g_assert_cmpuint(ring.image_pool().retired_count(), ==, 0);
        g_assert_true(ring.image_pool().lookup(id) == nullptr);

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
test_ring_image_reset_retires_every_id(void)
{
        /* Ring::reset() destroys every image at once, and the pool does not
         * own them: an id it still calls live afterwards resolves to a
         * destroyed vte::image::Image, which is what any cell naming it, and
         * the ring's own walks, would then read.
         */
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);
        append_rows(ring, 24);

        place_image(ring, 2, 3);
        place_image(ring, 8, 3);

        /* The fixture: two resident images, each with an id that resolves to
         * it. That is the state reset() has to take apart.
         */
        g_assert_cmpuint(ring.image_map().size(), ==, 2);
        g_assert_cmpuint(ring.image_pool().live_count(), ==, 2);

        auto ids = std::vector<pool_id_t>{};
        for (auto const& [priority, image] : ring.image_map()) {
                auto const id = image->get_pool_id();
                g_assert_cmpuint(id.value(), !=, vte::image::k_ref_pool_id_none.value());
                g_assert_true(ring.image_pool().lookup(id) == image.get());
                ids.push_back(id);
        }

        ring.reset();
        ring.validate_images();

        g_assert_cmpuint(ring.image_map().size(), ==, 0);

        /* Not one of them resolves any more. Anything an id still answered
         * with here would be a destroyed image.
         */
        for (auto const id : ids)
                g_assert_null(ring.image_pool().lookup(id));

        g_assert_cmpuint(ring.image_pool().live_count(), ==, 0);
}

static void
test_ring_image_reset_returns_the_ids(void)
{
        /* Place, reset, repeat: every RIS and every "reset" the user runs
         * takes this path, so it must not spend the id space. A live id is
         * unreclaimable - only a sweep returns an id, and only a retired one -
         * so leaked ids accumulate until append_image() can allocate no more
         * and silently drops every image from then on.
         */
        auto ring = Ring{24, false};
        ring.set_visible_rows(24);

        append_rows(ring, 24);
        place_image(ring, 2, 3);

        /* The fixture: the image really took an id out of the pool. */
        g_assert_cmpuint(ring.image_map().size(), ==, 1);
        g_assert_cmpuint(ring.image_pool().live_count(), ==, 1);
        g_assert_cmpuint(ring.image_map().begin()->second->get_pool_id().value(),
                         !=, vte::image::k_ref_pool_id_none.value());

        ring.reset();
        auto const available = ring.image_pool().available();

        /* A reset does not rewind the row numbering, so each round places its
         * image against the rows that round appended.
         */
        for (auto i = 0; i < 200; i++) {
                auto const base = long(ring.next());
                append_rows(ring, 24);
                place_image(ring, base + 2, 3);
                g_assert_cmpuint(ring.image_map().size(), ==, 1);
                ring.reset();
        }

        ring.validate_images();

        /* Two hundred images later the pool is exactly where the first reset
         * left it: the ids came back rather than piling up.
         */
        g_assert_cmpuint(ring.image_pool().live_count(), ==, 0);
        g_assert_cmpuint(ring.image_pool().retired_count(), ==, 0);
        g_assert_cmpuint(ring.image_pool().available(), ==, available);
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
                ring.stamp_image_row(vte::grid::coords(2 + r, 0),
                                     1,
                                     tile_row_t(uint32_t(r)));
        ring.set_placing_image(nullptr);

        /* Each stamped cell names the image AND its own place in it. */
        for (auto r = 0u; r < 3u; r++) {
                auto const* row = ring.index(2 + r);
                g_assert_nonnull(row);
                g_assert_cmpint(row->len, >, 0);

                auto const& attr = row->cells[0].attr;
                g_assert_true(attr.image());

                auto const ref = attr.image_ref();
                g_assert_cmpuint(ref.pool_id().value(), ==, id.value());
                g_assert_cmpuint(ref.tile_row().value(), ==, r);
                g_assert_cmpuint(ref.tile_col().value(), ==, 0);

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
        g_assert_cmpuint(id.value(), !=, vte::image::k_ref_pool_id_none.value());

        auto* row = ring.index_writable(2);
        g_assert_cmpint(row->len, >, 0);
        row->cells[0].attr.set_image_ref(vte::image::Ref{id, tile_row_t{0}, tile_col_t{0}});

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
                ring.stamp_image_row(vte::grid::coords(5 + r, 0),
                                     1,
                                     tile_row_t(uint32_t(r)));
        ring.set_placing_image(nullptr);

        auto const placed = ring.find_image_anchor(id);
        g_assert_true(placed.has_value());
        g_assert_cmpint(placed->row(), ==, 5);
        g_assert_cmpint(placed->column(), ==, 0);

        /* Insert a row above it: every row below shifts down by one, and the
         * anchoring cell goes with them.
         */
        ring.insert(5, 0);
        ring.validate_images();

        auto const shifted = ring.find_image_anchor(id);
        g_assert_true(shifted.has_value());
        g_assert_cmpint(shifted->row(), ==, 6);
        g_assert_cmpint(shifted->column(), ==, 0);

        /* An id nothing names has no anchor, rather than a wrong one. */
        auto const unused = ring.image_pool().allocate(nullptr);
        g_assert_false(ring.find_image_anchor(unused).has_value());
        g_assert_false(ring.find_image_anchor(vte::image::k_ref_pool_id_none).has_value());
}

/* Collect the screen positions of the cells that name @id, as tile coordinate
 * to position, so a test can say where each piece of a picture ended up.
 */
static std::map<std::pair<uint32_t, uint32_t>, std::pair<long, long>>
image_cell_positions(Ring& ring,
                     pool_id_t id)
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

                        found[{ref.tile_row().value(), ref.tile_col().value()}] = {r, c};
                }
        }

        return found;
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
                ring.stamp_image_row(vte::grid::coords(top + r, left),
                                     cols_wide,
                                     tile_row_t(uint32_t(r)));
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
        row->cells[0].attr.set_image_ref(vte::image::Ref{id, tile_row_t{3}, tile_col_t{0}});
        g_assert_true(ring.index(1)->cells[0].attr.image());

        /* The id is retired, which is the state a freed image leaves its own
         * id in and the only way the ring can hold one that resolves to
         * nothing. Leaving it live would be a state the ring cannot reach: a
         * live id names an image the ring holds.
         */
        ring.image_pool().retire(id);
        g_assert_null(ring.image_pool().lookup(id));

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
        g_assert_cmpuint(ref.tile_row().value(), ==, 3);
        g_assert_cmpuint(ref.tile_col().value(), ==, 0);

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
        g_assert_cmpuint(ref.pool_id().value(), ==, vte::image::k_ref_pool_id_none.value());
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
        ring.stamp_image_row(vte::grid::coords(1, 0),
                             1,
                             tile_row_t(uint32_t(0)));
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
        g_assert_cmpuint(ref.tile_row().value(), ==, 0);
        g_assert_cmpuint(ref.tile_col().value(), ==, 0);

        /* And it resolves to THE SAME image object, not to nothing and not
         * to some other image that inherited the id.
         */
        g_assert_true(ref.valid());
        g_assert_true(ring.image_pool().lookup(ref) == image);

        /* The id is re-resolved rather than replayed: whatever it is now, it
         * is the id the image actually holds.
         */
        g_assert_cmpuint(ref.pool_id().value(), ==, image->get_pool_id().value());
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
                ring.stamp_image_row(vte::grid::coords(2 + r, 0),
                                     1,
                                     tile_row_t(uint32_t(r)));
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
                ring.stamp_image_row(vte::grid::coords(top + r, left),
                                     cols_wide,
                                     tile_row_t(uint32_t(r)));
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
         * The alternative is all-or-nothing: any write whose rectangle touches
         * an image's bounding box deletes the whole image, which is what a
         * whole-image blit forces, since it cannot draw an image missing some
         * of its cells. The draw walks the cells, so it can.
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
                ring.stamp_image_row(vte::grid::coords(2 + r, 0),
                                     4,
                                     tile_row_t(uint32_t(r)));
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
                ring.stamp_image_row(vte::grid::coords(2 + r, 0),
                                     4,
                                     tile_row_t(uint32_t(r)));
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
        ring.stamp_image_row(vte::grid::coords(1, 0),
                             4,
                             tile_row_t(uint32_t(0)));
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
        ring.stamp_image_row(vte::grid::coords(1, 0),
                             4,
                             tile_row_t(uint32_t(0)));
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
test_ring_image_limit_spares_the_unrecoverable(void)
{
        /* Of two images the budget can only keep one, the one to drop is the
         * one that can be read back, not simply the older one.
         *
         * The two orders differ as soon as the cursor has moved back up the
         * screen: an image placed above one that has already reached the
         * scrollback is the YOUNGER of the two, and it is the one with nowhere
         * to be read back from.
         */
        auto ring = Ring{1024, true};
        ring.set_visible_rows(24);
        append_rows(ring, 10);

        /* The older image, far enough down the ring that the writable window
         * will still be holding its row; then the younger one above it, which
         * that window will have left behind.
         */
        place_image(ring, 8, 1);
        auto const unrecoverable = ring.image_map().rbegin()->first;

        place_image(ring, 2, 1);
        auto const recoverable = ring.image_map().rbegin()->first;

        g_assert_cmpuint(unrecoverable, <, recoverable);

        /* Scroll until row 2 is frozen and row 8 is not. Without that split the
         * two orders pick the same image and the test proves nothing.
         */
        append_rows(ring, 28);
        g_assert_cmpuint(ring.writable_start_for_test(), >, 2);
        g_assert_cmpuint(ring.writable_start_for_test(), <=, 8);

        /* Both still resident, and together over the budget about to be set. */
        g_assert_cmpuint(ring.image_map().size(), ==, 2);
        auto const budget = ring.image_map().begin()->second->resource_size();
        g_assert_cmpuint(ring.image_memory_used(), >, budget);

        ring.set_image_memory_max(budget);
        ring.validate_images();

        g_assert_cmpuint(ring.image_memory_used(), <=, budget);
        g_assert_true(ring.image_map().find(unrecoverable) != ring.image_map().end());
        g_assert_true(ring.image_map().find(recoverable) == ring.image_map().end());
        g_assert_cmpuint(ring.image_spill_count_for_test(), ==, 1);

        /* And the one that went was only moved: its row reads back with its
         * picture, which is what makes it the harmless one to take.
         */
        auto const* const data = ring.index(2);
        g_assert_nonnull(data);
        g_assert_cmpint(data->len, >, 0);
        g_assert_true(data->cells[0].attr.image());

        auto const* const image = ring.image_pool().lookup(data->cells[0].attr.image_ref());
        g_assert_nonnull(image);
        g_assert_cmpuint(image->get_priority(), ==, recoverable);
}


static void
test_image_geometry_follows_its_layout_cell(void)
{
        /* An image is laid out against one cell and keeps it: the footprint
         * is measured in that cell, and so is the draw.
         *
         * Which cell that is, is Terminal::image_cell_size()'s business - the
         * font's, floored - and is asserted where a font exists, in
         * image-contract-test.cc. What is asserted here is what the image does
         * with the cell it was given, since the anchoring rules and the draw
         * both read the answer back.
         */
        auto const width_px = 95;
        auto const height_px = 45;
        auto const epsilon = 1e-9;

        auto make = [&](int cell_w, int cell_h) {
                auto surface = vte::take_freeable
                        (cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                    width_px, height_px));
                return std::make_unique<vte::image::Image>(std::move(surface),
                                                           1, width_px, height_px,
                                                           0, 0, cell_w, cell_h);
        };

        auto const image = make(10, 20);
        auto const coarse = make(20, 40);

        /* Fixture: two images of the same pixels, laid out against cells that
         * really do differ. Without that the comparison below is vacuous.
         */
        g_assert_cmpint(image->get_cell_width(), ==, 10);
        g_assert_cmpint(image->get_cell_height(), ==, 20);
        g_assert_cmpint(coarse->get_cell_width(), ==, 20);
        g_assert_cmpint(coarse->get_cell_height(), ==, 40);

        /* The footprint covers the pixels, and no more than it must: a
         * partial trailing cell still occupies a whole cell, because a cell
         * is the smallest thing that can carry a reference and pixels outside
         * the footprint are pixels nothing can draw.
         */
        g_assert_cmpint(image->get_width() * image->get_cell_width(), >=, width_px);
        g_assert_cmpint(image->get_height() * image->get_cell_height(), >=, height_px);
        g_assert_cmpint((image->get_width() - 1) * image->get_cell_width(), <, width_px);
        g_assert_cmpint((image->get_height() - 1) * image->get_cell_height(), <, height_px);

        /* And the cell is what decides how many: the same pixels over a cell
         * twice the size cover fewer cells on both axes.
         */
        g_assert_cmpint(coarse->get_width(), <, image->get_width());
        g_assert_cmpint(coarse->get_height(), <, image->get_height());

        /* Drawn against the cell it was laid out at, an image is its own
         * size; drawn against a larger one it grows by the ratio, which is
         * how an image keeps covering the same cells when the font changes
         * under it. The footprint does not move with the draw.
         */
        g_assert_cmpfloat_with_epsilon(image->get_width_pixels(image->get_cell_width()),
                                       double(width_px), epsilon);
        g_assert_cmpfloat_with_epsilon(image->get_height_pixels(image->get_cell_height()),
                                       double(height_px), epsilon);
        g_assert_cmpfloat_with_epsilon(image->get_width_pixels(2 * image->get_cell_width()),
                                       2. * width_px, epsilon);

        auto const cols = image->get_width();
        (void)image->get_width_pixels(3 * image->get_cell_width());
        g_assert_cmpint(image->get_width(), ==, cols);
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
        ring.stamp_image_row(vte::grid::coords(1, 0),
                             1,
                             tile_row_t(uint32_t(0)));
        ring.set_placing_image(nullptr);

        /* Freeze it into the scrollback. */
        append_rows(ring, 200);

        /* Thaw it into the cached row: this is what the draw loop does. */
        auto const* const thawed = ring.index(1);
        g_assert_nonnull(thawed);
        g_assert_cmpint(thawed->len, >, 0);
        g_assert_true(thawed->cells[0].attr.image());

        auto const ref = thawed->cells[0].attr.image_ref();
        auto const cached_id = ref.pool_id().value();
        g_assert_cmpuint(cached_id, !=, vte::image::k_ref_pool_id_none.value());
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
        g_assert_cmpuint(newimg->get_pool_id().value(), !=, cached_id);

        /* And the cached row, served again without re-thawing, still resolves
         * to nothing rather than to the new picture.
         */
        auto const* const again = ring.index(1);
        g_assert_nonnull(again);
        g_assert_true(again->cells[0].attr.image());
        g_assert_cmpuint(again->cells[0].attr.image_ref().pool_id().value(), ==, cached_id);
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
        ring.stamp_image_row(vte::grid::coords(1, 0),
                             4,
                             tile_row_t(uint32_t(0)));
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
static pool_id_t
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
        ring.stamp_image_row(vte::grid::coords(2, 0),
                             4,
                             tile_row_t(uint32_t(0)));
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
                ring.stamp_image_row(vte::grid::coords(row, 0),
                                     4,
                                     tile_row_t(uint32_t(0)));
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
        g_test_add_func("/vte/image/geometry-follows-layout-cell", test_image_geometry_follows_its_layout_cell);
        g_test_add_func("/vte/image/ref/covers-max-legal-image", test_image_ref_covers_max_legal_image);
        g_test_add_func("/vte/image/ref/out-of-range-cannot-alias", test_image_ref_out_of_range_cannot_alias);
        g_test_add_func("/vte/image/ref/fields-do-not-alias", test_image_ref_fields_do_not_alias);
        g_test_add_func("/vte/image/ref/zero-is-not-an-image", test_image_ref_zero_is_not_an_image);
        g_test_add_func("/vte/image/ref/stripe-identity", test_image_ref_stripe_identity);
        g_test_add_func("/vte/image/coordinate-spaces-are-distinct",
                        test_image_coordinate_spaces_are_distinct);
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
        g_test_add_func("/vte/ring/image-pool/reset-retires-every-id", test_ring_image_reset_retires_every_id);
        g_test_add_func("/vte/ring/image-pool/reset-returns-the-ids", test_ring_image_reset_returns_the_ids);

        g_test_add_func("/vte/ring/image-pool/cells-hold-object-replacement", test_ring_image_cells_hold_object_replacement);
        g_test_add_func("/vte/ring/image-pool/covers-every-cell-it-claims", test_ring_image_covers_every_cell_it_claims);
        g_test_add_func("/vte/ring/image-pool/cells-carry-the-reference", test_ring_image_cells_carry_the_reference);
        g_test_add_func("/vte/ring/image-pool/sweep-sees-cell-references", test_ring_image_sweep_sees_cell_references);

        g_test_add_func("/vte/ring/image-pool/anchor-follows-the-cells", test_ring_image_anchor_follows_the_cells);
        g_test_add_func("/vte/ring/image-pool/cells-stay-with-their-image", test_ring_image_cells_stay_with_their_image);

        g_test_add_func("/vte/ring/image-pool/reference-survives-freeze", test_ring_image_reference_survives_freeze);
        g_test_add_func("/vte/ring/image-pool/reference-rebinds-after-thaw", test_ring_image_reference_rebinds_after_thaw);

        g_test_add_func("/vte/ring/image/partial-erase-keeps-the-rest", test_ring_image_partial_erase_keeps_the_rest);
        g_test_add_func("/vte/ring/image/full-erase-frees-it", test_ring_image_full_erase_frees_it);

        g_test_add_func("/vte/ring/image/pixels-survive-eviction", test_ring_image_pixels_survive_eviction);
        g_test_add_func("/vte/ring/image/spill-is-reclaimed", test_ring_image_spill_is_reclaimed);

        g_test_add_func("/vte/ring/image/limit-is-enforced", test_ring_image_limit_is_enforced);
        g_test_add_func("/vte/ring/image/limit-zero-disables", test_ring_image_limit_zero_disables);
        g_test_add_func("/vte/ring/image/limit-shrinks-immediately", test_ring_image_limit_shrinks_immediately);
        g_test_add_func("/vte/ring/image/limit-spares-the-unrecoverable", test_ring_image_limit_spares_the_unrecoverable);

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
        g_test_add_func("/vte/ring/image/rectangle-answers-for-frozen-rows",
                        test_ring_image_rectangle_answers_for_frozen_rows);
#endif

        return g_test_run();
}
