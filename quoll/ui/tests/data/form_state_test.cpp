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
#include <quoll/ui/data/form_state.hpp>
#include <quoll/ui/detail/data/linked_member.hpp>
#include <quoll/ui/detail/data/props_member.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace quoll::ui;
using namespace std::chrono_literals;

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::session;
using quoll::test::settle;
using quoll::test::virtual_time;

template <class C>
using test_app = app<C, memory_renderer>;

struct item {
  std::int64_t id = 0;
  std::string name;
  double price = 0;
  int stock = 0;
  bool active = false;

  [[nodiscard]] bool operator==(item const &) const = default;
};

// What was saved, in order.
std::vector<item> sent;

// Holds the item, and saves it in 10 ms, as a server would.
struct shop {
  state<item> current{item{.id = 1, .name = "tea", .price = 2.5, .stock = 3}};

  task<item> save(item edited) {
    co_await quoll::async::sleep_for(10ms);
    sent.push_back(edited);
    current = edited;
    co_return edited;
  }
};

struct editor {
  struct props {
    item value;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  shared<shop> data;
  form_state<&props::value> form;
  action<&shop::save> save{data};

  element view(props const &) {
    return div({
        label({text("Name"), input() | bind(form, &item::name)}),
        label({text("Price"), input() | bind(form, &item::price)}),
        label({text("Stock"), input() | bind(form, &item::stock)}),
        label({text("Active"), input() | bind(form, &item::active)}),
        when(!form.valid(), [] { return p("not numbers"); }),
        button("Save") | disabled(save->loading || !form.valid()) | on_click(save.with(form)),
    });
  }
};

struct shop_page {
  shared<shop> data;

  element view() { return component<editor>({.value = *data->current}); }
};

static_assert(detail::LinkedMember<form_state<&editor::props::value>>);
static_assert(detail::PropsMember<form_state<&editor::props::value>>);

// The input of the field labelled `name`.
[[nodiscard]] node_id field(memory_renderer const &shown, std::string_view name) {
  return shown.children(shown.find_text(name)).at(1);
}

// What the field labelled `name` shows, as its value attribute.
[[nodiscard]] std::string shown_in(memory_renderer const &shown, std::string_view name) {
  auto const html = shown.html();
  auto const label = html.find(std::string{name} + "<input");
  auto const start = html.find("value=\"", label);

  if (label == std::string::npos || start == std::string::npos) {
    return "?";
  }

  auto const from = start + 7;

  return html.substr(from, html.find('"', from) - from);
}

} // namespace

