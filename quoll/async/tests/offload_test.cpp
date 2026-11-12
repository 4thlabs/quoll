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
#include <quoll/async/offload.hpp>
#include <quoll/async/offloader.hpp>
#include <quoll/async/offloader_ref.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/service.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/work_item.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

namespace async = quoll::async;

using async::task;

// An offloader running what it is given at once: what an executor's own does, seen from the task.
struct calling_offloader {
  int *calls;

  [[nodiscard]] task<> offload(void (*work)(void *) noexcept, void *argument) const {
    ++*calls;
    work(argument);
    co_return;
  }
};

static_assert(async::Offloader<calling_offloader>);
static_assert(async::Offloader<async::offloader_ref>);
static_assert(std::is_trivially_copyable_v<async::offloader_ref>);

// A run_loop's executor, that offers it.
class offering_executor {
public:
  using services = async::service_list<async::offloader_ref>;

  offering_executor(async::run_loop::executor inner, calling_offloader const &offloader) noexcept
      : inner_(inner), offloader_(&offloader) {}

  void post(async::work_item &item) const noexcept { inner_.post(item); }

  [[nodiscard]] async::offloader_ref query(async::service_tag<async::offloader_ref>) const {
    return async::offloader_ref{*offloader_};
  }

  [[nodiscard]] bool operator==(offering_executor const &) const noexcept = default;

private:
  async::run_loop::executor inner_;
  calling_offloader const *offloader_;
};

// What quoll::test::run() takes, offering that offloader.
struct offering_context {
  async::run_loop loop;
  calling_offloader offloader;

  [[nodiscard]] offering_executor get_executor() { return offering_executor{loop.get_executor(), offloader}; }

  void run() { loop.run(); }
};

template <class T>
T run_offering(task<T> work, int &calls) {
  offering_context context{.loop = {}, .offloader = calling_offloader{&calls}};

  return std::move(*quoll::test::run(context, std::move(work)));
}

} // namespace

TEST_CASE("offload: on an executor that offers nothing, the work runs right there", "[async]") {
  async::run_loop loop;
  auto const here = std::this_thread::get_id();

  auto const working = []() -> task<std::thread::id> {
    co_return co_await async::offload([] { return std::this_thread::get_id(); });
  };

  REQUIRE(quoll::test::run(loop, working()) == here);

  auto const failing = []() -> task<int> {
    co_return co_await async::offload([]() -> int { throw std::runtime_error{"from the work"}; });
  };

  REQUIRE_THROWS_AS(quoll::test::run(loop, failing()), std::runtime_error);
}

TEST_CASE("offload: through the executor's offloader, a value, nothing or a failure", "[async]") {
  auto calls = 0;

  SECTION("a value that only moves") {
    auto const working = []() -> task<std::unique_ptr<int>> {
      co_return co_await async::offload([] { return std::make_unique<int>(42); });
    };

    REQUIRE(*run_offering(working(), calls) == 42);
    REQUIRE(calls == 1);
  }

  SECTION("nothing") {
    auto ran = false;

    auto const working = [&]() -> task<int> {
      co_await async::offload([&] { ran = true; });
      co_return 1;
    };

    REQUIRE(run_offering(working(), calls) == 1);
    REQUIRE(ran);
    REQUIRE(calls == 1);
  }

  SECTION("what it throws comes out of the task, with a value and without") {
    auto const with_value = []() -> task<int> {
      co_return co_await async::offload([]() -> int { throw std::runtime_error{"from the work"}; });
    };
    auto const without = []() -> task<int> {
      co_await async::offload([] { throw std::runtime_error{"from the work"}; });
      co_return 1;
    };

    REQUIRE_THROWS_AS(run_offering(with_value(), calls), std::runtime_error);
    REQUIRE_THROWS_AS(run_offering(without(), calls), std::runtime_error);
    REQUIRE(calls == 2);
  }
}

TEST_CASE("offload: a stop requested before it starts cancels it, on an executor that offers nothing too", "[async]") {
  async::run_loop loop;
  std::stop_source stop;
  auto ran = false;

  auto const working = [&ran]() -> task<int> {
    co_await async::offload([&ran] { ran = true; });
    co_return 1;
  };

  stop.request_stop();
  // Ended on the stop: test::run says so.
  REQUIRE_THROWS_WITH(
      quoll::test::run(loop, working(), stop.get_token()), "quoll::test::run: the task ended on a stop"
  );
  REQUIRE_FALSE(ran);
}

TEST_CASE("offload: it calls what it holds as what it holds", "[async]") {
  struct two_ways {
    int operator()() & { return 1; }

    std::string operator()() && { return "moved"; }
  };

  static_assert(std::same_as<decltype(async::offload(two_ways{})), task<int>>);

  async::run_loop loop;

  auto const working = []() -> task<int> { co_return co_await async::offload(two_ways{}); };

  REQUIRE(quoll::test::run(loop, working()) == 1);
}
