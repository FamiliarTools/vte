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

/* The image contract, asserted on the CELLS of a real terminal.
 *
 * Two halves of it. The lifetime half is that a write to a cell an image owns
 * takes that cell back from the image - see Terminal::erase_images_in_rect()
 * and Ring::validate_image_cells(); every sequence handler that writes cells
 * is supposed to route through the one choke point that enforces it. The
 * geometry half is that an image is laid out against the cell this terminal
 * reports to applications - see Terminal::image_cell_size().
 *
 * ring-test.cc cannot check either. It drives Ring directly, so it can only
 * ever assert what a test itself chose to do to the cells, and it has no font,
 * so the cell an image would really be laid out against is exactly the part it
 * has to invent. The escape sequence has to be parsed for those questions to be
 * asked at all, which needs a Terminal, which needs a widget - so this is a
 * widget test that reads cells, and it links the library's objects rather than
 * the shared library because none of that is exported.
 *
 * Exits 77 (meson's "skipped") when there is no display to realize a widget
 * on, so a sandboxed build reports "skipped" rather than a failure it cannot
 * tell apart from a real one.
 */

#include "config.h"

#include <glib.h>
#include <gtk/gtk.h>

#include <string>

#include "vte/vte.h"

#if WITH_SIXEL

#include "vteinternal.hh"
#include "vterowdata.hh"
#include "vteunistr.h"

using namespace vte::base;

static VteTerminal* terminal;
static vte::terminal::Terminal* impl;

/* Run the main loop until @done or the deadline.
 *
 * feed() only queues bytes; the parser runs from the scheduler, which falls
 * back to a 10hz source on the default main context when no frame clock is
 * advancing (see scheduler.cc) - which is the case for an Xvfb window nothing
 * is composing. So the loop has to be iterated, and iterated for long enough
 * to cover several of those ticks.
 */
static bool
pump_until(bool (*done)(), int timeout_ms)
{
        auto const deadline = g_get_monotonic_time() + timeout_ms * 1000;

        while (g_get_monotonic_time() < deadline) {
                g_main_context_iteration(nullptr, false);

                if (done())
                        return true;

                g_usleep(1000);
        }

        return done();
}

static bool
sized(void)
{
        return impl->m_cell_width > 0 && impl->m_cell_height > 0;
}

static bool
idle(void)
{
        return impl->m_incoming_queue.empty();
}

static void
feed(std::string const& data)
{
        vte_terminal_feed(terminal, data.data(), data.size());
        pump_until(idle, 5000);
}

/* Ask the ring to check its own image invariants.
 *
 * Worth calling and worth NOT relying on. Ring::validate_images() is built into
 * every build, but its assertions are vte_assert_*, which -DG_DISABLE_ASSERT
 * turns into nothing - and the library objects this test links carry that flag
 * unless the tree was configured with -Ddbg=true. Only a test's OWN
 * assertions are certain to be live here, so every fact this file depends on is
 * asserted below rather than delegated to the ring.
 */
static void
check_the_ring(Ring const& ring)
{
        ring.validate_images();
}

/* The one image the terminal is holding, by the maps. */
static vte::image::Image const*
the_image(Ring const& ring)
{
        g_assert_cmpuint(ring.image_map().size(), ==, 1);

        return ring.image_map().begin()->second.get();
}

/* A combining mark aimed at a cell an image owns.
 *
 * The mark is the case the choke point was missing. insert_char()'s zero-width
 * branch writes cells and then jumps past the erase call that every other write
 * makes, so the mark landed inside the image's rectangle, the image stayed
 * alive, and the cell it no longer owned went on naming it.
 *
 * The fixture is asserted before the behaviour is: that the image is resident
 * with cells, and that the mark actually landed on one of them. Without those,
 * a test that never reaches the state can pass while proving nothing - if the
 * mark were dropped on the way in (no previous cell, a fragment, a tab), the
 * contract assertion below would hold vacuously.
 */
