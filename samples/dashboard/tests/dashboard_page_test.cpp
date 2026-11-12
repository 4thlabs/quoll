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

#include "components/pages.hpp"
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
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::session;
using quoll::test::settle;
using quoll::test::virtual_time;

using dashboard_app = quoll::ui::app<dashboard_web::app_shell, memory_renderer>;

// One call for sales: how long it takes, and whether it fails then.
struct sales_call {
  std::chrono::milliseconds delay{0};
  bool fails = false;
};

// What the server holds, for the test running.
struct fake_server {
  std::vector<products::product> items;
  std::map<std::int64_t, products::monthly_sales> sales;
  // How long the sales of a product take to come.
  std::map<std::int64_t, std::chrono::milliseconds> delays;
  // The products whose sales fail, once their delay has passed.
  std::set<std::int64_t> failing;
  // What the next calls for a product's sales do, in turn, before the delays and failures above.
  std::map<std::int64_t, std::deque<sales_call>> calls;
  // How long a save takes to be answered, the next ones in turn, then all.
  std::deque<std::chrono::milliseconds> update_delays;
  std::chrono::milliseconds update_delay{0};
  bool refuses = false;
  // A save throws what is no std::exception.
  bool throws_other = false;
  // The list fails with a message that says nothing.
  bool list_fails = false;
  // The saves the server received, and the sales it was asked for.
  int updates = 0;
  int sales_asked = 0;
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
      .calls = {},
      .update_delays = {},
      .update_delay = {},
      .refuses = false,
      .throws_other = false,
      .list_fails = false,
      .updates = 0,
      .sales_asked = 0,
  };
}

// The pieces of the page a user acts on, found as a user finds them: by what they say.
struct page_driver {
  dashboard_app &instance;
  memory_renderer const &shown;

  void filter(std::string text) const { instance.dispatch(shown.find("input"), "input", std::move(text)); }

  // A row is a link around the product's name.
  void choose(std::string_view name) const { instance.dispatch(shown.parent(shown.find_text(name)), "click"); }

  // A field is a label around its name and its input.
  void type(std::string_view field, std::string text) const {
    auto const label = shown.parent(shown.find_text(field));

    instance.dispatch(shown.children(label).back(), "input", std::move(text));
  }

  // What is typed shows once the detail has viewed again, as the form tells it: settled before the next event, as in
  // a browser, where a click comes after the input.
  [[nodiscard]] task<> type_and_settle(std::string_view field, std::string text) const {
    type(field, std::move(text));
    co_await settle();
  }

  void save() const { instance.dispatch(shown.find_text("Save"), "click"); }

  void try_again() const { instance.dispatch(shown.find_text("Try again"), "click"); }

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

task<std::vector<products::product>> list() {
  if (server.list_fails) {
    throw std::runtime_error{""};
  }

  co_return server.items;
}

task<products::product> update(std::int64_t id, products::product_update changes) {
  ++server.updates;

  // As the server is when the save is sent.
  bool const refused = server.refuses;

  if (server.throws_other) {
    throw 42;
  }

  auto delay = server.update_delay;

  if (!server.update_delays.empty()) {
    delay = server.update_delays.front();
    server.update_delays.pop_front();
  }

  if (delay > std::chrono::milliseconds{0}) {
    co_await quoll::async::sleep_for(delay);
  }

  if (refused) {
    throw std::runtime_error{"the server is gone"};
  }

  auto &item = *std::ranges::find(server.items, id, &products::product::id);

  item.name = std::move(changes.name);
  item.price = changes.price;
  item.stock = changes.stock;
  co_return item;
}

task<products::monthly_sales> sales(std::int64_t id) {
  ++server.sales_asked;

  auto const delay = server.delays.find(id);
  sales_call call{.delay = delay == server.delays.end() ? std::chrono::milliseconds{0} : delay->second,
                  .fails = server.failing.contains(id)};

  if (auto const planned = server.calls.find(id); planned != server.calls.end() && !planned->second.empty()) {
    call = planned->second.front();
    planned->second.pop_front();
  }

  if (call.delay > std::chrono::milliseconds{0}) {
    co_await quoll::async::sleep_for(call.delay);
  }

  if (call.fails) {
    throw std::runtime_error{"no sales for now"};
  }

  if (server.throws_other) {
    throw 42;
  }

  co_return server.sales.at(id);
}

} // namespace products_client

