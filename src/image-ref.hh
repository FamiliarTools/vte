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
 *      cols = ceil(VTE_SIXEL_MAX_WIDTH  / VTE_SIXEL_CELL_WIDTH)
 *      rows = ceil(VTE_SIXEL_MAX_HEIGHT / VTE_SIXEL_CELL_HEIGHT)
 *
 * At 2048x2052 over a 10x20 cell that is 205 columns and 103 rows, both
 * comfortably inside 9 bits. This is a bound on constants, checked below, so
 * changing either cap or the emulated cell is a build error rather than a
 * silent wrap into the pool id - which would alias one image onto another.
 *
 * This is the reason the emulated cell has to be FIXED and not the font's.
 * The widget clamps its font cell only to 1x2 pixels; sizing tile
 * coordinates against that would let a legal image need 2048 columns and
 * 1026 rows, which needs 11 bits per axis and does not fit. The packing is
 * total regardless (out-of-range values mask into their own field rather
 * than carrying), but totality only chooses the less bad corruption -
 * drawing the wrong part of the right image instead of part of a different
 * one. The fixed cell is what makes the situation not arise.
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
        (VTE_SIXEL_MAX_WIDTH + VTE_SIXEL_CELL_WIDTH - 1) / VTE_SIXEL_CELL_WIDTH;
inline constexpr int k_max_image_tile_rows =
        (VTE_SIXEL_MAX_HEIGHT + VTE_SIXEL_CELL_HEIGHT - 1) / VTE_SIXEL_CELL_HEIGHT;

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

} // namespace vte::image
