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
#include <vector>

#include "image-ref.hh"

namespace vte::image {

/*
 * PoolT: the id space that vte::image::Ref::pool_id() indexes into.
 *
 * A cell names its image by a 14-bit id and nothing else - there is no room
 * in VteCellAttr for a generation counter. That makes id REUSE the central
 * hazard, and it is not a leak, it is a wrong picture: if an image is freed
 * while a cell in the scrollback still refers to its id, and that id is
 * immediately handed to the next image, the stale cell silently starts
 * displaying the new image. Worse, it does so at the stale cell's own tile
 * coordinates, so the result is a slice of one image embedded in another.
 *
 * The fix is to make an id unreusable until nothing refers to it, which is
 * mark and sweep - the pattern the ring already uses for hyperlink indices
 * in hyperlink_gc(), for the same reason and at the same amortised cost.
 * An id is therefore in one of three states:
 *
 *      Free      - available to allocate.
 *      Live      - allocated, payload resolvable.
 *      Retired   - the image is gone, but cells may still refer to the id.
 *                  Resolves to nullptr, and is NOT available to allocate.
 *
 * retire() moves Live to Retired. Only a sweep moves Retired to Free, and
 * only for ids that the sweep did not see referenced. A retired id that is
 * still referenced stays retired: reuse is what we are preventing, and its
 * cells resolve to nullptr and draw as ordinary background until they die.
 * The retained set is therefore bounded by the ring, not unbounded.
 *
 * Note this is why lookup() returning nullptr is a NORMAL outcome that
 * callers must handle by drawing nothing, rather than an assertion. A cell
 * outliving its image is expected, not a bug.
 *
 * The pool is non-owning: it maps id to payload. Ownership stays with
 * whoever holds the images.
 */

template<typename T>
class PoolT {
public:
        enum class State : uint8_t {
                Free,
                Live,
                Retired,
        };

private:
        struct Entry {
                T* payload{nullptr};
                State state{State::Free};
                bool marked{false};
        };

        /* Indexed by id. Slot 0 is permanently unused: k_ref_pool_id_none is
         * reserved so that a zeroed Ref names nothing.
         */
        std::vector<Entry> m_entries{};

        /* Ids known to be Free, for O(1) allocation. Ids never allocated yet
         * are not in here; they come from growing m_entries.
         */
        std::vector<uint32_t> m_free_list{};

        bool m_sweeping{false};

        inline constexpr bool in_range(uint32_t id) const noexcept
        {
                return id != k_ref_pool_id_none && id < m_entries.size();
        }

public:
        PoolT()
        {
                /* Slot 0, the reserved "no image" id. */
                m_entries.emplace_back();
        }

        /* Allocate an id for a payload. Returns k_ref_pool_id_none if the id
         * space is exhausted, which the caller must treat as "cannot store
         * this image" rather than ignoring.
         */
        uint32_t allocate(T* payload) noexcept
        {
                uint32_t id = k_ref_pool_id_none;

                if (!m_free_list.empty()) {
                        id = m_free_list.back();
                        m_free_list.pop_back();
                } else if (m_entries.size() <= k_ref_pool_id_max) {
                        id = uint32_t(m_entries.size());
                        m_entries.emplace_back();
                } else {
                        return k_ref_pool_id_none;
                }

                auto& e = m_entries[id];
                e.payload = payload;
                e.state = State::Live;
                e.marked = false;
                return id;
        }

        /* The payload is going away. The id is quarantined, not freed. */
        void retire(uint32_t id) noexcept
        {
                if (!in_range(id))
                        return;

                auto& e = m_entries[id];
                if (e.state != State::Live)
                        return;

                e.payload = nullptr;
                e.state = State::Retired;
        }

        /* nullptr if the id is free or retired: a cell may legitimately
         * outlive its image.
         */
        T* lookup(uint32_t id) const noexcept
        {
                if (!in_range(id))
                        return nullptr;

                auto const& e = m_entries[id];
                return e.state == State::Live ? e.payload : nullptr;
        }

        inline T* lookup(Ref const& ref) const noexcept
        {
                return lookup(ref.pool_id());
        }

        State state(uint32_t id) const noexcept
        {
                return in_range(id) ? m_entries[id].state : State::Free;
        }

        /* Mark and sweep. Between sweep_begin() and sweep_end() the caller
         * marks every id still referenced by a live cell.
         */
        void sweep_begin() noexcept
        {
                for (auto& e : m_entries)
                        e.marked = false;
                m_sweeping = true;
        }

        void mark(uint32_t id) noexcept
        {
                if (in_range(id))
                        m_entries[id].marked = true;
        }

        inline void mark(Ref const& ref) noexcept
        {
                mark(ref.pool_id());
        }

        /* Returns the number of ids returned to the free list. A sweep that
         * was never begun frees nothing, rather than freeing everything.
         */
        size_t sweep_end() noexcept
        {
                if (!m_sweeping)
                        return 0;

                m_sweeping = false;

                size_t freed = 0;
                for (uint32_t id = 1; id < m_entries.size(); id++) {
                        auto& e = m_entries[id];
                        if (e.state != State::Retired || e.marked)
                                continue;

                        e.state = State::Free;
                        e.payload = nullptr;
                        m_free_list.push_back(id);
                        freed++;
                }

                return freed;
        }

        size_t live_count() const noexcept
        {
                size_t n = 0;
                for (auto const& e : m_entries)
                        n += (e.state == State::Live);
                return n;
        }

        size_t retired_count() const noexcept
        {
                size_t n = 0;
                for (auto const& e : m_entries)
                        n += (e.state == State::Retired);
                return n;
        }

        /* How many ids could still be handed out. */
        size_t available() const noexcept
        {
                return m_free_list.size() + (k_ref_pool_id_max + 1 - m_entries.size());
        }
};

} // namespace vte::image
