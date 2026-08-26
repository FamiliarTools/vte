/*
 * Copyright (C) 2002,2009,2010 Red Hat, Inc.
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
 *
 * Red Hat Author(s): Nalin Dahyabhai, Behdad Esfahbod
 */

#include "config.h"

#include "debug.hh"
#include "ring.hh"
#include "vterowdata.hh"

#include <string.h>

#if WITH_SIXEL

#include "cxx-utils.hh"

#include <algorithm>


#endif /* WITH_SIXEL */

/*
 * Copy the common attributes from VteCellAttr to VteStreamCellAttr or vice versa.
 */
static inline void
_attrcpy (void *dst, void *src)
{
        memcpy(dst, src, VTE_CELL_ATTR_COMMON_BYTES);
}

using namespace vte::base;

/*
 * VteRing: A buffer ring
 */

#if VTE_DEBUG
void
Ring::validate() const
{
	_vte_debug_print(vte::debug::category::RING,
                         "Delta = {}, Length = {}, Next = {}, Max = {}, Writable = {}",
                         m_start, m_end - m_start, m_end,
                         m_max, m_end - m_writable);

	vte_assert_cmpuint(m_start, <=, m_writable);
	vte_assert_cmpuint(m_writable, <=, m_end);

	vte_assert_cmpuint(m_end - m_start, <=, m_max);
	vte_assert_cmpuint(m_end - m_writable, <=, m_mask);

#if WITH_SIXEL
        validate_images();
#endif
}

#else
#define validate(...) do { } while(0)
#endif

#if WITH_SIXEL

void
Ring::validate_images() const
{
        /* The image maps are keyed by row number, and a row number only means
         * anything for as long as the ring still holds that row. Nothing about
         * freeing a row tells the maps, so every path that destroys rows has to
         * say so itself, and a path that forgets is invisible: the ring stays
         * self-consistent, the images just quietly stop being reachable. Checking
         * it here turns "audit the ring by reading it" into "run any workload".
         *
         * Free when no image is resident, which is very nearly always: the walk
         * is over the maps, and the mirror already says whether they are empty.
         */
        if (!m_has_images)
                return;

        /* The two maps hold the same images, one keyed by priority and one by
         * top row, so their sizes cannot drift apart. */
        vte_assert_cmpuint(m_image_by_top_map.size(), ==, m_image_map.size());

        auto memory_used = size_t{0};

        for (auto const& [priority, image] : m_image_map) {
                /* Every resident image still covers a row the ring holds. Its top
                 * may well be above m_start - an image straddling the start is half
                 * in the scrollback and stays - but its bottom cannot be, or none of
                 * its rows exists any more and nothing can ever draw it, erase it or
                 * move it again.
                 */
                vte_assert_cmpint(long(image->get_bottom()), >=, long(m_start));

                /* The by-top map is an index into the priority map: same images,
                 * each filed under the row it actually starts at. A stale key makes
                 * drop_images_before()'s early exit skip a live entry, and makes
                 * unlink_image_from_top_map() miss the entry it is unlinking.
                 */
                auto const [begin, end] = m_image_by_top_map.equal_range(image->get_top());
                auto found = false;
                for (auto it = begin; it != end; ++it)
                        found = found || it->second == image.get();
                vte_assert_true(found);

                memory_used += image_cost(image.get());
        }

        /* What the GC spends its budget against is what the resident images
         * actually hold, so that a live image is never evicted to make room for
         * pixels that are already freed or that nobody can reach.
         */
        vte_assert_cmpuint(memory_used, ==, m_image_fast_memory_used);

        validate_image_cells();
}

/*
 * The cells are what an image IS: the draw walks them, and scrolling,
 * insertion, deletion and rewrap move a picture by moving them without knowing
 * that images exist. So the maps agreeing with themselves says nothing about
 * where the picture actually is; only the cells can say that.
 *
 * Checked in one direction, because only one direction is an invariant. Every
 * cell that names an image is that image's and sits where it says it sits. A
 * cell INSIDE an image's rectangle may legitimately not be the image's at all,
 * either because a write took that cell back - a partial erase keeps the rest
 * of the picture - or because the row was too short to be stamped when the
 * image was placed.
 */
void
Ring::validate_image_cells() const
{
        for (auto r = m_writable; r < m_end; r++) {
                auto const* const row = get_writable_index(r);

                for (auto c = 0; c < row->len; c++) {
                        auto const& cell = row->cells[c];
                        if (!cell.attr.image())
                                continue;

                        /* The cell holds the image rather than the text that
                         * was there, as one whole cell. Text extraction appends
                         * each non-fragment cell's own c, so a cell still
                         * holding its old character copies as that character,
                         * and a fragment copies as nothing at all.
                         */
                        vte_assert_cmpuint(cell.c, ==, VTE_OBJECT_REPLACEMENT_CHARACTER);
                        vte_assert_cmpuint(cell.attr.columns(), ==, 1);
                        vte_assert_false(cell.attr.fragment());

                        auto const ref = cell.attr.image_ref();
                        auto const* const image = m_image_pool.lookup(ref);

                        /* An id that resolves to nothing is a normal outcome:
                         * the image has been freed and the cell is on its way
                         * out with its row. Nothing can be asked of a picture
                         * that is gone.
                         */
                        if (image == nullptr)
                                continue;

                        /* An image is exempt from every rule that moves images
                         * for as long as its own emission burst is running, so
                         * its rectangle is deliberately behind its cells until
                         * the burst ends.
                         */
                        if (image == m_placing_image)
                                continue;

                        /* The tile coordinate names a piece of THIS picture. */
                        vte_assert_cmpint(long(ref.tile_row()), <, long(image->get_height()));
                        vte_assert_cmpint(long(ref.tile_col()), <, long(image->get_width()));

                        /* And the cell sits exactly where that piece belongs.
                         * This is the anchoring itself. A path that moves cells
                         * without moving the image, or an image without its
                         * cells, leaves a rectangle naming rows and columns the
                         * picture no longer covers - and the rectangle is what
                         * decides which images an erase can reach, which ones
                         * the scrollback has taken, and which ones a reflow
                         * tears apart.
                         */
                        vte_assert_cmpint(long(r), ==,
                                          long(image->get_top()) + long(ref.tile_row()));
                        vte_assert_cmpint(long(c), ==,
                                          long(image->get_left()) + long(ref.tile_col()));
                }
        }
}

#endif /* WITH_SIXEL */

Ring::Ring(row_t max_rows,
           bool has_streams)
        : m_max{MAX(max_rows, 3)},
          m_has_streams{has_streams},
          m_last_attr{basic_cell.attr}
{
	_vte_debug_print(vte::debug::category::RING, "New ring {}", (void*)this);

	m_array = (VteRowData* ) g_malloc0 (sizeof (m_array[0]) * (m_mask + 1));

	if (has_streams) {
		m_attr_stream = _vte_file_stream_new ();
		m_text_stream = _vte_file_stream_new ();
		m_row_stream = _vte_file_stream_new ();
		m_image_stream = _vte_file_stream_new ();
	} else {
		m_attr_stream = m_text_stream = m_row_stream = nullptr;
		m_image_stream = nullptr;
	}

	m_utf8_buffer = g_string_sized_new (128);

	_vte_row_data_init (&m_cached_row);

        m_hyperlinks = g_ptr_array_new();
        auto empty_str = g_string_new_len("", 0);
        g_ptr_array_add(m_hyperlinks, empty_str);

	validate();
}

Ring::~Ring()
{
	for (size_t i = 0; i <= m_mask; i++)
		_vte_row_data_fini (&m_array[i]);

	g_free (m_array);

	if (m_has_streams) {
		g_object_unref (m_attr_stream);
		g_object_unref (m_text_stream);
		g_object_unref (m_row_stream);
		g_object_unref (m_image_stream);
	}

	g_string_free (m_utf8_buffer, TRUE);

        for (size_t i = 0; i < m_hyperlinks->len; i++)
                g_string_free (hyperlink_get(i), TRUE);
        g_ptr_array_free (m_hyperlinks, TRUE);

	_vte_row_data_fini(&m_cached_row);
}

#define SET_BIT(buf, n) buf[(n) / 8] |= (1 << ((n) % 8))
#define GET_BIT(buf, n) ((buf[(n) / 8] >> ((n) % 8)) & 1)

/*
 * Do a round of garbage collection. Hyperlinks that no longer occur in the ring are wiped out.
 */
void
Ring::hyperlink_gc()
{
        row_t i, j;
        hyperlink_idx_t idx;
        VteRowData* row;
        char *used;

        _vte_debug_print(vte::debug::category::HYPERLINK,
                         "hyperlink: GC starting (highest used idx is {})",
                         m_hyperlink_highest_used_idx);

        m_hyperlink_maybe_gc_counter = 0;

        if (m_hyperlink_highest_used_idx == 0) {
                _vte_debug_print(vte::debug::category::HYPERLINK,
                                 "hyperlink: GC done (no links at all, nothing to do)");
                return;
        }

        /* One bit for each idx to see if it's used. */
        used = (char *) g_malloc0 (m_hyperlink_highest_used_idx / 8 + 1);

        /* A few special values not to be garbage collected. */
        SET_BIT(used, m_hyperlink_current_idx);
        SET_BIT(used, m_hyperlink_hover_idx);
        SET_BIT(used, m_last_attr.hyperlink_idx_or_none());

        for (i = m_writable; i < m_end; i++) {
                row = get_writable_index(i);
                for (j = 0; j < row->len; j++) {
                        /* An image cell's m_link holds an image reference, not a
                         * hyperlink index. It indexes nothing in `used`, whose size
                         * is bounded by m_hyperlink_highest_used_idx, so passing one
                         * to SET_BIT would be an out-of-bounds heap write.
                         */
                        if (row->cells[j].attr.image())
                                continue;

                        idx = row->cells[j].attr.hyperlink_idx();
                        SET_BIT(used, idx);
                }
        }

        for (idx = 1; idx <= m_hyperlink_highest_used_idx; idx++) {
                if (!GET_BIT(used, idx) && hyperlink_get(idx)->len != 0) {
                        _vte_debug_print(vte::debug::category::HYPERLINK,
                                         "hyperlink: GC purging link {} to id;uri=\"{}\"",
                                         idx,
                                         hyperlink_get(idx)->str);
                        /* Wipe out the ID and URI itself so it doesn't linger on in the memory for a long time */
                        memset(hyperlink_get(idx)->str, 0, hyperlink_get(idx)->len);
                        g_string_truncate (hyperlink_get(idx), 0);
                }
        }

        while (m_hyperlink_highest_used_idx >= 1 && hyperlink_get(m_hyperlink_highest_used_idx)->len == 0) {
               m_hyperlink_highest_used_idx--;
        }

        _vte_debug_print(vte::debug::category::HYPERLINK,
                         "hyperlink: GC done (highest used idx is now {})",
                         m_hyperlink_highest_used_idx);

        g_free (used);
}

/*
 * Cumulate the given value, and do a GC when 65536 is reached.
 */
void
Ring::hyperlink_maybe_gc(row_t increment)
{
        m_hyperlink_maybe_gc_counter += increment;

        _vte_debug_print(vte::debug::category::HYPERLINK,
                         "hyperlink: maybe GC, counter at {}",
                         m_hyperlink_maybe_gc_counter);

        if (m_hyperlink_maybe_gc_counter >= 65536)
                hyperlink_gc();
}

#if WITH_SIXEL

void
Ring::image_gc_region() noexcept
{
        cairo_region_t *region = cairo_region_create();

        for (auto rit = m_image_map.rbegin();
             rit != m_image_map.rend();
             ) {
                auto const& image = rit->second;
                auto const rect = cairo_rectangle_int_t{image->get_left(),
                                                        image->get_top(),
                                                        image->get_width(),
                                                        image->get_height()};

                if (cairo_region_contains_rectangle(region, &rect) == CAIRO_REGION_OVERLAP_IN) {
                        /* vte::image::Image has been completely overdrawn; delete it */

                        m_image_fast_memory_used -= image_cost(image.get());

                        /* Apparently this is the cleanest way to erase() with a reverse iterator... */
                        /* Unlink the image from m_image_by_top_map, then erase it from m_image_map */
                        note_image_freed(image.get());
                        unlink_image_from_top_map(image.get());
                        rit = image_map_type::reverse_iterator{m_image_map.erase(std::next(rit).base())};
                        continue;
                }

                cairo_region_union_rectangle(region, &rect);
                ++rit;
        }

        cairo_region_destroy(region);

        sync_has_images();
}

/*
 * Mark and sweep the image id space.
 *
 * Marks every id still named by a cell in the writable rows, then releases
 * the retired ids nothing marked. Rows already frozen into the stream are
 * deliberately NOT walked: the same restriction hyperlink_gc() operates
 * under, and it is sound for the same reason only once frozen rows carry
 * their image reference in the stream rather than in the pool. Until that
 * exists, this is conservative in the safe direction - it can only fail to
 * reclaim an id, never reclaim one too early.
 */
bool
Ring::find_image_anchor(uint32_t pool_id,
                        row_t* out_row,
                        column_t* out_col) const noexcept
{
        if (pool_id == vte::image::k_ref_pool_id_none)
                return false;

        for (auto i = m_writable; i < m_end; i++) {
                auto const row = get_writable_index(i);
                for (auto j = 0; j < row->len; j++) {
                        auto const& attr = row->cells[j].attr;
                        if (!attr.image())
                                continue;

                        auto const ref = attr.image_ref();
                        if (ref.pool_id() != pool_id)
                                continue;
                        if (ref.tile_row() != 0 || ref.tile_col() != 0)
                                continue;

                        *out_row = i;
                        *out_col = j;
                        return true;
                }
        }

        return false;
}

void
Ring::stamp_image_row(row_t position,
                      column_t left,
                      column_t columns,
                      uint32_t image_row) noexcept
{
        if (m_placing_image == nullptr)
                return;

        auto const id = m_placing_image->get_pool_id();
        if (id == vte::image::k_ref_pool_id_none)
                return;

        if (position < m_writable || position >= m_end)
                return;

        auto const row = get_writable_index(position);

        for (auto col = std::max(left, column_t{0}); col < left + columns; col++) {
                if (col >= row->len)
                        break;

                auto const tile_col = uint32_t(col - left);

                /* Refuse rather than store a masked reference: a truncated
                 * tile coordinate draws the wrong part of the image.
                 */
                if (!vte::image::Ref::fits(id, image_row, tile_col))
                        continue;

                row->cells[col].attr.set_image_ref(vte::image::Ref{id, image_row, tile_col});

                /* The cell is the image's now, so it holds the object
                 * replacement character rather than whatever text the erase
                 * left behind. It is never drawn as a glyph - the image is
                 * drawn instead - and text extraction reports it as blank.
                 */
                row->cells[col].c = VTE_OBJECT_REPLACEMENT_CHARACTER;
        }
}

