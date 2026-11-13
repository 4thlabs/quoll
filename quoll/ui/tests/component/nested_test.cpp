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
#include <quoll/async/work_item.hpp>
#include <quoll/test/memory_renderer.hpp>
#include <quoll/test/run.hpp>
#include <quoll/test/ui_session.hpp>
#include <quoll/test/virtual_time.hpp>
#include <quoll/ui.hpp>
#include <quoll/ui/view/node_id.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace quoll::ui;
using namespace std::chrono_literals;

using quoll::async::task;
using quoll::test::memory_renderer;
using quoll::test::session;
using quoll::test::settle;
using quoll::test::virtual_time;

using test_app = app<memory_renderer>;

// Says what comes and goes: its props tell where.
struct journal {
  std::vector<std::string> lines;
};

// Writes `line` into `log` as it is destroyed: what holds it says, as it goes, that it has.
class telling {
public:
  telling(journal *log, std::string line) noexcept : log_(log), line_(std::move(line)) {}

  telling(telling const &) = delete;
  telling(telling &&) = delete;
  telling &operator=(telling const &) = delete;
  telling &operator=(telling &&) = delete;

  ~telling() { log_->lines.push_back(line_); }

private:
  journal *log_;
  std::string line_;
};

// What comes and goes, as React's effects say it: an effect on nothing runs once its component first shows, and its
// run, stopped as the component goes, says so as the stop unwinds it.
void use_journal(journal *log, std::string const &label) {
  use_effect([log, label]() -> task<> {
    log->lines.push_back("mounted " + label);

    telling const going{log, "unmounted " + label};

    co_await quoll::async::sleep_for(24h);
  });
}

// A row with a count of its own, and the label its parent gives it.
struct row_props {
  std::string label;
  journal *log = nullptr;
};

constexpr component<row_props> row = [](row_props const &given) {
  if (given.label == "boom") {
    throw std::runtime_error{"no such row"};
  }

  auto &clicks = use_state(0);

  auto const add = [&clicks] { clicks = *clicks + 1; };

  use_journal(given.log, given.label);
  return button(given.label + ":" + std::to_string(*clicks)) | on_click(add);
};

// Rows by label, in the order it holds them, under a header that counts the views.
struct rows_data {
  state<std::vector<std::string>> labels{std::vector<std::string>{"a", "b"}};
  state<int> views;
  journal log;
};

constexpr component<> rows = [] {
  auto &self = use_context<rows_data>();

  auto const add = [&self] { self->views = *self->views + 1; };
  auto const row_of = [&self](std::string const &label) {
    return row({.label = label, .log = &self->log}) | key(label);
  };

  // Read, so that a change views it again.
  static_cast<void>(*self->views);

  return div({p("views") | on_click(add), ul({list(self->labels, row_of)})});
};

// Rows without a context, for a view written once.
constexpr component<> plain_rows = [] {
  return div({p("views"), ul({row({.label = "a"}) | key("a"), row({.label = "b"}) | key("b")})});
};

// A row whose click waits for an hour, so that it is still running as the row goes.
struct slow_row_props {
  journal *log = nullptr;
};

constexpr component<slow_row_props> slow_row = [](slow_row_props const &given) {
  auto const slow = [log = given.log]() -> task<> {
    // Destroyed as the stop unwinds it: the row is still there then, kept by this task.
    telling const ending{log, "handler ended"};

    co_await quoll::async::sleep_for(1h);
  };

  return button("slow") | on_click(slow);
};

struct maybe_slow_data {
  state<bool> shows{true};
  journal log;
};

constexpr component<> maybe_slow = [] {
  auto &self = use_context<maybe_slow_data>();

  return div({when(*self->shows, [&self] { return slow_row({.log = &self->log}); })});
};

// Its own journal, handed to a slow row: as the panel goes, it copies there what the row wrote, which only the row's
// task can have ended.
struct slow_panel_props {
  journal *log = nullptr;
};

// What the panel holds as long as it is there, made in its first view: as it goes, it copies `from` into `to`.
struct witness {
  journal *to = nullptr;
  journal from;

  witness() = default;
  witness(witness const &) = delete;
  witness(witness &&) = delete;
  witness &operator=(witness const &) = delete;
  witness &operator=(witness &&) = delete;

  ~witness() {
    if (to == nullptr) {
      return;
    }

    to->lines.insert(to->lines.end(), from.lines.begin(), from.lines.end());
    to->lines.emplace_back("panel gone");
  }
};

constexpr component<slow_panel_props> slow_panel = [](slow_panel_props const &given) {
  // A state's value lives as long as its component: set once, which views it once more.
  auto &last = use_state<std::shared_ptr<witness>>();

  if (!*last) {
    auto made = std::make_shared<witness>();

    made->to = given.log;
    last = std::move(made);
  }

  return slow_row({.log = &(*last)->from});
};

struct maybe_panel_data {
  state<bool> shows{true};
  journal log;
};

constexpr component<> maybe_panel = [] {
  auto &self = use_context<maybe_panel_data>();

  return div({when(*self->shows, [&self] { return slow_panel({.log = &self->log}); })});
};

