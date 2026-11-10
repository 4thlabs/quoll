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
#include <quoll/async/detached.hpp>
#include <quoll/async/detail/end_stopped.hpp>
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/stopped.hpp>
#include <quoll/async/stopped_as_optional.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/overloaded.hpp>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using quoll::async::run_loop;
using quoll::async::task;
using quoll::test::overloaded;

// How a co_spawn root ended.
struct ending {
  bool completed = false;
  bool stopped = false;
  std::exception_ptr failure;
  std::optional<int> value;
};

// Writes its name to the log as it is destroyed: what a frame destroyed with its locals leaves.
class witness {
public:
  witness(std::vector<std::string> &log, std::string name) : log_(&log), name_(std::move(name)) {}

  witness(witness const &) = delete;
  witness(witness &&) = delete;
  witness &operator=(witness const &) = delete;
  witness &operator=(witness &&) = delete;

  ~witness() { log_->push_back("~" + name_); }

private:
  std::vector<std::string> *log_;
  std::string name_;
};

// Ends on a stop after a turn, as an operation woken by a stop request would.
task<> stops(std::vector<std::string> &log) {
  witness const kept{log, "leaf"};

  co_await quoll::async::yield();
  co_await quoll::async::detail::end_stopped{};
  log.emplace_back("leaf resumed");
}

// The trap: a loop that catches everything and tries again.
task<> retries_for_ever(std::vector<std::string> &log) {
  witness const kept{log, "loop"};

  while (true) {
    try {
      co_await stops(log);
      log.emplace_back("loop resumed");
    } catch (std::exception const &) {
      log.emplace_back("loop caught");
    }
  }
}

task<int> above(std::vector<std::string> &log) {
  witness const kept{log, "top"};

  co_await retries_for_ever(log);
  log.emplace_back("top resumed");
  co_return 1;
}

auto ending_handler(ending &ended) {
  return overloaded{[&ended](int value) {
                      ended.completed = true;
                      ended.value = value;
                    },
                    [&ended] { ended.completed = true; },
                    [&ended](std::exception_ptr failure) {
                      ended.completed = true;
                      ended.failure = std::move(failure);
                    },
                    [&ended](quoll::async::stopped_t) {
                      ended.completed = true;
                      ended.stopped = true;
                    }};
}

} // namespace

TEST_CASE("stopped: a stop resumes no coroutine of the chain, which is destroyed from the top", "[async][stopped]") {
  run_loop loop;
  std::vector<std::string> log;
  ending ended;

  co_spawn(loop.get_executor(), above(log), ending_handler(ended));
  loop.run();

  CHECK(ended.completed);
  CHECK(ended.stopped);
  CHECK_FALSE(ended.failure);
  // No catch ran and nothing after a co_await: the frames went from the top, their locals with them, the co_await under
  // way in each first.
  CHECK(log == std::vector<std::string>{"~leaf", "~loop", "~top"});
}

namespace {

task<int> sees_the_stop(std::vector<std::string> &log) {
  auto const seen = co_await quoll::async::stopped_as_optional(stops(log));

  log.emplace_back(seen ? "went on" : "saw the stop");
  co_return seen ? 1 : 2;
}

task<int> answer() {
  co_await quoll::async::yield();
  co_return 42;
}

task<int> sees_the_answer() {
  auto const seen = co_await quoll::async::stopped_as_optional(answer());

  co_return seen.value_or(-1);
}

task<int> fails() {
  co_await quoll::async::yield();
  throw std::runtime_error{"boom"};
}

task<int> sees_the_failure() {
  try {
    static_cast<void>(co_await quoll::async::stopped_as_optional(fails()));
  } catch (std::runtime_error const &) {
    co_return 3;
  }

  co_return 0;
}

} // namespace

TEST_CASE("stopped_as_optional: a stop of the work gives nullopt, and the awaiter goes on", "[async][stopped]") {
  run_loop loop;
  std::vector<std::string> log;
  ending ended;

  co_spawn(loop.get_executor(), sees_the_stop(log), ending_handler(ended));
  loop.run();

  CHECK(ended.value == 2);
  CHECK(log == std::vector<std::string>{"~leaf", "saw the stop"});
}

