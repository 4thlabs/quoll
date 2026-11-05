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

#include <quoll/asio.hpp>
#include <quoll/asio/detail/cancellation.hpp>
#include <quoll/asio/detail/operation.hpp>
#include <quoll/async/any_executor.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/current_executor.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detached.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/client/transport.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/write.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connect.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/listen.hpp>
#include <quoll/io/write.hpp>
#include <quoll/rest.hpp>
#include <quoll/server/listen.hpp>
#include <quoll/server/serve.hpp>
#include <quoll/test/check.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/run_task.hpp>
#include <quoll/threads/thread_pool.hpp>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>

#include <asio/post.hpp>

#include <catch2/catch_test_macros.hpp>

#include <signal.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

// The contract of the REST calls over TCP. Not in the unnamed namespace: Glaze reflects types with external linkage.
namespace asio_test {

struct account {
  std::int64_t id = 0;
  std::string owner;
};

struct new_account {
  std::string owner;
};

struct not_found {
  std::string detail;
};

namespace accounts {
using namespace quoll::rest;

using get = endpoint<"accounts.get", quoll::http::verb::get, "/{id:int64}", returns<account>,
                     returns<not_found, quoll::http::status::not_found>>;
using open = endpoint<"accounts.open", quoll::http::verb::post, "/", body<new_account>,
                      returns<account, quoll::http::status::created>>;

using api = quoll::rest::api<"/api/accounts", get, open>;
} // namespace accounts

struct accounts_service {
  quoll::async::task<quoll::rest::reply<accounts::get>> operator()(accounts::get, std::int64_t id) const {
    if (id == 42) {
      co_return account{.id = 42, .owner = "Ada"};
    }

    co_return not_found{"no account " + std::to_string(id)};
  }

  account operator()(accounts::open, new_account opened) const { return {.id = 7, .owner = std::move(opened.owner)}; }
};

} // namespace asio_test