void
Ring::sweep_image_pool() noexcept
{
        m_image_pool.sweep_begin();

        for (auto i = m_writable; i < m_end; i++) {
                auto const row = get_writable_index(i);
                for (auto j = 0; j < row->len; j++) {
                        auto const& cell = row->cells[j];
                        if (cell.attr.image())
                                m_image_pool.mark(cell.attr.image_ref());
                }
        }

        /* The cached row is a thawed copy of a frozen row, and its cells name
         * images by the ids they resolved to when it was thawed. index()
         * serves it again without re-thawing, and get_hyperlink_at_position()
         * leaves cells in it with no row number attached at all, so it holds
         * references exactly as a writable row does. A sweep blind to it frees
         * an id the cache still names, and the next image allocated takes that
         * id and is drawn where the old one was - which is the aliasing the
         * pool exists to make impossible.
         */
        for (auto j = 0; j < m_cached_row.len; j++) {
                auto const& cell = m_cached_row.cells[j];
                if (cell.attr.image())
                        m_image_pool.mark(cell.attr.image_ref());
        }

        /* An image that is still resident keeps its id whether or not any
         * cell names it yet: the cells are stamped separately from the
         * allocation, so an image can legitimately exist for a moment with
         * none.
         */
        for (auto const& [priority, image] : m_image_map)
                m_image_pool.mark(image->get_pool_id());

        m_image_pool.sweep_end();
}

/*
 * Write an image's pixels to the image stream, so that a row naming it can
 * still be drawn after the image itself has been freed.
 *
 * Keyed by priority: it comes from a monotonically increasing counter and is
 * never reused, so unlike a pool id it still means the same image whenever it
 * is read back.
 */
/*
 * Append the current attribute's image reference to the attr stream.
 *
 * Written BEFORE the record's 2-byte trailer, because the trailer has to stay
 * last for thaw_row's backwards walk to find it.
 */
void
Ring::append_stream_image_ref() noexcept
{
        StreamImageRef sref;
        memset(&sref, 0, sizeof(sref));

        /* Resolve the pool id to the image's stable priority while the pool can
         * still answer. An image already gone writes priority 0, which resolves
         * to nothing on the way back in.
         */
        auto const* img = m_image_pool.lookup(m_last_attr.image_ref());
        sref.priority = img ? uint64_t(img->get_priority()) + 1 : 0;
        sref.ref_bits = m_last_attr.link_raw();

        _vte_stream_append(m_attr_stream, (char const*)&sref, sizeof(sref));
}

void
Ring::spill_image(vte::image::Image const* image) noexcept
{
        if (!m_has_streams || m_image_stream == nullptr)
                return;

        auto const priority = image->get_priority();

        /* Already spilled. A restored image can be evicted again, and
         * re-appending its pixels every time would grow the stream without
         * bound while adding nothing.
         */
        if (m_image_spill.find(priority) != m_image_spill.end())
                return;

        auto* const surface = image->get_surface();
        if (surface == nullptr ||
            cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
                return;

        cairo_surface_flush(surface);

        auto const* const data = cairo_image_surface_get_data(surface);
        if (data == nullptr)
                return;

        auto const stride = cairo_image_surface_get_stride(surface);
        auto const height = image->get_height_px();
        auto const width = image->get_width_px();
        if (stride <= 0 || width <= 0 || height <= 0)
                return;

        auto const offset = _vte_stream_head(m_image_stream);

        auto record = ImageSpillRecord{};
        record.priority = uint64_t(priority);
        record.width_px = int32_t(width);
        record.height_px = int32_t(height);
        record.left_cells = int32_t(image->get_left());
        record.top_cells = int32_t(image->get_top());
        /* The image's OWN layout cell. Writing the constants here would make
         * an evicted-then-restored image silently change scale whenever the
         * two differ.
         */
        record.cell_width = int32_t(image->get_cell_width());
        record.cell_height = int32_t(image->get_cell_height());

        /* Store tightly packed rather than at the surface's stride: the
         * stride is an allocation detail of the cairo surface we happen to
         * have now, and the surface built on the way back in may choose a
         * different one.
         */
        auto const row_bytes = size_t(width) * 4;
        record.data_len = uint32_t(row_bytes * size_t(height));

        _vte_stream_append(m_image_stream, (char const*)&record, sizeof(record));
        for (auto y = 0; y < height; y++)
                _vte_stream_append(m_image_stream,
                                   (char const*)(data + size_t(y) * size_t(stride)),
                                   row_bytes);

        m_image_spill[priority] = ImageSpill{offset,
                                             long(image->get_top()),
                                             long(image->get_bottom())};

        /* Announced, because from outside the library an evicted-and-spilled
         * image and one that simply stayed resident look exactly alike: both
         * draw. A rendered test of the spill path has no other way to know its
         * insult landed, and one that assumed it would be testing the resident
         * path while claiming to test this one.
         */
        _vte_debug_print(vte::debug::category::RING,
                         "Spilled image {} rows {}..{} at offset {}",
                         priority, long(image->get_top()),
                         long(image->get_bottom()), offset);
}

/*
 * Rebuild an image previously written by spill_image(), or nullptr if it was
 * never spilled or has since been reclaimed.
 *
 * The restored image joins m_image_map like any other, so it is subject to the
 * same eviction; spill_image() is idempotent so that cycle is stable.
 */
vte::image::Image*
Ring::restore_image(size_t priority) /* throws */
{
        if (!m_has_streams || m_image_stream == nullptr)
                return nullptr;

        auto const it = m_image_spill.find(priority);
        if (it == m_image_spill.end())
                return nullptr;

        auto record = ImageSpillRecord{};
        if (!_vte_stream_read(m_image_stream, it->second.offset,
                              (char*)&record, sizeof(record)))
                return nullptr;

        /* Everything below comes off disk, so none of it is trusted. */
        if (record.priority != uint64_t(priority) ||
            record.width_px <= 0 || record.height_px <= 0 ||
            record.width_px > VTE_SIXEL_MAX_WIDTH ||
            record.height_px > VTE_SIXEL_MAX_HEIGHT ||
            record.cell_width <= 0 || record.cell_height <= 0)
                return nullptr;

        auto const row_bytes = size_t(record.width_px) * 4;
        if (record.data_len != row_bytes * size_t(record.height_px))
                return nullptr;

        auto surface = vte::take_freeable
                (cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                            record.width_px, record.height_px));
        if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS)
                return nullptr;

        auto* const dest = cairo_image_surface_get_data(surface.get());
        auto const stride = cairo_image_surface_get_stride(surface.get());
        if (dest == nullptr || stride < int(row_bytes))
                return nullptr;

        auto data_offset = it->second.offset + sizeof(record);
        for (auto y = 0; y < record.height_px; y++) {
                if (!_vte_stream_read(m_image_stream,
                                      data_offset + size_t(y) * row_bytes,
                                      (char*)(dest + size_t(y) * size_t(stride)),
                                      row_bytes))
                        return nullptr;
        }

        cairo_surface_mark_dirty(surface.get());

        /* Copied out of the packed record first: a packed field cannot bind
         * to the constructor's reference parameters.
         */
        auto const width_px = int(record.width_px);
        auto const height_px = int(record.height_px);
        auto const left_cells = int(record.left_cells);

        /* The row comes from the LIVE record, not from the disk one.
         *
         * record.top_cells was written at eviction and names the row the image
         * had then. A rewrap renumbers the ring from zero and re-anchors the
         * spill map with it, so after any reflow the disk copy names a row of a
         * ring that no longer exists. The map entry is the one thing that has
         * been kept current, and at eviction the two are equal by construction,
         * so this is the same number until the moment it stops being.
         *
         * What this does NOT fix is where the picture is drawn, and saying so
         * is the point of the note: the draw walks the cells that name an image
         * and places each stripe from ITS row and column, so an image with a
         * stale anchor still paints in the right place. Measured - the rendered
         * probe passes either way. The anchor is what the ring's own
         * bookkeeping decides on: which images a row drop takes with it, what
         * image_gc() and the by-top map see, and which rectangle an erase
         * compares against. An anchor a hundred rows below the picture makes
         * every one of those answer about a row the image does not occupy.
         */
        auto const top_cells = int(it->second.top);
        auto const cell_width = int(record.cell_width);
        auto const cell_height = int(record.cell_height);

        auto image = std::make_unique<vte::image::Image>(std::move(surface),
                                                         priority,
                                                         width_px,
                                                         height_px,
                                                         left_cells,
                                                         top_cells,
                                                         cell_width,
                                                         cell_height);

        auto const pool_id = m_image_pool.allocate(image.get());
        if (pool_id == vte::image::k_ref_pool_id_none)
                return nullptr;

        image->set_pool_id(pool_id);

        auto* const raw = image.get();
        m_image_fast_memory_used += image_cost(image.get());
        m_image_map[priority] = std::move(image);
        m_image_by_top_map.emplace(raw->get_top(), raw);
        sync_has_images();

        _vte_debug_print(vte::debug::category::RING,
                         "Restored image {} at row {}",
                         priority, long(raw->get_top()));

        /* Faulting an image back in from the scrollback ADDS to the budget, so
         * it has to be collected against like any other addition. Without this
         * scrolling back through history that held images pulls every one of
         * them into RAM and nothing ever evicts them: measured at 8.3 times the
         * configured budget from Page Up alone.
         */
        image_gc(raw);

        return raw;
}

/*
 * Drop spilled images whose rows have left the ring entirely, and advance the
 * stream's tail past them.
 *
 * A spill is needed only while some row that names it can still be thawed. Once
 * the last such row is gone the pixels are unreachable, so keeping them is pure
 * growth - which is the failure this stream would otherwise introduce.
 */
void
Ring::reclaim_image_spill(row_t before_row) noexcept
{
        if (!m_has_streams || m_image_stream == nullptr)
                return;

        for (auto it = m_image_spill.begin(); it != m_image_spill.end(); ) {
                if (it->second.bottom >= long(before_row))
                        ++it;
                else
                        it = m_image_spill.erase(it);
        }

        /* Records are appended in priority order and priorities only grow, so
         * the surviving entry with the smallest offset bounds everything still
         * reachable.
         */
        auto tail = _vte_stream_head(m_image_stream);
        for (auto const& [priority, spill] : m_image_spill)
                tail = std::min(tail, spill.offset);

        _vte_stream_advance_tail(m_image_stream, tail);
}

void
Ring::image_gc(vte::image::Image const* exempt) noexcept
{
        while (m_image_fast_memory_used > m_image_memory_max) {
                if (m_image_map.empty()) {
                        /* If this happens, we've miscounted somehow. */
                        break;
                }

                /* Oldest first, except that an image restored from the
                 * scrollback a moment ago must not be the victim.
                 *
                 * It is by construction the OLDEST thing in the map - that is
                 * what being in the scrollback means - so without this it would
                 * be evicted immediately every time and the row that faulted it
                 * in would fault it again on the next frame.
                 */
                auto victim = m_image_map.begin();
                if (exempt != nullptr && victim->second.get() == exempt) {
                        ++victim;
                        if (victim == m_image_map.end())
                                break;
                }

                auto& image = victim->second;

                /* Evicted for memory, not erased by the user: rows naming it
                 * can still be thawed, so keep the pixels where they cost
                 * disk instead of RAM.
                 */
                spill_image(image.get());

                m_image_fast_memory_used -= image_cost(image.get());
                note_image_freed(image.get());
                unlink_image_from_top_map(image.get());
                m_image_map.erase(victim);
        }

        sync_has_images();
}

Ring::image_by_top_map_type::iterator
Ring::erase_image(Ring::image_by_top_map_type::iterator it) noexcept
{
        /* Free the image @it refers to and return an iterator to the next one.
         *
         * The priority has to be taken before unlinking, since erasing from
         * m_image_map destroys the Image that m_image_by_top_map only points to.
         */
        auto const image = it->second;
        auto const priority = image->get_priority();

        m_image_fast_memory_used -= image_cost(image);
        note_image_freed(image);
        auto const next = m_image_by_top_map.erase(it);
        m_image_map.erase(priority);
        sync_has_images();

        return next;
}

void
Ring::drop_images_before(row_t row) noexcept
{
        /* Free every image that now lies entirely before @row, i.e. whose last row has
         * left the ring. Without this the maps grow without bound: the row-dropping
         * paths advance m_start and m_end and never consult m_image_map, so an image
         * scrolled out of the scrollback stayed resident until a reset. It is also what
         * lets the ring evict images predictably rather than only under the size cap.
         *
         * m_image_by_top_map is ordered by top row, so an image whose top is at or after
         * @row cannot possibly end before it: stopping at the first such entry is exact,
         * not an approximation. An image straddling @row is KEPT - part of it is still in
         * the ring - and it is dropped later when its bottom follows.
         */
        for (auto it = m_image_by_top_map.begin();
             it != m_image_by_top_map.end() && it->first < row; ) {
                auto const image = it->second;
                if (long(image->get_bottom()) >= long(row)) {
                        ++it;
                        continue;
                }

                /* Its rows are leaving the ring, but they can be thawed until
                 * they are discarded outright, which is where the spill is
                 * reclaimed.
                 */
                spill_image(image);

                it = erase_image(it);
        }
}

void
Ring::drop_images_after(row_t row) noexcept
{
        /* Free every image that has a row at or after @row, i.e. that reaches past
         * the last row the ring holds. The counterpart of drop_images_before() for
         * the one path that destroys rows at the bottom instead of at the front:
         * shrink(), which pulls m_end back and never consulted the image maps, so an
         * image below the new end kept a row number that no longer exists.
         *
         * Unlike the top row, the bottom row is not the map's key, so there is no
         * early exit: an image with a small top can still be tall enough to reach
         * past @row. Shrinking is rare enough that the full walk does not matter.
         */
        for (auto it = m_image_by_top_map.begin();
             it != m_image_by_top_map.end(); ) {
                if (long(it->second->get_bottom()) < long(row)) {
                        ++it;
                        continue;
                }

                it = erase_image(it);
        }
}

bool
Ring::image_rows_survive_rewrap(long top,
                                long bottom,
                                column_t columns) noexcept
{
        /* Whether the physical rows [@top, @bottom] come through a reflow to
         * @columns unchanged, which is the condition under which re-anchoring
         * only the top row is a truthful description of what happened to the
         * rectangle. The rule itself is spelled out at the caller below; it
         * lives here because it has to be asked of images that are no longer
         * resident too, where there is no Image object to ask.
         */
        auto record = RowRecord{};

        if (top > long(m_start) &&
            (!read_row_record(&record, row_t(top - 1)) ||
             record.soft_wrapped))
                return false;

        for (auto row = top; row <= bottom; ++row) {
                /* A row outside the ring has no record to judge it by, and
                 * the image has already lost it in any case.
                 */
                if (row < long(m_start) || row >= long(m_end) ||
                    !read_row_record(&record, row_t(row)) ||
                    record.soft_wrapped ||
                    column_t(record.width) > columns)
                        return false;
        }

        return true;
}

