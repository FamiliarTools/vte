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
 * A cell names its image by a 14-bit id alone; there is no room in
 * VteCellAttr for a generation counter. Reusing an id while a cell in the
 * scrollback still refers to it would make that cell display a slice of a
 * different image, at its own tile coordinates.
 *
 * An id is therefore made unreusable until nothing refers to it, by mark and
 * sweep, as the ring already does for hyperlink indices in hyperlink_gc().
 * An id is in one of three states:
 *
 *      Free      - available to allocate.
 *      Live      - allocated, payload resolvable.
 *      Retired   - the image is gone, but cells may still refer to the id;
 *                  resolves to nullptr, and is not available to allocate.
 *
 * retire() moves Live to Retired. Only a sweep moves Retired to Free, and
 * only for ids it did not see referenced, so the retained set is bounded by
 * the ring. This is why lookup() returning nullptr is an expected outcome
 * that callers handle by drawing nothing.
 *
 * The pool is non-owning: it maps id to payload, and ownership stays with
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
        std::vector<pool_id_t> m_free_list{};

        bool m_sweeping{false};

        inline constexpr bool in_range(pool_id_t id) const noexcept
        {
                return id != k_ref_pool_id_none && id.value() < m_entries.size();
        }

public:
        PoolT()
        {
                /* Slot 0, the reserved "no image" id. */
                m_entries.emplace_back();
        }

        /* Allocate an id for a payload. Returns k_ref_pool_id_none if the id
         * space is exhausted; the caller must then refuse the image.
         */
        pool_id_t allocate(T* payload) noexcept
        {
                auto id = k_ref_pool_id_none;

                if (!m_free_list.empty()) {
                        id = m_free_list.back();
                        m_free_list.pop_back();
                } else if (m_entries.size() <= k_ref_pool_id_max) {
                        id = pool_id_t{uint32_t(m_entries.size())};
                        m_entries.emplace_back();
                } else {
                        return k_ref_pool_id_none;
                }

                auto& e = m_entries[id.value()];
                e.payload = payload;
                e.state = State::Live;
                e.marked = false;
                return id;
        }

        /* The payload is going away. The id is quarantined, not freed. */
        void retire(pool_id_t id) noexcept
        {
                if (!in_range(id))
                        return;

                auto& e = m_entries[id.value()];
                if (e.state != State::Live)
                        return;

                e.payload = nullptr;
                e.state = State::Retired;
        }

        /* nullptr if the id is free or retired: a cell may legitimately
         * outlive its image.
         */
        T* lookup(pool_id_t id) const noexcept
        {
                if (!in_range(id))
                        return nullptr;

                auto const& e = m_entries[id.value()];
                return e.state == State::Live ? e.payload : nullptr;
        }

        inline T* lookup(Ref const& ref) const noexcept
        {
                return lookup(ref.pool_id());
        }

        State state(pool_id_t id) const noexcept
        {
                return in_range(id) ? m_entries[id.value()].state : State::Free;
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

        void mark(pool_id_t id) noexcept
        {
                if (in_range(id))
                        m_entries[id.value()].marked = true;
        }

        inline void mark(Ref const& ref) noexcept
        {
                mark(ref.pool_id());
        }

        /* Returns the number of ids returned to the free list. A sweep that
         * was never begun frees nothing.
         */
        size_t sweep_end() noexcept
        {
                if (!m_sweeping)
                        return 0;

                m_sweeping = false;

                size_t freed = 0;
                for (auto id = size_t{1}; id < m_entries.size(); id++) {
                        auto& e = m_entries[id];
                        if (e.state != State::Retired || e.marked)
                                continue;

                        e.state = State::Free;
                        e.payload = nullptr;
                        m_free_list.push_back(pool_id_t{uint32_t(id)});
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
