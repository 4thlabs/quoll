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

#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace resource_test {

using namespace quoll::ui;
using namespace std::chrono_literals;

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::session;
using quoll::test::virtual_time;

using test_app = app<memory_renderer>;

std::vector<std::int64_t> loads;
int failures_left = 0;

// A later id loads faster: the load of an id left behind would end last, and show, unless stopped.
task<std::string> sales_of(std::int64_t id) {
  loads.push_back(id);
  co_await quoll::async::sleep_for(std::chrono::milliseconds{10 - id});

  if (failures_left > 0) {
    --failures_left;
    throw std::runtime_error{"no sales"};
  }

  co_return "sales of " + std::to_string(id);
}

element told(std::exception_ptr const &error) {
  try {
    std::rethrow_exception(error);
  } catch (std::exception const &failure) {
    return p(std::string{"failed: "} + failure.what());
  }
}

// What it loads follows a prop; while it loads again, it keeps what it showed.
struct sales_view : component<sales_view> {
  struct props {
    std::int64_t id = 0;

    bool operator==(props const &) const = default;
  };

  resource<std::string> sales{&props::id, sales_of};

  element view() {
    return sales.match(
        [](std::optional<std::string> const &before) { return p(before ? "again, " + *before : "loading"); },
        [](std::exception_ptr const &error) { return told(error); }, [](std::string const &loaded) { return p(loaded); }
    );
  }
};

inline constexpr tag<sales_view> sales_view;

struct sales_data {
  state<std::int64_t> id{1};
};

struct sales_page : component<sales_page> {
  element view() {
    auto &self = use_context<sales_data>();

    return sales_view({.id = *self->id});
  }
};

inline constexpr tag<sales_page> sales_page;

// One that may follow nothing yet: an optional holding none, before which it shows `idle`.
struct chosen_view : component<chosen_view> {
  struct props {
    std::optional<std::int64_t> id;

    bool operator==(props const &) const = default;
  };

  resource<std::string> sales{[](props const &given) { return given.id; }, sales_of};

  element view() {
    return sales.match(
        [] { return p("pick one"); },
        [](std::optional<std::string> const &before) { return p(before ? "again" : "loading"); },
        [](std::exception_ptr const &error) { return told(error); }, [](std::string const &loaded) { return p(loaded); }
    );
  }
};

inline constexpr tag<chosen_view> chosen_view;

struct chosen_data {
  state<std::optional<std::int64_t>> id;
};

struct chosen_page : component<chosen_page> {
  element view() {
    auto &self = use_context<chosen_data>();

    return chosen_view({.id = *self->id});
  }
};

inline constexpr tag<chosen_page> chosen_page;

// With three functions, what follows holding none shows the loading one.
struct unchosen_view : component<unchosen_view> {
  struct props {
    std::optional<std::int64_t> id;

    bool operator==(props const &) const = default;
  };

  resource<std::string> sales{&props::id, sales_of};

  element view() {
    return sales.match(
        [this] { return p(sales.loading() ? "loading" : "waiting"); },
        [](std::exception_ptr const &error) { return told(error); }, [](std::string const &loaded) { return p(loaded); }
    );
  }
};

inline constexpr tag<unchosen_view> unchosen_view;

struct unchosen_page : component<unchosen_page> {
  element view() { return unchosen_view({}); }
};

inline constexpr tag<unchosen_page> unchosen_page;

// A match is given each case: one forgotten does not compile.
template <class R>
concept matches_without_loaded =
    requires(R const &sales, element (*loading)(), element (*failed)(std::exception_ptr const &)) {
      sales.match(loading, failed);
    };

static_assert(!matches_without_loaded<resource<std::string>>);

// One that follows a state of its own, std::tie(...), and whose button reloads it.
struct retried_view : component<retried_view> {
  state<std::int64_t> id{4};
  resource<std::string> sales{std::tie(id), sales_of};

  element view() {
    auto const shown = sales.match(
        [] { return p("loading"); }, [](std::exception_ptr const &error) { return told(error); },
        [](std::string const &loaded) { return p(loaded); }
    );

    return div({shown, button(sales.loading() ? "busy" : "retry") | on_click([this] { sales.reload(); })});
  }
};

inline constexpr tag<retried_view> retried_view;

// A context's, on one of its states, debounced.
struct catalog {
  state<std::int64_t> id{6};
  resource<std::string> sales{id, sales_of, {.debounce = 5ms}};
};

struct catalog_page : component<catalog_page> {
  element view() {
    auto &found = use_context<catalog>();

    return found->sales.match(
        [] { return p("loading"); }, [](std::exception_ptr const &error) { return told(error); },
        [](std::string const &loaded) { return p(loaded); }
    );
  }
};

inline constexpr tag<catalog_page> catalog_page;

} // namespace resource_test

using namespace resource_test;

