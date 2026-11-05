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

#include <quoll/async/detail/slots.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stop_token>
#include <system_error>
#include <vector>

using quoll::async::task;
using quoll::async::detail::slots;

namespace {

// Takes a place, says so, stays a while, and gives it back.
task<int> take_turn(slots &places, std::vector<int> &order, int who) {
  co_await places.acquire();
  order.push_back(who);
  co_await quoll::async::yield();
  order.push_back(-who);
  places.release();
  co_return who;
}

} // namespace

TEST_CASE("slots: what waits goes first come, first served, one place at a time", "[async][slots]") {
  slots places{1};
  std::vector<int> order;

  REQUIRE(quoll::test::run(
      quoll::async::when_all(take_turn(places, order, 1), take_turn(places, order, 2), take_turn(places, order, 3))));
  REQUIRE(order == std::vector{1, -1, 2, -2, 3, -3});
  REQUIRE(places.available() == 1);
}

TEST_CASE("slots: as many go at once as there are places", "[async][slots]") {
  slots places{2};
  std::vector<int> order;

  REQUIRE(quoll::test::run(
      quoll::async::when_all(take_turn(places, order, 1), take_turn(places, order, 2), take_turn(places, order, 3))));
  REQUIRE(order.at(0) == 1);
  REQUIRE(order.at(1) == 2);
  REQUIRE(places.available() == 2);
}

TEST_CASE("slots: a stop ends a wait, and leaves the place to the others", "[async][slots]") {
  slots places{1};
  std::stop_source stopping;
  bool stopped = false;
  std::vector<int> order;

  auto const waits_in_vain = [&]() -> task<int> {
    try {
      co_await places.acquire();
    } catch (std::system_error const &error) {
      stopped = error.code() == std::errc::operation_canceled;
    }

    co_return 0;
  };

  auto const holds_then_stops = [&]() -> task<int> {
    co_await places.acquire();
    co_await quoll::async::yield();
    stopping.request_stop();
    co_await quoll::async::yield();
    places.release();
    co_return 0;
  };

  auto const waits_until_stopped = [&]() -> task<int> {
    co_return co_await quoll::async::detail::stop_scope<int>{waits_in_vain(), stopping.get_token()};
  };

  auto const all = [&]() -> task<int> {
    co_await quoll::async::when_all(holds_then_stops(), waits_until_stopped(), take_turn(places, order, 3));
    co_return 0;
  };

  REQUIRE(quoll::test::run(all()));
  REQUIRE(stopped);
  REQUIRE(order == std::vector{3, -3});
  REQUIRE(places.available() == 1);
}

TEST_CASE("slots: stopped already, a wait does not start", "[async][slots]") {
  slots places{1};
  std::stop_source stopping;

  stopping.request_stop();

  auto const waits = [&]() -> task<int> {
    co_await places.acquire();
    co_return 1;
  };

  auto const stopped = [&]() -> task<int> {
    try {
      co_return co_await quoll::async::detail::stop_scope<int>{waits(), stopping.get_token()};
    } catch (std::system_error const &) {
      co_return 2;
    }
  };

  REQUIRE(quoll::test::run(stopped()) == 2);
  REQUIRE(places.available() == 1);
}

TEST_CASE("slots: a stop coming after a place was given leaves it to what waited", "[async][slots]") {
  slots places{1};
  std::stop_source stopping;
  bool got_it = false;

  auto const holds = [&]() -> task<int> {
    co_await places.acquire();
    co_await quoll::async::yield();
    // What waits has the place from here, but has not resumed.
    places.release();
    stopping.request_stop();
    co_return 0;
  };

  auto const waits = [&]() -> task<int> {
    co_await places.acquire();
    got_it = true;
    places.release();
    co_return 0;
  };

  auto const waits_with_a_stop = [&]() -> task<int> {
    co_return co_await quoll::async::detail::stop_scope<int>{waits(), stopping.get_token()};
  };

  REQUIRE(quoll::test::run(quoll::async::when_all(holds(), waits_with_a_stop())));
  REQUIRE(got_it);
  REQUIRE(places.available() == 1);
}