namespace {

task<int> awaits_twice() {
  auto seen = quoll::async::stopped_as_optional(answer());

  static_cast<void>(co_await std::move(seen));

  try {
    static_cast<void>(co_await std::move(seen)); // NOLINT(bugprone-use-after-move): what the test is about.
  } catch (std::logic_error const &) {
    co_return 1;
  }

  co_return 0;
}

} // namespace

TEST_CASE("stopped_as_optional: awaited twice throws rather than run its task again", "[async][stopped]") {
  run_loop loop;
  ending twice;

  co_spawn(loop.get_executor(), awaits_twice(), ending_handler(twice));
  loop.run();

  CHECK(twice.value == 1);
}

TEST_CASE("stopped_as_optional: gives the value, and rethrows the exception", "[async][stopped]") {
  run_loop loop;
  ending answered;
  ending failed;

  co_spawn(loop.get_executor(), sees_the_answer(), ending_handler(answered));
  co_spawn(loop.get_executor(), sees_the_failure(), ending_handler(failed));
  loop.run();

  CHECK(answered.value == 42);
  CHECK(failed.value == 3);
}

namespace {

task<> waits(int turns) {
  for (int turn = 0; turn < turns; ++turn) {
    co_await quoll::async::yield();
  }
}

task<> stops_in_a_when_all(std::vector<std::string> &log) {
  witness const kept{log, "awaiter"};

  static_cast<void>(co_await quoll::async::when_all(waits(3), stops(log)));
  log.emplace_back("awaiter resumed");
}

task<> fails_later() {
  co_await waits(3);
  throw std::runtime_error{"boom"};
}

task<> fails_in_a_when_all(std::vector<std::string> &log) {
  static_cast<void>(co_await quoll::async::when_all(stops(log), fails_later()));
}

} // namespace

TEST_CASE("when_all: a child ending on a stop ends it, and its awaiter, on a stop", "[async][stopped]") {
  run_loop loop;
  std::vector<std::string> log;
  ending ended;

  co_spawn(loop.get_executor(), stops_in_a_when_all(log), ending_handler(ended));
  loop.run();

  CHECK(ended.stopped);
  // The other child ran to its end; the awaiter did not resume.
  CHECK(log == std::vector<std::string>{"~leaf", "~awaiter"});
}

TEST_CASE("when_all: a failure wins over a stop", "[async][stopped]") {
  run_loop loop;
  std::vector<std::string> log;
  ending ended;

  co_spawn(loop.get_executor(), fails_in_a_when_all(log), ending_handler(ended));
  loop.run();

  CHECK_FALSE(ended.stopped);
  REQUIRE(ended.failure);
  CHECK_THROWS_AS(std::rethrow_exception(ended.failure), std::runtime_error);
}

namespace {

task<> stops_in_a_scope(std::vector<std::string> &log) {
  witness const kept{log, "scope awaiter"};

  co_await quoll::async::detail::stop_scope<void>{stops(log), std::stop_token{}};
  log.emplace_back("scope awaiter resumed");
}

} // namespace

TEST_CASE("stop_scope: a stop of the work ends its awaiter too", "[async][stopped]") {
  run_loop loop;
  std::vector<std::string> log;
  ending ended;

  co_spawn(loop.get_executor(), stops_in_a_scope(log), ending_handler(ended));
  loop.run();

  CHECK(ended.stopped);
  CHECK(log == std::vector<std::string>{"~leaf", "~scope awaiter"});
}

TEST_CASE("stopped: async::detached takes a stop as no failure", "[async][stopped]") {
  run_loop loop;
  std::vector<std::string> log;

  co_spawn(loop.get_executor(), above(log), quoll::async::detached);
  loop.run();

  CHECK(log == std::vector<std::string>{"~leaf", "~loop", "~top"});
}