static void
test_combining_mark_takes_the_cell(void)
{
        auto& ring = *impl->m_screen->row_data;

        /* An image 200 pixels wide is at least three cells wide on any cell
         * this terminal can have, which is what the case needs: a cell to write
         * into that is not the anchor, and cells left over afterwards so that
         * the image's survival is a fact about the contract rather than about
         * there being nothing left of it.
         */
        feed("\x1b[H"
             "\x1bP0;0;0q"
             "\"1;1;200;20"
             "#0;2;0;0;100#0" +
             std::string(200, '~') +
             "\x1b\\");

        auto const* const image = the_image(ring);
        auto const id = image->get_pool_id();
        auto const top = Ring::row_t(image->get_top());
        auto const left = Ring::column_t(image->get_left());

        g_assert_cmpint(long(image->get_width()), >=, 3);

        /* The burst has ended, so the image is subject to the ordinary rules
         * rather than exempt from them as it is while it is being placed.
         */
        g_assert_null(ring.placing_image());

        /* Fixture, part one: the cell the mark is aimed at is the image's. */
        auto const col = left + 1;
        auto* row = ring.index_writable(top);
        g_assert_cmpint(long(row->len), >, long(col));

        g_assert_true(row->cells[col].attr.image());
        g_assert_cmpuint(row->cells[col].attr.image_ref().pool_id(), ==, id);
        g_assert_cmpuint(row->cells[col].c, ==, VTE_OBJECT_REPLACEMENT_CHARACTER);

        check_the_ring(ring);

        /* Put the cursor one column PAST the target and print a combining
         * acute. A zero-width character combines onto the cell to its left,
         * which is how a write reaches a cell the cursor is not on.
         */
        auto const cursor_row = long(top) - impl->m_screen->insert_delta + 1;
        feed("\x1b[" + std::to_string(cursor_row) +
             ";" + std::to_string(col + 2) + "H"
             "\xcc\x81");

        /* Fixture, part two: the mark reached that exact cell.
         *
         * The cell now holds a two character combining sequence, whichever base
         * it was built on. That is true whether or not the choke point was
         * called, so it says the write happened without saying anything about
         * the contract.
         */
        row = ring.index_writable(top);
        g_assert_cmpint(long(row->len), >, long(col));
        g_assert_cmpuint(_vte_unistr_strlen(row->cells[col].c), ==, 2);

        /* The contract: the cell that was written is not the image's any more. */
        g_assert_false(row->cells[col].attr.image());

        /* Nor is anything left in its text standing for the image. The base of
         * the combining sequence is the blank the choke point left, the same
         * base the mark would have got on any other erased cell, so get_text()
         * does not hand the clipboard, the selection or the a11y snapshot an
         * object replacement character at a position no image covers.
         */
        g_assert_cmpuint(_vte_unistr_get_base(row->cells[col].c), !=,
                         VTE_OBJECT_REPLACEMENT_CHARACTER);

        /* And the rest of the picture is: a write takes back the cells it
         * covers, not the whole image.
         */
        g_assert_cmpuint(ring.image_map().size(), ==, 1);
        g_assert_cmpuint(the_image(ring)->get_pool_id(), ==, id);

        check_the_ring(ring);
}

/* A rectangular copy that takes an image's cells with it.
 *
 * DECCRA copies whole cells from one rectangle of the screen to another, and a
 * cell that names an image IS that image at one of its tiles. Copied as it
 * stands, it makes a second run of cells claim tiles of a picture that does not
 * cover them, with nothing behind it to draw and nothing that can ever erase or
 * move it as the image's own cells are erased and moved.
 *
 * The rectangle copied here holds the image AND a letter to its right, and the
 * letter is what says the copy landed: a destination that is merely free of
 * images proves nothing if the copy never reached it. So the fixture is asserted
 * in both directions - the source really is the image plus a marker, and the
 * marker really did arrive - before the contract is asked at all.
 */