// A count with no props.
constexpr component<> counter = [] {
  auto &count = use_state(0);

  auto const add = [&count] { count = *count + 1; };

  return p("count:" + std::to_string(*count)) | on_click(add);
};

// A row as its whole view.
struct panel_props {
  std::string label;
  journal *log = nullptr;
};

constexpr component<panel_props> panel = [](panel_props const &given) {
  use_journal(given.log, "panel");
  return row({.label = given.label, .log = given.log});
};

// Two cells, side by side in the view of whoever places it.
constexpr component<> cells = [] {
  return list(std::vector<std::string>{"1", "2"}, [](std::string const &cell) { return p(cell); });
};

// Components of three types, none keyed, a row at the same rank in its view and in the panel's.
struct mixed_data {
  state<int> views;
  journal log;
};

constexpr component<> mixed = [] {
  auto &self = use_context<mixed_data>();

  auto const add = [&self] { self->views = *self->views + 1; };

  // Read, so that a change views it again.
  static_cast<void>(*self->views);

  return div({
      p("views") | on_click(add),
      panel({.label = "x", .log = &self->log}),
      counter(),
      row({.label = "y", .log = &self->log}),
      cells(),
  });
};

// Two unkeyed rows, the first shown or not.
struct two_rows_data {
  state<bool> first{true};
  journal log;
};

constexpr component<> two_rows = [] {
  auto &self = use_context<two_rows_data>();

  return div({
      when(*self->first, [&self] { return row({.label = "a", .log = &self->log}); }),
      row({.label = "b", .log = &self->log}),
  });
};

struct stopping_row_props {
  std::function<task<>()> *stop = nullptr;
};

constexpr component<stopping_row_props> stopping_row = [](stopping_row_props const &given) {
  return button("Stop") | on_click([stop = given.stop]() -> task<> { co_await (*stop)(); });
};

struct stopping_parent_data {
  std::function<task<>()> *stop = nullptr;
};

constexpr component<> stopping_parent = [] {
  auto &self = use_context<stopping_parent_data>();

  return div({stopping_row({.stop = self->stop})});
};

// Shows a row, and says when it goes, after the row.
struct page_with_row_data {
  journal log;
};

constexpr component<> page_with_row = [] {
  auto &self = use_context<page_with_row_data>();

  use_journal(&self->log, "page");
  return div({row({.label = "a", .log = &self->log})});
};

// A handler reading its props by reference after a wait, while the parent gives others.
struct reader_props {
  std::string label;
  std::vector<std::string> *read = nullptr;
};

constexpr component<reader_props> reader = [](reader_props const &given) {
  return button(given.label) | on_click([&given]() -> task<> {
           co_await quoll::async::sleep_for(1min);
           given.read->push_back(given.label);
         });
};

struct reading_data {
  state<std::string> label{std::string{"old"}};
  std::vector<std::string> read;
};

constexpr component<> reading = [] {
  auto &self = use_context<reading_data>();

  return div({reader({.label = *self->label, .read = &self->read})});
};

// Its view is a fragment: the key of the component goes on each of its nodes.
struct cell_pair_props {
  std::string label;
};

constexpr component<cell_pair_props> cell_pair = [](cell_pair_props const &given) {
  return list(std::vector<std::string>{given.label + "1", given.label + "2"}, [](std::string const &cell) {
    return li(cell);
  });
};

struct cell_pairs_data {
  state<std::vector<std::string>> order{std::vector<std::string>{"a", "b"}};
};

constexpr component<> cell_pairs = [] {
  auto &self = use_context<cell_pairs_data>();

  return ul({list(self->order, [](std::string const &label) { return cell_pair({.label = label}) | key(label); })});
};

// A renderer failing its next insert once told to: a render that fails past the views.
struct failing_renderer {
  memory_renderer shown;
  bool *fails = nullptr;

  void create_element(node_id id, std::string_view tag) { shown.create_element(id, tag); }

  void create_text(node_id id, std::string_view content) { shown.create_text(id, content); }

  void set_text(node_id id, std::string_view content) { shown.set_text(id, content); }

  void set_class(node_id id, std::string_view classes) { shown.set_class(id, classes); }

  void set_attribute(node_id id, std::string_view name, std::string_view value) {
    shown.set_attribute(id, name, value);
  }

  void remove_attribute(node_id id, std::string_view name) { shown.remove_attribute(id, name); }

  void set_value(node_id id, std::string_view value) { shown.set_value(id, value); }

  void set_checked(node_id id, bool checked) { shown.set_checked(id, checked); }

  void listen(node_id id, std::string_view name) { shown.listen(id, name); }

  void unlisten(node_id id, std::string_view name) { shown.unlisten(id, name); }

  void insert(node_id parent, node_id child, node_id before) {
    if (*fails) {
      *fails = false;
      throw std::runtime_error{"no room"};
    }

    shown.insert(parent, child, before);
  }

  void remove(node_id id) { shown.remove(id); }
};

// Its view is a fragment with a node without a key, at position 0, and one with the key 0.
constexpr component<> headed = [] {
  return list(std::vector<int>{0, 1}, [](int item) { return item == 0 ? p("header") : li("first") | key(0); });
};

// Its view has the key it is given.
struct tagged_props {
  std::string own;
};