/*
 * Work out where each spilled image's rows are about to land, and drop the
 * records that are not going to land anywhere.
 *
 * This is the spill's half of the reflow. A spill record remembers the ring
 * rows its pixels covered, and those rows are what reclaim_image_spill()
 * decides on: once every row naming an image is gone, its bytes can leave the
 * stream. rewrap() renumbers the whole ring from zero, so a record left holding
 * its old numbers describes rows that do not exist, compares as still-live
 * against every future row number, and pins the stream's tail for the rest of
 * the session.
 *
 * Renumbering is possible because a spilled image is keyed on the same thing a
 * resident one is - a ring row - and that row still has a frozen record and a
 * text offset right up until the new row stream replaces the old one. Nothing
 * about the image's own storage is needed. So the mapping is taken here, before
 * the reflow starts, and applied by rewrap_spills_in_range() as the new rows are
 * produced.
 *
 * The alternative - dropping every spill record at a reflow - would be cheap and
 * would also stop the growth, but it would throw away the pixels of images the
 * user can still scroll back to, on every window resize. Renumbering costs one
 * text-offset lookup per parked image and keeps them.
 *
 * Returns the plan in text-offset order. Must run after the freeze and before
 * the new row stream is installed.
 */
std::vector<Ring::SpillRewrap>
Ring::plan_spill_rewrap(column_t columns) noexcept
{
        auto plan = std::vector<SpillRewrap>{};

        for (auto it = m_image_spill.begin(); it != m_image_spill.end(); ) {
                auto const& spill = it->second;
                auto ofs = CellTextOffset{};

                /* The same delete-on-tear rule the resident images get: an
                 * image whose covered rows the reflow rewrites no longer lines
                 * up with the text it was emitted beside, and restoring it later
                 * would paint it across whatever moved underneath.
                 */
                if (!image_rows_survive_rewrap(spill.top, spill.bottom, columns) ||
                    spill.top < long(m_start) ||
                    spill.top >= long(m_end) ||
                    !frozen_row_column_to_text_offset(spill.top, 0, &ofs)) {
                        it = m_image_spill.erase(it);
                        continue;
                }

                plan.push_back(SpillRewrap{ofs.text_offset,
                                           it->first,
                                           spill.bottom - spill.top});
                ++it;
        }

        std::sort(plan.begin(), plan.end(),
                  [](SpillRewrap const& a, SpillRewrap const& b) {
                          return a.text_offset < b.text_offset;
                  });

        return plan;
}

/*
 * Re-anchor the spilled images whose old top row falls in [@text_start_ofs,
 * @text_end_ofs), the same range and the same forward pass that
 * rewrap_images_in_range() uses for the resident ones. @cursor is carried across
 * the calls of one rewrap.
 *
 * Only the top moves, and the height comes along with it: the reflow does not
 * change the picture, and the records that get this far are exactly the ones
 * whose rows the reflow leaves alone.
 */
void
Ring::rewrap_spills_in_range(std::vector<SpillRewrap> const& plan,
                             size_t& cursor,
                             size_t text_start_ofs,
                             size_t text_end_ofs,
                             row_t new_row_index) noexcept
{
        while (cursor < plan.size()) {
                auto const& entry = plan[cursor];

                /* Not this new row's text yet; a later call will place it. */
                if (entry.text_offset >= text_end_ofs)
                        return;

                ++cursor;

                /* Before the range: the ranges tile the whole text stream and
                 * this pass runs in offset order, so this is unreachable. Drop
                 * rather than skip, so that no record can survive holding a row
                 * number from the old ring - which is the whole failure this
                 * function exists to prevent.
                 */
                if (entry.text_offset < text_start_ofs) {
                        m_image_spill.erase(entry.priority);
                        continue;
                }

                auto const it = m_image_spill.find(entry.priority);
                if (it == m_image_spill.end())
                        continue;

                it->second.top = long(new_row_index);
                it->second.bottom = long(new_row_index) + entry.span;
        }
}

void
Ring::drop_images_torn_by_rewrap(column_t columns) noexcept
{
        /* Free every image whose rows a reflow to @columns is about to take apart.
         *
         * An image is a rectangle of PHYSICAL rows, and a reflow is precisely an
         * operation that changes how many physical rows a paragraph takes up.
         * rewrap_images_in_range() re-anchors an image's TOP row and leaves its
         * height alone, which is right only if the rows below the top come out of
         * the reflow the same way the top does. When they do not, the image goes on
         * claiming rows that now hold something else, and since text is painted over
         * images, whatever reflowed into them is drawn across the picture.
         *
         * A covered row comes through untouched when it is a paragraph of its own
         * that already fits: hard wrapped, so nothing below is joined onto it, and no
         * wider than @columns, so it is not split in two. Then the reflow copies it
         * across as exactly one row and the whole rectangle merely shifts, which is
         * what re-anchoring the top expresses.
         *
         * The row ABOVE the top is held to the same rule, because a paragraph that
         * continues into the image's first row rewrites that row just as thoroughly.
         * Placing an image tears that boundary apart, but nothing stops later text
         * from soft wrapping across it again.
         *
         * Anything else, the image is deleted - the same delete-on-tear rule the ring
         * already applies to an insert or a remove whose seam runs through an image.
         * The alternative is not "keep the image": it is keeping an image that no
         * longer lines up with the text it was emitted next to, which is what the
         * code did before and which looked like corruption. Honestly gone beats
         * silently wrong. It does mean an image sharing a row with text too wide for
         * the new width is lost, and lost for good, since widening again cannot bring
         * back pixels nobody kept.
         *
         * Must run after the freeze, so that every row has a record to read, and
         * before the by-top map is walked to re-anchor the survivors.
         */
        for (auto it = m_image_by_top_map.begin();
             it != m_image_by_top_map.end(); ) {
                auto const image = it->second;
                auto const top = long(image->get_top());
                auto const bottom = long(image->get_bottom());

                if (image_rows_survive_rewrap(top, bottom, columns)) {
                        ++it;
                        continue;
                }

                _vte_debug_print(vte::debug::category::RING,
                                 "Dropping image at rows {}..{}: its rows do not survive a rewrap to {} columns",
                                 top, bottom, columns);

                it = erase_image(it);
        }
}

/*
 * Ring::erase_images_in_rect:
 * @top, @bottom, @left, @right: an inclusive rectangle in ring coordinates
 * @damage_top, @damage_bottom: out, the rows the deleted images occupied
 *
 * Delete, whole, every image whose cells intersect the given rectangle; the
 * image being placed by its own emission burst is held out. Returns whether
 * anything was deleted, in which case the out parameters bound the rows that
 * need repainting.
 *
 * An image is deleted in full even when only one of its cells is touched. The
 * alternative, splitting the image and keeping the untouched part, costs several
 * hundred lines of geometry for a fidelity no producer needs: cell erase is the
 * only way a producer can take its image back, and they all erase at least the
 * whole area they drew into.
 *
 * All comparisons are made in signed long. An image stores its position in int,
 * a ring row is an unsigned long, and the callers legitimately pass rows derived
 * from a cursor position minus one; mixing those in an unsigned comparison turns
 * an empty rectangle into an enormous one.
 */
bool
Ring::erase_images_in_rect(long top,
                           long bottom,
                           long left,
                           long right,
                           long* damage_top,
                           long* damage_bottom) noexcept
{
        auto const none = std::vector<vte::image::Image*>{};
        return erase_images_in_rect_except(top, bottom, left, right,
                                           damage_top, damage_bottom, none);
}

/*
 * Ring::erase_images_in_rect_except:
 * @exempt: images that are moving with the cells rather than being taken by
 *   them, and so must keep their cells
 *
 * The body of erase_images_in_rect(); see there. The exemption exists for
 * shift_images_for_scroll(), whose caller is about to memmove these cells
 * sideways rather than write over them.
 */
bool
Ring::erase_images_in_rect_except(long top,
                                  long bottom,
                                  long left,
                                  long right,
                                  long* damage_top,
                                  long* damage_bottom,
                                  std::vector<vte::image::Image*> const& exempt) noexcept
{
        if (top > bottom || left > right)
                return false;

        auto damaged = false;

        auto const is_exempt = [&](vte::image::Image const* image) noexcept {
                return image == m_placing_image ||
                       std::find(exempt.begin(), exempt.end(), image) != exempt.end();
        };

        auto note_damage = [&](vte::image::Image const* image) noexcept {
                auto const t = long(image->get_top());
                auto const b = long(image->get_bottom());
                if (!damaged) {
                        *damage_top = t;
                        *damage_bottom = b;
                        damaged = true;
                } else {
                        *damage_top = std::min(*damage_top, t);
                        *damage_bottom = std::max(*damage_bottom, b);
                }
        };

        /* Step 1: the cells inside the rectangle stop belonging to any image.
         *
         * This used to delete every image whose bounding box merely touched
         * the rectangle, because a whole-image blit could not draw an image
         * that had lost some of its cells - so losing one cell had to mean
         * losing all of them. The draw walks the cells now, so the honest
         * thing is possible: the cells written over stop being the image's,
         * and the rest of the image is untouched.
         */
        auto const first = std::max(top, long(m_writable));
        auto const last = std::min(bottom, long(m_end) - 1);

        for (auto r = first; r <= last; r++) {
                auto* const row = get_writable_index(r);

                auto const from = std::max(left, long{0});
                auto const to = std::min(right, long(row->len) - 1);

                for (auto c = from; c <= to; c++) {
                        auto& cell = row->cells[c];
                        if (!cell.attr.image())
                                continue;

                        auto const* image = m_image_pool.lookup(cell.attr.image_ref());

                        /* An image whose own emission burst is still running is
                         * placing these cells right now; it is not being erased
                         * by them. Neither is one that is following the cells.
                         */
                        if (image != nullptr && is_exempt(image))
                                continue;

                        if (image != nullptr)
                                note_damage(image);

                        /* Clearing the hyperlink index clears the union tag with
                         * it, so the cell stops naming an image and the draw
                         * skips it.
                         */
                        cell.attr.set_hyperlink_idx(0);
                        cell.c = 0;
                        damaged = true;
                }
        }

        /* Step 2: free any image that no cell names any more.
         *
         * Only images that intersect the rectangle can have lost a cell here,
         * and each is checked over its OWN rows rather than the whole ring, so
         * the work is bounded by the image's height and not by the scrollback.
         */
        for (auto it = m_image_by_top_map.begin();
             it != m_image_by_top_map.end(); ) {
                auto* const image = it->second;

                /* Keyed by top row: once past @bottom, nothing left begins
                 * inside the rectangle either.
                 */
                if (long(image->get_top()) > bottom)
                        break;

                if (is_exempt(image) ||
                    long(image->get_bottom()) < top ||
                    long(image->get_left()) > right ||
                    long(image->get_left()) + long(image->get_width()) - 1 < left) {
                        ++it;
                        continue;
                }

                if (image_has_any_cell(image)) {
                        ++it;
                        continue;
                }

                note_damage(image);
                it = erase_image(it);
        }

        return damaged;
}

/*
 * Ring::shift_images_for_scroll:
 * @top, @bottom, @left, @right: the inclusive rectangle of cells about to be
 *   moved sideways, in ring coordinates
 * @amount: how far, in cells; positive moves right, negative left
 * @damage_top, @damage_bottom: out, the rows that need repainting
 *
 * What the caller of a horizontal scroll (ICH, DCH, SL, SR, insert mode) owes
 * the images before it memmoves the cells.
 *
 * A cell carries its own tile coordinate and the draw reads the picture off
 * the cells, so cells that are memmoved sideways carry their piece of the
 * image with them for free. The only thing that does not move by itself is
 * the image's own rectangle, which every lifetime rule reads and which
 * validate_image_cells() requires to agree with the cells. So an image every
 * one of whose CELLS is inside the moving rectangle, and stays inside it
 * after the move, is shifted here and keeps them.
 *
 * The test is over the cells and not over the image's rectangle because the
 * two are not the same shape: a picture whose lower rows have been written
 * over still has a rectangle that reaches down to where they were, and that
 * rectangle would reject a one-row ICH on the row the picture actually still
 * occupies. The cells are what moves and what the draw reads, so the cells
 * are what decides.
 *
 * An image that does not fit that description would be torn - part of it
 * moved, part of it standing still, or part of it pushed off the edge of the
 * region and gone - and no single rectangle can describe the result. Those
 * lose the cells inside the rectangle exactly as any other write to them
 * would, which is what erase_images_in_rect() already does.
 */
bool
Ring::shift_images_for_scroll(long top,
                              long bottom,
                              long left,
                              long right,
                              long amount,
                              long* damage_top,
                              long* damage_bottom) noexcept
{
        if (top > bottom || left > right)
                return false;

        auto followers = std::vector<vte::image::Image*>{};

        if (amount != 0) {
                for (auto const& [key, image] : m_image_by_top_map) {
                        /* Keyed by top row: once past @bottom nothing left
                         * begins inside the rectangle either. */
                        if (long(image->get_top()) > bottom)
                                break;

                        /* An image is exempt from every rule that moves images
                         * while its own emission burst is running. */
                        if (image == m_placing_image)
                                continue;

                        if (long(image->get_bottom()) < top)
                                continue;

                        /* Every cell of this image, over the rows it spans,
                         * has to be inside the rectangle: one outside it would
                         * stand still while the rest of the picture moved, and
                         * then no single left/top can describe where the
                         * picture is.
                         *
                         * A cell that the move pushes off the end of the
                         * rectangle is not such a case. It is overwritten or
                         * dropped exactly as a cell of text there would be,
                         * and what is left still sits at left+amount+tile_col,
                         * so the picture is simply clipped - which is what the
                         * cell whose column DCH deleted deserves. What cannot
                         * happen is every cell going that way, since then the
                         * image would be left resident with nothing naming it.
                         */
                        /* Only the writable rows can be read cell by cell, so
                         * an image reaching up into the frozen ones has rows
                         * this test cannot see. Those cells are outside the
                         * rectangle by definition - the rectangle is writable -
                         * so such an image would be torn; treat it as one
                         * rather than judge it on the rows that happen to be
                         * visible here.
                         */
                        if (long(image->get_top()) < long(m_writable))
                                continue;

                        auto const first = long(image->get_top());
                        auto const last = std::min(long(image->get_bottom()),
                                                   long(m_end) - 1);

                        auto follows = true;
                        auto any_cell = false;
                        auto any_survivor = false;

                        for (auto r = first; r <= last && follows; r++) {
                                auto const* const row = get_writable_index(r);

                                for (auto c = long{0}; c < long(row->len); c++) {
                                        auto const& cell = row->cells[c];
                                        if (!cell.attr.image())
                                                continue;
                                        if (m_image_pool.lookup(cell.attr.image_ref()) != image)
                                                continue;

                                        any_cell = true;

                                        if (r < top || r > bottom ||
                                            c < left || c > right) {
                                                follows = false;
                                                break;
                                        }

                                        if (c + amount >= left && c + amount <= right)
                                                any_survivor = true;
                                }
                        }

                        if (any_cell && follows && any_survivor)
                                followers.push_back(image);
                }
        }

        auto damaged = erase_images_in_rect_except(top, bottom, left, right,
                                                   damage_top, damage_bottom,
                                                   followers);

        for (auto* const image : followers) {
                auto const t = long(image->get_top());
                auto const b = long(image->get_bottom());
                if (!damaged) {
                        *damage_top = t;
                        *damage_bottom = b;
                        damaged = true;
                } else {
                        *damage_top = std::min(*damage_top, t);
                        *damage_bottom = std::max(*damage_bottom, b);
                }

                image->set_left(int(long(image->get_left()) + amount));
        }

        return damaged;
}

