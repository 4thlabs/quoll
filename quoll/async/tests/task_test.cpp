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
#include <quoll/async/work_item.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>

#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using quoll::async::co_spawn;
using quoll::async::run_loop;
using quoll::async::task;

using quoll::test::overloaded;

task<int> answer() { co_return 42; }

task<int> add_one(int value) { co_return value + 1; }

task<int> chain() {
  int const value = co_await answer();

  co_return co_await add_one(value);
}

task<> fail() {
  throw std::runtime_error("boom");
  co_return;
}

task<std::string> recover() {
  try {
    co_await fail();
  } catch (std::runtime_error const &error) {
    co_return error.what();
  }

  co_return "not thrown";
}

task<> record(std::vector<std::string> &log, std::string name) {
  for (int step = 1; step <= 2; ++step) {
    log.push_back(name + std::to_string(step));
    co_await quoll::async::yield();
  }
}

// The frame keeps a copy of `resource` alive.
task<> hold([[maybe_unused]] std::shared_ptr<int> resource, bool &started) {
  started = true;
  co_return;
}

// An executor whose host loop cannot take more work.
struct full_executor {
  void post(quoll::async::work_item &) const { throw std::bad_alloc{}; }

  [[nodiscard]] bool operator==(full_executor const &) const noexcept = default;
};

task<int> owns(std::shared_ptr<int> resource) { co_return *resource; }

} // namespace

TEST_CASE("A task returns the value of the tasks it awaits", "[async][task]") {
  run_loop loop;
  std::optional<int> result;

  co_spawn(loop.get_executor(), chain(),
           overloaded{[&](int value) { result = value; }, [](std::exception_ptr) { FAIL("unexpected exception"); }});

  REQUIRE_FALSE(result);
  loop.run();
  REQUIRE(result == 43);
}

TEST_CASE("An exception propagates to the awaiting task", "[async][task]") {
  run_loop loop;
  std::optional<std::string> result;

  co_spawn(loop.get_executor(), recover(),
           overloaded{[&](std::string value) { result = std::move(value); },
                      [](std::exception_ptr) { FAIL("unexpected exception"); }});
  loop.run();

  REQUIRE(result == "boom");
}

TEST_CASE("co_spawn hands an escaping exception to the handler", "[async][task][co_spawn]") {
  run_loop loop;
  std::exception_ptr error;
  bool succeeded = false;

  co_spawn(loop.get_executor(), fail(),
           overloaded{[&] { succeeded = true; }, [&](std::exception_ptr thrown) { error = std::move(thrown); }});
  loop.run();

  REQUIRE_FALSE(succeeded);
  REQUIRE_THROWS_AS(std::rethrow_exception(error), std::runtime_error);
}

TEST_CASE("A task is lazy and frees its frame when destroyed without running", "[async][task]") {
  auto resource = std::make_shared<int>(0);
  bool started = false;
  {
    auto work = hold(resource, started);
    REQUIRE(resource.use_count() == 2);
  }

  REQUIRE_FALSE(started);
  REQUIRE(resource.use_count() == 1);
}

TEST_CASE("A finished task chain frees every frame", "[async][task][co_spawn]") {
  run_loop loop;
  auto resource = std::make_shared<int>(0);
  bool started = false;
  bool completed = false;

  co_spawn(loop.get_executor(), hold(resource, started),
           overloaded{[&] { completed = true; }, [](std::exception_ptr) { FAIL("unexpected exception"); }});
  REQUIRE(resource.use_count() == 2);
  loop.run();

  REQUIRE(started);
  REQUIRE(completed);
  REQUIRE(resource.use_count() == 1);
}

TEST_CASE("yield lets the other queued tasks run first", "[async][task][yield]") {
  run_loop loop;
  std::vector<std::string> log;
  int completed = 0;
  auto const handler = overloaded{[&] { ++completed; }, [](std::exception_ptr) { FAIL("unexpected exception"); }};

  co_spawn(loop.get_executor(), record(log, "a"), handler);
  co_spawn(loop.get_executor(), record(log, "b"), handler);
  loop.run();

  REQUIRE(completed == 2);
  REQUIRE(log == std::vector<std::string>{"a1", "b1", "a2", "b2"});
}

TEST_CASE("co_spawn hands a failing post to the caller and destroys the task", "[async][co_spawn]") {
  auto const resource = std::make_shared<int>(42);
  bool handled = false;

  REQUIRE_THROWS_AS(co_spawn(full_executor{}, owns(resource),
                             overloaded{[&](int) { handled = true; }, [&](std::exception_ptr) { handled = true; }}),
                    std::bad_alloc);

  // The task never ran, its handler was not invoked, and its frame (holding a copy of the resource) is gone.
  REQUIRE_FALSE(handled);
  REQUIRE(resource.use_count() == 1);
}

namespace {

[[gnu::noinline]] std::uintptr_t where_the_stack_is() {
  volatile char here = 0;

  return reinterpret_cast<std::uintptr_t>(&here);
}

} // namespace

TEST_CASE("task: a long loop over tasks that do not suspend does not grow the stack", "[async]") {
  std::uintptr_t lowest = std::numeric_limits<std::uintptr_t>::max();
  std::uintptr_t highest = 0;

  auto const immediate = [&]() -> quoll::async::task<int> {
    auto const here = where_the_stack_is();

    lowest = std::min(lowest, here);
    highest = std::max(highest, here);
    co_return 1;
  };

  constexpr auto rounds = 20'000;

  auto const looping = [&]() -> quoll::async::task<long> {
    long sum = 0;

    for (auto round = 0; round < rounds; ++round) {
      sum += co_await immediate();
    }

    co_return sum;
  };

  quoll::async::run_loop loop;

  REQUIRE(quoll::test::run(loop, looping()) == rounds);

// A sanitizer's frames make no tail calls: the program gets more stack, and a debug build there is what it is.
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
  // A few frames, rather than one per round. A stack limit would only see this with many more rounds.
  REQUIRE(highest - lowest < 16 * 1024);
#endif
}
