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
#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/with_timeout.hpp>
#include <quoll/test/allocations.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/virtual_time.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <new>

namespace {

namespace async = quoll::async;

using async::task;
using namespace std::chrono_literals;

task<int> after_a_while() {
  co_await async::sleep_for(1ms);
  co_return 1;
}

task<int> with_two_children() {
  auto const [first, second] = co_await async::when_all(after_a_while(), after_a_while());

  co_return first + second;
}

task<int> bounded() { co_return co_await async::with_timeout(1s, after_a_while()); }

enum class ended { with_a_value, with_bad_alloc, otherwise, not_at_all };

// Every allocation of `make()` fails in turn: it reaches the handler or what spawns. What terminates instead ends the
// test program, which is the failure.
template <class Make>
void fails_cleanly_at_every_allocation(Make make) {
  // Coroutine frames come from what this thread freed from now on, so that what fails is what else there is to allocate
  // rather than the frames: each would otherwise fail once, and end the loop early.
  for (auto warming = 0; warming < 4; ++warming) {
    quoll::test::virtual_time time;

    async::co_spawn(time.get_executor(), make(), async::detached);
    time.run();
  }

  for (long failing_at = 0;; ++failing_at) {
    quoll::test::virtual_time time;
    auto outcome = ended::not_at_all;

    quoll::test::allocations_before_failing = failing_at;

    try {
      async::co_spawn(time.get_executor(), make(),
                      quoll::test::overloaded{[&](int) { outcome = ended::with_a_value; },
                                              [&](std::exception_ptr const &failure) {
                                                try {
                                                  std::rethrow_exception(failure);
                                                } catch (std::bad_alloc const &) {
                                                  outcome = ended::with_bad_alloc;
                                                } catch (...) {
                                                  outcome = ended::otherwise;
                                                }
                                              }});
      time.run();
    } catch (std::bad_alloc const &) {
      outcome = ended::with_bad_alloc;
    }

    auto const left = quoll::test::allocations_before_failing;

    quoll::test::allocations_before_failing = -1;

    REQUIRE(outcome != ended::otherwise);
    REQUIRE(outcome != ended::not_at_all);

    // Nothing was asked of the last allowance: every allocation has failed in turn.
    if (left != -1) {
      REQUIRE(outcome == ended::with_a_value);
      return;
    }
  }
}

} // namespace

TEST_CASE("out of memory: when_all gives it to what awaits", "[async]") {
  fails_cleanly_at_every_allocation(with_two_children);
}

TEST_CASE("out of memory: with_timeout gives it to what awaits", "[async][timer]") {
  fails_cleanly_at_every_allocation(bounded);
}
