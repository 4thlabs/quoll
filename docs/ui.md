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
compile, as does one inside an optional, a pair, a tuple, a variant or an expected. A pointer, or a view in a
container, as a `std::vector<std::string_view>`, or inside a type of the program's, as a member of a struct, still
points to what the caller gave.

**Made outside a view**, in a handler or `mounted()`, a callback knows no component: it runs in the caller's task, as a
`std::function` would, and what its function captures is the caller's to keep alive.

## Effects and actions

What a component loads as its states change, and what it runs on a click, are members of it. Both write what they
give into a state of the component, their target, and the view reads only states. An `effect` is React's `useEffect`:
it runs after a view in which one of its states changed. An `action<&function>` is what a handler runs, once at a
time by default.

```cpp
struct sales_panel {
  state<std::int64_t> id;

  // The sales, whether they are loading, and why they did not load.
  async_state<products::monthly_sales> sales;
  effect load{id, products_client::sales, sales};

  element view() {
    if (sales->loading) {
      return p("Loading its sales");
    }

    if (sales->error) {
      return p("No sales for now");
    }

    return component<sales_chart>({.sales = sales->value});
  }
};
```

**An `async_state<T>`**, in `<quoll/ui/async_state.hpp>`, is the target most components want: a state of an
`async_value<T>`, whose fields are `value`, `loading` and `error`, read as `sales->loading`. `async_state<>` has no
`value`, for a run that gives nothing, as a save whose answer only the handler wants. It is a state like any other,
set and updated as one: `items.update([](auto &held) { held.value.push_back(...); })`.

**What a target receives goes by the type of its elements.** A target is an `async_state`, a `state<T>`, or a
`state<std::tuple<...>>` of several elements:

- a `bool` is whether a run is under way;
- a `std::exception_ptr` is why the last run failed;
- every other element is one of the values the function gives, in order; the `value` of an `async_state`.

An effect's target is loading from the start, as React's `useState(true)`: the first view of its component comes
before its first run. As a run starts, its target is loading and has no error, the values before kept, as a state keeps its value until it
is set. As it ends, the target holds all it gave at once, in one render, or why it failed, the values before kept. A
target holds one `bool` and one `std::exception_ptr` at most, and a function giving a `bool` or an error as a value
does not compile: the target could not tell it from its flags. Each value is of the type the function gives, with no
conversion, which could make a view of what the run held, and moves without throwing, so that a run writes all it
gave or nothing. A target needs neither flag:

```cpp
state<products::monthly_sales> sales;                          // the value alone
state<std::tuple<products::monthly_sales, bool>> sales;        // and whether it is loading
```

**An effect runs as its states change**, as React's `useEffect(…, [states])`:

- it depends on one state, on several as `std::tie(filter, page)`, or on none as `std::tie()`, which runs it once, as
  its component first shows, or on a field of its component's props, as `effect load{&props::id, get, product};`,
  which React's `useEffect(…, [props.id])` is: a page under a route loads what its parameter names so;
- after a view of its component in which one changed, compared with `==`, once the view is shown, the function runs as
  a task of the component with a copy of each value;
- the run before is stopped, as React's cleanup: what it gives after the view of the change writes nothing, even an
  answer that came anyway;
- the view a later change makes comes before the run, which starts once that view is shown, as in React: it still
  shows the values before, not yet loading. A value that must match the state, as a product's sales, can be given with what
  it is of, and the view compare them;
- a state holding a `std::optional` gives what it holds; while one holds none, nothing runs, the run before stopped
  and the target loading no more;
- `reload()` runs it again after the view it asks for, as a change would: what a Try again button calls. An effect runs
  one way, after a view, as React has it. One that cannot start, comparing or copying its states, or starting its run,
  as for a lack of memory, stops the run before all the same and ends on that failure, which `reload()` tries again;
- a target with no `std::exception_ptr` leaves a failure to the app's `on_error`, as a handler's.

Several loads go in one function, which gives a `std::tuple`, and fail together:

```cpp
state<std::tuple<products::product, products::monthly_sales, bool, std::exception_ptr>> detail;

effect load_detail{id, [](std::int64_t id) -> task<std::tuple<products::product, products::monthly_sales>> {
  auto [item, sales] = co_await when_all(products_client::get(id), products_client::sales(id));

  co_return {std::move(item), std::move(sales)};
}, detail};
```

An effect with no target runs a function that gives nothing, as a search a while after what is typed:

```cpp
effect search{query, [this](std::string text) -> task<> {
  co_await quoll::async::sleep_for(300ms);
  co_await find(std::move(text));
}};
```