namespace {

namespace http = quoll::http;
namespace io = quoll::io;

using quoll::async::task;
using quoll::test::check;
using quoll::test::overloaded;
using tcp = ::asio::ip::tcp;

// Spawns `work` on `context` and stores its outcome in `result` / `failure`.
template <class T>
void spawn(::asio::io_context &context, task<T> work, std::optional<T> &result, std::exception_ptr &failure,
           std::stop_token stop_token = {}) {
  co_spawn(quoll::asio::executor{context.get_executor()}, std::move(work),
           overloaded{[&](T value) { result = std::move(value); },
                      [&](std::exception_ptr error) { failure = std::move(error); }},
           std::move(stop_token));
}

// Runs `context` until it is out of work, then rethrows a failure, so that Catch2 reports it.
void run(::asio::io_context &context, std::exception_ptr const &failure) {
  context.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
}

// A socket listening on a free port of the loopback interface.
tcp::acceptor listen(::asio::io_context &context) {
  return tcp::acceptor{context, tcp::endpoint{::asio::ip::address_v4::loopback(), 0}};
}

[[nodiscard]] std::uint16_t port_of(tcp::acceptor const &acceptor) { return acceptor.local_endpoint().port(); }

task<int> count_yields(int yields) {
  for (int index = 0; index < yields; ++index) {
    co_await quoll::async::yield();
  }

  co_return yields;
}

task<http::response> get(quoll::asio::connector connector, std::uint16_t port) {
  auto stream = co_await connector.connect("127.0.0.1", port);
  http::request request;

  request.method = http::verb::get;
  request.target = "/accounts/42";
  request.fields.add("Host", "localhost");
  check(co_await http::write(stream, request));

  std::string buffer;
  http::response_parser parser;

  check(co_await http::read(stream, buffer, parser));
  co_return parser.release();
}

// A GET, then a stop request: the server's accept loop ends once the response is in.
task<http::response> get_then_stop(quoll::asio::connector connector, std::uint16_t port, std::stop_source &source) {
  auto response = co_await get(std::move(connector), port);

  source.request_stop();
  co_return response;
}

task<http::request> serve_one(quoll::asio::tcp_stream stream) {
  std::string buffer;
  http::request_parser parser;

  check(co_await http::read(stream, buffer, parser));

  auto request = parser.release();
  http::response response;

  response.status = http::status::ok;
  response.body = "hello over TCP";
  check(co_await http::write(stream, response));
  co_return request;
}

task<std::error_code> connect_error(quoll::asio::connector connector, std::uint16_t port) {
  try {
    (void)co_await connector.connect("127.0.0.1", port);
  } catch (std::system_error const &error) {
    co_return error.code();
  }

  co_return std::error_code{};
}

task<io::io_result> read_once(io::any_stream &stream, std::span<std::byte> buffer) {
  co_return co_await stream.read_some(buffer);
}

task<> request_stop(std::stop_source &source) {
  source.request_stop();
  co_return;
}

// Connects, then reads from a peer that never writes and requests stop: when_all starts the read first, so the stop
// request reaches a pending read.
task<io::io_result> read_until_stopped(quoll::asio::connector connector, std::uint16_t port, std::stop_source &source) {
  auto stream = co_await connector.connect("127.0.0.1", port);
  std::array<std::byte, 16> buffer{};
  auto const [read, stopped] = co_await quoll::async::when_all(read_once(stream, buffer), request_stop(source));

  co_return read;
}

// Starts connecting, then requests stop while the attempt is pending.
task<std::error_code> connect_then_stop(quoll::asio::connector connector, std::uint16_t port,
                                        std::stop_source &source) {
  auto const [error, stopped] = co_await quoll::async::when_all(connect_error(connector, port), request_stop(source));

  co_return error;
}

task<io::io_result> connect_and_read(quoll::asio::connector connector, std::uint16_t port) {
  auto stream = co_await connector.connect("127.0.0.1", port);
  std::array<std::byte, 16> buffer{};

  co_return co_await stream.read_some(buffer);
}

// Awaits an operation that records the thread starting it, then completes from `context`.
task<std::thread::id> start_thread(::asio::io_context &context) {
  std::thread::id started_on;
  auto const cancel = std::make_shared<quoll::asio::detail::cancellation>(context.get_executor());
  auto const [error] = co_await quoll::asio::detail::make_operation<>(cancel, [&](auto handler) {
    started_on = std::this_thread::get_id();
    ::asio::post(context, [handler = std::move(handler)]() mutable { std::move(handler)(std::error_code{}); });
  });

  check(error);
  co_return started_on;
}

// Awaits an operation whose initiation throws.
task<int> start_failing(::asio::io_context &context) {
  auto const cancel = std::make_shared<quoll::asio::detail::cancellation>(context.get_executor());
  [[maybe_unused]] auto const [error] =
      co_await quoll::asio::detail::make_operation<>(cancel, [](auto) { throw std::runtime_error{"cannot start"}; });

  co_return 0;
}

// An asio connector counting the connections it opens.
class counting_connector {
public:
  counting_connector(quoll::asio::connector connector, int &connections) noexcept
      : connector_(std::move(connector)), connections_(&connections) {}

  [[nodiscard]] task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    ++*connections_;
    return connector_.connect(std::move(host), port);
  }

private:
  quoll::asio::connector connector_;
  int *connections_;
};

// Sends a GET on `transport`: the error it fails with, if any.
task<std::error_code> send_error(quoll::client::transport<quoll::asio::connector> transport) {
  try {
    (void)co_await transport.send(http::request{});
  } catch (std::system_error const &error) {
    co_return error.code();
  }

  co_return std::error_code{};
}

using accounts_client = quoll::rest::client<asio_test::accounts::api, quoll::client::transport<counting_connector>>;

struct accounts_calls {
  asio_test::account found;
  std::string missing;
  asio_test::account opened;
};

// The REST calls of the TCP tests, on one client.
template <class Client>
task<accounts_calls> make_calls(Client &client) {
  using namespace asio_test;

  accounts_calls calls;

  // The POST first: it opens a connection, which the GETs then reuse (a POST never takes an idle one).
  calls.opened = co_await client.template call<accounts::open>({.owner = "Grace"});
  calls.found = co_await client.template call<accounts::get>(42);
  calls.missing = (co_await client.template try_call<accounts::get>(7))->template get<http::status::not_found>().detail;
  co_return calls;
}