static void
test_copy_rect_leaves_the_image_behind(void)
{
        auto& ring = *impl->m_screen->row_data;

        /* Start from nothing, so the images below are this test's own however
         * this file is ordered.
         */
        feed("\x1b" "c"); /* RIS, split so the c is not read as more hex */
        g_assert_cmpuint(ring.image_map().size(), ==, 0);

        feed("\x1b[H"
             "\x1bP0;0;0q"
             "\"1;1;200;20"
             "#0;2;0;0;100#0" +
             std::string(200, '~') +
             "\x1b\\");

        auto const* const image = the_image(ring);
        auto const id = image->get_pool_id();
        auto const top = Ring::row_t(image->get_top());
        auto const left = long(image->get_left());
        auto const width = long(image->get_width());
        auto const height = long(image->get_height());

        g_assert_null(ring.placing_image());
        g_assert_cmpint(width, >=, 3);
        g_assert_cmpint(height, >=, 1);

        /* The rectangle in screen coordinates: the image, plus the column just
         * right of it for the marker. Both rectangles have to fit on the page
         * with room between them, or the sequence is ignored or clipped and the
         * test asks its question of a copy that never happened.
         */
        auto const screen_top = long(top) - impl->m_screen->insert_delta + 1;
        auto const marker_col = left + width + 1;
        auto const dest_top = screen_top + height + 2;

        g_assert_cmpint(marker_col, <=, long(impl->m_column_count));
        g_assert_cmpint(dest_top + height - 1, <=, long(impl->m_row_count));

        feed("\x1b[" + std::to_string(screen_top) +
             ";" + std::to_string(marker_col) + "H"
             "X");

        /* Fixture, part one: the source rectangle is the image with a marker
         * beside it, and the image is anchored as it should be.
         */
        auto const* row = ring.index_writable(top);
        g_assert_cmpint(long(row->len), >=, marker_col);

        g_assert_true(row->cells[left].attr.image());
        g_assert_cmpuint(row->cells[left].attr.image_ref().pool_id(), ==, id);
        g_assert_cmpuint(row->cells[marker_col - 1].c, ==, 'X');
        g_assert_false(row->cells[marker_col - 1].attr.image());

        g_assert_true(ring.image_cells_are_anchored());
        check_the_ring(ring);

        /* Copy that rectangle down the page, left edge to column one. */
        feed("\x1b[" + std::to_string(screen_top) +
             ";" + std::to_string(left + 1) +
             ";" + std::to_string(screen_top + height - 1) +
             ";" + std::to_string(marker_col) +
             ";1;" + std::to_string(dest_top) +
             ";1;1$v");

        /* Fixture, part two: the copy reached the destination. The marker sat
         * @width columns right of the rectangle's left edge, so it is now
         * @width columns right of column one.
         */
        auto const dest_row = Ring::row_t(impl->m_screen->insert_delta + dest_top - 1);
        auto const* drow = ring.index_writable(dest_row);
        g_assert_nonnull(drow);
        g_assert_cmpint(long(drow->len), >, width);
        g_assert_cmpuint(drow->cells[width].c, ==, 'X');

        /* The contract: what was copied is not the picture. No cell of the
         * destination names an image, and none of them stands in for one
         * either - a cell left holding the image's U+FFFC would report an
         * object replacement character to copied text and to the screen reader
         * with nothing behind it.
         */
        for (auto r = dest_row; r < dest_row + Ring::row_t(height); r++) {
                auto const* const crow = ring.index_writable(r);
                g_assert_nonnull(crow);

                for (auto c = 0; c < crow->len; c++) {
                        g_assert_false(crow->cells[c].attr.image());
                        g_assert_cmpuint(crow->cells[c].c, !=,
                                         VTE_OBJECT_REPLACEMENT_CHARACTER);
                }
        }

        /* And the source is untouched: a copy reads it, so the picture stays
         * where it was, whole and still anchored to its own cells.
         */
        g_assert_cmpuint(ring.image_map().size(), ==, 1);
        g_assert_cmpuint(the_image(ring)->get_pool_id(), ==, id);

        row = ring.index_writable(top);
        g_assert_true(row->cells[left].attr.image());
        g_assert_cmpuint(row->cells[left].attr.image_ref().pool_id(), ==, id);

        g_assert_true(ring.image_cells_are_anchored());
        check_the_ring(ring);
}

