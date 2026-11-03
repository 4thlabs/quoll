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

#ifndef QUOLL_ASIO_SYNC_WAIT_HPP
#define QUOLL_ASIO_SYNC_WAIT_HPP

#include <quoll/asio/executor.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/task.hpp>

#include <asio/io_context.hpp>
#include <asio/signal_set.hpp>

#include <csignal>
#include <exception>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <type_traits>
#include <utility>

namespace quoll::asio {

namespace detail {

// Where sync_wait() keeps the outcome of its task.
template <class T>
struct sync_outcome {
  std::optional<std::conditional_t<std::is_void_v<T>, bool, T>> value;
  std::exception_ptr failure;
};

// Stores the task's outcome, and stops waiting for signals: the loop can then drain.
template <class T>
struct sync_completion {
  sync_outcome<T> *outcome;
  ::asio::signal_set *signals;

  void operator()(std::exception_ptr failure) const {
    outcome->failure = std::move(failure);
    stop_waiting();
  }

  void operator()() const
    requires std::is_void_v<T>
  {
    outcome->value.emplace(true);
    stop_waiting();
  }

  template <class U>
    requires(!std::is_void_v<T> && std::same_as<U, T>)
  void operator()(U value) const {
    outcome->value.emplace(std::move(value));
    stop_waiting();
  }

  void stop_waiting() const {
    std::error_code ignored;

    signals->cancel(ignored);
  }
};

} // namespace detail

/**
 * @brief Runs `work` on an asio I/O loop in the calling thread, and returns its value or rethrows its exception: the
 * entry point of a program, like std::this_thread::sync_wait.
 *
 * The task runs on an executor offering the loop's network, so that io::connect(), io::listen(), and what is built on
 * them (quoll::server::listen, quoll::client::transport, a REST client), work without being handed anything. While
 * it waits, the first SIGINT or SIGTERM (Ctrl-C) requests stop on the task, which ends a server; the next one has its
 * default action, ending a process whose task does not stop.
 *
 * Returns once the task has completed and the work it started has drained (connections cancelled by the stop request
 * finish). Throws std::logic_error if the loop runs out of work before the task completes.
 */
template <class T>
T sync_wait(async::task<T> work) {
  ::asio::io_context context{1};
  ::asio::signal_set signals{context, SIGINT, SIGTERM};
  std::stop_source stop;
  detail::sync_outcome<T> outcome;

  // The first signal asks the task to stop, and gives the signals their default action back: if the task does not
  // stop, a second Ctrl-C ends the process.
  signals.async_wait([&stop, &signals](std::error_code error, int) {
    if (!error) {
      std::error_code ignored;

      stop.request_stop();
      signals.clear(ignored);
    }
  });
  async::co_spawn(executor<::asio::io_context::executor_type>{context.get_executor()}, std::move(work),
                  detail::sync_completion<T>{&outcome, &signals}, stop.get_token());
  context.run();

  if (outcome.failure) {
    std::rethrow_exception(outcome.failure);
  }

  if (!outcome.value) {
    throw std::logic_error{"quoll::asio::sync_wait: the task waits for something that cannot come"};
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*outcome.value);
  }
}

} // namespace quoll::asio

#endif // QUOLL_ASIO_SYNC_WAIT_HPP
