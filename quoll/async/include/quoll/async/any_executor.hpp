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

#ifndef QUOLL_ASYNC_ANY_EXECUTOR_HPP
#define QUOLL_ASYNC_ANY_EXECUTOR_HPP

#include <quoll/async/detail/executor_vtable.hpp>
#include <quoll/async/detail/type_id.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/executor_ref.hpp>
#include <quoll/async/work_item.hpp>

#include <cassert>
#include <concepts>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace quoll::async {

/**
 * @brief Owning, type-erased Executor; models Executor itself.
 *
 * Holds a copy of any executor, for code that keeps one without being a template over its type: what serve() spawns
 * its connections on, for instance. Built from an executor_ref, it copies the referenced executor, which may then go
 * away. An executor of up to eight pointers (an asio::any_io_executor), that moves without throwing, is held in place;
 * a bigger one is allocated on the heap. Posting never allocates.
 *
 * Answers the wrapped executor's service queries (see service_list). A moved-from any_executor can only be assigned to
 * or destroyed.
 *
 * Thread safety: same as the wrapped executor.
 */
class any_executor {
public:
  // The exclusions come first: they stop Executor<any_executor> from checking this constructor against itself.
  template <class E>
    requires(!std::same_as<E, any_executor> && !std::same_as<E, executor_ref> && Executor<E>)
  explicit any_executor(E const &executor) : vtable_(&detail::executor_vtable_for<E>) {
    executor_ = vtable_->copy(&executor, storage_);
  }

  /** @brief A copy of the executor `executor` references. */
  explicit any_executor(executor_ref executor)
      : vtable_(executor.vtable_), executor_(vtable_->copy(executor.executor_, storage_)) {}

  any_executor(any_executor const &other) { copy_from(other); }

  any_executor(any_executor &&other) noexcept { take(other); }

  any_executor &operator=(any_executor const &other) {
    if (this != &other) {
      any_executor copy{other};

      reset();
      take(copy);
    }

    return *this;
  }

  any_executor &operator=(any_executor &&other) noexcept {
    if (this != &other) {
      reset();
      take(other);
    }

    return *this;
  }

  ~any_executor() { reset(); }

  /** @brief Posts `item` to the wrapped executor. */
  void post(work_item &item) const {
    assert(vtable_ != nullptr && "any_executor used after being moved from");
    vtable_->post(executor_, item);
  }

  /** @brief The wrapped executor if its type is exactly `E`, otherwise `nullptr`. */
  template <class E>
  [[nodiscard]] E const *target() const noexcept {
    return vtable_ != nullptr && vtable_->type == detail::type_id_of<E>() ? static_cast<E const *>(executor_) : nullptr;
  }

  /** @brief The service `S` offered by the wrapped executor, if it offers it (see service_list). */
  template <class S>
  [[nodiscard]] std::optional<S> query() const {
    assert(vtable_ != nullptr && "any_executor used after being moved from");

    std::optional<S> found;

    static_cast<void>(vtable_->query(executor_, detail::type_id_of<S>(), &found));
    return found;
  }

  /** @brief Equal when both wrap executors of the same type that compare equal. */
  [[nodiscard]] friend bool operator==(any_executor const &lhs, any_executor const &rhs) noexcept {
    return lhs.vtable_ == rhs.vtable_ && (lhs.vtable_ == nullptr || lhs.vtable_->equal(lhs.executor_, rhs.executor_));
  }

private:
  friend class executor_ref;

  void copy_from(any_executor const &other) {
    assert(other.vtable_ != nullptr && "any_executor copied after being moved from");
    vtable_ = other.vtable_;
    executor_ = vtable_->copy(other.executor_, storage_);
  }

  void take(any_executor &other) noexcept {
    vtable_ = std::exchange(other.vtable_, nullptr);

    if (vtable_ == nullptr) {
      return;
    }

    if (vtable_->in_place) {
      vtable_->relocate(other.executor_, storage_);
      executor_ = storage_;
    } else {
      executor_ = other.executor_;
    }

    other.executor_ = nullptr;
  }

  void reset() noexcept {
    if (vtable_ != nullptr) {
      vtable_->destroy(executor_);
      vtable_ = nullptr;
      executor_ = nullptr;
    }
  }

  alignas(std::max_align_t) std::byte storage_[detail::executor_storage_size];
  detail::executor_vtable const *vtable_ = nullptr;
  void *executor_ = nullptr;
};

static_assert(Executor<any_executor>);

inline executor_ref::executor_ref(any_executor const &executor) noexcept
    : executor_(executor.executor_), vtable_(executor.vtable_) {
  assert(vtable_ != nullptr && "executor_ref to an any_executor moved from");
}

} // namespace quoll::async

#endif // QUOLL_ASYNC_ANY_EXECUTOR_HPP
