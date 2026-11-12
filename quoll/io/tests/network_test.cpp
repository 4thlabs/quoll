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

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/service.hpp>
#include <quoll/async/task.hpp>
#include <quoll/io/any_listener.hpp>
#include <quoll/io/any_stream.hpp>
#include <quoll/io/bound_listener.hpp>
#include <quoll/io/connect.hpp>
#include <quoll/io/connect_tls.hpp>
#include <quoll/io/listen.hpp>
#include <quoll/io/listen_tls.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/network.hpp>
#include <quoll/io/network_connector.hpp>
#include <quoll/io/network_ref.hpp>
#include <quoll/io/tls_network.hpp>
#include <quoll/io/tls_network_ref.hpp>
#include <quoll/io/tls_options.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace async = quoll::async;
namespace io = quoll::io;

using async::task;

// What the fake network saw.
struct network_log {
  std::vector<std::string> calls;
  // The ends of the connections it opened that a server would hold.
  std::vector<io::memory_stream> peers;
};

// A listener handing out nothing: enough to be wrapped.
struct idle_listener {
  int *accepts;

  task<io::any_stream> accept() {
    ++*accepts;
    co_return io::any_stream{io::make_memory_stream_pair().first};
  }
};

// A listener bound to a port, as a TCP acceptor is.
struct bound_listener {
  int *accepts;

  task<io::any_stream> accept() {
    ++*accepts;
    co_return io::any_stream{io::make_memory_stream_pair().first};
  }

  [[nodiscard]] std::uint16_t local_port() const noexcept { return 4242; }
};

static_assert(io::BoundListener<bound_listener>);
static_assert(!io::BoundListener<idle_listener>);

struct fake_network {
  network_log *log;

  [[nodiscard]] task<io::any_stream> connect(std::string host, std::uint16_t port) const {
    log->calls.push_back("connect " + host + ":" + std::to_string(port));

    auto [ours, theirs] = io::make_memory_stream_pair();

    log->peers.push_back(std::move(theirs));
    co_return io::any_stream{std::move(ours)};
  }

  [[nodiscard]] io::any_listener listen(std::string address, std::uint16_t port) const {
    log->calls.push_back("listen " + address + ":" + std::to_string(port));
    return io::any_listener{idle_listener{&accepts}};
  }

  static inline int accepts = 0;
};

static_assert(io::Network<fake_network>);

// An executor of a run_loop offering the fake network, as an I/O loop's executor would.
class network_executor {
public:
  using services = async::service_list<io::network_ref>;

  network_executor(async::run_loop &loop, network_log &log) : loop_(loop.get_executor()), network_{&log} {}

  void post(async::work_item &item) const { loop_.post(item); }

  [[nodiscard]] io::network_ref query(async::service_tag<io::network_ref>) const { return io::network_ref{network_}; }

  [[nodiscard]] bool operator==(network_executor const &other) const noexcept {
    return loop_ == other.loop_ && network_.log == other.network_.log;
  }

private:
  async::run_loop::executor loop_;
  fake_network network_;
};

task<int> use_network() {
  auto stream = co_await io::network_connector{}.connect("example.com", 80);
  auto listener = co_await io::listen(8080);

  (void)co_await listener.accept();
  co_return stream.target<io::memory_stream>() != nullptr ? 1 : 0;
}

} // namespace

TEST_CASE("io::connect and io::listen go through the network of the task's executor", "[io][network]") {
  async::run_loop loop;
  network_log log;
  std::optional<int> result;

  co_spawn(
      network_executor{loop, log}, use_network(),
      quoll::test::overloaded{
          [&](int value) { result = value; }, [](std::exception_ptr error) { std::rethrow_exception(error); }
      }
  );
  loop.run();

  REQUIRE(result == 1);
  REQUIRE(log.calls == std::vector<std::string>{"connect example.com:80", "listen 127.0.0.1:8080"});
  REQUIRE(fake_network::accepts == 1);
}

TEST_CASE("io::connect and io::listen need an executor offering a network", "[io][network]") {
  REQUIRE_THROWS_AS(quoll::test::run(io::connect("example.com", 80)), std::logic_error);
  REQUIRE_THROWS_AS(quoll::test::run(io::listen(8080)), std::logic_error);
}

TEST_CASE("any_listener moves, and recovers the listener it wraps", "[io][network]") {
  int accepts = 0;
  io::any_listener listener{idle_listener{&accepts}};
  io::any_listener moved{std::move(listener)};

  REQUIRE(moved.target<idle_listener>() != nullptr);
  REQUIRE(moved.target<int>() == nullptr);
  // Not bound to a port.
  REQUIRE_FALSE(moved.local_port());
  REQUIRE(quoll::test::run(moved.accept()).has_value());
  REQUIRE(accepts == 1);

  listener = std::move(moved);
  REQUIRE(listener.target<idle_listener>()->accepts == &accepts);
}

