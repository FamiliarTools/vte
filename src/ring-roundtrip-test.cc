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

#include <algorithm>
#include <cstring>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ring.hh"
#include "vtedefines.hh"
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

/* Printable ASCII only, which is a distinct thing to generate rather than a
 * subset of the above. A row of nothing but bytes 32 to 126 is recorded as
 * is_ascii, and a paragraph all of whose rows are gets reflowed by a shortcut
 * path in rewrap that never reads the text stream and advances by a whole row
 * of characters at a time. Nothing else in the corpus reaches that path
 * reliably, and it is the path with the arithmetic in it.
 */
static gunichar const kAsciiChars[] = {
        'a', 'Z', '0', ' ', '~', 'q', '7', '/',
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

/* An instruction to write a hyperlink onto a run of a generated row's cells,
 * together with the description of the run that was written.
 *
 * Only the hyperlink tests below pass one of these; every other caller leaves
 * the generated row unlinked.
 */
struct LinkRequest {
        Ring::hyperlink_idx_t idx{0};   /* in: the pool idx to write */
        bool reach_end_of_row{false};   /* in: the run must end at the last cell */
        Ring::column_t first{0};        /* out: the first linked cell */
        Ring::column_t last{0};         /* out: the last linked cell, inclusive */
        Ring::column_t neighbour{0};    /* out: a cell of the same row with no link */
};

/* Write the requested hyperlink idx onto a run of @row's cells, and record
 * where the run ended up.
 *
 * The run is grown outwards over fragment cells rather than being allowed to
 * end inside a wide character. A fragment does not carry its own attributes
 * through the stream - it is rebuilt from the attributes of the cell it
 * continues - so a run covering a base cell but not its fragment describes a
 * row the ring cannot produce, and the test, not the ring, would be wrong.
 */
static void
write_hyperlink_run(VteRowData* row,
                    RowSnapshot* snapshot,
                    LinkRequest* link)
{
        auto const len = Ring::column_t(snapshot->cells.size());
        g_assert_cmpint(len, >=, 8);

        auto first = Ring::column_t(2 + rand_below(int(len) / 3));
        auto last = link->reach_end_of_row
                ? len - 1
                : std::max(first, std::min(len - 2, first + 2 + Ring::column_t(rand_below(6))));

        while (first > 0 && snapshot->cells[size_t(first)].attr.fragment())
                first--;
        while (last + 1 < len && snapshot->cells[size_t(last) + 1].attr.fragment())
                last++;

        g_assert_cmpint(first, >=, 1);
        g_assert_cmpint(first, <=, last);
        g_assert_cmpint(last, <=, len - 1);

        for (auto col = first; col <= last; col++) {
                row->cells[col].attr.hyperlink_idx = link->idx;
                snapshot->cells[size_t(col)].attr.hyperlink_idx = link->idx;
        }

        link->first = first;
        link->last = last;
        link->neighbour = first - 1;
}

/* Fill @row with @width columns of generated cells and the given wrapping and
 * bidi flags, and return the reference copy.
 *
 * The row is a parameter rather than something appended here because the same
 * generator has to serve both the corpora, which append, and the fuzz target,
 * which also rewrites a row already in the ring - and rewriting a row that has
 * been thawed back out of the streams is precisely the case worth generating.
 *
 * The flags are a parameter rather than another random draw because the rewrap
 * tests need them to describe a paragraph: rewrap only ever sees paragraphs,
 * and a row whose flags contradict its neighbours' is not a row the terminal
 * can produce.
 *
 * Runs of identical attributes are deliberately common: the attribute stream
 * is run-length encoded, so a corpus of uniformly random cells would only ever
 * exercise the degenerate one-run-per-cell case and would miss every bug in
 * run continuation.
 */
static RowSnapshot
generate_row_into(VteRowData* row,
                  guint8 bidi_flags,
                  bool soft_wrapped,
                  int width,
                  bool ascii_only,
                  LinkRequest* link)
{
        auto snapshot = RowSnapshot{};
        snapshot.bidi_flags = bidi_flags;
        snapshot.soft_wrapped = soft_wrapped;

        _vte_row_data_clear(row);
        row->attr.bidi_flags = snapshot.bidi_flags;
        row->attr.soft_wrapped = snapshot.soft_wrapped;

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

                if (ascii_only) {
                        /* No wide characters, no combining marks and no empty
                         * cells: every one of those clears the record's
                         * is_ascii bit and takes the paragraph off the shortcut
                         * path this mode exists to reach.
                         */
                        cell.c = kAsciiChars[rand_below(G_N_ELEMENTS(kAsciiChars))];
                        cell.attr.set_columns(1);
                        cell.attr.set_fragment(false);
                        _vte_row_data_append(row, &cell);
                        snapshot.cells.push_back(cell);
                        col++;
                } else if (roll < 6) {
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

        if (link != nullptr)
                write_hyperlink_run(row, &snapshot, link);

        return snapshot;
}

/* Append one generated row of @width cells to @ring. */
static RowSnapshot
append_generated_row(Ring& ring,
                     guint8 bidi_flags,
                     bool soft_wrapped,
                     int width,
                     LinkRequest* link = nullptr)
{
        return generate_row_into(ring.append(bidi_flags), bidi_flags, soft_wrapped,
                                 width, false /* ascii_only */, link);
}

/* Append one generated row of a random width. */
static RowSnapshot
append_generated_row(Ring& ring,
                     guint8 bidi_flags,
                     bool soft_wrapped)
{
        return append_generated_row(ring, bidi_flags, soft_wrapped, rand_below(80));
}

/* Append one generated row with random wrapping and bidi flags. The freeze
 * and thaw tests keep every row independent, since freezing is per row.
 */
static RowSnapshot
append_generated_row(Ring& ring)
{
        auto const bidi_flags = guint8(rand_below(16));
        auto const soft_wrapped = bool(rand_below(2));
        return append_generated_row(ring, bidi_flags, soft_wrapped);
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

/* @when names the point the ring is being compared at, so that a failure from
 * a long random sequence says which step of it diverged rather than only which
 * row did.
 */
static void
assert_row_matches(Ring& ring,
                   Ring::row_t position,
                   RowSnapshot const& expected,
                   char const* when = "after a round trip")
{
        auto const* const row = ring.index(position);
        g_assert_nonnull(row);

        if (row->len != expected.cells.size()) {
                g_error("row %lu: length %u %s, %zu before",
                        position, row->len, when, expected.cells.size());
        }

        if (bool(row->attr.soft_wrapped) != expected.soft_wrapped) {
                g_error("row %lu: soft_wrapped %u %s, %u before",
                        position, row->attr.soft_wrapped, when,
                        unsigned(expected.soft_wrapped));
        }
        if (row->attr.bidi_flags != expected.bidi_flags) {
                g_error("row %lu: bidi flags %u %s, %u before",
                        position, row->attr.bidi_flags, when, expected.bidi_flags);
        }

        for (size_t col = 0; col < expected.cells.size(); col++) {
                auto const& got = row->cells[col];
                auto const& want = expected.cells[col];

                if (got.c != want.c) {
                        g_error("row %lu col %zu: character U+%04X %s, "
                                "U+%04X before",
                                position, col, got.c, when, want.c);
                }
                if (!attrs_equal(got.attr, want.attr)) {
                        g_error("row %lu col %zu: attr %08x/colors %016" G_GINT64_MODIFIER "x "
                                "%s, %08x/%016" G_GINT64_MODIFIER "x before",
                                position, col,
                                got.attr.attr, (guint64)got.attr.colors(),
                                when,
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

/* Whether two distinct rows are both held in the streams rather than in the
 * writable array.
 *
 * The ring publishes no accessor for that boundary, but it does publish one
 * consequence of it: index() answers for a writable row out of that row's own
 * storage, and for a frozen row out of the single row it thaws into, so two
 * different rows answering with the same address are two frozen rows.
 *
 * This is used only to assert that a test is testing something. The hyperlink
 * test below is entirely about bytes in the attribute stream, and an assertion
 * of that kind which quietly ran against the writable array would pass while
 * proving nothing at all.
 */
static bool
rows_are_frozen(Ring& ring,
                Ring::row_t a,
                Ring::row_t b)
{
        g_assert_cmpuint(a, !=, b);
        return ring.index(a) == ring.index(b);
}

static void
assert_rows_are_frozen(Ring& ring,
                       Ring::row_t a,
                       Ring::row_t b,
                       char const* what)
{
        if (!rows_are_frozen(ring, a, b)) {
                g_error("rows %lu and %lu are not both in the streams, so %s "
                        "would be checked against the writable array",
                        a, b, what);
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

/* Round-trip properties of rewrapping.
 *
 * Rewrapping is the other direction the compressed tier is read in, and a much
 * less forgiving one: instead of rebuilding a row that was frozen from the same
 * row, it rebuilds a whole new set of rows out of the SAME text and attribute
 * streams, driven only by the row records it recomputes as it goes. Nothing in
 * the ring cross-checks the result, so an off-by-one in the wrap arithmetic or
 * in the attribute walk shows up as scrollback that quietly says something else
 * after the window is resized.
 *
 * Two properties are worth pinning down, and they are different properties.
 *
 * The first is that reflowing does not change the TEXT. Whatever the width, the
 * characters of a paragraph, read in order and ignoring the fragment cells that
 * only stand for a wide character's second column, must be exactly the ones
 * that were appended, with their attributes, and there must be the same number
 * of paragraphs.
 *
 * The second is that reflowing is REVERSIBLE. Going out to a set of widths and
 * back has to land on precisely the state we left, row for row and cell for
 * cell, and any marker carried along has to come home to the same cell. That is
 * what makes a resize round trip safe for the cursor and the selection.
 *
 * The second property is only true from a state rewrap itself produced, which
 * is why the corpus is canonicalised with one rewrap before the excursion
 * starts. Rewrap does not consult the old division of a paragraph into rows, so
 * an arbitrarily divided paragraph is not a fixed point of a same width rewrap;
 * a canonically divided one is.
 */

/* The width the excursion starts and ends at, and the widths it passes
 * through. The floor is 3 rather than 1 on purpose: at width 1 a double width
 * character cannot fit at all, and the wrap rule emits empty rows forever
 * rather than dropping it. That is legal but pathological and not what this
 * test is about.
 */
static Ring::column_t const kHomeColumns = 80;
static Ring::column_t const kExcursion[] = { 80, 40, 7, 132, 3, 80 };

/* One paragraph's logical content: its characters in order, with the fragment
 * cells left out, plus the bidi flags the whole paragraph carries.
 *
 * Fragments are excluded because they are geometry, not content. Rewrapping
 * moves a wide character bodily from one row to another, so its fragment's
 * position changes while nothing about the text does.
 */
struct ParagraphSnapshot {
        std::vector<VteCell> cells;
        guint8 bidi_flags;
};

/* Append one generated paragraph: a few soft wrapped rows closed by a hard
 * wrapped one.
 *
 * The bidi flags are uniform across the paragraph because rewrap collapses them
 * to the first row's, that being the only sensible thing to do when the rows a
 * paragraph is divided into are about to be replaced by different ones. A
 * paragraph whose rows disagreed would therefore be a paragraph the terminal
 * cannot produce, and asserting the flags round tripped would be asserting
 * something rewrap never promised.
 *
 * Every paragraph ends hard wrapped, so the ring's last row does too, and the
 * text stream ends with a newline. A ring ending in a soft wrapped row is a
 * legal but separate case: it is the one where the final paragraph has no
 * terminator to strip.
 */
static ParagraphSnapshot
append_generated_paragraph(Ring& ring)
{
        auto paragraph = ParagraphSnapshot{};
        paragraph.bidi_flags = guint8(rand_below(16));

        auto const rows = 1 + rand_below(4);
        for (auto r = 0; r < rows; r++) {
                auto const soft_wrapped = (r + 1 < rows);
                auto const row = append_generated_row(ring, paragraph.bidi_flags,
                                                      soft_wrapped);
                for (auto const& cell : row.cells) {
                        if (!cell.attr.fragment())
                                paragraph.cells.push_back(cell);
                }
        }

        return paragraph;
}

static std::vector<ParagraphSnapshot>
build_paragraph_corpus(Ring& ring)
{
        auto expected = std::vector<ParagraphSnapshot>{};

        while (ring.length() < Ring::row_t(kCorpusRows))
                expected.push_back(append_generated_paragraph(ring));

        return expected;
}

/* The geometric invariants a rewrapped row owes us, whatever its content.
 *
 * A row wider than the width we asked for would be text the terminal cannot
 * display; a fragment that does not sit immediately behind the character it
 * continues would be a wide character sawn in half by the wrap.
 */
static void
assert_row_geometry(VteRowData const* row,
                    Ring::row_t position,
                    Ring::column_t columns)
{
        if (Ring::column_t(row->len) > columns) {
                g_error("row %lu: %u cells after rewrapping to %ld columns",
                        position, row->len, columns);
        }

        for (unsigned int col = 0; col < row->len; col++) {
                auto const& cell = row->cells[col];
                if (!cell.attr.fragment())
                        continue;

                if (col == 0) {
                        g_error("row %lu: begins with a fragment cell, so a wide "
                                "character was split across the wrap", position);
                }

                auto const& base = row->cells[col - 1];
                if (base.c != cell.c) {
                        g_error("row %lu col %u: fragment carries U+%04X but the "
                                "cell it continues carries U+%04X",
                                position, col, cell.c, base.c);
                }
        }
}

/* Read the whole ring back as paragraphs, checking each row's geometry on the
 * way through. Rows are joined while they are soft wrapped, which is the same
 * rule rewrap itself uses to find a paragraph.
 */
static std::vector<ParagraphSnapshot>
read_paragraphs(Ring& ring,
                Ring::column_t columns,
                bool may_end_open = false)
{
        auto paragraphs = std::vector<ParagraphSnapshot>{};
        auto open = false;

        for (auto position = ring.delta(); position < ring.next(); position++) {
                auto const* const row = ring.index(position);
                g_assert_nonnull(row);

                assert_row_geometry(row, position, columns);

                if (!open) {
                        paragraphs.push_back(ParagraphSnapshot{});
                        paragraphs.back().bidi_flags = row->attr.bidi_flags;
                        open = true;
                }

                for (unsigned int col = 0; col < row->len; col++) {
                        if (!row->cells[col].attr.fragment())
                                paragraphs.back().cells.push_back(row->cells[col]);
                }

                if (!row->attr.soft_wrapped)
                        open = false;
        }

        if (open && !may_end_open) {
                g_error("the ring ends in a soft wrapped row, which the corpus "
                        "never generates");
        }

        return paragraphs;
}

static void
assert_paragraphs_match(std::vector<ParagraphSnapshot> const& got,
                        std::vector<ParagraphSnapshot> const& want,
                        Ring::column_t columns)
{
        if (got.size() != want.size()) {
                g_error("%zu paragraphs at width %ld, %zu when generated",
                        got.size(), columns, want.size());
        }

        for (size_t p = 0; p < want.size(); p++) {
                if (got[p].bidi_flags != want[p].bidi_flags) {
                        g_error("paragraph %zu: bidi flags %u at width %ld, %u "
                                "when generated",
                                p, got[p].bidi_flags, columns, want[p].bidi_flags);
                }
                if (got[p].cells.size() != want[p].cells.size()) {
                        g_error("paragraph %zu: %zu characters at width %ld, %zu "
                                "when generated",
                                p, got[p].cells.size(), columns, want[p].cells.size());
                }

                for (size_t i = 0; i < want[p].cells.size(); i++) {
                        auto const& got_cell = got[p].cells[i];
                        auto const& want_cell = want[p].cells[i];

                        if (got_cell.c != want_cell.c) {
                                g_error("paragraph %zu char %zu: U+%04X at width %ld, "
                                        "U+%04X when generated",
                                        p, i, got_cell.c, columns, want_cell.c);
                        }
                        if (!attrs_equal(got_cell.attr, want_cell.attr)) {
                                g_error("paragraph %zu char %zu: attr %08x/colors "
                                        "%016" G_GINT64_MODIFIER "x at width %ld, "
                                        "%08x/%016" G_GINT64_MODIFIER "x when generated",
                                        p, i,
                                        got_cell.attr.attr, (guint64)got_cell.attr.colors(),
                                        columns,
                                        want_cell.attr.attr, (guint64)want_cell.attr.colors());
                        }
                }
        }
}

/* Copy the whole ring out, row for row, addressed relative to delta(). Absolute
 * row numbers are not stable across a rewrap: the ring restarts its numbering
 * at zero, so only the offset from delta() means anything.
 */
static std::vector<RowSnapshot>
snapshot_rows(Ring& ring)
{
        auto rows = std::vector<RowSnapshot>{};

        for (auto position = ring.delta(); position < ring.next(); position++) {
                auto const* const row = ring.index(position);
                g_assert_nonnull(row);

                auto snapshot = RowSnapshot{};
                snapshot.bidi_flags = row->attr.bidi_flags;
                snapshot.soft_wrapped = row->attr.soft_wrapped;
                snapshot.cells.assign(row->cells, row->cells + row->len);
                rows.push_back(std::move(snapshot));
        }

        return rows;
}

/* A marker planted on a particular character, and where that character was.
 */
struct PlantedMarker {
        char const* what;
        Ring::row_t row; /* relative to delta() */
        Ring::column_t col;
        vteunistr c;
};

template<typename Predicate>
static PlantedMarker
plant_marker(std::vector<RowSnapshot> const& rows,
             char const* what,
             Predicate&& predicate)
{
        for (size_t r = 0; r < rows.size(); r++) {
                for (size_t c = 0; c < rows[r].cells.size(); c++) {
                        if (predicate(rows, r, c)) {
                                return PlantedMarker{what,
                                                     Ring::row_t(r),
                                                     Ring::column_t(c),
                                                     rows[r].cells[c].c};
                        }
                }
        }

        g_error("the generated corpus holds no %s to plant a marker on", what);
}

/* Markers are planted on CHARACTERS only, and that restriction is the test
 * being honest rather than the test being timid. A marker past a row's end
 * keeps its column only if it happens to land past the end again after the
 * reflow, and a marker on a row that has scrolled off the top is deliberately
 * clamped to column zero. Both are documented behaviours of the ring; neither
 * is an identity, so neither belongs in a round-trip assertion.
 */
static std::vector<PlantedMarker>
plant_markers(std::vector<RowSnapshot> const& rows)
{
        auto markers = std::vector<PlantedMarker>{};

        markers.push_back(plant_marker(
                rows, "first character of a paragraph",
                [](std::vector<RowSnapshot> const& r, size_t row, size_t col) {
                        return col == 0 &&
                                (row == 0 || !r[row - 1].soft_wrapped) &&
                                r[row].cells[col].c != 0;
                }));

        markers.push_back(plant_marker(
                rows, "narrow character in the middle of a paragraph",
                [](std::vector<RowSnapshot> const& r, size_t row, size_t col) {
                        return row > 0 && col > 0 && r[row - 1].soft_wrapped &&
                                !r[row].cells[col].attr.fragment() &&
                                r[row].cells[col].attr.columns() == 1 &&
                                r[row].cells[col].c != 0;
                }));

        markers.push_back(plant_marker(
                rows, "base cell of a double width character",
                [](std::vector<RowSnapshot> const& r, size_t row, size_t col) {
                        return !r[row].cells[col].attr.fragment() &&
                                r[row].cells[col].attr.columns() == 2;
                }));

        markers.push_back(plant_marker(
                rows, "fragment cell",
                [](std::vector<RowSnapshot> const& r, size_t row, size_t col) {
                        return r[row].cells[col].attr.fragment();
                }));

        /* The last character in the ring, which is the one whose text offset
         * runs up against the head of the text stream.
         */
        for (size_t r = rows.size(); r-- > 0; ) {
                if (rows[r].cells.empty())
                        continue;
                auto const col = rows[r].cells.size() - 1;
                markers.push_back(PlantedMarker{"last character of the ring",
                                                Ring::row_t(r),
                                                Ring::column_t(col),
                                                rows[r].cells[col].c});
                break;
        }
        g_assert_cmpuint(markers.size(), ==, 5);

        return markers;
}

/* At an intermediate width all we can say about a marker is that it still names
 * the character it was planted on. Which row and column that is depends on the
 * width, and asserting a particular one would just be restating the wrap rule.
 */
static void
assert_marker_still_on_its_character(Ring& ring,
                                     VteVisualPosition const& position,
                                     PlantedMarker const& planted,
                                     Ring::column_t columns)
{
        if (position.row < 0 ||
            Ring::row_t(position.row) < ring.delta() ||
            Ring::row_t(position.row) >= ring.next()) {
                g_error("marker on the %s left the ring at width %ld: row %ld, "
                        "ring is [%lu, %lu)",
                        planted.what, columns, position.row,
                        ring.delta(), ring.next());
        }

        auto const* const row = ring.index(Ring::row_t(position.row));
        g_assert_nonnull(row);

        if (position.col < 0 || position.col >= Ring::column_t(row->len)) {
                g_error("marker on the %s is at column %ld of a %u cell row at "
                        "width %ld, so it no longer names a character",
                        planted.what, position.col, row->len, columns);
        }

        if (row->cells[position.col].c != planted.c) {
                g_error("marker on the %s names U+%04X at width %ld, U+%04X when "
                        "planted",
                        planted.what, row->cells[position.col].c, columns, planted.c);
        }
}

static void
assert_marker_came_home(Ring& ring,
                        VteVisualPosition const& position,
                        PlantedMarker const& planted)
{
        auto const row = Ring::row_t(position.row) - ring.delta();

        if (row != planted.row || position.col != planted.col) {
                g_error("marker on the %s came back to row %lu col %ld, planted "
                        "at row %lu col %ld",
                        planted.what, row, position.col, planted.row, planted.col);
        }
}

static void
assert_rows_identical(Ring& ring,
                      std::vector<RowSnapshot> const& want)
{
        if (ring.length() != Ring::row_t(want.size())) {
                g_error("%lu rows back at the home width, %zu before the excursion",
                        ring.length(), want.size());
        }

        for (size_t i = 0; i < want.size(); i++)
                assert_row_matches(ring, ring.delta() + Ring::row_t(i), want[i]);
}

/* Reflow the generated corpus through a range of widths and check, after every
 * one of them, that the paragraphs still say what they said when they were
 * written.
 *
 * Note what is NOT asserted: that the rows come back the way they were
 * appended. Rewrap does not look at how a paragraph was divided, only at its
 * total width and where it ends, so re-wrapping to the width it was written at
 * is not the identity on an arbitrarily divided corpus. Demanding that would
 * assert a promise the ring never made.
 */
static void
test_rewrap_preserves_paragraphs(void)
{
        /* Room for many more rows than the corpus can produce even at the
         * narrowest width, so that rewrap never has to drop rows off the top:
         * that is a separate behaviour and it would silently eat the text this
         * test is comparing.
         */
        auto ring = Ring{Ring::row_t(1) << 20, true /* has_streams */};
        ring.set_visible_rows(kVisibleRows);

        auto const expected = build_paragraph_corpus(ring);
        assert_actually_froze(ring);

        VteVisualPosition* markers[] = { nullptr };

        for (auto const columns : kExcursion) {
                ring.rewrap(columns, markers);
                assert_paragraphs_match(read_paragraphs(ring, columns), expected,
                                        columns);
        }
}

/* From a state rewrap itself produced, a round trip out to other widths and
 * back is the identity, on the rows and on the markers alike.
 */
static void
test_rewrap_canonical_state_is_a_fixed_point(void)
{
        auto ring = Ring{Ring::row_t(1) << 20, true /* has_streams */};
        ring.set_visible_rows(kVisibleRows);

        auto const expected = build_paragraph_corpus(ring);
        assert_actually_froze(ring);

        /* Canonicalise: after this the ring is divided into rows the way rewrap
         * divides them, which is the only division that can be a fixed point.
         */
        VteVisualPosition* no_markers[] = { nullptr };
        ring.rewrap(kHomeColumns, no_markers);

        auto const canonical = snapshot_rows(ring);
        assert_paragraphs_match(read_paragraphs(ring, kHomeColumns), expected,
                                kHomeColumns);

        auto const planted = plant_markers(canonical);
        auto positions = std::vector<VteVisualPosition>(planted.size());
        auto markers = std::vector<VteVisualPosition*>(planted.size() + 1, nullptr);
        for (size_t i = 0; i < planted.size(); i++) {
                positions[i].row = long(ring.delta() + planted[i].row);
                positions[i].col = planted[i].col;
                markers[i] = &positions[i];
        }

        for (auto const columns : kExcursion) {
                ring.rewrap(columns, markers.data());
                assert_paragraphs_match(read_paragraphs(ring, columns), expected,
                                        columns);
                for (size_t i = 0; i < planted.size(); i++)
                        assert_marker_still_on_its_character(ring, positions[i],
                                                             planted[i], columns);
        }

        assert_rows_identical(ring, canonical);
        for (size_t i = 0; i < planted.size(); i++)
                assert_marker_came_home(ring, positions[i], planted[i]);
}

/* The walker invariant: a hyperlink target is opaque bytes framed by a length.
 *
 * The attribute stream is a sequence of records, and a record with a hyperlink
 * is a record followed by the target's bytes followed by two bytes repeating
 * the target's length so that the stream can also be walked backwards. Nothing
 * separates a record from the payload before it. The only thing that keeps the
 * stream parseable is that every walk over it steps across a payload BY ITS
 * LENGTH, and the ring walks it in four places: freeze_row, the forward walk of
 * thaw_row, the backwards walk thaw_row does when it truncates, and rewrap.
 *
 * That is the whole invariant, and it is invisible in normal use because a real
 * target is a short run of printable ASCII: get one of the six steps wrong by a
 * constant and the walk still lands somewhere harmless most of the time. So the
 * targets here are chosen to be as unlike a target as the API allows while
 * still being legal - a single byte, the longest target that can be read back,
 * one whose leading bytes are a forged record, one whose trailing bytes are a
 * forged length, and one made of newlines and bytes that are not UTF-8 at all.
 * A stride that lands short or long after any of those lands ON something that
 * looks like a record, and the rows that follow come back as something else.
 *
 * Hence the shape of the assertions. Reading a target back is the direct half;
 * the sweep over every row AFTER a payload is the other half, and it is the one
 * that catches a mis-stride, because a misframed record corrupts the rows that
 * follow it rather than the payload itself.
 *
 * What is NOT asserted, deliberately:
 *
 *  - the hyperlink_idx a thawed cell reports. A cell thawed for display gets
 *    the pseudo idx VTE_HYPERLINK_IDX_TARGET_IN_STREAM, and one thawed through
 *    get_hyperlink_at_position gets a freshly allocated pool idx. Only whether
 *    a cell carries a link, and which target it names, are contractual.
 *  - anything about targets longer than VTE_HYPERLINK_TOTAL_LENGTH_MAX or
 *    containing a NUL. Freezing one of those would truncate the two byte length
 *    while appending the whole payload and corrupt the stream by design; the
 *    cap belongs to the escape sequence parser, and a char* cannot carry a NUL.
 *  - that a payload is valid UTF-8 or renderable. To the ring it is bytes.
 */

/* Rows before the first payload, so that the walks have somewhere to come from,
 * and enough rows in total that everything of interest ends up frozen with room
 * to spare. Both are checked rather than assumed: see assert_rows_are_frozen().
 */
static int const kHyperlinkLeadRows = 24;
static int const kHyperlinkCorpusRows = 320;

/* A width narrow enough that most paragraphs have to be re-divided, so the
 * rewrap walk really does cross attribute records looking for row boundaries.
 */
static Ring::column_t const kNarrowColumns = 37;

/* Every other payload's run is made to end exactly at its row's last cell, so
 * that the record flushed for it ends exactly where the next row's text begins.
 * That equality is the one case the backwards walk singles out, and the runs in
 * between cover the case where it does not hold.
 *
 * The designated one, the target the deepest walk backwards is aimed at, is the
 * first - which is why the first is also the longest payload there can be.
 */
static size_t const kBoundaryTarget = 0;

static bool
target_reaches_end_of_row(size_t target)
{
        return target % 2 == 0;
}

/* Payloads that are legal for the ring to carry - NUL free and no longer than
 * the largest length it will read back - and otherwise as hostile to a walk
 * that guesses at framing as they can be made.
 */
static std::vector<std::string>
adversarial_hyperlink_targets(void)
{
        auto targets = std::vector<std::string>{};

        /* The longest payload the ring will read back: thaw_row reads it into a
         * buffer of exactly this size and asserts the length against it, so
         * this is the value sitting on the edge of both.
         */
        auto longest = std::string{};
        while (longest.size() < VTE_HYPERLINK_TOTAL_LENGTH_MAX)
                longest.push_back(char('!' + (longest.size() % 90)));
        targets.push_back(std::move(longest));
        g_assert_cmpuint(targets.back().size(), ==, VTE_HYPERLINK_TOTAL_LENGTH_MAX);

        /* The shortest payload there is. A record, its payload and its trailing
         * length are never closer together than this, so a stride that is off
         * by a constant lands inside a neighbouring field rather than in the
         * middle of a long payload where it might go unnoticed.
         */
        targets.emplace_back("x");

        /* A payload whose leading bytes are a forged attribute record with a
         * large hyperlink length of its own. A forward stride that landed at
         * the start of the payload instead of past it would read this and jump
         * off into the middle of the stream.
         */
        auto forgery = std::string{};
        forgery.append(sizeof(gsize), '\x41');        /* where text_end_offset sits */
        forgery.append(sizeof(uint32_t), '\x42');     /* where the attr word sits */
        forgery.append(sizeof(uint64_t), '\x43');     /* where the colour triple sits */
        forgery.push_back('\xd0');                    /* where hyperlink_length sits: */
        forgery.push_back('\x07');                    /* 2000, little endian, NUL free */
        g_assert_cmpuint(forgery.size(), ==, sizeof(gsize) + sizeof(VteStreamCellAttr));
        forgery.append(200, '\x2e');
        targets.push_back(std::move(forgery));

        /* A payload whose LAST two bytes are a plausible trailing length. The
         * backwards walk reads the two bytes that follow a payload; one that
         * read the two bytes that end it would get 0x0122 from here and step
         * back to a record that is not there.
         */
        auto decoy = std::string(300, 'T');
        decoy[decoy.size() - 2] = '\x22';
        decoy[decoy.size() - 1] = '\x01';
        targets.push_back(std::move(decoy));

        /* A payload that is not text: runs of newlines, which is what separates
         * rows in the TEXT stream, and bytes that no UTF-8 decoder will accept.
         */
        auto binary = std::string{};
        for (auto i = 0; i < 40; i++) {
                binary.append(size_t(1 + i % 5), '\n');
                for (auto b = 0; b < 6; b++)
                        binary.push_back(char(0x80 + (i * 7 + b * 13) % 0x80));
        }
        targets.push_back(std::move(binary));

        for (auto const& target : targets) {
                g_assert_cmpuint(target.size(), >, 0);
                g_assert_cmpuint(target.size(), <=, VTE_HYPERLINK_TOTAL_LENGTH_MAX);
                g_assert_cmpuint(target.find('\0'), ==, std::string::npos);
        }

        return targets;
}

/* Where one payload was written into the corpus. */
struct LinkedRun {
        size_t target;            /* index into the table of targets */
        Ring::row_t row;          /* relative to delta() */
        Ring::column_t first;
        Ring::column_t last;      /* inclusive */
        Ring::column_t neighbour; /* a cell of the same row carrying no link */
};

static size_t
first_difference(char const* got,
                 std::string const& want)
{
        size_t i = 0;
        while (got[i] != '\0' && i < want.size() && got[i] == want[i])
                i++;
        return i;
}

/* Append a plain row that is guaranteed to hold at least one cell.
 *
 * An empty row contributes no attribute change, so a run that ended on the row
 * before it would be flushed to the stream only once a later row was frozen and
 * the record boundary would not be where this test says it is.
 */
static void
append_plain_row(Ring& ring,
                 std::vector<RowSnapshot>* rows)
{
        rows->push_back(append_generated_row(ring,
                                             guint8(rand_below(16)),
                                             bool(rand_below(2)),
                                             1 + rand_below(60)));
}

static void
assert_targets_round_trip(Ring& ring,
                          std::vector<std::string> const& targets,
                          std::vector<LinkedRun> const& runs,
                          std::vector<RowSnapshot> const& rows,
                          char const* when)
{
        /* Every row, in particular every row that follows a payload. A forward
         * stride that lands short or long after one reads the next record out
         * of the middle of something else, and it is these rows, not the
         * payload, that then come back wrong.
         */
        for (size_t i = 0; i < rows.size(); i++)
                assert_row_matches(ring, ring.delta() + Ring::row_t(i), rows[i]);

        for (auto const& run : runs) {
                auto const& target = targets[run.target];

                for (auto col = run.first; col <= run.last; col++) {
                        char const* got = nullptr;
                        auto const idx = ring.get_hyperlink_at_position(
                                ring.delta() + run.row, col, false, &got);

                        if (idx == 0 || got == nullptr) {
                                g_error("row %lu col %ld reports no hyperlink %s, "
                                        "but a target of %zu bytes was written there",
                                        run.row, col, when, target.size());
                        }
                        if (strcmp(got, target.c_str()) != 0) {
                                g_error("row %lu col %ld: a target of %zu bytes came "
                                        "back as %zu bytes %s, first differing at "
                                        "byte %zu",
                                        run.row, col, target.size(), strlen(got),
                                        when, first_difference(got, target));
                        }
                }

                char const* got = nullptr;
                auto const idx = ring.get_hyperlink_at_position(
                        ring.delta() + run.row, run.neighbour, false, &got);
                if (idx != 0 || got != nullptr) {
                        g_error("row %lu col %ld carries no hyperlink but reports "
                                "idx %u and a target of %zu bytes %s",
                                run.row, run.neighbour, idx,
                                got != nullptr ? strlen(got) : 0, when);
                }
        }
}

/* Count, over the whole ring, how many cells name each target - and fail if any
 * cell names something else.
 *
 * Whether a cell carries a link is read from the cell, which is contractual;
 * WHICH link it carries is asked of the ring, because a thawed cell's idx names
 * the stream rather than the pool.
 */
static std::vector<size_t>
count_linked_cells(Ring& ring,
                   std::vector<std::string> const& targets)
{
        auto counts = std::vector<size_t>(targets.size(), 0);
        auto linked = std::vector<Ring::column_t>{};

        for (auto position = ring.delta(); position < ring.next(); position++) {
                auto const* const row = ring.index(position);
                g_assert_nonnull(row);

                /* Collect the columns first: reading a target thaws a row over
                 * the one this pointer is into.
                 */
                linked.clear();
                for (unsigned int col = 0; col < row->len; col++) {
                        if (row->cells[col].attr.hyperlink_idx != 0)
                                linked.push_back(Ring::column_t(col));
                }

                for (auto const col : linked) {
                        char const* got = nullptr;
                        auto const idx = ring.get_hyperlink_at_position(position, col,
                                                                       false, &got);
                        if (idx == 0 || got == nullptr) {
                                g_error("row %lu col %ld carries a hyperlink but "
                                        "reports no target", position, col);
                        }

                        size_t t = 0;
                        while (t < targets.size() && targets[t] != got)
                                t++;
                        if (t == targets.size()) {
                                g_error("row %lu col %ld names a target of %zu bytes "
                                        "that the corpus never wrote",
                                        position, col, strlen(got));
                        }
                        counts[t]++;
                }
        }

        return counts;
}

static void
assert_counts_match(std::vector<size_t> const& got,
                    std::vector<size_t> const& want,
                    char const* when)
{
        g_assert_cmpuint(got.size(), ==, want.size());

        for (size_t t = 0; t < want.size(); t++) {
                if (got[t] != want[t]) {
                        g_error("target %zu is on %zu cells %s, on %zu before",
                                t, got[t], when, want[t]);
                }
        }
}

static void
test_hyperlink_walker_invariant(void)
{
        /* Room for far more rows than the corpus produces, so that nothing is
         * ever dropped off the top: the tail of the streams is advanced when
         * rows are discarded, and this test is about their contents.
         */
        auto ring = Ring{Ring::row_t(1) << 20, true /* has_streams */};
        ring.set_visible_rows(kVisibleRows);

        auto const targets = adversarial_hyperlink_targets();
        auto rows = std::vector<RowSnapshot>{};
        auto runs = std::vector<LinkedRun>{};

        for (auto i = 0; i < kHyperlinkLeadRows; i++)
                append_plain_row(ring, &rows);

        for (size_t t = 0; t < targets.size(); t++) {
                auto link = LinkRequest{};

                /* Allocated immediately before the cells that will name it. An
                 * idx is safe from the collector only while it is the current
                 * one or while a writable cell names it, and the collector runs
                 * inside get_hyperlink_idx itself - which is the point. Once the
                 * row is frozen the pool entry is free to go, and from then on
                 * the stream holds the only copy of the target.
                 */
                link.idx = ring.get_hyperlink_idx(targets[t].c_str());
                g_assert_cmpuint(link.idx, !=, 0);
                link.reach_end_of_row = target_reaches_end_of_row(t);

                auto row = append_generated_row(ring,
                                                guint8(rand_below(16)),
                                                link.reach_end_of_row
                                                        ? false
                                                        : bool(rand_below(2)),
                                                24 + rand_below(40),
                                                &link);
                runs.push_back(LinkedRun{t, Ring::row_t(rows.size()),
                                         link.first, link.last, link.neighbour});
                rows.push_back(std::move(row));

                /* A run's record is written only when the NEXT attribute change
                 * is frozen, and the run still current at freezing time is
                 * served from memory. Without a plain row behind it a payload
                 * would never reach the stream and this test would prove
                 * nothing.
                 */
                auto const plain = 1 + rand_below(3);
                for (auto i = 0; i < plain; i++)
                        append_plain_row(ring, &rows);
        }

        while (rows.size() < size_t(kHyperlinkCorpusRows))
                append_plain_row(ring, &rows);

        auto const boundary_row = runs[kBoundaryTarget].row;

        /* A boundary run has to end on its row's last cell, and the row after it
         * has to carry text of its own. That is what makes the flushed record's
         * text_end_offset equal to the next row's text_start_offset, which is
         * the case the backwards walk singles out. The designated one, the row
         * the deepest walk backwards stops at, is checked along with the rest.
         */
        g_assert_true(target_reaches_end_of_row(kBoundaryTarget));
        for (auto const& run : runs) {
                if (!target_reaches_end_of_row(run.target))
                        continue;

                g_assert_cmpint(run.last, ==,
                                Ring::column_t(rows[run.row].cells.size()) - 1);
                g_assert_false(rows[run.row].soft_wrapped);
                g_assert_cmpuint(rows[run.row + 1].cells.size(), >, 0);
        }

        assert_actually_froze(ring);
        for (auto const& run : runs) {
                assert_rows_are_frozen(ring, ring.delta() + run.row,
                                       ring.delta() + run.row + 1,
                                       "a payload and the row after it");
        }

        assert_targets_round_trip(ring, targets, runs, rows, "after freezing");

        /* Now walk backwards into the frozen region. Bringing a row back into
         * the writable array thaws every row above it, and each of those reads
         * the record before its own by stepping back over a payload using the
         * two byte length that follows it. The deepest probe is the row just
         * after the designated run, so the last step of all is the one that has
         * to notice a record ending exactly on a row boundary.
         */
        auto probes = std::vector<Ring::row_t>{};
        for (auto i = 0; i < 3; i++) {
                probes.push_back(boundary_row + 2 +
                                 Ring::row_t(rand_below(int(rows.size()) -
                                                        int(boundary_row) - 2)));
        }
        std::sort(probes.begin(), probes.end(), std::greater<Ring::row_t>{});
        for (auto const probe : probes)
                g_assert_nonnull(ring.index_writable(ring.delta() + probe));

        auto const thawed_end = ring.next();
        auto const thawed_start = ring.delta() + boundary_row + 1;
        g_assert_nonnull(ring.index_writable(thawed_start));

        /* Push all of it back out. The writable array holds fewer rows than
         * twice the number just thawed, and once it is full every further
         * append freezes exactly one row, so this many appends is more than
         * enough - and the assertion below is what actually checks it.
         */
        auto const refreeze = 2 * (thawed_end - thawed_start) + 128;
        for (Ring::row_t i = 0; i < refreeze; i++)
                append_plain_row(ring, &rows);

        assert_rows_are_frozen(ring, thawed_start, thawed_end - 1,
                               "the region that was brought back into memory");

        /* Everything that was thawed has been written to the streams a second
         * time, from attributes the backwards walk reconstructed. Its contents
         * did not change on the way, so every reference copy still holds.
         */
        assert_targets_round_trip(ring, targets, runs, rows, "after a re-freeze");

        /* Finally rewrap, which walks the attribute stream a third way: not row
         * by row but paragraph by paragraph, stepping over payloads as it goes.
         * Rows and columns do not survive a reflow, so the check is that each
         * target is still on exactly the cells it was on, and no target the
         * corpus never wrote appears anywhere.
         */
        auto expected = std::vector<size_t>(targets.size(), 0);
        for (auto const& run : runs)
                expected[run.target] += size_t(run.last - run.first + 1);
        assert_counts_match(count_linked_cells(ring, targets), expected,
                            "before rewrapping");

        VteVisualPosition* no_markers[] = { nullptr };

        ring.rewrap(kNarrowColumns, no_markers);
        assert_counts_match(count_linked_cells(ring, targets), expected,
                            "after narrowing");

        ring.rewrap(kHomeColumns, no_markers);
        assert_counts_match(count_linked_cells(ring, targets), expected,
                            "back at the home width");
}

/* Differential fuzzing: the ring's whole mutating surface against a model that
 * is right by construction.
 *
 * Every test above pins one property of one path down with a script somebody
 * wrote. This one is the opposite bet. It drives the ring through random
 * sequences of every mutating call it publishes, interleaved with the freezing,
 * thawing and rewrapping those calls trigger, and after every single call it
 * compares the ring against the simplest correct description of what a
 * scrollback ring is: a deque of rows, plus one number saying where the first
 * of them sits.
 *
 * The interleaving is the point. resize, shrink, insert, remove,
 * drop_scrollback and reset each move the boundary between the rows held in
 * memory and the rows held in the streams, and each moves it differently: two
 * drop rows off the front and advance delta, one drops them off the back and
 * leaves delta alone, several thaw rows back out of the streams and truncate
 * them, one throws the streams away. Mixing them reaches states no hand
 * written script would think to write down, and one of those states is the
 * interesting one: a row that was frozen, thawed back into memory, rewritten,
 * and then frozen a SECOND time. Freezing appends to the streams while thawing
 * truncates them, so that cycle is where the two can disagree about where the
 * next row's record belongs, and nothing about it is visible until some later
 * read comes back with a different row than the one that was written.
 *
 * The model deliberately knows nothing about wrapping - it is a deque, not a
 * second implementation of the ring, and that is what makes a disagreement
 * decidable rather than an argument between two guesses. Where the ring is
 * entitled to reorganise rows, which is rewrap and only rewrap, the model does
 * not try to predict the result: the check there is at the paragraph level,
 * which reflowing is required to preserve, and the model is then resynced from
 * the ring. Predicting the row division would be re-implementing the wrap
 * arithmetic inside the test.
 *
 * The sequences are bounded and seeded rather than run under a coverage guided
 * fuzzer, so this is an ordinary unit test that always terminates; a failure
 * reports its seed and --seed= replays it exactly.
 */

/* Room for far more rows than any sequence can produce, so that the ring never
 * drops a row to stay under its maximum. Capacity behaviour is not being left
 * untested, it is being tested deliberately and in isolation by the resize and
 * shrink operations - which is the difference between exercising it and
 * having it happen underneath an assertion about something else.
 */
static Ring::row_t const kFuzzMaxRows = Ring::row_t(1) << 20;

/* Enough rows appended before the sequence starts that the writable window is
 * already full and the ring is already using the streams. Otherwise the first
 * few dozen operations would run entirely in memory and prove nothing about
 * serialisation. Checked rather than assumed: see the non-vacuity guard at the
 * end of the test.
 */
static int const kFuzzWarmupRows = 96;

/* Narrow rows, because a rewrap to a narrow width turns each of them into
 * several and the sequences are long.
 */
static int const kFuzzMaxRowWidth = 20;

/* How often the whole ring is compared against the model. Every operation is
 * checked for its delta and length; the full cell for cell sweep is periodic
 * because it costs a thaw per row.
 */
static int const kFuzzSweepInterval = 16;

enum FuzzOp {
        FUZZ_APPEND,
        FUZZ_EDIT_ROW,
        FUZZ_INSERT_ROW,
        FUZZ_REMOVE_ROW,
        FUZZ_RESIZE_SMALL,
        FUZZ_SHRINK,
        FUZZ_DROP_SCROLLBACK,
        FUZZ_RESET,
        FUZZ_SET_VISIBLE_ROWS,
        FUZZ_THAW_DEEP,
        FUZZ_REWRAP,
        FUZZ_OP_COUNT
};

static char const* const kFuzzOpNames[FUZZ_OP_COUNT] = {
        "append", "edit", "insert", "remove", "resize", "shrink",
        "drop-scrollback", "reset", "set-visible-rows", "thaw", "rewrap",
};

/* Appending dominates because that is what a terminal does; everything else is
 * frequent enough to keep colliding with it. The weights are a coverage knob
 * and nothing depends on their exact values - but they were not guessed. They
 * are set so that the ring's length settles well above the writable array's
 * 32 rows, because below that nothing is ever frozen and every assertion here
 * would be checking an in-memory array against another in-memory array.
 */
static int const kFuzzOpWeights[FUZZ_OP_COUNT] = {
        45, 12, 6, 6, 3, 3, 3, 1, 4, 5, 8,
};

/* How many rows to keep out of @length, for the operations that discard rows.
 *
 * Uniform over the whole range would be the obvious draw, and it is wrong here:
 * drawn every twentieth operation it holds the ring down to a handful of rows,
 * which never leave the writable array, so the compressed tier this whole file
 * is about would go untested behind assertions that all pass. So the draw is
 * bimodal - usually a small bite out of the end, occasionally the full range,
 * which still reaches one row and everything in between.
 */
static size_t
fuzz_keep_count(size_t length)
{
        g_assert_cmpuint(length, >, 0);

        if (rand_below(6) == 0)
                return size_t(1 + rand_below(int(length)));

        return length - size_t(rand_below(int(length / 8) + 1));
}

/* The reference model: row i of @rows is the ring's row at @delta + i. */
struct RingModel {
        std::deque<RowSnapshot> rows;
        Ring::row_t delta;
};

static FuzzOp
pick_fuzz_op(void)
{
        auto total = 0;
        for (auto const weight : kFuzzOpWeights)
                total += weight;

        auto roll = rand_below(total);
        for (auto op = 0; op < FUZZ_OP_COUNT; op++) {
                roll -= kFuzzOpWeights[op];
                if (roll < 0)
                        return FuzzOp(op);
        }

        g_assert_not_reached();
}

static std::string
fuzz_context(int sequence,
             int op,
             char const* name)
{
        char buf[128];
        g_snprintf(buf, sizeof(buf), "at sequence %d step %d (%s)",
                   sequence, op, name);
        return std::string(buf);
}

static RowSnapshot
generate_fuzz_row(VteRowData* row)
{
        auto const bidi_flags = guint8(rand_below(16));
        auto const soft_wrapped = bool(rand_below(2));
        auto const width = rand_below(kFuzzMaxRowWidth);
        auto const ascii_only = rand_below(4) == 0;

        return generate_row_into(row, bidi_flags, soft_wrapped, width, ascii_only,
                                 nullptr);
}

/* The model's rows read as paragraphs, by the same rule rewrap uses: rows are
 * joined while they are soft wrapped, fragment cells are geometry rather than
 * content and are dropped, and a paragraph carries its FIRST row's bidi flags
 * because that is the one rewrap keeps.
 *
 * The one subtlety is the last paragraph. A row's text reaches the stream as
 * its characters plus, if it is hard wrapped, a newline - so a paragraph at the
 * end of the ring that is soft wrapped AND has no characters contributes not
 * one byte, and rewrap, which walks the text stream, cannot know it was ever
 * there. Dropping such a paragraph here is not papering over a disagreement: it
 * is the model declining to claim something the ring never stored.
 */
static std::vector<ParagraphSnapshot>
model_paragraphs(RingModel const& model)
{
        auto paragraphs = std::vector<ParagraphSnapshot>{};
        auto open = false;

        for (auto const& row : model.rows) {
                if (!open) {
                        paragraphs.push_back(ParagraphSnapshot{});
                        paragraphs.back().bidi_flags = row.bidi_flags;
                        open = true;
                }

                for (auto const& cell : row.cells) {
                        if (!cell.attr.fragment())
                                paragraphs.back().cells.push_back(cell);
                }

                if (!row.soft_wrapped)
                        open = false;
        }

        if (open && paragraphs.back().cells.empty())
                paragraphs.pop_back();

        return paragraphs;
}

/* Compare every row of the ring against the model, and report whether any of
 * them came out of the streams.
 *
 * The ring publishes no accessor for the writable boundary but it does publish
 * a consequence of it, as rows_are_frozen() explains: a frozen row is answered
 * out of the single row the ring thaws into, so two consecutive rows answering
 * with the same address are two frozen rows.
 */
static bool
sweep_against_model(Ring& ring,
                    RingModel const& model,
                    char const* when)
{
        auto const* previous = static_cast<VteRowData const*>(nullptr);
        auto froze = false;

        for (size_t i = 0; i < model.rows.size(); i++) {
                auto const position = model.delta + Ring::row_t(i);

                auto const* const row = ring.index(position);
                g_assert_nonnull(row);
                if (row == previous)
                        froze = true;
                previous = row;

                assert_row_matches(ring, position, model.rows[i], when);
        }

        return froze;
}

static void
resync_model_from_ring(Ring& ring,
                       RingModel& model)
{
        auto const rows = snapshot_rows(ring);

        model.rows.assign(rows.begin(), rows.end());
        model.delta = ring.delta();
}

/* Reflow to a random width with up to two markers riding along, check what a
 * reflow is required to preserve, and only then resync the model.
 *
 * Markers are planted on cells the model knows are there and are characters:
 * not on a fragment, not on an empty cell, never past a row's end and never on
 * a row outside the ring. Every one of those is either a caller error the ring
 * answers with an assertion or a documented clamp, so a marker on one of them
 * would be asserting something rewrap never promised.
 *
 * What is checked afterwards is the invariant, not the outcome: the paragraphs
 * still say what they said, no row is wider than the width asked for, no wide
 * character was sawn in half by the wrap, and each marker still names the
 * character it was planted on. Which row and column that character ended up at
 * is the wrap rule's business, and restating it here would only assert that the
 * test and the ring compute the same thing.
 */
static void
fuzz_rewrap(Ring& ring,
            RingModel& model,
            char const* when)
{
        /* Sweep before reflowing, not only on the periodic schedule. After the
         * reflow the ring's division into rows is a new one and the old one is
         * gone, so this is the last moment at which the rows the model knows
         * about can be compared at all - everything afterwards is necessarily
         * the weaker paragraph level check. It also means a ring that has
         * already diverged is caught here rather than handed to rewrap, which
         * walks the row records as a linked structure and can spin on a
         * corrupt one instead of returning something wrong.
         */
        sweep_against_model(ring, model, when);

        /* Not 1 or 2: at width 1 a double width character cannot fit at all and
         * the wrap rule emits empty rows rather than dropping it, which is legal
         * but pathological.
         */
        auto const columns = Ring::column_t(3 + rand_below(130));

        auto candidates = std::vector<std::pair<size_t, size_t>>{};
        for (size_t r = 0; r < model.rows.size(); r++) {
                for (size_t c = 0; c < model.rows[r].cells.size(); c++) {
                        auto const& cell = model.rows[r].cells[c];
                        if (!cell.attr.fragment() && cell.c != 0)
                                candidates.emplace_back(r, c);
                }
        }

        auto planted = std::vector<PlantedMarker>{};
        auto const wanted = candidates.empty() ? 0 : rand_below(3);
        for (auto i = 0; i < wanted; i++) {
                auto const& pick = candidates[rand_below(int(candidates.size()))];
                planted.push_back(PlantedMarker{"a character the model planted a marker on",
                                                Ring::row_t(pick.first),
                                                Ring::column_t(pick.second),
                                                model.rows[pick.first].cells[pick.second].c});
        }

        auto positions = std::vector<VteVisualPosition>(planted.size());
        auto markers = std::vector<VteVisualPosition*>(planted.size() + 1, nullptr);
        for (size_t i = 0; i < planted.size(); i++) {
                positions[i].row = long(model.delta + planted[i].row);
                positions[i].col = planted[i].col;
                markers[i] = &positions[i];
        }

        ring.rewrap(columns, markers.data());

        /* The ring may legitimately end in a soft wrapped row here: the fuzz
         * generates rows independently, so the last paragraph is often still
         * open. The corpora above never do, which is why they hold that against
         * the ring and this does not.
         */
        assert_paragraphs_match(read_paragraphs(ring, columns, true /* may end open */),
                                model_paragraphs(model), columns);

        for (size_t i = 0; i < planted.size(); i++)
                assert_marker_still_on_its_character(ring, positions[i], planted[i],
                                                     columns);

        resync_model_from_ring(ring, model);
}

static void
test_fuzz_differential(void)
{
        auto const sequences = g_test_thorough() ? 128 : 32;
        auto const steps = g_test_thorough() ? 800 : 200;
        auto sequences_that_froze = 0;

        for (auto sequence = 0; sequence < sequences; sequence++) {
                auto ring = Ring{kFuzzMaxRows, true /* has_streams */};
                auto model = RingModel{};

                model.delta = ring.delta();
                ring.set_visible_rows(1 + rand_below(8));

                for (auto i = 0; i < kFuzzWarmupRows; i++)
                        model.rows.push_back(generate_fuzz_row(ring.append(0)));

                auto froze = false;

                for (auto step = 0; step < steps; step++) {
                        auto const op = pick_fuzz_op();
                        auto const context = fuzz_context(sequence, step,
                                                          kFuzzOpNames[op]);
                        auto const length = model.rows.size();

                        switch (op) {
                        case FUZZ_APPEND: {
                                /* A burst, because a terminal scrolls in runs
                                 * and a run is what pushes rows out of the
                                 * writable window.
                                 */
                                auto const count = 1 + rand_below(8);
                                for (auto i = 0; i < count; i++)
                                        model.rows.push_back(generate_fuzz_row(ring.append(0)));
                                break;
                        }

                        case FUZZ_EDIT_ROW: {
                                if (length == 0)
                                        break;

                                /* Biased towards the oldest rows, which are the
                                 * frozen ones: rewriting a row that had to be
                                 * thawed first, and will be frozen again after,
                                 * is the whole reason this operation exists.
                                 */
                                auto const span = rand_below(2) ?
                                        std::max(size_t(1), length / 4) : length;
                                auto const i = size_t(rand_below(int(span)));

                                auto* const row = ring.index_writable(model.delta +
                                                                      Ring::row_t(i));
                                g_assert_nonnull(row);
                                model.rows[i] = generate_fuzz_row(row);
                                break;
                        }

                        case FUZZ_INSERT_ROW: {
                                auto const i = size_t(rand_below(int(length) + 1));
                                auto const bidi_flags = guint8(rand_below(16));

                                ring.insert(model.delta + Ring::row_t(i), bidi_flags);

                                /* insert() clears the row, so the model's copy
                                 * is an empty, hard wrapped row carrying only
                                 * the bidi flags it was given.
                                 */
                                auto inserted = RowSnapshot{};
                                inserted.bidi_flags = bidi_flags;
                                inserted.soft_wrapped = false;
                                model.rows.insert(model.rows.begin() +
                                                  std::ptrdiff_t(i),
                                                  std::move(inserted));
                                break;
                        }

                        case FUZZ_REMOVE_ROW: {
                                /* Deliberately allowed to aim outside the ring:
                                 * remove() guards itself with contains() and is
                                 * documented to do nothing there, so that is
                                 * behaviour worth pinning rather than caller
                                 * error worth avoiding.
                                 */
                                auto const low = model.delta >= 2 ?
                                        model.delta - 2 : Ring::row_t(0);
                                auto const high = model.delta + Ring::row_t(length) + 2;
                                auto const position = low +
                                        Ring::row_t(rand_below(int(high - low) + 1));

                                ring.remove(position);

                                if (position >= model.delta &&
                                    position < model.delta + Ring::row_t(length)) {
                                        model.rows.erase(model.rows.begin() +
                                                         std::ptrdiff_t(position - model.delta));
                                }
                                break;
                        }

                        case FUZZ_RESIZE_SMALL: {
                                if (length == 0)
                                        break;

                                /* Lowering the maximum drops rows off the FRONT
                                 * and advances delta. The maximum is put back
                                 * immediately so that a later rewrap can never
                                 * be the thing that truncates: rewrap dropping
                                 * rows is a separate behaviour, and letting it
                                 * happen underneath the paragraph check would
                                 * silently eat the text being compared.
                                 */
                                auto const keep = fuzz_keep_count(length);

                                ring.resize(Ring::row_t(keep));
                                ring.resize(kFuzzMaxRows);

                                auto const dropped = length - keep;
                                model.rows.erase(model.rows.begin(),
                                                 model.rows.begin() + std::ptrdiff_t(dropped));
                                model.delta += Ring::row_t(dropped);
                                break;
                        }

                        case FUZZ_SHRINK: {
                                if (length == 0)
                                        break;

                                /* Unlike resize, shrink drops rows off the BACK
                                 * and leaves delta where it was, walking
                                 * backwards through the frozen region a row at a
                                 * time to do it.
                                 */
                                auto const keep = fuzz_keep_count(length);

                                ring.shrink(Ring::row_t(keep));
                                model.rows.resize(keep);
                                break;
                        }

                        case FUZZ_DROP_SCROLLBACK: {
                                /* next() is a legal argument and means drop
                                 * everything, so it has to stay reachable even
                                 * though the usual draw keeps a row.
                                 */
                                auto const i = length == 0 || rand_below(12) == 0 ?
                                        length : length - fuzz_keep_count(length);
                                auto const position = model.delta + Ring::row_t(i);

                                ring.drop_scrollback(position);

                                model.rows.erase(model.rows.begin(),
                                                 model.rows.begin() + std::ptrdiff_t(i));
                                model.delta = position;
                                break;
                        }

                        case FUZZ_RESET:
                                ring.reset();

                                /* Row numbering does not restart: the ring keeps
                                 * counting from where it was, so delta advances
                                 * by everything that was in it.
                                 */
                                model.delta += Ring::row_t(length);
                                model.rows.clear();
                                break;

                        case FUZZ_SET_VISIBLE_ROWS:
                                ring.set_visible_rows(Ring::row_t(1 + rand_below(8)));
                                break;

                        case FUZZ_THAW_DEEP:
                                if (length == 0)
                                        break;

                                /* Pull the entire scrollback back into memory,
                                 * which is the deepest walk backwards through
                                 * the streams the ring ever does.
                                 */
                                g_assert_nonnull(ring.index_writable(model.delta));
                                break;

                        case FUZZ_REWRAP:
                                fuzz_rewrap(ring, model, context.c_str());
                                break;

                        case FUZZ_OP_COUNT:
                                g_assert_not_reached();
                        }

                        if (ring.delta() != model.delta) {
                                g_error("delta %lu %s, model says %lu",
                                        ring.delta(), context.c_str(), model.delta);
                        }
                        if (ring.length() != Ring::row_t(model.rows.size())) {
                                g_error("length %lu %s, model says %zu",
                                        ring.length(), context.c_str(),
                                        model.rows.size());
                        }

                        if (step % kFuzzSweepInterval == kFuzzSweepInterval - 1)
                                froze |= sweep_against_model(ring, model, context.c_str());
                }

                auto const context = fuzz_context(sequence, steps, "end of sequence");
                froze |= sweep_against_model(ring, model, context.c_str());

                if (froze)
                        sequences_that_froze++;
        }

        /* Guard against the whole thing quietly proving nothing. If a change to
         * the ring's sizing kept every sequence inside the writable array, every
         * assertion above would still pass while never touching a stream.
         */
        if (sequences_that_froze * 2 <= sequences) {
                g_error("only %d of %d sequences pushed rows out of the writable "
                        "window - this test would pass without exercising the "
                        "streams at all",
                        sequences_that_froze, sequences);
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
        g_test_add_func("/vte/ring/roundtrip/rewrap/paragraph-stability",
                        test_rewrap_preserves_paragraphs);
        g_test_add_func("/vte/ring/roundtrip/rewrap/canonical-identity",
                        test_rewrap_canonical_state_is_a_fixed_point);
        g_test_add_func("/vte/ring/roundtrip/hyperlink/walker-invariant",
                        test_hyperlink_walker_invariant);
        g_test_add_func("/vte/ring/roundtrip/fuzz/differential",
                        test_fuzz_differential);

        return g_test_run();
}
