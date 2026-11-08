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
// Each scenario checks what it gets: a well-formed request its answer, over TCP and over TLS when the build has it; a
// request past a limit, or one that stalls, the status the server owes it. A request stopped while it runs and a peer
// gone in mid-request only have to end. A wrong answer, a scenario taking more than 10 s, a connection the server saw
// end on anything but a stream error, or a sanitizer's report fails the run.
//
// Build it with -DQUOLL_LOAD=ON -DQUOLL_BUILD_TESTS=OFF and a sanitizer's flags.

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/current_executor.hpp>
#include <quoll/async/detached.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/async/runtime_options.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/client/client.hpp>
#include <quoll/client/client_options.hpp>
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
#include <quoll/io/listen_tls.hpp>
#include <quoll/io/tls_network_ref.hpp>
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

// Each piece of a streamed body is told apart from the others, so that a piece lost, doubled or out of order shows.
std::string streamed_piece(int index) {
  auto piece = std::to_string(index) + ":";

  piece.resize(piece_size, static_cast<char>('a' + index % 26));
  return piece;
}

std::string const &streamed_body() {
  static std::string const whole = [] {
    std::string body;

    for (int index = 0; index < pieces; ++index) {
      body += streamed_piece(index);
    }

    return body;
  }();

  return whole;
}

// The first few lines that say what went wrong, each written at once, as one failure repeated would fill the log and
// threads writing piece by piece would mix theirs.
class reporter {
public:
  void operator()(std::string_view who, std::string_view what) {
    if (++reported_ <= 20) {
      std::cerr << (std::string{"unexpected, "} + std::string{who} + ": " + std::string{what} + "\n");
    }
  }

private:
  std::atomic<int> reported_{0};
};

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

// How the server's connections ended, when not as they should.
struct server_outcomes {
  // A stream error: a peer gone or reset, a handshake refused. What the abusive scenarios cause.
  std::atomic<std::uint64_t> stream_errors{0};
  // Anything else: what the load is there to find.
  std::atomic<std::uint64_t> failures{0};
};

// Short limits and timeouts: what goes past them comes often, and the server ends soon after its clients.
http::serve_options server_options(server_outcomes &outcomes, reporter &report) {
  http::serve_options options;

  options.connection.limits.max_header_size = std::size_t{8} * 1024;
  options.connection.limits.max_body_size = std::size_t{256} * 1024;
  options.connection.read_timeout = 500ms;
  options.connection.idle_timeout = 2s;
  options.connection.lingering_timeout = 200ms;
  options.shutdown_timeout = 2s;
  options.on_connection_error = [&outcomes, &report](std::exception_ptr const &error) {
    try {
      std::rethrow_exception(error);
    } catch (std::system_error const &) {
      ++outcomes.stream_errors;
    } catch (std::exception const &failure) {
      ++outcomes.failures;
      report("the server", failure.what());
    } catch (...) {
      ++outcomes.failures;
      report("the server", "an exception of no standard type");
    }
  };
  return options;
}

// The server, on a runtime of its own and threads of its own: over TCP, and over TLS when the build has it.
class server_thread {
public:
  server_thread(std::size_t threads, reporter &report)
      : threads_(threads), report_(&report), thread_([this](std::stop_token stop) { run(std::move(stop)); }) {
    ports_ = ports_promise_.get_future().get();
  }

  server_thread(server_thread const &) = delete;
  server_thread &operator=(server_thread const &) = delete;
  server_thread(server_thread &&) = delete;
  server_thread &operator=(server_thread &&) = delete;
  ~server_thread() = default;

  [[nodiscard]] std::uint16_t port() const noexcept { return ports_.first; }

  // 0 without TLS in this build.
  [[nodiscard]] std::uint16_t secure_port() const noexcept { return ports_.second; }

  // Stops the server and waits for its connections to end, within its shutdown timeout.
  void finish() {
    thread_.request_stop();
    thread_.join();
  }