TEST_CASE("network_ref refers to a network, and recovers it", "[io][network]") {
  network_log log;
  fake_network const network{&log};
  io::network_ref const ref{network};

  REQUIRE(ref.target<fake_network>() == &network);
  REQUIRE(ref.target<int>() == nullptr);
}

TEST_CASE("any_listener reports the port of a listener bound to one", "[io][network]") {
  int accepts = 0;
  io::any_listener const listener{bound_listener{&accepts}};

  REQUIRE(listener.local_port() == 4242);
}

namespace {

// Networks as the concepts allow them: a task that reads what it is given when it runs, not at the call.
struct viewing_network {
  std::string *seen;

  [[nodiscard]] task<io::any_stream> connect(std::string_view host, std::uint16_t) const {
    seen->assign(host);
    co_return io::any_stream{io::make_memory_stream_pair().first};
  }

  [[nodiscard]] io::any_listener listen(std::string, std::uint16_t) const {
    static int accepts = 0;

    return io::any_listener{idle_listener{&accepts}};
  }
};

static_assert(io::Network<viewing_network>);

struct viewing_tls_network {
  std::string *seen;

  [[nodiscard]] task<io::any_stream>
  connect_tls(std::string_view host, std::uint16_t, io::tls_client_options const &options) const {
    seen->assign(host);
    *seen += "|" + options.ca_file;
    co_return io::any_stream{io::make_memory_stream_pair().first};
  }

  [[nodiscard]] io::any_listener
  listen_tls(std::string address, std::uint16_t, io::tls_server_options const &options) const {
    seen->assign(address);
    *seen += "|" + options.certificate_file;

    static int accepts = 0;

    return io::any_listener{idle_listener{&accepts}};
  }
};

static_assert(io::TlsNetwork<viewing_tls_network>);

// An executor offering the TLS network, as an I/O loop's built with a TLS library would.
class tls_executor {
public:
  using services = async::service_list<io::tls_network_ref>;

  tls_executor(async::run_loop &loop, std::string &seen) : loop_(loop.get_executor()), network_{&seen} {}

  void post(async::work_item &item) const { loop_.post(item); }

  [[nodiscard]] io::tls_network_ref query(async::service_tag<io::tls_network_ref>) const {
    return io::tls_network_ref{network_};
  }

  [[nodiscard]] bool operator==(tls_executor const &other) const noexcept {
    return loop_ == other.loop_ && network_.seen == other.network_.seen;
  }

private:
  async::run_loop::executor loop_;
  viewing_tls_network network_;
};

struct unmovable_listener {
  unmovable_listener(unmovable_listener &&) = delete;

  task<io::any_stream> accept();
};

static_assert(io::Listener<unmovable_listener>);
static_assert(!std::is_constructible_v<io::any_listener, unmovable_listener>);

struct counted_listener {
  std::shared_ptr<int> gone;

  counted_listener(counted_listener const &) = delete;
  counted_listener &operator=(counted_listener const &) = delete;

  counted_listener(counted_listener &&other) noexcept : gone(std::move(other.gone)) {}

  counted_listener &operator=(counted_listener &&) = delete;

  explicit counted_listener(std::shared_ptr<int> counter) : gone(std::move(counter)) {}

  ~counted_listener() {
    if (gone) {
      ++*gone;
    }
  }

  task<io::any_stream> accept() { co_return io::any_stream{io::make_memory_stream_pair().first}; }
};

} // namespace

TEST_CASE("network_ref: what it is given stays until its task has run", "[io][network]") {
  std::string seen;
  viewing_network const network{&seen};
  io::network_ref const reference{network};
  // Long enough not to fit in a string's own storage, for ASan to see it go.
  std::string const host(64, 'x');
  auto connecting = reference.connect(host, 80);

  auto const connected = quoll::test::run(std::move(connecting));

  REQUIRE(connected.has_value());
  REQUIRE(seen == host);
}

TEST_CASE("tls_network_ref: what it is given stays until its task has run", "[io][network]") {
  std::string seen;
  viewing_tls_network const network{&seen};
  io::tls_network_ref const reference{network};
  std::string const host(64, 'x');
  auto connecting = reference.connect_tls(host, 443, io::tls_client_options{.ca_file = std::string(64, 'y')});

  auto const connected = quoll::test::run(std::move(connecting));

  REQUIRE(connected.has_value());
  REQUIRE(seen == host + "|" + std::string(64, 'y'));
  REQUIRE(reference.target<viewing_tls_network>() == &network);
  REQUIRE(reference.target<int>() == nullptr);
}

TEST_CASE("io::connect_tls and io::listen_tls go through the TLS network of the task's executor", "[io][network]") {
  async::run_loop loop;
  std::string seen;
  bool ran = false;

  auto const work = [&]() -> task<int> {
    (void)co_await io::network_connector{}.connect_tls("example.com", 443, {.ca_file = "ca.pem"});

    auto const connected = seen;

    (void)co_await io::listen_tls(8443, {.certificate_file = "cert.pem", .private_key_file = "key.pem"});
    ran = connected == "example.com|ca.pem" && seen == "127.0.0.1|cert.pem";
    co_return 0;
  };

  co_spawn(
      tls_executor{loop, seen}, work(),
      quoll::test::overloaded{[](int) {}, [](std::exception_ptr error) { std::rethrow_exception(error); }}
  );
  loop.run();

  REQUIRE(ran);
}

