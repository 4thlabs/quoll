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

#include <quoll/async/any_executor.hpp>
#include <quoll/async/co_spawn.hpp>
#include <quoll/async/context_awaitable.hpp>
#include <quoll/async/current_executor.hpp>
#include <quoll/async/current_stop_token.hpp>
#include <quoll/async/detached.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/reports.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(__EMSCRIPTEN__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

namespace async = quoll::async;

using async::co_spawn;
using async::task;
using async::this_executor;
using quoll::test::overloaded;

// An executor of the loop, with something on the heap: what is left of a copy that outlived it shows under ASan.
class owning_executor {
public:
  explicit owning_executor(async::run_loop &loop)
      : executor_(std::make_shared<async::run_loop::executor>(loop.get_executor())) {}

  void post(async::work_item &item) const { executor_->post(item); }

  [[nodiscard]] bool operator==(owning_executor const &) const noexcept = default;

private:
  std::shared_ptr<async::run_loop::executor> executor_;
};

static_assert(async::Executor<owning_executor>);

// Past what any_executor holds in place.
class big_executor {
public:
  explicit big_executor(async::run_loop &loop) : executor_(loop) {}

  void post(async::work_item &item) const { executor_.post(item); }

  [[nodiscard]] bool operator==(big_executor const &) const noexcept = default;

private:
  owning_executor executor_;
  std::array<void *, 16> padding_{};
};

static_assert(async::Executor<big_executor>);

// Posts `allowed` times, then throws.
class running_out_executor {
public:
  running_out_executor(async::run_loop &loop, int allowed)
      : executor_(loop.get_executor()), left_(std::make_shared<int>(allowed)) {}

  void post(async::work_item &item) const {
    if (*left_ == 0) {
      throw std::runtime_error{"out of room"};
    }

    --*left_;
    executor_.post(item);
  }

  [[nodiscard]] bool operator==(running_out_executor const &) const noexcept = default;

private:
  async::run_loop::executor executor_;
  std::shared_ptr<int> left_;
};

static_assert(async::Executor<running_out_executor>);
static_assert(async::ContextAwaitable<decltype(co_spawn(this_executor, std::declval<task<>>(), async::detached))>);

task<> note(std::vector<std::string> &log, std::string what) {
  log.push_back(std::move(what));
  co_return;
}

task<> note_later(std::vector<std::string> &log, std::string what) {
  co_await async::yield();
  co_await async::yield();
  log.push_back(std::move(what));
}

task<int> answer() { co_return 42; }

task<int> fail() {
  throw std::runtime_error{"failed"};
  co_return 0;
}

task<bool> can_stop() { co_return (co_await async::current_stop_token()).stop_possible(); }

} // namespace

TEST_CASE("co_spawn(this_executor): on one thread, the task starts once the coroutine lets it", "[async]") {
  async::run_loop loop;
  std::vector<std::string> log;

  auto const parent = [&]() -> task<int> {
    log.emplace_back("before");
    co_await co_spawn(this_executor, note(log, "spawned"), async::detached);
    log.emplace_back("after");
    co_return 0;
  };

  REQUIRE(quoll::test::run(loop, parent()) == 0);
  REQUIRE(log == std::vector<std::string>{"before", "after", "spawned"});
}

TEST_CASE("co_spawn without an executor: the tag is the default", "[async]") {
  async::run_loop loop;
  std::vector<std::string> log;
  std::stop_source source;
  auto can_be_stopped = false;

  auto const parent = [&]() -> task<int> {
    log.emplace_back("before");
    co_await co_spawn(note(log, "spawned"), async::detached);
    co_await co_spawn(can_stop(),
                      overloaded{[&](bool possible) { can_be_stopped = possible; }, [](std::exception_ptr const &) {}},
                      source.get_token());
    log.emplace_back("after");
    co_return 0;
  };

  REQUIRE(quoll::test::run(loop, parent()) == 0);
  REQUIRE(log == std::vector<std::string>{"before", "after", "spawned"});
  REQUIRE(can_be_stopped);
}

TEST_CASE("co_spawn(this_executor): the task runs on the executor of the coroutine", "[async]") {
  async::run_loop loop;
  auto ran_on_loop = false;

  auto const child = [&]() -> task<> {
    auto const executor = async::any_executor{co_await async::current_executor()};

    ran_on_loop = executor.target<async::run_loop::executor>() != nullptr &&
                  *executor.target<async::run_loop::executor>() == loop.get_executor();
  };

  auto const parent = [&]() -> task<int> {
    co_await co_spawn(this_executor, child(), async::detached);
    co_return 0;
  };

  REQUIRE(quoll::test::run(loop, parent()) == 0);
  REQUIRE(ran_on_loop);
}