// REST calls on one client, then a stop request for the server.
task<accounts_calls> call_accounts(accounts_client client, std::stop_source &source) {
  auto calls = co_await make_calls(client);

  source.request_stop();
  co_return calls;
}

using quoll::test::free_port;

// A connection through the network of the runtime's executor, to a socket listening there: the bytes sent arrive.
task<std::string> loop_through_network() {
  auto listener = co_await io::listen(0);
  auto const port = listener.local_port().value();
  auto [accepted, connected] = co_await quoll::async::when_all(listener.accept(), io::connect("127.0.0.1", port));
  std::string const sent = "ping";
  std::array<char, 16> received{};

  check((co_await io::write(connected, std::as_bytes(std::span{sent}))).error);

  auto const read = co_await accepted.read_some(std::as_writable_bytes(std::span{received}));

  check(read.error);
  co_return std::string{received.data(), read.transferred};
}

// The time a sleep of `duration` takes.
task<std::chrono::nanoseconds> measure_sleep(std::chrono::nanoseconds duration) {
  auto const start = std::chrono::steady_clock::now();

  co_await quoll::async::sleep_for(duration);
  co_return std::chrono::steady_clock::now() - start;
}

// Reads from a connected peer that never sends, up to the timeout.
task<std::error_code> read_from_silent_peer(std::chrono::nanoseconds timeout) {
  auto listener = co_await io::listen(0);
  auto const port = listener.local_port().value();
  auto [accepted, connected] = co_await quoll::async::when_all(listener.accept(), io::connect("127.0.0.1", port));

  auto const read = [](io::any_stream &stream) -> task<std::size_t> {
    std::array<char, 16> received{};
    auto const result = co_await stream.read_some(std::as_writable_bytes(std::span{received}));

    check(result.error);
    co_return result.transferred;
  };

  try {
    static_cast<void>(co_await quoll::async::with_timeout(timeout, read(accepted)));
  } catch (std::system_error const &error) {
    co_return error.code();
  }

  co_return std::error_code{};
}

task<std::error_code> sleep_error(std::chrono::nanoseconds duration) {
  try {
    co_await quoll::async::sleep_for(duration);
  } catch (std::system_error const &error) {
    co_return error.code();
  }

  co_return std::error_code{};
}

// A client of a REST server that accepts and never answers, whose timeout is the one given to its URL.
task<std::error_code> call_silent_server(std::chrono::nanoseconds timeout) {
  auto listener = co_await io::listen(0);
  quoll::rest::client<asio_test::accounts::api> accounts{
      "http://127.0.0.1:" + std::to_string(listener.local_port().value()), {.response_timeout = timeout}};

  auto const call = [&]() -> task<std::error_code> {
    try {
      (void)co_await accounts.call<asio_test::accounts::get>(42);
    } catch (std::system_error const &error) {
      co_return error.code();
    }

    co_return std::error_code{};
  };

  auto [accepted, error] = co_await quoll::async::when_all(listener.accept(), call());

  co_return error;
}

// A peer connected to a server whose connections are idle for `timeout` at most: the server's closing ends its read.
task<std::error_code> idle_on_server(std::chrono::nanoseconds timeout) {
  http::router const router;
  auto listener = co_await io::listen(0);
  auto const port = listener.local_port().value();
  std::stop_source stop;
  quoll::server::serve_options options;

  options.connection.idle_timeout = timeout;

  auto const peer = [&]() -> task<std::error_code> {
    auto connection = co_await io::connect("127.0.0.1", port);
    std::array<char, 16> received{};
    auto const read = co_await connection.read_some(std::as_writable_bytes(std::span{received}));

    stop.request_stop();
    co_return read.error;
  };

  auto const serving = [&]() -> task<> {
    co_await quoll::async::detail::stop_scope<void>{quoll::server::serve(listener, router, std::move(options)),
                                                    stop.get_token()};
  };

  auto [served, error] = co_await quoll::async::when_all(serving(), peer());

  co_return error;
}