/*
 * Ring::shift_images_for_vscroll:
 * @top, @bottom, @left, @right: the inclusive rectangle of cells about to be
 *   moved up or down, in ring coordinates
 * @amount: how far, in rows; positive moves the cells down, negative up
 * @damage_top, @damage_bottom: out, the rows that need repainting
 *
 * The vertical sibling of shift_images_for_scroll(), for the partial-rows
 * branches of Terminal::scroll_text_up() and scroll_text_down() - the ones
 * DECSLRM left/right margins select, where the cells are memcpy'd from row to
 * row and the rows themselves do not move.
 *
 * The mechanism is the same one: a cell carries its own tile of the picture and
 * the draw reads the picture off the cells, so cells copied to another row take
 * their piece of the image with them. What does not move by itself is the
 * image's own top row, which is both its anchor and the key it is filed under
 * in m_image_by_top_map, so a follower is re-anchored AND re-keyed here.
 *
 * Where this differs from the horizontal rule is what happens to a picture the
 * region would crop. Sideways, a cell pushed off the end of the rectangle is
 * simply dropped and the rest follows clipped. Vertically the same clipping
 * would have to move the image's top OUT of the region: one row above it for
 * every tile row the scroll ate. That is not available here.
 *
 *   - The full-width vertical scroll of the very same region - the branch taken
 *     the moment DECSLRM is not narrowing it - goes through ring_remove() and
 *     ring_insert(), and shift_images_for_remove() destroys an image the seam
 *     runs through rather than cropping it. Cropping here would make an image
 *     survive or die according to whether the margins happened to be full
 *     width, which is not a distinction the user made.
 *   - row_t is unsigned, so an image cropped up past row 0 is filed under a key
 *     near ULONG_MAX, where every ordered walk over m_image_by_top_map - all of
 *     which stop on a key comparison - steps straight past it.
 *
 * So a follower here must have every one of its cells inside the rectangle AND
 * still inside it after the move. Anything else loses the cells the rectangle
 * takes, exactly as erase_images_in_rect() already does.
 */
bool
Ring::shift_images_for_vscroll(long top,
                               long bottom,
                               long left,
                               long right,
                               long amount,
                               long* damage_top,
                               long* damage_bottom) noexcept
{
        if (top > bottom || left > right)
                return false;

        auto followers = std::vector<vte::image::Image*>{};

        if (amount != 0) {
                for (auto const& [key, image] : m_image_by_top_map) {
                        /* Keyed by top row: once past @bottom nothing left
                         * begins inside the rectangle either. */
                        if (long(image->get_top()) > bottom)
                                break;

                        /* An image is exempt from every rule that moves images
                         * while its own emission burst is running. */
                        if (image == m_placing_image)
                                continue;

                        if (long(image->get_bottom()) < top)
                                continue;

                        /* As in shift_images_for_scroll(): rows below
                         * m_writable cannot be read cell by cell, and cells
                         * there are outside a rectangle that is writable by
                         * definition, so such an image is torn.
                         */
                        if (long(image->get_top()) < long(m_writable))
                                continue;

                        auto const first = long(image->get_top());
                        auto const last = std::min(long(image->get_bottom()),
                                                   long(m_end) - 1);

                        auto follows = true;
                        auto any_cell = false;

                        for (auto r = first; r <= last && follows; r++) {
                                auto const* const row = get_writable_index(r);

                                for (auto c = long{0}; c < long(row->len); c++) {
                                        auto const& cell = row->cells[c];
                                        if (!cell.attr.image())
                                                continue;
                                        if (m_image_pool.lookup(cell.attr.image_ref()) != image)
                                                continue;

                                        any_cell = true;

                                        /* Inside the rectangle now, and inside
                                         * it still once the move has landed.
                                         * The columns do not move, so only the
                                         * row is tested twice.
                                         */
                                        if (r < top || r > bottom ||
                                            c < left || c > right ||
                                            r + amount < top || r + amount > bottom) {
                                                follows = false;
                                                break;
                                        }
                                }
                        }

                        if (any_cell && follows)
                                followers.push_back(image);
                }
        }

        auto damaged = erase_images_in_rect_except(top, bottom, left, right,
                                                   damage_top, damage_bottom,
                                                   followers);

        for (auto* const image : followers) {
                /* Both the rows it is leaving and the rows it is arriving at
                 * have to be repainted. */
                auto const t = std::min(long(image->get_top()),
                                        long(image->get_top()) + amount);
                auto const b = std::max(long(image->get_bottom()),
                                        long(image->get_bottom()) + amount);
                if (!damaged) {
                        *damage_top = t;
                        *damage_bottom = b;
                        damaged = true;
                } else {
                        *damage_top = std::min(*damage_top, t);
                        *damage_bottom = std::max(*damage_bottom, b);
                }

                /* The anchor and the key are one fact stored twice, so they
                 * move together or validate_images() catches it. */
                unlink_image_from_top_map(image);
                image->set_top(int(long(image->get_top()) + amount));
                m_image_by_top_map.emplace(row_t(image->get_top()), image);
                m_images_changed = true;
        }

        return damaged;
}

/*
 * Whether any cell in the writable rows still names @image.
 *
 * Scans only the rows the image spans. An image outside the writable window
 * has no cells to find and is answered by the callers' own residency rules,
 * not here.
 */
bool
Ring::image_has_any_cell(vte::image::Image const* image) const noexcept
{
        auto const id = image->get_pool_id();
        if (id == vte::image::k_ref_pool_id_none)
                return false;

        auto const first = std::max(long(image->get_top()), long(m_writable));
        auto const last = std::min(long(image->get_bottom()), long(m_end) - 1);

        for (auto r = first; r <= last; r++) {
                auto const* const row = get_writable_index(r);
                for (auto c = 0; c < row->len; c++) {
                        auto const& cell = row->cells[c];
                        if (cell.attr.image() &&
                            cell.attr.image_ref().pool_id() == id)
                                return true;
                }
        }

        return false;
}

/*
 * Ring::shift_images_for_insert:
 * @position: the row about to be inserted at
 *
 * A row is being pushed in at @position, so every row from there down moves
 * one further down. An image entirely below the seam moves with its rows; an
 * image the seam runs through is destroyed, because its rows are no longer
 * contiguous and there is no position it could keep that would still describe
 * where its pixels are. Composed over the rows of a region scroll, this is the
 * "an image inside the scrolled region moves, an image straddling its edge
 * dies" rule that the region scroll needs.
 *
 * Inserting at the end of the ring is exempt. An image is anchored at the
 * cursor before the rows it covers are created, so during its own emission it
 * genuinely straddles the end; without this every sixel emitted at the bottom
 * of the screen would classify itself as straddling and delete itself. Nothing
 * else lives at or past the end, so the exemption cannot spare anything real.
 *
 * The keys of m_image_by_top_map are the images' top rows, and no image can be
 * above row 0: the rules only ever move an image between existing rows, and
 * the rewrap drops any image whose row left the ring. So the keys order the
 * same way the rows do, and the walks below can stop on a key comparison.
 */
void
Ring::shift_images_for_insert(row_t position) noexcept
{
        if (position == m_end)
                return;

        /* Destroyed: top strictly above the seam, bottom at or below it. */
        for (auto it = m_image_by_top_map.begin();
             it != m_image_by_top_map.end() && it->first < position; ) {
                auto const image = it->second;
                if (image == m_placing_image ||
                    long(image->get_bottom()) < long(position)) {
                        ++it;
                        continue;
                }

                it = erase_image(it);
                m_images_changed = true;
        }

        /* Moved: top at or below the seam. Walked backwards, so that a re-keyed
         * entry (which always lands after every entry not yet visited, its key
         * having just grown by one) cannot be visited twice.
         */
        auto it = m_image_by_top_map.end();
        while (it != m_image_by_top_map.begin()) {
                auto const cur = std::prev(it);
                if (cur->first < position)
                        break;

                auto const at_begin = (cur == m_image_by_top_map.begin());
                auto const before = at_begin ? cur : std::prev(cur);
                auto const image = cur->second;

                if (image != m_placing_image) {
                        auto node = m_image_by_top_map.extract(cur);
                        image->set_top(image->get_top() + 1);
                        node.key() = row_t(image->get_top());
                        m_image_by_top_map.insert(std::move(node));
                        m_images_changed = true;
                }

                if (at_begin)
                        break;

                it = std::next(before);
        }
}

/*
 * Ring::shift_images_for_remove:
 * @position: the row about to be removed
 *
 * The counterpart of shift_images_for_insert(): everything below @position
 * moves one row up, an image containing @position loses one of its rows and is
 * destroyed. There is no end-of-ring exemption to make here, since a row that
 * is being removed exists by definition.
 */
void
Ring::shift_images_for_remove(row_t position) noexcept
{
        /* Walked forwards, so that a re-keyed entry (whose key has just shrunk
         * by one) always lands before every entry not yet visited.
         */
        for (auto it = m_image_by_top_map.begin();
             it != m_image_by_top_map.end(); ) {
                auto const image = it->second;
                if (image == m_placing_image) {
                        ++it;
                        continue;
                }

                auto const top = long(image->get_top());
                if (top > long(position)) {
                        auto const next = std::next(it);
                        auto node = m_image_by_top_map.extract(it);
                        image->set_top(int(top - 1));
                        node.key() = row_t(top - 1);
                        m_image_by_top_map.insert(std::move(node));
                        m_images_changed = true;
                        it = next;
                        continue;
                }

                if (long(image->get_bottom()) >= long(position)) {
                        it = erase_image(it);
                        m_images_changed = true;
                        continue;
                }

                ++it;
        }
}

void
Ring::unlink_image_from_top_map(vte::image::Image const* image) noexcept
{
        auto [begin, end] = m_image_by_top_map.equal_range(image->get_top());

        for (auto it = begin; it != end; ++it) {
                if (it->second != image)
                        continue;

                m_image_by_top_map.erase(it);
                break;
        }
}

void
Ring::rebuild_image_top_map() /* throws */
{
        m_image_by_top_map.clear();

        for (auto it = m_image_map.begin(), end = m_image_map.end();
             it != end;
             ++it) {
                auto const& image = it->second;
                m_image_by_top_map.emplace(std::piecewise_construct,
                                           std::forward_as_tuple(image->get_top()),
                                           std::forward_as_tuple(image.get()));
        }
}

/* Re-anchor the images that belong to the old rows whose text now makes up the
 * single new row @new_row_index, spanning [@text_start_ofs, @text_end_ofs) of the
 * text stream. @it is the shared cursor into m_image_by_top_map, carried across
 * the calls of one rewrap; both the map and the ranges are ordered by text
 * offset, so one forward pass visits every image exactly once.
 *
 * Only the row moves. The column is left exactly as it is, because reflow simply
 * has no opinion about it: frozen_row_column_to_text_offset() deliberately
 * disregards an image's column (it maps column 0), and the cells under an image
 * are blanked, so a text-offset round trip would anchor the image to whatever text
 * reflowed into that row rather than to where the image is. The column is absolute
 * and nothing about narrowing the window invalidates it: an image that no longer
 * fits is merely clipped by the draw loop, and reappears intact when the window is
 * widened again. Deleting it instead would lose a prompt-emitted image on every
 * window retile, with nobody around to re-emit it.
 *
 * An image is dropped only when its position genuinely no longer exists: its row
 * has left the ring, or its text offset cannot be mapped.
 */
void
Ring::rewrap_images_in_range(Ring::image_by_top_map_type::iterator& it,
                             size_t text_start_ofs,
                             size_t text_end_ofs,
                             row_t new_row_index) noexcept
{
        while (it != m_image_by_top_map.end()) {
                auto const image = it->second;
                auto const top = image->get_top();

                /* Rows outside the ring have no text offset to map through, and
                 * frozen_row_column_to_text_offset() does not report that: below
                 * m_start it clamps the position onto the first row of the ring
                 * (which is why images below the scrollback start all used to pile
                 * onto it), and at or past m_end it synthesises an offset past the
                 * stream head (which used to leave the image holding a row number
                 * in the old ring's numbering). Both fabricate a position for a row
                 * that is gone, so the image goes with it.
                 */
                if (top < 0 || row_t(top) < m_start || row_t(top) >= m_end) {
                        it = erase_image(it);
                        continue;
                }

                auto ofs = CellTextOffset{};
                if (!frozen_row_column_to_text_offset(top, 0, &ofs)) {
                        it = erase_image(it);
                        continue;
                }

                /* Not this new row's text yet; a later call will place it. */
                if (ofs.text_offset >= text_end_ofs)
                        break;

                /* Before the range: unreachable, as the ranges passed to the
                 * successive calls tile the whole text stream and this pass runs in
                 * offset order. Drop rather than skip, so that no image can survive
                 * holding a row number from the old ring.
                 */
                if (ofs.text_offset < text_start_ofs) {
                        it = erase_image(it);
                        continue;
                }

                image->set_top(new_row_index);
                ++it;
        }
}

#endif /* WITH_SIXEL */

/*
 * Find existing idx for the hyperlink or allocate a new one.
 *
 * Returns 0 if given no hyperlink or an empty one, or if the pool is full.
 * Returns the idx (either already existing or newly allocated) from 1 up to
 * VTE_HYPERLINK_COUNT_MAX inclusive otherwise.
 *
 * FIXME do something more effective than a linear search
 */
Ring::hyperlink_idx_t
Ring::get_hyperlink_idx_no_update_current(char const* hyperlink)
{
        hyperlink_idx_t idx;
        gsize len;
        GString *str;

        if (!hyperlink || !hyperlink[0])
                return 0;

        len = strlen(hyperlink);

        /* Linear search for this particular URI */
        auto const last_idx = m_hyperlink_highest_used_idx + 1;
        for (idx = 1; idx < last_idx; ++idx) {
                if (strcmp(hyperlink_get(idx)->str, hyperlink) == 0) {
                        _vte_debug_print(vte::debug::category::HYPERLINK,
                                         "get_hyperlink_idx: already existing idx {} for id;uri=\"{}\"",
                                         idx, hyperlink);
                        return idx;
                }
        }

        /* FIXME it's the second time we're GCing if coming from get_hyperlink_idx */
        hyperlink_gc();

        /* Another linear search for an empty slot where a GString is already allocated */
        for (idx = 1; idx < m_hyperlinks->len; idx++) {
                if (hyperlink_get(idx)->len == 0) {
                        _vte_debug_print(vte::debug::category::HYPERLINK,
                                         "get_hyperlink_idx: reassigning old idx {} for id;uri=\"{}\"",
                                         idx, hyperlink);
                        /* Grow size if required, however, never shrink to avoid long-term memory fragmentation. */
                        g_string_append_len (hyperlink_get(idx), hyperlink, len);
                        m_hyperlink_highest_used_idx = MAX (m_hyperlink_highest_used_idx, idx);
                        return idx;
                }
        }

        /* All allocated slots are in use. Gotta allocate a new one */
        vte_assert_cmpuint(m_hyperlink_highest_used_idx + 1, ==, m_hyperlinks->len);

        /* VTE_HYPERLINK_COUNT_MAX should be big enough for this not to happen under
           normal circumstances. Anyway, it's cheap to protect against extreme ones. */
        if (m_hyperlink_highest_used_idx == VTE_HYPERLINK_COUNT_MAX) {
                _vte_debug_print(vte::debug::category::HYPERLINK,
                                 "get_hyperlink_idx: idx 0 (ran out of available idxs) for id;uri=\"{}\"",
                                 hyperlink);
                return 0;
        }

        idx = ++m_hyperlink_highest_used_idx;
        _vte_debug_print(vte::debug::category::HYPERLINK,
                         "get_hyperlink_idx: brand new idx {} for id;uri=\"{}\"",
                         idx, hyperlink);
        str = g_string_new_len (hyperlink, len);
        g_ptr_array_add(m_hyperlinks, str);

        vte_assert_cmpuint(m_hyperlink_highest_used_idx + 1, ==, m_hyperlinks->len);

        return idx;
}