constexpr component<tagged_props> tagged = [](tagged_props const &given) {
  return given.own.empty() ? li("tagged") : li("tagged") | key(given.own);
};

constexpr component<> plain = [] { return p("plain"); };

// Its view has a key of its own, as its siblings' do.
constexpr component<> keyed_item = [] { return li("item") | key("1"); };

// Viewed twice: the reconciler then looks at the keys of the nodes it showed.
struct colliding_data {
  state<int> views;
};

constexpr component<> colliding = [] {
  auto &self = use_context<colliding_data>();

  // Read, so that a change views it again.
  static_cast<void>(*self->views);

  return ul({
      headed() | key("k"),
      counter() | key("k"),
      plain() | key("k"),
      tagged({.own = "b"}) | key("a"),
      tagged({}) | key("a\x1f"
                       "b"),
      keyed_item(),
      keyed_item(),
  });
};

// Gives its child a pointer into its own props.
struct quoting_child_props {
  std::string const *text = nullptr;
  std::vector<std::string> *read = nullptr;
};

constexpr component<quoting_child_props> quoting_child = [](quoting_child_props const &given) {
  return button("quote") | on_click([&given]() -> task<> {
           co_await quoll::async::sleep_for(1min);
           given.read->push_back(*given.text);
         });
};

struct quoting_props {
  std::string text;
  std::vector<std::string> *read = nullptr;
};

constexpr component<quoting_props> quoting = [](quoting_props const &given) {
  return quoting_child({.text = &given.text, .read = given.read});
};

struct quoted_data {
  state<std::string> label{std::string{"an old label, too long for a short string"}};
  std::vector<std::string> read;
};

constexpr component<> quoted = [] {
  auto &self = use_context<quoted_data>();

  return div({quoting({.text = *self->label, .read = &self->read})});
};

// A component with one of its own, which holds it, and a state that says when the component goes.
struct witnessed_props {
  journal *log = nullptr;
};

constexpr component<witnessed_props> witnessed = [](witnessed_props const &given) {
  auto &bye = use_state<std::shared_ptr<telling>>();

  if (!*bye) {
    bye = std::make_shared<telling>(given.log, "gone");
  }

  return div({counter()});
};

struct witnessing_data {
  journal *log = nullptr;
};

constexpr component<> witnessing = [] {
  auto &self = use_context<witnessing_data>();

  return div({witnessed({.log = self->log})});
};

struct refusing_executor {
  using services = quoll::async::service_list<quoll::async::timer_ref>;

  virtual_time::executor inner;
  bool *fail;

  [[nodiscard]] quoll::async::timer_ref query(quoll::async::service_tag<quoll::async::timer_ref> tag) const {
    return inner.query(tag);
  }

  void post(quoll::async::work_item &item) const {
    if (*fail) {
      throw std::runtime_error{"no more"};
    }

    inner.post(item);
  }

  [[nodiscard]] bool operator==(refusing_executor const &) const noexcept = default;
};

// Its view makes the executor refuse what comes next: its own effect cannot start.
struct refuser_props {
  bool *fail = nullptr;
  journal *log = nullptr;
};

constexpr component<refuser_props> refuser = [](refuser_props const &given) {
  use_journal(given.log, "refuser");
  *given.fail = true;
  return p("refuser");
};

struct refusing_page_data {
  bool *fail = nullptr;
  journal log;
};

constexpr component<> refusing_page = [] {
  auto &self = use_context<refusing_page_data>();

  return div({refuser({.fail = self->fail, .log = &self->log}), row({.label = "a", .log = &self->log})});
};

// Its handler holds what reads the component's props as the last copy of the handler goes.
struct touched_props {
  journal *log = nullptr;
};

constexpr component<touched_props> touched = [](touched_props const &given) {
  std::shared_ptr<void> const guard{nullptr, [&given](void * /*nothing*/) {
                                      if (given.log != nullptr) {
                                        given.log->lines.emplace_back("touched");
                                      }
                                    }};

  return button("touch") | on_click([guard] {});
};

struct maybe_touched_data {
  state<bool> shows{true};
  journal log;
};

constexpr component<> maybe_touched = [] {
  auto &self = use_context<maybe_touched_data>();

  return div({when(*self->shows, [&self] { return touched({.log = &self->log}); })});
};

// Its handler holds what reads its props as the handler goes: the props are still there then, as the view goes.
struct keeper_props {
  std::string label;
  journal *log = nullptr;
};

constexpr component<keeper_props> keeper = [](keeper_props const &given) {
  std::shared_ptr<void> const guard{nullptr, [&given](void * /*nothing*/) { given.log->lines.push_back(given.label); }};

  return p("keeper") | on_click([guard] {});
};

struct maybe_keeper_data {
  state<bool> shows{true};
  journal log;
};

constexpr component<> maybe_keeper = [] {
  auto &self = use_context<maybe_keeper_data>();

  return div({when(*self->shows, [&self] {
    return keeper({.label = "a label, too long for a short string", .log = &self->log});
  })});
};

// Reads its props in an effect on nothing, after a wait.
struct loader_props {
  std::string id;
  std::vector<std::string> *read = nullptr;
};

constexpr component<loader_props> loader = [](loader_props const &given) {
  use_effect([&given]() -> task<> {
    co_await quoll::async::sleep_for(1min);
    given.read->push_back(given.id);
  });

  return p(given.id);
};

