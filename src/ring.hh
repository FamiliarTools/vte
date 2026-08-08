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
 * Red Hat Author(s): Behdad Esfahbod
 */

/* The interfaces in this file are subject to change at any time. */

#pragma once

#include <gio/gio.h>
#include <vte/vte.h>

#include "vterowdata.hh"
#include "vtestream.h"

#if WITH_SIXEL
#include "cairo-glue.hh"
#include "image.hh"
#include "image-pool.hh"
#include <map>
#include <memory>
#endif

#include <type_traits>

typedef struct _VteVisualPosition {
	long row, col;
} VteVisualPosition;

namespace vte {

namespace base {

/*
 * Ring:
 *
 * A scrollback buffer ring.
 */
class Ring {
public:
        typedef guint32 hyperlink_idx_t;
        // FIXME make this size_t (or off_t?)
        typedef gulong row_t;
        typedef glong column_t;

        static const row_t kDefaultMaxRows = VTE_SCROLLBACK_INIT;

        Ring(row_t max_rows = kDefaultMaxRows,
             bool has_streams = false);
        ~Ring();

        // prevent accidents
        Ring(Ring& o) = delete;
        Ring(Ring const& o) = delete;
        Ring(Ring&& o) = delete;
        Ring& operator= (Ring& o) = delete;
        Ring& operator= (Ring const& o) = delete;
        Ring& operator= (Ring&& o) = delete;

        inline bool contains(row_t position) const {
                return (position >= m_start && position < m_end);
        }

        inline row_t delta() const { return m_start; }
        inline row_t length() const { return m_end - m_start; }
        inline row_t next() const { return m_end; }

        //FIXMEchpe rename this to at()
        //FIXMEchpe use references not pointers
        VteRowData const* index(row_t position); /* const? */
        bool is_soft_wrapped(row_t position);
        bool contains_prompt_beginning(row_t position);

        void hyperlink_maybe_gc(row_t increment);
        hyperlink_idx_t get_hyperlink_idx(char const* hyperlink);
        hyperlink_idx_t get_hyperlink_at_position(row_t position,
                                                  column_t col,
                                                  bool update_hover_idx,
                                                  char const** hyperlink);

        row_t reset();
        void resize(row_t max_rows = kDefaultMaxRows);
        void shrink(row_t max_len = kDefaultMaxRows);
        VteRowData* insert(row_t position, guint8 bidi_flags);
        VteRowData* append(guint8 bidi_flags);
        void remove(row_t position);
        void drop_scrollback(row_t position);
        void set_visible_rows(row_t rows);
        void rewrap(column_t columns,
                    VteVisualPosition** markers);
        bool write_contents(GOutputStream* stream,
                            VteWriteFlags flags,
                            GCancellable* cancellable,
                            GError** error);

        inline VteRowData* index_writable(row_t position) {
                ensure_writable(position);
                return get_writable_index(position);
        }

private:

        #if VTE_DEBUG
        void validate() const;
        #if WITH_SIXEL
        void validate_images() const;
        #endif
        #endif

        inline GString* hyperlink_get(hyperlink_idx_t idx) const { return (GString*)g_ptr_array_index(m_hyperlinks, idx); }

        inline VteRowData* get_writable_index(row_t position) const { return &m_array[position & m_mask]; }

        void hyperlink_gc();
        hyperlink_idx_t get_hyperlink_idx_no_update_current(char const* hyperlink);

        typedef struct _CellAttrChange {
                gsize text_end_offset;  /* offset of first character no longer using this attr */
                VteStreamCellAttr attr;
        } CellAttrChange;

        /* What an image run writes after the hyperlink tail.
         *
         * The image is identified by its PRIORITY, not by its pool id. The
         * pool id is an index into an in-memory table that is reused once a
         * sweep reclaims it, so a frozen row holding one could come back
         * pointing at a different image. The priority is allocated from a
         * monotonically increasing counter and is never reused, so it is safe
         * to write down and still means the same image whenever it is read
         * back. It is also already the key of m_image_map, so resolving it is
         * a lookup rather than a search.
         */
        typedef struct _VTE_GNUC_PACKED _StreamImageRef {
                uint64_t priority;
                uint32_t ref_bits;      /* tile coordinates; pool id ignored */
        } StreamImageRef;