TEST_CASE("resource: it loads what a prop gives, again as it changes, keeping what it showed", "[ui][resource]") {
  loads.clear();
  failures_left = 0;

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, sales_page, {.contexts = {provide<sales_data>()}}};

  auto const act = [&]() -> task<> {
    CHECK(shown.html() == "<p>loading</p>");

    co_await quoll::async::sleep_for(10ms);
    CHECK(shown.html() == "<p>sales of 1</p>");

    // Changed again while it loads: the load of 2, slower, stopped as 3 comes, rather than end after it and show.
    instance.context<sales_data>().id = 2;
    co_await quoll::test::settle();
    CHECK(shown.html() == "<p>again, sales of 1</p>");

    instance.context<sales_data>().id = 3;
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<p>sales of 3</p>");
  CHECK(loads == std::vector<std::int64_t>{1, 2, 3});
}

TEST_CASE("resource: with four functions, the first shows it before anything is followed", "[ui][resource]") {
  loads.clear();
  failures_left = 0;

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, chosen_page, {.contexts = {provide<chosen_data>()}}};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(10ms);
    CHECK(shown.html() == "<p>pick one</p>");
    CHECK(loads.empty());

    instance.context<chosen_data>().id = 5;
    co_await quoll::test::settle();
    CHECK(shown.html() == "<p>loading</p>");

    co_await quoll::async::sleep_for(10ms);
    CHECK(shown.html() == "<p>sales of 5</p>");

    // None again: what was loaded goes with it.
    instance.context<chosen_data>().id = std::nullopt;
    co_await quoll::async::sleep_for(10ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<p>pick one</p>");
  CHECK(loads == std::vector<std::int64_t>{5});
}

TEST_CASE("resource: a failure shows, and reload() loads it again", "[ui][resource]") {
  loads.clear();
  failures_left = 1;

  std::vector<std::string> errors;
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, retried_view, {.on_error = [&errors](std::exception_ptr const &error) {
                      try {
                        std::rethrow_exception(error);
                      } catch (std::exception const &failure) {
                        errors.emplace_back(failure.what());
                      }
                    }}};

  auto const act = [&]() -> task<> {
    co_await quoll::async::sleep_for(10ms);
    CHECK(shown.html() == "<div><p>failed: no sales</p><button>retry</button></div>");

    instance.dispatch(shown.find("button"), "click");
    co_await quoll::test::settle();
    CHECK(shown.html() == "<div><p>loading</p><button>busy</button></div>");

    co_await quoll::async::sleep_for(10ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<div><p>sales of 4</p><button>retry</button></div>");
  CHECK(errors == std::vector<std::string>{"no sales"});
  CHECK(loads == std::vector<std::int64_t>{4, 4});
}

TEST_CASE("resource: outside the making of a struct, or with no function, it is refused", "[ui][resource]") {
  using Catch::Matchers::ContainsSubstring;

  state<std::int64_t> id{1};

  CHECK_THROWS_WITH(
      (resource<std::string>{&sales_view::props::id, sales_of}), ContainsSubstring("is a member of a component written")
  );
  CHECK_THROWS_WITH(
      (resource<std::string>{id, static_cast<task<std::string> (*)(std::int64_t)>(nullptr)}),
      ContainsSubstring("no function")
  );
}

TEST_CASE("resource: a context's loads as it is provided, its changes debounced", "[ui][resource]") {
  loads.clear();
  failures_left = 0;

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, catalog_page, {.contexts = {provide<catalog>()}}};

  auto const act = [&]() -> task<> {
    CHECK(shown.html() == "<p>loading</p>");
    co_await quoll::async::sleep_for(10ms);
    CHECK(shown.html() == "<p>sales of 6</p>");

    // A change waits for the pause before it loads.
    instance.context<catalog>().id = 7;
    co_await quoll::async::sleep_for(2ms);
    CHECK(loads == std::vector<std::int64_t>{6});
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<p>sales of 7</p>");
  CHECK(loads == std::vector<std::int64_t>{6, 7});
}

TEST_CASE("resource: what it follows holding none while it loads stops that load", "[ui][resource]") {
  loads.clear();
  failures_left = 0;

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, chosen_page, {.contexts = {provide<chosen_data>()}}};

  auto const act = [&]() -> task<> {
    instance.context<chosen_data>().id = 5;
    co_await quoll::test::settle();
    CHECK(shown.html() == "<p>loading</p>");

    // None before it ends: stopped, and nothing it gives shows.
    instance.context<chosen_data>().id = std::nullopt;
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<p>pick one</p>");
  CHECK(loads == std::vector<std::int64_t>{5});
}

TEST_CASE("resource: with three functions, what follows holding none shows the loading one", "[ui][resource]") {
  loads.clear();

  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, unchosen_page};

  auto const act = []() -> task<> { co_await quoll::async::sleep_for(10ms); };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html() == "<p>waiting</p>");
  CHECK(loads.empty());
}

// What a load gives needs no ==: only what it follows is compared.
struct report {
  std::vector<int> rows;
};

task<report> report_of(std::int64_t id) { co_return report{.rows = {static_cast<int>(id)}}; }

TEST_CASE("resource: what it loads needs no ==", "[ui][resource]") {
  state<std::int64_t> id{1};
  resource<report> const loaded{id, report_of};

  CHECK_FALSE(loaded.loading());
}
