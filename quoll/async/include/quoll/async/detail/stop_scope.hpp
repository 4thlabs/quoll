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

#ifndef QUOLL_ASYNC_DETAIL_STOP_SCOPE_HPP
#define QUOLL_ASYNC_DETAIL_STOP_SCOPE_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/detail/awaiting.hpp>
#include <quoll/async/task.hpp>

#include <coroutine>
#include <optional>
#include <stop_token>
#include <utility>

namespace quoll::async::detail {

// Awaits a task under `stop_token` rather than the awaiter's, on the awaiter's executor. Movable until it suspends, so
// that it can be co_await-ed as a temporary.
template <class T>
class [[nodiscard]] stop_scope {
public:
  stop_scope(task<T> work, std::stop_token stop_token) noexcept
      : work_(std::move(work)), stop_token_(std::move(stop_token)) {}

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter, await_context const &context) {
    context_.emplace(await_context{context.executor, std::move(stop_token_)});

    return awaiting::suspend(work_, awaiter, *context_);
  }

  T await_resume() { return work_.await_resume(); }

private:
  task<T> work_;
  std::stop_token stop_token_;
  std::optional<await_context> context_;
};

static_assert(ContextAwaitable<stop_scope<void>>);
static_assert(ContextAwaitable<stop_scope<int>>);

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_STOP_SCOPE_HPP
