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

#ifndef QUOLL_TEST_PROBE_HPP
#define QUOLL_TEST_PROBE_HPP

#include <coroutine>
#include <exception>

namespace quoll::test {

namespace detail {

struct probe_coroutine {
  struct promise_type {
    [[nodiscard]] probe_coroutine get_return_object() noexcept {
      return {std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }

    [[nodiscard]] std::suspend_always final_suspend() const noexcept { return {}; }

    void return_void() const noexcept {}

    [[noreturn]] void unhandled_exception() const noexcept { std::terminate(); }
  };

  std::coroutine_handle<promise_type> handle;
};

inline probe_coroutine mark_resumed(bool &resumed) {
  resumed = true;
  co_return;
}

} // namespace detail

/**
 * @brief A coroutine that only tells whether it was resumed: the handle to give an awaitable driven by hand, so that
 * a test can destroy the awaitable in any state and see whether its wake still reaches the coroutine.
 *
 * Resumed at most once: a second resumption would resume a coroutine that has ended.
 */
class probe {
public:
  probe() : coroutine_(detail::mark_resumed(resumed_).handle) {}

  probe(probe const &) = delete;
  probe &operator=(probe const &) = delete;
  probe(probe &&) = delete;
  probe &operator=(probe &&) = delete;

  ~probe() { coroutine_.destroy(); }

  [[nodiscard]] std::coroutine_handle<> handle() const noexcept { return coroutine_; }

  [[nodiscard]] bool resumed() const noexcept { return resumed_; }

private:
  bool resumed_ = false;
  std::coroutine_handle<detail::probe_coroutine::promise_type> coroutine_;
};

} // namespace quoll::test

#endif // QUOLL_TEST_PROBE_HPP
