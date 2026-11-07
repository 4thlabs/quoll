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

#ifndef QUOLL_ASYNC_DETAIL_DETACHED_HPP
#define QUOLL_ASYNC_DETAIL_DETACHED_HPP

#include <quoll/async/detail/recycled_frame.hpp>
#include <quoll/async/work_item.hpp>

#include <coroutine>
#include <exception>
#include <utility>

namespace quoll::async::detail {

// Root coroutine of co_spawn. Created suspended; once released and resumed, its frame destroys itself when it ends.
class detached {
public:
  class promise_type : public recycled_frame {
  public:
    [[nodiscard]] detached get_return_object() noexcept {
      return detached{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }

    [[nodiscard]] std::suspend_never final_suspend() const noexcept { return {}; }

    void return_void() const noexcept {}

    // The body hands every task exception to the completion handler: what is left is the handler throwing.
    [[noreturn]] void unhandled_exception() const noexcept { std::terminate(); }

  private:
    friend class detached;

    work_item start_;
  };

  detached(detached const &) = delete;
  detached &operator=(detached const &) = delete;

  detached(detached &&other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  detached &operator=(detached &&) = delete;

  ~detached() { destroy(); }

  // What to post to start the coroutine. It lives in the frame, so it stays valid until the coroutine runs.
  [[nodiscard]] work_item &start_item() const noexcept {
    auto &item = handle_.promise().start_;

    item.handle = handle_;
    return item;
  }

  // Gives up ownership: from now on the frame destroys itself when the coroutine ends.
  void release() noexcept { handle_ = {}; }

private:
  explicit detached(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

  void destroy() noexcept {
    if (handle_) {
      handle_.destroy();
    }
  }

  std::coroutine_handle<promise_type> handle_;
};

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_DETACHED_HPP