// REST calls with a client built from a URL, then Ctrl-C, which the runtime turns into a stop request.
task<accounts_calls> call_then_interrupt(std::uint16_t port) {
  quoll::rest::client<asio_test::accounts::api> client{"http://127.0.0.1:" + std::to_string(port)};
  auto calls = co_await make_calls(client);

  std::raise(SIGINT);
  co_return calls;
}

task<accounts_calls> serve_and_call(http::router const &router, std::uint16_t port) {
  auto [served, calls] =
      co_await quoll::async::when_all(quoll::server::listen(router, port), call_then_interrupt(port));

  co_return calls;
}

// The action SIGINT has.
[[nodiscard]] void (*sigint_handler())(int) {
  struct sigaction action {};

  ::sigaction(SIGINT, nullptr, &action);
  return action.sa_handler;
}

// Ctrl-C: the task is asked to stop, and SIGINT gets the action it had before the runtime back.
task<bool> interrupt_then_check_restored(void (*before)(int)) {
  std::raise(SIGINT);

  auto const stop_token = co_await quoll::async::current_stop_token();

  while (!stop_token.stop_requested()) {
    co_await quoll::async::yield();
  }

  co_return sigint_handler() == before;
}

// Yields `count` times, counting.
task<> keep_yielding(int &yields, int count) {
  for (int index = 0; index < count; ++index) {
    co_await quoll::async::yield();
    ++yields;
  }
}

// Leaves work running when it completes, as a server leaves connections.
task<int> leave_work_running(int &yields) {
  auto const executor = quoll::async::any_executor{co_await quoll::async::current_executor()};

  co_spawn(executor, keep_yielding(yields, 100), quoll::async::detached);
  co_return 7;
}

using quoll::test::run_task;

task<int> answer() { co_return 42; }

task<int> fail() {
  throw std::runtime_error{"failed"};
  co_return 0;
}

} // namespace

TEST_CASE("asio executor resumes coroutines on its io_context", "[asio]") {
  ::asio::io_context context;
  std::optional<int> result;
  std::exception_ptr failure;

  spawn(context, count_yields(3), result, failure);

  REQUIRE_FALSE(result);

  run(context, failure);

  REQUIRE(result == 3);
}

TEST_CASE("asio executors are equal when their asio executors are", "[asio]") {
  ::asio::io_context context;
  ::asio::io_context other;

  REQUIRE(quoll::asio::executor{context.get_executor()} == quoll::asio::executor{context.get_executor()});
  REQUIRE_FALSE(quoll::asio::executor{context.get_executor()} == quoll::asio::executor{other.get_executor()});
}

TEST_CASE("asio: a client GET over TCP, end to end", "[asio][http]") {
  ::asio::io_context context;
  auto acceptor = listen(context);
  std::optional<http::response> response;
  std::optional<http::request> request;
  std::exception_ptr failure;

  acceptor.async_accept([&](std::error_code error, tcp::socket socket) {
    check(error);
    spawn(context, serve_one(quoll::asio::tcp_stream{std::move(socket)}), request, failure);
  });
  spawn(context, get(quoll::asio::connector{context.get_executor()}, port_of(acceptor)), response, failure);
  run(context, failure);

  REQUIRE(request);
  REQUIRE(request->method == http::verb::get);
  REQUIRE(request->target == "/accounts/42");
  REQUIRE(response);
  REQUIRE(response->status == http::status::ok);
  REQUIRE(response->body == "hello over TCP");
}

TEST_CASE("asio: connecting to a port nobody listens on fails", "[asio]") {
  ::asio::io_context context;
  std::uint16_t port = 0;

  {
    auto acceptor = listen(context);

    port = port_of(acceptor);
  }

  std::optional<std::error_code> error;
  std::exception_ptr failure;

  spawn(context, connect_error(quoll::asio::connector{context.get_executor()}, port), error, failure);
  run(context, failure);

  REQUIRE(error);
  REQUIRE(*error == ::asio::error::connection_refused);
}

