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
    return div({
               text("Count: " + std::to_string(*count)) | tw("text-lg"),
               button("Add") | tw("btn btn-primary") | on_click([this] { count = *count + 1; }),
           }) |
           tw("flex items-center gap-2");
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
    return main_({
        input() | bind(draft) | attribute("placeholder", "A new note"),
        button("Add") | on_click([this] { items.update([this](auto &all) { all.push_back(*draft); }); }),
        when(items->empty(), [] { return p("Nothing yet"); }),
        ul({list(items, [](std::string const &item) { return li(item) | key(item); })}),
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
| Call an API            | A `rest::client` over [`fetch_client`](runtimes.md#fetch_client)                                                     |

The full table, with Tailwind and the build, is in [the sample](../samples/ui_notes/README.md#the-api).

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

- Nested components with a state of their own: a component is the root of an app for now.
- Base components.
- Making a page written by `to_html` live (hydration): a `value` on a `select` selects nothing yet.
- Restoring the states after a rebuild.

Tools that take Vite and Tailwind's setup out of an application, and a PWA, are
[step 7](PLAN.md#step-7-the-quoll-command-driving-vite-maintainer-merges-the-first-pull-request), and
desktop and mobile shells are [step 10](PLAN.md#step-10-desktop-and-mobile-maintainer-merges).