/* Place the test image at the home position, on a screen holding nothing else,
 * and return it. Wide enough to be several cells across on any cell this
 * terminal can have, so that a footprint is something to measure rather than a
 * rounding artefact.
 */
static vte::image::Image const*
place_the_image(Ring const& ring)
{
        feed("\x1b" "c"); /* RIS, split so the c is not read as more hex */
        g_assert_cmpuint(ring.image_map().size(), ==, 0);

        feed("\x1b[H"
             "\x1bP0;0;0q"
             "\"1;1;200;20"
             "#0;2;0;0;100#0" +
             std::string(200, '~') +
             "\x1b\\");

        return the_image(ring);
}

/* The cell an image is laid out against is the cell the terminal reports.
 *
 * CSI 14t, TIOCGWINSZ and XTSMGRAPHICS all answer with the unscaled font cell,
 * and a sixel stream carries pixels and no way to ask for cells, so that reply
 * is the only figure a sender can size an image from. Laying out against
 * anything else - a fixed cell, or the cell that happened to be in effect when
 * the first image of the session arrived - makes the image cover a rectangle
 * of the grid its sender did not ask for, with no way to find out.
 *
 * The font is changed here rather than merely inspected, because a layout cell
 * read once and cached would agree with the reported cell until something moved
 * it and never again. The fixture is asserted first: the reported cell really
 * did change, or the comparison after it means nothing.
 */
static void
test_footprint_is_the_reported_cell(void)
{
        auto& ring = *impl->m_screen->row_data;

        auto small = vte::take_freeable(pango_font_description_from_string("Monospace 10"));
        vte_terminal_set_font(terminal, small.get());

        auto const* image = place_the_image(ring);

        /* Fixture: a font cell above the floor, so that what is compared
         * below is the font's cell and not VTE_SIXEL_CELL_MIN_* standing in
         * for it.
         */
        g_assert_cmpint(impl->m_cell_width_unscaled, >, long(VTE_SIXEL_CELL_MIN_WIDTH));
        g_assert_cmpint(impl->m_cell_height_unscaled, >, long(VTE_SIXEL_CELL_MIN_HEIGHT));

        auto const cell_w = impl->m_cell_width_unscaled;
        auto const cell_h = impl->m_cell_height_unscaled;

        g_assert_cmpint(long(image->get_cell_width()), ==, cell_w);
        g_assert_cmpint(long(image->get_cell_height()), ==, cell_h);
        g_assert_cmpint(long(image->get_width()), ==,
                        (long(image->get_width_px()) + cell_w - 1) / cell_w);
        g_assert_cmpint(long(image->get_height()), ==,
                        (long(image->get_height_px()) + cell_h - 1) / cell_h);

        auto large = vte::take_freeable(pango_font_description_from_string("Monospace 22"));
        vte_terminal_set_font(terminal, large.get());

        image = place_the_image(ring);

        /* Fixture: the reported cell moved, and the image is the same pixels
         * as before. Neither is worth asserting the contract against alone.
         */
        g_assert_cmpint(impl->m_cell_width_unscaled, !=, cell_w);
        g_assert_cmpint(impl->m_cell_height_unscaled, !=, cell_h);
        g_assert_cmpint(impl->m_cell_width_unscaled, >, long(VTE_SIXEL_CELL_MIN_WIDTH));

        /* The contract: the image that arrived after the change is laid out
         * against the cell reported after the change.
         */
        g_assert_cmpint(long(image->get_cell_width()), ==, impl->m_cell_width_unscaled);
        g_assert_cmpint(long(image->get_cell_height()), ==, impl->m_cell_height_unscaled);
        g_assert_cmpint(long(image->get_width()), ==,
                        (long(image->get_width_px()) + impl->m_cell_width_unscaled - 1) /
                        impl->m_cell_width_unscaled);

        vte_terminal_set_font(terminal, nullptr);
}