TEST_CASE("asio: the peer closing its socket is the end of the stream", "[asio]") {
  ::asio::io_context context;
  auto acceptor = listen(context);
  std::optional<io::io_result> read;
  std::exception_ptr failure;

  // The accepted socket is destroyed at once, closing the connection.
  acceptor.async_accept([](std::error_code error, tcp::socket) { check(error); });
  spawn(context, connect_and_read(quoll::asio::connector{context.get_executor()}, port_of(acceptor)), read, failure);
  run(context, failure);

  REQUIRE(read);
  REQUIRE(read->error == io::error::end_of_stream);
  REQUIRE(read->transferred == 0U);
}

TEST_CASE("asio: a stop request cancels a pending read", "[asio]") {
  ::asio::io_context context;
  auto acceptor = listen(context);
  std::optional<tcp::socket> silent_peer;
  std::optional<io::io_result> read;
  std::exception_ptr failure;
  std::stop_source source;

  // The peer stays connected and never writes: only the stop request ends the read.
  acceptor.async_accept([&](std::error_code error, tcp::socket socket) {
    check(error);
    silent_peer.emplace(std::move(socket));
  });
  spawn(context, read_until_stopped(quoll::asio::connector{context.get_executor()}, port_of(acceptor), source), read,
        failure, source.get_token());
  run(context, failure);

  REQUIRE(read);
  REQUIRE(read->error == std::errc::operation_canceled);
}

TEST_CASE("asio: a stop request abandons a connection attempt", "[asio]") {
  ::asio::io_context context;
  auto acceptor = listen(context);
  std::optional<std::error_code> error;
  std::exception_ptr failure;
  std::stop_source source;

  source.request_stop();
  spawn(context, connect_error(quoll::asio::connector{context.get_executor()}, port_of(acceptor)), error, failure,
        source.get_token());
  run(context, failure);

  REQUIRE(error);
  REQUIRE(*error == std::errc::operation_canceled);
}

TEST_CASE("asio: a client on a thread pool talks through sockets run by another thread", "[asio][http]") {
  ::asio::io_context context;
  auto acceptor = listen(context);
  // Keeps run() going while the client runs on the pool between two of its socket operations.
  auto work = ::asio::make_work_guard(context);
  quoll::threads::thread_pool pool{2};
  std::promise<http::response> response;
  std::optional<http::request> request;
  std::exception_ptr failure;

  acceptor.async_accept([&](std::error_code error, tcp::socket socket) {
    check(error);
    spawn(context, serve_one(quoll::asio::tcp_stream{std::move(socket)}), request, failure);
  });
  co_spawn(pool.get_executor(), get(quoll::asio::connector{context.get_executor()}, port_of(acceptor)),
           overloaded{[&](http::response value) {
                        response.set_value(std::move(value));
                        work.reset();
                      },
                      [&](std::exception_ptr error) {
                        response.set_exception(std::move(error));
                        work.reset();
                      }});
  run(context, failure);

  auto const received = response.get_future().get();

  REQUIRE(request);
  REQUIRE(request->target == "/accounts/42");
  REQUIRE(received.body == "hello over TCP");
}

TEST_CASE("asio: a stop request while connecting abandons the attempt", "[asio]") {
  ::asio::io_context context;
  auto acceptor = listen(context);
  std::optional<std::error_code> error;
  std::exception_ptr failure;
  std::stop_source source;

  spawn(context, connect_then_stop(quoll::asio::connector{context.get_executor()}, port_of(acceptor), source), error,
        failure, source.get_token());
  run(context, failure);

  REQUIRE(error);
  REQUIRE(*error == std::errc::operation_canceled);
}

