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

// A mixed load on a server and its clients, each on a runtime of several threads, for the sanitizers to watch: it runs
// every night under ThreadSanitizer, and under AddressSanitizer with UndefinedBehaviorSanitizer (load.yml).
//
//   quoll_load [seconds] [threads] [clients]      10 s, 4 threads a side and 16 clients by default
//
// Well-formed requests get what they ask for, or the run fails. The rest (a request stopped while it runs, a peer gone
// in mid-request, a header or a body past the limits, a request that is no HTTP, a peer that stalls) only has to end,
// as the server answers or cuts it: what matters there is what a sanitizer reports, which fails the run too.
//
// Build it with -DQUOLL_LOAD=ON -DQUOLL_BUILD_TESTS=OFF and a sanitizer's flags.

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/detached.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/runtime_options.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/client/client.hpp>
#include <quoll/http/body_reader.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connect.hpp>
#include <quoll/io/listen.hpp>
#include <quoll/io/write.hpp>
#include <quoll/runtime.hpp>
#include <quoll/server/serve_options.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/overloaded.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace {

namespace async = quoll::async;
namespace http = quoll::http;
namespace io = quoll::io;

using async::task;
using namespace std::chrono_literals;
using steady_clock = std::chrono::steady_clock;

constexpr std::size_t piece_size = 4096;
constexpr int pieces = 32;

http::router make_router() {
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
  // Writes each piece back as it reads it: the connection reads and writes at once.
  router.post("/stream", [](http::body_reader body) {
    http::response response;

    response.body_writer = [body](http::body_stream out) -> task<> {
      while (auto const piece = co_await body.read()) {
        co_await out.write(*piece);
      }
    };
    return response;
  });
  router.add(http::verb::get, "/slow", "slow", [](http::request, http::route_match) -> task<http::response> {
    co_await async::sleep_for(50ms);

    http::response response;

    response.body = "slow";
    co_return response;
  });
  router.get("/throws", []() -> http::response { throw std::runtime_error{"on purpose"}; });

  return router;
}

// Short limits and timeouts: what goes past them comes often, and the server ends soon after its clients.
http::serve_options server_options(std::atomic<std::uint64_t> &connection_errors) {
  http::serve_options options;

  options.connection.limits.max_header_size = std::size_t{8} * 1024;
  options.connection.limits.max_body_size = std::size_t{256} * 1024;
  options.connection.read_timeout = 500ms;
  options.connection.idle_timeout = 2s;
  options.connection.lingering_timeout = 200ms;
  options.shutdown_timeout = 2s;
  options.on_connection_error = [&connection_errors](std::exception_ptr) { ++connection_errors; };
  return options;
}

// The server, on a runtime of its own and threads of its own.
class server_thread {
public:
  explicit server_thread(std::size_t threads)
      : threads_(threads), thread_([this](std::stop_token stop) { run(std::move(stop)); }) {
    port_ = port_promise_.get_future().get();
  }

  server_thread(server_thread const &) = delete;
  server_thread &operator=(server_thread const &) = delete;
  server_thread(server_thread &&) = delete;
  server_thread &operator=(server_thread &&) = delete;
  ~server_thread() = default;

  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  // Stops the server and waits for its connections to end, within its shutdown timeout.
  void finish() {
    thread_.request_stop();
    thread_.join();
  }

  // Read once finish() has returned.
  [[nodiscard]] std::uint64_t connection_errors() const noexcept { return connection_errors_.load(); }

private:
  task<> serve() {
    std::optional<io::any_listener> listener;

    try {
      listener.emplace(co_await io::listen(0));
    } catch (...) {
      port_promise_.set_exception(std::current_exception());
      co_return;
    }

    port_promise_.set_value(listener->local_port().value());
    co_await http::server{make_router(), server_options(connection_errors_)}.serve(std::move(*listener));
  }

  void run(std::stop_token stop) {
    quoll::runtime runtime{async::runtime_options{.threads = threads_}};

    co_spawn(runtime.get_executor(), serve(), async::detached, std::move(stop));
    runtime.run();
  }

  std::size_t threads_;
  std::promise<std::uint16_t> port_promise_;
  std::uint16_t port_ = 0;
  std::atomic<std::uint64_t> connection_errors_{0};
  // Last: its thread uses the rest.
  std::jthread thread_;
};