**An action runs a function once at a time**, which its template argument names, and gives its signature:

- a free function returning a task, as `action<&products_client::update> save{saving};`;
- a member function of a context, given its `shared<C>`, as `action<&catalog::save> save{products, saving};`, which
  runs as the context's `run()` does: its work goes on as the component goes;
- a member function of the component itself, given `this`, as `action<&product_detail::save> save{this, saving};`,
  declared before the action.

What it gives goes to its target, and to what follows a success, given last, as React Query's `onSuccess`:

```cpp
struct rename_form {
  struct props {
    products::product item;
  };

  state<std::optional<std::string>> name;

  // Whether a save is on its way, and why the last one failed; saved, what is typed is forgotten.
  async_state<products::product> saving;
  action<&products_client::update> save{saving, [this](products::product const &) { name = std::nullopt; }};

  element view(props const &given) {
    products::product_update const changes{.name = name->value_or(given.item.name)};

    if (saving->loading) {
      return button("Saving…") | attribute("disabled", "");
    }

    return button("Save") | on_click(save.with(given.item.id, changes));
  }
};
```

- `save.with(args...)` gives a handler that runs it with copies of the arguments, for `on_click`: what it gave goes to
  its target and to what follows a success, and a failure its target has no place for, with no `std::exception_ptr`
  or no target, to the app's `on_error`, as a handler's;
- `co_await save.run(args...)` gives what the function gave, or what it threw, as a `std::expected`, for a handler
  that goes on with it, after what follows a success, which throws to it what it throws;
- both take the function's parameters, so that a braced argument builds one, as `save.with(id, {.name = "tea"})`;
- a call while one is under way is refused, `run_refused` at once, its target left as it is;
- with `overlap::restart` as its second parameter, a call stops the run under way instead, which gives `run_replaced`
  and writes nothing;
- a run its caller stops, as a timeout does, is under way no more;
- what follows a success is told what the run gave, `void()` for a function that gives nothing; a failure, a refused
  call or a stopped one tell it nothing;
- its runs are its component's tasks: a component that goes, as a detail keyed by its product once another is chosen,
  stops them, and what follows them. A save that must go on, as one the list of the page shows, is a context's: the
  action runs a member function of it, whose work goes on as the detail stops waiting.

None is copied nor moved: a run under way refers to it, as a component stays where it was made. Its target and what
it is given are members of the same component, declared before it. Arguments and values are copied into the run, the
arguments given to the function as rvalues, so that none can be a view, which does not compile.

## Contexts

Data several components show and change, as the products a list and a detail share, is a context: an aggregate with
no view, its states, its effects and member functions. The app makes it from its options and holds it as long as it
lives, and a component finds it through a `shared<T>` member, by type.

```cpp
struct catalog {
  // The products, whether they are loading, and why they did not load.
  async_state<std::vector<products::product>> items;
  effect load{std::tie(), products_client::list, items};

  task<products::product> save(std::int64_t id, products::product_update changes) {
    auto saved = co_await products_client::update(id, std::move(changes));

    items.update([&saved](auto &held) {
      std::ranges::replace_if(held.value, [&saved](auto const &item) { return item.id == saved.id; }, saved);
    });
    co_return saved;
  }
};

struct product_list {
  shared<catalog> products;

  element view() {
    if (products->items->loading) {
      return p("Loading the products");
    }

    // ...
  }
};

struct product_detail {
  shared<catalog> products;

  // The save is the catalog's: it goes on as the detail goes.
  async_state<products::product> saving;
  action<&catalog::save> save{products, saving};
};

int main() {
  quoll::emscripten::mount<dashboard_page>("#app", {.contexts = {provide<catalog>()}});
}
```

- `provide<T>()`, in `<quoll/ui/provide.hpp>`, gives the app a context, one of each type, which has no view, `mounted()` nor `unmounted()`: its
  effects do what a component's `mounted()` would. The app makes them as it is made, in their order, and each finds
  those it uses itself, as a `shared<U>` member, among those given before it, so that none uses one that uses it. Their
  changes reach the app once it is made, and they go the last given first, telling the app of nothing as they go, so
  that one may still read those it uses as it goes;
- a `shared<T>` finds the app's context as the app makes its component: an app given none of that type throws
  `std::logic_error` there, before making the component, the app's own or a context included, whose destructor then
  never runs;
- `*shared` and `shared->` give the context; once a component read it so, a change of its states, or of a context it
  uses, shows that component again, whose view runs again even if its own states did not change. A component that
  only runs on it does not show again as it changes. A view that changes a context it reads shows again, as one
  changing its own states does: one that changes it at each view would show without end;
