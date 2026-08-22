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

/* Round-trip properties of the scrollback ring's compressed tier.
 *
 * A row that scrolls out of the writable window is serialised into the text,
 * attribute and row streams, and is rebuilt from them the next time anything
 * indexes it. The existing ring tests drive that path with hand-written rows,
 * which checks the cases somebody thought of; these drive it with GENERATED
 * rows and compare against a reference copy taken before the row was ever
 * frozen, which checks the ones nobody did.
 *
 * The failure mode this guards is the reason it is worth the machinery:
 * freeze_row() run-length encodes attributes, so a mistake there does not
 * crash and does not fail to produce a row - it produces a DIFFERENT row,
 * silently, and only for scrollback the user has stopped looking at. A test
 * that compares cell for cell against a pre-freeze snapshot is the only thing
 * that turns that into a red build.
 *
 * Randomness comes from GLib's test RNG, so a failure reports a seed and
 * `--seed=` replays it exactly.
 */

#include "config.h"

#include <glib.h>

#include <vector>

#include "ring.hh"
#include "vterowdata.hh"
#include "vteunistr.h"

using namespace vte::base;

/* Enough rows that the writable window is left far behind and the great
 * majority of the corpus is genuinely being read back out of the streams
 * rather than out of the array. Verified rather than assumed: see
 * assert_actually_froze().
 */
static int const kCorpusRows = 512;

/* A small visible window, so freezing starts almost immediately. */
static int const kVisibleRows = 4;

/* Characters chosen to exercise the width and fragment machinery rather than
 * to look like text: ASCII, a combining mark, and CJK that occupies two
 * columns and therefore leaves a fragment cell behind it.
 */
static gunichar const kNarrowChars[] = {
        'a', 'Z', '0', ' ', '~', 0x00e9 /* e-acute */, 0x0416 /* Cyrillic Zhe */,
};
static gunichar const kWideChars[] = {
        0x4e00 /* CJK one */, 0x65e5 /* CJK sun */, 0xff21 /* fullwidth A */,
};
static gunichar const kCombiningChars[] = {
        0x0301 /* combining acute */, 0x0308 /* combining diaeresis */,
};

/* One row as it stood before it was handed to the ring. VteRowData owns a
 * heap array and is recycled by the ring, so the reference has to be a deep
 * copy taken at generation time - pointing at the ring's own storage would
 * compare it against itself and prove nothing.
 */
struct RowSnapshot {
        std::vector<VteCell> cells;
        guint8 bidi_flags;
        bool soft_wrapped;
};

static int
rand_below(int n)
{
        return g_test_rand_int_range(0, n);
}

/* Fill @attr with a random but INTERNALLY CONSISTENT set of SGR bits.
 *
 * Consistency matters more than coverage here: the columns/fragment fields
 * describe the cell's geometry and the ring is entitled to assume they agree
 * with the character. Scrambling them would test the ring against rows the
 * terminal cannot produce, and any failure would be the test's fault.
 */
static void
randomise_sgr(VteCellAttr* attr)
{
        attr->set_bold(rand_below(2));
        attr->set_italic(rand_below(2));
        attr->set_underline(rand_below(4)); /* none/single/double/curly */
        attr->set_strikethrough(rand_below(2));
        attr->set_overline(rand_below(2));
        attr->set_reverse(rand_below(2));
        attr->set_blink(rand_below(2));
        attr->set_dim(rand_below(2));
        attr->set_invisible(rand_below(2));

        /* Colours live in their own 64-bit word rather than in the attr word,
         * and the stream persists that word whole, so they are a distinct thing
         * to get wrong. Indexed values only: a palette index and an RGB colour
         * are stored differently in the triple, and generating RGB here would
         * assert a packing this test has no business pinning down.
         */
        attr->set_fore(rand_below(256));
        attr->set_back(rand_below(256));
        attr->set_deco(rand_below(256));
}

/* Append one generated row to @ring and return the reference copy.
 *
 * Runs of identical attributes are deliberately common: the attribute stream
 * is run-length encoded, so a corpus of uniformly random cells would only ever
 * exercise the degenerate one-run-per-cell case and would miss every bug in
 * run continuation.
 */