struct loading_data {
  state<std::string> id{std::string{"an old id, too long for a short string"}};
  std::vector<std::string> read;
};

constexpr component<> loading = [] {
  auto &self = use_context<loading_data>();

  return div({loader({.id = *self->id, .read = &self->read})});
};

struct keeping_data {
  state<std::string> label{std::string{"the first label, too long for a short string"}};
  state<bool> shows{true};
  state<bool> extra{false};
  journal log;
};

constexpr component<> keeping = [] {
  auto &self = use_context<keeping_data>();

  return div({
      when(*self->shows, [&self] { return keeper({.label = *self->label, .log = &self->log}); }),
      when(*self->extra, [] { return p("extra"); }),
  });
};

// Throws for an empty label: the guard of its last view that showed still reads the label of that view.
constexpr component<keeper_props> picky_keeper = [](keeper_props const &given) {
  if (given.label.empty()) {
    throw std::invalid_argument{"no label"};
  }

  std::shared_ptr<void> const guard{nullptr, [&given](void * /*nothing*/) { given.log->lines.push_back(given.label); }};

  return p("picky") | on_click([guard] {});
};

struct picky_page_data {
  state<std::string> label{std::string{"a label, too long for a short string"}};
  state<bool> shows{true};
  state<bool> extra{false};
  journal log;
};

constexpr component<> picky_page = [] {
  auto &self = use_context<picky_page_data>();

  return div({
      when(*self->shows, [&self] { return picky_keeper({.label = *self->label, .log = &self->log}); }),
      when(*self->extra, [] { return p("extra"); }),
  });
};

// One key for two rows of one component, in two lists of a view: of the app's root, or of a component's.
struct two_lists_data {
  journal log;
};

constexpr component<> two_lists = [] {
  auto &self = use_context<two_lists_data>();

  return div({
      ul({row({.label = "a", .log = &self->log}) | key("1")}),
      ul({row({.label = "b", .log = &self->log}) | key("1")}),
  });
};

constexpr component<> two_lists_inside = [] { return div({two_lists()}); };

// A component with one of its own, each saying when its effect runs.
struct mounted_panel_props {
  journal *log = nullptr;
};

constexpr component<mounted_panel_props> mounted_panel = [](mounted_panel_props const &given) {
  use_journal(given.log, "panel");
  return row({.label = "inner", .log = given.log});
};

struct mounted_page_data {
  journal log;
};

constexpr component<> mounted_page = [] {
  auto &self = use_context<mounted_page_data>();

  use_journal(&self->log, "page");
  return div({mounted_panel({.log = &self->log})});
};

// Writes `said` as it is destroyed, unless moved from: tells when what holds it goes.
class parting {
public:
  parting(std::vector<std::string> *into, std::string what) : to_(into), said_(std::move(what)) {}

  parting(parting const &) = delete;

  parting(parting &&other) noexcept : to_(std::exchange(other.to_, nullptr)), said_(std::move(other.said_)) {}

  parting &operator=(parting const &) = delete;
  parting &operator=(parting &&) = delete;

  ~parting() {
    if (to_ != nullptr) {
      to_->push_back(said_);
    }
  }

private:
  std::vector<std::string> *to_;
  std::string said_;
};

// Given, in its props, what reads the props of its parent as it goes.
struct guarded_child_props {
  std::shared_ptr<void> guard;
};

constexpr component<guarded_child_props> guarded_child = [](guarded_child_props const & /*given*/) {
  return p("guarded");
};

struct guarding_props {
  std::string text;
  std::vector<std::string> *read = nullptr;
  parting gone;
};

constexpr component<guarding_props> guarding = [](guarding_props const &given) {
  auto const reads = [&given](void * /*nothing*/) { given.read->push_back(given.text); };

  return guarded_child({.guard = std::shared_ptr<void>{nullptr, reads}});
};

struct guarded_data {
  state<std::string> text{std::string{"the first text, too long for a short string"}};
  std::vector<std::string> read;
};

constexpr component<> guarded = [] {
  auto &self = use_context<guarded_data>();

  return div({guarding({.text = *self->text, .read = &self->read, .gone = parting{&self->read, "props gone"}})});
};

// A slow row given, in its props, what reads its parent as it goes.
struct watched_row_props {
  std::shared_ptr<void> guard;
};

constexpr component<watched_row_props> watched_row = [](watched_row_props const & /*given*/) {
  return button("slow") | on_click([]() -> task<> { co_await quoll::async::sleep_for(1h); });
};

struct watching_props {
  journal *log = nullptr;
  std::string name{"the watching panel, too long for a short string"};
};

constexpr component<watching_props> watching = [](watching_props const &given) {
  // Says when the panel goes: a state's value, destroyed with it.
  auto &gone = use_state<std::shared_ptr<telling>>();

  if (!*gone) {
    gone = std::make_shared<telling>(given.log, "panel gone");
  }

  auto const reads = [&given](void * /*nothing*/) { given.log->lines.push_back(given.name); };

  return watched_row({.guard = std::shared_ptr<void>{nullptr, reads}});
};

struct maybe_watching_data {
  state<bool> shows{true};
  journal log;
};