TEST_CASE("asio: a stop request from a pool thread cancels a read pending on the I/O thread", "[asio]") {
  ::asio::io_context context;
  auto acceptor = listen(context);
  auto work = ::asio::make_work_guard(context);
  quoll::threads::thread_pool pool{2};
  std::optional<tcp::socket> silent_peer;
  std::promise<io::io_result> read;
  std::stop_source source;

  acceptor.async_accept([&](std::error_code error, tcp::socket socket) {
    check(error);
    silent_peer.emplace(std::move(socket));
  });
  // when_all runs on the pool: its second child requests stop there, while the read waits on the I/O thread.
  co_spawn(pool.get_executor(),
           read_until_stopped(quoll::asio::connector{context.get_executor()}, port_of(acceptor), source),
           overloaded{[&](io::io_result value) {
                        read.set_value(value);
                        work.reset();
                      },
                      [&](std::exception_ptr error) {
                        read.set_exception(std::move(error));
                        work.reset();
                      }},
           source.get_token());
  context.run();

  REQUIRE(read.get_future().get().error == std::errc::operation_canceled);
}

TEST_CASE("asio: an operation starts on its I/O executor, even awaited from a pool without a stop token", "[asio]") {
  ::asio::io_context context;
  auto work = ::asio::make_work_guard(context);
  quoll::threads::thread_pool pool{1};
  std::promise<std::thread::id> started_on;

  co_spawn(pool.get_executor(), start_thread(context),
           overloaded{[&](std::thread::id value) {
                        started_on.set_value(value);
                        work.reset();
                      },
                      [&](std::exception_ptr error) {
                        started_on.set_exception(std::move(error));
                        work.reset();
                      }});
  context.run();

  REQUIRE(started_on.get_future().get() == std::this_thread::get_id());
}

TEST_CASE("asio: an operation that cannot start rethrows to its awaiter", "[asio]") {
  ::asio::io_context context;
  std::optional<int> result;
  std::exception_ptr failure;
  std::stop_source source;

  SECTION("without a stop token") { spawn(context, start_failing(context), result, failure); }

  SECTION("with a stop token") { spawn(context, start_failing(context), result, failure, source.get_token()); }

  context.run();

  REQUIRE_FALSE(result);
  REQUIRE(failure);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::runtime_error);
}

TEST_CASE("asio: quoll::server serves a router over TCP, until a stop request", "[asio][server][http]") {
  ::asio::io_context context;
  quoll::asio::acceptor acceptor{listen(context)};
  auto const port = acceptor.socket().local_endpoint().port();
  quoll::asio::executor const executor{context.get_executor()};
  http::router router;

  router.add(http::verb::get, "/accounts/{id}", "accounts.get",
             [](http::request, http::route_match match) -> task<http::response> {
               http::response response;

               response.body = "account " + match.params.front();
               co_return response;
             });

  std::stop_source source;
  std::optional<http::response> response;
  std::exception_ptr failure;
  bool served = false;

  co_spawn(executor, quoll::server::serve(acceptor, router),
           overloaded{[&] { served = true; }, [&](std::exception_ptr error) { failure = std::move(error); }},
           source.get_token());
  spawn(context, get_then_stop(quoll::asio::connector{context.get_executor()}, port, source), response, failure);
  run(context, failure);

  REQUIRE(served);
  REQUIRE(response);
  REQUIRE(response->status == http::status::ok);
  REQUIRE(response->body == "account 42");
}

TEST_CASE("asio: a REST client calls a mounted API over TCP, on one connection kept alive", "[asio][rest][client]") {
  ::asio::io_context context;
  quoll::asio::acceptor acceptor{listen(context)};
  auto const port = acceptor.socket().local_endpoint().port();
  quoll::asio::executor const executor{context.get_executor()};
  http::router router;

  quoll::rest::mount<asio_test::accounts::api>(router, asio_test::accounts_service{});

  std::stop_source source;
  int connections = 0;
  accounts_client client{quoll::client::transport{
      counting_connector{quoll::asio::connector{context.get_executor()}, connections}, "127.0.0.1", port}};
  std::optional<accounts_calls> calls;
  std::exception_ptr failure;

  co_spawn(executor, quoll::server::serve(acceptor, router),
           overloaded{[] {}, [&](std::exception_ptr error) { failure = std::move(error); }}, source.get_token());
  spawn(context, call_accounts(std::move(client), source), calls, failure);
  run(context, failure);

  REQUIRE(calls);
  REQUIRE(calls->found.owner == "Ada");
  REQUIRE(calls->missing == "no account 7");
  REQUIRE(calls->opened.id == 7);
  REQUIRE(calls->opened.owner == "Grace");
  REQUIRE(connections == 1);
}

