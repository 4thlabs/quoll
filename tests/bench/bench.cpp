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

// What Quoll costs, for docs/BENCHMARKS.md:
//
//   quoll_bench tcp [seconds]        requests per second, latency and allocations per request, over loopback TCP
//   quoll_bench allocations [--check] allocations per request over a memory stream, the same on every run
//   quoll_bench serve <port>         the routes alone until Ctrl-C, for a load generator of another project
//
// Build it with -DCMAKE_BUILD_TYPE=Release -DQUOLL_BENCH=ON -DQUOLL_BUILD_TESTS=OFF.

#include <quoll/async.hpp>
#include <quoll/async/lifetimebound.hpp>
#include <quoll/client.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/connector.hpp>
#include <quoll/io/listen.hpp>
#include <quoll/io/listen_tls.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/tls_network_ref.hpp>
#include <quoll/rest.hpp>
#include <quoll/runtime.hpp>
#include <quoll/server.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <future>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

// What the client's and the server's threads allocate goes to the counter of their role. OpenSSL and the C library
// allocate with malloc, uncounted, and so does the thread on which asio resolves a name, which has no role.
// On lines of their own, so that counting does not slow down what it counts: their two threads would fight for one.
alignas(64) std::atomic<std::uint64_t> server_allocations{0};
alignas(64) std::atomic<std::uint64_t> client_allocations{0};
constinit thread_local std::atomic<std::uint64_t> *allocation_counter = nullptr;

void *counted_allocation(std::size_t size, std::size_t alignment = 0) {
  if (allocation_counter != nullptr) {
    allocation_counter->fetch_add(1, std::memory_order_relaxed);
  }

  auto *const memory =
      alignment == 0
          ? std::malloc(std::max<std::size_t>(size, 1))
          : std::aligned_alloc(alignment, (std::max(size, alignment) + alignment - 1) / alignment * alignment);

  if (memory == nullptr) {
    throw std::bad_alloc{};
  }

  return memory;
}

} // namespace

void *operator new(std::size_t size) { return counted_allocation(size); }

void *operator new(std::size_t size, std::align_val_t alignment) {
  return counted_allocation(size, static_cast<std::size_t>(alignment));
}

void operator delete(void *memory) noexcept { std::free(memory); }

void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

void operator delete(void *memory, std::align_val_t) noexcept { std::free(memory); }

void operator delete(void *memory, std::size_t, std::align_val_t) noexcept { std::free(memory); }

// Named: Glaze reads the names of its fields through something a type in an unnamed namespace cannot have with Clang.
namespace quoll_bench {

// A small resource, as a typical REST call carries one.
struct card {
  std::int64_t id = 0;
  std::string name;
  std::vector<std::string> tags;
};

} // namespace quoll_bench

namespace {

using quoll_bench::card;

namespace async = quoll::async;
namespace client = quoll::client;
namespace http = quoll::http;
namespace io = quoll::io;
namespace rest = quoll::rest;
namespace server = quoll::server;

using async::task;
using std::chrono::steady_clock;

template <class... Arguments>
void print_line(std::format_string<Arguments...> format, Arguments &&...arguments) {
  std::cout << std::format(format, std::forward<Arguments>(arguments)...) << '\n';
}

card make_card() { return {42, "Quoll", {"marsupial", "carnivore", "nocturnal"}}; }

using get_card = rest::endpoint<"card.get", http::verb::get, "/card/{id:int64}", rest::returns<card>>;
using echo_card = rest::endpoint<"card.echo", http::verb::post, "/card", rest::body<card>, rest::returns<card>>;
using card_api = rest::api<"/api", get_card, echo_card>;

struct card_service {
  card operator()(get_card, std::int64_t) const { return make_card(); }

  card operator()(echo_card, card value) const { return value; }
};

http::router make_router() {
  http::router router;

  router.get("/hello", [](http::request) -> task<http::response> {
    http::response response;

    response.body = "Hello, world!";
    co_return response;
  });
  router.post("/echo", [](http::request request) -> task<http::response> {
    http::response response;

    response.body = std::move(request.body);
    co_return response;
  });
  rest::mount<card_api>(router, card_service{});
  return router;
}

http::request make_get(bool closing = false) {
  http::request request;

  request.target = "/hello";

  if (closing) {
    request.fields.add("Connection", "close");
  }

  return request;
}

http::request make_post(std::string const &body) {
  http::request request;

  request.method = http::verb::post;
  request.target = "/echo";
  request.body = body;
  return request;
}

// The bodies of the large requests.
std::string const &megabyte() {
  static std::string const body(std::size_t{1024} * 1024, 'x');

  return body;
}

// The server of the measures, on a thread and a loop of its own, so that it does not share a CPU with the client.
class server_thread {
public:
  server_thread() : thread_([this](std::stop_token stop) { run(std::move(stop)); }) {
    plain_port_ = plain_.get_future().get();
    secure_port_ = secure_.get_future().get();
  }