enum class scenario : std::uint8_t {
  get,
  expect_continue,
  stream,
  failing,
  stopped,
  cut,
  oversized_header,
  oversized_body,
  malformed,
  stalled,
};

constexpr std::array<std::string_view, 10> scenario_names{
    "GET, kept alive",       "POST, 100-continue", "streamed both ways", "handler throws", "stopped in mid-request",
    "peer gone mid-request", "header too large",   "body too large",     "no HTTP",        "peer stalls"};

// The order a client takes them in: mostly well-formed requests, as the others take longer (a peer stalling waits out
// the server's read timeout) and would otherwise fill the run.
constexpr std::array turns{scenario::get,     scenario::expect_continue,
                           scenario::stream,  scenario::get,
                           scenario::stopped, scenario::stream,
                           scenario::failing, scenario::get,
                           scenario::cut,     scenario::expect_continue,
                           scenario::get,     scenario::oversized_header,
                           scenario::stream,  scenario::get,
                           scenario::stopped, scenario::oversized_body,
                           scenario::get,     scenario::malformed,
                           scenario::stream,  scenario::stalled};

// Shared by the clients' threads.
struct tally {
  struct counts {
    std::atomic<std::uint64_t> done{0};
    std::atomic<std::uint64_t> unexpected{0};
  };

  std::array<counts, scenario_names.size()> by_scenario{};
  std::atomic<int> reported{0};

  [[nodiscard]] counts &of(scenario which) noexcept { return by_scenario.at(static_cast<std::size_t>(which)); }

  // The first few, as one failing scenario would otherwise fill the log.
  void report(scenario which, std::string_view what) {
    if (++reported <= 20) {
      std::cerr << "unexpected, " << scenario_names.at(static_cast<std::size_t>(which)) << ": " << what << '\n';
    }
  }
};

http::request make_request(http::verb method, std::string target, std::string body = {}) {
  http::request request;

  request.method = method;
  request.target = std::move(target);
  request.body = std::move(body);
  return request;
}

task<> read_until_closed(io::any_stream &stream QUOLL_LIFETIMEBOUND) {
  std::array<std::byte, 4096> buffer{};

  while (true) {
    auto const result = co_await stream.read_some(buffer);

    if (result.error || result.transferred == 0) {
      co_return;
    }
  }
}

// Sends `data` on a connection of its own, waits `pause`, then reads what comes back until the server closes, or for
// a second at most, unless `listen` is false: a peer that stops listening is part of the load too.
task<bool> send_raw(std::uint16_t port, std::string data, std::chrono::milliseconds pause, bool listen) {
  auto stream = co_await io::connect("127.0.0.1", port);

  // Whether all of it left does not matter: the server may cut the connection first.
  static_cast<void>(co_await io::write(stream, std::as_bytes(std::span{data})));

  if (pause > 0ms) {
    co_await async::sleep_for(pause);
  }

  if (!listen) {
    co_return true;
  }

  try {
    co_await async::with_timeout(1s, read_until_closed(stream));
  } catch (std::system_error const &) {
    // Timed out: the server keeps a connection it may keep.
  }

  co_return true;
}

