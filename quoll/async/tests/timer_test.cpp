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
#include <quoll/async/detail/stop_scope.hpp>
#include <quoll/async/executor_ref.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/timer.hpp>
#include <quoll/async/timer_ref.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace {

using namespace std::chrono_literals;

using quoll::async::sleep_for;
using quoll::async::task;
using quoll::async::when_all;
using quoll::async::with_timeout;
using quoll::test::run;
using quoll::test::virtual_time;

static_assert(quoll::async::Timer<quoll::async::timer_ref>);

// `value`, after `delay`.
task<int> value_after(std::chrono::nanoseconds delay, int value) {
  co_await sleep_for(delay);
  co_return value;
}

// Nothing, after `delay`.
task<> nothing_after(std::chrono::nanoseconds delay) { co_await sleep_for(delay); }

task<int> immediate(int value) { co_return value; }

// The error code `work` ends with: none if it succeeds.
template <class T>
task<std::error_code> error_of(task<T> work) {
  try {
    static_cast<void>(co_await std::move(work));
  } catch (std::system_error const &error) {
    co_return error.code();
  }

  co_return std::error_code{};
}

// Requests stop on `source` once `delay` has elapsed.
task<> stop_after(std::stop_source &source, std::chrono::nanoseconds delay) {
  co_await sleep_for(delay);
  source.request_stop();
}

// The error code of `work`, stopped after `delay`.
template <class T>
task<std::error_code> error_when_stopped(task<T> work, std::stop_source &source, std::chrono::nanoseconds delay) {
  auto [error, stopped] = co_await when_all(error_of(std::move(work)), stop_after(source, delay));

  co_return error;
}

// Sleeps on, whatever is requested, then gives `value`.
task<int> ignores_stop(std::chrono::nanoseconds delay, int value) {
  // Not through sleep_for: the wait must go on after the stop request, under a token that never stops.
  co_await quoll::async::detail::stop_scope<void>{nothing_after(delay), std::stop_token{}};
  co_return value;
}

// Sleeps; a stop request makes it fail with an error of its own.
task<int> fails_when_stopped() {
  try {
    co_await sleep_for(1h);
  } catch (std::system_error const &) {
    throw std::runtime_error{"cleanup failed"};
  }

  co_return 0;
}

task<bool> wait_through_ref(quoll::async::timer_ref timer, std::chrono::nanoseconds duration) {
  co_return co_await timer.wait_for(duration);
}

task<bool> wait_stopped(quoll::async::timer_ref timer, std::stop_source &source) {
  auto [elapsed, stopped] = co_await when_all(wait_through_ref(timer, 1h), stop_after(source, 1s));

  co_return elapsed;
}

task<int> return_after_sleeping(virtual_time const &time) {
  co_await sleep_for(1s);
  co_return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(time.now()).count());
}

task<int> sleep_in_parallel() {
  auto [one, two] = co_await when_all(value_after(1s, 1), value_after(2s, 2));

  co_return one + two;
}

task<int> started_then_answer(bool &started) {
  started = true;
  co_return 1;
}

} // namespace

TEST_CASE("sleep_for suspends the task for the duration", "[async][timer]") {
  virtual_time time;

  REQUIRE(run(time, return_after_sleeping(time)) == 1);
  REQUIRE(time.now() == 1s);
}

TEST_CASE("sleep_for: tasks sleep side by side", "[async][timer]") {
  virtual_time time;

  REQUIRE(run(time, sleep_in_parallel()) == 3);
  REQUIRE(time.now() == 2s);
}

TEST_CASE("sleep_for: a stop request ends the sleep with operation_canceled", "[async][timer]") {
  virtual_time time;
  std::stop_source source;

  auto const error = run(time, error_when_stopped(nothing_after(1h), source, 1s), source.get_token());

  REQUIRE(error == std::errc::operation_canceled);
  REQUIRE(time.now() == 1s);
}

TEST_CASE("sleep_for: a stop requested before does not wait", "[async][timer]") {
  virtual_time time;
  std::stop_source source;

  source.request_stop();

  REQUIRE(run(time, error_of(nothing_after(1h)), source.get_token()) == std::errc::operation_canceled);
  REQUIRE(time.now() == 0s);
}

TEST_CASE("sleep_for and with_timeout need an executor offering a timer", "[async][timer]") {
  bool started = false;

  REQUIRE_THROWS_AS(run(error_of(nothing_after(1s))), std::logic_error);
  REQUIRE_THROWS_AS(run(with_timeout(1s, started_then_answer(started))), std::logic_error);
  REQUIRE_FALSE(started);
}

TEST_CASE("timer_ref refers to a timer, and tells a stop request apart", "[async][timer]") {
  virtual_time time;
  auto const executor = time.get_executor();
  quoll::async::timer_ref const timer{executor};
  std::stop_source source;

  REQUIRE(timer.target<virtual_time::executor>() == &executor);
  REQUIRE(timer.target<int>() == nullptr);
  REQUIRE(run(time, wait_through_ref(timer, 1s)) == true);
  REQUIRE(time.now() == 1s);
  REQUIRE(run(time, wait_stopped(timer, source), source.get_token()) == false);
  REQUIRE(time.now() == 2s);
}

