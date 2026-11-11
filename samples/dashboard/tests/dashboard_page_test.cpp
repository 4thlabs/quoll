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

// The page of samples/dashboard/web, run natively: the calls to the server are defined here, over products in memory,
// in place of products_client.cpp, which goes through fetch. The chart is a widget, which memory_renderer logs.

#include "components/dashboard_page.hpp"
#include "product.hpp"
#include "products_client.hpp"

#include <quoll/async/sleep_for.hpp>
#include <quoll/async/task.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::session;
using quoll::test::settle;
using quoll::test::virtual_time;

using dashboard_app = quoll::ui::app<dashboard_web::dashboard_page, memory_renderer>;

// What the server holds, for the test running.
struct fake_server {
  std::vector<products::product> items;
  std::map<std::int64_t, products::monthly_sales> sales;
  // How long the sales of a product take to come.
  std::map<std::int64_t, std::chrono::milliseconds> delays;
  // The products whose sales fail, once their delay has passed.
  std::set<std::int64_t> failing;
  bool refuses = false;
};

fake_server server;

[[nodiscard]] fake_server three_products() {
  return {
      .items =
          {
              {.id = 1, .name = "Espresso beans", .category = "Coffee", .price = 12.5, .stock = 40},
              {.id = 2, .name = "Green tea", .category = "Tea", .price = 8, .stock = 60},
              {.id = 3, .name = "Grinder", .category = "Gear", .price = 49, .stock = 0},
          },
      .sales =
          {
              {1, {.months = {"Jan", "Feb"}, .units = {10, 20}}},
              {2, {.months = {"Jan", "Feb", "Mar"}, .units = {40, 50, 60}}},
              {3, {.months = {"Jan"}, .units = {5}}},
          },
      .delays = {},
      .failing = {},
      .refuses = false,
  };
}

// The pieces of the page a user acts on, found as a user finds them: by what they say.
struct page_driver {
  dashboard_app &instance;
  memory_renderer const &shown;

  void filter(std::string text) const { instance.dispatch(shown.find("input"), "input", std::move(text)); }

  // A row is a button around the product's name.
  void choose(std::string_view name) const { instance.dispatch(shown.parent(shown.find_text(name)), "click"); }

  // A field is a label around its name and its input.
  void type(std::string_view field, std::string text) const {
    auto const label = shown.parent(shown.find_text(field));

    instance.dispatch(shown.children(label).back(), "input", std::move(text));
  }

  // What is typed reaches the detail through the field's callback, a task of the detail: it has run by the next
  // event, as in a browser, where a click comes after the input.
  [[nodiscard]] task<> type_and_settle(std::string_view field, std::string text) const {
    type(field, std::move(text));
    co_await settle();
  }

  void save() const { instance.dispatch(shown.find_text("Save"), "click"); }

  void select_point(int index) const {
    instance.dispatch(shown.find("canvas"), "point_selected", "{\"index\":" + std::to_string(index) + "}");
  }
};

// The props of the chart, as its module last got them.
[[nodiscard]] std::string chart_props(memory_renderer const &shown) {
  auto const log = shown.widget_log();
  auto const last = std::ranges::find_if(log.rbegin(), log.rend(), [](std::string const &line) {
    return line.starts_with("mount ") || line.starts_with("update ");
  });

  return last == log.rend() ? std::string{} : *last;
}

} // namespace

namespace products_client {

task<std::vector<products::product>> list() { co_return server.items; }

task<products::product> update(std::int64_t id, products::product_update changes) {
  if (server.refuses) {
    throw std::runtime_error{"the server is gone"};
  }

  auto &item = *std::ranges::find(server.items, id, &products::product::id);

  item.name = std::move(changes.name);
  item.price = changes.price;
  item.stock = changes.stock;
  co_return item;
}

task<products::monthly_sales> sales(std::int64_t id) {
  if (auto const delay = server.delays.find(id); delay != server.delays.end()) {
    co_await quoll::async::sleep_for(delay->second);
  }

  if (server.failing.contains(id)) {
    throw std::runtime_error{"no sales for now"};
  }

  co_return server.sales.at(id);
}

} // namespace products_client

TEST_CASE("dashboard web: the list filters, a product chosen shows its sales in the chart, and a point its figure",
          "[samples][dashboard]") {
  server = three_products();

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown};
  page_driver const user{instance, shown};
  std::string loaded;
  std::string filtered;
  std::string chart;
  std::string figure;

  auto const act = [&]() -> task<> {
    co_await settle();
    loaded = shown.html();

    user.filter("TEA");
    co_await settle();
    filtered = shown.html();

    user.choose("Green tea");
    co_await settle();
    chart = chart_props(shown);

    user.select_point(1);
    co_await settle();
    figure = shown.html();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(loaded.contains("Espresso beans"));
  CHECK(loaded.contains("Grinder"));
  CHECK(loaded.contains("out of stock"));
  CHECK(filtered.contains("Green tea"));
  CHECK_FALSE(filtered.contains("Grinder"));
  CHECK(chart == R"(mount ../widgets/line_chart.js {"months":["Jan","Feb","Mar"],"units":[40,50,60]})");
  CHECK(figure.contains("Feb: 50 units"));
}

