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
 * Ref: what a cell covered by an image stores in VteCellAttr::m_link.
 *
 * Exactly 32 bits wide; see the union in cell.hh for why the field cannot
 * grow. Layout, most significant first:
 *
 *      pool_id  : 14   which image, an index into the image pool
 *      tile_row :  9   which cell row of that image this cell is
 *      tile_col :  9   which cell column of that image this cell is
 *
 * The tile coordinates are relative to the image, not to the screen, so a
 * cell keeps naming the same piece of the image wherever its row is moved
 * to. Rewrap therefore re-anchors an image for free: it rebuilds row_stream
 * only, and the image follows the cells that survived.
 *
 * The coordinate widths cover the largest image the parser admits, divided
 * by the emulated cell that sixel geometry is expressed in:
 *
 *      cols = ceil(VTE_SIXEL_MAX_WIDTH  / VTE_SIXEL_CELL_MIN_WIDTH)
 *      rows = ceil(VTE_SIXEL_MAX_HEIGHT / VTE_SIXEL_CELL_MIN_HEIGHT)
 *
 * which at 2048x2052 over the 4x8 minimum is 512 columns and 257 rows, both
 * within 9 bits. Images are laid out against the font's cell, which is
 * normally larger, so this is the worst case rather than the usual one.
 *
 * The minimum has to be enforced: the widget clamps its font cell only to
 * 1x2 pixels, and at that size a legal image would need 11 bits of column.
 * Terminal::image_cell_size() applies the minimum and is the only producer
 * of an image layout cell.
 *
 * 14 bits of pool id is 16383 concurrently live images; id 0 is reserved for
 * "no image", so that a zeroed Ref names nothing.
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
 * caps and the emulated cell, so that the assertions below hold for any font.
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

        /* Whether these values can be represented exactly. The placement path
         * must check this and refuse the image, rather than store a masked
         * reference.
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

        /* A Ref is valid iff it names an image. A zeroed Ref is invalid, so
         * clearing a cell cannot conjure a reference to a live image.
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

        /* Whether two cells belong to the same stripe, that is one tile row
         * of one image. The stripe is the unit of image lifetime, so that a
         * single surviving cell pins one row of tiles rather than the whole
         * image.
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
 * @left of a screen @columns wide; zero if it cannot be placed at all.
 *
 * DEC STD 070 11.2.2: "Sixels defined to be printed past the right margin are
 * not printed."
 *
 * A pure function because the stored surface, the cell footprint and the run
 * of cells erased underneath are computed in different places and must agree
 * on the same rectangle.
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