constexpr component<> maybe_watching = [] {
  auto &self = use_context<maybe_watching_data>();

  return div({when(*self->shows, [&self] { return watching({.log = &self->log}); })});
};

// A slow row with no props, under a panel that says when it goes: nothing of the panel waits for the row's task.
constexpr component<> bare_slow_row = [] {
  return button("slow") | on_click([]() -> task<> { co_await quoll::async::sleep_for(1h); });
};

struct bare_panel_props {
  journal *log = nullptr;
};

constexpr component<bare_panel_props> bare_panel = [](bare_panel_props const &given) {
  auto &gone = use_state<std::shared_ptr<telling>>();

  if (!*gone) {
    gone = std::make_shared<telling>(given.log, "panel gone");
  }

  return bare_slow_row();
};

struct maybe_bare_panel_data {
  state<bool> shows{true};
  journal log;
};

constexpr component<> maybe_bare_panel = [] {
  auto &self = use_context<maybe_bare_panel_data>();

  return div({when(*self->shows, [&self] { return bare_panel({.log = &self->log}); })});
};

} // namespace

TEST_CASE(
    "component: a component of the view gets its props, and keeps its states as its parent views again",
    "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, rows, {.contexts = {provide<rows_data>()}}};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find_text("a:0"), "click");
    co_await settle();

    // The parent views again: the row keeps its count.
    instance.dispatch(shown.find("p"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == "<div><p>views</p><ul><button>a:1</button><button>b:0</button></ul></div>");
}

TEST_CASE(
    "component: a keyed component of a list keeps its states with its item as the list moves", "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, rows, {.contexts = {provide<rows_data>()}}};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find_text("b:0"), "click");
    co_await settle();

    instance.context<rows_data>().labels = std::vector<std::string>{"b", "c", "a"};
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(
      shown.html() == "<div><p>views</p><ul><button>b:1</button><button>c:0</button><button>a:0</button></ul></div>"
  );
}

TEST_CASE(
    "component: an effect on nothing runs once a component first shows, and is stopped as it goes", "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, rows, {.contexts = {provide<rows_data>()}}};
  auto const &log = instance.context<rows_data>().log.lines;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<rows_data>().labels = std::vector<std::string>{"b"};
    co_await settle();
    // Viewed again with b still there: nothing comes or goes.
    instance.dispatch(shown.find("p"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  // b goes as the app stops.
  REQUIRE(log == std::vector<std::string>{"mounted a", "mounted b", "unmounted a", "unmounted b"});
}

TEST_CASE("component: one that goes has its tasks stopped, and stays until they have ended", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, maybe_slow, {.contexts = {provide<maybe_slow_data>()}}};
  auto const &log = instance.context<maybe_slow_data>().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    instance.context<maybe_slow_data>().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"handler ended"});
  REQUIRE(time.now() < 1h);
  REQUIRE(shown.html() == "<div></div>");
}

TEST_CASE("component: to_html writes the views of the components of a view", "[ui][components]") {
  REQUIRE(to_html(plain_rows()) == "<div><p>views</p><ul><button>a:0</button><button>b:0</button></ul></div>");
}

TEST_CASE("component: one decorated otherwise than with a key is refused", "[ui][components]") {
  journal log;
  auto const decorated = row({.label = "a", .log = &log}) | tw("p-2");

  REQUIRE_THROWS_AS(to_html(decorated), std::logic_error);

  // Checked or not, as a value given: either is a decoration.
  REQUIRE_THROWS_AS(to_html(row({.label = "a", .log = &log}) | checked(true)), std::logic_error);
  REQUIRE_THROWS_AS(to_html(row({.label = "a", .log = &log}) | checked(false)), std::logic_error);

  auto const retagged = row({.label = "a", .log = &log}) | decorator([](element &target) { target.tag = "section"; });

  REQUIRE_THROWS_AS(to_html(retagged), std::logic_error);
}

TEST_CASE(
    "component: components in components, of several types, keep their states, their effects and stops after their "
    "children's",
    "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, mixed, {.contexts = {provide<mixed_data>()}}};
  auto const &log = instance.context<mixed_data>().log.lines;

  auto const act = [&]() -> task<> {
    REQUIRE(
        shown.html() == "<div><p>views</p><button>x:0</button><p>count:0</p><button>y:0</button><p>1</p><p>2</p></div>"
    );

    instance.dispatch(shown.find_text("x:0"), "click");
    instance.dispatch(shown.find_text("count:0"), "click");
    instance.dispatch(shown.find_text("count:0"), "click");
    co_await settle();

    instance.dispatch(shown.find("p"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(
      shown.html() == "<div><p>views</p><button>x:1</button><p>count:2</p><button>y:0</button><p>1</p><p>2</p></div>"
  );
  REQUIRE(
      log ==
      std::vector<std::string>{
          "mounted x", "mounted panel", "mounted y", "unmounted x", "unmounted panel", "unmounted y"
      }
  );
}

TEST_CASE("component: an unkeyed component keeps its place by rank among those of its type", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, two_rows, {.contexts = {provide<two_rows_data>()}}};
  auto const &log = instance.context<two_rows_data>().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find_text("a:0"), "click");
    co_await settle();

    // b is first now: it takes the place, and the states, of the first row.
    instance.context<two_rows_data>().first = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == "<div><button>b:1</button></div>");
  // b's row is the first's: what it holds, its effect included, is a's, which goes as the app stops, and the second
  // goes.
  REQUIRE(log == std::vector<std::string>{"mounted a", "mounted b", "unmounted b", "unmounted a"});
}

