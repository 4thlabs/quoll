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

#ifndef QUOLL_SERVER_SERVE_HPP
#define QUOLL_SERVER_SERVE_HPP

#include <quoll/async/any_executor.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/current_executor.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/router.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/listener.hpp>
#include <quoll/server/connection_options.hpp>
#include <quoll/server/serve_connection.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

namespace quoll::server {

/** @brief How serve() accepts its connections and serves each one. */
struct serve_options {
  /** @brief The options of each connection (see serve_connection). */
  connection_options connection;

  /**
   * @brief Called with the exception that ended a connection (a stream exception, `std::bad_alloc`...), on the
   * executor; must not throw. Handler failures are not among them: they answer 500 (see connection_options). Empty:
   * the failure only ends the connection.
   */
  std::function<void(std::exception_ptr)> on_connection_error;

  /**
   * @brief Decides what an accept failure does, other than `operation_canceled`: true accepts again, false rethrows
   * the failure. A task, so that it may wait first (a back-off on `too_many_files_open`, with the application's
   * timer). Empty: a failure of one connection only (`connection_aborted`, `connection_reset`) is skipped, any other
   * is rethrown.
   */
  std::function<async::task<bool>(std::error_code)> on_accept_error;
};

namespace detail {

[[nodiscard]] inline bool one_connection_failure(std::error_code error) noexcept {
  return error == std::errc::connection_aborted || error == std::errc::connection_reset;
}

// What the connections of a serve() share: its options, kept alive by each connection, which may outlive serve().
struct shared_options {
  serve_options options;
};

// The outcome of a connection spawned by serve(): it ends by itself, and a failure only ends it, reported to the
// application's callback if it gave one.
struct connection_outcome {
  std::shared_ptr<shared_options const> shared;

  void operator()() const noexcept {}

  void operator()(std::exception_ptr failure) const noexcept {
    if (shared->options.on_connection_error) {
      shared->options.on_connection_error(std::move(failure));
    }
  }
};

template <class Stream>
async::task<> serve_shared(Stream stream, http::router const &router, std::shared_ptr<shared_options const> shared) {
  co_await serve_connection(std::move(stream), router, shared->options.connection);
}

} // namespace detail

/**
 * @brief Accepts the connections of `listener` and serves each one with `router`, concurrently, on `executor`.
 *
 * Each connection runs serve_connection() as a task of its own, spawned on `executor` with the stop token of the
 * awaiter: a thread pool or a strand, for instance, while the accept loop stays on its I/O loop. A stop request is
 * immediate, not graceful: it ends the accept loop and cancels whatever the connections have pending, a read, a
 * handler's operations, a write.
 *
 * The task completes when accepting stops with `std::errc::operation_canceled`: a stop request, or the application
 * closing its listener. Another accept failure goes to `options.on_accept_error`, or by default is skipped when it
 * concerns one connection only and rethrown otherwise; a rethrow ends the accept loop, not the connections already
 * served.
 *
 * The spawned connections may outlive the task: `listener` and `router` outlive them, for instance until `executor`
 * has drained (as a runtime does).
 */
template <async::Executor Executor, io::Listener Listener>
async::task<> serve(Executor executor, Listener &listener, http::router const &router, serve_options options = {}) {
  auto const stop_token = co_await async::current_stop_token();
  auto const shared = std::make_shared<detail::shared_options const>(std::move(options));
  auto const &accept_error = shared->options.on_accept_error;

  while (!stop_token.stop_requested()) {
    std::optional<io::any_stream> stream;
    std::exception_ptr failure;
    std::error_code error;

    try {
      stream.emplace(co_await listener.accept());
    } catch (std::system_error const &thrown) {
      failure = std::current_exception();
      error = thrown.code();
    }

    if (!stream) {
      if (error == std::errc::operation_canceled) {
        co_return;
      }

      auto const again = accept_error ? co_await accept_error(error) : detail::one_connection_failure(error);

      if (!again) {
        std::rethrow_exception(failure);
      }

      continue;
    }

    async::co_spawn(executor, detail::serve_shared(std::move(*stream), router, shared),
                    detail::connection_outcome{shared}, stop_token);
  }
}

/**
 * @brief serve() on the executor the task runs on: the connections run where the accept loop does.
 *
 * @code
 * co_await quoll::server::serve(listener, router);
 * @endcode
 */
template <io::Listener Listener>
async::task<> serve(Listener &listener, http::router const &router, serve_options options = {}) {
  // A copy: the connections may outlive the executor of the awaiting chain.
  async::any_executor executor{co_await async::current_executor()};

  co_await serve(std::move(executor), listener, router, std::move(options));
}

} // namespace quoll::server

#endif // QUOLL_SERVER_SERVE_HPP
