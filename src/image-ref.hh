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

/* The interfaces in this file are subject to change at any time. */

#pragma once

#include <cstdint>

#include "vtedefines.hh"

namespace vte::image {

/*
 * Ref: what an image cell stores in VteCellAttr::m_link.
 *
 * Exactly 32 bits, because that is the whole budget: see the discriminated
 * union in cell.hh for why the field cannot grow and why VteCell cannot
 * either.
 *
 * Layout, most significant first:
 *
 *      pool_id  : 14   which image, an index into the image pool
 *      tile_row :  9   which cell row of that image this cell is
 *      tile_col :  9   which cell column of that image this cell is
 *
 * The coordinates are per-image, not per-screen, so a cell knows which piece
 * of the image it carries no matter where the row has since been moved to.
 * That is what lets rewrap re-anchor an image for free: rewrap rebuilds
 * row_stream alone, so the cells keep their coordinates and the image simply
 * follows the cells that survived.
 *
 * The coordinate widths are sized against the LARGEST image the parser will
 * admit, divided by the emulated cell that sixel geometry is expressed in:
 *
 *      cols = ceil(VTE_SIXEL_MAX_WIDTH  / VTE_SIXEL_CELL_MIN_WIDTH)
 *      rows = ceil(VTE_SIXEL_MAX_HEIGHT / VTE_SIXEL_CELL_MIN_HEIGHT)
 *
 * At 2048x2052 over the 4x8 FLOOR that is exactly 512 columns and 257 rows,
 * both inside 9 bits - the floor is chosen to make this true rather than
 * discovered to be true. Images are laid out against the font's cell, which
 * is normally much larger, so this is the worst case and not the usual one.
 *
 * The floor is what makes the packing safe, and it has to be ENFORCED
 * somewhere real. The widget clamps its font cell only to 1x2 pixels, so
 * without a floor of our own a legal image would need 2048 columns - 11 bits
 * - and its tail would simply never be stamped. Terminal::image_cell_size()
 * applies the floor and is the only producer of an image layout cell.
 *
 * 14 bits of pool id is 16383 concurrently live images (0 is reserved as the
 * "no image" id so that a zeroed Ref is invalid rather than a reference to
 * image 0).
 */

inline constexpr unsigned k_ref_pool_id_bits = 14;
inline constexpr unsigned k_ref_tile_row_bits = 9;
inline constexpr unsigned k_ref_tile_col_bits = 9;

static_assert(k_ref_pool_id_bits + k_ref_tile_row_bits + k_ref_tile_col_bits == 32,
              "vte::image::Ref must be exactly 32 bits: it lives in VteCellAttr::m_link");

inline constexpr unsigned k_ref_tile_col_shift = 0;
inline constexpr unsigned k_ref_tile_row_shift = k_ref_tile_col_shift + k_ref_tile_col_bits;
inline constexpr unsigned k_ref_pool_id_shift = k_ref_tile_row_shift + k_ref_tile_row_bits;

inline constexpr uint32_t k_ref_pool_id_max = (1u << k_ref_pool_id_bits) - 1u;
inline constexpr uint32_t k_ref_tile_row_max = (1u << k_ref_tile_row_bits) - 1u;
inline constexpr uint32_t k_ref_tile_col_max = (1u << k_ref_tile_col_bits) - 1u;

/* The pool id reserved to mean "not an image". */
inline constexpr uint32_t k_ref_pool_id_none = 0u;

/* The worst-case tile footprint of a legal image, in cells. Derived from the
 * caps and the emulated cell, so it is a fact about the constants rather than
 * an assumption about fonts - which is what the previous version of this got
 * wrong, by asserting against a minimum cell size nothing enforced.
 */
inline constexpr int k_max_image_tile_cols =
        (VTE_SIXEL_MAX_WIDTH + VTE_SIXEL_CELL_MIN_WIDTH - 1) / VTE_SIXEL_CELL_MIN_WIDTH;
inline constexpr int k_max_image_tile_rows =
        (VTE_SIXEL_MAX_HEIGHT + VTE_SIXEL_CELL_MIN_HEIGHT - 1) / VTE_SIXEL_CELL_MIN_HEIGHT;

static_assert(k_max_image_tile_cols <= int(k_ref_tile_col_max) + 1,
              "a legal image admits more tile columns than a Ref can address; "
              "widen k_ref_tile_col_bits, lower VTE_SIXEL_MAX_WIDTH, or raise "
              "VTE_SIXEL_CELL_WIDTH");
static_assert(k_max_image_tile_rows <= int(k_ref_tile_row_max) + 1,
              "a legal image admits more tile rows than a Ref can address; "
              "widen k_ref_tile_row_bits, lower VTE_SIXEL_MAX_HEIGHT, or raise "
              "VTE_SIXEL_CELL_HEIGHT");

class Ref {
private:
        uint32_t m_bits{0};

public:
        Ref() = default;

