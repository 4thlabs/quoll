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

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/executor.hpp>
#include <quoll/async/task.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/router.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/listener.hpp>
#include <quoll/server/detail/connection.hpp>
#include <quoll/server/serve_connection.hpp>

#include <optional>
#include <system_error>
#include <utility>

namespace quoll::server {

/**
 * @brief Accepts the connections of `listener` and serves each one with `router`, concurrently, on `executor`.
 *
 * Each connection runs serve_connection() as a task of its own, spawned on `executor` with the stop token of the
 * awaiter: a stop request ends the accept loop and cancels the reads of the connections. The task completes when
 * accepting stops (a stop request); another failure to accept is rethrown. A connection's own failure only ends it.
 *
 * The spawned connections may outlive the task: `listener` and `router` outlive them, for instance until `executor`
 * has drained.
 */
template <async::Executor Executor, io::Listener Listener>
async::task<> serve(Executor executor, Listener &listener, http::router const &router, http::parse_limits limits = {}) {
  auto const stop_token = co_await async::current_stop_token();

  while (!stop_token.stop_requested()) {
    std::optional<io::any_stream> stream;

    try {
      stream.emplace(co_await listener.accept());
    } catch (std::system_error const &error) {
      if (error.code() == std::errc::operation_canceled) {
        co_return;
      }

      throw;
    }

    async::co_spawn(executor, serve_connection(std::move(*stream), router, limits), detail::ignore_outcome{},
                    stop_token);
  }
}

} // namespace quoll::server

#endif // QUOLL_SERVER_SERVE_HPP
