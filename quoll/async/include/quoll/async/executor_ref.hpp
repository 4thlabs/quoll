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

#ifndef QUOLL_ASYNC_EXECUTOR_REF_HPP
#define QUOLL_ASYNC_EXECUTOR_REF_HPP

#include <quoll/async/detail/executor_vtable.hpp>
#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/work_item.hpp>

#include <concepts>
#include <optional>
#include <type_traits>

namespace quoll::async {

class any_executor;

/**
 * @brief Non-owning, type-erased reference to an Executor; models Executor itself.
 *
 * Trivially copyable and never allocates. The referenced executor must outlive every executor_ref to it: binding a
 * temporary is rejected at compile time.
 *
 * Thread safety: same as the referenced executor.
 */
class executor_ref {
public:
  // The exclusion comes first: it stops Executor<executor_ref> from checking this constructor against itself.
  /** @brief References `executor`, which must outlive it. */
  template <class E>
    requires(!std::same_as<E, executor_ref> && !std::same_as<E, any_executor> && Executor<E>)
  explicit executor_ref(E const &executor) noexcept : executor_(&executor), vtable_(&detail::executor_vtable_for<E>) {}

  template <class E>
    requires(!std::same_as<E, executor_ref> && !std::same_as<E, any_executor> && Executor<E>)
  explicit executor_ref(E const &&executor) = delete;

  /**
   * @brief References the executor `executor` wraps, rather than the wrapper: its target() and query() are the wrapped
   * executor's. Valid while `executor` lives and is not moved. Defined in any_executor.hpp.
   */
  inline explicit executor_ref(any_executor const &executor) noexcept;
  explicit executor_ref(any_executor const &&executor) = delete;

  /** @brief Posts `item` to the referenced executor. */
  void post(work_item &item) const { vtable_->post(executor_, item); }

  /** @brief The referenced executor if its type is exactly `E`, otherwise `nullptr`. */
  template <class E>
  [[nodiscard]] E const *target() const noexcept {
    return vtable_->type == detail::type_id_of<E>() ? static_cast<E const *>(executor_) : nullptr;
  }

  /** @brief The service `S` offered by the referenced executor, if it offers it (see service_list). */
  template <class S>
  [[nodiscard]] std::optional<S> query() const {
    std::optional<S> found;

    static_cast<void>(vtable_->query(executor_, detail::type_id_of<S>(), &found));
    return found;
  }

  /** @brief Equal when both reference executors of the same type that compare equal. */
  [[nodiscard]] friend bool operator==(executor_ref const &lhs, executor_ref const &rhs) noexcept {
    return lhs.vtable_ == rhs.vtable_ && lhs.vtable_->equal(lhs.executor_, rhs.executor_);
  }

private:
  friend class any_executor;

  void const *executor_;
  detail::executor_vtable const *vtable_;
};

static_assert(Executor<executor_ref>);
static_assert(std::is_trivially_copyable_v<executor_ref>);

} // namespace quoll::async

#endif // QUOLL_ASYNC_EXECUTOR_REF_HPP
