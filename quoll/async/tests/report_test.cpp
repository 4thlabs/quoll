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
#include <quoll/async/detail/report.hpp>
#include <quoll/async/report.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/reports.hpp>

#include <catch2/catch_test_macros.hpp>

#if !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace async = quoll::async;

using async::task;

task<> throwing(std::exception_ptr failure) {
  co_await async::yield();
  std::rethrow_exception(failure);
}

task<> counting(int &turns) {
  for (; turns < 3; ++turns) {
    co_await async::yield();
  }
}

} // namespace

TEST_CASE("detached: an exception a task lets escape is reported, and the others go on", "[async]") {
  async::run_loop loop;
  auto turns = 0;

  async::co_spawn(loop.get_executor(), throwing(std::make_exception_ptr(std::runtime_error{"boom"})), async::detached);
  async::co_spawn(loop.get_executor(), counting(turns), async::detached);
  loop.run();

  REQUIRE(turns == 3);
  REQUIRE(quoll::test::reports() ==
          std::vector<std::string>{"quoll: a task spawned with async::detached ended on an exception: boom\n"});
}

TEST_CASE("detached: a stop is no failure, and an exception of any type is reported", "[async]") {
  async::run_loop loop;

  async::co_spawn(loop.get_executor(),
                  throwing(std::make_exception_ptr(
                      std::system_error{std::make_error_code(std::errc::operation_canceled), "stopped"})),
                  async::detached);
  loop.run();

  REQUIRE(quoll::test::reports().empty());

  async::co_spawn(loop.get_executor(), throwing(std::make_exception_ptr(42)), async::detached);
  loop.run();

  REQUIRE(
      quoll::test::reports() ==
      std::vector<std::string>{"quoll: a task spawned with async::detached ended on an exception: an exception of a "
                               "type that is no std::exception\n"});
}

TEST_CASE("report: to the sink set, and to stderr with none", "[async]") {
  // Given back however the test ends, for the cases after it.
  struct restoring {
    quoll::async::report_sink kept = quoll::async::set_report_sink(nullptr);

    restoring() = default;
    restoring(restoring const &) = delete;
    restoring(restoring &&) = delete;
    restoring &operator=(restoring const &) = delete;
    restoring &operator=(restoring &&) = delete;

    ~restoring() { quoll::async::set_report_sink(kept); }
  } const restore;

  REQUIRE(restore.kept == &quoll::test::keep_report);

#if !defined(__EMSCRIPTEN__)
  // What reaches stderr, read back through a pipe put in its place.
  std::fflush(stderr);

  std::array<int, 2> pipe_ends{};

  REQUIRE(::pipe(pipe_ends.data()) == 0);

  auto const saved = ::dup(STDERR_FILENO);

  REQUIRE(::dup2(pipe_ends[1], STDERR_FILENO) == STDERR_FILENO);
  quoll::async::report("a test of report()", std::make_exception_ptr(std::runtime_error{"on stderr"}));
  std::fflush(stderr);
  ::dup2(saved, STDERR_FILENO);
  ::close(saved);
  ::close(pipe_ends[1]);

  std::array<char, 128> read_back{};
  auto const got = ::read(pipe_ends[0], read_back.data(), read_back.size());

  ::close(pipe_ends[0]);
  REQUIRE(std::string_view{read_back.data(), static_cast<std::size_t>(got)} ==
          "quoll: a test of report(): on stderr\n");
#endif

  REQUIRE(quoll::async::set_report_sink(&quoll::test::keep_report) == nullptr);
  quoll::async::report("a test of report()", std::make_exception_ptr(std::runtime_error{"kept"}));
  REQUIRE(quoll::test::reports() == std::vector<std::string>{"quoll: a test of report(): kept\n"});
}

namespace {

// A clock the test moves by hand, for report()'s seconds.
std::atomic<std::int64_t> fake_milliseconds{0};

std::chrono::steady_clock::time_point fake_now() noexcept {
  return std::chrono::steady_clock::time_point{std::chrono::milliseconds{fake_milliseconds.load()}};
}

// The clock given back however the test ends.
struct on_fake_clock {
  quoll::async::detail::report_clock kept = quoll::async::detail::set_report_clock(&fake_now);

  on_fake_clock() = default;
  on_fake_clock(on_fake_clock const &) = delete;
  on_fake_clock(on_fake_clock &&) = delete;
  on_fake_clock &operator=(on_fake_clock const &) = delete;
  on_fake_clock &operator=(on_fake_clock &&) = delete;

  ~on_fake_clock() { quoll::async::detail::set_report_clock(kept); }
};

void report_line(int line) {
  quoll::async::report("one of many", std::make_exception_ptr(std::runtime_error{std::to_string(line)}));
}

} // namespace

TEST_CASE("report: at most ten lines a second, and what went past them is counted", "[async]") {
  on_fake_clock const clock;

  fake_milliseconds = 10'000;
  quoll::async::set_report_sink(&quoll::test::keep_report);

  for (auto line = 0; line < 13; ++line) {
    report_line(line);
  }

  REQUIRE(quoll::test::reports().size() == 10);
  REQUIRE(quoll::test::reports().back() == "quoll: one of many: 9\n");

  // A second on: those left out are said first.
  fake_milliseconds = 11'500;
  report_line(13);

  auto const reported = quoll::test::reports();

  REQUIRE(reported.size() == 12);
  REQUIRE(reported[10] == "quoll: 3 more reports left out since the last line written\n");
  REQUIRE(reported[11] == "quoll: one of many: 13\n");
}

TEST_CASE("report: what ends a program goes past the limit, with what was left out", "[async]") {
  on_fake_clock const clock;

  fake_milliseconds = 20'000;
  quoll::async::set_report_sink(&quoll::test::keep_report);

  for (auto line = 0; line < 12; ++line) {
    report_line(line);
  }

  quoll::async::detail::report_always("the end", std::make_exception_ptr(std::runtime_error{"why"}));

  auto const reported = quoll::test::reports();

  REQUIRE(reported.size() == 12);
  REQUIRE(reported[10] == "quoll: 2 more reports left out since the last line written\n");
  REQUIRE(reported[11] == "quoll: the end: why\n");
}