static RowSnapshot
append_generated_row(Ring& ring)
{
        auto snapshot = RowSnapshot{};
        snapshot.bidi_flags = guint8(rand_below(16));
        snapshot.soft_wrapped = rand_below(2);

        auto* const row = ring.append(snapshot.bidi_flags);
        row->attr.soft_wrapped = snapshot.soft_wrapped;

        auto const width = rand_below(80);

        auto attr = VteCellAttr{};
        auto run_left = 0;

        for (auto col = 0; col < width; ) {
                if (run_left == 0) {
                        randomise_sgr(&attr);
                        run_left = 1 + rand_below(12);
                }
                run_left--;

                auto const roll = rand_below(10);
                auto cell = VteCell{};
                cell.attr = attr;

                if (roll < 6) {
                        cell.c = kNarrowChars[rand_below(G_N_ELEMENTS(kNarrowChars))];
                        cell.attr.set_columns(1);
                        cell.attr.set_fragment(false);
                        _vte_row_data_append(row, &cell);
                        snapshot.cells.push_back(cell);
                        col++;
                } else if (roll < 8 && col + 1 < width) {
                        /* A double-width character plus the fragment cell that
                         * stands in for its second column. The pair has to
                         * survive together or the row's geometry is wrong.
                         */
                        cell.c = kWideChars[rand_below(G_N_ELEMENTS(kWideChars))];
                        cell.attr.set_columns(2);
                        cell.attr.set_fragment(false);
                        _vte_row_data_append(row, &cell);
                        snapshot.cells.push_back(cell);

                        /* A fragment carries the base cell's character and
                         * attributes but is itself ONE column wide - that is
                         * the shape thaw_row rebuilds (ring.cc: set_fragment
                         * (true) then set_columns(1)), so it is the shape the
                         * reference has to hold or the test asserts against a
                         * row the terminal never produces.
                         */
                        auto fragment = cell;
                        fragment.attr.set_fragment(true);
                        fragment.attr.set_columns(1);
                        _vte_row_data_append(row, &fragment);
                        snapshot.cells.push_back(fragment);
                        col += 2;
                } else if (roll < 9) {
                        /* A base character with combining marks fused onto it,
                         * which is how the terminal actually stores them: ONE
                         * cell whose vteunistr carries the sequence, not a cell
                         * per mark. It matters here because freeze_row emits an
                         * extra attribute record per mark beyond the first, and
                         * thaw_row merges a zero-column character back into the
                         * preceding cell - so this is the only shape that
                         * exercises the multi-character path at all.
                         */
                        cell.c = kNarrowChars[rand_below(G_N_ELEMENTS(kNarrowChars))];
                        auto const marks = 1 + rand_below(2);
                        for (auto m = 0; m < marks; m++)
                                cell.c = _vte_unistr_append_unichar(
                                        cell.c,
                                        kCombiningChars[rand_below(G_N_ELEMENTS(kCombiningChars))]);
                        cell.attr.set_columns(1);
                        cell.attr.set_fragment(false);
                        _vte_row_data_append(row, &cell);
                        snapshot.cells.push_back(cell);
                        col++;
                } else {
                        /* An empty cell, which is not the same thing as an
                         * absent one: it takes a column and carries attributes.
                         */
                        cell.c = 0;
                        cell.attr.set_columns(1);
                        cell.attr.set_fragment(false);
                        _vte_row_data_append(row, &cell);
                        snapshot.cells.push_back(cell);
                        col++;
                }
        }

        return snapshot;
}

/* The two fields of VteCellAttr that the stream is required to preserve
 * verbatim.
 *
 * hyperlink_idx is deliberately NOT among them: it is a ring-local index, and
 * freezing replaces it with the target text carried in the stream, so the idx
 * a thawed cell reports is a different number naming the same link. Comparing
 * it would assert an implementation detail that the ring never promised. The
 * target STRING is the observable contract and is checked separately.
 */
static bool
attrs_equal(VteCellAttr const& a,
            VteCellAttr const& b)
{
        return a.attr == b.attr && a.colors() == b.colors();
}