        /* The stride of one CellAttrChange record in the attr stream: the
         * fixed part, then the hyperlink target and its two terminating
         * bytes.
         *
         * Centralised because the freeze, thaw, truncate and rewrap paths
         * each walk these records independently - eight sites - and a stride
         * that disagrees between any two of them desynchronises the reader
         * from the writer, which corrupts the scrollback SILENTLY rather
         * than failing. Anything added to the record's variable tail goes
         * here and nowhere else.
         */
        static inline constexpr gsize attr_record_stride(gsize hyperlink_length,
                                                        bool has_image = false) noexcept
        {
                return sizeof(CellAttrChange) + hyperlink_length + 2 +
                        (has_image ? sizeof(StreamImageRef) : 0);
        }

        /* Whether a record read back from the stream carries an image
         * reference. The tag lives in the attr word, which IS persisted, so
         * the reader can tell without any out-of-band state.
         */
        static inline constexpr bool record_has_image(CellAttrChange const& c) noexcept
        {
                return !!(c.attr.attr & VTE_ATTR_IMAGE_MASK);
        }

        typedef struct _RowRecord {
                size_t text_start_offset;  /* offset where text of this row begins */
                size_t attr_start_offset;  /* offset of the first character's attributes */
                uint32_t width: 16;        /* for rewrapping speedup: the number of character cells (columns) */
                uint32_t is_ascii: 1;      /* for rewrapping speedup: guarantees that line contains 32..126 bytes only. Can be 0 even when ascii only. */
                uint32_t soft_wrapped: 1;  /* end of line is not '\n' */
                uint32_t bidi_flags: 4;
        } RowRecord;

        static_assert(std::is_standard_layout_v<RowRecord> && std::is_trivial_v<RowRecord>, "Ring::RowRecord is not POD");

        /* Represents a cell position, see ../doc/rewrap.txt */
        typedef struct _CellTextOffset {
                size_t text_offset;    /* byte offset in text_stream (or perhaps beyond) */
                int fragment_cells;  /* extra number of cells to walk within a multicell character */
                int eol_cells;       /* -1 if over a character, >=0 if at EOL or beyond */
        } CellTextOffset;

        static_assert(std::is_standard_layout_v<CellTextOffset> && std::is_trivial_v<CellTextOffset>, "Ring::CellTextOffset is not POD");

        inline bool read_row_record(RowRecord* record /* out */,
                                    row_t position)
        {
                return _vte_stream_read(m_row_stream,
                                        position * sizeof(*record),
                                        (char*)record,
                                        sizeof(*record));
        }

        inline void append_row_record(RowRecord const* record,
                                      row_t position)
        {
                _vte_stream_append(m_row_stream,
                                   (char const*)record,
                                   sizeof(*record));
        }

        bool frozen_row_column_to_text_offset(row_t position,
                                              column_t column,
                                              CellTextOffset* offset);
        bool frozen_row_text_offset_to_column(row_t position,
                                              CellTextOffset const* offset,
                                              column_t* column);

        bool write_row(GOutputStream* stream,
                       VteRowData* row,
                       VteWriteFlags flags,
                       GCancellable* cancellable,
                       GError** error);

        void ensure_writable_room();

        inline void ensure_writable(row_t position) {
                if G_UNLIKELY (position < m_writable) {
                        //FIXMEchpe surely this can be optimised
                        while (position < m_writable)
                                thaw_one_row();
                }
        }

        void freeze_one_row();
        void maybe_freeze_one_row();
        void thaw_one_row();
        void discard_one_row();
        void maybe_discard_one_row();

        void freeze_row(row_t position,
                        VteRowData const* row);
        void thaw_row(row_t position,
                      VteRowData* row,
                      bool do_truncate,
                      int hyperlink_column,
                      char const** hyperlink);
        void reset_streams(row_t position);

	row_t m_max;
	row_t m_start{0};
        row_t m_end{0};

	/* Writable */
	row_t m_writable{0};
        row_t m_mask{31};
	VteRowData *m_array;