TEST_CASE("dashboard web: the detail saves what is typed, and refuses a price that is no number",
          "[samples][dashboard]") {
  server = three_products();

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown};
  page_driver const user{instance, shown};
  std::string refused;
  std::string infinite;

  auto const act = [&]() -> task<> {
    co_await settle();
    user.choose("Grinder");
    co_await settle();

    co_await user.type_and_settle("Price", "a lot");
    user.save();
    co_await settle();
    refused = shown.html();

    // A number from_chars reads, which JSON cannot hold.
    co_await user.type_and_settle("Price", "inf");
    user.save();
    co_await settle();
    infinite = shown.html();

    co_await user.type_and_settle("Price", "45.5");
    co_await user.type_and_settle("Stock", "7");
    user.save();
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(refused.contains("The price and the stock are numbers"));
  CHECK(infinite.contains("The price and the stock are numbers"));
  CHECK(server.items[2] ==
        products::product{.id = 3, .name = "Grinder", .category = "Gear", .price = 45.5, .stock = 7});
  CHECK(instance.component().items->at(2) == server.items[2]);
  CHECK(instance.component().problem->empty());
}

TEST_CASE("dashboard web: a change the server refuses shows why, and stays typed for the next save",
          "[samples][dashboard]") {
  server = three_products();
  server.refuses = true;

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown};
  page_driver const user{instance, shown};
  std::string refused;

  auto const act = [&]() -> task<> {
    co_await settle();
    user.choose("Green tea");
    co_await settle();

    co_await user.type_and_settle("Name", "Sencha");
    user.save();
    co_await settle();
    refused = shown.html();

    // Nothing typed since: the save sends the name that stayed.
    server.refuses = false;
    user.save();
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(refused.contains("role=\"alert\""));
  CHECK(refused.contains("the server is gone"));
  CHECK(server.items[1].name == "Sencha");
  CHECK(instance.component().problem->empty());
}

TEST_CASE("dashboard web: the sales of a product chosen before are not shown once another is", "[samples][dashboard]") {
  server = three_products();
  server.delays = {{1, 10ms}, {2, 1ms}};

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown};
  page_driver const user{instance, shown};

  auto const act = [&]() -> task<> {
    co_await settle();

    // The first product's sales come after the second's.
    user.choose("Espresso beans");
    user.choose("Green tea");
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(chart_props(shown) == R"(mount ../widgets/line_chart.js {"months":["Jan","Feb","Mar"],"units":[40,50,60]})");
  CHECK(instance.component().sales->value() == server.sales.at(2));
}

TEST_CASE("dashboard web: the sales of the product chosen that fail show why, those of one left say nothing",
          "[samples][dashboard]") {
  server = three_products();
  server.delays = {{1, 10ms}, {2, 1ms}};
  server.failing = {1, 3};

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown};
  page_driver const user{instance, shown};
  std::string left;
  std::string failed;

  auto const act = [&]() -> task<> {
    co_await settle();

    // The first product's sales fail after the second's have come.
    user.choose("Espresso beans");
    user.choose("Green tea");
    co_await quoll::async::sleep_for(20ms);
    left = shown.html();

    user.choose("Grinder");
    co_await settle();
    failed = shown.html();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK_FALSE(left.contains("no sales for now"));
  CHECK(left.contains("<canvas"));
  CHECK(failed.contains("no sales for now"));
  CHECK_FALSE(failed.contains("Loading its sales"));
}

TEST_CASE("dashboard web: a month without units, from sales that do not match, shows no figure",
          "[samples][dashboard]") {
  server = three_products();
  server.sales[2] = {.months = {"Jan", "Feb", "Mar"}, .units = {40}};

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown};
  page_driver const user{instance, shown};
  std::string first;
  std::string third;

  auto const act = [&]() -> task<> {
    co_await settle();
    user.choose("Green tea");
    co_await settle();

    user.select_point(0);
    co_await settle();
    first = shown.html();

    user.select_point(2);
    co_await settle();
    third = shown.html();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(first.contains("Jan: 40 units"));
  // The click on a month with no units is no point: the figure of the last one stays.
  CHECK(third.contains("Jan: 40 units"));
  CHECK_FALSE(third.contains("Mar"));
}