TEST_CASE("the erased executors answer the query for a timer", "[async][timer]") {
  virtual_time time;
  auto const executor = time.get_executor();
  quoll::async::run_loop loop;
  auto const without = loop.get_executor();

  REQUIRE(quoll::async::executor_ref{executor}.query<quoll::async::timer_ref>());
  REQUIRE(quoll::async::any_executor{executor}.query<quoll::async::timer_ref>());
  REQUIRE_FALSE(quoll::async::executor_ref{without}.query<quoll::async::timer_ref>());
}

TEST_CASE("with_timeout returns the value of a task that ends in time, without waiting more", "[async][timer]") {
  virtual_time time;

  REQUIRE(run(time, with_timeout(1h, value_after(1s, 42))) == 42);
  REQUIRE(time.now() == 1s);
}

TEST_CASE("with_timeout: a task<> that ends in time", "[async][timer]") {
  virtual_time time;

  REQUIRE(run(time, error_of(with_timeout(1h, nothing_after(1s)))) == std::error_code{});
  REQUIRE(time.now() == 1s);
}

TEST_CASE("with_timeout hands over a move-only value", "[async][timer]") {
  virtual_time time;

  auto const answer = [] -> task<std::unique_ptr<int>> { co_return std::make_unique<int>(7); };
  auto const result = run(time, with_timeout(1h, answer()));

  REQUIRE(result);
  REQUIRE(**result == 7);
}

TEST_CASE("with_timeout: a task that never suspends takes no time", "[async][timer]") {
  virtual_time time;

  REQUIRE(run(time, with_timeout(1h, immediate(3))) == 3);
  REQUIRE(time.now() == 0s);
}

TEST_CASE("with_timeout: past the timeout, the task is stopped and the error is timed_out", "[async][timer]") {
  virtual_time time;

  REQUIRE(run(time, error_of(with_timeout(1s, value_after(1h, 42)))) == std::errc::timed_out);
  REQUIRE(time.now() == 1s);
}

TEST_CASE("with_timeout: a timeout of zero or less expires as soon as the task waits", "[async][timer]") {
  virtual_time time;

  REQUIRE(run(time, error_of(with_timeout(0s, nothing_after(1h)))) == std::errc::timed_out);
  REQUIRE(run(time, error_of(with_timeout(-1s, nothing_after(1h)))) == std::errc::timed_out);
  REQUIRE(time.now() == 0s);
}

TEST_CASE("with_timeout: a task that ignores the stop request still gives its result", "[async][timer]") {
  virtual_time time;

  REQUIRE(run(time, with_timeout(1s, ignores_stop(5s, 42))) == 42);
  REQUIRE(time.now() == 5s);
}

TEST_CASE("with_timeout: another failure of a stopped task is rethrown as it is", "[async][timer]") {
  virtual_time time;

  REQUIRE_THROWS_AS(run(time, with_timeout(1s, fails_when_stopped())), std::runtime_error);
  REQUIRE(time.now() == 1s);
}

TEST_CASE("with_timeout: a failure before the timeout is rethrown, and the timer is not waited for", "[async][timer]") {
  virtual_time time;

  auto const fail = [] -> task<int> {
    co_await sleep_for(1s);
    throw std::runtime_error{"failed"};
  };

  REQUIRE_THROWS_AS(run(time, with_timeout(1h, fail())), std::runtime_error);
  REQUIRE(time.now() == 1s);
}

TEST_CASE("with_timeout: a task canceling itself before the timeout stays canceled", "[async][timer]") {
  virtual_time time;

  auto const cancel = [] -> task<int> {
    co_await sleep_for(1s);
    throw std::system_error{std::make_error_code(std::errc::operation_canceled)};
  };

  REQUIRE(run(time, error_of(with_timeout(1h, cancel()))) == std::errc::operation_canceled);
}

TEST_CASE("with_timeout: a stop request of the awaiter stays operation_canceled", "[async][timer]") {
  virtual_time time;
  std::stop_source source;

  auto const error =
      run(time, error_when_stopped(with_timeout(1h, value_after(1h, 42)), source, 1s), source.get_token());

  REQUIRE(error == std::errc::operation_canceled);
  REQUIRE(time.now() == 1s);
}

TEST_CASE("with_timeout: a stop requested before stops the task at once", "[async][timer]") {
  virtual_time time;
  std::stop_source source;

  source.request_stop();

  REQUIRE(run(time, error_of(with_timeout(1h, value_after(1h, 42))), source.get_token()) ==
          std::errc::operation_canceled);
  REQUIRE(time.now() == 0s);
}

TEST_CASE("with_timeout nests: the shortest timeout decides, and both report timed_out", "[async][timer]") {
  virtual_time inner_first;
  virtual_time outer_first;

  REQUIRE(run(inner_first, error_of(with_timeout(1h, with_timeout(1s, value_after(5h, 42))))) == std::errc::timed_out);
  REQUIRE(inner_first.now() == 1s);
  REQUIRE(run(outer_first, error_of(with_timeout(1s, with_timeout(1h, value_after(5h, 42))))) == std::errc::timed_out);
  REQUIRE(outer_first.now() == 1s);
}
