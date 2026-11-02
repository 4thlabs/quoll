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
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/yield.hpp>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <tuple>

namespace {

using quoll::async::co_spawn;
using quoll::async::current_stop_token;
using quoll::async::run_loop;
using quoll::async::task;
using quoll::async::when_all;
using quoll::async::yield;

template <class... Fs>
struct overloaded : Fs... {
  using Fs::operator()...;
};

// Bounds the loops below, so that a stop request that never arrives fails the test instead of hanging it.
constexpr int max_steps = 100;

task<> count_until_stopped(int &steps) {
  auto const stop_token = co_await current_stop_token();
  while (!stop_token.stop_requested() && steps < max_steps) {
    ++steps;
    co_await yield();
  }
}

task<int> steps_until_stopped() {
  int steps = 0;
  co_await count_until_stopped(steps);
  co_return steps;
}

task<> stop_after(std::stop_source source, int yields) {
  for (int step = 0; step < yields; ++step) {
    co_await yield();
  }
  source.request_stop();
}

task<bool> stop_possible() {
  auto const stop_token = co_await current_stop_token();
  co_return stop_token.stop_possible();
}

task<> fail_after_yield() {
  co_await yield();
  throw std::runtime_error("boom");
}

task<int> sibling_steps_after_failure() {
  int steps = 0;
  try {
    co_await when_all(fail_after_yield(), count_until_stopped(steps));
  } catch (std::runtime_error const &) {
    co_return steps;
  }
  co_return -1;
}

task<std::tuple<int, int>> two_children_until_stopped() {
  co_return co_await when_all(steps_until_stopped(), steps_until_stopped());
}

auto const ignore_success = [] {};
auto const fail_on_error = [](std::exception_ptr) { FAIL("unexpected exception"); };

} // namespace

TEST_CASE("A task spawned without stop token cannot be stopped", "[async][stop_token]") {
  run_loop loop;
  std::optional<bool> possible;

  co_spawn(loop.get_executor(), stop_possible(), overloaded{[&](bool value) { possible = value; }, fail_on_error});
  loop.run();

  REQUIRE(possible == false);
}

TEST_CASE("co_spawn hands its stop token to the task", "[async][stop_token][co_spawn]") {
  run_loop loop;
  std::stop_source source;
  std::optional<int> steps;

  co_spawn(loop.get_executor(), steps_until_stopped(), overloaded{[&](int value) { steps = value; }, fail_on_error},
           source.get_token());
  co_spawn(loop.get_executor(), stop_after(source, 3), overloaded{ignore_success, fail_on_error});
  loop.run();

  REQUIRE(steps);
  REQUIRE(*steps > 0);
  REQUIRE(*steps < max_steps);
}

TEST_CASE("when_all stops the other children when one fails", "[async][stop_token][when_all]") {
  run_loop loop;
  std::optional<int> steps;

  co_spawn(loop.get_executor(), sibling_steps_after_failure(),
           overloaded{[&](int value) { steps = value; }, fail_on_error});
  loop.run();

  REQUIRE(steps);
  REQUIRE(*steps > 0);
  REQUIRE(*steps < max_steps);
}

TEST_CASE("when_all forwards the awaiter's stop request to its children", "[async][stop_token][when_all]") {
  run_loop loop;
  std::stop_source source;
  std::optional<std::tuple<int, int>> steps;

  co_spawn(loop.get_executor(), two_children_until_stopped(),
           overloaded{[&](std::tuple<int, int> value) { steps = value; }, fail_on_error}, source.get_token());
  co_spawn(loop.get_executor(), stop_after(source, 3), overloaded{ignore_success, fail_on_error});
  loop.run();

  REQUIRE(steps);
  REQUIRE(std::get<0>(*steps) < max_steps);
  REQUIRE(std::get<1>(*steps) < max_steps);
}