// True when the scenario got what it should: for a well-formed request, its answer; for the others, that it ended.
task<bool> run_scenario(scenario which, http::client const &client QUOLL_LIFETIMEBOUND, std::uint16_t port) {
  switch (which) {
  case scenario::get: {
    auto const response = co_await client.send(make_request(http::verb::get, "/hello"));

    co_return response.status == http::status::ok &&response.body == "hello";
  }
  case scenario::expect_continue: {
    std::string const body(std::size_t{64} * 1024, 'e');
    auto request = make_request(http::verb::post, "/echo", body);

    request.fields.add("Expect", "100-continue");

    auto const response = co_await client.send(std::move(request));

    co_return response.status == http::status::ok &&response.body == body;
  }
  case scenario::stream: {
    auto request = make_request(http::verb::post, "/stream");

    request.body_writer = [](http::body_stream out) -> task<> {
      std::string const piece(piece_size, 's');

      for (int index = 0; index < pieces; ++index) {
        co_await out.write(piece);
      }
    };

    auto const response = co_await client.send(std::move(request));

    co_return response.status == http::status::ok &&response.body.size() == piece_size *pieces;
  }
  case scenario::failing: {
    auto const response = co_await client.send(make_request(http::verb::get, "/throws"));

    co_return response.status == http::status::internal_server_error;
  }
  case scenario::stopped: {
    try {
      auto const response = co_await async::with_timeout(10ms, client.send(make_request(http::verb::get, "/slow")));

      co_return response.status == http::status::ok;
    } catch (std::system_error const &error) {
      co_return error.code() == std::errc::timed_out;
    }
  }
  case scenario::cut:
    co_return co_await send_raw(
        port, "POST /echo HTTP/1.1\r\nHost: load\r\nContent-Length: 100000\r\n\r\n" + std::string(1000, 'c'), 0ms,
        false);
  case scenario::oversized_header:
    co_return co_await send_raw(
        port, "GET /hello HTTP/1.1\r\nHost: load\r\nX-Large: " + std::string(std::size_t{16} * 1024, 'h') + "\r\n\r\n",
        0ms, true);
  case scenario::oversized_body:
    co_return co_await send_raw(port,
                                "POST /echo HTTP/1.1\r\nHost: load\r\nContent-Length: 10485760\r\n\r\n" +
                                    std::string(std::size_t{64} * 1024, 'b'),
                                0ms, true);
  case scenario::malformed:
    co_return co_await send_raw(port, "NOT HTTP AT ALL\r\n\r\n", 0ms, true);
  case scenario::stalled:
    // Longer than the server's read timeout.
    co_return co_await send_raw(port, "GET /hello HTTP/1.1\r\nHo", 700ms, true);
  }

  co_return false;
}

// Runs the scenarios in turn, from `first`, until `deadline`, over one client kept for the well-formed requests.
task<> client_loop(std::uint16_t port, std::size_t first, steady_clock::time_point deadline,
                   tally &counted QUOLL_LIFETIMEBOUND) {
  http::client const client{"http://127.0.0.1:" + std::to_string(port)};

  for (auto turn = first; steady_clock::now() < deadline; ++turn) {
    auto const which = turns.at(turn % turns.size());
    auto &counts = counted.of(which);

    try {
      if (co_await run_scenario(which, client, port)) {
        ++counts.done;
        continue;
      }

      counted.report(which, "a wrong answer");
    } catch (std::exception const &error) {
      counted.report(which, error.what());
    }

    ++counts.unexpected;
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    auto const seconds = argc > 1 ? std::stoi(argv[1]) : 10;
    auto const threads = argc > 2 ? std::stoul(argv[2]) : 4;
    auto const clients = argc > 3 ? std::stoul(argv[3]) : 16;
    tally counted;
    std::atomic<int> client_failures{0};
    std::uint64_t connection_errors = 0;

    {
      server_thread server{threads};
      quoll::runtime runtime{async::runtime_options{.threads = threads}};
      auto const deadline = steady_clock::now() + std::chrono::seconds{seconds};

      for (std::size_t client = 0; client < clients; ++client) {
        co_spawn(runtime.get_executor(), client_loop(server.port(), client, deadline, counted),
                 quoll::test::overloaded{[] {},
                                         [&client_failures](std::exception_ptr const &error) {
                                           ++client_failures;

                                           try {
                                             std::rethrow_exception(error);
                                           } catch (std::exception const &failure) {
                                             std::cerr << "a client ended: " << failure.what() << '\n';
                                           }
                                         }});
      }

      runtime.run();
      server.finish();
      connection_errors = server.connection_errors();
    }

    bool failed = client_failures > 0;

    std::cout << std::left << std::setw(24) << "scenario" << std::right << std::setw(10) << "done" << std::setw(12)
              << "unexpected\n";

    for (std::size_t index = 0; index < scenario_names.size(); ++index) {
      auto const &counts = counted.by_scenario.at(index);

      std::cout << std::left << std::setw(24) << scenario_names.at(index) << std::right << std::setw(10)
                << counts.done.load() << std::setw(11) << counts.unexpected.load() << '\n';

      // Each scenario has to have run, and as it should.
      failed = failed || counts.done == 0 || counts.unexpected > 0;
    }

    std::cout << "connections the server saw end on an error: " << connection_errors << '\n';
    return failed ? 1 : 0;
  } catch (std::exception const &error) {
    std::cerr << "quoll_load: " << error.what() << '\n';
    return 1;
  }
}