        /* Storage:
         *
         * row_stream contains records of VteRowRecord for each physical row.
         * (This stream is regenerated when the contents rewrap on resize.)
         *
         * text_stream is the text in UTF-8.
         *
         * attr_stream contains entries that consist of:
         *  - a VteCellAttrChange.
         *  - a string of attr.hyperlink_length length containing the (typically empty) hyperlink data.
         *    As far as the ring is concerned, this hyperlink data is opaque. Only the caller cares that
         *    if nonempty, it actually contains the ID and URI separated with a semicolon. Not NUL terminated.
         *  - 2 bytes repeating attr.hyperlink_length so that we can walk backwards.
         */
	bool m_has_streams;
	VteStream *m_attr_stream, *m_text_stream, *m_row_stream;

        /* The fourth stream: the PIXELS of images that have been evicted from
         * memory while rows that name them can still be thawed back.
         *
         * Without it only the image REFERENCE survives the scrollback, so an
         * image dropped under memory pressure - or simply scrolled far enough
         * back - resolves to nothing and its cells draw as background. The
         * reference is rebound by priority, which is monotonic and never
         * reused, so it is also the right key here.
         */
        VteStream* m_image_stream{nullptr};
	size_t m_last_attr_text_start_offset{0};
	VteCellAttr m_last_attr;
	GString *m_utf8_buffer;

	VteRowData m_cached_row;
	row_t m_cached_row_num{(row_t)-1};

        row_t m_visible_rows{0};  /* to keep at least a screenful of lines in memory, bug 646098 comment 12 */

        GPtrArray *m_hyperlinks;  /* The hyperlink pool. Contains GString* items.
                                   [0] points to an empty GString, [1] to [VTE_HYPERLINK_COUNT_MAX] contain the id;uri pairs. */
        char m_hyperlink_buf[VTE_HYPERLINK_TOTAL_LENGTH_MAX + 1];  /* One more hyperlink buffer to get the value if it's not placed in the pool. */
        hyperlink_idx_t m_hyperlink_highest_used_idx{0};  /* 0 if no hyperlinks at all in the pool. */
        hyperlink_idx_t m_hyperlink_current_idx{0};  /* The hyperlink idx used for newly created cells.
                                                   Must not be GC'd even if doesn't occur onscreen. */
        hyperlink_idx_t m_hyperlink_hover_idx{0};  /* The hyperlink idx of the hovered cell.
                                                 An idx is allocated on hover even if the cell is scrolled out to the streams. */
        row_t m_hyperlink_maybe_gc_counter{0};  /* Do a GC when it reaches 65536. */

#if WITH_SIXEL

private:
        size_t m_next_image_priority{0};
        size_t m_image_fast_memory_used{0};
        size_t m_image_memory_max{VTE_IMAGE_MEMORY_MAX_DEFAULT};

        /* m_image_priority_map stores the Image. key is the priority of the image. */
        using image_map_type = std::map<size_t, std::unique_ptr<vte::image::Image>>;
        image_map_type m_image_map{};

        /* m_image_by_top_map stores only an iterator to the Image in m_image_priority_map;
         * key is the top row of the image.
         */
        using image_by_top_map_type = std::multimap<row_t, vte::image::Image*>;
        image_by_top_map_type m_image_by_top_map{};

        /* The image whose own emission burst is currently running, or nullptr.
         * Placing an image erases the cells it is about to cover, and moves the
         * cursor down over them; both of those are delete verbs for every other
         * image, so the one being placed has to be held out of the rules or it
         * would delete itself before it ever renders. Never dereferenced, only
         * compared, and cleared whenever the image it names is freed.
         */
        vte::image::Image* m_placing_image{nullptr};

        /* Mirrors !m_image_map.empty(). Reading the map instead costs a cache line
         * nothing else on the per-character path wants - the image maps sit past
         * m_hyperlink_buf, two kilobytes further into the object - which measures
         * as a 3 to 6 percent throughput loss on text that goes through
         * insert_char() one character at a time.
         *
         * It is declared HERE, among the image fields, and deliberately not up
         * beside m_start/m_end where the hot scalars live. Putting it there
         * displaces m_writable/m_mask/m_array and costs the BULK path - the run
         * writer that handles ordinary ASCII - about 8 percent, which is a worse
         * trade on the commoner workload. Confirmed by measuring the two
         * placements against each other and against an inert-padding build.
         *
         * Kept in step by sync_has_images(), which recomputes from the map rather
         * than reasoning about what the caller just did, so the mirror can never
         * claim something the map does not.
         */
        bool m_has_images{false};

