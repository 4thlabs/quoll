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

#ifndef QUOLL_ASYNC_CO_SPAWN_HPP
#define QUOLL_ASYNC_CO_SPAWN_HPP

#include <quoll/async/any_executor.hpp>
#include <quoll/async/await_context.hpp>
#include <quoll/async/completion_handler.hpp>
#include <quoll/async/detail/context_adapter.hpp>
#include <quoll/async/detail/detached.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/executor_ref.hpp>
#include <quoll/async/task.hpp>

#include <cassert>
#include <concepts>
#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace quoll::async {

namespace detail {

// The frame owns the executor and the await_context of the whole chain.
template <Executor E, class T, CompletionHandler<T> Handler>
detached spawn_root(E executor, task<T> work, Handler handler, std::stop_token stop_token) {
  await_context const context{executor_ref{executor}, std::move(stop_token)};
  std::exception_ptr error;

  if constexpr (std::is_void_v<T>) {
    try {
      co_await context_adapter<task<T>>{std::move(work), context};
    } catch (...) {
      error = std::current_exception();
    }

    if (error) {
      std::invoke(handler, std::move(error));
    } else {
      std::invoke(handler);
    }
  } else {
    std::optional<T> value;

    try {
      value.emplace(co_await context_adapter<task<T>>{std::move(work), context});
    } catch (...) {
      error = std::current_exception();
    }

    if (error) {
      std::invoke(handler, std::move(error));
    } else {
      std::invoke(handler, std::move(*value));
    }
  }
}

} // namespace detail

/**
 * @brief Runs `work` on `executor` and hands its outcome to `handler`.
 *
 * The task starts from the executor (never inline), and `handler` is invoked on the task's last executor with its
 * value, or with the exception it threw. The executor is copied and lives as long as the task. If `handler` throws,
 * std::terminate is called.
 *
 * `stop_token` becomes the stop token of the task's await_context (see current_stop_token()).
 *
 * An executor_ref is copied through any_executor, into what it references: the task may outlive it.
 */
template <Executor E, class T, CompletionHandler<T> Handler>
void co_spawn(E const &executor, task<T> work, Handler handler, std::stop_token stop_token = {}) {
  static_assert(
      !std::convertible_to<T, std::exception_ptr>,
      "quoll::async::co_spawn: a task of std::exception_ptr cannot be told apart from a failure by its handler");

  auto root = [&] {
    if constexpr (std::same_as<E, executor_ref>) {
      return detail::spawn_root(any_executor{executor}, std::move(work), std::move(handler), std::move(stop_token));
    } else {
      return detail::spawn_root(executor, std::move(work), std::move(handler), std::move(stop_token));
    }
  }();

  // If post throws, nothing has started: `root` destroys the frame, and the exception reaches the caller.
  executor.post(root.start_item());
  root.release();
}

/** @brief The type of async::this_executor. */
struct this_executor_t {
  explicit this_executor_t() = default;
};

/**
 * @brief Tag of co_spawn() on the executor of the coroutine that awaits it.
 *
 * @code
 * co_await quoll::async::co_spawn(quoll::async::this_executor, refresh(), quoll::async::detached);
 * @endcode
 */
inline constexpr this_executor_t this_executor{};

namespace detail {

// Spawns once it knows the awaiter's executor, and never suspends.
template <class T, class Handler>
class [[nodiscard]] spawn_on_this_executor {
public:
  spawn_on_this_executor(task<T> work, Handler handler, std::stop_token stop_token)
      : work_(std::move(work)), handler_(std::move(handler)), stop_token_(std::move(stop_token)) {}

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  // If post throws, the exception reaches the awaiter, as with the other co_spawn().
  [[nodiscard]] bool await_suspend(std::coroutine_handle<>, await_context const &context) {
    assert(!spawned_ && "co_spawn(this_executor) awaited twice");
    spawned_ = true;
    ::quoll::async::co_spawn(context.executor, std::move(work_), std::move(handler_), std::move(stop_token_));
    return false;
  }

  void await_resume() const noexcept {}

private:
  task<T> work_;
  Handler handler_;
  std::stop_token stop_token_;
  bool spawned_ = false;
};

} // namespace detail

/**
 * @brief co_spawn() on the executor of the coroutine that awaits the result, copied so that the task may outlive it.
 *
 * Awaiting it does not suspend, and does not wait for the task, which starts from the executor and never inline: on one
 * thread, once the coroutine lets it; on a thread pool, possibly at once, beside the coroutine. Nothing happens without
 * the `co_await`: the task is destroyed unstarted, and its handler is not called. An awaitable is awaited once. The
 * task does not take the stop token of the coroutine: pass `co_await current_stop_token()` for it to stop along with
 * it.
 */
template <class T, CompletionHandler<T> Handler>
[[nodiscard]] detail::spawn_on_this_executor<T, Handler> co_spawn(this_executor_t, task<T> work, Handler handler,
                                                                  std::stop_token stop_token = {}) {
  return {std::move(work), std::move(handler), std::move(stop_token)};
}

/**
 * @brief co_spawn(this_executor, ...): the executor of the coroutine is what it runs on by default.
 *
 * @code
 * co_await quoll::async::co_spawn(refresh(), quoll::async::detached);
 * @endcode
 */
template <class T, CompletionHandler<T> Handler>
[[nodiscard]] detail::spawn_on_this_executor<T, Handler> co_spawn(task<T> work, Handler handler,
                                                                  std::stop_token stop_token = {}) {
  return {std::move(work), std::move(handler), std::move(stop_token)};
}

} // namespace quoll::async

#endif // QUOLL_ASYNC_CO_SPAWN_HPP
