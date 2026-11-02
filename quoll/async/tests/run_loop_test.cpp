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
#include <quoll/async/yield.hpp>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <string>
#include <vector>

namespace {

using quoll::async::co_spawn;
using quoll::async::run_loop;
using quoll::async::task;

template <class... Fs>
struct overloaded : Fs... {
  using Fs::operator()...;
};

auto const ignore_success = [] {};
auto const fail_on_error = [](std::exception_ptr) { FAIL("unexpected exception"); };

task<> append(std::vector<std::string> &log, std::string name) {
  log.push_back(std::move(name));
  co_return;
}

task<> yield_twice() {
  co_await quoll::async::yield();
  co_await quoll::async::yield();
}

} // namespace

TEST_CASE("run_loop::run on an empty loop resumes nothing", "[async][run_loop]") {
  run_loop loop;

  REQUIRE(loop.run() == 0);
}

TEST_CASE("run_loop resumes coroutines in the order they were posted", "[async][run_loop]") {
  run_loop loop;
  std::vector<std::string> log;

  for (auto const *name : {"a", "b", "c"}) {
    co_spawn(loop.get_executor(), append(log, name), overloaded{ignore_success, fail_on_error});
  }

  REQUIRE(log.empty());
  REQUIRE(loop.run() == 3);
  REQUIRE(log == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("run_loop::run also resumes what is posted while it runs", "[async][run_loop]") {
  run_loop loop;
  bool completed = false;

  co_spawn(loop.get_executor(), yield_twice(), overloaded{[&] { completed = true; }, fail_on_error});

  // The spawned root, then the task again after each yield.
  REQUIRE(loop.run() == 3);
  REQUIRE(completed);
  REQUIRE(loop.run() == 0);
}

TEST_CASE("run_loop executors are equal when they post to the same loop", "[async][run_loop]") {
  run_loop loop;
  run_loop other;

  REQUIRE(loop.get_executor() == loop.get_executor());
  REQUIRE_FALSE(loop.get_executor() == other.get_executor());
}