  server_thread(server_thread const &) = delete;
  server_thread &operator=(server_thread const &) = delete;
  server_thread(server_thread &&) = delete;
  server_thread &operator=(server_thread &&) = delete;
  ~server_thread() = default;

  [[nodiscard]] std::uint16_t plain_port() const noexcept { return plain_port_; }

  // 0 without TLS in this build.
  [[nodiscard]] std::uint16_t secure_port() const noexcept { return secure_port_; }

private:
  task<> serve() {
    auto const router = make_router();
    auto listener = co_await io::listen(0);
    std::optional<io::any_listener> tls_listener;

    // Asked rather than caught: anything else that fails to set TLS up is a failure, and ends the program.
    if ((co_await async::current_executor()).query<io::tls_network_ref>()) {
      tls_listener =
          co_await io::listen_tls(0, {.certificate_file = std::string{QUOLL_BENCH_TLS_DIR} + "/server.pem",
                                      .private_key_file = std::string{QUOLL_BENCH_TLS_DIR} + "/server-key.pem"});
    } else {
      std::cerr << "no TLS in this build\n";
    }

    secure_.set_value(tls_listener ? tls_listener->local_port().value() : std::uint16_t{0});
    plain_.set_value(listener.local_port().value());

    if (tls_listener) {
      co_await async::when_all(server::serve(listener, router), server::serve(*tls_listener, router));
    } else {
      co_await server::serve(listener, router);
    }
  }

  void run(std::stop_token stop) {
    allocation_counter = &server_allocations;

    quoll::runtime runtime;

    co_spawn(runtime.get_executor(), serve(), async::detached, std::move(stop));
    runtime.run();
  }

  std::promise<std::uint16_t> plain_;
  std::promise<std::uint16_t> secure_;
  std::uint16_t plain_port_ = 0;
  std::uint16_t secure_port_ = 0;
  // Last: its thread uses the rest.
  std::jthread thread_;
};

// What a scenario measured, between the end of its warm-up and its end. Only the client's thread touches it.
struct measure {
  steady_clock::time_point start;
  steady_clock::time_point stop;
  std::vector<std::chrono::nanoseconds> latencies{};
  std::optional<steady_clock::time_point> began{};
  std::optional<steady_clock::time_point> ended{};
  std::uint64_t server_begin = 0;
  std::uint64_t client_begin = 0;
  std::uint64_t server_end = 0;
  std::uint64_t client_end = 0;
  std::size_t requests = 0;
  std::string failure{};

  void begin(steady_clock::time_point now) {
    began = now;
    server_begin = server_allocations.load(std::memory_order_relaxed);
    client_begin = client_allocations.load(std::memory_order_relaxed);
  }

  void end(steady_clock::time_point now) {
    ended = now;
    server_end = server_allocations.load(std::memory_order_relaxed);
    client_end = client_allocations.load(std::memory_order_relaxed);
  }
};

// A failure must not pass for a cheap request. A REST call throws on its own.
void check(http::response const &response) {
  if (response.status != http::status::ok) {
    throw std::runtime_error{"status " + std::to_string(static_cast<int>(response.status))};
  }
}

void check(card const &) noexcept {}

// Sends one request after the other until the scenario ends, as `call` makes them.
template <class Call>
task<> send_in_turn(measure &measured QUOLL_LIFETIMEBOUND, Call call) {
  try {
    for (;;) {
      auto const before = steady_clock::now();

      if (before >= measured.stop) {
        if (!measured.ended) {
          measured.end(before);
        }

        co_return;
      }

      if (before >= measured.start && !measured.began) {
        measured.begin(before);
      }

      check(co_await call());

      if (measured.began && !measured.ended) {
        ++measured.requests;

        // Past what was kept, the percentiles are those of the first requests.
        if (measured.latencies.size() < measured.latencies.capacity()) {
          measured.latencies.push_back(steady_clock::now() - before);
        }
      }
    }
  } catch (std::exception const &failure) {
    measured.failure = failure.what();
    measured.stop = steady_clock::now();
  }
}

// What a client does lives on the loop: its transport and the connections it keeps end with the tasks, before the
// runtime.
template <class MakeCall>
task<> start_tasks(quoll::runtime::executor executor, measure &measured QUOLL_LIFETIMEBOUND, int tasks,
                   MakeCall make_call) {
  auto const call = make_call();

  for (auto index = 0; index < tasks; ++index) {
    co_spawn(executor, send_in_turn(measured, call), async::detached);
  }

  co_return;
}

// Runs `tasks` tasks of the call `make_call` makes on the client's loop for `seconds`, after a warm-up, and prints a
// row of the table.
template <class MakeCall>
bool run_scenario(std::string_view name, std::chrono::duration<double> seconds, int tasks, MakeCall make_call) {
  constexpr std::chrono::milliseconds warm_up{500};
  constexpr std::size_t most_kept = 4'000'000;

  auto const now = steady_clock::now();
  measure measured{.start = now + warm_up,
                   .stop = now + warm_up + std::chrono::duration_cast<steady_clock::duration>(seconds)};

  measured.latencies.reserve(most_kept);

  {
    quoll::runtime runtime;

    // Not kept in a variable: run() ends once no executor of the runtime is left.
    co_spawn(runtime.get_executor(), start_tasks(runtime.get_executor(), measured, tasks, std::move(make_call)),
             async::detached);
    runtime.run();
  }

  if (!measured.failure.empty() || !measured.began || !measured.ended || measured.requests == 0) {
    print_line("| {} | failed: {} | | | | |", name, measured.failure.empty() ? "no request" : measured.failure);
    return false;
  }

  auto kept = std::move(measured.latencies);

  std::ranges::sort(kept);

  auto const requests = static_cast<double>(measured.requests);
  auto const percentile = [&](double fraction) {
    auto const at = std::min(kept.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(kept.size())));

    return std::chrono::duration<double, std::micro>(kept[at]).count();
  };
  auto const elapsed = std::chrono::duration<double>(*measured.ended - *measured.began).count();