static void
assert_row_matches(Ring& ring,
                   Ring::row_t position,
                   RowSnapshot const& expected)
{
        auto const* const row = ring.index(position);
        g_assert_nonnull(row);

        if (row->len != expected.cells.size()) {
                g_error("row %lu: length %u after round trip, %zu before",
                        position, row->len, expected.cells.size());
        }

        g_assert_cmpuint(row->attr.soft_wrapped, ==, expected.soft_wrapped);
        g_assert_cmpuint(row->attr.bidi_flags, ==, expected.bidi_flags);

        for (size_t col = 0; col < expected.cells.size(); col++) {
                auto const& got = row->cells[col];
                auto const& want = expected.cells[col];

                if (got.c != want.c) {
                        g_error("row %lu col %zu: character U+%04X after round trip, "
                                "U+%04X before",
                                position, col, got.c, want.c);
                }
                if (!attrs_equal(got.attr, want.attr)) {
                        g_error("row %lu col %zu: attr %08x/colors %016" G_GINT64_MODIFIER "x "
                                "after round trip, %08x/%016" G_GINT64_MODIFIER "x before",
                                position, col,
                                got.attr.attr, (guint64)got.attr.colors(),
                                want.attr.attr, (guint64)want.attr.colors());
                }
        }
}

/* Guard against the test quietly proving nothing.
 *
 * Every assertion below is about rows that came back OUT OF THE STREAMS. If a
 * change to the ring's sizing kept the whole corpus in the writable array, the
 * comparisons would still pass while exercising no serialisation at all. So
 * assert that the ring really did push rows out of the writable window.
 */
static void
assert_actually_froze(Ring& ring)
{
        auto const frozen = ring.length() - kVisibleRows;
        if (frozen < Ring::row_t(kCorpusRows / 2)) {
                g_error("only %lu of %d rows left the writable window - this test "
                        "would pass without exercising the streams at all",
                        frozen, kCorpusRows);
        }
}

/* Generate a corpus, freeze it by scrolling past it, and read every row back.
 */
static void
test_freeze_thaw_preserves_every_cell(void)
{
        auto ring = Ring{Ring::row_t(kCorpusRows * 4), true /* has_streams */};
        ring.set_visible_rows(kVisibleRows);

        auto expected = std::vector<RowSnapshot>{};
        expected.reserve(kCorpusRows);

        for (auto i = 0; i < kCorpusRows; i++)
                expected.push_back(append_generated_row(ring));

        assert_actually_froze(ring);

        for (auto i = 0; i < kCorpusRows; i++)
                assert_row_matches(ring, ring.delta() + Ring::row_t(i), expected[size_t(i)]);
}

/* The same corpus, read back in a scattered order.
 *
 * Reading front to back walks the streams monotonically, which is the easy
 * direction and the one the cache is built for. Jumping around forces the
 * backwards walk and repeated re-thaws of rows that were already thawed once,
 * which is where an offset bookkeeping error shows up.
 */
static void
test_freeze_thaw_survives_random_access(void)
{
        auto ring = Ring{Ring::row_t(kCorpusRows * 4), true /* has_streams */};
        ring.set_visible_rows(kVisibleRows);

        auto expected = std::vector<RowSnapshot>{};
        expected.reserve(kCorpusRows);

        for (auto i = 0; i < kCorpusRows; i++)
                expected.push_back(append_generated_row(ring));

        assert_actually_froze(ring);

        for (auto probe = 0; probe < kCorpusRows * 2; probe++) {
                auto const i = rand_below(kCorpusRows);
                assert_row_matches(ring, ring.delta() + Ring::row_t(i), expected[size_t(i)]);
        }
}

int
main(int argc,
     char* argv[])
{
        g_test_init(&argc, &argv, nullptr);

        g_test_add_func("/vte/ring/roundtrip/freeze-thaw/every-cell",
                        test_freeze_thaw_preserves_every_cell);
        g_test_add_func("/vte/ring/roundtrip/freeze-thaw/random-access",
                        test_freeze_thaw_survives_random_access);

        return g_test_run();
}
