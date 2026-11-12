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

#include <quoll/async/await_context.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/detail/chain_owner.hpp>
#include <quoll/async/detail/end_stopped.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/http/write.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/byte_stream.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/io_result.hpp>
#include <quoll/io/listener.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/read_until_closed.hpp>
#include <quoll/test/reports.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace http = quoll::http;
namespace io = quoll::io;

using http::verb;
using quoll::async::task;
using quoll::test::overloaded;

// Ends on the stop once one is requested: an accept with no peer coming, a read with nothing coming, as a socket's.
class wait_for_stop {
public:
  wait_for_stop() = default;
  wait_for_stop(wait_for_stop const &) = delete;
  wait_for_stop &operator=(wait_for_stop const &) = delete;
  wait_for_stop &operator=(wait_for_stop &&) = delete;
  ~wait_for_stop() = default;

  // Only moved before it suspends: nothing is registered yet.
  wait_for_stop(wait_for_stop &&) noexcept {}

  [[nodiscard]] bool await_ready() const noexcept { return false; }

  void await_suspend(std::coroutine_handle<> handle, quoll::async::await_context const &context) {
    item_.handle = handle;
    context_ = &context;
    // Invoked at once if stop was already requested: the owner is posted, and never unwinds inline.
    callback_.emplace(context.stop_token, waker{this});
  }

  void await_resume() { callback_.reset(); }

private:
  struct waker {
    wait_for_stop *self;

    void operator()() const noexcept { quoll::async::detail::stop_chain(self->context_->owner); }
  };

  quoll::async::work_item item_;
  quoll::async::await_context const *context_ = nullptr;
  std::optional<std::stop_callback<waker>> callback_;
};

// Hands over its failures, then its connections, in order; then waits for a stop request, which ends serve().
struct canned_listener {
  std::deque<std::error_code> failures;
  std::deque<io::any_stream> streams;

  task<io::any_stream> accept() {
    if (!failures.empty()) {
      auto const failure = failures.front();

      failures.pop_front();
      throw std::system_error{failure, "canned_listener"};
    }

    if (!streams.empty()) {
      auto stream = std::move(streams.front());

      streams.pop_front();
      co_return stream;
    }

    // Ends on the stop, and does not resume: what follows is for the compiler, as a task of a value ends on a return.
    co_await wait_for_stop{};
    throw std::logic_error{"canned_listener: resumed after a stop"};
  }
};

static_assert(io::Listener<canned_listener>);

// Hands over one connection, then fails with `failure` (by default, one that ends serve()).
struct one_then_broken {
  std::optional<io::any_stream> stream;
  std::exception_ptr failure =
      std::make_exception_ptr(std::system_error{std::make_error_code(std::errc::too_many_files_open), "broken"});

  task<io::any_stream> accept() {
    if (stream) {
      auto accepted = std::move(*stream);

      stream.reset();
      co_return accepted;
    }

    std::rethrow_exception(failure);
  }
};

static_assert(io::Listener<one_then_broken>);

// A stream that never receives anything: its read waits for a stop request, as a socket's does.
struct idle_stream {
  struct read_awaitable {
    wait_for_stop wait;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle, quoll::async::await_context const &context) {
      wait.await_suspend(handle, context);
    }

    io::io_result await_resume() {
      wait.await_resume();
      return {std::make_error_code(std::errc::operation_canceled), 0};
    }
  };

  struct write_awaitable {
    std::size_t size;

    [[nodiscard]] bool await_ready() const noexcept { return true; }

    void await_suspend(std::coroutine_handle<>, quoll::async::await_context const &) noexcept {}

    io::io_result await_resume() const noexcept { return {{}, size}; }
  };

  [[nodiscard]] read_awaitable read_some(std::span<std::byte>) { return {}; }

  [[nodiscard]] write_awaitable write_some(std::span<std::byte const> data) { return {data.size()}; }
};

static_assert(io::ByteStream<idle_stream>);

// A stream whose operations throw: what serve_connection() cannot handle.
struct broken_stream {
  struct awaitable {
    [[nodiscard]] bool await_ready() const noexcept { return true; }

    void await_suspend(std::coroutine_handle<>, quoll::async::await_context const &) noexcept {}

    io::io_result await_resume() { throw std::runtime_error{"broken stream"}; }
  };

  [[nodiscard]] awaitable read_some(std::span<std::byte>) { return {}; }

  [[nodiscard]] awaitable write_some(std::span<std::byte const>) { return {}; }
};

static_assert(io::ByteStream<broken_stream>);

http::router make_router() {
  http::router router;

  router.add(verb::get, "/{name}", "echo", [](http::request, http::route_match match) -> task<http::response> {
    http::response response;

    response.body = match.params.front();
    co_return response;
  });
  return router;
}

// A client: one GET, its response's body; `done` is set once it is in.
task<std::string> get(io::memory_stream client, std::string target, bool &done) {
  http::request request;
  std::string buffer;
  http::response_parser parser;

  request.target = std::move(target);
  request.fields.add("Host", "x");
  (void)co_await http::write(client, request);
  (void)co_await http::read(client, buffer, parser);
  done = true;
  co_return parser.release().body;
}

// Sends nothing until `other` is answered (or it waited long enough not to hang a serial server).
task<std::string> get_after(io::memory_stream client, std::string target, bool const &other, bool &done) {
  for (int round = 0; round < 1000 && !other; ++round) {
    co_await quoll::async::yield();
  }

  co_return co_await get(std::move(client), std::move(target), done);
}

