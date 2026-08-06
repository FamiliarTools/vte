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

#include "config.h"

#include "image.hh"

#if VTE_GTK == 4

#include "glib-glue.hh"

namespace vte {

namespace image {

/* Return a GdkTexture wrapping the image's pixels, creating it on first use.
 *
 * The surface is never modified after the SIXEL decoder hands it over, so the
 * texture can share its pixel buffer instead of copying it, and never needs to
 * be invalidated. Keeping it alive for as long as the Image also means GSK
 * sees a stable texture pointer across frames, which lets it both skip the
 * damage region and re-use the already uploaded GPU copy.
 */
GdkTexture*
Image::get_texture() const noexcept
{
        if (m_texture)
                return m_texture.get();

        auto const surface = m_surface.get();
        if (cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE ||
            cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32)
                return nullptr;

        cairo_surface_flush(surface);

        /* Take the extent from the surface rather than from m_width_pixels /
         * m_height_pixels: those describe the image, this describes the buffer
         * that is about to be handed to GDK, and reading past its end would be
         * fatal if they ever disagreed. Scaling to the display size is the
         * drawing context's job anyway.
         */
        auto const width = cairo_image_surface_get_width(surface);
        auto const height = cairo_image_surface_get_height(surface);
        if (width <= 0 || height <= 0)
                return nullptr;

        auto const stride = cairo_image_surface_get_stride(surface);
        auto const bytes = vte::take_freeable
                (g_bytes_new_with_free_func(cairo_image_surface_get_data(surface),
                                            size_t(stride) * size_t(height),
                                            GDestroyNotify(cairo_surface_destroy),
                                            cairo_surface_reference(surface)));

        /* GDK_MEMORY_DEFAULT is defined per endianness to be exactly
         * CAIRO_FORMAT_ARGB32, i.e. premultiplied. */
        m_texture = vte::glib::take_ref
                (gdk_memory_texture_new(width,
                                        height,
                                        GDK_MEMORY_DEFAULT,
                                        bytes.get(),
                                        stride));

        return m_texture.get();
}

} // namespace image

} // namespace vte

#endif /* VTE_GTK == 4 */