TEST_CASE("form_state: its fields show the value of the props, and what is typed", "[ui][form]") {
  sent = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<shop_page> instance{time.get_executor(), shown, {.contexts = {provide<shop>()}}};
  std::vector<std::string> before;
  std::vector<std::string> after;

  auto const act = [&]() -> task<> {
    co_await settle();
    before = {shown_in(shown, "Name"), shown_in(shown, "Price"), shown_in(shown, "Stock")};
    instance.dispatch(field(shown, "Price"), "input", "3.25");
    instance.dispatch(field(shown, "Name"), "input", "green tea");
    co_await settle();
    after = {shown_in(shown, "Name"), shown_in(shown, "Price"), shown_in(shown, "Stock")};
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(before == std::vector<std::string>{"tea", "2.5", "3"});
  CHECK(after == std::vector<std::string>{"green tea", "3.25", "3"});
  CHECK_FALSE(shown.html().contains("not numbers"));
}

TEST_CASE("form_state: a field typed wrong is kept as typed, the form not valid, a click with it refused",
          "[ui][form]") {
  sent = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<shop_page> instance{time.get_executor(), shown, {.contexts = {provide<shop>()}}};
  std::string typed;
  bool told_wrong = false;
  bool locked = false;
  bool refused_quietly = false;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(field(shown, "Price"), "input", "12,5");
    instance.dispatch(field(shown, "Stock"), "input", "7");
    co_await settle();
    typed = shown_in(shown, "Price");
    told_wrong = shown.html().contains("not numbers");
    locked = shown.html().contains("<button disabled=\"\">");

    // A click on the button locked, which the memory renderer sends all the same, as a script would: refused.
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(20ms);
    refused_quietly = sent.empty() && instance.context<shop>().current->stock == 3;

    // Typed right, it saves what reads: the price typed, the stock typed.
    instance.dispatch(field(shown, "Price"), "input", "12.5");
    co_await settle();
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(typed == "12,5");
  CHECK(told_wrong);
  CHECK(locked);
  CHECK(refused_quietly);
  REQUIRE(sent.size() == 1);
  CHECK(sent[0] == item{.id = 1, .name = "tea", .price = 12.5, .stock = 7});
  // Saved, the fields show what came back.
  CHECK(shown_in(shown, "Price") == "12.5");
  CHECK(shown_in(shown, "Stock") == "7");
  CHECK_FALSE(shown.html().contains("not numbers"));
}

TEST_CASE("form_state: saved, a field holding what came back is typed in no more, one typed meanwhile stays",
          "[ui][form]") {
  sent = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<shop_page> instance{time.get_executor(), shown, {.contexts = {provide<shop>()}}};
  std::string name_after_save;
  std::string price_after_save;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.dispatch(field(shown, "Name"), "input", "green tea");
    co_await settle();
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // While the save is on its way: the price changes, and is not sent.
    instance.dispatch(field(shown, "Price"), "input", "4");
    co_await quoll::async::sleep_for(20ms);
    name_after_save = shown_in(shown, "Name");
    price_after_save = shown_in(shown, "Price");

    // What came back is the item now: a name typed back to the old one is typed in, and sent as it is.
    instance.context<shop>().current = item{.id = 1, .name = "black tea", .price = 2.5, .stock = 3};
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(sent.size() == 1);
  CHECK(sent[0] == item{.id = 1, .name = "green tea", .price = 2.5, .stock = 3});
  CHECK(name_after_save == "green tea");
  CHECK(price_after_save == "4");
  // The name was typed in no more once saved: it shows what the props give now. The price is still typed.
  CHECK(shown_in(shown, "Name") == "black tea");
  CHECK(shown_in(shown, "Price") == "4");
}

TEST_CASE("form_state: with(form) sends the form as it stands at the click", "[ui][form]") {
  sent = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<shop_page> instance{time.get_executor(), shown, {.contexts = {provide<shop>()}}};

  auto const act = [&]() -> task<> {
    co_await settle();

    // The button's handler is the one of the view before the input: the click reads the form then.
    auto const button = shown.find("button");

    instance.dispatch(field(shown, "Stock"), "input", "9");
    instance.dispatch(button, "click");
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(sent.size() == 1);
  CHECK(sent[0].stock == 9);
}

TEST_CASE("form_state: a bool field is a checkbox, changed as it is clicked", "[ui][form]") {
  sent = {};

  virtual_time time;
  memory_renderer const shown;
  test_app<shop_page> instance{time.get_executor(), shown, {.contexts = {provide<shop>()}}};
  bool checked_before = true;
  bool checked_after = false;

  auto const act = [&]() -> task<> {
    co_await settle();
    checked_before = shown.html().contains("checked");
    instance.dispatch(field(shown, "Active"), "change", "true");
    co_await settle();
    checked_after = shown.html().contains("checked=\"\"");
    instance.dispatch(shown.find("button"), "click");
    co_await quoll::async::sleep_for(20ms);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(shown.html().contains("type=\"checkbox\""));
  CHECK_FALSE(checked_before);
  CHECK(checked_after);
  REQUIRE(sent.size() == 1);
  CHECK(sent[0].active);
}

namespace {

struct nameless {
  struct props {
    item value;
  };

  form_state<&props::value> form;

  element view(props const &) { return input() | bind(form, static_cast<std::string item::*>(nullptr)); }
};

struct nameless_page {
  element view() { return component<nameless>({}); }
};

} // namespace

TEST_CASE("form_state: bind() of no field throws", "[ui][form]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<nameless_page> instance{time.get_executor(), shown};

  CHECK_THROWS_AS(instance.start(), std::invalid_argument);
}

namespace {

// No ==: its form compares the fields typed in with the props at each view.
struct plain_item {
  double price = 0;
  int stock = 0;
};

struct plain_editor {
  struct props {
    plain_item value;
  };

  form_state<&props::value> form;

  element view(props const &) {
    return div({
        label({text("Price"), input() | bind(form, &plain_item::price)}),
        label({text("Stock"), input() | bind(form, &plain_item::stock)}),
    });
  }
};

struct plain_page {
  element view() { return component<plain_editor>({.value = {.price = 2.5, .stock = 3}}); }
};

} // namespace

TEST_CASE("form_state: what is typed stays as typed while it reads as the value shown, as 2.50 for a 2.5",
          "[ui][form]") {
  virtual_time time;
  memory_renderer const shown;
  test_app<plain_page> instance{time.get_executor(), shown};
  std::vector<std::string> seen;

  auto const act = [&]() -> task<> {
    co_await settle();

    for (auto const *const typed : {"2.5", "2.50", "03"}) {
      instance.dispatch(field(shown, typed[0] == '0' ? "Stock" : "Price"), "input", typed);
      co_await settle();
      seen.push_back(shown_in(shown, typed[0] == '0' ? "Stock" : "Price"));
    }
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  CHECK(seen == std::vector<std::string>{"2.5", "2.50", "03"});
}
