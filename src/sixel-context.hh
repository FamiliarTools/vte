/*
 * Copyright © 2020 Christian Persch
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

#include <bit>
#include <cstdint>
#include <iterator>
#include <utility>

#ifdef VTE_COMPILATION
#include <cairo.h>
#include "cairo-glue.hh"
#endif

#include "glib-glue.hh"
#include "sixel-parser.hh"
#include "vtedefines.hh"

namespace vte::sixel {

class Context {

        friend class Parser;

public:
        /* The colour registers must be initialised HERE and not only in
         * prepare(), which resets them only when the terminal uses PRIVATE
         * colour registers. With shared registers - DECRST 1070 - nothing
         * else writes them, so the first image is parsed against an all-zero
         * palette and every colour it selects but does not define comes out
         * fully transparent, i.e. the image is invisible.
         *
         * (All-zero rather than indeterminate because the Context is created
         * with make_unique, which value-initialises. That makes this a
         * reliably wrong picture rather than undefined behaviour - worse in
         * one way, since it will never trip a sanitizer.)
         */
        Context() noexcept { reset_colors(); }
        ~Context() = default;

        Context(Context const&) = delete;
        Context(Context&&) noexcept = delete;

        Context& operator=(Context const&) = delete;
        Context& operator=(Context&&) noexcept = delete;

        /* Packed colour, RGBA 8 bits per component */
        using color_t = uint32_t;

        /* Indexed colour */
        using color_index_t = uint16_t;

        static inline constexpr auto const k_termprop_icon_image_id = 65535;

private:

        uint32_t m_introducer{0};
        uint32_t m_st{0};
        int m_id{-1};

        static inline constexpr unsigned const k_max_width = VTE_SIXEL_MAX_WIDTH;

        static inline constexpr unsigned const k_max_height = VTE_SIXEL_MAX_HEIGHT;
        static_assert((k_max_height % 6) == 0, "k_max_height not divisible by 6");

        static inline constexpr int const k_num_colors = VTE_SIXEL_NUM_COLOR_REGISTERS;
        static_assert((k_num_colors & (k_num_colors - 1)) == 0, "k_num_colors not a power of 2");

        /* The width and height as set per DECGRA */
        unsigned m_raster_width{0};
        unsigned m_raster_height{0};

        /* The width and height as per the SIXEL data received */
        unsigned m_width{0};
        unsigned m_height{0};

        /* Whether colour 0 is fully transparent, i.e. P2=1. */
        bool m_bg_transparent{false};

public:

        constexpr auto max_width()  const noexcept { return k_max_width;  }
        constexpr auto max_height() const noexcept { return k_max_height; }
        constexpr auto num_colors() const noexcept { return k_num_colors;  }

        /* The image's extent.
         *
         * DECGRA's raster dimensions declare an area to be FILLED with colour
         * 0. When colour 0 is opaque that area is real - it is painted, and
         * the image genuinely occupies it, so a raster larger than the data
         * wins.
         *
         * When P2=1 makes colour 0 fully transparent, nothing is painted
         * there. Honouring the raster then makes an image that declares
         * "1;1;1000;1000" and sends twenty rows of data occupy a thousand
         * pixels of rows - erasing the cells under them and pushing the
         * cursor far down the screen - to display nothing at all. So the
         * extent is the extent of the DATA.
         *
         * DEC STD 070 7.2 / 8.1.2; foot trims the same case.
         */
        constexpr auto image_width() const noexcept
        {
                return m_bg_transparent ? m_width : std::max(m_width, m_raster_width);
        }

        constexpr auto image_height() const noexcept
        {
                return m_bg_transparent ? m_height : std::max(m_height, m_raster_height);
        }

