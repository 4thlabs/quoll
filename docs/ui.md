# UI

An interface written in C++ and compiled to WebAssembly, in the same language as the rest of an application. The
DOM gives text, input methods, selection and accessibility, and Tailwind the styles. Link `quoll::emscripten`, which
brings `quoll::ui`. [`samples/notes/web`](../samples/notes/web) is a whole page, with its build, and
[`samples/dashboard`](../samples/dashboard) a screen with a chart.js widget; what is still to come is
[at the end](#what-it-lacks).

## A component

A component is an aggregate with a `view()`, and `state<T>` members for what the view depends on.

```cpp
#include <quoll/emscripten.hpp>
#include <quoll/ui.hpp>

#include <string>

namespace async = quoll::async;

using namespace quoll::ui;

struct counter {
  state<int> count;

  element view() {
    auto const add = [this] { count = *count + 1; };

    auto const shown = div({
        text("Count: " + std::to_string(*count)) | tw("text-lg"),
        button("Add") | tw("btn btn-primary") | on_click(add),
    });

    return shown | tw("flex items-center gap-2");
  }
};

int main() { quoll::emscripten::mount<counter>("#app"); }
```

A state changing runs the view again, once however many changed in between, and only what differs
reaches the DOM. Handlers capture `this`, as the component lives at a stable address, and may be plain
functions or return a task:

```cpp
struct notes_page {
  state<std::vector<std::string>> items;
  state<std::string> draft;

  // Optional: runs once it first shows.
  async::task<> mounted() { items = co_await load_notes(); }

  element view() {
    auto const add = [this] {
      items.update([this](auto &all) { all.push_back(*draft); });
    };

    auto const nothing = [] { return p("Nothing yet"); };

    auto const row = [](std::string const &item) {
      return li(item) | key(item);
    };

    return main_({
        input() | bind(draft) | attribute("placeholder", "A new note"),
        button("Add") | on_click(add),
        when(items->empty(), nothing),
        ul({list(items, row)}),
    });
  }
};
```

| To                     | Write                                                                                                                |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------- |
| Show an element        | `div({...})`, `button("Add")`, `node("video", {...})`; `main_` for `<main>`                                          |
| Show a text            | `text("Hello")`, or `p("Hello")` for a tag holding it                                                             |
| Style it               | `\| tw("flex gap-2")`, `\| tw_if(done, "line-through")`, classes written whole, as Tailwind reads them in the source |
| Set an attribute       | `\| attribute("placeholder", "A note")`                                                                              |
| Handle an event        | `\| on_click(...)`, `\| on_input(...)`, `\| on("submit", ...)`                                                       |
| Edit a value           | `input() \| bind(draft)`, for a `state<std::string>`                                                                 |
| Repeat, or show or not | `list(items, ...)` with a `key()` on each, `when(condition, ...)`                                                    |
| Read a state           | `*count`, `items->size()`                                                                                            |
| Change a state         | `count = 1;`, `count.set(1);`, `items.update(...)`                                                                   |
| Call an API            | A `rest::client`, over [`fetch`](runtimes.md#httpclient-in-a-browser)                                                     |
| Make a decorator       | `decorator([](element &target) { target.classes += " card"; })`                                                    |

## Components of a view

A view may hold components of its own, each with states of its own. A component of a view has a `struct props`,
which its parent gives it, and a `view(props const &)`; the parent places it with `component<C>(props)`:

```cpp
struct note_row {
  struct props {
    std::string text;
  };

  state<bool> done;

  element view(props const &given) {
    auto const toggle = [this] { done = !*done; };

    return li(given.text) | tw_if(*done, "line-through") | on_click(toggle);
  }
};

struct notes_page {
  state<std::vector<std::string>> items;

  element view() {
    auto const row = [](std::string const &item) {
      return component<note_row>({.text = item}) | key(item);
    };

    return ul({list(items, row)});
  }
};
```

**Its states are kept** from one view of the parent to the next, while it stays at the same place:

- with a key, its type and key among the components of the parent's view: `| key(item)` keeps each row with its item
  as the list changes;
- without one, its type and rank there, so that one going hands its states to the next of its type;
- two of one type with one key in a view, even in two lists of it, throw `std::logic_error`.

**Its props come from the parent's view**, which shows in its own view what changes there:

- `given` stays valid for what that view made: a handler of the view may keep a reference to it until its task ends;
- the component, and the components it places, may keep one until its next view that returns;
- `mounted()` may keep one to the props of the first view, until it ends;
- the props it gives a component of its own view may refer to it, its `this` or its `given`: they stay as long as
  those props. They may not refer into the value of one of its states, which a change replaces: pass the value.

**It is decorated in its own view:** a component element takes a key and nothing else, and one decorated otherwise
throws `std::logic_error`.

**It comes and goes:**

- `task<> mounted()` runs once it first shows, after those of the components of its view;
- once the parent's view no longer has it, its tasks are asked to stop, `void unmounted()` runs, and it goes once they
  have ended, as [what a stop does](async.md#stopping-a-task) says;
- a handler of its view runs as one of its tasks, so the component outlives it.

A component without props, a `view()` alone, is placed with `component<C>()`. The app's own component may have a
`void unmounted()` too: it runs as the app stops, after those of the components of its view.

### A view not run again

A component whose props compare equal to the last ones, and none of whose states changed since, is not viewed again:
its last view is given again, with the props it was made with. The components inside it are viewed again only if they
changed.

```cpp
struct note_row {
  struct props {
    notes::note item;
    callback<void(std::int64_t)> on_remove;

    [[nodiscard]] bool operator==(props const &) const = default;
  };

  // ...
};
```

- **Props are compared with their `==`**, when they have one. Without one, the component is viewed again each time its
  parent is, unless the parent's view was itself given again.
- **Only its states and its props by value view it again.** A prop that points to data the parent changes, or a plain
  member a component changes, does not: make it a state, or pass the value itself.
- **A callback compares equal to any other**, whatever its function captures. A component not viewed again keeps the
  callbacks of its last view, which call with what the parent captured then: capture only what does not change from a
  view to the next, as `this`, and pass the rest as props.

### Calling the parent back

A component tells its parent what happened through a `callback<Signature>` in its props: a function the parent wrote,
which runs as one of the parent's tasks, not the child's.

```cpp
struct note_row {
  struct props {
    notes::note item;
    callback<void(std::int64_t)> on_remove;
  };

  element view(props const &given) {
    auto const remove = [&given] {
      return given.on_remove(given.item.id);
    };

    return li({
        span(given.item.text),
        button("Delete") | on_click(remove),
    });
  }
};

struct notes_page {
  state<std::vector<notes::note>> notes;

  task<> remove(std::int64_t id);

  element view() {
    auto const on_remove = [this](std::int64_t id) {
      return remove(id);
    };

    auto const row = [&](notes::note const &item) {
      return component<note_row>({.item = item, .on_remove = on_remove}) | key(item.id);
    };

    return ul({list(notes, row)});
  }
};
```

**It belongs to the component whose view made it**, or to the app for the app's own view:

- a call starts the function as a task of that component, under its stop;
- the caller waits for its end, and gets what it gives (`callback<int(int)>`) or what it throws;
- a caller that goes stops waiting, and the work goes on: a row removed while the page deletes its note leaves the
  delete to finish, and what the work throws then goes to `on_error`;
- a work stopped, as its own component goes, stops its caller too.

**A call runs nothing**, ending on a stop, once the app has stopped or is gone, or once the component is gone: a
callback holds its component weakly, so that one kept by its own component leaves no cycle. Kept once the view that
made it is replaced, a call throws `std::logic_error`, as what its function refers to of that view may be gone.

**Its arguments are copied into the task.** The function returns its value, or a task of it. A view (`std::string_view`,
`std::span`, a range view), a reference to write to, or a result that would refer to what the work held does not
compile. A pointer, or a view inside another type, still points to what the caller gave.

**Made outside a view**, in a handler or `mounted()`, a callback knows no component: it runs in the caller's task, as a
`std::function` would, and what its function captures is the caller's to keep alive.

## Effects and actions

What a component runs as a state changes, and what it runs on a click, once at a time, are members of it, which show
again as they change, as states do. `effect<T>` is React's `useEffect` with a state of the component as its
dependency; `action<R(Args...)>`, or `keyed_action<Key, R(Args...)>` for runs by key, is what a handler runs. Each
gives its result as a `std::expected` of the value or of what the function threw, as an `std::exception_ptr`, and
keeps a copy of it for the view: a value whose copy throws, as for a lack of memory, is the failure of that run.

```cpp
struct dashboard_page {
  state<std::optional<std::int64_t>> selected;

  effect<products::monthly_sales> sales{selected, [](std::int64_t id) {
    return products_client::sales(id);
  }};

  keyed_action<std::int64_t, products::product(products::product_update)> saves{products_client::update};

  void select(std::int64_t id) {
    selected = std::optional<std::int64_t>{id};
  }

  element view() {
    auto const loading = [] {
      return p("Loading its sales");
    };

    auto const done = [this] {
      auto const &result = sales.result();

      return result ? component<sales_chart>({.sales = *result}) : p("No sales for now");
    };

    return div({
        when(sales.status() == run_status::running, loading),
        when(sales.status() == run_status::done, done),
    });
  }
};
```

**An effect runs as its state changes**, as React's `useEffect(…, [state])`:

- after a view of its component in which the state changed, compared with `==`, once the view is shown, the function
  runs as a task of the component with a copy of the state's value;
- the run before is stopped: its answer writes nothing, even one that came anyway, as React's cleanup would have it;
- a state holding a `std::optional` runs it with what it holds, and stops it, with nothing run, once it is empty;
- `status()` says where it is: `running` from its first view, as the view a change makes already says, with no
  result of the value before, `done` with its result, as React Query's `success` or `error`, or `none` while an
  optional state holds none; `result()` is the
  result of the last run once `status()` is `done`, and throws `std::logic_error` before;
- `reload()` runs it again for the value the state holds now, after the view it asks for, as a change of the state
  would: what a Try again button calls, as setting a state to the value it holds changes nothing. It returns nothing:
  an effect runs one way, after a view, as React has it; a run that cannot start, as for a lack of memory, ends on
  that failure, which `reload()` tries again;
- `effect<>` runs a function of nothing, as a search a while after what is typed:

```cpp
effect<> search{query, [this](std::string text) -> task<> {
  co_await quoll::async::sleep_for(300ms);
  co_await find(std::move(text));
}};
```

**An action runs once at a time**:

- `co_await save.run(args...)` gives its result: `run_refused`, at once, for a call while one is under way;
- with `overlap::restart` as its second parameter, a call stops the run under way instead, whose result is then
  `run_replaced`;
- `status()` says where it is, as an effect's: `none`, `running`, for a button to wait on, or `done`; a run its
  caller stops is under way no more;
- `result()` is the result of the last run, once `status()` is `done`; it goes as the next starts, as with React
  Query's mutations, and a call refused or replaced writes none;
- a `keyed_action` keeps its runs by key, which its function takes first: one under way for a product refuses another
  for that product, and leaves the others alone; `status(key)` and `result(key)`.

```cpp
task<bool> save(std::int64_t id, products::product_update changes) {
  auto const saved = co_await saves.run(id, std::move(changes));

  if (saved) {
    show(*saved);
  }

  co_return saved.has_value();
}
```

None is copied nor moved: a run under way refers to it, as a component stays where it was made. An effect depends on
a state of the same component, declared before it. Keys, arguments and values are copied into the run, so that none
can be a reference or a view, which does not compile. A function given empty, as a null pointer, throws
`std::invalid_argument` as the member is made.

## JavaScript widgets

A JavaScript library, as chart.js, shows in a node of the view that the app leaves to it: a widget. It is declared
once, with the module that mounts it, as an import would name it from the C++ file declaring it, the tag of its node
and its props:

```cpp
struct chart_props {
  std::vector<std::string> labels;
  std::vector<double> values;
};

struct point_selected {
  int index;
};

inline constexpr auto line_chart = widget<chart_props>("./src/widgets/line_chart.js", "canvas");

struct sales_page {
  state<std::vector<double>> sales;
  state<int> selected;

  element view() {
    auto const select = [this](point_selected event) {
      selected = event.index;
    };

    auto const chart = line_chart({.labels = months(), .values = *sales});

    return chart | tw("h-64") | on("point_selected", select);
  }
};
```

The module, `src/widgets/line_chart.js` here, exports `mount(node, props, emit)`, which returns `update(props)` and
`unmount()`:

```js
import { Chart } from "chart.js/auto";

export function mount(node, props, emit) {
  const select = (_, points) => {
    if (points.length > 0) {
      emit("point_selected", { index: points[0].index });
    }
  };

  const chart = new Chart(node, {
    type: "line",
    data: { labels: props.labels, datasets: [{ data: props.values }] },
    options: { onClick: select },
  });

  return {
    update(props) {
      chart.data.labels = props.labels;
      chart.data.datasets[0].data = props.values;
      chart.update();
    },
    unmount() {
      chart.destroy();
    },
  };
}
```

- **Its props go as JSON**, written by Glaze: `mount` once the render has placed its node, `update` when the JSON
  changes, `unmount` before the node goes, and as the app stops. Another module at its place is another node.
- **What it emits is an event of its node:** `on(name, function)` reads its JSON into the function's parameter,
  leaving the fields it does not declare. A value that is not that JSON goes to `on_error`.
- **Its node is the module's:** the element takes classes, attributes and events, no children. `to_html` writes it
  empty, for the module to mount once the page is live.
- **Its module loads on demand.** The Vite plugin, in `tools/vite-plugin`, finds the widgets of the C++ sources it is given, and
  makes their registry, `virtual:quoll-widgets`, which the page sets before the program starts:

  ```js
  import widgets from "virtual:quoll-widgets";

  globalThis.quollWidgets = widgets;
  ```

  A path starting with `./` or `../` is resolved from the C++ file declaring it, inside the Vite root or not; any other,
  a package's name, is imported as it is.

  Each module is a chunk of the build, loaded as its first widget shows. `mount` then runs with the props the widget
  holds by then; one unmounted while its module loads is never mounted.
- **Failures go to `on_error`**, with the details in the console: a widget whose module the registry does not have,
  whose `update` or `unmount` throws, or whose module fails to load or to mount, which happens once the render has
  gone on. A `mount` may return a promise, which is awaited. What it emits that JSON cannot hold reaches nothing. A
  widget placed through a renderer that shows none, a `WidgetRenderer`, throws `std::logic_error`.

## HTML

`to_html` writes the same interface as HTML, natively as under WebAssembly, for a page generated ahead
of time or sent by a server. Handlers do nothing there: this is the page as it first shows. Text and
attribute values are escaped, but an attribute's meaning is the application's to check, such as an
`href` with a `javascript:` URL. A whole page starts with `<!doctype html>` and `<meta charset="utf-8">`.

```cpp
#include <quoll/ui.hpp>

#include <iostream>

int main() {
  using namespace quoll::ui;

  std::cout << to_html(main_({h1("Notes") | tw("text-3xl"), p("Hello")})) << '\n';
}
```

## Renderers

`ui::app` only knows a `Renderer`: an interface of node operations, which `dom_renderer` applies
to the page. `quoll::emscripten::mount` puts the two together. It takes a CSS selector, `#app` by
default, and keeps the app for as long as the page: one app per component type.

## Hot reload

For now this is a plugin of Vite, in `tools/vite-plugin`, in three levels: CSS through Tailwind, a literal
of `tw()`, `text()` or a tag sent to the running program with no compilation, keeping its state, and
anything else by a rebuild that reloads the page, losing its state. To try it, see
[the sample](../samples/notes/web/README.md#try-hot-reload).

A rebuild compiles with line tables only, so that a trace still names files and lines: the plugin's
`debug` option, `'lines'` by default, takes `'full'` for the variables in the browser's debugger and `'none'` for the
shortest rebuilds:

```js
quoll({
  target: "my_page",
  sources: ["main.cpp", "components/page.hpp"],
  debug: "full",
});
```

## What it lacks

From [step 6](PLAN.md#step-6-the-ui-dsl-complete) of the roadmap:

- A handle on a widget, for what is not a prop, as writing to a terminal.
- Base components.
- Making a page written by `to_html` live (hydration): a `value` on a `select` selects nothing yet.
- Restoring the states after a rebuild.

Tools that take Vite and Tailwind's setup out of an application, and a PWA, are
[step 7](PLAN.md#step-7-the-quoll-command-driving-vite-maintainer-merges-the-first-pull-request), and
desktop and mobile shells are [step 10](PLAN.md#step-10-desktop-and-mobile-maintainer-merges).