/* The zoom does not move the footprint.
 *
 * GNOME/vte#253: "output some image, increase zoom and the image zooms with it
 * (fine so far); output the same image again, and the new image is smaller than
 * the zoomed one." The zoom scales the drawn cell and leaves the reported one
 * alone, so an image laid out against the reported cell covers the same cells
 * before and after, and the two placements are drawn at the same size.
 */
static void
test_footprint_ignores_the_zoom(void)
{
        auto& ring = *impl->m_screen->row_data;

        auto const* image = place_the_image(ring);

        auto const cell_w = impl->m_cell_width_unscaled;
        auto const drawn_w = impl->m_cell_width;
        auto const width = image->get_width();
        auto const height = image->get_height();

        g_assert_cmpint(width, >=, 3);

        vte_terminal_set_font_scale(terminal, 2.);

        image = place_the_image(ring);

        /* Fixture: the zoom took effect - the cell the image is DRAWN in grew
         * - and it left the reported cell where it was.
         */
        g_assert_cmpint(impl->m_cell_width, >, drawn_w);
        g_assert_cmpint(impl->m_cell_width_unscaled, ==, cell_w);

        /* The contract: same file, same footprint, so the two placements are
         * drawn at the same size as each other.
         */
        g_assert_cmpint(long(image->get_cell_width()), ==, cell_w);
        g_assert_cmpint(image->get_width(), ==, width);
        g_assert_cmpint(image->get_height(), ==, height);

        vte_terminal_set_font_scale(terminal, 1.);
}

int
main(int argc,
     char* argv[])
{
#if VTE_GTK == 3
        auto const inited = gtk_init_check(&argc, &argv);
#elif VTE_GTK == 4
        auto const inited = gtk_init_check();
#endif
        if (!inited) {
                g_printerr("SKIP: no display to realize a terminal on\n");
                return 77;
        }

        terminal = VTE_TERMINAL(vte_terminal_new());
        vte_terminal_set_enable_sixel(terminal, true);
        vte_terminal_set_size(terminal, 80, 24);

#if VTE_GTK == 3
        auto const window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(terminal));
        gtk_widget_show_all(window);
#elif VTE_GTK == 4
        auto const window = gtk_window_new();
        gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(terminal));
        gtk_window_present(GTK_WINDOW(window));
#endif

        impl = _vte_terminal_get_impl(terminal);

        /* The cell is what an image's footprint is measured in, so nothing
         * below means anything until the widget has one.
         */
        if (!pump_until(sized, 10000)) {
                g_printerr("SKIP: the terminal never got a font cell\n");
                return 77;
        }

        test_combining_mark_takes_the_cell();

        g_print("PASS: a combining mark takes back the cell it lands on\n");

        test_copy_rect_leaves_the_image_behind();

        g_print("PASS: a rectangular copy does not copy the image\n");

        test_footprint_ignores_the_zoom();

        g_print("PASS: the zoom does not move an image's footprint\n");

        /* Last, because it leaves the terminal on a different font. */
        test_footprint_is_the_reported_cell();

        g_print("PASS: an image is laid out against the reported cell\n");

        return 0;
}

#else /* !WITH_SIXEL */

int
main(int argc,
     char* argv[])
{
        g_printerr("SKIP: built without SIXEL support\n");
        return 77;
}

#endif /* WITH_SIXEL */
