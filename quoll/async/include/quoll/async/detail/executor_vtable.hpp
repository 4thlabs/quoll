//   ___              _ _
//  / _ \ _   _  ___ | | |
// | | | | | | |/ _ \| | |
// | |_| | |_| | (_) | | |
//  \__\_\\__,_|\___/|_|_|
//
// BSD 3-Clause License
//
// Copyright (c) 2026, Thomas Gourgues (thomas.gourgues@gmail.com)
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef QUOLL_ASYNC_DETAIL_EXECUTOR_VTABLE_HPP
#define QUOLL_ASYNC_DETAIL_EXECUTOR_VTABLE_HPP

#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/service.hpp>
#include <quoll/async/work_item.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>

namespace quoll::async::detail {

// Room for an executor held in place by an any_executor: an asio executor, a pointer to a loop or a pool.
inline constexpr std::size_t executor_storage_size = 4 * sizeof(void *);

// One table per executor type, shared by executor_ref (post, equal, query, type) and any_executor (all of it).
struct executor_vtable {
  void (*post)(void const *executor, work_item &item);
  bool (*equal)(void const *lhs, void const *rhs) noexcept;
  // Stores the service named by `service` in `out`, a std::optional of it; false if the executor does not offer it.
  bool (*query)(void const *executor, type_id service, void *out);
  // Copies `executor` into `storage` when held in place, otherwise onto the heap; returns the copy.
  void *(*copy)(void const *executor, void *storage);
  // Moves an executor held in place into `storage`, and destroys the source.
  void (*relocate)(void *executor, void *storage) noexcept;
  // Destroys a copy made by `copy`.
  void (*destroy)(void *executor) noexcept;
  bool in_place;
  type_id type;
};

template <class E>
[[nodiscard]] consteval bool fits_in_place() noexcept {
  return sizeof(E) <= executor_storage_size && alignof(E) <= alignof(std::max_align_t) &&
         std::is_nothrow_move_constructible_v<E>;
}

template <class E, class... S>
[[nodiscard]] bool query_services(E const &executor, type_id service_type, void *out, service_list<S...>) {
  auto const found = [&]<class One>() {
    if (service_type != type_id_of<One>()) {
      return false;
    }

    static_cast<std::optional<One> *>(out)->emplace(executor.query(service<One>));
    return true;
  };

  return (found.template operator()<S>() || ...);
}

template <class E>
[[nodiscard]] bool query_executor(void const *executor, type_id service_type, void *out) {
  if constexpr (requires { typename E::services; }) {
    return query_services(*static_cast<E const *>(executor), service_type, out, typename E::services{});
  } else {
    return false;
  }
}

template <Executor E>
inline constexpr executor_vtable executor_vtable_for{
    [](void const *executor, work_item &item) { static_cast<E const *>(executor)->post(item); },
    [](void const *lhs, void const *rhs) noexcept {
      return *static_cast<E const *>(lhs) == *static_cast<E const *>(rhs);
    },
    &query_executor<E>,
    [](void const *executor, void *storage) -> void * {
      auto const &source = *static_cast<E const *>(executor);

      if constexpr (fits_in_place<E>()) {
        return std::construct_at(static_cast<E *>(storage), source);
      } else {
        return std::make_unique<E>(source).release();
      }
    },
    [](void *executor, void *storage) noexcept {
      if constexpr (fits_in_place<E>()) {
        auto &source = *static_cast<E *>(executor);

        std::construct_at(static_cast<E *>(storage), std::move(source));
        std::destroy_at(&source);
      }
    },
    [](void *executor) noexcept {
      if constexpr (fits_in_place<E>()) {
        std::destroy_at(static_cast<E *>(executor));
      } else {
        // Owned from here: the unique_ptr frees the copy.
        std::unique_ptr<E> const owned{static_cast<E *>(executor)};
      }
    },
    fits_in_place<E>(),
    type_id_of<E>(),
};

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_EXECUTOR_VTABLE_HPP
