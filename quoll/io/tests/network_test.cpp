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
#include <quoll/io/listen.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/network.hpp>
#include <quoll/io/network_connector.hpp>
#include <quoll/io/network_ref.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
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

  co_spawn(network_executor{loop, log}, use_network(),
           quoll::test::overloaded{[&](int value) { result = value; },
                                   [](std::exception_ptr error) { std::rethrow_exception(error); }});
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
