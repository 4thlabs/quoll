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

#ifndef QUOLL_SERVER_SERVE_OPTIONS_HPP
#define QUOLL_SERVER_SERVE_OPTIONS_HPP

#include <quoll/async/task.hpp>
#include <quoll/server/connection_options.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <system_error>

namespace quoll::http {

/** @brief How a server accepts its connections and serves each one. */
struct serve_options {
  /** @brief The options of each connection (see server::serve_connection). */
  connection_options connection{};

  /**
   * @brief How many connections the server serves at once, those of server::serve_connection() included, which it never
   * refuses. Past it, a new connection is accepted and closed at once, without a word, which keeps the accept queue
   * moving. Zero: no limit but what the system allows.
   */
  std::size_t max_connections = 10'000;

  /**
   * @brief How long the requests under way may take to finish once the server is asked to stop. Past it, they are
   * stopped. Zero stops them at once, as does an executor without a timer, which cannot bound the wait.
   */
  std::chrono::nanoseconds shutdown_timeout = std::chrono::seconds{30};

  /**
   * @brief Called with the exception that ended a connection (a stream exception, `std::bad_alloc`...), on the
   * executor; must not throw. Handler failures are not among them: they answer 500 (see connection_options). Empty:
   * it is written to stderr, and ends the connection.
   */
  std::function<void(std::exception_ptr)> on_connection_error{};

  /**
   * @brief Decides what an accept failure does, other than `operation_canceled`: true accepts again, false rethrows
   * the failure. A task, so that it may wait first (a back-off on `too_many_files_open`, with async::sleep_for).
   * Empty: a failure of one connection only (`connection_aborted`, `connection_reset`) is skipped, running out of
   * something (file descriptors, memory) is retried after 100 ms if the executor has a timer, for as long as it lasts,
   * written to stderr once until a connection is accepted again, and any other is rethrown.
   *
   * This and on_connection_error are the server's, for all of its listen() and serve(): on several threads, they may be
   * called concurrently.
   */
  std::function<async::task<bool>(std::error_code)> on_accept_error{};
};

} // namespace quoll::http

#endif // QUOLL_SERVER_SERVE_OPTIONS_HPP
