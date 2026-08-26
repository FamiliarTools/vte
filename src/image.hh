/*
 * Copyright © 2016-2020 Hayaki Saito <saitoha@me.com>
 * Copyright © 2020 Hans Petter Jansson <hpj@cl.no>
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

#pragma once

#include "cairo-glue.hh"
#include "image-ref.hh"

#if VTE_GTK == 4
#include <gdk/gdk.h>

#include "refptr.hh"
#endif

namespace vte {

namespace base {

class Ring;

} // namespace base

namespace image {

class Image {
private:
        // Image data, device-independent
        vte::Freeable<cairo_surface_t> m_surface{};

        // Draw/prune priority, must be unique
        size_t m_priority;

        // Image dimensions in pixels
        int m_width_pixels;
        int m_height_pixels;

        // Top left corner offset in cell units
        int m_left_cells;
        int m_top_cells;

        // Cell dimensions in pixels at time of image creation
        int m_cell_width;
        int m_cell_height;

        /* The id cells use to name this image; see image-ref.hh. Zero means
         * the image is not addressable by any cell, which is a resource
         * failure rather than a normal state.
         */
        vte::image::pool_id_t m_pool_id{vte::image::k_ref_pool_id_none};


#if VTE_GTK == 4
        /* Lazily created from the immutable m_surface, whose pixel buffer it
         * shares (zero copy), so that resource_size() stays truthful and the
         * ring's image GC remains the only eviction mechanism needed.
         */
        mutable vte::glib::RefPtr<GdkTexture> m_texture{};
#endif

        /* An image's position is the ring's to change, and only between rows.
         * What the access control buys is that NOTHING OUTSIDE THE RING can
         * change it: the rectangle is half of a fact whose other half is the
         * ring's by-top index, and a caller who could move one without the
         * other would leave the image filed under a row it no longer starts at.
         *
         * It buys nothing against the ring itself, which is a friend and whose
         * every method can reach this. Keeping the two halves in step there is
         * a rule the ring holds itself to, not one the language enforces:
         * re-key with the move, as Ring::reanchor_image() does, or move under a
         * rebuild of the index that lands before the next key read, as
         * Ring::rewrap_images_in_range() does.
         */
        friend class vte::base::Ring;

        inline void set_top(int row) noexcept { m_top_cells = row; }

        /* The column has no index to keep in step - nothing is keyed by it -
         * so this one is private only to keep the two edges together, and
         * because moving an image sideways is a rule of the ring's and not
         * something a caller may decide on its own. See
         * Ring::shift_images_for_scroll() for the one rule that uses it.
         */
        inline void set_left(int col) noexcept { m_left_cells = col; }

public:
        Image(vte::Freeable<cairo_surface_t> surface,
              size_t priority,
              int width_pixels,
              int height_pixels,
              int col,
              int row,
              int cell_width,
              int cell_height) noexcept
                : m_surface{std::move(surface)},
                  m_priority{priority},
                  m_width_pixels{width_pixels},
                  m_height_pixels{height_pixels},
                  m_left_cells{col},
                  m_top_cells{row},
                  m_cell_width{cell_width},
                  m_cell_height{cell_height}
        {
        }

        ~Image() = default;

        Image(Image const&) = delete;
        Image(Image&&) = delete;
        Image operator=(Image const&) = delete;
        Image operator=(Image&&) = delete;

        inline constexpr auto get_priority() const noexcept { return m_priority; }
        inline constexpr auto get_pool_id() const noexcept { return m_pool_id; }
        inline void set_pool_id(pool_id_t id) noexcept { m_pool_id = id; }
        /* Where the image sits, in cells. This is the ring's row index for the
         * image and not what the draw reads: see the note on the image maps in
         * ring.hh. The row is the ring's to move, and it has two movers:
         * Ring::reanchor_image(), which re-keys the by-top map in the same
         * step, and Ring::rewrap_images_in_range(), which leaves the key
         * stale on purpose and is paid for by the rebuild Ring::rewrap() runs
         * before the next key read (see the note on set_top() above). The
         * column is the ring's to move too, through
         * Ring::shift_images_for_scroll(), which is what lets a picture
         * follow the cells an ICH or a DCH carries sideways.
         */
        inline auto get_left() const noexcept { return m_left_cells; }
        inline auto get_top() const noexcept { return m_top_cells; }
        inline constexpr auto get_width() const noexcept { return (m_width_pixels + m_cell_width - 1) / m_cell_width; }
        inline constexpr auto get_height() const noexcept { return (m_height_pixels + m_cell_height - 1) / m_cell_height; }
        inline auto get_bottom() const noexcept { return m_top_cells + get_height() - 1; }

        /* The cell this image was laid out against, which is fixed at
         * placement and travels with the image. The draw must use THIS and
         * not the terminal's current value: it addresses the image in ITS own
         * pixel grid, and an image restored from the scrollback has to keep
         * the scale it was placed at.
         */
        inline constexpr auto get_cell_width() const noexcept { return m_cell_width; }
        inline constexpr auto get_cell_height() const noexcept { return m_cell_height; }

        inline constexpr auto get_width_px() const noexcept { return m_width_pixels; }
        inline constexpr auto get_height_px() const noexcept { return m_height_pixels; }

        /* The image's display size at the given current cell dimensions. The
         * image is stretched so that it keeps covering the same cells as when
         * it was created; m_cell_width/m_cell_height are the cell dimensions
         * that were in effect then. Fractional, since neither the ratio nor
         * the result need be integral, and rounding here would accumulate a
         * visible error over a large image.
         */
        inline constexpr auto get_width_pixels(long cell_width) const noexcept
        {
                return m_width_pixels * double(cell_width) / double(m_cell_width);
        }

        inline constexpr auto get_height_pixels(long cell_height) const noexcept
        {
                return m_height_pixels * double(cell_height) / double(m_cell_height);
        }

        inline auto get_surface() const noexcept { return m_surface.get(); }

#if VTE_GTK == 4
        GdkTexture* get_texture() const noexcept;
#endif

        inline auto resource_size() const noexcept
        {
                if (cairo_image_surface_get_stride(m_surface.get()) != 0)
                        return cairo_image_surface_get_stride(m_surface.get()) * m_height_pixels;

                /* Not an image surface: Only the device knows for sure, so we guess */
                return m_width_pixels * m_height_pixels * 4;
        }

}; // class Image

} // namespace image

} // namespace vte