  print_line("| {} | {:.0f} | {:.0f} | {:.0f} | {:.1f} | {:.1f} |", name, requests / elapsed, percentile(0.50),
             percentile(0.99), static_cast<double>(measured.server_end - measured.server_begin) / requests,
             static_cast<double>(measured.client_end - measured.client_begin) / requests);
  return true;
}

int run_tcp(std::chrono::duration<double> seconds) {
  allocation_counter = &client_allocations;

  server_thread const server;
  auto const plain = "http://127.0.0.1:" + std::to_string(server.plain_port());
  auto const secure = "https://127.0.0.1:" + std::to_string(server.secure_port());
  auto ok = true;

  print_line("| Scenario | Requests/s | p50 (µs) | p99 (µs) | Allocations, server | Allocations, client |");
  print_line("| --- | --- | --- | --- | --- | --- |");

  auto const get = [](std::string url, bool closing = false, client::transport_options options = {}) {
    return [url = std::move(url), closing, options = std::move(options)] {
      return [transport = client::transport<>{url, options}, closing] { return transport.send(make_get(closing)); };
    };
  };

  ok = run_scenario("GET, one connection", seconds, 1, get(plain)) && ok;
  ok = run_scenario("GET, 16 at once", seconds, 16, get(plain)) && ok;
  ok = run_scenario("GET, one connection each", seconds, 1, get(plain, true)) && ok;
  ok = run_scenario("POST 1 MiB, echoed", seconds, 1,
                    [&plain] {
                      return [transport = client::transport<>{plain}] { return transport.send(make_post(megabyte())); };
                    }) &&
       ok;
  ok = run_scenario("REST GET, JSON", seconds, 1,
                    [&plain] { return [api = rest::client<card_api>{plain}] { return api.call<get_card>(42); }; }) &&
       ok;
  ok = run_scenario(
           "REST POST, JSON", seconds, 1,
           [&plain] { return [api = rest::client<card_api>{plain}] { return api.call<echo_card>(make_card()); }; }) &&
       ok;

  if (server.secure_port() != 0) {
    client::transport_options options;

    options.tls.ca_file = std::string{QUOLL_BENCH_TLS_DIR} + "/ca.pem";

    ok = run_scenario("GET over TLS, one connection", seconds, 1, get(secure, false, options)) && ok;
    ok = run_scenario("GET over TLS, one connection each", seconds, 1, get(secure, true, options)) && ok;
  }

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

// A connection to `router`, served on the same thread, over a memory stream.
class memory_connector {
public:
  memory_connector(quoll::test::virtual_time &time, http::router const &router) noexcept
      : time_(&time), router_(&router) {}

  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    auto [client_end, server_end] = io::make_memory_stream_pair();

    co_spawn(time_->get_executor(), server::serve_connection(std::move(server_end), *router_), async::detached);
    co_return io::any_stream{std::move(client_end)};
  }

private:
  quoll::test::virtual_time *time_;
  http::router const *router_;
};

static_assert(io::Connector<memory_connector>);

// The allocations of a call, once what the first ones set up is there. The call and what it holds end here, on the
// loop.
template <class MakeCall>
task<double> allocations_of(MakeCall make_call) {
  constexpr auto warm_up = 16;
  constexpr auto measured = 256;

  auto const call = make_call();

  for (auto index = 0; index < warm_up; ++index) {
    check(co_await call());
  }

  auto const before = client_allocations.load(std::memory_order_relaxed);

  for (auto index = 0; index < measured; ++index) {
    check(co_await call());
  }

  co_return static_cast<double>(client_allocations.load(std::memory_order_relaxed) - before) / measured;
}