        inline constexpr explicit Ref(uint32_t bits) noexcept
                : m_bits{bits}
        {
        }

        inline constexpr Ref(uint32_t pool_id,
                             uint32_t tile_row,
                             uint32_t tile_col) noexcept
                : m_bits{((pool_id  & k_ref_pool_id_max)  << k_ref_pool_id_shift) |
                         ((tile_row & k_ref_tile_row_max) << k_ref_tile_row_shift) |
                         ((tile_col & k_ref_tile_col_max) << k_ref_tile_col_shift)}
        {
        }

        /* Whether these values can be represented exactly. A caller that is
         * able to refuse an image - the placement path - must check this and
         * refuse, rather than store a masked reference.
         */
        static inline constexpr bool fits(uint32_t pool_id,
                                          uint32_t tile_row,
                                          uint32_t tile_col) noexcept
        {
                return pool_id <= k_ref_pool_id_max &&
                       tile_row <= k_ref_tile_row_max &&
                       tile_col <= k_ref_tile_col_max;
        }

        /* The largest image footprint, in cells, that can be referenced. */
        static inline constexpr uint32_t max_tile_rows = k_ref_tile_row_max + 1;
        static inline constexpr uint32_t max_tile_cols = k_ref_tile_col_max + 1;

        inline constexpr uint32_t bits() const noexcept { return m_bits; }

        inline constexpr uint32_t pool_id() const noexcept
        {
                return (m_bits >> k_ref_pool_id_shift) & k_ref_pool_id_max;
        }

        inline constexpr uint32_t tile_row() const noexcept
        {
                return (m_bits >> k_ref_tile_row_shift) & k_ref_tile_row_max;
        }

        inline constexpr uint32_t tile_col() const noexcept
        {
                return (m_bits >> k_ref_tile_col_shift) & k_ref_tile_col_max;
        }

        /* A Ref is valid iff it names a real image. Note that this makes a
         * zeroed Ref invalid, so memset-ing a cell to zero cannot conjure a
         * reference to a live image.
         */
        inline constexpr bool valid() const noexcept
        {
                return pool_id() != k_ref_pool_id_none;
        }

        /* Whether two cells belong to the same image. */
        inline constexpr bool same_image(Ref const& other) const noexcept
        {
                return pool_id() == other.pool_id();
        }

        /* Whether two cells belong to the same stripe: one tile row of one
         * image. The stripe is the unit of image lifetime, so that a single
         * surviving cell pins one row of tiles rather than a whole
         * multi-megapixel image.
         */
        inline constexpr bool same_stripe(Ref const& other) const noexcept
        {
                return same_image(other) && tile_row() == other.tile_row();
        }

        inline constexpr bool operator==(Ref const& other) const noexcept
        {
                return m_bits == other.m_bits;
        }
};

static_assert(sizeof(Ref) == sizeof(uint32_t), "vte::image::Ref must be 32 bits wide");

/* How much of an image, in pixels, may be printed when it starts at column
 * `left` of a screen `columns` wide.
 *
 * DEC STD 070 11.2.2: "Sixels defined to be printed past the right margin are
 * not printed." Zero means the image cannot be placed at all.
 *
 * Pulled out as a pure function because three separate things have to agree
 * about it - the stored surface, the cell footprint, and the run of cells
 * erased underneath - and they are computed in different places. When they
 * disagree, the draw and the lifetime rules act on different rectangles.
 */
inline constexpr long clipped_width_px(long image_width_px,
                                       long left,
                                       long columns,
                                       long cell_width) noexcept
{
        auto const available = columns - left;
        if (available <= 0 || image_width_px <= 0 || cell_width <= 0)
                return 0;

        auto const max_px = available * cell_width;
        return image_width_px < max_px ? image_width_px : max_px;
}

} // namespace vte::image
