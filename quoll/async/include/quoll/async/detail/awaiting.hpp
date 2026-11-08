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

#ifndef QUOLL_ASYNC_DETAIL_AWAITING_HPP
#define QUOLL_ASYNC_DETAIL_AWAITING_HPP

#include <quoll/async/await_context.hpp>

#include <concepts>
#include <coroutine>
#include <type_traits>

namespace quoll::async::detail {

// What an await_suspend returns that its caller must use: a coroutine to resume, whether it suspended.
template <class R>
concept not_void = !std::is_void_v<R>;

// The way to what an awaitable does as it suspends, for Quoll's own awaiting: a co_await, stop_scope, any_stream. task
// keeps its await_suspend for this alone, a friend, as a program never starts a task itself: it awaits it, or spawns
// it. Any other awaitable's is public, and reached the same way.
struct awaiting {
  // Constrained, so that ContextAwaitable asks rather than fails to compile, from here, where task's await_suspend is
  // reachable. What it returns (a coroutine to resume, whether it suspended) must not be dropped.
  template <class A>
    requires requires(A &awaitable, std::coroutine_handle<> handle, await_context const &context) {
      { awaitable.await_suspend(handle, context) } -> std::same_as<void>;
    }
  static void suspend(A &awaitable, std::coroutine_handle<> handle, await_context const &context) {
    awaitable.await_suspend(handle, context);
  }

  template <class A>
    requires requires(A &awaitable, std::coroutine_handle<> handle, await_context const &context) {
      { awaitable.await_suspend(handle, context) } -> not_void;
    }
  [[nodiscard]] static decltype(auto) suspend(A &awaitable, std::coroutine_handle<> handle,
                                              await_context const &context) {
    return awaitable.await_suspend(handle, context);
  }
};

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_AWAITING_HPP