TEST_CASE("co_spawn: a task on the executor of a coroutine outlives it", "[async]") {
  async::run_loop loop;
  std::vector<std::string> log;

  auto const by_tag = [&]() -> task<> { co_await co_spawn(this_executor, note_later(log, "child"), async::detached); };

  auto const by_reference = [&]() -> task<> {
    co_spawn(co_await async::current_executor(), note_later(log, "child"), async::detached);
  };

  auto const finished = [&] { log.emplace_back("parent"); };
  auto const failed = [](std::exception_ptr const &) { FAIL("the parent failed"); };

  // The executor the parent ran on goes with its frame, before the child ends.
  co_spawn(owning_executor{loop}, by_tag(), overloaded{finished, failed});
  co_spawn(owning_executor{loop}, by_reference(), overloaded{finished, failed});
  co_spawn(async::any_executor{big_executor{loop}}, by_tag(), overloaded{finished, failed});
  co_spawn(big_executor{loop}, by_reference(), overloaded{finished, failed});
  loop.run();

  REQUIRE(log == std::vector<std::string>{"parent", "parent", "parent", "parent", "child", "child", "child", "child"});
}

TEST_CASE("co_spawn(this_executor): without the co_await, nothing runs, and the task goes", "[async]") {
  std::vector<std::string> log;
  auto handled = false;
  auto kept = std::make_shared<int>(0);

  auto const holding = [](std::vector<std::string> &to, std::shared_ptr<int>) -> task<> {
    to.emplace_back("ran");
    co_return;
  };

  {
    auto const dropped =
        co_spawn(this_executor, holding(log, kept),
                 overloaded{[&] { handled = true; }, [&](std::exception_ptr const &) { handled = true; }});

    REQUIRE(kept.use_count() == 2);
  }

  REQUIRE(kept.use_count() == 1);
  REQUIRE(log.empty());
  REQUIRE_FALSE(handled);
}

TEST_CASE("co_spawn(this_executor): a post that throws reaches the coroutine, and nothing runs", "[async]") {
  async::run_loop loop;
  std::vector<std::string> log;
  std::string caught;
  auto handled = false;
  auto kept = std::make_shared<int>(0);

  auto const holding = [](std::vector<std::string> &to, std::shared_ptr<int>) -> task<> {
    to.emplace_back("ran");
    co_return;
  };

  auto const parent = [&]() -> task<> {
    try {
      co_await co_spawn(this_executor, holding(log, kept),
                        overloaded{[&] { handled = true; }, [&](std::exception_ptr const &) { handled = true; }});
    } catch (std::runtime_error const &failure) {
      caught = failure.what();
    }
  };

  // Enough for the parent to start, and no more.
  co_spawn(running_out_executor{loop, 1}, parent(), async::detached);
  loop.run();

  REQUIRE(caught == "out of room");
  REQUIRE(log.empty());
  REQUIRE_FALSE(handled);
  REQUIRE(kept.use_count() == 1);
}

TEST_CASE("co_spawn(this_executor): the handler gets the value or the exception", "[async]") {
  async::run_loop loop;
  int value = 0;
  std::string failure;

  auto const parent = [&]() -> task<int> {
    co_await co_spawn(this_executor, answer(),
                      overloaded{[&](int result) { value = result; }, [](std::exception_ptr const &) {}});
    co_await co_spawn(this_executor, fail(),
                      overloaded{[](int) {},
                                 [&](std::exception_ptr const &error) {
                                   try {
                                     std::rethrow_exception(error);
                                   } catch (std::runtime_error const &thrown) {
                                     failure = thrown.what();
                                   }
                                 }});
    co_return 0;
  };

  REQUIRE(quoll::test::run(loop, parent()) == 0);
  REQUIRE(value == 42);
  REQUIRE(failure == "failed");
}

TEST_CASE("co_spawn(this_executor): the stop token of the coroutine is not taken, unless it is passed", "[async]") {
  async::run_loop loop;
  std::stop_source source;
  auto alone = true;
  auto passed = false;

  auto const parent = [&]() -> task<int> {
    co_await co_spawn(this_executor, can_stop(),
                      overloaded{[&](bool possible) { alone = possible; }, [](std::exception_ptr const &) {}});
    co_await co_spawn(this_executor, can_stop(),
                      overloaded{[&](bool possible) { passed = possible; }, [](std::exception_ptr const &) {}},
                      co_await async::current_stop_token());
    co_return 0;
  };

  REQUIRE(quoll::test::run(loop, parent(), source.get_token()) == 0);
  REQUIRE_FALSE(alone);
  REQUIRE(passed);
}

