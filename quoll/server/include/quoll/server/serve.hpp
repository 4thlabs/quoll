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
#include <quoll/async/executor.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/router.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/listener.hpp>
#include <quoll/server/connection_options.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <system_error>
#include <utility>

namespace quoll::server {

/** @brief How serve() accepts its connections and serves each one. */
struct serve_options {
  /** @brief The options of each connection (see serve_connection). */
  connection_options connection{};

  /**
   * @brief How many connections are served at once. Past it, a new connection is accepted and closed at once, without a
   * word, which keeps the accept queue moving. Zero: no limit, up to what the system allows.
   */
  std::size_t max_connections = 10'000;

  /**
   * @brief How long the requests under way may take to finish once serve() is asked to stop. Past it, they are stopped.
   * Zero stops them at once, as does an executor without a timer, which cannot bound the wait.
   */
  std::chrono::nanoseconds shutdown_timeout = std::chrono::seconds{30};

  /**
   * @brief Called with the exception that ended a connection (a stream exception, `std::bad_alloc`...), on the
   * executor; must not throw. Handler failures are not among them: they answer 500 (see connection_options). Empty:
   * the failure only ends the connection.
   */
  std::function<void(std::exception_ptr)> on_connection_error{};

  /**
   * @brief Decides what an accept failure does, other than `operation_canceled`: true accepts again, false rethrows
   * the failure. A task, so that it may wait first (a back-off on `too_many_files_open`, with async::sleep_for).
   * Empty: a failure of one connection only (`connection_aborted`, `connection_reset`) is skipped, running out of
   * something (file descriptors, memory) is retried after 100 ms if the executor has a timer, for as long as it lasts
   * and without a word, and any other is rethrown. Set this to be told.
   */
  std::function<async::task<bool>(std::error_code)> on_accept_error{};
};

namespace detail {

// A listener, whichever it is. Non-owning: the listener outlives the task.
struct listener_ref {
  void *listener;
  async::task<io::any_stream> (*accept)(void *listener);

  template <io::Listener Listener>
  explicit listener_ref(Listener &target) noexcept
      : listener(&target),
        accept([](void *erased) -> async::task<io::any_stream> { return static_cast<Listener *>(erased)->accept(); }) {}
};

[[nodiscard]] async::task<> serve_accepting(async::any_executor executor, listener_ref listener,
                                            http::router const &router, serve_options options);
[[nodiscard]] async::task<> serve_accepting_here(listener_ref listener, http::router const &router,
                                                 serve_options options);

} // namespace detail

/**
 * @brief Accepts the connections of `listener` and serves each one with `router`, concurrently, on `executor`.
 *
 * Each connection runs serve_connection() as a task of its own, spawned on `executor` (a thread pool or a strand,
 * for instance, while the accept loop stays on its I/O loop). An executor without a timer, as a thread pool has none,
 * leaves the timeouts of `options.connection` without effect, and a slow peer can hold its connection. A stop request
 * is graceful: it ends the accept loop, closes the idle connections, and lets the requests under way finish, within
 * `options.shutdown_timeout`, past which they are stopped.
 *
 * Accepting stops with `std::errc::operation_canceled`: a stop request, or the application closing its listener.
 * Another accept failure goes to `options.on_accept_error`. By default it is skipped when it concerns one connection
 * only, retried after 100 ms when the system ran out of something (file descriptors, memory) and it has a timer to wait
 * with, and rethrown otherwise; a rethrow ends the accept loop, not the connections already served.
 *
 * However the accept loop ends (a stop request, a closed listener, a failure, an exception from
 * `options.on_accept_error`), the connections it spawned end as above, and the task completes once they have ended,
 * then rethrows the failure if there was one: `listener` and `router` need to outlive the task only.
 */
template <async::Executor Executor, io::Listener Listener>
async::task<> serve(Executor executor, Listener &listener QUOLL_LIFETIMEBOUND,
                    http::router const &router QUOLL_LIFETIMEBOUND, serve_options options = {}) {
  return detail::serve_accepting(async::any_executor{std::move(executor)}, detail::listener_ref{listener}, router,
                                 std::move(options));
}

/**
 * @brief serve() on the executor the task runs on: the connections run where the accept loop does.
 *
 * @code
 * co_await quoll::server::serve(listener, router);
 * @endcode
 */
template <io::Listener Listener>
async::task<> serve(Listener &listener QUOLL_LIFETIMEBOUND, http::router const &router QUOLL_LIFETIMEBOUND,
                    serve_options options = {}) {
  return detail::serve_accepting_here(detail::listener_ref{listener}, router, std::move(options));
}

} // namespace quoll::server

#endif // QUOLL_SERVER_SERVE_HPP
