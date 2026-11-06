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

#include <quoll/server/server.hpp>

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/current_executor.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/detail/task_group.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/listen_tls.hpp>
#include <quoll/server/detail/serve_connection.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <utility>

namespace quoll::http::detail {

// What a server and its copies share, with its tasks.
struct server_state {
  server_state(http::router routes, serve_options serving_options)
      : router(std::move(routes)), options(std::move(serving_options)) {}

  http::router router;
  serve_options options;
  std::atomic<std::size_t> connections{0};
  // Once set, `router` is read by connections, and closed.
  std::atomic<bool> serving{false};
};

namespace {

[[nodiscard]] bool one_connection_failure(std::error_code error) noexcept {
  return error == std::errc::connection_aborted || error == std::errc::connection_reset;
}

[[nodiscard]] bool out_of_resources(std::error_code error) noexcept {
  return error == std::errc::too_many_files_open || error == std::errc::too_many_files_open_in_system ||
         error == std::errc::no_buffer_space || error == std::errc::not_enough_memory;
}

// What an accept failure does by default. Running out of something passes, as connections end: not a reason to end a
// server, nor to try again at once. Without a timer, it cannot wait.
async::task<bool> accept_again(std::error_code error) {
  if (one_connection_failure(error)) {
    co_return true;
  }

  if (!out_of_resources(error)) {
    co_return false;
  }

  auto const timer = (co_await async::current_executor()).query<async::timer_ref>();

  if (!timer) {
    co_return false;
  }

  co_await timer->wait_for(std::chrono::milliseconds{100});
  co_return true;
}

// What the connections of a serve() share. It holds none of what the application gave, which `state` and the frame of
// serve() hold, past each connection's last use of it: they end on the accept loop rather than on a connection's
// thread, as a connection's frame ends after serve() resumes, where what a handler or a callback holds has no business
// ending.
struct shared_options {
  server_state *state;
  async::detail::task_group connections;
  // Ends them gently, where the stop token of their tasks cancels what they have pending.
  std::stop_source drain;
};

// The outcome of a connection spawned by serve(): it ends by itself, and a failure only ends it, reported to the
// application's callback if it gave one.
struct connection_outcome {
  std::shared_ptr<shared_options> shared;

  void ended() const noexcept {
    shared->state->connections.fetch_sub(1);
    shared->connections.ended();
  }

  void operator()() const noexcept { ended(); }

  void operator()(std::exception_ptr failure) const noexcept {
    if (shared->state->options.on_connection_error) {
      shared->state->options.on_connection_error(std::move(failure));
    }

    ended();
  }
};

// Takes a place among the server's connections, if it has one left. Two accept loops may ask at once, and connections()
// reads it: it never goes past the limit, not even for an instant.
[[nodiscard]] bool try_open(server_state &state) noexcept {
  auto const max_connections = state.options.max_connections;
  auto open = state.connections.load();

  do {
    if (max_connections != 0 && open >= max_connections) {
      return false;
    }
  } while (!state.connections.compare_exchange_weak(open, open + 1));

  return true;
}

async::task<> serve_shared(io::any_stream stream, http::router const &router, connection_options const &options,
                           std::shared_ptr<shared_options> shared) {
  co_await serve_connection(std::move(stream), router, options);
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
  auto const timeout = shared.state->options.shutdown_timeout;
  auto const timer = (co_await async::current_executor()).query<async::timer_ref>();

  if (timeout <= std::chrono::nanoseconds::zero() || !timer) {
    co_return;
  }

  std::stop_source deadline;

  co_await async::when_all(join_then_cancel(shared.connections, deadline),
                           stop_past_deadline(*timer, timeout, deadline.get_token(), connections_stop));
}

async::task<> serve_accepting(async::any_executor executor, io::any_listener listener,
                              std::shared_ptr<server_state> state) {
  state->serving.store(true);

  auto const stop_token = co_await async::current_stop_token();
  auto const shared = std::make_shared<shared_options>(state.get());
  connection_options connection = state->options.connection;

  connection.drain = shared->drain.get_token();

  auto const &accept_error = state->options.on_accept_error;
  std::stop_source connections_stop;
  auto const drain = [&shared] { shared->drain.request_stop(); };
  std::stop_callback const drain_on_stop{stop_token, drain};
  std::exception_ptr ending;

  while (!stop_token.stop_requested()) {
    std::optional<io::any_stream> stream;
    std::exception_ptr failure;
    std::error_code error;

    try {
      stream.emplace(co_await listener.accept());
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

    if (!try_open(*state)) {
      continue;
    }

    shared->connections.started();

    try {
      async::co_spawn(executor, serve_shared(std::move(*stream), state->router, connection, shared),
                      connection_outcome{shared}, connections_stop.get_token());
    } catch (...) {
      // Not spawned: stop the others, wait for them, then fail.
      state->connections.fetch_sub(1);
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

async::task<> serve_accepting_here(io::any_listener listener, std::shared_ptr<server_state> state) {
  // A copy, held by each connection: their frames end after serve() resumes.
  async::any_executor executor{co_await async::current_executor()};

  co_await serve_accepting(std::move(executor), std::move(listener), std::move(state));
}

[[nodiscard]] async::task<> listen_and_serve(std::shared_ptr<server_state> state, std::uint16_t port,
                                             listen_options where) {
  auto listener = where.tls.enabled() ? co_await io::listen_tls(port, std::move(where.tls), std::move(where.address))
                                      : co_await io::listen(port, std::move(where.address));

  co_await serve_accepting_here(std::move(listener), std::move(state));
}

// One more connection for connections(), for as long as it lives.
class counted_connection {
public:
  explicit counted_connection(server_state &state) noexcept : state_(&state) { state_->connections.fetch_add(1); }

  counted_connection(counted_connection const &) = delete;
  counted_connection &operator=(counted_connection const &) = delete;
  counted_connection(counted_connection &&) = delete;
  counted_connection &operator=(counted_connection &&) = delete;

  ~counted_connection() { state_->connections.fetch_sub(1); }

private:
  server_state *state_;
};

[[nodiscard]] async::task<> serve_one(io::any_stream stream, std::shared_ptr<server_state> state) {
  state->serving.store(true);

  counted_connection const counted{*state};

  co_await serve_connection(std::move(stream), state->router, state->options.connection);
}

} // namespace

} // namespace quoll::http::detail

namespace quoll::http {

server::server(serve_options options) : server(http::router{}, std::move(options)) {}

server::server(http::router router, serve_options options)
    : state_(std::make_shared<detail::server_state>(std::move(router), std::move(options))) {}

http::router &server::open_routes() {
  if (state_->serving.load()) {
    throw std::logic_error{"quoll::http::server: routes cannot be added once it serves"};
  }

  return state_->router;
}

http::router const &server::routes() const noexcept { return state_->router; }

std::size_t server::connections() const noexcept { return state_->connections.load(); }

async::task<> server::listen(std::uint16_t port, listen_options where) const {
  return detail::listen_and_serve(state_, port, std::move(where));
}

async::task<> server::serve(io::any_listener listener) const {
  return detail::serve_accepting_here(std::move(listener), state_);
}

async::task<> server::serve_on(async::any_executor executor, io::any_listener listener) const {
  return detail::serve_accepting(std::move(executor), std::move(listener), state_);
}

async::task<> server::serve_connection(io::any_stream stream) const {
  return detail::serve_one(std::move(stream), state_);
}

} // namespace quoll::http