/*
 * Find existing idx for the hyperlink or allocate a new one.
 *
 * Returns 0 if given no hyperlink or an empty one, or if the pool is full.
 * Returns the idx (either already existing or newly allocated) from 1 up to
 * VTE_HYPERLINK_COUNT_MAX inclusive otherwise.
 *
 * The current idx is also updated, in order not to be garbage collected.
 */
Ring::hyperlink_idx_t
Ring::get_hyperlink_idx(char const* hyperlink)
{
        /* Release current idx and do a round of GC to possibly purge its hyperlink,
         * even if new hyperlink is nullptr or empty. */
        m_hyperlink_current_idx = 0;
        hyperlink_gc();

        m_hyperlink_current_idx = get_hyperlink_idx_no_update_current(hyperlink);
        return m_hyperlink_current_idx;
}

void
Ring::freeze_row(row_t position,
                 VteRowData const* row)
{
	VteCell *cell;
	GString *buffer = m_utf8_buffer;
        GString *hyperlink;
	int i;
        gboolean froze_hyperlink = FALSE;

	_vte_debug_print(vte::debug::category::RING,
                         "Freezing row {}",
                         position);

        g_assert(m_has_streams);

	RowRecord record;
	memset(&record, 0, sizeof(record));
	record.text_start_offset = _vte_stream_head(m_text_stream);
	record.attr_start_offset = _vte_stream_head(m_attr_stream);
        record.width = row->len;
	record.is_ascii = 1;

	g_string_truncate (buffer, 0);
	for (i = 0, cell = row->cells; i < row->len; i++, cell++) {
		VteCellAttr attr;
		int num_chars;

		/* Attr storage:
		 *
		 * 1. We don't store attrs for fragments.  They can be
		 * reconstructed using the columns of their start cell.
		 *
		 * 2. We store one attr per vteunistr character starting
		 * from the second character, with columns=0.
		 *
		 * That's enough to reconstruct the attrs, and to store
		 * the text in real UTF-8.
		 */
		attr = cell->attr;
		if (G_LIKELY (!attr.fragment())) {
			CellAttrChange attr_change;
                        guint16 hyperlink_length;

			if (!m_last_attr.same_for_stream(attr)) {
				m_last_attr_text_start_offset = record.text_start_offset + buffer->len;
				memset(&attr_change, 0, sizeof (attr_change));
				attr_change.text_end_offset = m_last_attr_text_start_offset;
                                _attrcpy(&attr_change.attr, &m_last_attr);
                                hyperlink = hyperlink_get(m_last_attr.hyperlink_idx_or_none());
                                attr_change.attr.hyperlink_length = hyperlink->len;
				_vte_stream_append (m_attr_stream, (char const* ) &attr_change, sizeof (attr_change));
                                if (G_UNLIKELY (hyperlink->len != 0)) {
                                        _vte_stream_append (m_attr_stream, hyperlink->str, hyperlink->len);
                                        froze_hyperlink = TRUE;
                                }
                                hyperlink_length = attr_change.attr.hyperlink_length;
                                /* Image reference BEFORE the trailer: the
                                 * trailer must stay last for the backwards
                                 * walk. See attr_trailer() in ring.hh.
                                 */
                                if (G_UNLIKELY (m_last_attr.image()))
                                        append_stream_image_ref();
                                {
                                        auto const trailer = attr_trailer(hyperlink_length,
                                                                          m_last_attr.image());
                                        _vte_stream_append (m_attr_stream, (char const* ) &trailer, 2);
                                }

                                /* An image cell's reference, after the
                                 * hyperlink tail. Only the run's FIRST cell's
                                 * reference is written: the run is one stripe
                                 * of one image by construction, because
                                 * same_for_stream() excludes the tile column
                                 * from the run-length key precisely so that a
                                 * stripe is one run. The tile column is
                                 * recovered on thaw by counting.
                                 */
                                auto const has_image = m_last_attr.image();

				if (!buffer->len)
					/* This row doesn't use last_attr, adjust */
                                        record.attr_start_offset += attr_record_stride(hyperlink_length, has_image);
				m_last_attr = attr;
			}

			num_chars = _vte_unistr_strlen (cell->c);
			if (num_chars > 1) {
                                /* Combining chars */
				attr.set_columns(0);
				m_last_attr_text_start_offset = record.text_start_offset + buffer->len
								  + g_unichar_to_utf8 (_vte_unistr_get_base (cell->c), nullptr);
				memset(&attr_change, 0, sizeof (attr_change));
				attr_change.text_end_offset = m_last_attr_text_start_offset;
                                _attrcpy(&attr_change.attr, &m_last_attr);
                                hyperlink = hyperlink_get(m_last_attr.hyperlink_idx_or_none());
                                attr_change.attr.hyperlink_length = hyperlink->len;
				_vte_stream_append (m_attr_stream, (char const* ) &attr_change, sizeof (attr_change));
                                if (G_UNLIKELY (hyperlink->len != 0)) {
                                        _vte_stream_append (m_attr_stream, hyperlink->str, hyperlink->len);
                                        froze_hyperlink = TRUE;
                                }
                                hyperlink_length = attr_change.attr.hyperlink_length;
                                /* Image reference BEFORE the trailer: the
                                 * trailer must stay last for the backwards
                                 * walk. See attr_trailer() in ring.hh.
                                 */
                                if (G_UNLIKELY (m_last_attr.image()))
                                        append_stream_image_ref();
                                {
                                        auto const trailer = attr_trailer(hyperlink_length,
                                                                          m_last_attr.image());
                                        _vte_stream_append (m_attr_stream, (char const* ) &trailer, 2);
                                }
				m_last_attr = attr;
			}

			if (cell->c < 32 || cell->c > 126) record.is_ascii = 0;
			_vte_unistr_append_to_string (cell->c, buffer);
		}
	}
	if (!row->attr.soft_wrapped)
		g_string_append_c (buffer, '\n');
	record.soft_wrapped = row->attr.soft_wrapped;
        record.bidi_flags = row->attr.bidi_flags;

	_vte_stream_append(m_text_stream, buffer->str, buffer->len);
	append_row_record(&record, position);

        /* After freezing some hyperlinks, do a hyperlink GC. The constant is totally arbitrary, feel free to fine tune. */
        if (froze_hyperlink)
                hyperlink_maybe_gc(1024);
}

/* If do_truncate (data is placed back from the stream to the ring), real new hyperlink idxs are looked up or allocated.
 *
 * If !do_truncate (data is fetched only to be displayed), hyperlinked cells are given the pseudo idx VTE_HYPERLINK_IDX_TARGET_IN_STREAM,
 * except for the hyperlink_hover_idx which gets this real idx. This is important for hover underlining.
 *
 * Optionally updates the hyperlink parameter to point to the ring-owned hyperlink target. */
void
Ring::thaw_row(row_t position,
               VteRowData* row,
               bool do_truncate,
               int hyperlink_column,
               char const** hyperlink)
{
	RowRecord records[2], record;
	VteCellAttr attr;
	CellAttrChange attr_change;
        auto stream_image_ref = vte::image::Ref{};
        auto stream_image_tile_col = uint32_t{0};
	VteCell cell;
	char const* p, *q, *end;
	GString *buffer = m_utf8_buffer;
        char hyperlink_readbuf[VTE_HYPERLINK_TOTAL_LENGTH_MAX + 1];

        hyperlink_readbuf[0] = '\0';
        if (hyperlink) {
                m_hyperlink_buf[0] = '\0';
                *hyperlink = m_hyperlink_buf;
        }

	_vte_debug_print(vte::debug::category::RING,
                         "Thawing row {}",
                         position);

        g_assert(m_has_streams);

	_vte_row_data_clear (row);

	attr_change.text_end_offset = 0;

	if (!read_row_record(&records[0], position))
		return;
	if ((position + 1) * sizeof (records[0]) < _vte_stream_head (m_row_stream)) {
		if (!read_row_record(&records[1], position + 1))
			return;
	} else
		records[1].text_start_offset = _vte_stream_head (m_text_stream);

	g_string_set_size (buffer, records[1].text_start_offset - records[0].text_start_offset);
	if (!_vte_stream_read (m_text_stream, records[0].text_start_offset, buffer->str, buffer->len))
		return;

	record = records[0];

	if (G_LIKELY (buffer->len && buffer->str[buffer->len - 1] == '\n'))
                g_string_truncate (buffer, buffer->len - 1);
	else
		row->attr.soft_wrapped = TRUE;
        row->attr.bidi_flags = records[0].bidi_flags;

	p = buffer->str;
	end = p + buffer->len;
	while (p < end) {
		if (record.text_start_offset >= m_last_attr_text_start_offset) {
			attr = m_last_attr;
                        strcpy(hyperlink_readbuf, hyperlink_get(attr.hyperlink_idx_or_none())->str);
		} else {
			if (record.text_start_offset >= attr_change.text_end_offset) {
                                auto const record_start = record.attr_start_offset;
				if (!_vte_stream_read (m_attr_stream, record_start, (char *) &attr_change, sizeof (attr_change)))
					return;
                                vte_assert_cmpuint (attr_change.attr.hyperlink_length, <=, VTE_HYPERLINK_TOTAL_LENGTH_MAX);
                                if (attr_change.attr.hyperlink_length && !_vte_stream_read (m_attr_stream, record_start + sizeof (attr_change), hyperlink_readbuf, attr_change.attr.hyperlink_length))
                                        return;
                                hyperlink_readbuf[attr_change.attr.hyperlink_length] = '\0';

                                /* An image run stores its FIRST cell's
                                 * reference after the hyperlink tail; the tag
                                 * is in the attr word, which is persisted, so
                                 * the reader needs no out-of-band state.
                                 */
                                auto const has_image = record_has_image(attr_change);
                                stream_image_ref = vte::image::Ref{};
                                stream_image_tile_col = 0;
                                auto stream_image_pool_id = vte::image::k_ref_pool_id_none;
                                if (G_UNLIKELY (has_image)) {
                                        StreamImageRef sref;
                                        if (!_vte_stream_read (m_attr_stream,
                                                               record_start + sizeof (attr_change) +
                                                               attr_change.attr.hyperlink_length,
                                                               (char*) &sref, sizeof(sref)))
                                                return;

                                        stream_image_ref = vte::image::Ref{sref.ref_bits};

                                        /* Resolve the stable priority back to
                                         * a resident image. If it is still
                                         * here, the cell names it again by
                                         * whatever pool id it holds NOW - so
                                         * scrolling an image out of the
                                         * writable window and back does not
                                         * lose it.
                                         *
                                         * If it is gone, the cell stays an
                                         * image cell that resolves to nothing,
                                         * rather than to whatever now occupies
                                         * some recycled id.
                                         */
                                        if (sref.priority != 0) {
                                                auto const priority = size_t(sref.priority - 1);
                                                auto const it = m_image_map.find(priority);
                                                if (it != m_image_map.end()) {
                                                        stream_image_pool_id = it->second->get_pool_id();
                                                } else if (auto* const restored = restore_image(priority)) {
                                                        /* Evicted, but its pixels were kept. */
                                                        stream_image_pool_id = restored->get_pool_id();
                                                }
                                        }
                                }

                                record.attr_start_offset = record_start +
                                        attr_record_stride(attr_change.attr.hyperlink_length, has_image);

                                _attrcpy(&attr, &attr_change.attr);

                                if (G_UNLIKELY (has_image)) {
                                        /* The pool id is deliberately DROPPED.
                                         *
                                         * It named an entry in an in-memory
                                         * pool whose quarantine only tracks
                                         * cells in the writable rows, so by
                                         * the time this row is thawed that id
                                         * may already have been reclaimed and
                                         * handed to a different image -
                                         * exactly the aliasing the pool exists
                                         * to prevent. Keeping the tile
                                         * coordinates and dropping the id
                                         * gives a cell that still knows it is
                                         * part of an image, and which resolves
                                         * to no image rather than to the wrong
                                         * one.
                                         *
                                         * Restoring the picture itself needs
                                         * the pixels in the stream too, and a
                                         * fresh id allocated here; that does
                                         * not exist yet.
                                         */
                                        attr.set_image_ref(vte::image::Ref{
                                                stream_image_pool_id,
                                                stream_image_ref.tile_row(),
                                                stream_image_ref.tile_col()});
                                } else {
                                        attr.set_hyperlink_idx(0);
                                }
                                if (G_UNLIKELY (!has_image && attr_change.attr.hyperlink_length)) {
                                        if (do_truncate) {
                                                /* Find the existing idx or allocate a new one, just as when receiving an OSC 8 escape sequence.
                                                 * Do not update the current idx though. */
                                                attr.set_hyperlink_idx(get_hyperlink_idx_no_update_current(hyperlink_readbuf));
                                        } else {
                                                /* Use a special hyperlink idx, except if to be underlined because the hyperlink is the same as the hovered cell's. */
                                                attr.set_hyperlink_idx(VTE_HYPERLINK_IDX_TARGET_IN_STREAM);
                                                if (m_hyperlink_hover_idx != 0 && strcmp(hyperlink_readbuf, hyperlink_get(m_hyperlink_hover_idx)->str) == 0) {
                                                        /* FIXME here we're calling the expensive strcmp() above and get_hyperlink_idx_no_update_current() way too many times. */
                                                        attr.set_hyperlink_idx(get_hyperlink_idx_no_update_current(hyperlink_readbuf));
                                                }
                                        }
                                }
			}
		}

		cell.attr = attr;

                /* Recover this cell's tile column by counting from the run's
                 * first cell. The column is not stored per cell on purpose:
                 * doing so would make the run-length key vary per cell and
                 * cost one 26-byte record each, measured.
                 */
                if (G_UNLIKELY (cell.attr.image())) {
                        auto const base = cell.attr.image_ref();
                        cell.attr.set_image_ref(vte::image::Ref{base.pool_id(),
                                                                base.tile_row(),
                                                                stream_image_tile_col});
                        if (stream_image_tile_col < vte::image::k_ref_tile_col_max)
                                stream_image_tile_col++;
                }

                _VTE_DEBUG_IF(vte::debug::category::RING | vte::debug::category::HYPERLINK) {
                        /* Debug: Reverse the colors for the stream's contents. */
                        if (!do_truncate) {
                                cell.attr.attr ^= VTE_ATTR_REVERSE;
                        }
                }
		cell.c = g_utf8_get_char (p);

		q = g_utf8_next_char (p);
		record.text_start_offset += q - p;
		p = q;

		if (G_UNLIKELY (cell.attr.columns() == 0)) {
			if (G_LIKELY (row->len)) {
				/* Combine it */
				row->cells[row->len - 1].c = _vte_unistr_append_unichar (row->cells[row->len - 1].c, cell.c);
                                /* Spread it to all the previous cells of a potentially multicell character */
                                for (int i = row->len - 1; i >= 1 && row->cells[i].attr.fragment(); i--) {
                                        row->cells[i - 1].c = row->cells[i].c;
                                }
			} else {
				cell.attr.set_columns(1);
                                if (row->len == hyperlink_column && hyperlink != nullptr)
                                        *hyperlink = strcpy(m_hyperlink_buf, hyperlink_readbuf);
				_vte_row_data_append (row, &cell);
			}
		} else {
                        if (row->len == hyperlink_column && hyperlink != nullptr)
                                *hyperlink = strcpy(m_hyperlink_buf, hyperlink_readbuf);
			_vte_row_data_append (row, &cell);
			if (cell.attr.columns() > 1) {
				/* Add the fragments */
				int i, columns = cell.attr.columns();
				cell.attr.set_fragment(true);
				cell.attr.set_columns(1);
                                for (i = 1; i < columns; i++) {
                                        if (row->len == hyperlink_column && hyperlink != nullptr)
                                                *hyperlink = strcpy(m_hyperlink_buf, hyperlink_readbuf);
					_vte_row_data_append (row, &cell);
                                }
			}
		}
	}

        /* FIXME this is extremely complicated (by design), figure out something better.
           This is the only place where we need to walk backwards in attr_stream,
           which is the reason for the hyperlink's length being repeated after the hyperlink itself. */
	if (do_truncate) {
		gsize attr_stream_truncate_at = records[0].attr_start_offset;
		_vte_debug_print(vte::debug::category::RING, "Truncating");
		if (records[0].text_start_offset <= m_last_attr_text_start_offset) {
			/* Check the previous attr record. If its text ends where truncating, this attr record also needs to be removed. */
                        guint16 trailer;
                        if (_vte_stream_read (m_attr_stream, attr_stream_truncate_at - 2, (char *) &trailer, 2)) {
                                auto const hyperlink_length = trailer_length(trailer);
                                auto const tr_image = trailer_has_image(trailer);
                                vte_assert_cmpuint (hyperlink_length, <=, VTE_HYPERLINK_TOTAL_LENGTH_MAX);
                                if (_vte_stream_read (m_attr_stream, attr_stream_truncate_at - attr_record_stride(hyperlink_length, tr_image), (char *) &attr_change, sizeof (attr_change))) {
                                        if (records[0].text_start_offset == attr_change.text_end_offset) {
                                                _vte_debug_print(vte::debug::category::RING, "... at attribute change");
                                                attr_stream_truncate_at -= attr_record_stride(hyperlink_length, tr_image);
                                        }
				}
			}
			/* Reconstruct last_attr from the first record of attr_stream that we cut off,
			   last_attr_text_start_offset from the last record that we keep. */
			if (_vte_stream_read (m_attr_stream, attr_stream_truncate_at, (char *) &attr_change, sizeof (attr_change))) {
                                _attrcpy(&m_last_attr, &attr_change.attr);
                                m_last_attr.set_hyperlink_idx(0);
                                if (attr_change.attr.hyperlink_length && _vte_stream_read (m_attr_stream, attr_stream_truncate_at + sizeof (attr_change), (char *) &hyperlink_readbuf, attr_change.attr.hyperlink_length)) {
                                        hyperlink_readbuf[attr_change.attr.hyperlink_length] = '\0';
                                        m_last_attr.set_hyperlink_idx(get_hyperlink_idx(hyperlink_readbuf));
                                }
                                if (_vte_stream_read (m_attr_stream, attr_stream_truncate_at - 2, (char *) &trailer, 2)) {
                                        auto const prev_length = trailer_length(trailer);
                                        auto const prev_image = trailer_has_image(trailer);
                                        vte_assert_cmpuint (prev_length, <=, VTE_HYPERLINK_TOTAL_LENGTH_MAX);
                                        if (_vte_stream_read (m_attr_stream, attr_stream_truncate_at - attr_record_stride(prev_length, prev_image), (char *) &attr_change, sizeof (attr_change))) {
                                                m_last_attr_text_start_offset = attr_change.text_end_offset;
                                        } else {
                                                m_last_attr_text_start_offset = 0;
                                        }
				} else {
					m_last_attr_text_start_offset = 0;
				}
			} else {
				m_last_attr_text_start_offset = 0;
				m_last_attr = basic_cell.attr;
			}
		}
		_vte_stream_truncate (m_row_stream, position * sizeof (record));
		_vte_stream_truncate (m_attr_stream, attr_stream_truncate_at);
		_vte_stream_truncate (m_text_stream, records[0].text_start_offset);
	}
}

