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

#ifndef QUOLL_ASYNC_DETAIL_CONTEXT_PROMISE_HPP
#define QUOLL_ASYNC_DETAIL_CONTEXT_PROMISE_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/detail/context_adapter.hpp>
#include <quoll/async/detail/recycled_frame.hpp>

#include <type_traits>
#include <utility>

namespace quoll::async {

template <class T>
class task;

} // namespace quoll::async

namespace quoll::async::detail {

template <class A>
inline constexpr bool is_task_v = false;

template <class T>
inline constexpr bool is_task_v<task<T>> = true;

// Base of the promises of coroutines running on an await_context: every co_await hands the context to the awaited
// ContextAwaitable, and anything else is rejected at compile time.
class context_promise : public recycled_frame {
public:
  template <class A>
    requires ContextAwaitable<std::remove_cvref_t<A>>
  [[nodiscard]] context_adapter<A> await_transform(A &&awaitable) noexcept(
      std::is_nothrow_constructible_v<context_adapter<A>, A &&, await_context const &>) {
    // A task is moved into the co_await, so that its frame belongs to its awaiter: one awaited by name could be
    // destroyed elsewhere while it runs, or awaited again once it has ended.
    static_assert(!is_task_v<std::remove_cvref_t<A>> || !std::is_lvalue_reference_v<A>,
                  "quoll: co_await std::move(task): a task awaited by name could be dropped while it runs, or "
                  "started again");

    return context_adapter<A>{std::forward<A>(awaitable), *context_};
  }

  void set_context(await_context const &context) noexcept { context_ = &context; }

private:
  await_context const *context_ = nullptr;
};

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_CONTEXT_PROMISE_HPP
