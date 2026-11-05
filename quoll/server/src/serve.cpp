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
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/detail/task_group.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/server/serve_connection.hpp>

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>

namespace quoll::server::detail {

namespace {

[[nodiscard]] bool one_connection_failure(std::error_code error) noexcept {
  return error == std::errc::connection_aborted || error == std::errc::connection_reset;
}

[[nodiscard]] bool out_of_resources(std::error_code error) noexcept {
  return error == std::errc::too_many_files_open || error == std::errc::too_many_files_open_in_system ||
         error == std::errc::no_buffer_space || error == std::errc::not_enough_memory;
}

// What an accept failure does by default. Running out of something passes, as connections end: not a reason to end a
// server, nor to try again at once. Without a timer, connections get a moment to end before it gives up.
async::task<bool> accept_again(std::error_code error) {
  if (one_connection_failure(error)) {
    co_return true;
  }

  if (!out_of_resources(error)) {
    co_return false;
  }

  auto const timer = (co_await async::current_executor()).query<async::timer_ref>();

  if (!timer) {
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    co_return false;
  }

  co_await timer->wait_for(std::chrono::milliseconds{100});
  co_return true;
}

// What the connections of a serve() share: its options, and the count serve() waits on before it completes.
struct shared_options {
  serve_options options;
  async::detail::task_group connections;
  // Ends them gently, where the stop token of their tasks cancels what they have pending.
  std::stop_source drain;
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

async::task<> join_then_cancel(async::detail::task_group &connections, std::stop_source &deadline) {
  co_await connections.join();
  deadline.request_stop();
}

async::task<> stop_past_deadline(async::timer_ref timer, std::chrono::nanoseconds timeout, std::stop_token cancelled,
                                 std::stop_source &connections_stop) {
  try {
    if (co_await async::detail::stop_scope<bool>{timer.wait_for(timeout), std::move(cancelled)}) {
      connections_stop.request_stop();
    }
  } catch (...) {
    // Without a deadline left to wait for, nothing else would ever stop them.
    connections_stop.request_stop();
    throw;
  }
}

// The connections have been asked to drain: waits for them, up to the timeout, past which they are stopped. Without a
// timeout, or a timer to bound it, it has nothing to do.
async::task<> wind_down(shared_options &shared, std::stop_source &connections_stop) {
  auto const timeout = shared.options.shutdown_timeout;
  auto const timer = (co_await async::current_executor()).query<async::timer_ref>();

  if (timeout <= std::chrono::nanoseconds::zero() || !timer) {
    co_return;
  }

  std::stop_source deadline;

  co_await async::when_all(join_then_cancel(shared.connections, deadline),
                           stop_past_deadline(*timer, timeout, deadline.get_token(), connections_stop));
}

} // namespace

async::task<> serve_accepting(async::any_executor executor, listener_ref listener, http::router const &router,
                              serve_options options) {
  auto const stop_token = co_await async::current_stop_token();
  auto const shared = std::make_shared<shared_options>(std::move(options));

  shared->options.connection.drain = shared->drain.get_token();

  auto const &accept_error = shared->options.on_accept_error;
  std::stop_source connections_stop;
  auto const drain = [&shared] { shared->drain.request_stop(); };
  std::stop_callback const drain_on_stop{stop_token, drain};
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
        again = accept_error ? co_await accept_error(error) : co_await accept_again(error);
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

    auto const max_connections = shared->options.max_connections;

    if (max_connections != 0 && shared->connections.live() >= max_connections) {
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

  shared->drain.request_stop();

  try {
    co_await wind_down(*shared, connections_stop);
  } catch (...) {
    if (!ending) {
      ending = std::current_exception();
    }
  }

  // Whatever happened above: what the connections use outlives them. Allocates nothing, and is done if they have ended.
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