- a `shared<T>` is a member of the component itself, as a state is, and names the type as given, with no `const`: one
  inside a member of it is not found;
- its effects start after each view of the app, as a component's after its own;
- `shared.run(&T::member, args...)` runs a member function of the context, or of a base of it, with copies of the arguments, as a task of
  the app, and gives what it gave or throws what it threw: a component that goes stops waiting, and the work goes on,
  what it changes showing wherever it is read; what it throws once nothing waits goes to the app's `on_error`;
- `app.context<T>()` gives it from outside, as a test does;
- a component a view written once shows, with `to_html`, has no app: using its `shared<T>` throws
  `std::logic_error`.

A component provides a context to the components of its view, and theirs, as React's `Provider`, through a
`provided<T>` member: a `shared<T>` among them finds it before the app's, the nearest first, and it goes with the
component, as a page under a route keeps what only it needs.

```cpp
struct checkout_page {
  // The cart being filled, for the steps of the page: gone as the page goes.
  provided<cart_draft> cart;

  element view() {
    return div({component<cart_lines>(), component<payment_step>()});
  }
};

struct cart_lines {
  shared<cart_draft> cart;
  // ...
};
```

- its states show again the components reading it, and the component providing it; its effects start after each
  view of that component, as its tasks; it uses the contexts its component finds;
- a component using it keeps the component providing it while it lives, and `run()` as long as its work.

## Routing

The pages of an app, and the addresses that show them, are routes, checked at compile time: a route's parameters are
fields of its page's props, which give their types, and a link to a page takes those props.

```cpp
struct product_page {
  struct props {
    std::int64_t id = 0;
  };

  // Its sales, loaded again as the address gives another id.
  async_state<products::monthly_sales> sales;
  effect load{&props::id, products_client::sales, sales};

  element view(props const &given);
};

inline constexpr auto routes = ui::routes(
    ui::route<"/", home_page>(),
    ui::route<"/products", products_layout>(
        ui::route<"", product_list>(),          // /products, its `q` from ?q=tea
        ui::route<"{id}", product_page>()),     // /products/7
    ui::redirect<"/items/{id}", "/products/{id}">(),
    ui::not_found<missing_page>());

// The app's own component, or a layout: the page of the address in its outlet().
struct app_shell {
  element view() {
    return div({nav({link_to<home_page>("Home"), link_to("/products", "Products")}), outlet()});
  }
};

int main() {
  quoll::emscripten::mount<app_shell>(
      "#app", {.contexts = {ui::routing<routes>(quoll::emscripten::browser_history()), provide<catalog>()}});
}
```

- `routes(...)` is the table, in order: the first route that matches an address wins. A route of the top level
  starts with '/', one under another adds its own segments to its parent's, none for its index route;
- a route's path is a template, as the REST DSL's: `{name}` is a field of its page's props, of any type a number,
  a bool, a string or an optional of one gives, written `{id}` and not `{id:int64}`; `{name:path}` takes the rest of
  the path. An address whose parameter is no such value, as `/products/seven`, matches no route;
- the other fields of the props come from the query, `?q=tea`, percent-decoded, or stay as made when it lacks them;
- a route with routes under it is a layout: its page shows the page of the address in its `outlet()`, as the app's
  own component does in its own. A page shown for another value of its parameter is the same component, given other
  props, as an effect on that prop sees;
- `redirect<From, To>()` goes on to `To`, the parameters of `From` given by name and its query kept, replacing the
  address in the history; `not_found<Page>()` shows when no route matches;
- an address has one form, as a browser keeps it: rooted, its `.` and `..` segments resolved, no last '/' but for
  `/`; what goes onto the history and what is matched are both of it, and a route's path, a redirect's too, is
  written in it, which a path ending with '/' is not;
- `routing<routes>(history)` is a context of the app, `shared<router>` in a component. `go(href)` and
  `go<Page>(props)` push an address onto the history; going to the address shown, or to one that redirects there,
  takes its place instead, as a browser's link to the page it shows does. `replace(href)` takes the place of the one
  now, `back()` goes back, `href<Page>(props)` writes one, and `where` is the location, a state;
- `link_to<Page>(props, text)` and `link_to(href, text)` are links, `<a href>` that a click follows without leaving
  the page; a click with a key held, or another button, opens elsewhere, as the browser does;
- the history is the browser's, `quoll::emscripten::browser_history()`, whose back button the router follows, or one
  in memory, `routing<routes>("/products/7")`, for a program with no browser and for tests;
- a page's props are not in an unnamed namespace: the names of their fields, which a route reads, cannot be reflected
  in a type without linkage under Clang.

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
