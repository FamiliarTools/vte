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
 * admit, divided by the SMALLEST cell it could be laid out in, so that a
 * legal image can never overflow its own coordinates:
 *
 *      cols = ceil(VTE_SIXEL_MAX_WIDTH  / min_cell_width)
 *      rows = ceil(VTE_SIXEL_MAX_HEIGHT / min_cell_height)
 *
 * At the upstream caps of 2048x2052 and a 4x8 cell that is 512 columns and
 * 257 rows, hence 9 bits each. Those are checked below, so RAISING the caps
 * is a build error.
 *
 * BUT THAT CHECK IS NOT SUFFICIENT ON ITS OWN, and it is important not to
 * read it as more than it is. k_min_cell_width/height are an ASSUMPTION, not
 * an enforced floor: the widget clamps cell metrics only to 1x2 and inits
 * them to 1x1, so a real layout can be finer than the assumed minimum and a
 * legal image can genuinely need more tile columns than the field holds.
 *
 * The packing is therefore TOTAL: out-of-range values are masked into their
 * own field and cannot carry into a neighbour. Silently truncating a tile
 * coordinate draws the wrong part of the right image; letting it carry into
 * pool_id draws part of a DIFFERENT image, so masking is strictly the less
 * bad of the two failures. Callers that can refuse a placement must ask
 * fits() first, and refuse, rather than relying on either.
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

/* The smallest cell the widget will lay out in. A cell smaller than this
 * would let a max-size image need more tile coordinates than a Ref can hold.
 */
inline constexpr int k_min_cell_width = 4;
inline constexpr int k_min_cell_height = 8;

static_assert((VTE_SIXEL_MAX_WIDTH + k_min_cell_width - 1) / k_min_cell_width
              <= int(k_ref_tile_col_max) + 1,
              "VTE_SIXEL_MAX_WIDTH admits more cell columns than a Ref can address; "
              "widen k_ref_tile_col_bits or lower the cap");
static_assert((VTE_SIXEL_MAX_HEIGHT + k_min_cell_height - 1) / k_min_cell_height
              <= int(k_ref_tile_row_max) + 1,
              "VTE_SIXEL_MAX_HEIGHT admits more cell rows than a Ref can address; "
              "widen k_ref_tile_row_bits or lower the cap");

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

} // namespace vte::image