  // Read once finish() has returned.
  [[nodiscard]] server_outcomes const &outcomes() const noexcept { return outcomes_; }

private:
  task<> serve() {
    std::optional<io::any_listener> listener;
    std::optional<io::any_listener> secure_listener;

    try {
      listener.emplace(co_await io::listen(0));

      // Asked rather than caught: anything else that fails to set TLS up is a failure.
      if ((co_await async::current_executor()).query<io::tls_network_ref>()) {
        secure_listener.emplace(
            co_await io::listen_tls(0, {.certificate_file = std::string{QUOLL_LOAD_TLS_DIR} + "/server.pem",
                                        .private_key_file = std::string{QUOLL_LOAD_TLS_DIR} + "/server-key.pem"}));
      }
    } catch (...) {
      ports_promise_.set_exception(std::current_exception());
      co_return;
    }

    ports_promise_.set_value(
        {listener->local_port().value(), secure_listener ? secure_listener->local_port().value() : std::uint16_t{0}});

    http::server const server{make_router(), server_options(outcomes_, *report_)};

    if (secure_listener) {
      co_await async::when_all(server.serve(std::move(*listener)), server.serve(std::move(*secure_listener)));
    } else {
      co_await server.serve(std::move(*listener));
    }
  }

  void run(std::stop_token stop) {
    quoll::runtime runtime{async::runtime_options{.threads = threads_}};

    co_spawn(runtime.get_executor(), serve(), async::detached, std::move(stop));
    runtime.run();
  }

  std::size_t threads_;
  reporter *report_;
  std::promise<std::pair<std::uint16_t, std::uint16_t>> ports_promise_;
  std::pair<std::uint16_t, std::uint16_t> ports_{};
  server_outcomes outcomes_;
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
  secure,
  secure_garbage,
};

constexpr std::array<std::string_view, 12> scenario_names{
    "GET, kept alive", "POST, 100-continue", "streamed both ways", "handler throws",
    "stopped mid-way", "peer gone mid-way",  "header too large",   "body too large",
    "no HTTP",         "peer stalls",        "TLS, kept alive",    "no TLS on TLS"};

static_assert(scenario_names.size() == static_cast<std::size_t>(scenario::secure_garbage) + 1,
              "quoll_load: a name for each scenario");

[[nodiscard]] constexpr bool needs_tls(scenario which) noexcept {
  return which == scenario::secure || which == scenario::secure_garbage;
}

// The order a client takes them in: mostly well-formed requests, as the others take longer (a peer stalling waits out
// the server's read timeout) and would otherwise fill the run.
constexpr std::array turns{scenario::get,     scenario::expect_continue,
                           scenario::stream,  scenario::secure,
                           scenario::stopped, scenario::stream,
                           scenario::failing, scenario::get,
                           scenario::cut,     scenario::expect_continue,
                           scenario::secure,  scenario::oversized_header,
                           scenario::stream,  scenario::get,
                           scenario::stopped, scenario::oversized_body,
                           scenario::get,     scenario::malformed,
                           scenario::secure,  scenario::stalled,
                           scenario::get,     scenario::secure_garbage};

// Shared by the clients' threads.
struct tally {
  struct counts {
    std::atomic<std::uint64_t> done{0};
    std::atomic<std::uint64_t> unexpected{0};
  };

  std::array<counts, scenario_names.size()> by_scenario{};

  [[nodiscard]] counts &of(scenario which) noexcept { return by_scenario.at(static_cast<std::size_t>(which)); }
};

// What a client talks to: the server's ports, and a client kept for each, as a program keeps its connections.
struct peers {
  http::client const &plain;
  http::client const *secure;
  std::uint16_t port;
  std::uint16_t secure_port;
};

http::request make_request(http::verb method, std::string target, std::string body = {}) {
  http::request request;

  request.method = method;
  request.target = std::move(target);
  request.body = std::move(body);
  return request;
}

// What came back on a connection of its own, and whether the server closed it within a second.
struct raw_answer {
  std::string received;
  bool closed = false;
};

// Reads until the server closes, or a read fails: a read the timeout cancelled says it kept the connection, which
// quoll::test::read_until_closed would not tell from a close.
task<raw_answer> read_answer(io::any_stream &stream QUOLL_LIFETIMEBOUND) {
  raw_answer answer;
  std::array<char, 256> chunk{};

  while (true) {
    auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

    if (read.error == std::errc::operation_canceled) {
      co_return answer;
    }

    if (read.error || read.transferred == 0) {
      answer.closed = true;
      co_return answer;
    }

    answer.received.append(chunk.data(), read.transferred);
  }
}