void
Ring::reset_streams(row_t position)
{
	_vte_debug_print(vte::debug::category::RING,
                         "Reseting streams to {}",
                         position);

	if (m_has_streams) {
		_vte_stream_reset(m_row_stream, position * sizeof(RowRecord));
                _vte_stream_reset(m_text_stream, _vte_stream_head(m_text_stream));
                _vte_stream_reset(m_attr_stream, _vte_stream_head(m_attr_stream));
	}

	m_last_attr_text_start_offset = 0;
	m_last_attr = basic_cell.attr;
}

Ring::row_t
Ring::reset()
{
        _vte_debug_print(vte::debug::category::RING,
                         "Reseting the ring at {}",
                         m_end);

        reset_streams(m_end);
        m_start = m_writable = m_end;
        m_cached_row_num = (row_t)-1;

#if WITH_SIXEL
        m_image_by_top_map.clear();
        m_image_map.clear();
        m_next_image_priority = 0;
        m_image_fast_memory_used = 0;
        m_placing_image = nullptr;
        m_images_changed = false;
        sync_has_images();
#endif

        return m_end;
}

VteRowData const*
Ring::index(row_t position)
{
	if (G_LIKELY (position >= m_writable))
		return get_writable_index(position);

	if (m_cached_row_num != position) {
		_vte_debug_print(vte::debug::category::RING,
                                 "Caching row {}",
                                 position);
                thaw_row(position, &m_cached_row, false, -1, nullptr);
		m_cached_row_num = position;
	}

	return &m_cached_row;
}

bool
Ring::is_soft_wrapped(row_t position)
{
        const VteRowData *row;
        RowRecord record;

        if (G_UNLIKELY (position < m_start || position >= m_end))
                return false;

        if (G_LIKELY (position >= m_writable)) {
                row = get_writable_index(position);
                return row->attr.soft_wrapped;
        }

        /* The row is scrolled out to the stream. Save work by not reading the actual row.
         * The requested information is readily available in row_stream, too. */
        if (G_UNLIKELY (!read_row_record(&record, position)))
                return false;
        return record.soft_wrapped;
}

/* Returns whether the given visual row contains the beginning of a prompt, i.e.
 * contains a prompt character which is immediately preceded by either a hard newline
 * or a non-prompt character (possibly at the end of previous, soft wrapped row).
 *
 * This way we catch soft wrapped multiline prompts at their first line only,
 * and catch prompts that do not begin at the beginning of a row.
 *
 * FIXME extend support for deliberately multiline (hard wrapped) prompts:
 * https://gitlab.gnome.org/GNOME/vte/-/issues/2681#note_1904004
 *
 * FIXME this is very slow, it unnecessarily reads text_stream
 * in which we're not interested at all. Implement a faster algorithm. */
bool
Ring::contains_prompt_beginning(row_t position)
{
        const VteRowData *row = index(position);
        if (row == NULL || row->len == 0) {
                return false;
        }

        /* First check the places where the previous character is also readily available. */
        int col = 0;
        while (col < row->len && row->cells[col].attr.shellintegration() == ShellIntegrationMode::ePROMPT) {
                col++;
        }
        while (col < row->len && row->cells[col].attr.shellintegration() != ShellIntegrationMode::ePROMPT) {
                col++;
        }
        if (col < row->len) {
                return true;
        }

        /* Finally check the first character where we might need to look at the previous row. */
        if (row->cells[0].attr.shellintegration() == ShellIntegrationMode::ePROMPT) {
                row = index(position - 1);
                if (row == NULL ||
                    !row->attr.soft_wrapped ||
                    (row->len >= 1 /* this is guaranteed beucase soft_wrapped */ &&
                     row->cells[row->len - 1].attr.shellintegration() != ShellIntegrationMode::ePROMPT)) {
                        return true;
                }
        }
        return false;
}

/*
 * Returns the hyperlink idx at the given position.
 *
 * Updates the hyperlink parameter to point to the hyperlink's target.
 * The buffer is owned by the ring and must not be modified by the caller.
 *
 * Optionally also updates the internal concept of the hovered idx. In this case,
 * a real idx is looked up or newly allocated in the hyperlink pool even if the
 * cell is scrolled out to the streams.
 * This is to be able to underline all cells that share the same hyperlink.
 *
 * Otherwise cells from the stream might get the pseudo idx VTE_HYPERLINK_IDX_TARGET_IN_STREAM.
 */
Ring::hyperlink_idx_t
Ring::get_hyperlink_at_position(row_t position,
                                column_t col,
                                bool update_hover_idx,
                                char const** hyperlink)
{
        hyperlink_idx_t idx;
        char const* hp;

        if (hyperlink == nullptr)
                hyperlink = &hp;
        *hyperlink = nullptr;

        if (update_hover_idx) {
                /* Invalidate the cache because new hover idx might result in new idxs to report. */
                m_cached_row_num = (row_t)-1;
        }

        if (G_UNLIKELY (!contains(position) || col < 0)) {
                if (update_hover_idx)
                        m_hyperlink_hover_idx = 0;
                return 0;
        }

        if (G_LIKELY (position >= m_writable)) {
                VteRowData* row = get_writable_index(position);
                if (col >= _vte_row_data_length(row)) {
                        if (update_hover_idx)
                                m_hyperlink_hover_idx = 0;
                        return 0;
                }
                *hyperlink = hyperlink_get(row->cells[col].attr.hyperlink_idx_or_none())->str;
                idx = row->cells[col].attr.hyperlink_idx_or_none();
        } else {
                thaw_row(position, &m_cached_row, false, col, hyperlink);
                /* Note: Intentionally don't set cached_row_num. We're about to update
                 * m_hyperlink_hover_idx which makes some idxs no longer valid. */
                idx = get_hyperlink_idx_no_update_current(*hyperlink);
        }
        if (**hyperlink == '\0')
                *hyperlink = nullptr;
        if (update_hover_idx)
                m_hyperlink_hover_idx = idx;
        return idx;
}

void
Ring::freeze_one_row()
{
	VteRowData* row;

	if (G_UNLIKELY (m_writable == m_start))
		reset_streams(m_writable);

	row = get_writable_index(m_writable);
	freeze_row(m_writable, row);

	m_writable++;
}

void
Ring::thaw_one_row()
{
	VteRowData* row;

	vte_assert_cmpuint(m_start, <, m_writable);

	ensure_writable_room();

	m_writable--;

	if (m_writable == m_cached_row_num)
		m_cached_row_num = (row_t)-1; /* Invalidate cached row */

	row = get_writable_index(m_writable);
        thaw_row(m_writable, row, true, -1, nullptr);
}

void
Ring::discard_one_row()
{
	m_start++;
#if WITH_SIXEL
        drop_images_before(m_start);
#endif
	if (G_UNLIKELY(m_start == m_writable)) {
		reset_streams(m_writable);
	} else if (m_start < m_writable) {
                /* Advance the tail sometimes. Not always, in order to slightly improve performance. */
                if (m_start % 256 == 0) {
                        RowRecord record;
                        _vte_stream_advance_tail(m_row_stream, m_start * sizeof (record));
                        if (G_LIKELY(read_row_record(&record, m_start))) {
                                _vte_stream_advance_tail(m_text_stream, record.text_start_offset);
                                _vte_stream_advance_tail(m_attr_stream, record.attr_start_offset);
                        }
                        reclaim_image_spill(m_start);
                }
	} else {
		m_writable = m_start;
	}
}

void
Ring::maybe_freeze_one_row()
{
        /* See the comment about m_visible_rows + 1 at ensure_writable_room(). */
        if (G_LIKELY(m_mask >= m_visible_rows + 1 &&
                     m_writable + m_mask + 1 == m_end))
		freeze_one_row();
	else
		ensure_writable_room();
}

//FIXMEchpe maybe inline this one
void
Ring::maybe_discard_one_row()
{
	if (length() == m_max)
		discard_one_row();
}

void
Ring::ensure_writable_room()
{
	row_t new_mask, old_mask, i, end;
	VteRowData* old_array, *new_array;;

        /* Keep at least m_visible_rows + 1 rows in the ring.
         * The BiDi spec requires that the just scrolled out row
         * is still alterable (can be switched to hard line ending).
         * It's nice anyway to make that hard wrapped upon a clear. */
        if (G_LIKELY(m_mask >= m_visible_rows + 1 &&
                     m_writable + m_mask + 1 > m_end))
		return;

	old_mask = m_mask;
	old_array = m_array;

	do {
		m_mask = (m_mask << 1) + 1;
        } while (m_mask < m_visible_rows + 1 || m_writable + m_mask + 1 <= m_end);

	_vte_debug_print(vte::debug::category::RING,
                         "Enlarging writable array from {} to {}",
                         old_mask, m_mask);

	m_array = (VteRowData* ) g_malloc0(sizeof (m_array[0]) * (m_mask + 1));

	new_mask = m_mask;
	new_array = m_array;

	end = m_writable + old_mask + 1;
	for (i = m_writable; i < end; i++)
		new_array[i & new_mask] = old_array[i & old_mask];

	g_free (old_array);
}

/**
 * Ring::resize:
 * @max_rows: new maximum numbers of rows in the ring
 *
 * Changes the number of lines the ring can contain.
 */
