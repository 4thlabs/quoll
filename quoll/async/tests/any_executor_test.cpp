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

#include <quoll/async.hpp>
#include <quoll/async/offers_service.hpp>
#include <quoll/test/overloaded.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace {

namespace async = quoll::async;

using async::any_executor;
using async::run_loop;

// A service an executor may offer.
struct greeting {
  std::string text;
};

// An executor offering a service, posting to a run_loop; `padding` makes it too big to be held in place.
template <std::size_t Padding>
class greeting_executor {
public:
  using services = async::service_list<greeting>;

  greeting_executor(run_loop &loop, std::string text) : loop_(loop.get_executor()), text_(std::move(text)) {}

  void post(async::work_item &item) const { loop_.post(item); }

  [[nodiscard]] greeting query(async::service_tag<greeting>) const { return {text_}; }

  [[nodiscard]] bool operator==(greeting_executor const &other) const noexcept { return text_ == other.text_; }

private:
  run_loop::executor loop_;
  std::string text_;
  std::array<char, Padding> padding_{};
};

using big_executor = greeting_executor<64>;

static_assert(async::OffersService<big_executor, greeting>);
static_assert(!async::OffersService<run_loop::executor, greeting>);

async::task<std::string> greet() {
  auto const executor = co_await async::current_executor();

  co_return executor.query<greeting>().value_or(greeting{"none"}).text;
}

} // namespace

TEST_CASE("any_executor posts to a copy of the executor it wraps", "[async][any_executor]") {
  run_loop loop;
  any_executor const executor{loop.get_executor()};
  async::work_item item{std::noop_coroutine()};

  executor.post(item);

  REQUIRE(loop.run() == 1);
  REQUIRE(executor.target<run_loop::executor>() != nullptr);
  REQUIRE(*executor.target<run_loop::executor>() == loop.get_executor());
  REQUIRE(executor.target<big_executor>() == nullptr);
}

TEST_CASE("any_executor copies, moves and compares executors held in place or on the heap", "[async][any_executor]") {
  run_loop loop;

  for (auto const &original : {any_executor{loop.get_executor()}, any_executor{big_executor{loop, "hello"}}}) {
    any_executor copy{original};

    REQUIRE(copy == original);

    any_executor moved{std::move(copy)};

    REQUIRE(moved == original);

    any_executor assigned{loop.get_executor()};

    assigned = moved;
    REQUIRE(assigned == original);
    assigned = std::move(moved);
    REQUIRE(assigned == original);

    async::work_item item{std::noop_coroutine()};

    assigned.post(item);
    REQUIRE(loop.run() == 1);
  }

  REQUIRE(any_executor{big_executor{loop, "a"}} != any_executor{big_executor{loop, "b"}});
  REQUIRE(any_executor{big_executor{loop, "a"}} != any_executor{loop.get_executor()});
}

TEST_CASE("any_executor copies what an executor_ref references, which may then go away", "[async][any_executor]") {
  run_loop loop;
  std::optional<any_executor> kept;

  {
    big_executor const executor{loop, "kept"};

    kept.emplace(async::executor_ref{executor});
  }

  REQUIRE(kept->query<greeting>()->text == "kept");

  // An executor_ref to an any_executor references the executor it wraps.
  async::executor_ref const ref{*kept};

  REQUIRE(ref.target<big_executor>() != nullptr);
  REQUIRE(ref.query<greeting>()->text == "kept");
  REQUIRE(any_executor{ref}.target<big_executor>() != nullptr);
}

TEST_CASE("executors answer the services they offer, through every erasure", "[async][any_executor]") {
  run_loop loop;
  big_executor const executor{loop, "hi"};

  REQUIRE(executor.query(async::service<greeting>).text == "hi");
  REQUIRE(async::executor_ref{executor}.query<greeting>()->text == "hi");
  REQUIRE(any_executor{executor}.query<greeting>()->text == "hi");
  REQUIRE_FALSE(any_executor{loop.get_executor()}.query<greeting>());
}

TEST_CASE("a task finds the services of the executor it runs on", "[async][any_executor]") {
  run_loop loop;
  std::optional<std::string> greeted;

  co_spawn(any_executor{big_executor{loop, "from the executor"}}, greet(),
           quoll::test::overloaded{[&](std::string text) { greeted = std::move(text); }, [](std::exception_ptr) {}});
  loop.run();

  REQUIRE(greeted == "from the executor");
  REQUIRE(quoll::test::run(greet()) == "none");
}