TEST_CASE("asio: a stop request abandons a request waiting for its response", "[asio][client]") {
  ::asio::io_context context;
  auto acceptor = listen(context);
  tcp::socket peer{context};
  std::array<char, 64> received{};
  std::stop_source source;

  // A server that reads the request, then never answers: the stop request comes once the client waits.
  acceptor.async_accept(peer, [&](std::error_code error) {
    check(error);
    peer.async_read_some(::asio::buffer(received), [&](std::error_code, std::size_t) { source.request_stop(); });
  });

  std::optional<std::error_code> error;
  std::exception_ptr failure;

  spawn(context,
        send_error(
            quoll::client::transport{quoll::asio::connector{context.get_executor()}, "127.0.0.1", port_of(acceptor)}),
        error, failure, source.get_token());
  run(context, failure);

  REQUIRE(error);
  REQUIRE(*error == std::errc::operation_canceled);
}

TEST_CASE("asio::runtime runs a task to its end", "[asio][runtime]") {
  REQUIRE(run_task(answer()) == 42);
  REQUIRE_THROWS_AS(run_task(fail()), std::runtime_error);
  REQUIRE_THROWS_AS(run_task(io::connect("127.0.0.1", free_port())), std::system_error);
}

TEST_CASE("asio::runtime turns the first Ctrl-C into a stop request, then gives SIGINT its action back",
          "[asio][runtime]") {
  if (sigint_handler() == SIG_IGN) {
    SKIP("SIGINT is ignored by this process (started in the background): raise() would not reach the runtime");
  }

  auto const before = sigint_handler();

  REQUIRE(run_task(interrupt_then_check_restored(before)));
  REQUIRE(sigint_handler() == before);
}

TEST_CASE("asio::runtime runs again, with a fresh stop token and the signals waited for again", "[asio][runtime]") {
  if (sigint_handler() == SIG_IGN) {
    SKIP("SIGINT is ignored by this process (started in the background): raise() would not reach the runtime");
  }

  auto const before = sigint_handler();
  quoll::asio::runtime runtime;

  for (int round = 0; round < 2; ++round) {
    auto const stop_token = runtime.get_stop_token();
    std::optional<bool> restored;
    std::exception_ptr failure;

    REQUIRE_FALSE(stop_token.stop_requested());
    co_spawn(runtime.get_executor(), interrupt_then_check_restored(before),
             overloaded{[&](bool value) { restored = value; }, [&](std::exception_ptr error) { failure = error; }},
             stop_token);
    runtime.run();

    if (failure) {
      std::rethrow_exception(failure);
    }

    REQUIRE(restored == true);
  }
}

TEST_CASE("asio::runtime runs until the work its tasks started has ended", "[asio][runtime]") {
  int yields = 0;

  REQUIRE(run_task(leave_work_running(yields)) == 7);
  REQUIRE(yields == 100);
}

TEST_CASE("asio::runtime offers its loop's network to its tasks", "[asio][runtime]") {
  REQUIRE(run_task(loop_through_network()) == "ping");
}

TEST_CASE("asio::runtime offers its loop's timer to its tasks", "[asio][runtime][timer]") {
  using namespace std::chrono_literals;

  REQUIRE(run_task(measure_sleep(20ms)) >= 20ms);
}

TEST_CASE("asio: with_timeout stops a read from a silent peer", "[asio][runtime][timer]") {
  using namespace std::chrono_literals;

  REQUIRE(run_task(read_from_silent_peer(20ms)) == std::errc::timed_out);
}

TEST_CASE("asio: a REST client from a URL with a response timeout stops waiting", "[asio][runtime][rest][timer]") {
  using namespace std::chrono_literals;

  REQUIRE(run_task(call_silent_server(20ms)) == std::errc::timed_out);
}

