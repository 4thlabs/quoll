# UI

> [!WARNING]
> `quoll::ui` is a **prototype**. It works in [`samples/ui_notes`](../samples/ui_notes), and what it
> lacks is below. Do not build on its API yet.

An interface written in C++ and compiled to WebAssembly, in the same language as the rest of an
application. The DOM gives text, IME, selection and accessibility, and Tailwind the styles. Link
`quoll::emscripten`, which brings `quoll::ui`.

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
| Style it               | `\| tw("flex gap-2")`, `\| tw_if(done, "line-through")`, classes written whole, as Tailwind reads them in the source |
| Set an attribute       | `\| attribute("placeholder", "A note")`                                                                              |
| Handle an event        | `\| on_click(...)`, `\| on_input(...)`, `\| on("submit", ...)`                                                       |
| Edit a value           | `input() \| bind(draft)`, for a `state<std::string>`                                                                 |
| Repeat, or show or not | `list(items, ...)` with a `key()` on each, `when(condition, ...)`                                                    |
| Change a state         | `count = 1;`, `count.set(1);`, `items.update(...)`                                                                   |
| Call an API            | A `rest::client`, over [`fetch`](runtimes.md#httpclient-in-a-browser)                                                     |

The full table, with Tailwind and the build, is in [the sample](../samples/ui_notes/README.md#the-api).

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
  those props.

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

    bool operator==(props const &) const = default;
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

For now this is a plugin of the sample, driven by Vite, in three levels: CSS through Tailwind, a literal
of `tw()`, `text()` or a tag sent to the running program with no compilation, keeping its state, and
anything else by a rebuild that reloads the page, losing its state. To try it, see
[the sample](../samples/ui_notes/README.md#try-hot-reload).

## What it lacks

From [step 6](PLAN.md#step-6-the-ui-dsl-complete) of the roadmap:

- Base components.
- Making a page written by `to_html` live (hydration): a `value` on a `select` selects nothing yet.
- Restoring the states after a rebuild.

Tools that take Vite and Tailwind's setup out of an application, and a PWA, are
[step 7](PLAN.md#step-7-the-quoll-command-driving-vite-maintainer-merges-the-first-pull-request), and
desktop and mobile shells are [step 10](PLAN.md#step-10-desktop-and-mobile-maintainer-merges).
