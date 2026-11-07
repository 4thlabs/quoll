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
#include <quoll/async/detail/optional_timeout.hpp>
#include <quoll/async/detail/task_group.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <system_error>

namespace {

namespace async = quoll::async;

using async::task;
using namespace std::chrono_literals;

task<int> after(std::chrono::nanoseconds wait) {
  co_await async::sleep_for(wait);
  co_return 1;
}

} // namespace

TEST_CASE("task_group: join with nothing running does not wait", "[async]") {
  async::run_loop loop;
  async::detail::task_group group;

  auto const joining = [&group]() -> task<int> {
    co_await group.join();
    co_return 1;
  };

  REQUIRE(group.live() == 0);
  REQUIRE(quoll::test::run(loop, joining()) == 1);
}

TEST_CASE("task_group: join ends with the last task", "[async]") {
  async::run_loop loop;
  async::detail::task_group group;
  auto joined = false;

  auto const joining = [&]() -> task<> {
    co_await group.join();
    joined = true;
  };

  auto const running = [&]() -> task<int> {
    group.started();
    group.started();
    async::co_spawn(loop.get_executor(), joining(), async::detached);
    co_await async::yield();

    // It waits, for the two.
    auto const waited_for_two = !joined;

    group.ended();
    co_await async::yield();

    auto const waited_for_one = !joined;
    auto const live = group.live();

    group.ended();
    co_await async::yield();
    co_return waited_for_two &&waited_for_one &&live == 1 && joined ? 1 : 0;
  };

  REQUIRE(quoll::test::run(loop, running()) == 1);
  REQUIRE(group.live() == 0);
}

TEST_CASE("with_optional_timeout: zero is none, and negative has expired", "[async][timer]") {
  quoll::test::virtual_time time;

  // None: an hour passes.
  REQUIRE(quoll::test::run(time, async::detail::with_optional_timeout(0ns, after(1h))) == 1);
  // Short enough.
  REQUIRE(quoll::test::run(time, async::detail::with_optional_timeout(2h, after(1h))) == 1);
  // A deadline computed too late does not lift the limit.
  REQUIRE_THROWS_AS(quoll::test::run(time, async::detail::with_optional_timeout(-1ns, after(1h))), std::system_error);
  REQUIRE_THROWS_AS(quoll::test::run(time, async::detail::with_optional_timeout(1s, after(1h))), std::system_error);
}

TEST_CASE("with_optional_timeout: an executor with no timer has none to apply", "[async]") {
  async::run_loop loop;

  auto const immediate = []() -> task<int> { co_return 1; };

  REQUIRE(quoll::test::run(loop, async::detail::with_optional_timeout(1ns, immediate())) == 1);
}