TEST_CASE("asio: a server closes a connection idle for too long", "[asio][runtime][server][timer]") {
  using namespace std::chrono_literals;

  REQUIRE(run_task(idle_on_server(20ms)) == io::error::end_of_stream);
}

TEST_CASE("asio: a stop request from another thread ends a sleep", "[asio][timer]") {
  using namespace std::chrono_literals;

  ::asio::io_context context;
  quoll::asio::executor const executor{context.get_executor()};
  std::stop_source source;
  std::optional<std::error_code> error;

  co_spawn(executor, sleep_error(1h),
           overloaded{[&](std::error_code result) { error = result; }, [](std::exception_ptr) {}}, source.get_token());

  std::jthread const stopper{[&source] {
    std::this_thread::sleep_for(10ms);
    source.request_stop();
  }};

  context.run();

  REQUIRE(error == std::errc::operation_canceled);
}

TEST_CASE("asio: a REST client from a URL calls server::listen, until Ctrl-C", "[asio][runtime][rest][client]") {
  if (sigint_handler() == SIG_IGN) {
    SKIP("SIGINT is ignored by this process (started in the background): raise() would not reach the runtime");
  }

  http::router router;

  quoll::rest::mount<asio_test::accounts::api>(router, asio_test::accounts_service{});

  auto const calls = run_task(serve_and_call(router, free_port()));

  REQUIRE(calls.found.owner == "Ada");
  REQUIRE(calls.missing == "no account 7");
  REQUIRE(calls.opened.owner == "Grace");
}

TEST_CASE("asio::runtime: an executor taken again after the last one went runs its task", "[asio]") {
  quoll::asio::runtime runtime;
  bool ran = false;

  { auto const gone = runtime.get_executor(); }

  auto const run_it = [&ran]() -> task<> {
    ran = true;
    co_return;
  };

  quoll::async::co_spawn(runtime.get_executor(), run_it(), quoll::async::detached);
  runtime.run();

  REQUIRE(ran);
}

TEST_CASE("asio::runtime: an executor on another thread holds run(), and its end lets it go", "[asio]") {
  quoll::asio::runtime runtime;

  auto held = runtime.get_executor();
  std::atomic<bool> dropped{false};
  std::jthread other{[held = std::move(held), &dropped]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    dropped = true;

    auto const last = std::move(held);
  }};

  runtime.run();

  REQUIRE(dropped);
}

TEST_CASE("asio::runtime: its io_context is asio's", "[asio]") {
  quoll::asio::runtime runtime;
  bool ran = false;

  ::asio::post(runtime.context(), [&ran] { ran = true; });
  runtime.run();

  REQUIRE(ran);
}

TEST_CASE("asio: over TCP, a stop request lets the request under way finish", "[asio]") {
  using namespace std::chrono_literals;

  auto const exchange = []() -> task<quoll::http::response> {
    quoll::http::router router;

    router.add(quoll::http::verb::get, "/slow", "slow",
               [](quoll::http::request, quoll::http::route_match) -> task<quoll::http::response> {
                 co_await quoll::async::sleep_for(100ms);

                 quoll::http::response response;

                 response.body = "done";
                 co_return response;
               });

    auto listener = co_await quoll::io::listen(0);
    std::stop_source stop;

    auto const serving = [&]() -> task<> {
      co_await quoll::async::detail::stop_scope<void>{quoll::server::serve(listener, router), stop.get_token()};
    };

    auto const stopping = [&]() -> task<> {
      co_await quoll::async::sleep_for(20ms);
      stop.request_stop();
    };

    auto const calling = [&]() -> task<quoll::http::response> {
      quoll::client::transport<> const transport{"http://127.0.0.1:" + std::to_string(listener.local_port().value())};
      quoll::http::request request;

      request.target = "/slow";
      co_return co_await transport.send(std::move(request));
    };

    auto [served, stopped, answer] = co_await quoll::async::when_all(serving(), stopping(), calling());

    co_return std::move(answer);
  };

  auto const answer = run_task(exchange());

  REQUIRE(answer.body == "done");
  REQUIRE(answer.fields.find("Connection") == "close");
}
