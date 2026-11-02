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

#ifndef QUOLL_ASYNC_DETAIL_WHEN_ALL_RUNNER_HPP
#define QUOLL_ASYNC_DETAIL_WHEN_ALL_RUNNER_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/detail/context_promise.hpp>
#include <quoll/async/detail/non_void.hpp>
#include <quoll/async/detail/when_all_state.hpp>
#include <quoll/async/task.hpp>

#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

namespace quoll::async::detail {

// Destroys the runner's frame, then transfers to the when_all parent if this was the last arrival. The state is read
// before the frame goes away, and outlives it: the parent cannot resume before this arrival.
struct when_all_final_awaiter {
  [[nodiscard]] bool await_ready() const noexcept { return false; }

  template <class Promise>
  [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
    auto &state = handle.promise().state();

    handle.destroy();
    return state.arrive();
  }

  void await_resume() const noexcept {}
};

// Coroutine awaiting one when_all child. Owns its frame until started, then the frame destroys itself.
class when_all_runner {
public:
  class promise_type : public context_promise {
  public:
    [[nodiscard]] when_all_runner get_return_object() noexcept {
      return when_all_runner{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }

    [[nodiscard]] when_all_final_awaiter final_suspend() const noexcept { return {}; }

    void return_void() const noexcept {}

    // The body catches everything and reports it to the state.
    [[noreturn]] void unhandled_exception() const noexcept { std::terminate(); }

    void set_state(when_all_state &state) noexcept { state_ = &state; }

    [[nodiscard]] when_all_state &state() const noexcept { return *state_; }

  private:
    when_all_state *state_ = nullptr;
  };

  when_all_runner(when_all_runner const &) = delete;
  when_all_runner &operator=(when_all_runner const &) = delete;

  when_all_runner(when_all_runner &&other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  when_all_runner &operator=(when_all_runner &&other) noexcept {
    if (this != &other) {
      destroy();
      handle_ = std::exchange(other.handle_, {});
    }

    return *this;
  }

  ~when_all_runner() { destroy(); }

  // Runs the child inline until its first suspension; the frame then owns itself.
  void start(await_context const &context, when_all_state &state) {
    auto &promise = handle_.promise();

    promise.set_context(context);
    promise.set_state(state);
    std::exchange(handle_, {}).resume();
  }

private:
  explicit when_all_runner(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

  void destroy() noexcept {
    if (handle_) {
      handle_.destroy();
    }
  }

  std::coroutine_handle<promise_type> handle_;
};

template <class T>
when_all_runner run_when_all_child(task<T> child, std::optional<non_void_t<T>> &slot, when_all_state &state) {
  try {
    if constexpr (std::is_void_v<T>) {
      co_await std::move(child);
      slot.emplace();
    } else {
      slot.emplace(co_await std::move(child));
    }
  } catch (...) {
    state.fail(std::current_exception());
  }
}

} // namespace quoll::async::detail

#endif // QUOLL_ASYNC_DETAIL_WHEN_ALL_RUNNER_HPP