struct budget {
  std::string_view scenario;
  double at_most;
};

// As measured with GCC 13 and its standard library, which any other allocates differently from: lower them when a
// change does better, and say so when one has to raise them.
constexpr budget budgets[] = {
    {"GET, one connection", 72.0}, {"GET, one connection each", 136.0}, {"POST 1 MiB, echoed", 144.0},
    {"REST GET, JSON", 89.0},      {"REST POST, JSON", 145.0},
};

int run_allocations(bool check) {
  allocation_counter = &client_allocations;

  auto const router = make_router();
  auto ok = true;

  auto const report = [&](std::string_view scenario, double allocations) {
    auto const found = std::ranges::find(budgets, scenario, &budget::scenario);

    if (check && (found == std::end(budgets) || allocations > found->at_most)) {
      print_line("{}: {:.2f} allocations per request, over its budget of {:.2f}", scenario, allocations,
                 found == std::end(budgets) ? 0.0 : found->at_most);
      ok = false;
      return;
    }

    print_line("| {} | {:.2f} |", scenario, allocations);
  };

  auto const measure_on = [&](std::string_view scenario, auto call) {
    quoll::test::virtual_time time;
    memory_connector const connector{time, router};

    auto const allocations = quoll::test::run(time, allocations_of([&call, connector] { return call(connector); }));

    if (!allocations) {
      print_line("{}: it never ended", scenario);
      ok = false;
      return;
    }

    report(scenario, *allocations);
  };

  auto const on_transport = [](auto send) {
    return [send = std::move(send)](memory_connector const &connector) {
      return [transport = client::transport{connector, "bench.example", 80}, send] { return send(transport); };
    };
  };

  auto const on_api = [](auto calling) {
    return [calling = std::move(calling)](memory_connector const &connector) {
      using memory_client = rest::client<card_api, client::transport<memory_connector>>;

      return [api = memory_client{client::transport{connector, "bench.example", 80}}, calling] { return calling(api); };
    };
  };

  print_line("| Scenario | Allocations per request, both sides |");
  print_line("| --- | --- |");
  measure_on("GET, one connection", on_transport([](auto const &transport) { return transport.send(make_get()); }));
  measure_on("GET, one connection each",
             on_transport([](auto const &transport) { return transport.send(make_get(true)); }));
  measure_on("POST 1 MiB, echoed",
             on_transport([](auto const &transport) { return transport.send(make_post(megabyte())); }));
  measure_on("REST GET, JSON", on_api([](auto const &api) { return api.template call<get_card>(42); }));
  measure_on("REST POST, JSON", on_api([](auto const &api) { return api.template call<echo_card>(make_card()); }));

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

task<> serve_alone(std::uint16_t port) {
  auto const router = make_router();
  auto listener = co_await io::listen(port);

  print_line("listening on 127.0.0.1:{}", listener.local_port().value());
  std::cout.flush();
  co_await server::serve(listener, router);
}

int run_serve(std::uint16_t port) {
  quoll::runtime runtime;

  co_spawn(runtime.get_executor(), serve_alone(port), async::detached, runtime.get_stop_token());
  runtime.run();
  return EXIT_SUCCESS;
}

template <class T>
bool parse(std::string_view text, T &value) {
  auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);

  return error == std::errc{} && end == text.data() + text.size();
}

int usage() {
  std::cerr << "usage: quoll_bench tcp [seconds] | allocations [--check] | serve <port>\n";
  return EXIT_FAILURE;
}

} // namespace

int main(int argc, char **argv) {
  std::vector<std::string_view> const arguments(argv + 1, argv + argc);

  if (arguments.empty()) {
    return usage();
  }

  if (arguments[0] == "tcp" && arguments.size() <= 2) {
    double seconds = 5.0;

    // Finite and of a sensible length: a huge one, or a NaN, does not convert to the ticks of the clock.
    if (arguments.size() == 2 && (!parse(arguments[1], seconds) || !(seconds > 0.0 && seconds <= 3600.0))) {
      return usage();
    }

    return run_tcp(std::chrono::duration<double>{seconds});
  }

  if (arguments[0] == "allocations" &&
      (arguments.size() == 1 || (arguments.size() == 2 && arguments[1] == "--check"))) {
    return run_allocations(arguments.size() == 2);
  }

  std::uint16_t port = 0;

  if (arguments[0] == "serve" && arguments.size() == 2 && parse(arguments[1], port)) {
    return run_serve(port);
  }

  return usage();
}
