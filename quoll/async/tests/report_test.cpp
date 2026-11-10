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
#include <quoll/async/run_loop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/reports.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

// For every test case of the binary: what is reported goes to quoll::test::reports(), not to stderr, which the tests
// that throw on purpose would fill.
class keeping_reports : public Catch::EventListenerBase {
public:
  using Catch::EventListenerBase::EventListenerBase;

  void testRunStarting(Catch::TestRunInfo const &) override {
    quoll::async::detail::set_report_sink(&quoll::test::keep_report);
  }

  void testCaseStarting(Catch::TestCaseInfo const &) override { quoll::test::clear_reports(); }
};

} // namespace

CATCH_REGISTER_LISTENER(keeping_reports)

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

TEST_CASE("report: to the sink set, and to stderr once it is given back", "[async]") {
  auto const kept = quoll::async::detail::set_report_sink(nullptr);

  REQUIRE(kept == &quoll::test::keep_report);

  // To stderr, which this test does not read: it only shows that nothing throws, and that the sink comes back.
  quoll::async::detail::report("a test of report()", std::make_exception_ptr(std::runtime_error{"on stderr"}));
  REQUIRE(quoll::async::detail::set_report_sink(kept) == nullptr);
  quoll::async::detail::report("a test of report()", std::make_exception_ptr(std::runtime_error{"kept"}));
  REQUIRE(quoll::test::reports() == std::vector<std::string>{"quoll: a test of report(): kept\n"});
}