void
Ring::resize(row_t max_rows)
{
	_vte_debug_print(vte::debug::category::RING,
                         "Resizing to {}",
                         max_rows);

	validate();

	/* Adjust the start of tail chunk now */
	if (length() > max_rows) {
		m_start = m_end - max_rows;
		if (m_start >= m_writable) {
			reset_streams(m_writable);
			m_writable = m_start;
		}

#if WITH_SIXEL
                /* Lowering the maximum drops rows off the front in one step - the
                 * same rows discard_one_row() drops one at a time - so the images
                 * anchored to them go the same way. This was the last of the
                 * row-destroying paths not routed through the image maps, and the
                 * images it stranded kept holding image memory too, so they went on
                 * distorting the budget that evicts the live ones.
                 *
                 * Two ordinary things reach it with rows to drop: lowering the
                 * scrollback-lines setting, and - when the scrollback is off, so
                 * that the maximum IS the row count - making the window shorter.
                 * A window resize with a scrollback is not one of them, since
                 * screen_set_size() has already brought the length down through
                 * rewrap() and shrink(), which do their own image bookkeeping.
                 */
                if (has_images())
                        drop_images_before(m_start);
#endif
	}

	m_max = max_rows;

	validate();
}

void
Ring::shrink(row_t max_len)
{
	if (length() <= max_len)
		return;

	_vte_debug_print(vte::debug::category::RING,
                         "Shrinking to {}",
                         max_len);

	validate();

	if (m_writable - m_start <= max_len)
		m_end = m_start + max_len;
	else {
		while (m_writable - m_start > max_len) {
			ensure_writable(m_writable - 1);
			m_end = m_writable;
		}
	}

	/* TODO May want to shrink down m_array */

#if WITH_SIXEL
        /* The rows past the new end are gone; an image anchored to one of them
         * would keep being drawn at a row number the ring no longer has. */
        if (has_images())
                drop_images_after(m_end);
#endif

	validate();
}

/**
 * Ring::insert:
 * @position: an index
 *
 * Inserts a new, empty, row into @ring at the @position'th offset.
 * The item at that position and any items after that are shifted down.
 *
 * Return: the newly added row.
 */
VteRowData*
Ring::insert(row_t position, guint8 bidi_flags)
{
	row_t i;
	VteRowData* row, tmp;

	_vte_debug_print(vte::debug::category::RING,
                         "Inserting at position {}",
                         position);
	validate();

	maybe_discard_one_row();
	ensure_writable(position);
	ensure_writable_room();

	vte_assert_cmpuint (position, >=, m_writable);
	vte_assert_cmpuint (position, <=, m_end);

#if WITH_SIXEL
        /* After maybe_discard_one_row(), so that the maps are already pruned of
         * whatever left the front of the ring. */
        if (has_images())
                shift_images_for_insert(position);
#endif

        //FIXMEchpe WTF use better data structures!
	tmp = *get_writable_index(m_end);
	for (i = m_end; i > position; i--)
		*get_writable_index(i) = *get_writable_index(i - 1);
	*get_writable_index(position) = tmp;

	row = get_writable_index(position);
	_vte_row_data_clear (row);
        row->attr.bidi_flags = bidi_flags;
	m_end++;

	maybe_freeze_one_row();
        validate();
	return row;
}

/**
 * Ring::remove:
 * @position: an index
 *
 * Removes the @position'th item from @ring.
 */
void
Ring::remove(row_t position)
{
	row_t i;
	VteRowData tmp;

	_vte_debug_print(vte::debug::category::RING,
                         "Removing item at position {}",
                         position);
        validate();

	if (G_UNLIKELY(!contains(position)))
		return;

	ensure_writable(position);

#if WITH_SIXEL
        if (has_images())
                shift_images_for_remove(position);
#endif

        //FIXMEchpe WTF as above
	tmp = *get_writable_index(position);
	for (i = position; i < m_end - 1; i++)
		*get_writable_index(i) = *get_writable_index(i + 1);
	*get_writable_index(m_end - 1) = tmp;

	if (m_end > m_writable)
		m_end--;

        validate();
}


/**
 * Ring::append:
 * @data: the new item
 *
 * Appends a new item to the ring.
 *
 * Return: the newly added row.
 */
VteRowData*
Ring::append(guint8 bidi_flags)
{
        return insert(next(), bidi_flags);
}


/**
 * Ring::drop_scrollback:
 * @position: drop contents up to this point, which must be in the writable region.
 *
 * Drop the scrollback (offscreen contents).
 *
 * TODOegmont: We wouldn't need the position argument after addressing 708213#c29.
 */
void
Ring::drop_scrollback(row_t position)
{
        ensure_writable(position);

        m_start = m_writable = position;
#if WITH_SIXEL
        drop_images_before(m_start);
#endif
        reset_streams(position);
}

/**
 * Ring::set_visible_rows:
 * @rows: the number of visible rows
 *
 * Set the number of visible rows.
 * It's required to be set correctly for the alternate screen so that it
 * never hits the streams. It's also required for clearing the scrollback.
 */
void
Ring::set_visible_rows(row_t rows)
{
        m_visible_rows = rows;
}


/* Convert a (row,col) into a CellTextOffset.
 * Requires the row to be frozen, or be outsize the range covered by the ring.
 */
bool
Ring::frozen_row_column_to_text_offset(row_t position,
				       column_t column,
				       CellTextOffset* offset)
{
	RowRecord records[2];
	VteCell *cell;
	GString *buffer = m_utf8_buffer;
	VteRowData const* row;
	unsigned int i, num_chars, off;

	if (position >= m_end) {
		offset->text_offset = _vte_stream_head(m_text_stream) + position - m_end;
		offset->fragment_cells = 0;
		offset->eol_cells = column;
		return true;
	}

	if (G_UNLIKELY (position < m_start)) {
		/* This happens when the marker (saved cursor position) is
		   scrolled off at the top of the scrollback buffer. */
		position = m_start;
		column = 0;
		/* go on */
	}

	vte_assert_cmpuint(position, <, m_writable);
	if (!read_row_record(&records[0], position))
		return false;
	if ((position + 1) * sizeof (records[0]) < _vte_stream_head(m_row_stream)) {
		if (!read_row_record(&records[1], position + 1))
			return false;
	} else
		records[1].text_start_offset = _vte_stream_head(m_text_stream);

	offset->fragment_cells = 0;
	offset->eol_cells = -1;
	offset->text_offset = records[0].text_start_offset;

        /* Save some work if we're in column 0. This holds true for images, whose column
         * positions are disregarded for the purposes of wrapping. */
        if (column == 0)
                return true;

        g_string_set_size (buffer, records[1].text_start_offset - records[0].text_start_offset);
	if (!_vte_stream_read(m_text_stream, records[0].text_start_offset, buffer->str, buffer->len))
		return false;

	if (G_LIKELY (buffer->len && buffer->str[buffer->len - 1] == '\n'))
                g_string_truncate (buffer, buffer->len - 1);

	row = index(position);

	/* row and buffer now contain the same text, in different representation */

	/* count the number of characters up to the given column */
	num_chars = 0;
	for (i = 0, cell = row->cells; i < row->len && i < column; i++, cell++) {
		if (G_LIKELY (!cell->attr.fragment())) {
			if (G_UNLIKELY (i + cell->attr.columns() > column)) {
				offset->fragment_cells = column - i;
				break;
			}
			num_chars += _vte_unistr_strlen(cell->c);
		}
	}
	if (i >= row->len) {
		offset->eol_cells = column - i;
	}

	/* count the number of UTF-8 bytes for the given number of characters */
	off = 0;
	while (num_chars > 0 && off < buffer->len) {
		off++;
		if ((buffer->str[off] & 0xC0) != 0x80) num_chars--;
	}
	offset->text_offset += off;
	return true;
}


/* Given a row number and a CellTextOffset, compute the column within that row.
   It's the caller's responsibility to ensure that CellTextOffset really falls into that row.
   Requires the row to be frozen, or be outsize the range covered by the ring.
 */
bool
Ring::frozen_row_text_offset_to_column(row_t position,
				       CellTextOffset const* offset,
				       column_t* column)
{
	RowRecord records[2];
	VteCell *cell;
	GString *buffer = m_utf8_buffer;
	VteRowData const* row;
	unsigned int i, off, num_chars, nc;

	if (position >= m_end) {
		*column = offset->eol_cells;
		return true;
	}

	if (G_UNLIKELY (position < m_start)) {
		/* This happens when the marker (saved cursor position) is
		   scrolled off at the top of the scrollback buffer. */
		*column = 0;
		return true;
	}

	vte_assert_cmpuint(position, <, m_writable);
	if (!read_row_record(&records[0], position))
		return false;
	if ((position + 1) * sizeof (records[0]) < _vte_stream_head(m_row_stream)) {
		if (!read_row_record(&records[1], position + 1))
			return false;
	} else
		records[1].text_start_offset = _vte_stream_head (m_text_stream);

	g_string_set_size (buffer, records[1].text_start_offset - records[0].text_start_offset);
	if (!_vte_stream_read(m_text_stream, records[0].text_start_offset, buffer->str, buffer->len))
		return false;

	if (G_LIKELY (buffer->len && buffer->str[buffer->len - 1] == '\n'))
                g_string_truncate (buffer, buffer->len - 1);

        /* Now that we've chopped off the likely trailing newline (which is only rarely missing,
         * if the ring ends in a soft wrapped line; see bug 181), the position we're about to
         * locate can be anywhere in the string, including just after its last character,
         * but not beyond that. */
        vte_assert_cmpuint(offset->text_offset, >=, records[0].text_start_offset);
        vte_assert_cmpuint(offset->text_offset, <=, records[0].text_start_offset + buffer->len);

	row = index(position);

	/* row and buffer now contain the same text, in different representation */

	/* count the number of characters for the given UTF-8 text offset */
	off = offset->text_offset - records[0].text_start_offset;
	num_chars = 0;
	for (i = 0; i < off; i++) {
		if ((buffer->str[i] & 0xC0) != 0x80) num_chars++;
	}

	/* count the number of columns for the given number of characters */
	for (i = 0, cell = row->cells; i < row->len; i++, cell++) {
		if (G_LIKELY (!cell->attr.fragment())) {
			if (num_chars == 0) break;
			nc = _vte_unistr_strlen(cell->c);
			if (nc > num_chars) break;
			num_chars -= nc;
		}
	}

	/* always add fragment_cells, but add eol_cells only if we're at eol */
	i += offset->fragment_cells;
	if (G_UNLIKELY (offset->eol_cells >= 0 && i == row->len))
		i += offset->eol_cells;
	*column = i;
	return true;
}


/**
 * Ring::rewrap:
 * @columns: new number of columns
 * @markers: 0-terminated array of #VteVisualPosition
 *
 * Reflow the @ring to match the new number of @columns.
 * For all @markers, find the cell at that position and update them to
 * reflect the cell's new position.
 */