TEST_CASE(
    "component: a view that throws changes nothing, and the next one starts from the last shown", "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  std::exception_ptr failure;
  test_app instance{
      time.get_executor(),
      shown,
      rows,
      {.on_error = [&](std::exception_ptr error) { failure = std::move(error); }, .contexts = {provide<rows_data>()}}
  };
  auto const &log = instance.context<rows_data>().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find_text("a:0"), "click");
    co_await settle();

    // c is made, then boom throws: c never shows, and b stays.
    instance.context<rows_data>().labels = std::vector<std::string>{"a", "c", "boom"};
    co_await settle();
    REQUIRE(shown.html() == "<div><p>views</p><ul><button>a:1</button><button>b:0</button></ul></div>");

    instance.context<rows_data>().labels = std::vector<std::string>{"a", "c"};
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::runtime_error);
  REQUIRE(shown.html() == "<div><p>views</p><ul><button>a:1</button><button>c:0</button></ul></div>");
  // b's run, stopped as b goes, unwinds once its stop reaches its frame, after c's first run started in that render.
  REQUIRE(
      log ==
      std::vector<std::string>{"mounted a", "mounted b", "mounted c", "unmounted b", "unmounted a", "unmounted c"}
  );
}

TEST_CASE("component: stop() from the task of a component fails rather than wait for itself", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  std::exception_ptr failure;
  test_app instance{
      time.get_executor(),
      shown,
      stopping_parent,
      {.on_error = [&](std::exception_ptr error) { failure = std::move(error); },
       .contexts = {provide<stopping_parent_data>()}}
  };
  std::function<task<>()> stop = [&] { return instance.stop(); };

  instance.context<stopping_parent_data>().stop = &stop;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::logic_error);
}

TEST_CASE(
    "component: one that goes stays, with its parent, until the tasks of its children have ended", "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, maybe_panel, {.contexts = {provide<maybe_panel_data>()}}};
  auto const &log = instance.context<maybe_panel_data>().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    instance.context<maybe_panel_data>().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"handler ended", "panel gone"});
  REQUIRE(shown.html() == "<div></div>");
}

TEST_CASE(
    "component: the effects of the app's root run after those of the components of its view, and stop after them",
    "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, page_with_row, {.contexts = {provide<page_with_row_data>()}}};
  auto const &log = instance.context<page_with_row_data>().log.lines;

  REQUIRE(quoll::test::run(time, session(instance, [] { return settle(); })) == 0);
  REQUIRE(log == std::vector<std::string>{"mounted a", "mounted page", "unmounted a", "unmounted page"});
}

TEST_CASE("component: a handler keeps the props of the view that made it until its task ends", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, reading, {.contexts = {provide<reading_data>()}}};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // The parent gives other props while the handler waits.
    instance.context<reading_data>().label = std::string{"new"};
    co_await settle();
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.context<reading_data>().read == std::vector<std::string>{"old"});
  REQUIRE(shown.html() == "<div><button>new</button></div>");
}

