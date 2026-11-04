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

#include <quoll/server/serve.hpp>

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/current_executor.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/task_group.hpp>
#include <quoll/server/serve_connection.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <system_error>
#include <utility>

namespace quoll::server::detail {

namespace {

[[nodiscard]] bool one_connection_failure(std::error_code error) noexcept {
  return error == std::errc::connection_aborted || error == std::errc::connection_reset;
}

// What the connections of a serve() share: its options, and the count serve() waits on before it completes.
struct shared_options {
  serve_options options;
  async::detail::task_group connections;
};

// The outcome of a connection spawned by serve(): it ends by itself, and a failure only ends it, reported to the
// application's callback if it gave one.
struct connection_outcome {
  std::shared_ptr<shared_options> shared;

  void operator()() const noexcept { shared->connections.ended(); }

  void operator()(std::exception_ptr failure) const noexcept {
    if (shared->options.on_connection_error) {
      shared->options.on_connection_error(std::move(failure));
    }

    shared->connections.ended();
  }
};

async::task<> serve_shared(io::any_stream stream, http::router const &router, std::shared_ptr<shared_options> shared) {
  co_await serve_connection(std::move(stream), router, shared->options.connection);
}

} // namespace

async::task<> serve_accepting(async::any_executor executor, listener_ref listener, http::router const &router,
                              serve_options options) {
  auto const stop_token = co_await async::current_stop_token();
  auto const shared = std::make_shared<shared_options>(std::move(options));
  auto const &accept_error = shared->options.on_accept_error;
  // The connections stop with serve(): on its stop request, and when its accept loop ends for another reason.
  std::stop_source connections_stop;
  std::stop_callback const forward_stop{stop_token, [&connections_stop] { connections_stop.request_stop(); }};
  std::exception_ptr ending;

  while (!stop_token.stop_requested()) {
    std::optional<io::any_stream> stream;
    std::exception_ptr failure;
    std::error_code error;

    try {
      stream.emplace(co_await listener.accept(listener.listener));
    } catch (std::system_error const &thrown) {
      failure = std::current_exception();
      error = thrown.code();
    } catch (...) {
      ending = std::current_exception();
      break;
    }

    if (!stream) {
      if (error == std::errc::operation_canceled) {
        break;
      }

      auto again = false;

      try {
        again = accept_error ? co_await accept_error(error) : one_connection_failure(error);
      } catch (...) {
        ending = std::current_exception();
        break;
      }

      if (!again) {
        ending = failure;
        break;
      }

      continue;
    }

    shared->connections.started();

    try {
      async::co_spawn(executor, serve_shared(std::move(*stream), router, shared), connection_outcome{shared},
                      connections_stop.get_token());
    } catch (...) {
      // Not spawned: stop the others, wait for them, then fail.
      shared->connections.ended();
      ending = std::current_exception();
      break;
    }
  }

  connections_stop.request_stop();
  co_await shared->connections.join();

  if (ending) {
    std::rethrow_exception(ending);
  }
}

async::task<> serve_accepting_here(listener_ref listener, http::router const &router, serve_options options) {
  // A copy, held by each connection: their frames end after serve() resumes.
  async::any_executor executor{co_await async::current_executor()};

  co_await serve_accepting(std::move(executor), listener, router, std::move(options));
}

} // namespace quoll::server::detail