TEST_CASE("dashboard web: the list filters, a product chosen shows its sales in the chart, and a point its figure",
          "[samples][dashboard]") {
  server = three_products();

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
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
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
  page_driver const user{instance, shown};
  std::string refused;
  std::string infinite;

  auto const act = [&]() -> task<> {
    co_await settle();
    user.choose("Grinder");
    co_await settle();

    // A click on the button locked, as a script may send: refused, as the form is not valid.
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
  CHECK(refused.contains(R"(<button class="btn btn-primary self-start" disabled="">Save</button>)"));
  CHECK(infinite.contains("The price and the stock are numbers"));
  // A click while the fields are wrong sends nothing, nor shows a failed save: the one save is the last.
  CHECK_FALSE(refused.contains("Not saved"));
  CHECK(server.updates == 1);
  CHECK(server.items[2] ==
        products::product{.id = 3, .name = "Grinder", .category = "Gear", .price = 45.5, .stock = 7});
  CHECK(instance.context<dashboard_web::catalog>().items->value.at(2) == server.items[2]);
  CHECK(instance.context<dashboard_web::catalog>().items->error == nullptr);
}

TEST_CASE("dashboard web: a change the server refuses shows why, and stays typed for the next save",
          "[samples][dashboard]") {
  server = three_products();
  server.refuses = true;

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
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
  CHECK(instance.context<dashboard_web::catalog>().items->error == nullptr);
}

TEST_CASE("dashboard web: the sales of a product chosen before are not shown once another is", "[samples][dashboard]") {
  server = three_products();
  server.delays = {{1, 10ms}, {2, 1ms}};

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
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
}

TEST_CASE("dashboard web: the sales of the product chosen that fail show why, those of one left say nothing",
          "[samples][dashboard]") {
  server = three_products();
  server.delays = {{1, 10ms}, {2, 1ms}};
  server.failing = {1, 3};

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
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
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
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

TEST_CASE("dashboard web: a field changed while a save is on its way keeps what it holds", "[samples][dashboard]") {
  server = three_products();
  server.update_delay = 10ms;

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
  page_driver const user{instance, shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    user.choose("Grinder");
    co_await settle();

    co_await user.type_and_settle("Price", "45.5");
    user.save();
    co_await settle();

    // Typed before the answer comes, in a field sent and in one not: not sent, and kept once it has come.
    co_await user.type_and_settle("Price", "46");
    co_await user.type_and_settle("Stock", "7");
    co_await quoll::async::sleep_for(20ms);

    user.save();
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(server.items[2] == products::product{.id = 3, .name = "Grinder", .category = "Gear", .price = 46, .stock = 7});
}

TEST_CASE("dashboard web: sales that failed load again on Try again, a product chosen again loading nothing",
          "[samples][dashboard]") {
  server = three_products();
  // The first request for its sales fails, the next succeeds.
  server.calls[1] = {{.delay = 1ms, .fails = true}, {.delay = 1ms, .fails = false}};

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
  page_driver const user{instance, shown};
  std::string failed;
  int asked_before = 0;

  auto const act = [&]() -> task<> {
    co_await settle();
    user.choose("Espresso beans");
    co_await quoll::async::sleep_for(5ms);
    failed = shown.html();

    // The same product chosen again: what the effect depends on did not change.
    asked_before = server.sales_asked;
    user.choose("Espresso beans");
    co_await quoll::async::sleep_for(5ms);
    CHECK(server.sales_asked == asked_before);

    user.try_again();
    co_await quoll::async::sleep_for(5ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(failed.contains("no sales for now"));
  CHECK(failed.contains("Try again"));
  CHECK_FALSE(shown.html().contains("no sales for now"));
  CHECK(chart_props(shown) == R"(mount ../widgets/line_chart.js {"months":["Jan","Feb"],"units":[10,20]})");
}

TEST_CASE("dashboard web: a failure of the sales and one of a save each stay, and a product chosen clears the first",
          "[samples][dashboard]") {
  server = three_products();
  server.failing = {3};
  server.delays = {{2, 10ms}};

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
  page_driver const user{instance, shown};
  std::string saved;
  std::string chosen;
  std::string refused;
  std::string loaded;

  auto const act = [&]() -> task<> {
    co_await settle();

    // The sales fail, then a save succeeds: the failure stays where the sales would be.
    user.choose("Grinder");
    co_await settle();
    co_await user.type_and_settle("Price", "45.5");
    user.save();
    co_await settle();
    saved = shown.html();

    // Another product chosen: its sales load, the last one's failure gone.
    user.choose("Green tea");
    co_await settle();
    chosen = shown.html();

    // A save refused while the sales load: their coming leaves its failure.
    server.refuses = true;
    co_await user.type_and_settle("Price", "9");
    user.save();
    co_await settle();
    refused = shown.html();
    co_await quoll::async::sleep_for(20ms);
    loaded = shown.html();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(saved.contains("no sales for now"));
  CHECK_FALSE(saved.contains("Loading its sales"));
  // The page's own alert, for the products, has nothing to say.
  CHECK_FALSE(saved.contains("alert alert-error"));
  CHECK_FALSE(chosen.contains("no sales for now"));
  CHECK(chosen.contains("Loading its sales"));
  CHECK(refused.contains("the server is gone"));
  CHECK(loaded.contains("the server is gone"));
  CHECK(loaded.contains("<canvas"));
}

TEST_CASE("dashboard web: a product saves once at a time, a click while it is on its way refused",
          "[samples][dashboard]") {
  server = three_products();
  server.update_delay = 10ms;

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
  page_driver const user{instance, shown};
  std::string waiting;
  int sent_meanwhile = 0;

  auto const act = [&]() -> task<> {
    co_await settle();
    user.choose("Grinder");
    co_await settle();

    // Two clicks in one turn, before the button waits: one save.
    co_await user.type_and_settle("Price", "45.5");
    user.save();
    user.save();
    co_await settle();
    waiting = shown.html();
    sent_meanwhile = server.updates;

    co_await quoll::async::sleep_for(20ms);
    co_await user.type_and_settle("Price", "46");
    user.save();
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(waiting.contains("Saving…"));
  CHECK(waiting.contains("disabled=\"\""));
  CHECK(sent_meanwhile == 1);
  CHECK(server.updates == 2);
  CHECK(server.items[2].price == 46);
  CHECK(instance.context<dashboard_web::catalog>().items->value.at(2).price == 46);
  CHECK_FALSE(shown.html().contains("Not saved"));
}

TEST_CASE("dashboard web: a save goes on once its detail is left, and the list shows it", "[samples][dashboard]") {
  server = three_products();
  server.update_delay = 10ms;

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
  page_driver const user{instance, shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    user.choose("Grinder");
    co_await settle();
    co_await user.type_and_settle("Price", "45.5");
    user.save();
    co_await settle();

    // Another product chosen while the save is on its way: the detail goes, and the page's save goes on.
    user.choose("Green tea");
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(server.updates == 1);
  CHECK(server.items[2].price == 45.5);
  CHECK(instance.context<dashboard_web::catalog>().items->value.at(2).price == 45.5);
}

TEST_CASE("dashboard web: a save ending on what is no std::exception is over, and says it failed",
          "[samples][dashboard]") {
  server = three_products();
  server.throws_other = true;

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
  page_driver const user{instance, shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    user.choose("Grinder");
    co_await settle();
    co_await user.type_and_settle("Price", "45.5");
    user.save();
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("the save failed"));
  CHECK_FALSE(shown.html().contains("Saving…"));
  CHECK(server.items[2].price == 49);
}

TEST_CASE("dashboard web: a save tried again shows it waits, and a detail chosen again starts with no failure",
          "[samples][dashboard]") {
  server = three_products();
  server.refuses = true;

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
  page_driver const user{instance, shown};
  std::string retrying;
  std::string reopened;

  auto const act = [&]() -> task<> {
    co_await settle();
    user.choose("Grinder");
    co_await settle();
    co_await user.type_and_settle("Price", "45.5");
    user.save();
    co_await settle();

    // Tried again, slowly: what shows is that it waits, not why the last one failed.
    server.refuses = false;
    server.update_delay = 10ms;
    user.save();
    co_await settle();
    retrying = shown.html();
    co_await quoll::async::sleep_for(20ms);

    // Refused once more, then chosen again: the new detail has tried no save.
    server.refuses = true;
    server.update_delay = {};
    co_await user.type_and_settle("Price", "9");
    user.save();
    co_await settle();
    user.choose("Green tea");
    co_await settle();
    user.choose("Grinder");
    co_await settle();
    reopened = shown.html();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(retrying.contains("Saving…"));
  CHECK_FALSE(retrying.contains("the server is gone"));
  CHECK(server.items[2].price == 45.5);
  CHECK_FALSE(reopened.contains("Not saved"));
}

TEST_CASE("dashboard web: sales ending on what is no std::exception say they did not load", "[samples][dashboard]") {
  server = three_products();

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};
  page_driver const user{instance, shown};

  auto const act = [&]() -> task<> {
    co_await settle();
    server.throws_other = true;
    user.choose("Grinder");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("the sales did not load"));
  CHECK_FALSE(shown.html().contains("Loading its sales"));
}

TEST_CASE("dashboard web: products that fail to load with nothing said still say they did not load",
          "[samples][dashboard]") {
  server = three_products();
  server.list_fails = true;

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options()};

  auto const act = []() -> task<> { co_await settle(); };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("the products did not load"));
  CHECK(shown.html().contains("role=\"alert\""));
}

TEST_CASE("dashboard web: a product's address opens its page, and the back button goes back to the list",
          "[samples][dashboard]") {
  server = three_products();

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options("/products/2")};
  page_driver const user{instance, shown};
  std::string opened;
  std::string other;

  auto const act = [&]() -> task<> {
    co_await settle();
    opened = shown.html();

    user.choose("Grinder");
    co_await settle();
    other = shown.html();

    instance.context<quoll::ui::router>().back();
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(opened.contains("Green tea</h2>"));
  CHECK(other.contains("Grinder</h2>"));

  // Back on the product before: its address, its page, its row chosen, and its chart, mounted again for it.
  CHECK(instance.context<quoll::ui::router>().where->path == "/products/2");
  CHECK(shown.html().contains("Green tea</h2>"));
  CHECK(shown.html().contains("bg-base-200\" data-quoll-link=\"\" href=\"/products/2\""));
  CHECK(chart_props(shown) == R"(mount ../widgets/line_chart.js {"months":["Jan","Feb","Mar"],"units":[40,50,60]})");
}

TEST_CASE("dashboard web: the root goes to the products, a product none holds and an unknown address say so, asking "
          "for no sales",
          "[samples][dashboard]") {
  struct seen {
    std::string start;
    std::string path;
    std::string shown;
  };

  std::vector<seen> pages = {
      {"/", "/products", "Choose a product"},
      {"/products/42", "/products/42", "No such product"},
      {"/nowhere", "/nowhere", "<a class=\"link\" data-quoll-link=\"\" href=\"/products\">See the products</a>"},
  };

  for (auto const &[start, path, shown_there] : pages) {
    server = three_products();

    virtual_time time;
    memory_renderer const shown;
    dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options(start)};

    auto const act = []() -> task<> { co_await settle(); };

    REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
    INFO(start);
    CHECK(instance.context<quoll::ui::router>().where->path == path);
    CHECK(shown.html().contains(shown_there));
    CHECK(server.sales_asked == 0);
  }
}

TEST_CASE("dashboard web: a product's own row goes nowhere new, the back button leaving its page at once",
          "[samples][dashboard]") {
  server = three_products();

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options("/products")};
  page_driver const user{instance, shown};
  std::string first;

  auto const act = [&]() -> task<> {
    co_await settle();
    user.choose("Green tea");
    co_await settle();

    // Its row again: the history holds one entry for it.
    user.choose("Green tea");
    co_await settle();

    instance.context<quoll::ui::router>().back();
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(instance.context<quoll::ui::router>().where->path == "/products");
  CHECK(shown.html().contains("Choose a product"));
}

TEST_CASE("dashboard web: a product's address says it loads, not that it is missing, before the products came",
          "[samples][dashboard]") {
  server = three_products();

  virtual_time time;
  memory_renderer const shown;
  dashboard_app instance{time.get_executor(), shown, dashboard_web::page_options("/products/2")};
  std::string first;

  // Its first view, before the catalog's load ran.
  auto const act = [&]() -> task<> {
    first = shown.html();
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(first.contains("Loading the product"));
  CHECK_FALSE(first.contains("No such product"));
  CHECK(shown.html().contains("Green tea</h2>"));
}