TEST_CASE("component: the nodes of a keyed component whose view is a fragment move with it", "[ui][components]") {
  virtual_time time;
  memory_renderer shown;
  test_app instance{time.get_executor(), shown, cell_pairs, {.contexts = {provide<cell_pairs_data>()}}};
  node_id before{};
  node_id after{};

  auto const act = [&]() -> task<> {
    before = shown.find_text("a2");
    instance.context<cell_pairs_data>().order = std::vector<std::string>{"b", "a"};
    co_await settle();
    after = shown.find_text("a2");
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(shown.html() == "<ul><li>b1</li><li>b2</li><li>a1</li><li>a2</li></ul>");
  REQUIRE(before == after);
}

TEST_CASE(
    "component: a render failing past the views changes no component, and the next one makes them again",
    "[ui][components]"
) {
  virtual_time time;
  bool fails = false;
  failing_renderer const shown{.fails = &fails};
  std::exception_ptr failure;
  app instance{
      time.get_executor(),
      shown,
      rows,
      {.on_error = [&](std::exception_ptr error) { failure = std::move(error); }, .contexts = {provide<rows_data>()}}
  };
  auto const &log = instance.context<rows_data>().log.lines;

  auto const act = [&]() -> task<> {
    co_await settle();
    fails = true;
    instance.context<rows_data>().labels = std::vector<std::string>{"a", "c"};
    co_await settle();

    instance.context<rows_data>().labels = std::vector<std::string>{"a", "c"};
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::runtime_error);
  REQUIRE(shown.shown.html() == "<div><p>views</p><ul><button>a:0</button><button>c:0</button></ul></div>");
  // b's run, stopped as b goes, unwinds once its stop reaches its frame, after c's first run started in that render.
  REQUIRE(
      log ==
      std::vector<std::string>{"mounted a", "mounted b", "mounted c", "unmounted b", "unmounted a", "unmounted c"}
  );
}

TEST_CASE(
    "component: the keys of the views of components never meet those of their siblings, nor positions",
    "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, colliding, {.contexts = {provide<colliding_data>()}}};

  auto const act = [&]() -> task<> {
    instance.context<colliding_data>().views = 1;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(
      shown.html() == "<ul><p>header</p><li>first</li><p>count:0</p><p>plain</p><li>tagged</li><li>tagged</"
                      "li><li>item</li><li>item</li></ul>"
  );
}

TEST_CASE(
    "component: a pointer into the props of its parent stays good for a handler of a component", "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, quoted, {.contexts = {provide<quoted_data>()}}};

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    instance.context<quoted_data>().label = std::string{"a new label"};
    co_await settle();
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(
      instance.context<quoted_data>().read == std::vector<std::string>{"an old label, too long for a short string"}
  );
}

TEST_CASE("component: an app destroyed without its stop leaves no component behind", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  journal log;

  {
    test_app instance{time.get_executor(), shown, witnessing, {.contexts = {provide<witnessing_data>()}}};

    instance.context<witnessing_data>().log = &log;

    auto const start = [&]() -> task<int> {
      instance.start();
      co_await settle();
      co_return 0;
    };

    REQUIRE(quoll::test::run(time, start()) == 0);
    REQUIRE(log.lines.empty());
  }

  REQUIRE(log.lines == std::vector<std::string>{"gone"});
}

TEST_CASE("component: an effect that cannot start is told to on_error, and the others start", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  bool fail = false;
  std::exception_ptr failure;
  app instance{
      refusing_executor{time.get_executor(), &fail},
      shown,
      refusing_page,
      {.on_error =
           [&](std::exception_ptr error) {
             fail = false;
             failure = std::move(error);
           },
       .contexts = {provide<refusing_page_data>()}}
  };
  auto const &log = instance.context<refusing_page_data>().log.lines;

  instance.context<refusing_page_data>().fail = &fail;

  REQUIRE(quoll::test::run(time, session(instance, [] { return settle(); })) == 0);
  REQUIRE_THROWS_AS(std::rethrow_exception(failure), std::runtime_error);
  REQUIRE(log == std::vector<std::string>{"mounted a", "unmounted a"});
}

TEST_CASE(
    "component: what its handlers hold goes before the component and its props, as its view goes", "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, maybe_touched, {.contexts = {provide<maybe_touched_data>()}}};
  auto const &log = instance.context<maybe_touched_data>().log.lines;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<maybe_touched_data>().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"touched"});
}

TEST_CASE("component: its props are still there as it goes", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, maybe_keeper, {.contexts = {provide<maybe_keeper_data>()}}};
  auto const &log = instance.context<maybe_keeper_data>().log.lines;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<maybe_keeper_data>().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"a label, too long for a short string"});
}

TEST_CASE("component: an effect on nothing keeps the props of the first view until it ends", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, loading, {.contexts = {provide<loading_data>()}}};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<loading_data>().id = std::string{"a new id"};
    co_await settle();
    co_await quoll::async::sleep_for(2min);
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(instance.context<loading_data>().read == std::vector<std::string>{"an old id, too long for a short string"});
  REQUIRE(shown.html() == "<div><p>a new id</p></div>");
}

TEST_CASE(
    "component: a render failing past the views leaves a kept component the props its view was given",
    "[ui][components]"
) {
  virtual_time time;
  bool fails = false;
  failing_renderer const shown{.fails = &fails};
  std::exception_ptr failure;
  app instance{
      time.get_executor(),
      shown,
      keeping,
      {.on_error = [&](std::exception_ptr error) { failure = std::move(error); }, .contexts = {provide<keeping_data>()}}
  };
  auto const &log = instance.context<keeping_data>().log.lines;

  auto const act = [&]() -> task<> {
    co_await settle();

    // Viewed with the second label, then the insert of the extra paragraph fails.
    fails = true;
    instance.context<keeping_data>().label = std::string{"the second label, too long for a short string"};
    instance.context<keeping_data>().extra = true;
    co_await settle();
    REQUIRE(failure);
    REQUIRE(shown.shown.html() == "<div><p>keeper</p></div>");

    instance.context<keeping_data>().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  // The view that did not show goes first, then the one shown.
  REQUIRE(
      log ==
      std::vector<std::string>{
          "the second label, too long for a short string", "the first label, too long for a short string"
      }
  );
}

TEST_CASE("component: one whose view throws keeps the props of its last view", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  std::exception_ptr failure;
  test_app instance{
      time.get_executor(),
      shown,
      picky_page,
      {.on_error = [&](std::exception_ptr error) { failure = std::move(error); },
       .contexts = {provide<picky_page_data>()}}
  };
  auto const &log = instance.context<picky_page_data>().log.lines;

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<picky_page_data>().label = std::string{};
    co_await settle();
    REQUIRE(failure);

    instance.context<picky_page_data>().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"a label, too long for a short string"});
}