task<> request_stop_soon(std::stop_source &source) {
  // Lets serve() accept the connection and start reading it first.
  for (int round = 0; round < 10; ++round) {
    co_await quoll::async::yield();
  }

  source.request_stop();
}

task<io::io_result> read_once(io::memory_stream &stream) {
  std::byte byte{};

  co_return co_await stream.read_some(std::span{&byte, 1});
}

template <class T>
auto store(std::optional<T> &into) {
  return overloaded{[&into](T value) { into = std::move(value); }, [](std::exception_ptr) {}};
}

// A route taking `duration`, as a handler that waits for something does.
http::router slow_router(std::chrono::nanoseconds duration) {
  http::router router;

  router.add(verb::get, "/slow", "slow", [duration](http::request, http::route_match) -> task<http::response> {
    co_await quoll::async::sleep_for(duration);

    http::response response;

    response.body = "done";
    co_return response;
  });
  return router;
}

task<> stop_after(std::chrono::nanoseconds duration, std::stop_source &source) {
  co_await quoll::async::sleep_for(duration);
  source.request_stop();
}

// One GET, kept on the connection afterwards: the response, or nothing if the connection ended first.
task<std::optional<http::response>> ask(io::memory_stream &client) {
  http::request request;
  std::string buffer;
  http::response_parser parser;

  request.target = "/slow";
  request.fields.add("Host", "x");
  (void)co_await http::write(client, request);

  if (co_await http::read(client, buffer, parser)) {
    co_return std::nullopt;
  }

  co_return parser.release();
}

auto ignored() {
  return overloaded{[] {}, [](std::exception_ptr) {}};
}

auto ignored_body() {
  return overloaded{[](std::string) {}, [](std::exception_ptr) {}};
}

// Hands over its connections 500 ms apart, then waits for a stop request.
struct later_listener {
  std::deque<io::any_stream> streams;

  task<io::any_stream> accept() {
    if (streams.empty()) {
      co_await wait_for_stop{};
      throw std::system_error{std::make_error_code(std::errc::operation_canceled), "later_listener"};
    }

    using namespace std::chrono_literals;

    co_await quoll::async::sleep_for(500ms);

    auto stream = std::move(streams.front());

    streams.pop_front();
    co_return stream;
  }
};

static_assert(io::Listener<later_listener>);

} // namespace

