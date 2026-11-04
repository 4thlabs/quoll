# Notes in a browser

The notes of [`samples/notes`](../notes) as an interface in C++: compiled to WASM, styled with Tailwind and daisyUI, on the
same contract as the server. A prototype: see the UI DSL in [`docs/PLAN.md`](../../docs/PLAN.md).

## Run it

With Docker, from the root of the repository. The image holds Clang, Emscripten and Node; building it takes some minutes,
once.

```bash
docker build -t quoll-dev .devcontainer

docker run --rm -it -p 5173:5173 -e QUOLL_POLL=1 \
  -v "$PWD":/workspaces/quoll -v quoll-cache:/home/dev/.cache -w /workspaces/quoll \
  quoll-dev samples/ui_notes/dev.sh
```

Then open <http://localhost:5173>. The first build compiles everything, for a minute or two. `QUOLL_POLL=1` is for macOS and Windows, where a
shared folder does not always report its changes.

Do not run `npm` on the host in this folder: `node_modules` holds the container's binaries.

In the dev container, or with Emscripten, Node and a C++23 compiler at hand, `samples/ui_notes/dev.sh` is enough.

## Try hot reload

Add a note, start typing another one, and change `notes_page.hpp` in your editor:

| Change | What happens | The note you are typing |
|---|---|---|
| `button("Add")` to `button("Ajouter")` | shows at once, without compiling | stays |
| `tw("btn btn-primary")` to `tw("btn btn-secondary")` | shows at once | stays |
| `tw("text-4xl font-bold")` to `tw("text-4xl font-bold text-rose-500")`, a class not used yet | Tailwind generates it, and it shows at once | stays |
| A comment, or spaces | nothing | stays |
| Anything else, such as adding a `state` | a rebuild of about 1 s, then the page reloads | **lost** |
| `int x = oops;` | the compiler's first error, over the page | |

## The API

```cpp
#include <quoll/emscripten.hpp>
#include <quoll/ui.hpp>

using namespace quoll::ui;

// A component: an aggregate with a view(), and state<T> members for what the view depends on.
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

int main() { quoll::emscripten::mount<counter>(); }   // in the element #app
```

| To | Write | Notes |
|---|---|---|
| Show an element | `div({...})`, `button("Add")`, `node("video", {...})` | braces for children, a string for a text; `node` for any tag; `main_` for `<main>` |
| Show a text | `text("Hello")` | |
| Style | `\| tw("flex gap-2")`, `\| tw_if(done, "line-through")` | write classes whole: Tailwind reads them in the source |
| Set an attribute | `\| attribute("placeholder", "A note")` | |
| Handle an event | `\| on_click(...)`, `\| on_input(...)`, `\| on("submit", ...)` | a plain function, or one returning a `task<>` |
| Edit a value | `input() \| bind(draft)` | `draft` is a `state<std::string>` |
| Repeat | `list(items, [](auto const &item) { return li(item) \| key(item); })` | `key` keeps a node when items move |
| Show or not | `when(!items->empty(), [] { return p("..."); })` | a plain `if` works too, between two returns |
| Change a state | `count = 1;`, `count.set(1);`, `items.update([](auto &all) { all.clear(); });` | the view then runs again, once |
| Read a state | `*count`, `items->size()` | |
| Load at the start | `task<> mounted() { items = co_await api.call<notes::list>({}); }` | an optional member |
| Call an API | `quoll::rest::client<notes::api, fetch_transport> api{fetch_transport{""}};` | the same contract as the server, so a wrong call does not compile; in a source of its own, see below |
| Split the view | a member function returning `element` | nested components with a state of their own are not there yet |
| Make a decorator | `decorator([](element &target) { target.classes += " card"; })` | |

Capture `this` in handlers, or values: the component lives at a stable address, and a handler stays alive until its task
ends.

## Keep rebuilds short

A page can live in a header, with no source of its own. Two things keep its rebuilds short:

- **Quoll, precompiled:** `target_precompile_headers` in `CMakeLists.txt`. Quoll stays header-only: this only tells the
  compiler to parse it once.
- **REST and JSON in a source apart:** `notes_client.cpp`, which compiles only when a call changes. It relays the contract
  by hand for now; it could be generated from it.

Measured from a save to the end of the build, for a change in the page, on one machine with 8 cores: 3.9 s with everything
in one source and nothing precompiled, about 1 s as it is here.

## Build for production

`npm run build` writes everything to `dist/`, the C++ compiled for size: about 51 KB of gzipped WASM, 7 KB of CSS and 5 KB of
JavaScript. Quoll's server does not serve static files yet.
