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

#ifndef QUOLL_ASYNC_DETAIL_OWNED_CHAIN_HPP
#define QUOLL_ASYNC_DETAIL_OWNED_CHAIN_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/detail/awaiting.hpp>
#include <quoll/async/detail/chain_owner.hpp>
#include <quoll/async/detail/context_promise.hpp>
#include <quoll/async/detail/non_void.hpp>
#include <quoll/async/task.hpp>

#include <coroutine>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace quoll::async::detail {

// Awaits a task as the owner of its chain, on the awaiter's executor, under the awaiter's stop token or one of its own:
// what the task returns, rethrows what it throws, or std::nullopt once the chain ended on a stop, which it has then
// destroyed from the top, as no coroutine of it resumes. Movable until it suspends, so that it can be co_await-ed as a
// temporary, and only so: awaited by name, it could be moved by another coroutine while it waits. Thread safety:
// awaited on one thread; its owner's stop() comes from any.
template <class T>
class [[nodiscard]] owned_chain {
public:
  explicit owned_chain(task<T> work) noexcept : work_(std::move(work)) {}

  owned_chain(task<T> work, std::stop_token stop_token) noexcept
      : stop_token_(std::move(stop_token)), work_(std::move(work)) {}

  owned_chain(owned_chain const &) = delete;
  owned_chain &operator=(owned_chain const &) = delete;
  owned_chain &operator=(owned_chain &&) = delete;

  // Moved into the awaiting coroutine before it suspends, when nothing has its address yet: awaited as a temporary only
  // (see awaited_as_a_temporary_v), it is never moved once awaited, and the one moved from has no task left.
  owned_chain(owned_chain &&other) noexcept
      : stop_token_(std::move(other.stop_token_)), work_(std::move(other.work_)) {}

  // Never with its task suspended but once await_resume() has unwound it: a stop reaches the nearest owner, which
  // nothing above unwinds.
  ~owned_chain() = default;

  /** @throws std::logic_error for a task with no frame: moved from, a second co_await of it included. */
  [[nodiscard]] bool await_ready() const { return work_.await_ready(); }

  [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter, await_context const &context) {
    owner_.bind(awaiter, context.executor);
    context_.emplace(
        await_context{context.executor, stop_token_ ? std::move(*stop_token_) : context.stop_token, &owner_});
    return awaiting::suspend(work_, awaiter, *context_);
  }

  [[nodiscard]] std::optional<non_void_t<T>> await_resume() {
    if (owner_.stopped()) {
      owner_.unwind();

      // Destroyed here, from the top: the frames below it go with it.
      [[maybe_unused]] auto const unwound = std::move(work_);

      return std::nullopt;
    }

    if constexpr (std::is_void_v<T>) {
      work_.await_resume();
      return non_void_t<T>{};
    } else {
      return work_.await_resume();
    }
  }

private:
  // In this order, so that the task's frames, destroyed first, still reach their context and its owner.
  chain_owner owner_;
  std::optional<await_context> context_;
  std::optional<std::stop_token> stop_token_;
  task<T> work_;
};

template <class T>
inline constexpr bool awaited_as_a_temporary_v<owned_chain<T>> = true;

static_assert(ContextAwaitable<owned_chain<void>>);
static_assert(ContextAwaitable<owned_chain<int>>);

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_OWNED_CHAIN_HPP
