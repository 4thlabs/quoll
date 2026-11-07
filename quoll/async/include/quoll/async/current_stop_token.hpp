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

#ifndef QUOLL_ASYNC_CURRENT_STOP_TOKEN_HPP
#define QUOLL_ASYNC_CURRENT_STOP_TOKEN_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>

#include <coroutine>
#include <stop_token>
#include <utility>

namespace quoll::async {

/** @brief Awaitable returned by current_stop_token(). Thread safety: none, as what awaits it. */
class [[nodiscard]] stop_token_awaitable {
public:
  [[nodiscard]] bool await_ready() const noexcept { return false; }

  // Reads the token and resumes at once: the coroutine never actually suspends.
  [[nodiscard]] bool await_suspend(std::coroutine_handle<>, await_context const &context) noexcept {
    stop_token_ = context.stop_token;
    return false;
  }

  [[nodiscard]] std::stop_token await_resume() noexcept { return std::move(stop_token_); }

private:
  std::stop_token stop_token_;
};

static_assert(ContextAwaitable<stop_token_awaitable>);

/** @brief The stop token of the awaiting coroutine's await_context: `co_await current_stop_token()`. */
[[nodiscard]] inline stop_token_awaitable current_stop_token() noexcept { return {}; }

} // namespace quoll::async

#endif // QUOLL_ASYNC_CURRENT_STOP_TOKEN_HPP
