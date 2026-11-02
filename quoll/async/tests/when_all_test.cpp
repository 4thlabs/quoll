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
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace {

using quoll::async::co_spawn;
using quoll::async::run_loop;
using quoll::async::task;
using quoll::async::when_all;


task<int> number(int value) {
  co_await quoll::async::yield();
  co_return value;
}

task<std::string> text() { co_return "text"; }

task<> nothing() { co_return; }

task<> fail() {
  co_await quoll::async::yield();
  throw std::runtime_error("boom");
}

task<> record(std::vector<std::string> &log, std::string name) {
  for (int step = 1; step <= 2; ++step) {
    log.push_back(name + std::to_string(step));
    co_await quoll::async::yield();
  }
}

task<> set_after_yield(bool &done) {
  co_await quoll::async::yield();
  co_await quoll::async::yield();
  done = true;
}

using quoll::test::run;

task<std::tuple<int, std::string, std::monostate>> mixed() {
  co_return co_await when_all(number(1), text(), nothing());
}

task<std::tuple<>> empty() { co_return co_await when_all(); }

task<std::tuple<std::monostate, std::monostate>> interleave(std::vector<std::string> &log) {
  co_return co_await when_all(record(log, "a"), record(log, "b"));
}

// True when when_all threw, and only after the child that did not fail had finished.
task<bool> rethrows_after_all_completed() {
  bool done = false;

  try {
    co_await when_all(fail(), set_after_yield(done));
  } catch (std::runtime_error const &) {
    co_return done;
  }

  co_return false;
}

} // namespace

TEST_CASE("when_all returns every value in order", "[async][when_all]") {
  auto const result = run(mixed());

  REQUIRE(result);
  REQUIRE(std::get<0>(*result) == 1);
  REQUIRE(std::get<1>(*result) == "text");
}

TEST_CASE("when_all of nothing completes immediately", "[async][when_all]") { REQUIRE(run(empty())); }

TEST_CASE("when_all runs its children concurrently", "[async][when_all]") {
  std::vector<std::string> log;

  REQUIRE(run(interleave(log)));
  REQUIRE(log == std::vector<std::string>{"a1", "b1", "a2", "b2"});
}

TEST_CASE("when_all rethrows after every child completed", "[async][when_all]") {
  auto const result = run(rethrows_after_all_completed());

  REQUIRE(result);
  REQUIRE(*result);
}