/* See ../doc/rewrap.txt for design and implementation details. */
void
Ring::rewrap(column_t columns,
             VteVisualPosition** markers)
{
	row_t old_row_index, new_row_index;
	int i;
	int num_markers = 0;
	CellTextOffset *marker_text_offsets;
	VteVisualPosition *new_markers;
	RowRecord old_record;
	CellAttrChange attr_change;
	VteStream *new_row_stream;
	gsize paragraph_start_text_offset;
	gsize paragraph_end_text_offset;
	gsize paragraph_len;  /* excluding trailing '\n' */
	gsize attr_offset;
	gsize old_ring_end;

	if (G_UNLIKELY(length() == 0))
		return;
	_vte_debug_print(vte::debug::category::RING,
                         "Ring before rewrapping:");
        validate();
	new_row_stream = _vte_file_stream_new();

	/* Freeze everything, because rewrapping is really complicated and we don't want to
	   duplicate the code for frozen and thawed rows. */
	while (m_writable < m_end)
		freeze_one_row();

#if WITH_SIXEL
	/* Take back the images the reflow is about to tear apart before anything is
	   re-anchored, so that the walk below only ever sees survivors. Reads the row
	   records, so it cannot run any earlier than this. */
	drop_images_torn_by_rewrap(columns);

	auto image_it = m_image_by_top_map.begin();

	/* The spilled images get the same treatment, from the same row records.
	   They are not in the by-top map - being spilled is precisely being out
	   of it - so they need their own plan, taken here while the old row
	   stream can still answer where their rows are. */
	auto spill_plan = plan_spill_rewrap(columns);
	auto spill_cursor = size_t{0};
#endif

	/* For markers given as (row,col) pairs find their offsets in the text stream.
	   This code requires that the rows are already frozen. */
	while (markers[num_markers] != nullptr)
		num_markers++;
	marker_text_offsets = (CellTextOffset *) g_malloc(num_markers * sizeof (marker_text_offsets[0]));
	new_markers = (VteVisualPosition *) g_malloc(num_markers * sizeof (new_markers[0]));
	for (i = 0; i < num_markers; i++) {
		/* Convert visual column into byte offset */
		if (!frozen_row_column_to_text_offset(markers[i]->row, markers[i]->col, &marker_text_offsets[i]))
			goto err;
		new_markers[i].row = new_markers[i].col = -1;
		_vte_debug_print(vte::debug::category::RING,
                                 "Marker #{} old coords:  row {}  col {}  ->  text_offset {} fragment_cells {}  eol_cells {}",
                                 i,
                                 markers[i]->row,
                                 markers[i]->col,
                                 marker_text_offsets[i].text_offset,
                                 marker_text_offsets[i].fragment_cells,
                                 marker_text_offsets[i].eol_cells);
	}

	/* Prepare for rewrapping */
	if (!read_row_record(&old_record, m_start))
		goto err;
	paragraph_start_text_offset = old_record.text_start_offset;
	paragraph_end_text_offset = _vte_stream_head(m_text_stream);  /* initialized to silence gcc */
	new_row_index = 0;

	attr_offset = old_record.attr_start_offset;
	if (!_vte_stream_read(m_attr_stream, attr_offset, (char *) &attr_change, sizeof (attr_change))) {
                _attrcpy(&attr_change.attr, &m_last_attr);
                attr_change.attr.hyperlink_length = hyperlink_get(m_last_attr.hyperlink_idx_or_none())->len;
		attr_change.text_end_offset = _vte_stream_head(m_text_stream);
	}

	old_row_index = m_start + 1;
	while (paragraph_start_text_offset < _vte_stream_head(m_text_stream)) {
		/* Find the boundaries of the next paragraph */
                gsize paragraph_width = 0;
		gboolean prev_record_was_soft_wrapped = FALSE;
		gboolean paragraph_is_ascii = TRUE;
                guint8 paragraph_bidi_flags = old_record.bidi_flags;
		gsize text_offset = paragraph_start_text_offset;
		RowRecord new_record;
		column_t col = 0;

		_vte_debug_print(vte::debug::category::RING,
				"  Old paragraph:  row {}  (text_offset {})  up to (exclusive)",
                                 old_row_index - 1,
                                 paragraph_start_text_offset);
		while (old_row_index <= m_end) {
                        paragraph_width += old_record.width;
			prev_record_was_soft_wrapped = old_record.soft_wrapped;
			paragraph_is_ascii = paragraph_is_ascii && old_record.is_ascii;
			if (G_LIKELY (old_row_index < m_end)) {
				if (!read_row_record(&old_record, old_row_index))
					goto err;
				paragraph_end_text_offset = old_record.text_start_offset;
			} else {
				paragraph_end_text_offset = _vte_stream_head (m_text_stream);
			}
			old_row_index++;
			if (!prev_record_was_soft_wrapped)
				break;
		}

		paragraph_len = paragraph_end_text_offset - paragraph_start_text_offset;
		if (!prev_record_was_soft_wrapped)  /* The last paragraph can be soft wrapped! */
			paragraph_len--;  /* Strip trailing '\n' */
		_vte_debug_print(vte::debug::category::RING,
				"  row {}  (text_offset {}){}  len {}  is_ascii {}",
                                 old_row_index - 1,
                                 paragraph_end_text_offset,
                                 prev_record_was_soft_wrapped ? "  soft_wrapped" : "",
                                 paragraph_len,
                                 paragraph_is_ascii);
		/* Wrap the paragraph */
		if (attr_change.text_end_offset <= text_offset) {
			/* Attr change at paragraph boundary, advance to next attr. */
                        attr_offset += attr_record_stride(attr_change.attr.hyperlink_length,
                                                          record_has_image(attr_change));
			if (!_vte_stream_read(m_attr_stream, attr_offset, (char *) &attr_change, sizeof (attr_change))) {
                                _attrcpy(&attr_change.attr, &m_last_attr);
                                attr_change.attr.hyperlink_length = hyperlink_get(m_last_attr.hyperlink_idx_or_none())->len;
				attr_change.text_end_offset = _vte_stream_head(m_text_stream);
			}
		}
		memset(&new_record, 0, sizeof (new_record));
		new_record.text_start_offset = text_offset;
		new_record.attr_start_offset = attr_offset;
		new_record.is_ascii = paragraph_is_ascii;
                new_record.bidi_flags = paragraph_bidi_flags;

		while (paragraph_len > 0) {
			/* Wrap one continuous run of identical attributes within the paragraph. */
			gsize runlength;  /* number of bytes we process in one run: identical attributes, within paragraph */
			if (attr_change.text_end_offset <= text_offset) {
				/* Attr change at line boundary, advance to next attr. */
                                attr_offset += attr_record_stride(attr_change.attr.hyperlink_length,
                                                          record_has_image(attr_change));
				if (!_vte_stream_read(m_attr_stream, attr_offset, (char *) &attr_change, sizeof (attr_change))) {
                                        _attrcpy(&attr_change.attr, &m_last_attr);
                                        attr_change.attr.hyperlink_length = hyperlink_get(m_last_attr.hyperlink_idx_or_none())->len;
					attr_change.text_end_offset = _vte_stream_head(m_text_stream);
				}
			}
			runlength = MIN(paragraph_len, attr_change.text_end_offset - text_offset);

                        if (paragraph_width <= (gsize) columns) {
                                /* Quick shortcut code path if the entire paragraph fits in one row. */
                                text_offset += runlength;
                                paragraph_len -= runlength;
                                /* The setting of "col" here is hacky. This very code here is potentially executed
                                   multiple times within a single paragraph, if it has attribute changes. The code above
                                   that reads the next attribute record has to iterate through those changes. Yet, we
                                   don't want to waste time tracking those attribute changes and finding their
                                   corresponding text offsets, we don't even want to read the text, as we won't need
                                   that. We rely on the fact that "paragraph_width" and "columns" are constants
                                   thoughout the wrapping of a particular paragraph, hence if this branch is hit once
                                   then it is hit every time; also "col" is unused then in this loop and only needs to
                                   have the correct value after we leave the loop. So each time simply set "col"
                                   straight away to its final value. */
                                col = paragraph_width;
                        } else if (G_UNLIKELY (attr_change.attr.columns() == 0)) {
				/* Combining characters all fit in the current row */
				text_offset += runlength;
				paragraph_len -= runlength;
			} else {
				while (runlength) {
					if (col >= columns - attr_change.attr.columns() + 1) {
						/* Wrap now, write the soft wrapped row's record */
                                                new_record.width = col;
						new_record.soft_wrapped = 1;
						_vte_stream_append(new_row_stream, (char const* ) &new_record, sizeof (new_record));
						_vte_debug_print(vte::debug::category::RING,
                                                                 "    New row {}  text_offset {}  attr_offset {}  soft_wrapped",
                                                                 new_row_index,
                                                                 new_record.text_start_offset,
                                                                 new_record.attr_start_offset);
						for (i = 0; i < num_markers; i++) {
							if (G_UNLIKELY (marker_text_offsets[i].text_offset >= new_record.text_start_offset &&
									marker_text_offsets[i].text_offset < text_offset)) {
								new_markers[i].row = new_row_index;
								_vte_debug_print(vte::debug::category::RING,
										"      Marker #{} will be here in row {}",
                                                                                 i,
                                                                                 new_row_index);
							}
						}

#if WITH_SIXEL
						rewrap_images_in_range(image_it,
                                                                       new_record.text_start_offset,
                                                                       text_offset,
                                                                       new_row_index);
						rewrap_spills_in_range(spill_plan,
                                                                       spill_cursor,
                                                                       new_record.text_start_offset,
                                                                       text_offset,
                                                                       new_row_index);
#endif

						new_row_index++;
						new_record.text_start_offset = text_offset;
						new_record.attr_start_offset = attr_offset;
						col = 0;
					}
					if (paragraph_is_ascii) {
						/* Shortcut for quickly wrapping ASCII (excluding TAB) text.
						   Don't read text_stream, and advance by a whole row of characters. */
						int len = MIN(runlength, (gsize) (columns - col));
						col += len;
						text_offset += len;
						paragraph_len -= len;
						runlength -= len;
					} else {
						/* Process one character only. */
						char textbuf[6];  /* fits at least one UTF-8 character */
						int textbuf_len;
						col += attr_change.attr.columns();
						/* Find beginning of next UTF-8 character */
						text_offset++; paragraph_len--; runlength--;
						textbuf_len = MIN(runlength, sizeof (textbuf));
						if (!_vte_stream_read(m_text_stream, text_offset, textbuf, textbuf_len))
							goto err;
						for (i = 0; i < textbuf_len && (textbuf[i] & 0xC0) == 0x80; i++) {
							text_offset++; paragraph_len--; runlength--;
						}
					}
				}
			}
		}

		/* Write the record of the paragraph's last row. */
		/* Hard wrapped, except maybe at the end of the very last paragraph */
                new_record.width = col;
		new_record.soft_wrapped = prev_record_was_soft_wrapped;
		_vte_stream_append(new_row_stream, (char const* ) &new_record, sizeof (new_record));
		_vte_debug_print(vte::debug::category::RING,
                                 "    New row {}  text_offset {}  attr_offset {}",
                                 new_row_index,
                                 new_record.text_start_offset,
                                 new_record.attr_start_offset);
		for (i = 0; i < num_markers; i++) {
			if (G_UNLIKELY (marker_text_offsets[i].text_offset >= new_record.text_start_offset &&
					marker_text_offsets[i].text_offset < paragraph_end_text_offset)) {
				new_markers[i].row = new_row_index;
				_vte_debug_print(vte::debug::category::RING,
                                                 "      Marker #{} will be here in row {}",
                                                 i,
                                                 new_row_index);
			}
		}

#if WITH_SIXEL
		rewrap_images_in_range(image_it,
                                       new_record.text_start_offset,
                                       paragraph_end_text_offset,
                                       new_row_index);
		rewrap_spills_in_range(spill_plan,
                                       spill_cursor,
                                       new_record.text_start_offset,
                                       paragraph_end_text_offset,
                                       new_row_index);
#endif

		new_row_index++;
		paragraph_start_text_offset = paragraph_end_text_offset;
	}

	/* Update the ring. */
	old_ring_end = m_end;
	g_object_unref(m_row_stream);
	m_row_stream = new_row_stream;
	m_writable = m_end = new_row_index;
	m_start = 0;
	if (m_end > m_max)
		m_start = m_end - m_max;
	m_cached_row_num = (row_t) -1;

	/* Find the markers. This requires that the ring is already updated. */
	for (i = 0; i < num_markers; i++) {
		/* Compute the row for markers beyond the ring */
		if (new_markers[i].row == -1)
			new_markers[i].row = markers[i]->row - old_ring_end + m_end;
		/* Convert byte offset into visual column */
                if (!frozen_row_text_offset_to_column(new_markers[i].row, &marker_text_offsets[i], &new_markers[i].col)) {
                        /* This really shouldn't happen. It's too late to "goto err", the old stream is closed, the ring is updated.
                         * It would be a bit cumbersome to refactor the code to still revert here. Choose a simple solution. */
                        new_markers[i].col = 0;
                }
		_vte_debug_print(vte::debug::category::RING,
                                 "Marker #{} new coords:  text_offset {}  fragment_cells {}  eol_cells {}  ->  row {}  col {}",
                                 i,
                                 marker_text_offsets[i].text_offset,
                                 marker_text_offsets[i].fragment_cells,
                                 marker_text_offsets[i].eol_cells,
                                 new_markers[i].row, new_markers[i].col);
		markers[i]->row = new_markers[i].row;
		markers[i]->col = new_markers[i].col;
	}
	g_free(marker_text_offsets);
	g_free(new_markers);

#if WITH_SIXEL
        try {
                rebuild_image_top_map();
        } catch (...) {
                vte::log_exception();
        }

        /* Reflow can make the content longer than the ring holds, in which case the
         * update above has just moved m_start forward and dropped rows off the front.
         * This has to run after rebuild_image_top_map(), since drop_images_before()
         * walks m_image_by_top_map in key order and the keys are only the new row
         * numbers once the map has been rebuilt.
         */
        drop_images_before(m_start);

        /* Any planned record the pass never reached has no new row: its old top
         * mapped past the end of the text the reflow produced, so there is no
         * row left to name it. Drop it rather than leave it holding an old
         * number, which is what would pin the stream.
         */
        for (auto i = spill_cursor; i < spill_plan.size(); i++)
                m_image_spill.erase(spill_plan[i].priority);

        /* And now that every surviving record is in the new numbering, the same
         * rule that runs when a row scrolls out can be asked again - this time
         * about rows it can actually compare against. Without this the bytes of
         * everything just dropped stay in the stream until some later scroll
         * happens to move the tail.
         */
        reclaim_image_spill(m_start);
#endif

	_vte_debug_print(vte::debug::category::RING, "Ring after rewrapping:");
        validate();
	return;

err:
#if VTE_DEBUG
	_vte_debug_print(vte::debug::category::RING,
			"Error while rewrapping");
	g_assert_not_reached();
#endif
	g_object_unref(new_row_stream);
	g_free(marker_text_offsets);
	g_free(new_markers);
}


bool
Ring::write_row(GOutputStream* stream,
                VteRowData* row,
                VteWriteFlags flags,
                GCancellable* cancellable,
                GError** error)
{
	VteCell *cell;
	GString *buffer = m_utf8_buffer;
	int i;
	gsize bytes_written;

	/* Simple version of the loop in freeze_row().
	 * TODO Should unify one day */
	g_string_truncate (buffer, 0);
	for (i = 0, cell = row->cells; i < row->len; i++, cell++) {
		if (G_LIKELY (!cell->attr.fragment()))
			_vte_unistr_append_to_string (cell->c, buffer);
	}
	if (!row->attr.soft_wrapped)
		g_string_append_c (buffer, '\n');

	return g_output_stream_write_all (stream, buffer->str, buffer->len, &bytes_written, cancellable, error);
}

/**
 * Ring::write_contents:
 * @stream: a #GOutputStream to write to
 * @flags: a set of #VteWriteFlags
 * @cancellable: optional #GCancellable object, %nullptr to ignore
 * @error: a #GError location to store the error occuring, or %nullptr to ignore
 *
 * Write entire ring contents to @stream according to @flags.
 *
 * Return: %TRUE on success, %FALSE if there was an error
 */
bool
Ring::write_contents(GOutputStream* stream,
                     VteWriteFlags flags,
                     GCancellable* cancellable,
                     GError** error)
{
	row_t i;

	_vte_debug_print(vte::debug::category::RING, "Writing contents to GOutputStream");

	if (m_start < m_writable)
	{
		RowRecord record;

		if (read_row_record(&record, m_start))
		{
			gsize start_offset = record.text_start_offset;
			gsize end_offset = _vte_stream_head(m_text_stream);
			char buf[4096];
			while (start_offset < end_offset)
			{
				gsize bytes_written, len;

				len = MIN (G_N_ELEMENTS (buf), end_offset - start_offset);

				if (!_vte_stream_read (m_text_stream, start_offset,
						       buf, len))
					return false;

				if (!g_output_stream_write_all (stream, buf, len,
								&bytes_written, cancellable,
								error))
					return false;

				start_offset += len;
			}
		}
		else
                        //FIXMEchpe g_set_error!!
			return false;
	}

	for (i = m_writable; i < m_end; i++) {
		if (!write_row(stream,
                               get_writable_index(i),
                               flags, cancellable, error))
			return false;
	}

	return true;
}

#if WITH_SIXEL

/**
 * Ring::append_image:
 * @surface: A Cairo surface object
 * @pixelwidth: vte::image::Image width in pixels
 * @pixelheight: vte::image::Image height in pixels
 * @left: Left position of image in cell units
 * @top: Top position of image in cell units
 * @cell_width: Width of image in cell units
 * @cell_height: Height of image in cell units
 *
 * Append an image to the internal image list.
 */
void
Ring::append_image(vte::Freeable<cairo_surface_t> surface,
                   int pixelwidth,
                   int pixelheight,
                   long left,
                   long top,
                   long cell_width,
                   long cell_height) /* throws */
{
        auto const priority = m_next_image_priority;
        auto [it, success] = m_image_map.try_emplace
                (priority, // key
                 std::make_unique<vte::image::Image>(std::move(surface),
                                                     priority,
                                                     pixelwidth,
                                                     pixelheight,
                                                     left,
                                                     top,
                                                     cell_width,
                                                     cell_height));
        if (!success)
                return;

        auto const& image = it->second;

        /* Give the image an id that cells can name it by.
         *
         * If the id space is exhausted, sweep once - retired ids that no
         * surviving cell refers to become available - and try again. If it is
         * still exhausted, DROP the image rather than storing one that no
         * cell can reference: an unreferenceable image is retention with no
         * way to draw it and no way to erase it.
         */
        auto pool_id = m_image_pool.allocate(image.get());
        if (pool_id == vte::image::k_ref_pool_id_none) {
                sweep_image_pool();
                pool_id = m_image_pool.allocate(image.get());
        }

        if (pool_id == vte::image::k_ref_pool_id_none) {
                m_image_map.erase(it);
                return;
        }

        image->set_pool_id(pool_id);

        ++m_next_image_priority;

        m_image_by_top_map.emplace(std::piecewise_construct,
                                   std::forward_as_tuple(image->get_top()),
                                   std::forward_as_tuple(image.get()));

        m_image_fast_memory_used += image_cost(image.get());

        /* From here until the caller says otherwise, this image is the one being
         * placed, and the lifetime rules leave it alone. It has to be marked
         * before the collectors run, because they can free it right back and
         * note_image_freed() is what keeps the marker from dangling.
         */
        m_placing_image = image.get();

        sync_has_images();

        image_gc_region();
        image_gc();
}

#endif /* WITH_SIXEL */
