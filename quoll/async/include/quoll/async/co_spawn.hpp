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

#include <quoll/async/await_context.hpp>
#include <quoll/async/completion_handler.hpp>
#include <quoll/async/detail/context_adapter.hpp>
#include <quoll/async/detail/detached.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/executor_ref.hpp>
#include <quoll/async/task.hpp>

#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace quoll::async {

namespace detail {

// The frame owns the executor and the await_context of the whole chain.
template <Executor E, class T, CompletionHandler<T> Handler>
detached spawn_root(E executor, task<T> work, Handler handler) {
  await_context const context{executor_ref{executor}};
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
 */
template <Executor E, class T, CompletionHandler<T> Handler>
void co_spawn(E const &executor, task<T> work, Handler handler) {
  auto root = detail::spawn_root(executor, std::move(work), std::move(handler));
  executor.post(root.handle());
  root.release();
}

} // namespace quoll::async

#endif // QUOLL_ASYNC_CO_SPAWN_HPP
