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
#include <quoll/asio/runtime.hpp>
#include <quoll/async/any_executor.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/current_executor.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detached.hpp>
#include <quoll/async/detail/end_stopped.hpp>
#include <quoll/async/detail/timeout_children.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/offload.hpp>
#include <quoll/async/runtime_options.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/strands_ref.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/client/client.hpp>
#include <quoll/http/body_reader.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/local_transport.hpp>
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
#include <quoll/io/pollable_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/rest.hpp>
#include <quoll/server/server.hpp>
#include <quoll/server/static_files.hpp>
#include <quoll/test/check.hpp>
#include <quoll/test/code_thrown_by.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/reports.hpp>
#include <quoll/test/run_task.hpp>
#include <quoll/test/under_stop.hpp>

#include <asio/any_io_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include <asio/post.hpp>
#include <asio/strand.hpp>
#include <asio/thread_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <signal.h>

#include <algorithm>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

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
  co_return co_await quoll::test::code_thrown_by(connector.connect("127.0.0.1", port));
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

// Sends a GET on `client`: the error it fails with, if any.
task<std::error_code> send_error(quoll::http::client client) {
  co_return co_await quoll::test::code_thrown_by(client.send(http::request{}));
}

using accounts_client = quoll::rest::client<asio_test::accounts::api, quoll::http::client>;

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

  // The POST first: it opens a connection, which the GETs then reuse.
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

  co_return co_await quoll::test::code_thrown_by(quoll::async::with_timeout(timeout, read(accepted)));
}