        /* The id space cells use to name images. An id outlives the image it
         * named until a sweep confirms no cell still refers to it, which is
         * what stops a freed image's id from being handed to a new image
         * while a scrollback cell still points at it. See image-pool.hh.
         */
        vte::image::PoolT<vte::image::Image> m_image_pool{};

        /* Set when a rule moved or deleted an image; the caller of the ring
         * mutation drains it to repaint. */
        bool m_images_changed{false};

        void image_gc() noexcept;
        void sweep_image_pool() noexcept;
        void image_gc_region() noexcept;
        void unlink_image_from_top_map(vte::image::Image const* image) noexcept;
        void rebuild_image_top_map() /* throws */;
        image_by_top_map_type::iterator erase_image(image_by_top_map_type::iterator it) noexcept;
        /* One spilled image: a fixed header, then the pixel data. */
        typedef struct _VTE_GNUC_PACKED _ImageSpillRecord {
                uint64_t priority;
                int32_t width_px;
                int32_t height_px;
                int32_t left_cells;
                int32_t top_cells;
                int32_t cell_width;
                int32_t cell_height;
                uint32_t data_len;
        } ImageSpillRecord;

        /* Where each spilled image lives in m_image_stream, plus the rows it
         * covered.
         *
         * The rows are kept here rather than read back from the record because
         * they have to be answerable AFTER the Image object is gone: they are
         * what decides when a spill can never be needed again, and therefore
         * when the stream's tail may advance past it.
         */
        struct ImageSpill {
                gsize offset;
                long top;
                long bottom;
        };
        std::map<size_t /* priority */, ImageSpill> m_image_spill{};

        void spill_image(vte::image::Image const* image) noexcept;
        vte::image::Image* restore_image(size_t priority) /* throws */;
        void reclaim_image_spill(row_t before_row) noexcept;

        bool image_has_any_cell(vte::image::Image const* image) const noexcept;
        void drop_images_before(row_t row) noexcept;
        void drop_images_after(row_t row) noexcept;
        void drop_images_torn_by_rewrap(column_t columns) noexcept;
        void rewrap_images_in_range(image_by_top_map_type::iterator& it,
                                    size_t text_start_ofs,
                                    size_t text_end_ofs,
                                    row_t new_row_index) noexcept;
        void shift_images_for_insert(row_t position) noexcept;
        void shift_images_for_remove(row_t position) noexcept;

        inline void note_image_freed(vte::image::Image const* image) noexcept {
                if (m_placing_image == image)
                        m_placing_image = nullptr;

                /* Retire, not release: cells may still name this id, and
                 * handing it straight to the next image would make those
                 * cells display the new one.
                 */
                m_image_pool.retire(image->get_pool_id());
        }

        /* Recompute m_has_images from the map rather than reasoning about what
         * the caller just did, so the mirror cannot say something the map does
         * not. Called wherever m_image_map changes. */
        inline void sync_has_images() noexcept { m_has_images = !m_image_map.empty(); }

public:
        auto const& image_map() const noexcept { return m_image_map; }

        /* For tests. */
        auto const& image_pool() const noexcept { return m_image_pool; }
        auto& image_pool() noexcept { return m_image_pool; }
        void sweep_image_pool_for_test() noexcept { sweep_image_pool(); }

        /* For tests: evict every resident image, as memory pressure would,
         * spilling the pixels of any that can still be thawed back.
         */
        void evict_all_images_for_test() noexcept
        {
                while (!m_image_map.empty()) {
                        auto& image = m_image_map.begin()->second;
                        spill_image(image.get());
                        m_image_fast_memory_used -= image->resource_size();
                        note_image_freed(image.get());
                        unlink_image_from_top_map(image.get());
                        m_image_map.erase(m_image_map.begin());
                }
                sync_has_images();
        }

        auto image_spill_count_for_test() const noexcept { return m_image_spill.size(); }

