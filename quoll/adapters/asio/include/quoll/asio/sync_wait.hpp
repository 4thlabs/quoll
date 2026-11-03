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

#include <signal.h>

#include <array>
#include <concepts>
#include <csignal>
#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <type_traits>
#include <utility>

namespace quoll::asio {

namespace detail {

// SIGINT and SIGTERM turned into a stop request while sync_wait() runs. Their actions from before are saved, and given
// back by release(): at the first signal, so that the next one has its usual effect (ending a process whose task does
// not stop), and once the task is done.
class stop_signals {
public:
  stop_signals(::asio::io_context &context, std::stop_source &stop) : signals_(context), stop_(&stop) {
    for (std::size_t index = 0; index < numbers.size(); ++index) {
      ::sigaction(numbers[index], nullptr, &saved_[index]);
      signals_.add(numbers[index]);
    }

    signals_.async_wait([this](std::error_code error, int) {
      if (!error) {
        stop_->request_stop();
        release();
      }
    });
  }

  stop_signals(stop_signals const &) = delete;
  stop_signals(stop_signals &&) = delete;
  stop_signals &operator=(stop_signals const &) = delete;
  stop_signals &operator=(stop_signals &&) = delete;

  ~stop_signals() { release(); }

  void release() noexcept {
    if (released_) {
      return;
    }

    std::error_code ignored;

    // Ends the wait, which would keep the loop running; then asio sets the default action as it lets the signals go,
    // and the saved one replaces it.
    signals_.cancel(ignored);
    signals_.clear(ignored);

    for (std::size_t index = 0; index < numbers.size(); ++index) {
      ::sigaction(numbers[index], &saved_[index], nullptr);
    }

    released_ = true;
  }

private:
  static constexpr std::array<int, 2> numbers{SIGINT, SIGTERM};

  ::asio::signal_set signals_;
  std::stop_source *stop_;
  std::array<struct sigaction, 2> saved_{};
  bool released_ = false;
};

// Where sync_wait() keeps the outcome of its task.
template <class T>
struct sync_outcome {
  std::optional<std::conditional_t<std::is_void_v<T>, bool, T>> value;
  std::exception_ptr failure;
};

// Stores the task's outcome, then asks what the task started to stop and lets the signals go: the loop can drain.
template <class T>
struct sync_completion {
  sync_outcome<T> *outcome;
  std::stop_source *stop;
  stop_signals *signals;

  void operator()(std::exception_ptr failure) const {
    outcome->failure = std::move(failure);
    done();
  }

  void operator()() const
    requires std::is_void_v<T>
  {
    outcome->value.emplace(true);
    done();
  }

  template <class U>
    requires(!std::is_void_v<T> && std::same_as<U, T>)
  void operator()(U value) const {
    outcome->value.emplace(std::move(value));
    done();
  }

  void done() const {
    stop->request_stop();
    signals->release();
  }
};

} // namespace detail

/**
 * @brief Runs `work` on an asio I/O loop in the calling thread, and returns its value or rethrows its exception: the
 * entry point of a program, like std::this_thread::sync_wait.
 *
 * The task runs on an executor offering the loop's network, so that io::connect(), io::listen(), and what is built on
 * them (quoll::server::listen, quoll::client::transport, a REST client), work without being handed anything.
 *
 * While the task runs, the first SIGINT or SIGTERM (Ctrl-C) requests stop on it, which ends a server; the signals
 * then get their former actions back, so that the next Ctrl-C ends a process whose task does not stop. Once the task
 * has completed, what it started and left running (connections) is asked to stop too, and sync_wait() returns when
 * the loop has drained. A task waiting for something that never comes keeps sync_wait() waiting, as with std's; if a
 * stop request leaves it waiting with nothing left to run, sync_wait() throws std::logic_error.
 *
 * POSIX signals: native only, like the adapter.
 */
template <class T>
T sync_wait(async::task<T> work) {
  ::asio::io_context context{1};
  std::stop_source stop;
  detail::stop_signals signals{context, stop};
  detail::sync_outcome<T> outcome;

  async::co_spawn(executor<::asio::io_context::executor_type>{context.get_executor()}, std::move(work),
                  detail::sync_completion<T>{&outcome, &stop, &signals}, stop.get_token());
  context.run();

  if (outcome.failure) {
    std::rethrow_exception(outcome.failure);
  }

  if (!outcome.value) {
    throw std::logic_error{"quoll::asio::sync_wait: the stopped task waits for something that cannot come"};
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*outcome.value);
  }
}

} // namespace quoll::asio

#endif // QUOLL_ASIO_SYNC_WAIT_HPP