private:

        /* m_colors[0] is the background fill, m_colors[1] the current pen;
         * the addressable colour registers start after them.
         */
        static inline constexpr unsigned k_color_register_offset = 2;

        color_t m_colors[k_color_register_offset + k_num_colors];

        color_index_t m_current_color{0};

        Parser m_sixel_parser{};

        /* All sixels on the current scanline OR'd together */
        uint8_t m_scanline_mask{0};

        int m_repeat_count{1};

        /*
         * m_scanlines_data stores the pixel data in indexed colours (not resolved
         * RGBA colours).
         *
         * Pixels are stored interleaved in scan lines of six vertical pixels.
         * This makes writing them cache-efficient, and allows to easily write
         * more pixels in one scanline than the previous scanlines without having
         * to copy and pad already-written data. The buffer is created at the
         * start, and enlarged (if necessary) when starting a new scanline.
         *
         * m_scanlines_data is allocated/re-allocated as needed, and stores
         * m_scanlines_data_capacity color_index_t items.
         *
         * The offsets of the scanlines in m_scanlines_data are stored in
         * m_scanlines_offsets; scanline N occupies
         * [m_scanlines_offsets[N], m_scanlines_offsets[N+1]).
         *
         * m_scanlines_offsets_pos points to the offset in m_scanlines_offsets of the
         * current scanline, and is never nullptr. When in a valid scanline, there is
         * space to write to m_scanlines_offsets_pos[1] to store the scanline end
         * position.
         *
         * m_scanline_begin is a pointer to the current scanline being written;
         * m_scanline_pos is a pointer to the current write position, and
         * m_scanline_end is a pointer to the end of the scanline. All scanlines
         * have space to write up to k_max_width sixels (i.e. have 6 * k_max_width
         * items), regardless of m_width.
         * If allocation fails, or height limits are exceeded, all three pointers
         * are set to nullptr.
         *
         * [FIXME: This could be further improved (e.g. wrt. memory fragmentation) by
         * using a tempfile to store the pixel data, having only a fixed buffer
         * of N * k_max_width * 6 size, and writing out the scanline data on DECGNL,
         * instead of re-/allocating memory for the whole buffer.]
         */

        size_t m_scanlines_data_capacity{0};
        vte::glib::FreePtr<color_index_t> m_scanlines_data{};

        color_index_t* m_scanline_begin{nullptr};
        color_index_t* m_scanline_end{nullptr};
        color_index_t* m_scanline_pos{nullptr};
        unsigned m_scanlines_offsets[(k_max_height + 5) / 6 + 1]; // one more than the maximum
                                                                  // number of scanlines since
                                                                  // we need to store begin and
                                                                  // end offsets for each scanline
        unsigned* m_scanlines_offsets_pos{nullptr};

        inline auto scanlines_data_begin() const noexcept
        {
                return m_scanlines_data.get();
        }

        inline auto scanlines_data_end() const noexcept
        {
                return m_scanlines_data.get() + m_scanlines_data_capacity;
        }

        inline auto scanlines_offsets_begin() noexcept
        {
                return std::begin(m_scanlines_offsets);
        }

        inline constexpr auto scanlines_offsets_end() const noexcept
        {
                return std::end(m_scanlines_offsets);
        }

        inline constexpr auto scanline_capacity() const noexcept
        {
                return k_max_width * 6;
        }

        inline constexpr auto scanlines_count() const noexcept
        {
                return unsigned(m_scanlines_offsets_pos - std::begin(m_scanlines_offsets));
        }

        /* Returns the capacity needed to storage an image of width×height
         * dimensions, plus one max-sized scanline.
         */
        inline constexpr auto
        capacity(size_t const width,
                 size_t const height) noexcept
        {
                auto const scanlines = (height + 5) / 6;
                return (width * scanlines + k_max_width) * 6;
        }

        inline constexpr auto minimum_capacity() noexcept { return capacity(k_max_width, 64); }

        bool ensure_scanlines_capacity() noexcept;

        void
        ensure_scanline() noexcept
        {
                if (!ensure_scanlines_capacity()) {
                        m_scanline_pos = m_scanline_begin = m_scanline_end = nullptr;
                        return;
                }

                m_scanlines_offsets_pos[1] = m_scanlines_offsets_pos[0];
                m_scanline_pos = m_scanline_begin = scanlines_data_begin() + m_scanlines_offsets_pos[0];
                m_scanline_end = m_scanline_begin + scanline_capacity();
        }

        void
        update_scanline_offsets() noexcept
        {
                /* Update the scanline end offset and the line width */
                auto const width = unsigned(m_scanline_pos - m_scanline_begin);
                assert((width % 6) == 0);
                m_width = std::min(std::max(m_width, width / 6), k_max_width);

                auto const pos = unsigned(m_scanline_pos - m_scanlines_data.get());
                assert((pos % 6) == 0);
                m_scanlines_offsets_pos[1] = std::max(m_scanlines_offsets_pos[1], pos);
        }

        bool
        finish_scanline()
        {
                if (m_scanline_begin == m_scanline_end)
                        return false;

                auto msb = [](unsigned v) constexpr noexcept -> unsigned
                {
                        return 8 * sizeof(unsigned) - __builtin_clz(v);
                };

                static_assert(msb(0b1u) == 1, "wrong");
                static_assert(msb(0b10u) == 2, "wrong");
                static_assert(msb(0b100u) == 3, "wrong");
                static_assert(msb(0b1000u) == 4, "wrong");
                static_assert(msb(0b1'0000u) == 5, "wrong");
                static_assert(msb(0b10'0000u) == 6, "wrong");
                static_assert(msb(0b11'1111u) == 6, "wrong");

                /* Update the image height if there was any pixel set in the current scanline. */
                m_height = m_scanline_mask ? std::min(scanlines_count() * 6 + msb(m_scanline_mask), k_max_height) : m_height;

                m_scanline_mask = 0;
                m_repeat_count = 1;

                update_scanline_offsets();

                return true;
        }

        inline constexpr auto
        param_to_color_register(int param) noexcept
        {
                /* Colour registers are wrapped, as per DEC documentation.
                 *
                 * We internally reserve registers 0 and 1 for the background
                 * and foreground colors, the buffer being initialized to 0.
                 * Therefore the user-provided registers are stored at + 2 their
                 * public number.
                 */
                return (param & (k_num_colors - 1)) + 2;
        }

        inline constexpr color_t
        make_color(unsigned r,
                   unsigned g,
                   unsigned b) noexcept
        {
                if constexpr (std::endian::native == std::endian::little) {
                        return b | g << 8 | r << 16 | 0xffu << 24 /* opaque */;
                } else if constexpr (std::endian::native == std::endian::big) {
                        return 0xffu /* opaque */ | r << 8 | g << 16 | b << 24;
                } else {
                        __builtin_unreachable();
                }
        }

        color_t
        make_color_hls(int h,
                       int l,
                       int s) noexcept;

        inline constexpr color_t
        make_color_rgb(unsigned r,
                       unsigned g,
                       unsigned b) noexcept
        {
                auto scale = [](unsigned value) constexpr noexcept -> auto
                {
                        return (value * 255u + 50u) / 100u;
                };

                return make_color(scale(r), scale(g), scale(b));
        }

        void
        set_color(color_index_t reg,
                  color_t color) noexcept
        {
                m_colors[m_current_color = reg] = color;
        }

        void
        set_color_hls(unsigned reg,
                      unsigned h,
                      unsigned l,
                      unsigned s) noexcept
        {
                set_color(reg, make_color_hls(h, l, s));
        }

        void
        set_color_rgb(unsigned reg,
                      unsigned r,
                      unsigned g,
                      unsigned b) noexcept
        {
                set_color(reg, make_color_rgb(r, g, b));
        }

        void
        set_current_color(unsigned reg) noexcept
        {
                m_current_color = reg;
        }

        template<typename C,
                 typename P>
        inline C* image_data(size_t* size,
                             unsigned stride,
                             P pen) noexcept;

        void
        DECGCI(vte::sixel::Sequence const& seq) noexcept
        {
                /*
                 * DECGCI - DEC Graphics Color Introducer
                 * Selects and defines the current colour.
                 *
                 * Arguments:
                 *   args[0]: colour register
                 *   args[1]: colour coordinate system
                 *     1: HLS
                 *     2: RGB
                 *   args[2..4]: colour components
                 *     args[2]: 0..360 for HLS or 0..100 for RGB
                 *     args[3]: 0..100 for HSL and RGB
                 *     args[4]: 0..100 for HSL and RGB
                 *
                 * Defaults:
                 *   args[0]: 0
                 *   args[2]: no default
                 *   args[3..5]: 0
                 *
                 * If only one parameter is specified, selects the colour register
                 * for the following SIXELs to use. If more parameters are specified,
                 * additionally re-defines that colour register with the colour
                 * specified by the parameters.
                 *
                 * If the colour values exceed the ranges specified above, the DEC
                 * documentation says that the sequence is ignored.
                 * [FIXMEchpe: alternatively, we could just clamp to the range]
                 * [FIXMEchpe: check whether we need to set the current colour
                 *  register even in that case]
                 *
                 * References: DEC PPLV2 § 5.8
                 */

                m_repeat_count = 1;

                auto const reg = param_to_color_register(seq.param(0, 0));

                switch (seq.size()) {
                case 0: /* no param means param 0 has default value */
                case 1:
                        /* Switch to colour register */
                        set_current_color(reg);
                        break;

                case 2 ... 5:
                        switch (seq.param(1)) {
                        case -1: /* this parameter admits no default */
                        default:
                                break;

                        case 1: /* HLS */ {
                                auto const h = seq.param(2, 0);
                                auto const l = seq.param(3, 0);
                                auto const s = seq.param(4, 0);
                                if (G_UNLIKELY(h > 360 || l > 100 || s > 100))
                                        break;

                                set_color_hls(reg, h, l, s);
                                break;
                        }

                        case 2: /* RGB */ {
                                auto const r = seq.param(2, 0);
                                auto const g = seq.param(3, 0);
                                auto const b = seq.param(4, 0);
                                if (G_UNLIKELY(r > 100 || g > 100 || b > 100))
                                        break;

                                set_color_rgb(reg, r, g, b);
                                break;
                        }
                        }
                        break;

                default:
                        break;
                }
        }

        void
        DECGCR(vte::sixel::Sequence const& seq) noexcept
        {
                /* DECGCR - DEC Graphics Carriage Return
                 * Moves the active position to the left margin.
                 *
                 * (Note: DECCRNLM mode does not apply here.)
                 *
                 * References: DEC PPLV2 § 5.8
                 */

                /* Failed already, or exceeded limits */
                if (m_scanline_begin == m_scanline_end)
                        return;

                /* Update the scanline end offset of the current scanline, and return
                 * position to the start of the scanline.
                 */
                update_scanline_offsets();

                m_repeat_count = 1;
                m_scanline_pos = m_scanline_begin;
        }

        void
        DECGCH(vte::sixel::Sequence const& seq) noexcept
        {
                /* DECGCH - DEC Graphics Cursor Home
                 * Moves the active position to the left margin and top.
                 *
                 * This is apparently only supported on VT240, not on VT340.
                 *
                 * So don't bother trying to support this in VTE.
                 *
                 * References: vt340test/j4james/xor_and_home.sh
                 */

                /* This is not compatible with the way we store the
                 * scanlines, so we cannot support it. The question is only
                 * what to do INSTEAD, and doing a DECGNL is the worse of the
                 * two options available.
                 *
                 * DEC STD 070 8.1 says a command that is not supported is
                 * parsed and ignored. Substituting a graphics newline is not
                 * ignoring it: it moves the active position down a scanline,
                 * so every sixel after this point lands one row lower than
                 * the sender placed it. A single stray byte silently
                 * displaces the rest of the image.
                 *
                 * The failure modes are not symmetric. Ignoring a command
                 * that a VT340 does not implement either draws the image the
                 * sender intended - because a sender targeting a VT340 would
                 * not emit it - or omits one positioning step. Honouring it as
                 * something else corrupts everything downstream of it. So:
                 * ignore.
                 */
                SIXEL_NOP(seq);
        }

        void
        DECGNL(vte::sixel::Sequence const& seq) noexcept
        {
                /* DECGNL - DEC Graphics Next Line
                 * Moves the active position to the left margin and
                 * down by one scanline (6 pixels).
                 *
                 * References: DEC PPLV2 § 5.8
                 */

                /* Failed already, or exceeded limits */
                if (!finish_scanline())
                        return;

                /* Go to next scanline. If the number of scanlines exceeds the maximum
                 * (as defined by k_max_height), set the scanline pointers to nullptr.
                 */
                ++m_scanlines_offsets_pos;
                if (m_scanlines_offsets_pos + 1 >= scanlines_offsets_end()) {
                        m_scanline_pos = m_scanline_begin = m_scanline_end = nullptr;
                        return;
                }

                ensure_scanline();
        }

        void
        DECGRA(vte::sixel::Sequence const& seq) noexcept
        {
                /*
                 * DECGRA - DEC Graphics Raster Attributes
                 * Selects the raster attributes for the SIXEL data following.
                 *
                 * Arguments:
                 *   args[0]: pixel aspect ratio numerator (max: 32k)
                 *   args[1]: pixel aspect ratio denominator (max: 32k)
                 *   args[2]: horizontal size (in px) of the image
                 *   args[3]: vertical size (in px) of the image
                 *
                 * Defaults:
                 *   args[0]: 1
                 *   args[1]: 1
                 *   args[2]: no default
                 *   args[3]: no default

                 * Note that the image will not be clipped to the provided
                 * size.
                 *
                 * References: DEC PPLV2 § 5.8
                 */

                /* If any SIXEL data, or positioning command (DECGCR, DECGNL) has
                 * been received prior to this command, then DECGRA should be ignored.
                 * This check only approximates that condition, but that's good enough.
                 */
                if (m_scanlines_offsets_pos != scanlines_offsets_begin() ||
                    m_scanline_begin != m_scanlines_data.get() ||
                    m_scanline_pos != m_scanline_begin ||
                    m_scanlines_offsets[1] != 0 ||
                    m_scanlines_offsets[1] != m_scanlines_offsets[0])
                        return;

                /* args[0] and args[1] are the pixel aspect ratio numerator
                 * and denominator (Pan and Pad). VTE does not implement a
                 * non-square pixel aspect: an image is drawn at the aspect
                 * its pixels imply.
                 *
                 * Implementing it means scaling the decoded image vertically
                 * by Pan/Pad, clamped to something sane, AND honouring the P1
                 * parameter of the DECSIXEL introducer, which selects an
                 * aspect from the table in DEC STD 070 7.1 and which DECGRA
                 * overrides. Doing only one of the two is worse than doing
                 * neither, because senders pick whichever they think is
                 * supported.
                 *
                 * Until then the parameters are read and ignored, which is
                 * what an unimplemented raster attribute should do.
                 */

                m_raster_width = seq.param(2, 0, 0, k_max_width);
                m_raster_height = seq.param(3, 0, 0, k_max_height);

                /* Nothing else needs to be done here right now; the current
                 * scanline has enough space for k_max_width sixels, and the
                 * new raster width and height will be taken into account when
                 * resizing the m_scanlines_data buffer next.
                 */
        }

        void
        DECGRI(vte::sixel::Sequence const& seq) noexcept
        {
                /* DECGRI - DEC Graphics Repeat Introducer
                 * Specifies the repeat count for the following SIXEL.
                 *
                 * Arguments:
                 *   args[0]: the repeat count
                 *
                 * Defaults:
                 *   args[0]: 1
                 *
                 * References: DEC PPLV2 § 5.8
                 *             DEC STD 070
                 */

                /* DEC terminals limited the repetition count to 255, but the SIXEL
                 * test data includes repeat counts much greater. Since we limit to
                 * k_max_width anyway when executing the repeat on the next sixel,
                 * don't limit here.
                 *
                 * A repeat count of 0 is treated like 1.
                 */
                m_repeat_count = seq.param(0, 1) ? : 1;
        }

        void
        SIXEL(uint8_t sixel) noexcept
        {
                /* SIXEL data
                 * Data encodes a scanline of six pixels in the integer range
                 * 0x00 .. 0x3f, with the LSB representing the top pixel
                 * and the MSB representing the bottom pixel.
                 *
                 * References: DEC PPLV2 § 5.5.1
                 */

                /* No scanline at all: the height cap tripped, or a scanline
                 * allocation failed. Both null every scanline pointer.
                 *
                 * This has to be checked BEFORE the arithmetic below, not
                 * inside it: the zero-sixel branch computes
                 * m_scanline_pos + m_repeat_count * 6, and pointer arithmetic
                 * on a null pointer is undefined behaviour, not a harmless
                 * null. It is reachable from untrusted input - a stream that
                 * runs past the height cap and then emits a repeat.
                 */
                if (G_UNLIKELY(m_scanline_pos == nullptr)) {
                        m_repeat_count = 1;
                        return;
                }

                if (sixel) {
                        auto const color = m_current_color;
                        auto const scanline_end = m_scanline_end;
                        auto const scanline_start = m_scanline_pos;
                        auto scanline_pos = m_scanline_pos;

                        for (auto n = m_repeat_count;
                             n > 0 && G_LIKELY(scanline_pos < scanline_end);
                             --n) {
                                /* Note that the scanline has space for at least 6 pixels, wo we
                                 * don't need to check scanline_pos < scanline_end in this inner loop.
                                 *
                                 * FIXMEchpe: this can likely be optimised with some SIMD?
                                 */
                                for (auto mask = 0b1u; mask < 0b100'0000u; mask <<= 1) {
                                        auto const old_color = *scanline_pos;
                                        *scanline_pos++ = sixel & mask ? color : old_color;
                                }

                                assert(scanline_pos <= scanline_end);
                        }

                        m_scanline_pos = scanline_pos;

                        /* Only record the sixel in the mask if it actually
                         * stored something. The mask is what raises the
                         * image's height, so OR-ing it unconditionally makes
                         * a sixel that fell off the right-hand end of a full
                         * scanline - storing nothing - grow the image
                         * downwards anyway.
                         */
                        if (scanline_pos != scanline_start)
                                m_scanline_mask |= sixel;

                } else {
                        /* If there are no bits to set, just advance the position,
                         * making sure to guard against overflow.
                         */
                        m_scanline_pos = std::clamp(m_scanline_pos + m_repeat_count * 6,
                                                    m_scanline_begin, m_scanline_end);
                }

                m_repeat_count = 1;
        }

        void
        SIXEL_NOP(vte::sixel::Sequence const& seq) noexcept
        {
                m_repeat_count = 1;
        }

        void
        SIXEL_ST(char32_t st) noexcept
        {
                m_st = st;

                /* Still need to finish the current scanline. */
                finish_scanline();
        }

public:

        void prepare(int id,
                     uint32_t introducer,
                     unsigned bg_red,
                     unsigned bg_green,
                     unsigned bg_blue,
                     bool bg_transparent,
                     bool private_color_registers) noexcept;

        void reset_colors() noexcept;

        void reset() noexcept;

        uint8_t* image_data() noexcept;

        // These are only used in the test suite
        color_index_t* image_data_indexed(size_t* size = nullptr,
                                          unsigned extra_width_stride = 0) noexcept;
        auto color(unsigned idx) const noexcept { return m_colors[idx]; }

#ifdef VTE_COMPILATION
        vte::Freeable<cairo_surface_t> image_cairo() noexcept;
#endif

        void
        set_mode(Parser::Mode mode)
        {
                m_sixel_parser.set_mode(mode);
        }

        auto
        parse(uint8_t const* const bufstart,
              uint8_t const* const bufend,
              bool eos) noexcept -> auto
        {
                return m_sixel_parser.parse(bufstart, bufend, eos, *this);
        }

        constexpr auto introducer() const noexcept { return m_introducer; }
        constexpr auto st() const noexcept { return m_st; }
        constexpr auto id() const noexcept { return m_id; }

        constexpr bool
        is_matching_controls() const noexcept
        {
                return ((introducer() ^ st()) & 0x80) == 0;
        }

}; // class Context

} // namespace vte::sixel
