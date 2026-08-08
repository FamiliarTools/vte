/*
 * Copyright © 2026 Christian Persch
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

int
main(int argc,
     char* argv[])
{
        g_test_init(&argc, &argv, nullptr);

#if WITH_SIXEL
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