task<std::error_code> sleep_error(std::chrono::nanoseconds duration) {
  co_return co_await quoll::test::code_thrown_by(quoll::async::sleep_for(duration));
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
  quoll::http::serve_options options;

  options.connection.idle_timeout = timeout;

  auto const peer = [&]() -> task<std::error_code> {
    auto connection = co_await io::connect("127.0.0.1", port);
    std::array<char, 16> received{};
    auto const read = co_await connection.read_some(std::as_writable_bytes(std::span{received}));

    stop.request_stop();
    co_return read.error;
  };

  auto const serving = [&]() -> task<> {
    co_await quoll::test::under_stop(quoll::http::server{router, std::move(options)}.serve(std::move(listener)),
                                     stop.get_token());
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
  http::server const server{router};
  auto [served, calls] = co_await quoll::async::when_all(server.listen(port), call_then_interrupt(port));

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

TEST_CASE("asio: a connection does not wait to fill a packet, on either side", "[asio]") {
  ::asio::io_context context;
  auto acceptor = listen(context);
  auto const port = port_of(acceptor);
  std::exception_ptr failure;
  std::optional<bool> connected;
  std::optional<bool> accepted;

  auto const without_nagle = [](tcp::socket const &socket) {
    tcp::no_delay option;

    socket.get_option(option);
    return option.value();
  };

  auto const connecting = [&]() -> task<bool> {
    co_return without_nagle(co_await quoll::asio::detail::connect_socket(
        ::asio::any_io_executor{context.get_executor()}, std::string{"127.0.0.1"}, port));
  };

  quoll::asio::detail::acceptor_state state{std::move(acceptor)};

  auto const accepting = [&]() -> task<bool> {
    co_return without_nagle(co_await quoll::asio::detail::accept_socket(state));
  };

  spawn(context, accepting(), accepted, failure);
  spawn(context, connecting(), connected, failure);
  run(context, failure);

  REQUIRE(connected == true);
  REQUIRE(accepted == true);
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

TEST_CASE("asio: poll_read tells a quiet connection from one with bytes or closed, reading nothing", "[asio]") {
  using namespace std::chrono_literals;

  ::asio::io_context context;
  auto acceptor = listen(context);
  tcp::socket peer{context};
  std::optional<std::string> trace;
  std::exception_ptr failure;

  // What the peer did takes a moment to arrive.
  auto const settled = [](io::any_stream &stream, io::read_readiness wanted) {
    for (auto turn = 0; turn < 1000 && stream.poll_read() != wanted; ++turn) {
      std::this_thread::sleep_for(1ms);
    }

    return stream.poll_read() == wanted;
  };

  auto const probe = [&](quoll::asio::connector connector, std::uint16_t port) -> task<std::string> {
    auto stream = co_await connector.connect("127.0.0.1", port);
    std::array<char, 16> chunk{};
    std::string seen;

    while (!peer.is_open()) {
      co_await quoll::async::yield();
    }

    seen += settled(stream, io::read_readiness::would_wait) ? "quiet" : "?";
    ::asio::write(peer, ::asio::buffer(std::string_view{"x"}));
    seen += settled(stream, io::read_readiness::ready) ? ",ready" : ",?";

    // The peek took nothing: the byte is still there to read.
    auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

    seen += "," + std::string(chunk.data(), read.transferred);
    seen += stream.poll_read() == io::read_readiness::would_wait ? ",quiet" : ",?";
    peer.close();
    seen += settled(stream, io::read_readiness::ready) ? ",ready" : ",?";

    auto const ended = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

    seen += ended.error == io::error::end_of_stream ? ",end" : ",?";
    co_return seen;
  };

  acceptor.async_accept(peer, [](std::error_code error) { check(error); });
  spawn(context, probe(quoll::asio::connector{context.get_executor()}, port_of(acceptor)), trace, failure);
  run(context, failure);

  REQUIRE(trace == "quiet,ready,x,quiet,ready,end");
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
  ::asio::thread_pool pool{2};
  std::promise<http::response> response;
  std::optional<http::request> request;
  std::exception_ptr failure;

  acceptor.async_accept([&](std::error_code error, tcp::socket socket) {
    check(error);
    spawn(context, serve_one(quoll::asio::tcp_stream{std::move(socket)}), request, failure);
  });
  co_spawn(quoll::asio::executor{pool.get_executor(), quoll::asio::object_strands::own},
           get(quoll::asio::connector{context.get_executor()}, port_of(acceptor)),
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

TEST_CASE("asio: a client on a thread pool sends a body_writer through sockets run by another thread", "[asio][http]") {
  ::asio::io_context context;
  auto acceptor = listen(context);
  // Keeps run() going while the client runs on the pool between two of its socket operations.
  auto work = ::asio::make_work_guard(context);
  ::asio::thread_pool pool{2};
  std::promise<http::response> response;
  std::optional<http::request> request;
  std::exception_ptr failure;

  acceptor.async_accept([&](std::error_code error, tcp::socket socket) {
    check(error);
    spawn(context, serve_one(quoll::asio::tcp_stream{std::move(socket)}), request, failure);
  });

  auto const uploading = [](quoll::asio::connector connector, std::uint16_t port) -> task<http::response> {
    quoll::http::client const client{std::move(connector), "127.0.0.1", port};
    http::request message;

    message.method = http::verb::post;
    message.target = "/accounts/42";
    message.body_writer = [](http::body_stream out) -> task<> {
      std::string const piece(std::size_t{4} * 1024, 'x');

      for (auto index = 0; index < 64; ++index) {
        co_await out.write(piece);
      }
    };
    co_return co_await client.send(std::move(message));
  };

  co_spawn(quoll::asio::executor{pool.get_executor(), quoll::asio::object_strands::own},
           uploading(quoll::asio::connector{context.get_executor()}, port_of(acceptor)),
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
  REQUIRE(request->body.size() == std::size_t{256} * 1024);
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
  ::asio::thread_pool pool{2};
  std::optional<tcp::socket> silent_peer;
  std::promise<io::io_result> read;
  std::stop_source source;

  acceptor.async_accept([&](std::error_code error, tcp::socket socket) {
    check(error);
    silent_peer.emplace(std::move(socket));
  });
  // when_all runs on the pool: its second child requests stop there, while the read waits on the I/O thread.
  co_spawn(quoll::asio::executor{pool.get_executor(), quoll::asio::object_strands::own},
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
  ::asio::thread_pool pool{1};
  std::promise<std::thread::id> started_on;

  co_spawn(quoll::asio::executor{pool.get_executor(), quoll::asio::object_strands::own}, start_thread(context),
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

  co_spawn(executor, quoll::http::server{router}.serve(std::move(acceptor)),
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
  accounts_client client{quoll::http::client{
      counting_connector{quoll::asio::connector{context.get_executor()}, connections}, "127.0.0.1", port}};
  std::optional<accounts_calls> calls;
  std::exception_ptr failure;

  co_spawn(executor, quoll::http::server{router}.serve(std::move(acceptor)),
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
        send_error(quoll::http::client{quoll::asio::connector{context.get_executor()}, "127.0.0.1", port_of(acceptor)}),
        error, failure, source.get_token());
  run(context, failure);

  REQUIRE(error);
  REQUIRE(*error == std::errc::operation_canceled);
}

TEST_CASE("asio: a stream moved before its shutdown runs keeps it, and refuses what would follow", "[asio]") {
  ::asio::io_context context;
  auto listening = listen(context);
  tcp::socket client{context};

  client.connect(listening.local_endpoint());

  auto server = listening.accept();
  quoll::asio::tcp_stream stream{std::move(client)};
  std::optional<std::pair<io::io_result, io::io_result>> results;
  std::exception_ptr failure;

  auto const work = [&]() -> task<std::pair<io::io_result, io::io_result>> {
    // Its state is bound at the call: the stream may move before the task runs.
    auto stopping = stream.shutdown_send();
    quoll::asio::tcp_stream moved{std::move(stream)};
    auto const stopped = co_await std::move(stopping);
    std::array<std::byte, 4> const data{};
    auto const written = co_await moved.write_some(data);

    co_return std::pair{stopped, written};
  };

  spawn(context, work(), results, failure);
  run(context, failure);

  std::array<char, 4> received{};
  std::error_code ended;

  static_cast<void>(server.read_some(::asio::buffer(received), ended));

  REQUIRE(results);
  REQUIRE_FALSE(results->first.error);
  REQUIRE(results->second.error == std::errc::broken_pipe);
  REQUIRE(ended == ::asio::error::eof);
}

TEST_CASE("asio: the executor over a strand, its loop run by two threads", "[asio]") {
  ::asio::io_context context;
  auto const strand = ::asio::make_strand(context);
  quoll::asio::executor<std::remove_cvref_t<decltype(strand)>> const executor{strand};
  std::optional<std::string> answer;
  std::exception_ptr failure;

  // What it opens and accepts runs one handler at a time, on two threads: TSan sees it if not.
  auto const work = [&]() -> task<std::string> {
    auto listener = executor.listen("127.0.0.1", 0);
    auto const port = listener.local_port().value();

    auto const serving = [&]() -> task<> {
      auto stream = co_await listener.accept();
      std::array<char, 8> received{};

      (void)co_await stream.read_some(std::as_writable_bytes(std::span{received}));

      std::string_view const reply = "pong";

      (void)co_await io::write(stream, std::as_bytes(std::span{reply}));
    };

    auto const calling = [&]() -> task<std::string> {
      auto stream = co_await executor.connect("127.0.0.1", port);
      std::string_view const ping = "ping";
      std::array<char, 8> received{};

      (void)co_await io::write(stream, std::as_bytes(std::span{ping}));

      auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{received}));

      co_return std::string{received.data(), read.transferred};
    };

    auto [served, reply] = co_await quoll::async::when_all(serving(), calling());

    static_cast<void>(served);
    co_return reply;
  };

  co_spawn(executor, work(),
           overloaded{[&](std::string value) { answer = std::move(value); },
                      [&](std::exception_ptr error) { failure = std::move(error); }});

  std::thread other{[&context] { context.run(); }};

  context.run();
  other.join();

  if (failure) {
    std::rethrow_exception(failure);
  }

  REQUIRE(answer == "pong");
}

namespace {

// A plain counter that two tasks of one strand add to, each yielding between steps: TSan sees it if they do not take
// turns.
task<> add_with_yields(int &counter, int steps) {
  for (int step = 0; step < steps; ++step) {
    ++counter;

    if (step % 16 == 0) {
      co_await quoll::async::yield();
    }
  }
}

task<int> two_on_one_strand() {
  int counter = 0;
  auto [first, second] =
      co_await quoll::async::when_all(add_with_yields(counter, 2000), add_with_yields(counter, 2000));

  static_cast<void>(first);
  static_cast<void>(second);
  co_return counter;
}

// A client making requests on one kept connection, GETs and POSTs asking for 100 Continue: how many answered right.
task<int> call_many(std::uint16_t port, int requests) {
  http::client const client{"http://127.0.0.1:" + std::to_string(port)};
  int right = 0;

  for (int index = 0; index < requests; ++index) {
    http::request get;

    get.method = http::verb::get;
    get.target = "/hello";

    auto const got = co_await client.send(std::move(get));

    right += got.body == "hello" ? 1 : 0;

    http::request post;

    post.method = http::verb::post;
    post.target = "/echo";
    post.body = std::string(4096, static_cast<char>('a' + index % 26));
    post.fields.add("Expect", "100-continue");

    auto const echoed = co_await client.send(std::move(post));

    right += echoed.body == std::string(4096, static_cast<char>('a' + index % 26)) ? 1 : 0;
  }

  co_return right;
}

} // namespace

namespace {

// Arrives, then waits up to two seconds for `expected` tasks to have arrived, blocking its thread: they meet only if
// they run at once, on threads of their own.
task<bool> meet(std::atomic<int> &arrived, int expected) {
  ++arrived;

  auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};

  while (arrived.load() < expected && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  co_return arrived.load() >= expected;
}

} // namespace

TEST_CASE("asio::runtime on several threads runs tasks of different executors at once", "[asio][runtime][threads]") {
  quoll::asio::runtime runtime{quoll::async::runtime_options{.threads = 2}};
  std::atomic<int> arrived{0};
  std::atomic<int> met{0};

  for (int task_index = 0; task_index < 2; ++task_index) {
    co_spawn(runtime.get_executor(), meet(arrived, 2),
             overloaded{[&met](bool together) { met += together ? 1 : 0; }, [](std::exception_ptr) {}});
  }

  runtime.run();

  REQUIRE(met == 2);
}

TEST_CASE("asio::runtime on several threads: http::server::listen and its clients, through the runtime's network",
          "[asio][runtime][threads][server]") {
  quoll::asio::runtime runtime{quoll::async::runtime_options{.threads = 4}};
  auto const port = quoll::test::free_port();
  http::router router;

  router.get("/hello", [](http::request) {
    http::response response;

    response.body = "hello";
    return response;
  });
  router.post("/echo", [](http::request request) {
    http::response response;

    response.body = std::move(request.body);
    return response;
  });

  constexpr int clients = 8;
  std::stop_source server_stop;
  std::atomic<int> right{0};
  std::atomic<int> ended{0};
  std::atomic<int> failures{0};

  co_spawn(runtime.get_executor(), http::server{router}.listen(port),
           overloaded{[] {}, [&failures](std::exception_ptr) { ++failures; }}, server_stop.get_token());

  auto const calling = [](std::uint16_t to) -> task<int> {
    // Until the server listens: it starts on another thread.
    for (int attempt = 0;; ++attempt) {
      try {
        co_return co_await call_many(to, 4);
      } catch (std::system_error const &) {
        if (attempt == 50) {
          throw;
        }
      }

      co_await quoll::async::sleep_for(std::chrono::milliseconds{20});
    }
  };

  for (int client = 0; client < clients; ++client) {
    co_spawn(runtime.get_executor(), calling(port),
             overloaded{[&](int counted) {
                          right += counted;

                          if (++ended == clients) {
                            server_stop.request_stop();
                          }
                        },
                        [&](std::exception_ptr) {
                          ++failures;

                          if (++ended == clients) {
                            server_stop.request_stop();
                          }
                        }});
  }

  runtime.run();

  REQUIRE(failures == 0);
  REQUIRE(right == clients * 4 * 2);
}

TEST_CASE("asio::runtime on several threads: a handler's exception stops every thread, and run() throws it",
          "[asio][runtime][threads]") {
  quoll::asio::runtime runtime{quoll::async::runtime_options{.threads = 4}};
  // Kept, it keeps the loop running: the work never drains, and only the stop can end run().
  auto const kept = runtime.get_executor();

  ::asio::post(runtime.context(), [] { throw std::runtime_error{"from a handler"}; });

  REQUIRE_THROWS_AS(runtime.run(), std::runtime_error);
}

namespace {

// The body of a GET of `target`, once the server, starting on another thread, listens.
task<std::string> get_once_listening(std::uint16_t port, std::string target) {
  for (int attempt = 0;; ++attempt) {
    try {
      http::client const client{"http://127.0.0.1:" + std::to_string(port)};
      http::request get;

      get.method = http::verb::get;
      get.target = target;
      co_return (co_await client.send(std::move(get))).body;
    } catch (std::system_error const &) {
      if (attempt == 50) {
        throw;
      }
    }

    co_await quoll::async::sleep_for(std::chrono::milliseconds{20});
  }
}

} // namespace

TEST_CASE("asio::runtime on several threads runs a server's connections at once", "[asio][runtime][threads][server]") {
  quoll::asio::runtime runtime{quoll::async::runtime_options{.threads = 4}};
  auto const port = quoll::test::free_port();
  std::atomic<int> arrived{0};
  http::router router;

  // Blocks its thread until both connections have arrived, which only connections on strands of their own can do.
  router.get("/meet", [&arrived](http::request) {
    ++arrived;

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};

    while (arrived.load() < 2 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }

    http::response response;

    response.body = arrived.load() >= 2 ? "met" : "alone";
    return response;
  });

  std::stop_source server_stop;
  std::atomic<int> met{0};
  std::atomic<int> ended{0};
  std::atomic<int> failures{0};

  co_spawn(runtime.get_executor(), http::server{router}.listen(port),
           overloaded{[] {}, [&failures](std::exception_ptr) { ++failures; }}, server_stop.get_token());

  for (int client = 0; client < 2; ++client) {
    co_spawn(runtime.get_executor(), get_once_listening(port, "/meet"),
             overloaded{[&](std::string const &body) {
                          met += body == "met" ? 1 : 0;

                          if (++ended == 2) {
                            server_stop.request_stop();
                          }
                        },
                        [&](std::exception_ptr) {
                          ++failures;

                          if (++ended == 2) {
                            server_stop.request_stop();
                          }
                        }});
  }

  runtime.run();

  REQUIRE(failures == 0);
  REQUIRE(met == 2);
}

TEST_CASE("asio::runtime on several threads: the tasks of one executor take turns", "[asio][runtime][threads]") {
  quoll::asio::runtime runtime{quoll::async::runtime_options{.threads = 4}};
  std::atomic<int> total{0};

  for (int task_index = 0; task_index < 8; ++task_index) {
    co_spawn(runtime.get_executor(), two_on_one_strand(),
             overloaded{[&total](int counted) { total += counted; }, [](std::exception_ptr) {}});
  }

  runtime.run();

  REQUIRE(total == 8 * 4000);
}

TEST_CASE("asio::runtime on several threads: a server and clients, each connection on a strand of its own",
          "[asio][runtime][threads][server][client]") {
  quoll::asio::runtime runtime{quoll::async::runtime_options{.threads = 4}};
  auto &context = runtime.context();
  ::asio::ip::tcp::acceptor listening{::asio::make_strand(context),
                                      ::asio::ip::tcp::endpoint{::asio::ip::address_v4::loopback(), 0}};
  auto const port = listening.local_endpoint().port();
  quoll::asio::acceptor acceptor{std::move(listening), ::asio::any_io_executor{context.get_executor()}};
  http::router router;

  router.get("/hello", [](http::request) {
    http::response response;

    response.body = "hello";
    return response;
  });
  router.post("/echo", [](http::request request) {
    http::response response;

    response.body = std::move(request.body);
    return response;
  });

  constexpr int clients = 16;
  constexpr int requests = 8;
  std::stop_source server_stop;
  std::atomic<int> right{0};
  std::atomic<int> ended{0};
  std::atomic<int> failures{0};

  co_spawn(runtime.get_executor(), http::server{router}.serve(std::move(acceptor)),
           overloaded{[] {}, [&failures](std::exception_ptr) { ++failures; }}, server_stop.get_token());

  for (int client = 0; client < clients; ++client) {
    // Each executor of the runtime is a strand of its own: the clients run in parallel.
    co_spawn(runtime.get_executor(), call_many(port, requests),
             overloaded{[&](int counted) {
                          right += counted;

                          if (++ended == clients) {
                            server_stop.request_stop();
                          }
                        },
                        [&](std::exception_ptr) {
                          ++failures;

                          if (++ended == clients) {
                            server_stop.request_stop();
                          }
                        }});
  }

  runtime.run();

  REQUIRE(failures == 0);
  REQUIRE(right == clients * requests * 2);
}

TEST_CASE("asio::runtime: its strands, one thread or several", "[asio][runtime]") {
  quoll::asio::runtime single;
  auto const executor = single.get_executor();
  auto const strands = executor.query(quoll::async::service<quoll::async::strands_ref>);

  // One thread: nothing to take turns over, so a strand is the loop itself.
  REQUIRE(strands.make_strand() == strands.make_strand());

  quoll::asio::runtime several{quoll::async::runtime_options{.threads = 2}};
  auto const many = several.get_executor().query(quoll::async::service<quoll::async::strands_ref>);

  REQUIRE_FALSE(many.make_strand() == many.make_strand());
  REQUIRE_FALSE(several.get_executor() == several.get_executor());
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

TEST_CASE("asio::runtime on several threads turns Ctrl-C into a stop request, and gives SIGINT its action back",
          "[asio][runtime][threads]") {
  if (sigint_handler() == SIG_IGN) {
    SKIP("SIGINT is ignored by this process (started in the background): raise() would not reach the runtime");
  }

  auto const before = sigint_handler();
  quoll::asio::runtime runtime{quoll::async::runtime_options{.threads = 4}};
  auto const stop_token = runtime.get_stop_token();
  std::atomic<bool> restored{false};
  std::atomic<bool> failed{false};

  co_spawn(runtime.get_executor(), interrupt_then_check_restored(before),
           overloaded{[&](bool value) { restored = value; }, [&](std::exception_ptr) { failed = true; }}, stop_token);
  runtime.run();

  REQUIRE_FALSE(failed);
  REQUIRE(restored);
  REQUIRE(sigint_handler() == before);
}

TEST_CASE("asio::runtime runs until the work its tasks started has ended", "[asio][runtime]") {
  int yields = 0;

  REQUIRE(run_task(leave_work_running(yields)) == 7);
  REQUIRE(yields == 100);
}

TEST_CASE("asio::runtime: run(task) is the program's exit status, and reports what the task let escape",
          "[asio][runtime]") {
  auto const succeeding = []() -> task<> { co_await quoll::async::yield(); };
  auto const failing = []() -> task<> {
    co_await quoll::async::yield();
    throw std::runtime_error{"no port"};
  };
  // Cancelled with no stop asked for: something else cut it short, a failure.
  auto const cancelled = []() -> task<> {
    co_await quoll::async::yield();
    throw std::system_error{std::make_error_code(std::errc::operation_canceled), "cut short"};
  };

  {
    quoll::asio::runtime runtime;

    REQUIRE(runtime.run(succeeding()) == EXIT_SUCCESS);
  }

  {
    quoll::asio::runtime runtime;

    REQUIRE(runtime.run(failing()) == EXIT_FAILURE);
  }

  {
    quoll::asio::runtime runtime;

    REQUIRE(runtime.run(cancelled()) == EXIT_FAILURE);
  }

  auto const reported = quoll::test::reports();

  REQUIRE(reported.size() == 2);
  REQUIRE(reported.front() == "quoll: the program's main task ended on an exception: no port\n");
  REQUIRE(reported.back().starts_with("quoll: the program's main task ended on an exception: cut short"));
}

TEST_CASE("asio::runtime: run(task) ending on a stop nothing asked for is a failure", "[asio][runtime][stopped]") {
  auto const stopped = []() -> task<> {
    co_await quoll::async::yield();
    co_await quoll::async::detail::end_stopped{};
  };

  {
    quoll::asio::runtime runtime;

    REQUIRE(runtime.run(stopped()) == EXIT_FAILURE);
  }

  auto const reported = quoll::test::reports();

  REQUIRE(reported == std::vector<std::string>{"quoll: the program's main task ended on a stop nothing asked for\n"});
}

TEST_CASE("asio::runtime: run(task) stopped by a Ctrl-C is no failure", "[asio][runtime][timer]") {
  if (sigint_handler() == SIG_IGN) {
    SKIP("SIGINT is ignored by this process (started in the background): raise() would not reach the runtime");
  }

  auto const interrupted = []() -> task<> {
    std::raise(SIGINT);
    co_await quoll::async::sleep_for(std::chrono::hours{1});
  };
  quoll::asio::runtime runtime;

  REQUIRE(runtime.run(interrupted()) == EXIT_SUCCESS);
  REQUIRE(quoll::test::reports().empty());
}

TEST_CASE("asio::runtime: once run(task)'s task ends, those spawned with the runtime's stop token are asked to stop",
          "[asio][runtime][timer]") {
  quoll::asio::runtime runtime;
  // Not an executor: one kept here would keep run() going.
  auto const token = runtime.get_stop_token();
  auto stopped = false;

  // Without the stop, it would keep run() going for an hour.
  auto const serving = []() -> task<> { co_await quoll::async::sleep_for(std::chrono::hours{1}); };
  auto const main = [&]() -> task<> {
    auto const here = co_await quoll::async::current_executor();

    quoll::async::co_spawn(
        here, serving(),
        overloaded{[] {}, [](std::exception_ptr) {}, [&stopped](quoll::async::stopped_t) { stopped = true; }}, token);
    co_await quoll::async::yield();
  };

  REQUIRE(runtime.run(main()) == EXIT_SUCCESS);
  REQUIRE(stopped);
  REQUIRE(quoll::test::reports().empty());
}

TEST_CASE("asio::runtime offers its loop's network to its tasks", "[asio][runtime]") {
  REQUIRE(run_task(loop_through_network()) == "ping");
}

TEST_CASE("asio::runtime offers its loop's timer to its tasks", "[asio][runtime][timer]") {
  using namespace std::chrono_literals;

  REQUIRE(run_task(measure_sleep(20ms)) >= 20ms);
}

TEST_CASE("asio::runtime: its timer's clock goes by as it waits", "[asio][runtime][timer]") {
  using namespace std::chrono_literals;

  auto const waiting = []() -> task<std::chrono::nanoseconds> {
    auto const executor = co_await quoll::async::current_executor();
    auto const timer = executor.query<quoll::async::timer_ref>();
    auto const started = timer->now();

    co_await quoll::async::sleep_for(20ms);
    co_return timer->now() - started;
  };

  REQUIRE(run_task(waiting()) >= 20ms);
}

TEST_CASE("asio::runtime: a stop ends a detached task on a real timer, and run() returns", "[asio][runtime][timer]") {
  using namespace std::chrono_literals;

  quoll::asio::runtime runtime;
  std::stop_source stop;
  auto went_on = false;

  auto const waiting = []() -> task<> { co_await quoll::async::sleep_for(1h); };

  auto const stopping = [&]() -> task<> {
    co_await quoll::async::sleep_for(10ms);
    stop.request_stop();
    went_on = true;
  };

  co_spawn(runtime.get_executor(), waiting(), quoll::async::detached, stop.get_token());
  co_spawn(runtime.get_executor(), stopping(), quoll::async::detached);
  runtime.run();

  REQUIRE(went_on);
}

TEST_CASE("asio::runtime: offload runs its work on another thread, and comes back", "[asio][runtime]") {
  auto const working = []() -> task<bool> {
    auto const loop_thread = std::this_thread::get_id();
    auto const worker = co_await quoll::async::offload([] { return std::this_thread::get_id(); });

    co_return worker != loop_thread &&std::this_thread::get_id() == loop_thread;
  };

  REQUIRE(run_task(working()));

  auto const failing = []() -> task<int> {
    co_return co_await quoll::async::offload([]() -> int { throw std::runtime_error{"from the work"}; });
  };

  REQUIRE_THROWS_AS(run_task(failing()), std::runtime_error);
}

TEST_CASE("asio::runtime: a stop keeps offload from starting, and does not interrupt what has", "[asio][runtime]") {
  using namespace std::chrono_literals;

  auto const stopped = []() -> task<std::string> {
    std::stop_source stop;
    auto ran = false;

    stop.request_stop();

    try {
      co_await quoll::test::under_stop(quoll::async::offload([&] { ran = true; }), stop.get_token());
    } catch (std::system_error const &thrown) {
      co_return thrown.code() == std::errc::operation_canceled && !ran ? "not started" : "another failure";
    }

    co_return "ran";
  };

  REQUIRE(run_task(stopped()) == "not started");

  auto const started = []() -> task<std::string> {
    std::stop_source stop;
    std::atomic<bool> running{false};

    auto const working = [&]() -> task<std::string> {
      co_await quoll::async::offload([&] {
        running = true;
        std::this_thread::sleep_for(50ms);
      });
      co_return "finished";
    };

    auto const stopping = [&]() -> task<> {
      while (!running) {
        co_await quoll::async::sleep_for(1ms);
      }

      stop.request_stop();
    };

    auto const under_stop = [&]() -> task<std::string> {
      co_return co_await quoll::test::under_stop(working(), stop.get_token());
    };

    auto [result, stopper] = co_await quoll::async::when_all(under_stop(), stopping());

    co_return std::move(result);
  };

  REQUIRE(run_task(started()) == "finished");
}

TEST_CASE("asio::runtime: a stop cancels an offload in line behind others", "[asio][runtime]") {
  using namespace std::chrono_literals;

  auto const exchange = []() -> task<std::string> {
    std::atomic<int> running{0};
    std::atomic<bool> release{false};
    std::atomic<bool> ran{false};
    std::stop_source stop;

    // As many as there are threads, for the next to wait in line.
    auto const holding = [&]() -> task<> {
      co_await quoll::async::offload([&] {
        ++running;

        while (!release) {
          std::this_thread::sleep_for(1ms);
        }
      });
    };

    auto const in_line = [&]() -> task<std::string> {
      try {
        co_await quoll::test::under_stop(quoll::async::offload([&] { ran = true; }), stop.get_token());
      } catch (std::system_error const &thrown) {
        co_return thrown.code() == std::errc::operation_canceled ? "canceled" : "another failure";
      }

      co_return "ran";
    };

    auto const stopping = [&]() -> task<> {
      while (running != 2) {
        co_await quoll::async::sleep_for(1ms);
      }

      co_await quoll::async::sleep_for(5ms);
      stop.request_stop();
      release = true;
    };

    auto [first, second, stopper, outcome] =
        co_await quoll::async::when_all(holding(), holding(), stopping(), in_line());

    co_return outcome + (ran ? " after running" : "");
  };

  REQUIRE(run_task(exchange()) == "canceled");
}

TEST_CASE("asio::runtime: run() waits for an offload a detached task is on", "[asio][runtime]") {
  using namespace std::chrono_literals;

  quoll::asio::runtime runtime;
  auto ended = false;

  auto const working = [&]() -> task<> {
    co_await quoll::async::offload([] { std::this_thread::sleep_for(30ms); });
    ended = true;
  };

  co_spawn(runtime.get_executor(), working(), quoll::async::detached);
  runtime.run();

  REQUIRE(ended);
}

TEST_CASE("asio: static_files over TCP: a file far larger than a piece, read away from the loop",
          "[asio][runtime][server]") {
  auto const directory = std::filesystem::temp_directory_path() / ("quoll_static_asio_" + std::to_string(::getpid()));

  std::filesystem::create_directories(directory);

  std::string content;

  for (std::size_t index = 0; content.size() < std::size_t{4} * 1024 * 1024; ++index) {
    content += std::to_string(index) + ",";
  }

  std::ofstream{directory / "large.bin", std::ios::binary} << content;

  auto const exchange = [&]() -> task<std::string> {
    http::router router;

    router.use(quoll::http::static_files("/files", directory));

    auto listener = co_await io::listen(0);
    auto const port = listener.local_port().value();
    std::stop_source stop;

    auto const serving = [&]() -> task<> {
      co_await quoll::test::under_stop(quoll::http::server{router, {.shutdown_timeout = {}}}.serve(std::move(listener)),
                                       stop.get_token());
    };

    auto const calling = [&]() -> task<std::string> {
      quoll::async::detail::stop_on_exit const stop_server{stop};
      quoll::http::client const client{"http://127.0.0.1:" + std::to_string(port)};
      http::request request;

      request.target = "/files/large.bin";

      auto response = co_await client.stream(std::move(request));
      std::string body;

      while (auto const piece = co_await response.body.read()) {
        body += *piece;
      }

      co_return body;
    };

    auto [served, body] = co_await quoll::async::when_all(serving(), calling());

    co_return std::move(body);
  };

  auto const received = run_task(exchange());

  std::filesystem::remove_all(directory);
  REQUIRE(received == content);
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

namespace {

struct refused_upload {
  std::string received;
  std::error_code wrote;
  std::error_code ended;
};

// A client sending a body far over the limit, more than a socket's buffers hold, then reading what comes.
task<refused_upload> upload_refused(quoll::http::connection_options connection) {
  http::router router;
  auto listener = co_await io::listen(0);
  auto const port = listener.local_port().value();
  std::stop_source stop;
  std::string const head = "POST /nowhere HTTP/1.1\r\nHost: h\r\nContent-Length: 16777216\r\n\r\n";
  std::string const body(16 * 1024 * 1024, 'x');

  auto const serving = [&]() -> task<> {
    co_await quoll::test::under_stop(
        quoll::http::server{router, {.connection = std::move(connection), .shutdown_timeout = {}}}.serve(
            std::move(listener)),
        stop.get_token());
  };

  auto const uploading = [&]() -> task<refused_upload> {
    quoll::async::detail::stop_on_exit const stop_server{stop};
    auto stream = co_await io::connect("127.0.0.1", port);
    refused_upload result;
    std::array<char, 4096> chunk{};

    result.wrote = (co_await io::write(stream, std::as_bytes(std::span{head}))).error;

    if (!result.wrote) {
      result.wrote = (co_await io::write(stream, std::as_bytes(std::span{body}))).error;
    }

    for (;;) {
      auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

      if (read.error) {
        result.ended = read.error;
        co_return result;
      }

      result.received.append(chunk.data(), read.transferred);
    }
  };

  auto [served, upload] = co_await quoll::async::when_all(serving(), uploading());

  co_return std::move(upload);
}

} // namespace

TEST_CASE("asio: a client with a path in its URL sends its requests under it", "[asio][runtime][client]") {
  auto const exchange = []() -> task<std::string> {
    http::router router;

    router.get("/api/v1/users/{id}", [](http::request message, int id) {
      http::response answer;

      answer.body = message.target + " " + std::to_string(id);
      return answer;
    });
    router.get("/api/v1/", [] {
      http::response answer;

      answer.body = "root";
      return answer;
    });

    auto listener = co_await io::listen(0);
    auto const port = listener.local_port().value();
    std::stop_source stop;

    auto const serving = [&]() -> task<> {
      co_await quoll::test::under_stop(quoll::http::server{router, {.shutdown_timeout = {}}}.serve(std::move(listener)),
                                       stop.get_token());
    };

    auto const calling = [&]() -> task<std::string> {
      quoll::async::detail::stop_on_exit const stop_server{stop};
      quoll::http::client const client{"http://127.0.0.1:" + std::to_string(port) + "/api/v1/"};

      auto const request_of = [](std::string target) {
        http::request message;

        message.target = std::move(target);
        return message;
      };

      auto const user = co_await client.send(request_of("/users/7?x=1"));
      auto const root = co_await client.send(request_of("/"));

      co_return user.body + " " + root.body;
    };

    auto [served, body] = co_await quoll::async::when_all(serving(), calling());

    co_return std::move(body);
  };

  REQUIRE(run_task(exchange()) == "/api/v1/users/7?x=1 7 root");
}

TEST_CASE("asio: a body read in pieces over TCP, on a connection then reused", "[asio][runtime][client]") {
  auto const exchange = []() -> task<std::string> {
    http::router router;

    router.get("/pieces", [](http::request) {
      http::response answer;

      answer.body_writer = [](http::body_stream out) -> task<> {
        std::string const piece(std::size_t{4} * 1024, 'x');

        for (auto index = 0; index < 256; ++index) {
          co_await out.write(piece);
        }
      };
      return answer;
    });
    router.get("/hello", [](http::request) {
      http::response answer;

      answer.body = "hello";
      return answer;
    });

    auto listener = co_await io::listen(0);
    auto const port = listener.local_port().value();
    std::stop_source stop;

    auto const serving = [&]() -> task<> {
      co_await quoll::test::under_stop(quoll::http::server{router, {.shutdown_timeout = {}}}.serve(std::move(listener)),
                                       stop.get_token());
    };

    auto const calling = [&]() -> task<std::string> {
      quoll::async::detail::stop_on_exit const stop_server{stop};
      quoll::http::client const client{"http://127.0.0.1:" + std::to_string(port)};
      http::request request;

      request.target = "/pieces";

      auto response = co_await client.stream(std::move(request));
      std::size_t size = 0;
      std::size_t largest = 0;

      while (auto const piece = co_await response.body.read()) {
        size += piece->size();
        largest = std::max(largest, piece->size());
      }

      http::request next;

      next.target = "/hello";

      auto const hello = co_await client.send(std::move(next));

      co_return std::to_string(size) + (largest < size ? " in pieces " : " whole ") + hello.body;
    };

    auto [served, body] = co_await quoll::async::when_all(serving(), calling());

    co_return std::move(body);
  };

  REQUIRE(run_task(exchange()) == "1048576 in pieces hello");
}

TEST_CASE("asio: a body_writer over TCP: all of it, a server refusing it, and a peer that stops reading",
          "[asio][runtime][client][timer]") {
  auto const endless = [](http::body_stream out) -> task<> {
    std::string const piece(std::size_t{64} * 1024, 'x');

    for (;;) {
      co_await out.write(piece);
    }
  };

  SECTION("against a server") {
    auto const exchange = [&]() -> task<std::string> {
      http::router router;

      router.post("/size", [](http::request message) {
        http::response answer;

        answer.body = std::to_string(message.body.size());
        return answer;
      });
      router.post("/refuse", [](http::request, http::body_reader) {
        http::response answer;

        answer.status = http::status::content_too_large;
        return answer;
      });

      auto listener = co_await io::listen(0);
      auto const port = listener.local_port().value();
      std::stop_source stop;

      auto const serving = [&]() -> task<> {
        co_await quoll::test::under_stop(
            quoll::http::server{router, {.shutdown_timeout = {}}}.serve(std::move(listener)), stop.get_token());
      };

      auto const calling = [&]() -> task<std::string> {
        quoll::async::detail::stop_on_exit const stop_server{stop};
        quoll::http::client const client{"http://127.0.0.1:" + std::to_string(port)};

        auto const upload = [](std::string target, http::body_writer writer) {
          http::request request;

          request.method = http::verb::post;
          request.target = std::move(target);
          request.body_writer = std::move(writer);
          return request;
        };

        auto const whole = co_await client.send(upload("/size", [](http::body_stream out) -> task<> {
          std::string const piece(std::size_t{4} * 1024, 'x');

          for (auto index = 0; index < 256; ++index) {
            co_await out.write(piece);
          }
        }));
        auto const refused = co_await client.send(upload("/refuse", endless));

        co_return whole.body + " " + std::to_string(static_cast<int>(refused.status));
      };

      auto [served, body] = co_await quoll::async::when_all(serving(), calling());

      co_return std::move(body);
    };

    REQUIRE(run_task(exchange()) == "1048576 413");
  }

  SECTION("against a peer that reads nothing: a write has its timeout") {
    auto const exchange = [&]() -> task<std::error_code> {
      auto listener = co_await io::listen(0);
      auto const port = listener.local_port().value();

      auto const calling = [&]() -> task<std::error_code> {
        quoll::http::client_options options;

        options.response_timeout = std::chrono::milliseconds{100};

        quoll::http::client const client{"http://127.0.0.1:" + std::to_string(port), options};
        http::request request;

        request.method = http::verb::post;
        request.body_writer = endless;

        try {
          static_cast<void>(co_await client.send(std::move(request)));
        } catch (std::system_error const &thrown) {
          co_return thrown.code();
        }

        co_return std::error_code{};
      };

      auto [accepted, error] = co_await quoll::async::when_all(listener.accept(), calling());

      co_return error;
    };

    REQUIRE(run_task(exchange()) == std::errc::timed_out);
  }
}

TEST_CASE("asio: the minimum rate on a real clock, and a writer reading its request as it writes",
          "[asio][runtime][server]") {
  using namespace std::chrono_literals;

  auto const exchange = [](std::size_t part, int parts) -> task<std::string> {
    http::router router;

    router.post("/both", [](http::body_reader body) {
      http::response answer;

      answer.body_writer = [body](http::body_stream out) -> task<> {
        while (auto const piece = co_await body.read()) {
          co_await out.write(*piece);
        }
      };
      return answer;
    });

    auto listener = co_await io::listen(0);
    auto const port = listener.local_port().value();
    std::stop_source stop;
    quoll::http::serve_options options;

    options.shutdown_timeout = {};
    options.connection.read_timeout = 500ms;
    options.connection.min_read_rate = 1024;

    auto const serving = [&]() -> task<> {
      co_await quoll::test::under_stop(quoll::http::server{router, options}.serve(std::move(listener)),
                                       stop.get_token());
    };

    auto const calling = [&]() -> task<std::string> {
      quoll::async::detail::stop_on_exit const stop_server{stop};
      auto stream = co_await io::connect("127.0.0.1", port);
      auto const size = part * static_cast<std::size_t>(parts);
      auto const head =
          "POST /both HTTP/1.1\r\nHost: h\r\nConnection: close\r\nContent-Length: " + std::to_string(size) + "\r\n\r\n";
      std::string const bytes(part, 'x');

      (void)co_await io::write(stream, std::as_bytes(std::span{head}));

      for (auto index = 0; index < parts; ++index) {
        if ((co_await io::write(stream, std::as_bytes(std::span{bytes}))).error) {
          break;
        }

        co_await quoll::async::sleep_for(50ms);
      }

      std::string received;
      std::array<char, 4096> chunk{};

      for (;;) {
        auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

        if (read.error) {
          co_return received;
        }

        received.append(chunk.data(), read.transferred);
      }
    };

    auto [served, received] = co_await quoll::async::when_all(serving(), calling());

    co_return std::move(received);
  };

  // Above the rate, past the timeout in all, the piece echoed as it comes.
  auto const fast = run_task(exchange(4096, 15));

  REQUIRE(fast.starts_with("HTTP/1.1 200"));
  REQUIRE(fast.ends_with("0\r\n\r\n"));

  // Below it, it ends short of its end, as the response had started.
  auto const slow = run_task(exchange(1, 30));

  REQUIRE(slow.starts_with("HTTP/1.1 200"));
  REQUIRE(!slow.ends_with("0\r\n\r\n"));
}

TEST_CASE("asio: a timeout as far back as nanoseconds go has expired, and stays so on a real clock",
          "[asio][runtime][server]") {
  auto const exchange = []() -> task<std::string> {
    http::router router;

    router.get("/hello", [](http::request) { return http::response{}; });

    auto listener = co_await io::listen(0);
    auto const port = listener.local_port().value();
    std::stop_source stop;
    quoll::http::serve_options options;

    options.shutdown_timeout = {};
    options.connection.read_timeout = std::chrono::nanoseconds::min();

    auto const serving = [&]() -> task<> {
      co_await quoll::test::under_stop(quoll::http::server{router, options}.serve(std::move(listener)),
                                       stop.get_token());
    };

    auto const calling = [&]() -> task<std::string> {
      quoll::async::detail::stop_on_exit const stop_server{stop};
      auto stream = co_await io::connect("127.0.0.1", port);
      std::string_view const start = "GET /hello HTTP/1.1\r\nHost: h\r\n";
      std::string received;
      std::array<char, 4096> chunk{};

      (void)co_await io::write(stream, std::as_bytes(std::span{start}));

      for (;;) {
        auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

        if (read.error) {
          co_return received;
        }

        received.append(chunk.data(), read.transferred);
      }
    };

    auto [served, received] = co_await quoll::async::when_all(serving(), calling());

    co_return std::move(received);
  };

  REQUIRE(run_task(exchange()).starts_with("HTTP/1.1 408"));
}

TEST_CASE("asio: a client still sending a refused body finishes, then reads its answer", "[asio][runtime]") {
  quoll::http::connection_options connection;

  connection.limits.max_body_size = 16;

  auto const upload = run_task(upload_refused(std::move(connection)));

  // Closing with its bytes unread would have reset the connection under the client.
  REQUIRE_FALSE(upload.wrote);
  REQUIRE(upload.received.starts_with("HTTP/1.1 413"));
  REQUIRE(upload.ended == io::error::end_of_stream);
}

TEST_CASE("asio: a REST client from a URL calls http::server, until Ctrl-C", "[asio][runtime][rest][client]") {
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

TEST_CASE("http::server: the task of a temporary runs once it has gone, with its options", "[asio][runtime][server]") {
  http::router router;
  bool told = false;
  http::status status = http::status::ok;
  std::stop_source stop;
  auto const port = free_port();

  router.get("/fails", []() -> http::response { throw std::runtime_error{"on purpose"}; });

  auto const get_then_stop = [&]() -> task<> {
    http::client const client{"http://127.0.0.1:" + std::to_string(port)};
    http::request request;

    request.target = "/fails";
    status = (co_await client.send(request)).status;
    stop.request_stop();
  };

  quoll::asio::runtime runtime;

  quoll::async::co_spawn(
      runtime.get_executor(),
      http::server{router, {.connection = {.on_handler_error = [&told](std::exception_ptr) { told = true; }}}}.listen(
          port),
      quoll::async::detached, stop.get_token());
  quoll::async::co_spawn(runtime.get_executor(), get_then_stop(), quoll::async::detached);
  runtime.run();

  REQUIRE(status == http::status::internal_server_error);
  REQUIRE(told);
}

TEST_CASE("http::server: a listen that failed leaves its routes open", "[asio][runtime][server]") {
  auto const try_it = []() -> task<bool> {
    auto taken = co_await quoll::io::listen(0);
    http::server server;
    bool failed = false;

    try {
      co_await server.listen(taken.local_port().value());
    } catch (std::system_error const &) {
      failed = true;
    }

    server.get("/after", []() { return http::response{}; });
    co_return failed;
  };

  REQUIRE(run_task(try_it()));
}

TEST_CASE("rest::mount: on a server, as on a router", "[asio][rest][server]") {
  http::server server;

  quoll::rest::mount<asio_test::accounts::api>(server, asio_test::accounts_service{});

  quoll::rest::client<asio_test::accounts::api, http::local_transport> client{http::local_transport{server.routes()}};
  auto const calls = run_task(make_calls(client));

  REQUIRE(calls.found.owner == "Ada");
  REQUIRE(calls.opened.owner == "Grace");
}

namespace {

// Serves `router` over TCP on the executor it runs on, and runs `calling` with a client of it.
template <class Calling>
task<> serve_and_call(http::router const &router QUOLL_LIFETIMEBOUND, Calling calling) {
  auto listener = co_await quoll::io::listen(0);
  auto const port = listener.local_port().value();
  std::stop_source stop;

  auto const serving = [&]() -> task<int> {
    co_await quoll::test::under_stop(http::server{router, {.shutdown_timeout = {}}}.serve(std::move(listener)),
                                     stop.get_token());
    co_return 1;
  };

  auto const calls = [&]() -> task<int> {
    co_await calling(http::client{"http://127.0.0.1:" + std::to_string(port)});
    stop.request_stop();
    co_return 1;
  };

  (void)co_await quoll::async::when_all(serving(), calls());
}

// Runs `calling` with a client of a server whose connections run on a runtime of several threads, each on a strand of
// its own.
template <class Calling>
void calling_a_server_on_several_threads(http::router const &router, Calling calling) {
  quoll::asio::runtime runtime{quoll::async::runtime_options{.threads = 4}};
  std::exception_ptr failure;

  co_spawn(runtime.get_executor(), serve_and_call(router, std::move(calling)),
           overloaded{[] {}, [&failure](std::exception_ptr error) { failure = std::move(error); }});
  runtime.run();

  if (failure) {
    std::rethrow_exception(failure);
  }
}

// The same with no strand for a connection: on asio's thread pool, through an executor that offers none, so that what
// a connection does beside itself, reading while it writes, runs on two threads at once.
template <class Calling>
void calling_a_server_with_no_strand(http::router const &router, Calling calling) {
  ::asio::thread_pool pool{4};
  std::promise<void> done;

  co_spawn(quoll::asio::executor{pool.get_executor(), quoll::asio::object_strands::own},
           serve_and_call(router, std::move(calling)),
           overloaded{[&done] { done.set_value(); },
                      [&done](std::exception_ptr error) { done.set_exception(std::move(error)); }});
  done.get_future().get();
}

} // namespace

TEST_CASE("response_writer: on a runtime of several threads, as the connection it runs in",
          "[asio][runtime][server][threads]") {
  http::router router;

  router.get("/rows", [](http::response_writer out) -> task<> {
    out.fields.set("X-Handler", "set");
    co_await out.write("one ");
    co_await out.write("two");
  });

  std::vector<std::string> bodies;

  calling_a_server_on_several_threads(router, [&bodies](http::client const client) -> task<> {
    for (auto request = 0; request < 50; ++request) {
      auto const response = co_await client.get("/rows");

      bodies.push_back(response.body + ' ' + std::string{response.fields.find("X-Handler").value_or("")});
    }
  });

  REQUIRE(bodies.size() == 50);

  for (auto const &body : bodies) {
    REQUIRE(body == "one two set");
  }
}

// What TSan is here for: the first write asks whether the connection goes on, as the read side feeds the parser, on
// another thread, as no strand makes them take turns.
TEST_CASE("response_writer: on a thread pool with no strand per connection, reading as it writes",
          "[asio][server][threads]") {
  http::router router;
  std::atomic<std::size_t> read_in_all{0};

  router.post("/both", [&read_in_all](http::body_reader in, http::response_writer out) -> task<> {
    auto const reading = [&]() -> task<int> {
      while (auto const piece = co_await in.read()) {
        read_in_all += piece->size();
      }

      co_return 1;
    };

    auto const writing = [&]() -> task<int> {
      for (auto piece = 0; piece < 20; ++piece) {
        co_await out.write("piece ");
      }

      co_return 1;
    };

    (void)co_await quoll::async::when_all(reading(), writing());
  });

  constexpr auto requests = 20;
  constexpr std::size_t body_size = 256 * 1024;
  std::vector<std::size_t> received;

  calling_a_server_with_no_strand(router, [&received](http::client const client) -> task<> {
    for (auto request = 0; request < requests; ++request) {
      http::request both;

      both.method = http::verb::post;
      both.target = "/both";
      both.body = std::string(body_size, 'x');

      // A peer says whether `100 Continue` is owed, which the handler does not look at.
      if (request % 2 == 0) {
        both.fields.set("Expect", "100-continue");
      }

      auto const response = co_await client.send(std::move(both));

      received.push_back(response.body.size());
    }
  });

  REQUIRE(received == std::vector<std::size_t>(requests, 20 * std::string_view{"piece "}.size()));
  REQUIRE(read_in_all == requests * body_size);
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
    auto const port = listener.local_port().value();
    std::stop_source stop;

    auto const serving = [&]() -> task<> {
      co_await quoll::test::under_stop(quoll::http::server{router}.serve(std::move(listener)), stop.get_token());
    };

    auto const stopping = [&]() -> task<> {
      co_await quoll::async::sleep_for(20ms);
      stop.request_stop();
    };

    auto const calling = [&]() -> task<quoll::http::response> {
      quoll::http::client const client{"http://127.0.0.1:" + std::to_string(port)};
      quoll::http::request request;

      request.target = "/slow";
      co_return co_await client.send(std::move(request));
    };

    auto [served, stopped, answer] = co_await quoll::async::when_all(serving(), stopping(), calling());

    co_return std::move(answer);
  };

  auto const answer = run_task(exchange());

  REQUIRE(answer.body == "done");
  REQUIRE(answer.fields.find("Connection") == "close");
}
