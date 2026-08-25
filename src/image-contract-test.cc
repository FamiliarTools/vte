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

/* The image lifetime contract, asserted on the CELLS of a real terminal.
 *
 * The contract is that a write to a cell an image owns takes that cell back
 * from the image - see Terminal::erase_images_in_rect() and
 * Ring::validate_image_cells(). Every sequence handler that writes cells is
 * supposed to route through the one choke point that enforces it.
 *
 * ring-test.cc cannot check that. It drives Ring directly, so it can only ever
 * assert what a test itself chose to do to the cells; whether vteseq.cc and
 * vte.cc actually call the choke point on the way in is precisely the part it
 * has to assume. The escape sequence has to be parsed for that question to be
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
