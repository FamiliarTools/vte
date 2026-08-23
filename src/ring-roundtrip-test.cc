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

/* Append one generated row to @ring with the given wrapping and bidi flags,
 * and return the reference copy.
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
append_generated_row(Ring& ring,
                     guint8 bidi_flags,
                     bool soft_wrapped)
{
        auto snapshot = RowSnapshot{};
        snapshot.bidi_flags = bidi_flags;
        snapshot.soft_wrapped = soft_wrapped;

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
                Ring::column_t columns)
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

        if (open) {
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

        return g_test_run();
}