TEST_CASE("detached: a stop during a wait ends that task, and the others go on", "[async][timer]") {
  quoll::test::virtual_time time;
  std::stop_source stop;
  std::vector<std::string> log;

  auto const waiting = [&]() -> task<> {
    log.emplace_back("waits");
    co_await async::sleep_for(std::chrono::hours{1});
    log.emplace_back("was not stopped");
  };

  auto const going_on = [&]() -> task<> {
    co_await async::sleep_for(std::chrono::seconds{1});
    stop.request_stop();
    co_await async::sleep_for(std::chrono::seconds{1});
    log.emplace_back("went on");
  };

  co_spawn(time.get_executor(), waiting(), async::detached, stop.get_token());
  co_spawn(time.get_executor(), going_on(), async::detached);
  time.run();

  REQUIRE(log == std::vector<std::string>{"waits", "went on"});
}

TEST_CASE("detached: a task with a value that a stop ends goes the same way", "[async][timer]") {
  quoll::test::virtual_time time;
  std::stop_source stop;
  auto went_on = false;

  auto const waiting = []() -> task<int> {
    co_await async::sleep_for(std::chrono::hours{1});
    co_return 42;
  };

  auto const stopping = [&]() -> task<> {
    co_await async::sleep_for(std::chrono::seconds{1});
    stop.request_stop();
    went_on = true;
  };

  co_spawn(time.get_executor(), waiting(), async::detached, stop.get_token());
  co_spawn(time.get_executor(), stopping(), async::detached);
  time.run();

  REQUIRE(went_on);
}

TEST_CASE("detached: nothing and a value are dropped", "[async]") {
  async::detached();
  async::detached(42);
  async::detached(std::exception_ptr{});

  REQUIRE(quoll::test::reports().empty());
}

#if !defined(__EMSCRIPTEN__)

TEST_CASE("co_spawn: a handler that throws ends the program", "[async]") {
  auto const child = ::fork();

  REQUIRE(child >= 0);

  if (child == 0) {
    std::set_terminate([] { ::_exit(42); });

    async::run_loop loop;

    async::co_spawn(
        loop.get_executor(), []() -> task<int> { co_return 1; }(),
        quoll::test::overloaded{[](int) { throw std::runtime_error{"from the handler"}; },
                                [](std::exception_ptr const &) {}});
    loop.run();
    ::_exit(0);
  }

  int status = 0;

  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 42);
}

#endif

TEST_CASE("detached: a failure is reported and the program goes on, a stop is not reported", "[async]") {
  async::detached(std::make_exception_ptr(std::runtime_error{"failure"}));
  // As asio reports one.
  async::detached(std::make_exception_ptr(std::system_error{ECANCELED, std::system_category()}));
  async::detached(std::make_exception_ptr(std::system_error{std::make_error_code(std::errc::timed_out)}));
  async::detached(std::make_exception_ptr(std::system_error{std::make_error_code(std::errc::operation_canceled)}));

  auto const reported = quoll::test::reports();

  REQUIRE(reported.size() == 2);
  REQUIRE(reported.front() == "quoll: a task spawned with async::detached ended on an exception: failure\n");
  REQUIRE(reported.back().starts_with("quoll: a task spawned with async::detached ended on an exception: "));
}

namespace {

task<int> holding_a_value(std::shared_ptr<int> held) {
  co_await async::yield();
  co_return *held;
}

task<> holding_nothing_to_return(std::shared_ptr<int> held) {
  co_await async::yield();
  *held = 2;
}

} // namespace

TEST_CASE("co_spawn: what the task holds is gone before its handler runs", "[async]") {
  async::run_loop loop;
  auto held = std::make_shared<int>(1);
  std::weak_ptr<int> const watching = held;
  std::optional<bool> alive_in_handler;

  SECTION("with a value") {
    async::co_spawn(loop.get_executor(), holding_a_value(std::move(held)),
                    quoll::test::overloaded{[&](int) { alive_in_handler = !watching.expired(); },
                                            [](std::exception_ptr const &) {}});
  }

  SECTION("without") {
    async::co_spawn(
        loop.get_executor(), holding_nothing_to_return(std::move(held)),
        quoll::test::overloaded{[&] { alive_in_handler = !watching.expired(); }, [](std::exception_ptr const &) {}});
  }

  loop.run();

  REQUIRE(alive_in_handler == false);
}