        /* The bytes the resident images are charged for, i.e. what the image GC
         * spends its budget against. It has to be the sum over exactly the images
         * the map holds; a row-destroying path that forgets to free leaves pixels
         * nobody can reach still holding budget, which live images then have to be
         * evicted to make room for. Exposed so a test can hold the counter to the
         * map from the outside.
         */
        inline auto image_memory_used() const noexcept { return m_image_fast_memory_used; }

        /* The image memory budget, in bytes.
         *
         * chpe asked for this to be real API twice (vte#255, vte#2084):
         * "some API to set the hard resource limit (like we have the
         * number-of-scrollback-lines API)". Bytes rather than a count is
         * what the rest of the field uses, and it is the quantity a user
         * can actually reason about.
         *
         * Zero is meaningful and not merely degenerate: it means "no image
         * memory", which disables images by making every one of them
         * immediately over budget.
         */
        inline auto image_memory_max() const noexcept { return m_image_memory_max; }

        void set_image_memory_max(size_t max) noexcept
        {
                m_image_memory_max = max;
                image_gc();
        }

        /* For tests: how many bytes the attr stream has been appended.
         *
         * freeze_row() run-length-codes attributes by memcmp over the WHOLE
         * VteCellAttr, so this is the only way to observe from the outside
         * that a per-cell-varying field has destroyed the coding.
         */
        inline auto attr_stream_head() const noexcept
        {
                return m_attr_stream ? _vte_stream_head(m_attr_stream) : 0;
        }

        /* Whether any image is resident. This is the guard the callers put in
         * front of every image rule, so that a ring holding no image - which is
         * very nearly always - pays one predicted branch on a cache line it is
         * already using. See m_has_images for why it is not read from the map.
         */
        inline bool has_images() const noexcept { return m_has_images; }

        inline void set_placing_image(vte::image::Image* image) noexcept { m_placing_image = image; }
        inline auto placing_image() const noexcept { return m_placing_image; }

        /* Stamp the cells of one row of the image being placed with the
         * reference that names it.
         *
         * @image_row is the row's index within the IMAGE, not the screen, so
         * the cell keeps knowing which piece of the picture it carries after
         * the row has been scrolled, rewrapped or moved.
         *
         * Only cells that already exist are stamped. erase_image_rect()
         * deliberately does not create cells past the end of a row - a cell
         * that exists counts toward the row length, and the length is what a
         * reflow wraps on - so an image over a short row is anchored by the
         * cells there are.
         */
        /* Where an image actually sits, according to the cells that name it.
         *
         * Returns the screen position of the image's top-left tile, found by
         * looking for the cell holding Ref{id, 0, 0}. Answers false when no
         * such cell is present in the writable rows - the image may be
         * entirely in the scrollback, or its anchoring cell may have been
         * overwritten.
         *
         * This is the point of anchoring images to cells: the cells are moved
         * by every operation that moves text - scrolling, insertion,
         * deletion, rewrap - without any of those operations having to know
         * that images exist.
         */
        bool find_image_anchor(uint32_t pool_id,
                               row_t* out_row,
                               column_t* out_col) const noexcept;

        void stamp_image_row(row_t position,
                             column_t left,
                             column_t columns,
                             uint32_t image_row) noexcept;

        inline bool take_images_changed() noexcept {
                auto const changed = m_images_changed;
                m_images_changed = false;
                return changed;
        }

        bool erase_images_in_rect(long top,
                                  long bottom,
                                  long left,
                                  long right,
                                  long* damage_top,
                                  long* damage_bottom) noexcept;

        void append_image(vte::Freeable<cairo_surface_t> surface,
                          int pixelwidth,
                          int pixelheight,
                          long left,
                          long top,
                          long cell_width,
                          long cell_height) /* throws */;

#else /* !WITH_SIXEL */

public:
        /* Without image support there is nothing to keep alive, so every image
         * rule folds away at compile time rather than behind a preprocessor
         * conditional at each of its call sites. */
        static constexpr bool has_images() noexcept { return false; }
        static constexpr bool take_images_changed() noexcept { return false; }

#endif /* WITH_SIXEL */
};

}; /* namespace base */

}; /* namespace vte */