TEST_CASE("io::listen_tls refuses options with no certificate, with no network to ask", "[io][network]") {
  REQUIRE_THROWS_AS(quoll::test::run(io::listen_tls(8443, {})), std::invalid_argument);
}

TEST_CASE("io::connect_tls needs an executor offering a TLS network", "[io][network]") {
  REQUIRE_THROWS_AS(quoll::test::run(io::connect_tls("example.com", 443, {})), std::logic_error);
}

TEST_CASE("any_listener: a listener replaced is gone, and it is seen from a const one", "[io][network]") {
  auto const first = std::make_shared<int>(0);
  auto const second = std::make_shared<int>(0);

  {
    io::any_listener listener{counted_listener{first}};
    auto const &same = listener;

    REQUIRE(same.target<counted_listener>() != nullptr);
    REQUIRE(same.target<int>() == nullptr);

    listener = io::any_listener{counted_listener{second}};
    REQUIRE(*first == 1);
    REQUIRE(*second == 0);
  }

  REQUIRE(*second == 1);
}

namespace {

// A network that takes what it needs at the call, as asio's does, and counts them.
struct calling_network {
  int *calls;

  [[nodiscard]] static task<io::any_stream> connected() {
    co_return io::any_stream{io::make_memory_stream_pair().first};
  }

  [[nodiscard]] task<io::any_stream> connect(std::string, std::uint16_t) const {
    ++*calls;
    return connected();
  }

  [[nodiscard]] task<io::any_stream> connect_tls(std::string, std::uint16_t, io::tls_client_options const &) const {
    ++*calls;
    return connected();
  }

  [[nodiscard]] io::any_listener listen(std::string, std::uint16_t) const {
    static int accepts = 0;

    return io::any_listener{idle_listener{&accepts}};
  }

  [[nodiscard]] io::any_listener listen_tls(std::string, std::uint16_t, io::tls_server_options const &) const {
    static int accepts = 0;

    return io::any_listener{idle_listener{&accepts}};
  }
};

} // namespace

namespace {

// What a connection's frame reads as it goes: a network may keep a view of what it was given until then.
struct reads_when_gone {
  std::string_view host;
  std::string *seen;

  reads_when_gone(std::string_view viewed, std::string *into) : host(viewed), seen(into) {}

  reads_when_gone(reads_when_gone const &) = delete;
  reads_when_gone &operator=(reads_when_gone const &) = delete;

  reads_when_gone(reads_when_gone &&other) noexcept : host(other.host), seen(std::exchange(other.seen, nullptr)) {}

  reads_when_gone &operator=(reads_when_gone &&) = delete;

  ~reads_when_gone() {
    if (seen != nullptr) {
      seen->assign(host);
    }
  }
};

struct remembering_network {
  std::string *seen;

  [[nodiscard]] static task<io::any_stream> connected(reads_when_gone) {
    co_return io::any_stream{io::make_memory_stream_pair().first};
  }

  [[nodiscard]] task<io::any_stream> connect(std::string_view host, std::uint16_t) const {
    return connected(reads_when_gone{host, seen});
  }

  [[nodiscard]] io::any_listener listen(std::string, std::uint16_t) const {
    static int accepts = 0;

    return io::any_listener{idle_listener{&accepts}};
  }
};

static_assert(io::Network<remembering_network>);

} // namespace

TEST_CASE("network_ref: what it is given outlives the connection's frame, run or not", "[io][network]") {
  std::string const host(64, 'x');

  for (auto const run_it : {false, true}) {
    std::string seen;
    remembering_network const network{&seen};

    {
      auto connecting = io::network_ref{network}.connect(host, 80);

      if (run_it) {
        auto const connected = quoll::test::run(std::move(connecting));

        REQUIRE(connected.has_value());
      }
    }

    REQUIRE(seen == host);
  }
}

TEST_CASE(
    "network_ref and tls_network_ref: the network is used at the call, and may go before the task runs", "[io][network]"
) {
  auto calls = 0;

  auto connecting = [&] {
    calling_network const network{&calls};

    return io::network_ref{network}.connect("example.com", 80);
  }();

  REQUIRE(calls == 1);
  auto const connected = quoll::test::run(std::move(connecting));

  REQUIRE(connected.has_value());

  auto encrypting = [&] {
    calling_network const network{&calls};

    return io::tls_network_ref{network}.connect_tls("example.com", 443, {});
  }();

  REQUIRE(calls == 2);
  auto const encrypted = quoll::test::run(std::move(encrypting));

  REQUIRE(encrypted.has_value());
}
