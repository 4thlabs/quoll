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

#ifndef QUOLL_ASYNC_CURRENT_EXECUTOR_HPP
#define QUOLL_ASYNC_CURRENT_EXECUTOR_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/executor_ref.hpp>

#include <coroutine>
#include <optional>

namespace quoll::async {

/** @brief Awaitable returned by current_executor(). */
class executor_awaitable {
public:
  [[nodiscard]] bool await_ready() const noexcept { return false; }

  // Reads the executor and resumes at once: the coroutine never actually suspends.
  [[nodiscard]] bool await_suspend(std::coroutine_handle<>, await_context const &context) noexcept {
    executor_.emplace(context.executor);
    return false;
  }

  [[nodiscard]] executor_ref await_resume() noexcept { return *executor_; }

private:
  // executor_ref has no empty state.
  std::optional<executor_ref> executor_;
};

static_assert(ContextAwaitable<executor_awaitable>);

/**
 * @brief The executor of the awaiting coroutine's await_context: `co_await current_executor()`.
 *
 * A reference, valid while the chain of awaiting coroutines runs; co_spawn() copies what it references, and code that
 * keeps it holds an any_executor. Ask it for the services of the context it posts to with `query<S>()`.
 */
[[nodiscard]] inline executor_awaitable current_executor() noexcept { return {}; }

} // namespace quoll::async

#endif // QUOLL_ASYNC_CURRENT_EXECUTOR_HPP