// Sends `data` on a connection of its own, waits `pause`, then reads what comes back until the server closes, for a
// second at most, unless `listen` is false: a peer that stops listening is part of the load too.
task<raw_answer> send_raw(std::uint16_t port, std::string data, std::chrono::milliseconds pause, bool listen) {
  auto stream = co_await io::connect("127.0.0.1", port);

  // Whether all of it left does not matter: the server may answer and close first.
  static_cast<void>(co_await io::write(stream, std::as_bytes(std::span{data})));

  if (pause > 0ms) {
    co_await async::sleep_for(pause);
  }

  if (!listen) {
    co_return raw_answer{};
  }

  try {
    co_return co_await async::with_timeout(1s, read_answer(stream));
  } catch (std::system_error const &error) {
    if (error.code() != std::errc::timed_out) {
      throw;
    }
  }

  co_return raw_answer{};
}

[[nodiscard]] bool answered(raw_answer const &answer, std::string_view status) {
  return answer.received.starts_with("HTTP/1.1 " + std::string{status});
}

// True when the scenario got what it should.
task<bool> run_scenario(scenario which, peers const &to QUOLL_LIFETIMEBOUND) {
  switch (which) {
  case scenario::get: {
    auto const response = co_await to.plain.send(make_request(http::verb::get, "/hello"));

    co_return response.status == http::status::ok &&response.body == "hello";
  }
  case scenario::expect_continue: {
    std::string const body(std::size_t{64} * 1024, 'e');
    auto request = make_request(http::verb::post, "/echo", body);

    request.fields.add("Expect", "100-continue");

    auto const response = co_await to.plain.send(std::move(request));

    co_return response.status == http::status::ok &&response.body == body;
  }
  case scenario::stream: {
    auto request = make_request(http::verb::post, "/stream");

    request.body_writer = [](http::body_stream out) -> task<> {
      for (int index = 0; index < pieces; ++index) {
        co_await out.write(streamed_piece(index));
      }
    };

    auto const response = co_await to.plain.send(std::move(request));

    co_return response.status == http::status::ok &&response.body == streamed_body();
  }
  case scenario::failing: {
    auto const response = co_await to.plain.send(make_request(http::verb::get, "/throws"));

    co_return response.status == http::status::internal_server_error;
  }
  case scenario::stopped: {
    try {
      auto const response = co_await async::with_timeout(10ms, to.plain.send(make_request(http::verb::get, "/slow")));

      co_return response.status == http::status::ok;
    } catch (std::system_error const &error) {
      co_return error.code() == std::errc::timed_out;
    }
  }
  case scenario::cut: {
    auto const answer = co_await send_raw(
        to.port, "POST /echo HTTP/1.1\r\nHost: load\r\nContent-Length: 100000\r\n\r\n" + std::string(1000, 'c'), 0ms,
        false);

    co_return answer.received.empty();
  }
  case scenario::oversized_header:
    co_return answered(co_await send_raw(to.port,
                                         "GET /hello HTTP/1.1\r\nHost: load\r\nX-Large: " +
                                             std::string(std::size_t{16} * 1024, 'h') + "\r\n\r\n",
                                         0ms, true),
                       "431");
  case scenario::oversized_body:
    co_return answered(co_await send_raw(to.port,
                                         "POST /echo HTTP/1.1\r\nHost: load\r\nContent-Length: 10485760\r\n\r\n" +
                                             std::string(std::size_t{64} * 1024, 'b'),
                                         0ms, true),
                       "413");
  case scenario::malformed:
    co_return answered(co_await send_raw(to.port, "NOT HTTP AT ALL\r\n\r\n", 0ms, true), "400");
  case scenario::stalled: {
    // Longer than the server's read timeout, which answers 408 and closes.
    auto const answer = co_await send_raw(to.port, "GET /hello HTTP/1.1\r\nHo", 700ms, true);

    co_return answered(answer, "408") && answer.closed;
  }
  case scenario::secure: {
    auto const hello = co_await to.secure->send(make_request(http::verb::get, "/hello"));
    auto const echoed = co_await to.secure->send(make_request(http::verb::post, "/echo", "over TLS"));

    co_return hello.body == "hello" && echoed.body == "over TLS";
  }
  case scenario::secure_garbage:
    // Plain HTTP to the TLS port: the handshake fails, and the server closes.
    co_return (co_await send_raw(to.secure_port, "GET /hello HTTP/1.1\r\nHost: load\r\n\r\n", 0ms, true)).closed;
  }

  co_return false;
}