TEST_CASE("serve: a stop request lets the request under way finish, and closes its connection", "[server]") {
  using namespace std::chrono_literals;

  auto const router = slow_router(1s);
  quoll::test::virtual_time time;
  auto [client, server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  std::optional<std::optional<http::response>> answer;
  bool served = false;

  listener.streams.emplace_back(std::move(server));
  quoll::async::co_spawn(
      time.get_executor(), quoll::http::server{router}.serve(std::move(listener)),
      overloaded{[&] { served = true; }, [](std::exception_ptr) {}}, source.get_token()
  );
  quoll::async::co_spawn(time.get_executor(), ask(client), store(answer));
  quoll::async::co_spawn(time.get_executor(), stop_after(100ms, source), ignored());
  time.run();

  REQUIRE(served);
  REQUIRE(answer.value().value().body == "done");
  REQUIRE(answer.value().value().fields.find("Connection") == "close");
  REQUIRE(time.now() == 1s);
  REQUIRE(quoll::test::run(read_once(client)).value().error == io::error::end_of_stream);
}

TEST_CASE("serve: a stop request closes an idle connection at once", "[server]") {
  using namespace std::chrono_literals;

  auto const router = slow_router(1s);
  quoll::test::virtual_time time;
  auto [client, server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  bool served = false;

  listener.streams.emplace_back(std::move(server));
  quoll::async::co_spawn(
      time.get_executor(), quoll::http::server{router}.serve(std::move(listener)),
      overloaded{[&] { served = true; }, [](std::exception_ptr) {}}, source.get_token()
  );
  quoll::async::co_spawn(time.get_executor(), stop_after(100ms, source), ignored());
  time.run();

  REQUIRE(served);
  REQUIRE(time.now() == 100ms);
  REQUIRE(quoll::test::run(read_once(client)).value().error == io::error::end_of_stream);
}

TEST_CASE("serve: past shutdown_timeout, the requests under way are stopped", "[server]") {
  using namespace std::chrono_literals;

  auto const router = slow_router(1h);
  quoll::test::virtual_time time;
  auto [client, server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  std::optional<std::optional<http::response>> answer;
  bool served = false;

  listener.streams.emplace_back(std::move(server));
  quoll::async::co_spawn(
      time.get_executor(), quoll::http::server{router, {.shutdown_timeout = 5s}}.serve(std::move(listener)),
      overloaded{[&] { served = true; }, [](std::exception_ptr) {}}, source.get_token()
  );
  quoll::async::co_spawn(time.get_executor(), ask(client), store(answer));
  quoll::async::co_spawn(time.get_executor(), stop_after(100ms, source), ignored());
  time.run();

  REQUIRE(served);
  REQUIRE(time.now() == 5100ms);
}

TEST_CASE("serve: without a shutdown_timeout, a stop request stops the requests under way at once", "[server]") {
  using namespace std::chrono_literals;

  auto const router = slow_router(1h);
  quoll::test::virtual_time time;
  auto [client, server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  std::optional<std::optional<http::response>> answer;
  bool served = false;

  listener.streams.emplace_back(std::move(server));
  quoll::async::co_spawn(
      time.get_executor(), quoll::http::server{router, {.shutdown_timeout = 0s}}.serve(std::move(listener)),
      overloaded{[&] { served = true; }, [](std::exception_ptr) {}}, source.get_token()
  );
  quoll::async::co_spawn(time.get_executor(), ask(client), store(answer));
  quoll::async::co_spawn(time.get_executor(), stop_after(100ms, source), ignored());
  time.run();

  REQUIRE(served);
  REQUIRE(time.now() == 100ms);
}

TEST_CASE("serve: past max_connections, a new connection is closed, and the others stay", "[server]") {
  using namespace std::chrono_literals;

  auto const router = slow_router(1s);
  quoll::test::virtual_time time;
  auto [kept_client, kept_server] = io::make_memory_stream_pair();
  auto [refused_client, refused_server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  std::optional<std::optional<http::response>> kept;
  std::optional<std::optional<http::response>> refused;

  listener.streams.emplace_back(std::move(kept_server));
  listener.streams.emplace_back(std::move(refused_server));
  quoll::async::co_spawn(
      time.get_executor(), quoll::http::server{router, {.max_connections = 1}}.serve(std::move(listener)), ignored(),
      source.get_token()
  );
  quoll::async::co_spawn(time.get_executor(), ask(kept_client), store(kept));
  quoll::async::co_spawn(time.get_executor(), ask(refused_client), store(refused));
  quoll::async::co_spawn(time.get_executor(), stop_after(2s, source), ignored());
  time.run();

  REQUIRE(kept.value().value().body == "done");
  REQUIRE_FALSE(refused.value().has_value());
}

TEST_CASE("serve: running out of file descriptors waits, then accepts again", "[server]") {
  using namespace std::chrono_literals;

  auto const router = slow_router(1s);
  quoll::test::virtual_time time;
  auto [client, server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  std::optional<std::optional<http::response>> answer;
  std::chrono::nanoseconds answered_at{};

  listener.failures.emplace_back(std::make_error_code(std::errc::too_many_files_open));
  listener.streams.emplace_back(std::move(server));
  quoll::async::co_spawn(
      time.get_executor(), quoll::http::server{router}.serve(std::move(listener)), ignored(), source.get_token()
  );
  quoll::async::co_spawn(
      time.get_executor(), ask(client),
      overloaded{
          [&](std::optional<http::response> value) {
            answer = std::move(value);
            answered_at = time.now();
          },
          [](std::exception_ptr) {}
      }
  );
  quoll::async::co_spawn(time.get_executor(), stop_after(2s, source), ignored());
  time.run();

  REQUIRE(answer.value().value().body == "done");
  // The handler's second, after the wait.
  REQUIRE(answered_at == 1100ms);
}

TEST_CASE("serve: without a limit, every connection is served", "[server]") {
  using namespace std::chrono_literals;

  auto const router = slow_router(1s);
  quoll::test::virtual_time time;
  auto [first_client, first_server] = io::make_memory_stream_pair();
  auto [second_client, second_server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  std::optional<std::optional<http::response>> first;
  std::optional<std::optional<http::response>> second;

  listener.streams.emplace_back(std::move(first_server));
  listener.streams.emplace_back(std::move(second_server));
  quoll::async::co_spawn(
      time.get_executor(), quoll::http::server{router, {.max_connections = 0}}.serve(std::move(listener)), ignored(),
      source.get_token()
  );
  quoll::async::co_spawn(time.get_executor(), ask(first_client), store(first));
  quoll::async::co_spawn(time.get_executor(), ask(second_client), store(second));
  quoll::async::co_spawn(time.get_executor(), stop_after(2s, source), ignored());
  time.run();

  REQUIRE(first.value().value().body == "done");
  REQUIRE(second.value().value().body == "done");
}

TEST_CASE("serve: a connection that ends frees its place under the limit", "[server]") {
  using namespace std::chrono_literals;

  auto const router = make_router();
  quoll::test::virtual_time time;
  auto [first_client, first_server] = io::make_memory_stream_pair();
  auto [second_client, second_server] = io::make_memory_stream_pair();
  later_listener listener;
  std::stop_source source;
  std::optional<std::string> second;
  bool first_done = false;
  bool second_done = false;

  listener.streams.emplace_back(std::move(first_server));
  listener.streams.emplace_back(std::move(second_server));
  quoll::async::co_spawn(
      time.get_executor(), quoll::http::server{router, {.max_connections = 1}}.serve(std::move(listener)), ignored(),
      source.get_token()
  );
  // The first client closes its end, once answered, which ends its connection; the second comes after.
  quoll::async::co_spawn(time.get_executor(), get(std::move(first_client), "/one", first_done), ignored_body());
  quoll::async::co_spawn(time.get_executor(), get(std::move(second_client), "/two", second_done), store(second));
  quoll::async::co_spawn(time.get_executor(), stop_after(2s, source), ignored());
  time.run();

  REQUIRE(first_done);
  REQUIRE(second == "two");
}

TEST_CASE("serve: an accept failure ends the loop, and lets the request under way finish first", "[server]") {
  using namespace std::chrono_literals;

  auto const router = slow_router(1s);
  quoll::test::virtual_time time;
  auto [client, server] = io::make_memory_stream_pair();
  one_then_broken listener;
  std::optional<std::optional<http::response>> answer;
  std::exception_ptr failure;

  listener.stream.emplace(std::move(server));
  listener.failure =
      std::make_exception_ptr(std::system_error{std::make_error_code(std::errc::permission_denied), "broken"});
  quoll::async::co_spawn(
      time.get_executor(), quoll::http::server{router}.serve(std::move(listener)),
      overloaded{[] {}, [&](std::exception_ptr thrown) { failure = std::move(thrown); }}
  );
  quoll::async::co_spawn(time.get_executor(), ask(client), store(answer));
  time.run();

  REQUIRE(answer.value().value().body == "done");
  REQUIRE(time.now() == 1s);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::system_error);
}

TEST_CASE("serve: connections are served concurrently", "[server]") {
  auto const router = make_router();
  quoll::async::run_loop loop;
  auto [first_client, first_server] = io::make_memory_stream_pair();
  auto [second_client, second_server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;

  // The first connection waits for the second to be answered: a serial server would answer it last.
  listener.streams.emplace_back(std::move(first_server));
  listener.streams.emplace_back(std::move(second_server));

  bool first_done = false;
  bool second_done = false;
  bool second_first = false;
  std::optional<std::string> first;
  std::optional<std::string> second;

  quoll::async::co_spawn(
      loop.get_executor(), quoll::http::server{router}.serve(std::move(listener)),
      overloaded{[] {}, [](std::exception_ptr) {}}, source.get_token()
  );
  quoll::async::co_spawn(
      loop.get_executor(), get_after(std::move(first_client), "/one", second_done, first_done),
      overloaded{
          [&](std::string body) {
            second_first = second_done;
            first = std::move(body);
            source.request_stop();
          },
          [](std::exception_ptr) {}
      }
  );
  quoll::async::co_spawn(loop.get_executor(), get(std::move(second_client), "/two", second_done), store(second));
  loop.run();

  REQUIRE(first == "one");
  REQUIRE(second == "two");
  REQUIRE(second_first);
}

TEST_CASE("serve: a stop request ends the accept loop and cancels idle connections", "[server]") {
  auto const router = make_router();
  quoll::async::run_loop loop;
  auto [idle_client, idle_server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  bool served = false;

  listener.streams.emplace_back(std::move(idle_server));
  quoll::async::co_spawn(
      loop.get_executor(), quoll::http::server{router}.serve(std::move(listener)),
      overloaded{[&] { served = true; }, [](std::exception_ptr) {}}, source.get_token()
  );
  quoll::async::co_spawn(loop.get_executor(), request_stop_soon(source), overloaded{[] {}, [](std::exception_ptr) {}});
  loop.run();

  REQUIRE(served);
  // The idle connection ended on the stop request, not on the client closing: its server end is gone.
  REQUIRE(quoll::test::run(read_once(idle_client)).value().error == io::error::end_of_stream);
}

TEST_CASE("serve: an accept failing for one connection is skipped, another is rethrown", "[server]") {
  auto const router = make_router();
  quoll::async::run_loop loop;
  auto [client, server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  std::optional<std::string> body;
  bool done = false;

  listener.failures = {
      std::make_error_code(std::errc::connection_aborted), std::make_error_code(std::errc::connection_reset)
  };
  listener.streams.emplace_back(std::move(server));
  quoll::async::co_spawn(
      loop.get_executor(), quoll::http::server{router}.serve(std::move(listener)),
      overloaded{[] {}, [](std::exception_ptr) {}}, source.get_token()
  );
  quoll::async::co_spawn(
      loop.get_executor(), get(std::move(client), "/after", done),
      overloaded{
          [&](std::string received) {
            body = std::move(received);
            source.request_stop();
          },
          [](std::exception_ptr) {}
      }
  );
  loop.run();

  REQUIRE(body == "after");

  // A policy may retry other failures (after waiting, with the application's timer): the stream after them is served.
  auto [retried_client, retried_server] = io::make_memory_stream_pair();
  canned_listener retrying;
  std::stop_source retry_source;
  std::vector<std::error_code> seen;
  std::optional<std::string> retried;
  bool retried_done = false;
  quoll::http::serve_options patient;

  retrying.failures = {std::make_error_code(std::errc::too_many_files_open)};
  retrying.streams.emplace_back(std::move(retried_server));
  patient.on_accept_error = [&](std::error_code error) -> task<bool> {
    seen.push_back(error);
    co_return true;
  };
  quoll::async::co_spawn(
      loop.get_executor(), quoll::http::server{router, std::move(patient)}.serve(std::move(retrying)),
      overloaded{[] {}, [](std::exception_ptr) {}}, retry_source.get_token()
  );
  quoll::async::co_spawn(
      loop.get_executor(), get(std::move(retried_client), "/retried", retried_done),
      overloaded{
          [&](std::string received) {
            retried = std::move(received);
            retry_source.request_stop();
          },
          [](std::exception_ptr) {}
      }
  );
  loop.run();

  REQUIRE(seen == std::vector{std::make_error_code(std::errc::too_many_files_open)});
  REQUIRE(retried == "retried");

  canned_listener failing;
  std::exception_ptr failure;

  failing.failures = {std::make_error_code(std::errc::too_many_files_open)};
  quoll::async::co_spawn(
      loop.get_executor(), quoll::http::server{router}.serve(std::move(failing)),
      overloaded{[] {}, [&](std::exception_ptr error) { failure = std::move(error); }}
  );
  loop.run();

  REQUIRE(failure);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::system_error);
}

TEST_CASE("serve: a connection's failure goes to on_connection_error", "[server]") {
  auto const router = make_router();
  quoll::async::run_loop loop;
  canned_listener listener;
  std::stop_source source;
  std::string reported;

  listener.streams.emplace_back(broken_stream{});

  quoll::http::serve_options options;

  options.on_connection_error = [&](std::exception_ptr failure) {
    try {
      std::rethrow_exception(failure);
    } catch (std::exception const &error) {
      reported = error.what();
    }

    source.request_stop();
  };

  quoll::async::co_spawn(
      loop.get_executor(), quoll::http::server{router, std::move(options)}.serve(std::move(listener)),
      overloaded{[] {}, [](std::exception_ptr) {}}, source.get_token()
  );
  loop.run();

  REQUIRE(reported == "broken stream");
}

TEST_CASE("serve: with no callback, a handler that throws and a connection that fails are reported", "[server]") {
  auto router = make_router();
  quoll::async::run_loop loop;
  auto [client, server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  std::optional<std::string> body;
  bool done = false;

  router.add(verb::get, "/fault/throws", "throws", [](http::request, http::route_match) -> task<http::response> {
    throw std::runtime_error{"a bug of the handler"};
  });
  listener.streams.emplace_back(broken_stream{});
  listener.streams.emplace_back(std::move(server));
  quoll::async::co_spawn(
      loop.get_executor(), quoll::http::server{router}.serve(std::move(listener)),
      overloaded{[] {}, [](std::exception_ptr) {}}, source.get_token()
  );
  quoll::async::co_spawn(
      loop.get_executor(), get(std::move(client), "/fault/throws", done),
      overloaded{
          [&](std::string received) {
            body = std::move(received);
            source.request_stop();
          },
          [](std::exception_ptr) {}
      }
  );
  loop.run();

  REQUIRE(done);

  auto const reported = quoll::test::reports();

  REQUIRE(reported.size() == 2);
  REQUIRE(std::ranges::count(reported, "quoll: a connection ended on an exception: broken stream\n") == 1);
  REQUIRE(
      std::ranges::count(reported, "quoll: a handler or a middleware threw, answered 500: a bug of the handler\n") == 1
  );
}

TEST_CASE("serve: a connection ending on a stop is no failure: no callback hears of it", "[server][stopped]") {
  auto router = make_router();
  quoll::async::run_loop loop;
  auto [client, server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  std::optional<std::string> received;
  auto told = 0;
  quoll::http::serve_options options;

  router.add(verb::get, "/fault/stops", "stops", [](http::request, http::route_match) -> task<http::response> {
    co_await quoll::async::detail::end_stopped{};
    co_return http::response{};
  });
  options.on_connection_error = [&](std::exception_ptr) { ++told; };
  options.connection.on_handler_error = [&](std::exception_ptr) { ++told; };
  listener.streams.emplace_back(std::move(server));
  quoll::async::co_spawn(
      loop.get_executor(), quoll::http::server{router, std::move(options)}.serve(std::move(listener)),
      overloaded{[] {}, [](std::exception_ptr) {}}, source.get_token()
  );
  // The connection goes with its frames: the client reads the end of the stream, with no answer.
  auto const ask = [](io::memory_stream stream) -> task<std::string> {
    http::request request;

    request.target = "/fault/stops";
    request.fields.add("Host", "x");
    (void)co_await http::write(stream, request);
    co_return co_await quoll::test::read_until_closed(stream);
  };

  quoll::async::co_spawn(
      loop.get_executor(), ask(std::move(client)),
      overloaded{
          [&](std::string answer) {
            received = std::move(answer);
            source.request_stop();
          },
          [&](std::exception_ptr) { source.request_stop(); }
      }
  );
  loop.run();

  REQUIRE(received == "");
  REQUIRE(told == 0);
  REQUIRE(quoll::test::reports().empty());
}

TEST_CASE(
    "serve: a handler catching all around a sleep does not see the server's stop, which ends it",
    "[server][stopped][timer]"
) {
  auto router = make_router();
  quoll::test::virtual_time time;
  auto [client, server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  auto caught = 0;
  auto turns = 0;
  auto served = false;

  // Bounded, so that the trap #151 closes, a handler catching the stop and sleeping again at once, ends the test.
  router.add(
      verb::get, "/retries", "retries",
      [&caught, &turns](http::request, http::route_match) -> task<http::response> {
        while (turns < 1000) {
          ++turns;

          try {
            co_await quoll::async::sleep_for(std::chrono::hours{1});
          } catch (std::exception const &) {
            ++caught;
          }
        }

        co_return http::response{};
      }
  );
  listener.streams.emplace_back(std::move(server));

  auto const ask = [](io::memory_stream stream) -> task<std::string> {
    http::request request;

    request.target = "/retries";
    request.fields.add("Host", "x");
    (void)co_await http::write(stream, request);
    co_return co_await quoll::test::read_until_closed(stream);
  };
  auto const stopping = [&source]() -> task<> {
    co_await quoll::async::sleep_for(std::chrono::seconds{1});
    source.request_stop();
  };

  quoll::async::co_spawn(
      time.get_executor(), quoll::http::server{router}.serve(std::move(listener)),
      overloaded{[&served] { served = true; }, [](std::exception_ptr) {}}, source.get_token()
  );
  quoll::async::co_spawn(
      time.get_executor(), ask(std::move(client)), overloaded{[](std::string) {}, [](std::exception_ptr) {}}
  );
  quoll::async::co_spawn(time.get_executor(), stopping(), overloaded{[] {}, [](std::exception_ptr) {}});
  time.run();

  // serve() ends as it does on a stop, its connection joined, with the handler gone through once, its catch unseen.
  REQUIRE(served);
  REQUIRE(turns == 1);
  REQUIRE(caught == 0);
}

TEST_CASE("serve: with callbacks, nothing is reported", "[server]") {
  auto router = make_router();
  quoll::async::run_loop loop;
  auto [client, server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  bool done = false;
  auto told = 0;
  quoll::http::serve_options options;

  router.add(verb::get, "/fault/throws", "throws", [](http::request, http::route_match) -> task<http::response> {
    throw std::runtime_error{"a bug of the handler"};
  });
  options.on_connection_error = [&](std::exception_ptr) { ++told; };
  options.connection.on_handler_error = [&](std::exception_ptr) { ++told; };
  listener.streams.emplace_back(broken_stream{});
  listener.streams.emplace_back(std::move(server));
  quoll::async::co_spawn(
      loop.get_executor(), quoll::http::server{router, std::move(options)}.serve(std::move(listener)),
      overloaded{[] {}, [](std::exception_ptr) {}}, source.get_token()
  );
  quoll::async::co_spawn(
      loop.get_executor(), get(std::move(client), "/fault/throws", done),
      overloaded{[&](std::string) { source.request_stop(); }, [](std::exception_ptr) {}}
  );
  loop.run();

  REQUIRE(done);
  REQUIRE(told == 2);
  REQUIRE(quoll::test::reports().empty());
}

TEST_CASE("serve: with no callback, accepts tried again are reported once, until one goes through", "[server][timer]") {
  auto const router = make_router();
  quoll::test::virtual_time time;
  canned_listener listener;
  std::stop_source source;
  bool done = false;

  // Twice out of descriptors, a connection, then out of them again: one report a streak.
  auto [client, server] = io::make_memory_stream_pair();

  listener.failures = {
      std::make_error_code(std::errc::too_many_files_open), std::make_error_code(std::errc::too_many_files_open)
  };
  listener.streams.emplace_back(std::move(server));

  struct then_failing {
    canned_listener *inner;
    bool failed_after = false;

    task<io::any_stream> accept() {
      if (inner->failures.empty() && inner->streams.empty() && !failed_after) {
        failed_after = true;
        throw std::system_error{std::make_error_code(std::errc::too_many_files_open), "then_failing"};
      }

      co_return co_await inner->accept();
    }
  };

  quoll::async::co_spawn(
      time.get_executor(), quoll::http::server{router}.serve(then_failing{&listener}),
      overloaded{[] {}, [](std::exception_ptr) {}}, source.get_token()
  );
  quoll::async::co_spawn(
      time.get_executor(), get(std::move(client), "/through", done),
      overloaded{[&](std::string) {}, [](std::exception_ptr) {}}
  );
  quoll::async::co_spawn(
      time.get_executor(),
      [](std::stop_source &stopping) -> task<> {
        co_await quoll::async::sleep_for(std::chrono::seconds{1});
        stopping.request_stop();
      }(source),
      overloaded{[] {}, [](std::exception_ptr) {}}
  );
  time.run();

  REQUIRE(done);

  auto const reported = quoll::test::reports();
  auto const retried = std::ranges::count_if(reported, [](std::string const &line) {
    return line.starts_with("quoll: cannot accept connections for want of resources, trying again every 100 ms: ");
  });

  REQUIRE(retried == 2);
  REQUIRE(reported.size() == 2);
}

TEST_CASE("serve: completes once its connections have ended, and rethrows an accept failure then", "[server]") {
  auto const router = make_router();
  quoll::async::run_loop loop;
  auto [client, server] = io::make_memory_stream_pair();
  one_then_broken listener{io::any_stream{std::move(server)}};
  bool answered = false;
  bool failed_after_answer = false;
  std::exception_ptr failure;
  std::optional<std::string> body;

  // The accept failure comes at once, while the connection is still to be served: serve() waits for it.
  quoll::async::co_spawn(
      loop.get_executor(), quoll::http::server{router}.serve(std::move(listener)),
      overloaded{
          [] {},
          [&](std::exception_ptr error) {
            failure = std::move(error);
            failed_after_answer = answered;
          }
      }
  );
  quoll::async::co_spawn(loop.get_executor(), get(std::move(client), "/served", answered), store(body));
  loop.run();

  REQUIRE(body == "served");
  REQUIRE(failure);
  REQUIRE(failed_after_answer);
}

TEST_CASE("serve: a failure of its accept loop stops the connections, then reaches the awaiter", "[server]") {
  auto const router = make_router();
  quoll::async::run_loop loop;
  one_then_broken listener{io::any_stream{idle_stream{}}};
  std::exception_ptr failure;

  // The connection waits for a request that never comes: only a stop request ends it.
  quoll::async::co_spawn(
      loop.get_executor(), quoll::http::server{router}.serve(std::move(listener)),
      overloaded{[] {}, [&](std::exception_ptr error) { failure = std::move(error); }}
  );
  loop.run();

  REQUIRE(failure);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::system_error);
}

TEST_CASE("serve: an exception from on_accept_error stops the connections, then reaches the awaiter", "[server]") {
  auto const router = make_router();
  quoll::async::run_loop loop;
  // A failure of one connection, which on_accept_error is asked about, after an idle connection.
  one_then_broken listener{
      io::any_stream{idle_stream{}},
      std::make_exception_ptr(std::system_error{std::make_error_code(std::errc::connection_reset), "reset"})
  };
  quoll::http::serve_options options;
  std::exception_ptr failure;

  options.on_accept_error = [](std::error_code) -> task<bool> {
    throw std::runtime_error{"on_accept_error"};
    co_return false;
  };
  quoll::async::co_spawn(
      loop.get_executor(), quoll::http::server{router, std::move(options)}.serve(std::move(listener)),
      overloaded{[] {}, [&](std::exception_ptr error) { failure = std::move(error); }}
  );
  loop.run();

  REQUIRE(failure);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::runtime_error);
}

TEST_CASE("serve: an accept that throws something else stops the connections, then reaches the awaiter", "[server]") {
  auto const router = make_router();
  quoll::async::run_loop loop;
  one_then_broken listener{io::any_stream{idle_stream{}}, std::make_exception_ptr(std::logic_error{"accept"})};
  std::exception_ptr failure;

  quoll::async::co_spawn(
      loop.get_executor(), quoll::http::server{router}.serve(std::move(listener)),
      overloaded{[] {}, [&](std::exception_ptr error) { failure = std::move(error); }}
  );
  loop.run();

  REQUIRE(failure);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::logic_error);
}

TEST_CASE("http::server: max_connections and connections() count for all it serves, and its copies", "[server]") {
  using namespace std::chrono_literals;

  auto const router = slow_router(1s);
  quoll::test::virtual_time time;
  auto [kept_client, kept_server] = io::make_memory_stream_pair();
  auto [refused_client, refused_server] = io::make_memory_stream_pair();
  canned_listener first;
  canned_listener second;
  std::stop_source source;
  std::optional<std::optional<http::response>> kept;
  std::optional<std::optional<http::response>> refused;
  std::size_t during = 0;

  first.streams.emplace_back(std::move(kept_server));
  second.streams.emplace_back(std::move(refused_server));

  http::server const server{router, {.max_connections = 1}};
  http::server const copy = server;

  auto const observe = [&]() -> task<> {
    co_await quoll::async::sleep_for(500ms);
    during = copy.connections();
  };

  quoll::async::co_spawn(time.get_executor(), server.serve(std::move(first)), ignored(), source.get_token());
  quoll::async::co_spawn(time.get_executor(), copy.serve(std::move(second)), ignored(), source.get_token());
  quoll::async::co_spawn(time.get_executor(), ask(kept_client), store(kept));
  quoll::async::co_spawn(time.get_executor(), ask(refused_client), store(refused));
  quoll::async::co_spawn(time.get_executor(), observe(), ignored());
  quoll::async::co_spawn(time.get_executor(), stop_after(2s, source), ignored());
  time.run();

  REQUIRE(kept.value().value().body == "done");
  REQUIRE_FALSE(refused.value().has_value());
  REQUIRE(during == 1);
  REQUIRE(server.connections() == 0);
}

TEST_CASE("http::server: routes are closed once it serves, not before", "[server]") {
  using namespace std::chrono_literals;

  auto const handler = []() { return http::response{}; };

  http::server server;

  server.get("/open", handler);

  http::server copy = server;
  auto pending = server.serve(canned_listener{});

  // A task made is no task running.
  server.get("/still-open", handler);

  quoll::test::virtual_time time;
  std::stop_source source;
  bool closed = false;

  auto const check = [&]() -> task<> {
    co_await quoll::async::sleep_for(1s);
    REQUIRE_THROWS_AS(server.get("/late", handler), std::logic_error);
    REQUIRE_THROWS_AS(copy.post("/late", handler), std::logic_error);
    REQUIRE_THROWS_AS(
        copy.use([](http::request request, http::next const &next) { return next(std::move(request)); }),
        std::logic_error
    );
    REQUIRE_THROWS_AS(copy.edit_routes([](http::router &) {}), std::logic_error);
    REQUIRE_NOTHROW((void)std::as_const(copy).routes());
    closed = true;
    source.request_stop();
  };

  quoll::async::co_spawn(time.get_executor(), std::move(pending), ignored(), source.get_token());
  quoll::async::co_spawn(time.get_executor(), check(), ignored());
  time.run();

  REQUIRE(closed);
}

TEST_CASE("http::server: a stream given directly takes a place, and is never refused", "[server]") {
  using namespace std::chrono_literals;

  auto const router = slow_router(1s);
  quoll::test::virtual_time time;
  auto [direct_client, direct_server] = io::make_memory_stream_pair();
  auto [second_client, second_server] = io::make_memory_stream_pair();
  auto [refused_client, refused_server] = io::make_memory_stream_pair();
  canned_listener listener;
  std::stop_source source;
  std::optional<std::optional<http::response>> direct;
  std::optional<std::optional<http::response>> second;
  std::optional<std::optional<http::response>> refused;
  std::size_t most = 0;

  listener.streams.emplace_back(std::move(refused_server));

  http::server const server{router, {.max_connections = 1}};

  auto const observe = [&]() -> task<> {
    co_await quoll::async::sleep_for(500ms);
    most = server.connections();
  };

  auto const serve_later = [&server](canned_listener given) -> task<> {
    co_await quoll::async::sleep_for(50ms);
    co_await server.serve(std::move(given));
  };

  quoll::async::co_spawn(time.get_executor(), server.serve_connection(std::move(direct_server)), ignored());
  quoll::async::co_spawn(time.get_executor(), server.serve_connection(std::move(second_server)), ignored());
  quoll::async::co_spawn(time.get_executor(), ask(direct_client), store(direct));
  quoll::async::co_spawn(time.get_executor(), ask(second_client), store(second));
  quoll::async::co_spawn(time.get_executor(), observe(), ignored());
  quoll::async::co_spawn(time.get_executor(), stop_after(100ms, source), ignored());
  quoll::async::co_spawn(time.get_executor(), serve_later(std::move(listener)), ignored(), source.get_token());
  quoll::async::co_spawn(time.get_executor(), ask(refused_client), store(refused));
  time.run();

  REQUIRE(direct.value().value().body == "done");
  REQUIRE(second.value().value().body == "done");
  REQUIRE_FALSE(refused.value().has_value());
  REQUIRE(most == 2);
}

namespace {

// A listener that says when it is gone.
struct watched_listener {
  std::shared_ptr<bool> closed;
  std::deque<io::any_stream> streams;

  struct closing {
    std::shared_ptr<bool> flag;

    closing(closing const &) = delete;
    closing &operator=(closing const &) = delete;
    closing(closing &&) = default;
    closing &operator=(closing &&) = default;

    explicit closing(std::shared_ptr<bool> watched) : flag(std::move(watched)) {}

    ~closing() {
      if (flag) {
        *flag = true;
      }
    }
  } goes_with_it;

  task<io::any_stream> accept() {
    if (!streams.empty()) {
      auto stream = std::move(streams.front());

      streams.pop_front();
      co_return stream;
    }

    co_await wait_for_stop{};
    throw std::system_error{std::make_error_code(std::errc::operation_canceled), "watched_listener"};
  }
};

static_assert(io::Listener<watched_listener>);

} // namespace

TEST_CASE("serve: a stop closes the listener at once, with a request still under way", "[server][timer]") {
  using namespace std::chrono_literals;

  quoll::test::virtual_time time;
  http::router router;
  auto answered = false;

  router.get("/slow", [&answered](http::request) -> task<http::response> {
    co_await quoll::async::sleep_for(10s);
    answered = true;
    co_return http::response{};
  });

  auto closed = std::make_shared<bool>(false);
  auto [client, ours] = io::make_memory_stream_pair();
  watched_listener listener{closed, {}, watched_listener::closing{closed}};

  listener.streams.emplace_back(std::move(ours));

  std::stop_source stop;
  std::optional<bool> closed_while_under_way;

  auto const stopping = [&]() -> task<int> {
    co_await quoll::async::sleep_for(1s);
    stop.request_stop();
    co_await quoll::async::sleep_for(1s);
    closed_while_under_way = *closed && !answered;
    co_return 1;
  };

  std::string const sent = "GET /slow HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";

  auto const peer = [&sent](io::memory_stream to) -> task<int> {
    (void)co_await io::write(to, std::as_bytes(std::span{sent}));

    std::array<char, 256> chunk;

    while (true) {
      auto const read = co_await to.read_some(std::as_writable_bytes(std::span{chunk}));

      if (read.error || read.transferred == 0) {
        co_return 1;
      }
    }
  };

  quoll::async::co_spawn(
      time.get_executor(), http::server{router}.serve(std::move(listener)),
      overloaded{[] {}, [](std::exception_ptr) {}}, stop.get_token()
  );
  quoll::async::co_spawn(time.get_executor(), stopping(), overloaded{[](int) {}, [](std::exception_ptr) {}});
  quoll::async::co_spawn(
      time.get_executor(), peer(std::move(client)), overloaded{[](int) {}, [](std::exception_ptr) {}}
  );
  time.run();

  REQUIRE(closed_while_under_way == true);
  REQUIRE(answered);
}

TEST_CASE("serve: what the network says of one connection does not end a server, nor spin", "[server][timer]") {
  using namespace std::chrono_literals;

  auto const failure = GENERATE(
      std::errc::network_down, std::errc::network_unreachable, std::errc::host_unreachable, std::errc::protocol_error,
      std::errc::operation_not_permitted
  );

  quoll::test::virtual_time time;
  http::router router;
  std::stop_source stop;
  canned_listener listener;
  auto [client, ours] = io::make_memory_stream_pair();
  std::optional<std::string> received;
  bool done = false;
  std::chrono::nanoseconds received_at{};

  router.get("/after", [](http::request) {
    http::response answer;

    answer.body = "after";
    return answer;
  });

  listener.failures = {std::make_error_code(failure)};
  listener.streams.emplace_back(std::move(ours));
  quoll::async::co_spawn(
      time.get_executor(), http::server{router}.serve(std::move(listener)),
      overloaded{[] {}, [](std::exception_ptr) {}}, stop.get_token()
  );
  quoll::async::co_spawn(
      time.get_executor(), get(std::move(client), "/after", done),
      overloaded{
          [&](std::string answer) {
            received = std::move(answer);
            received_at = time.get_executor().now();
            stop.request_stop();
          },
          [](std::exception_ptr) {}
      }
  );
  time.run();

  REQUIRE(received == "after");
  // It waited before it tried again.
  REQUIRE(received_at >= 100ms);
}

TEST_CASE("serve: a stop during the wait of on_accept_error is no failure", "[server][timer]") {
  using namespace std::chrono_literals;

  quoll::test::virtual_time time;
  http::router router;
  std::stop_source stop;
  canned_listener listener;
  quoll::http::serve_options options;
  std::exception_ptr failure;
  auto ended = false;

  listener.failures = {std::make_error_code(std::errc::too_many_files_open)};
  options.on_accept_error = [](std::error_code) -> task<bool> {
    co_await quoll::async::sleep_for(1h);
    co_return true;
  };

  auto const stopping = [&stop]() -> task<int> {
    co_await quoll::async::sleep_for(1s);
    stop.request_stop();
    co_return 1;
  };

  quoll::async::co_spawn(
      time.get_executor(), http::server{router, std::move(options)}.serve(std::move(listener)),
      overloaded{[&ended] { ended = true; }, [&failure](std::exception_ptr error) { failure = error; }},
      stop.get_token()
  );
  quoll::async::co_spawn(time.get_executor(), stopping(), overloaded{[](int) {}, [](std::exception_ptr) {}});
  time.run();

  REQUIRE(ended);
  REQUIRE_FALSE(failure);
}
