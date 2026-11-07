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

#ifndef QUOLL_ASYNC_TASK_HPP
#define QUOLL_ASYNC_TASK_HPP

#include <quoll/async/await_context.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/detail/task_promise.hpp>

#include <coroutine>
#include <type_traits>
#include <utility>

namespace quoll::async {

/**
 * @brief Lazy coroutine producing a `T` (or nothing for `task<>`).
 *
 * A task starts when awaited, runs on its awaiter's await_context, and resumes the awaiter by symmetric transfer
 * when it completes. An exception escaping the task is rethrown at the `co_await`. Inside a task, `co_await` only
 * accepts ContextAwaitable operations, so every operation knows the executor its awaiter resumes on. Use co_spawn to
 * run a task from outside a coroutine.
 *
 * Owns its coroutine frame: move-only, and destroying a task that never ran destroys the frame without running it.
 *
 * Thread safety: none; a task is awaited once, by one coroutine.
 */
template <class T = void>
class [[nodiscard]] task {
  static_assert(std::is_void_v<T> || std::is_object_v<T>, "task<T>: T must be void or an object type");
  static_assert(alignof(std::conditional_t<std::is_void_v<T>, char, T>) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
                "task<T>: T must not be over-aligned, as its coroutine frame is not");

public:
  using promise_type = detail::task_promise<T>;
  using value_type = T;

  task(task const &) = delete;
  task &operator=(task const &) = delete;

  task(task &&other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  task &operator=(task &&other) noexcept {
    if (this != &other) {
      destroy();
      handle_ = std::exchange(other.handle_, {});
    }

    return *this;
  }

  ~task() { destroy(); }

  /** @brief Always false: a task starts when awaited. */
  [[nodiscard]] bool await_ready() const noexcept { return false; }

  /** @brief Starts the task on `context`, which resumes `continuation` when it ends. */
  [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation,
                                                      await_context const &context) noexcept {
    handle_.promise().start(continuation, context);
    return handle_;
  }

  /** @brief What the task returned, or rethrows what it threw. */
  T await_resume() { return handle_.promise().take(); }

private:
  friend class detail::task_promise_base<T>;

  explicit task(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

  void destroy() noexcept {
    if (handle_) {
      handle_.destroy();
    }
  }

  std::coroutine_handle<promise_type> handle_;
};

static_assert(ContextAwaitable<task<>>);
static_assert(ContextAwaitable<task<int>>);

} // namespace quoll::async

#endif // QUOLL_ASYNC_TASK_HPP