// Runs the scenarios in turn, from `first`, until `deadline`, each within 10 s, over clients kept for the well-formed
// requests. Without TLS in this build, its scenarios are left out.
task<> client_loop(std::uint16_t port, std::uint16_t secure_port, std::size_t first, steady_clock::time_point deadline,
                   tally &counted QUOLL_LIFETIMEBOUND, reporter &report QUOLL_LIFETIMEBOUND) {
  http::client const plain{"http://127.0.0.1:" + std::to_string(port)};
  std::optional<http::client> secure;

  if (secure_port != 0) {
    http::client_options options;

    options.tls.ca_file = std::string{QUOLL_LOAD_TLS_DIR} + "/ca.pem";
    secure.emplace("https://127.0.0.1:" + std::to_string(secure_port), std::move(options));
  }

  peers const to{plain, secure ? &*secure : nullptr, port, secure_port};

  for (auto turn = first; steady_clock::now() < deadline; ++turn) {
    auto const which = turns.at(turn % turns.size());

    if (needs_tls(which) && secure_port == 0) {
      continue;
    }

    auto &counts = counted.of(which);
    auto const name = scenario_names.at(static_cast<std::size_t>(which));

    try {
      if (co_await async::with_timeout(10s, run_scenario(which, to))) {
        ++counts.done;
        continue;
      }

      report(name, "a wrong answer");
    } catch (std::exception const &error) {
      report(name, error.what());
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
    reporter report;
    tally counted;
    std::atomic<int> client_failures{0};
    std::uint64_t stream_errors = 0;
    std::uint64_t server_failures = 0;
    bool with_tls = false;

    {
      server_thread server{threads, report};
      quoll::runtime runtime{async::runtime_options{.threads = threads}};
      auto const deadline = steady_clock::now() + std::chrono::seconds{seconds};

      with_tls = server.secure_port() != 0;

      for (std::size_t client = 0; client < clients; ++client) {
        co_spawn(runtime.get_executor(),
                 client_loop(server.port(), server.secure_port(), client, deadline, counted, report),
                 quoll::test::overloaded{[] {},
                                         [&client_failures, &report](std::exception_ptr const &error) {
                                           ++client_failures;

                                           try {
                                             std::rethrow_exception(error);
                                           } catch (std::exception const &failure) {
                                             report("a client", failure.what());
                                           } catch (...) {
                                             report("a client", "an exception of no standard type");
                                           }
                                         }});
      }

      runtime.run();
      server.finish();
      stream_errors = server.outcomes().stream_errors;
      server_failures = server.outcomes().failures;
    }

    bool failed = client_failures > 0 || server_failures > 0;

    std::cout << std::left << std::setw(24) << "scenario" << std::right << std::setw(10) << "done" << std::setw(12)
              << "unexpected\n";

    for (std::size_t index = 0; index < scenario_names.size(); ++index) {
      auto const &counts = counted.by_scenario.at(index);
      auto const left_out = needs_tls(static_cast<scenario>(index)) && !with_tls;

      std::cout << std::left << std::setw(24) << scenario_names.at(index) << std::right << std::setw(10)
                << counts.done.load() << std::setw(11) << counts.unexpected.load() << (left_out ? "  (no TLS)" : "")
                << '\n';

      // Each scenario has to have run, and as it should.
      failed = failed || (!left_out && counts.done == 0) || counts.unexpected > 0;
    }

    std::cout << "connections the server saw end on a stream error: " << stream_errors
              << ", on anything else: " << server_failures << '\n';
    return failed ? 1 : 0;
  } catch (std::exception const &error) {
    std::cerr << "quoll_load: " << error.what() << '\n';
    return 1;
  }
}