TEST_CASE(
    "component: one whose view throws after a render that failed keeps the props of both views", "[ui][components]"
) {
  virtual_time time;
  bool fails = false;
  failing_renderer const shown{.fails = &fails};
  int failures = 0;
  app instance{
      time.get_executor(),
      shown,
      picky_page,
      {.on_error = [&](std::exception_ptr) { ++failures; }, .contexts = {provide<picky_page_data>()}}
  };
  auto const &log = instance.context<picky_page_data>().log.lines;

  auto const act = [&]() -> task<> {
    co_await settle();

    // Viewed with the second label, then the insert of the extra paragraph fails.
    fails = true;
    instance.context<picky_page_data>().label = std::string{"another label, too long for a short string"};
    instance.context<picky_page_data>().extra = true;
    co_await settle();

    // Its view throws: its guard still reads the second label.
    instance.context<picky_page_data>().label = std::string{};
    co_await settle();

    instance.context<picky_page_data>().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(failures == 2);
  // The view that did not show goes first, then the one shown.
  REQUIRE(
      log ==
      std::vector<std::string>{"another label, too long for a short string", "a label, too long for a short string"}
  );
}

TEST_CASE("component: two of one type with one key in a view are refused, even in two lists", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, two_lists, {.contexts = {provide<two_lists_data>()}}};

  auto const start_then_stop = [&]() -> task<int> {
    REQUIRE_THROWS_AS(instance.start(), std::logic_error);
    co_await instance.stop();
    co_return 0;
  };

  REQUIRE(quoll::test::run(time, start_then_stop()) == 0);
  REQUIRE(instance.context<two_lists_data>().log.lines.empty());

  test_app inside{time.get_executor(), shown, two_lists_inside};

  auto const inside_then_stop = [&]() -> task<int> {
    REQUIRE_THROWS_AS(inside.start(), std::logic_error);
    co_await inside.stop();
    co_return 0;
  };

  REQUIRE(quoll::test::run(time, inside_then_stop()) == 0);
}

TEST_CASE("component: an effect does not start once its component went before it could", "[ui][components]") {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, rows, {.contexts = {provide<rows_data>()}}};
  auto const &log = instance.context<rows_data>().log.lines;

  auto const start_then_stop = [&]() -> task<int> {
    instance.start();
    co_await instance.stop();
    co_return 0;
  };

  REQUIRE(quoll::test::run(time, start_then_stop()) == 0);
  REQUIRE(log.empty());
}

TEST_CASE(
    "component: the effects of a view's components run before their parent's, the root's last", "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, mounted_page, {.contexts = {provide<mounted_page_data>()}}};
  auto const &log = instance.context<mounted_page_data>().log.lines;

  REQUIRE(quoll::test::run(time, session(instance, [] { return settle(); })) == 0);
  REQUIRE(
      log ==
      std::vector<std::string>{
          "mounted inner", "mounted panel", "mounted page", "unmounted inner", "unmounted panel", "unmounted page"
      }
  );
}

TEST_CASE(
    "component: the effects of the root do not start once the app stopped before they could", "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, mounted_page, {.contexts = {provide<mounted_page_data>()}}};
  auto const &log = instance.context<mounted_page_data>().log.lines;

  auto const start_then_stop = [&]() -> task<int> {
    instance.start();
    co_await instance.stop();
    co_return 0;
  };

  REQUIRE(quoll::test::run(time, start_then_stop()) == 0);
  REQUIRE(log.empty());
}

TEST_CASE(
    "component: its props go before those of the view that placed it, which they may refer to", "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, guarded, {.contexts = {provide<guarded_data>()}}};

  auto const act = [&]() -> task<> {
    co_await settle();
    instance.context<guarded_data>().text = std::string{"the second text, too long for a short string"};
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(
      instance.context<guarded_data>().read ==
      std::vector<std::string>{
          "the first text, too long for a short string", "props gone", "the second text, too long for a short string",
          "props gone"
      }
  );
}

TEST_CASE(
    "component: the props of a component keep the one that placed it, which they may refer to", "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, maybe_watching, {.contexts = {provide<maybe_watching_data>()}}};
  auto const &log = instance.context<maybe_watching_data>().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // The panel goes: the row's task, stopped, holds the last of the row's props, which hold the panel.
    instance.context<maybe_watching_data>().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  // Its first view sets the state that says when it goes, which views it again: the guard its first view gave goes
  // then.
  REQUIRE(
      log ==
      std::vector<std::string>{
          "the watching panel, too long for a short string", "the watching panel, too long for a short string",
          "panel gone"
      }
  );
}

TEST_CASE(
    "component: one without props keeps nothing of its parent, which goes before its tasks end", "[ui][components]"
) {
  virtual_time time;
  memory_renderer const shown;
  test_app instance{time.get_executor(), shown, maybe_bare_panel, {.contexts = {provide<maybe_bare_panel_data>()}}};
  auto const &log = instance.context<maybe_bare_panel_data>().log.lines;

  auto const act = [&]() -> task<> {
    instance.dispatch(shown.find("button"), "click");
    co_await settle();

    // The panel goes as its view no longer shows it, while the row's task, stopped, has yet to end.
    instance.context<maybe_bare_panel_data>().shows = false;
    co_await settle();
  };

  REQUIRE(quoll::test::run(time, session(instance, act)) == 0);
  REQUIRE(log == std::vector<std::string>{"panel gone"});
  REQUIRE(time.now() < 1h);
  REQUIRE(shown.html() == "<div></div>");
}
