# ADR 0002: the shape of a UI component

- **Status:** decided by the maintainer on 2026-11-13: forms S and D below, a component in its header only; the
  prototypes of the next pull requests confirm the details
- **Date:** 2026-11-13
- **Related:** [#182](https://github.com/4thlabs/quoll/issues/182) (Glaze for JSON only),
  [#173](https://github.com/4thlabs/quoll/issues/173) (contexts), [`PLAN.md`](../PLAN.md) step 6
- **Supersedes**, in `PLAN.md`'s Decided section and `design-decisions.md`: components as functions only (2026-11-12),
  one way to write a component, and a header and a source for each, never `inline` (2026-11-13)

## Context

A component of `quoll::ui` today, from the dashboard sample:

```cpp
// components/product_view.hpp
struct product_view_props {
  std::int64_t id = 0;
  products::product item;

  bool operator==(product_view_props const &) const = default;
};

extern component<product_view_props> const product_view;

// components/product_view.cpp
constexpr component<product_view_props> product_view = [](product_view_props const &given) {
  auto &sales = use_async_state<products::monthly_sales>();
  auto &load_sales = use_effect(given.id, products_client::sales, sales);

  auto const sales_shown = [&]() -> element {
    if (sales->loading) {
      return p("Loading its sales") | tw("opacity-70");
    }

    if (sales->error) {
      auto const retry = [&load_sales] { load_sales.reload(); };
      auto const failed = div({
          error_text(told(sales->error, "the sales did not load")),
          button("Try again") | tw("btn btn-sm self-start") | on_click(retry),
      });

      return failed | tw("flex flex-col gap-2");
    }

    return sales_chart({.sales = sales->value});
  };

  auto const shown = div({
      product_detail({.item = given.item}),
      sales_shown(),
  });

  return shown | tw("flex flex-col gap-8");
};
```

The maintainer keeps the view's tree as it is: tags, children in braces, decorators after them with `|`. The rest he
finds unreadable, "the concepts mixed":

- **The props are a type apart.** `product_view_props` and `product_view` are two names for one thing, and the header
  repeats the type in an `extern` declaration its source defines again.
- **One body holds everything.** States, effects, handlers and parts of the view are lines of one lambda, told apart by
  their order and by `use_` calls, with rules no compiler checks: a handler captures by value what the view computed,
  and a function calling hooks, called in a branch or in a loop whose turns change, shifts the later calls of its
  place. (`auto x = use_state(0)` does not compile, and a hook in a branch shifts nothing else.)
- **Two files a component** were taken for the rebuild's budget, a second at most for one component changed. The
  maintainer would rather have a component in its header only, if it compiles cheaply.
- **No reflection.** Contexts (their members walked) and routes (the props' fields found by name) go through Glaze,
  which [#182](https://github.com/4thlabs/quoll/issues/182) takes out of everything but JSON. No reflection of
  Quoll's own either, until C++26 has it.

Constraints: C++23 on GCC 13, Clang 19 and Apple Clang 21, no macro but include guards and `QUOLL_` switches,
WebAssembly first. The Vite plugin replaces at once, without a rebuild, a string literal that is the only argument of
`tw`, `text` or a tag, and Tailwind reads the classes in the source.

## What other frameworks do

| Framework | A component | Its state | What loads | Its view |
|---|---|---|---|---|
| React (TS) | a function, its props an object | hooks, `useState` | `useEffect(f, [deps])`, its cleanup written by the user | JSX |
| Solid (TS) | a function, run once | signals | `createResource(source, fetcher)` | JSX, compiled to DOM calls |
| Vue 3 (TS) | options (`props`, `data`, `watch`, `methods`), or `setup(props)` run once | `ref`, `reactive` | `watch(source, f)` | a template, or `h(tag, attrs, children)`, run again on change |
| Svelte 5 (TS) | a file, `$props()` | `$state` | `$effect` | markup, compiled |
| Angular (TS) | a class | `signal()`, `input()` fields | `effect`, `resource` | a template |
| Lit (TS) | a class, its properties declared | `@state` fields | `updated(changed)` | `` html`...` `` |
| Leptos (Rust) | `#[component] fn`, run once | `signal()` | `Resource::new(source, fetcher)` | `view!`, or builders |
| Dioxus (Rust) | `#[component] fn`, its parameters its props | `use_signal` | `use_resource` | `rsx!` |
| Yew (Rust) | a function with hooks, or a `Component` with `update(msg)` and `view` | hooks, or the struct | `use_effect_with` | `html!` |
| Iced (Rust), Elm | a state, messages, `update`, `view` | the state, a value | a task returned by `update` | functions, `column![...]`, `div [ attrs ] [ children ]` |
| Flutter (Dart) | a widget, and for state a `State` class | the `State`'s fields, `setState` | `initState`, `didUpdateWidget` | constructors |
| SwiftUI | a struct, its fields its props | `@State`, found by reflection | `.task(id:) {...}`, cancelled as the id changes | result builders, modifiers after |
| Compose (Kotlin) | `@Composable fun` | `remember { mutableStateOf(...) }` | `LaunchedEffect(key) {...}`, cancelled as the key changes | `Column(modifier) { ... }` |
| Blazor (C#) | a `.razor` file, `[Parameter]` props | its fields | `OnParametersSetAsync` | markup |
| Nui (C++) | functions giving elements | `Observed<T>` | none of its own | `div{class_ = "x"}(span{}("text"))` |

What follows for Quoll:

- **Every declaration that reads in sections is class-shaped**: Angular, Lit, SwiftUI, Blazor, Flutter's `State`, Yew's
  struct components, and Vue's options, an object of named sections. One named member for each thing held, its type or
  keyword saying what it is, behaviour in named methods, the view in one method. The maintainer's own sketch had this
  shape. In each, **the framework makes the object**, not the parent that places it: that is what Quoll's earlier struct
  drafts lacked.
- **Every function-shaped one** (React, Solid, Compose, Leptos, Dioxus) keeps the concepts apart by convention only.
  Dioxus reads well because its compiler checks what a closure borrows; C++'s does not.
- **A load cancelled as its key changes** is Compose's `LaunchedEffect(key)` and SwiftUI's `.task(id:)`, and Quoll's
  effect. React's `useEffect` leaves it to a cleanup the user writes.
- **A load as one value**, `Option<Result<T, E>>` and a `match` in Rust, reads better than three fields.

A study written alongside (the samples rewritten with axum, reqwest and Dioxus, kept out of the repository) found
Quoll better at its form, its action that refuses to overlap, its widget in one line, its nested routes, its key on any
component and the size of its page, and Dioxus better at the declaration of a component, `#[component] fn
ProductView(product: Product) -> Element`. Both forms below keep the first list.

## Forms set aside

| | Form | Why not |
|---|---|---|
| 1 | Today's lambda, props apart, hooks in its body | the Context's three points |
| 2 | A struct deriving from `component<name>`, nested `props`, view `element operator()(props const &)` | `using component::component;` in every struct; Clang caches as false a constraint asked while the struct is incomplete; props passed as `given` though nested |
| 3 | The struct is its props, hooks in `operator()() const` | placed with braces, and still one body holding states and view |
| 4 | States, contexts and effects as members, the struct made where it is placed | placing it in the parent's view makes its members there, registering in the parent's store; they see only the first props |
| 5 | A life cycle, `mounted`, `updated`, `unmounted`, as React's classes | one concern in three functions, its comparison written by hand; Quoll's `mounted()` went with #185 for this |
| 6 | Props, and a nested `state` struct defined in the source, as Flutter | two structs; `state` hides Quoll's `state<T>`; a table to find across sources |
| 7 | Sections in a nested `inside`: `using contexts`, `struct state`, a constant table of effects, a static view | "not better", by the maintainer |
| 8 | Signals and a view run once, as Solid and Leptos | every view rewritten, each binding a closure C++ does not check |
| 9 | A state, messages, `update`, `view`, as Elm and Iced | a message type for each event, and no props: far from React's model |

Two placements of form S were set aside as well: `product_view::with({...})`, which the maintainer found tedious, and
a constructor making an inert shell, which needs the `using` line again and constructs every member at each
placement.

## Decided: two forms, each in its header

Both are written as functions are in C++, as the maintainer asked: a struct with its sections, form S, or a function
of its props, form D. Both are header-only, place the same way and keep the decorators of the view.

### Form S: the struct is the component

```cpp
// components/product_view.hpp: the whole component
struct product_view : component<product_view> {
  struct props {
    products::product item;

    bool operator==(props const &) const = default;
  };

  // State: its sales, loaded as it first shows and again as the id changes, the load before stopped.
  resource<products::monthly_sales> sales{given->item.id, products_client::sales};

  // Handlers
  void retry() { sales.reload(); }

  // View
  element view() {
    auto const shown = div({
        product_detail({.item = given->item}),
        sales.match(
            [] { return p("Loading its sales") | tw("opacity-70"); },
            [this](std::exception_ptr const &error) { return sales_failed(error); },
            [](products::monthly_sales const &loaded) { return sales_chart({.sales = loaded}); }),
    });

    return shown | tw("flex flex-col gap-8");
  }

  element sales_failed(std::exception_ptr const &error) {
    auto const shown = div({
        error_text(told(error, "the sales did not load")),
        button("Try again") | tw("btn btn-sm self-start") | on_click(this, &product_view::retry),
    });

    return shown | tw("flex flex-col gap-2");
  }
};

inline constexpr tag<product_view> product_view;
```

With a context, a form and an action:

```cpp
// components/product_detail.hpp
struct product_detail : component<product_detail> {
  struct props {
    products::product item;

    bool operator==(props const &) const = default;
  };

  // Contexts
  context<catalog> products;

  // State: the product given, with what is typed; and its save, once at a time.
  form_state<products::product> form{given->item};
  action<&catalog::save> save{products};

  // View
  element view() {
    auto const shown = section({
        h2(given->item.name) | tw("text-2xl font-semibold"),
        labelled("Name", input() | bind(form, &products::product::name)),
        labelled("Price", input() | bind(form, &products::product::price)),
        labelled("Stock", input() | bind(form, &products::product::stock)),
        when(!form.valid(), not_numbers),
        when(form.valid() && save->error != nullptr, [this] { return save_failed(); }),
        button(save->loading ? "Saving…" : "Save") | disabled(save->loading || !form.valid()) |
            on_click(save.with(form)) | tw("btn btn-primary self-start"),
    });

    return shown | tw("flex flex-col gap-3");
  }

  static element not_numbers() { return error_text("The price and the stock are numbers"); }
  element save_failed() { return error_text("Not saved: " + told(save->error, "the save failed")); }
};

inline constexpr tag<product_detail> product_detail;
```

**Where each concept is:**

| Concept | Where | Read as |
|---|---|---|
| Props | `struct props`, first | `given->item` |
| Contexts | `context<T> name;` | `products->items` |
| State | `state<T> name{start};`, `form_state<T> name{given->item};` | `*count` |
| A load | `resource<T> name{what it follows, function};` | `sales.match(...)`, `sales.reload()` |
| An effect writing into a state | `effect name{what it follows, function, target};` | `load.reload()` |
| An action | `action<&function> name{context};` | `save->loading`, `save.with(form)` |
| Handlers | member functions | `on_click(this, &product_view::retry)`, or `[this] { retry(); }` |
| View | `element view()`, and its parts as member functions | |
| A context for the components of its view | `provided<T> name;` | as today's `provide_context<T>()` |

**How it works, without reflection:**

- **Who makes it.** Placing a component gives an element holding its table and its props, nothing more. The app, the
  first time it finds that element at a place, copies the props beside the instance and makes the struct there, a
  scope open: each member joins the instance in its constructor, as hooks join the scope of a view today. At each
  later view the app gives the props anew, or skips the view when they compare equal and no state changed, as today,
  then calls `view()`.
- **`given`** is a member of the base whose `operator->` has a deduced return type, so the base never names
  `product_view::props` while the struct is incomplete. It is read in member functions and member initializers, where
  the struct is complete: no `using` line, and no constraint asked on an incomplete type.
- **What an effect or a load follows** is named by reference, `given->item.id` or a state, and compared after each
  view; a value made on the spot, `given->item.id + 1`, does not compile (a deleted overload). A function of values,
  `[this] { return ...; }`, for what is computed.
- **A context** is the same struct without a view: `provide<catalog>()` makes it under the same scope, its members
  joining it, and Glaze no longer walks it. One change to today's: `effect load{std::tie(), products_client::list,
  items};` follows a value made on the spot, which the rule above refuses, so a load that follows nothing is
  `resource<std::vector<products::product>> items{products_client::list};`, or an effect with no key.
- **A struct made by hand**, `struct product_view mine;`, compiles and throws `std::logic_error` as it runs: C++23
  cannot refuse it as it compiles.

**The placement**, `product_view({.item = item})`, is the variable declared after the struct, of the struct's name,
as POSIX's `struct stat` and `stat()`. After that line, the name is the variable; its `operator()(props)` gives the
element, and Quoll reaches the struct through it. The rest reads as today:

```cpp
product_view({.item = item}) | key(item.id)
link_to(product_page, {.id = item.id}, "Open")
quoll::emscripten::mount(app_shell, "#app", options);
route<"{id}", product_page>(&product_page::props::id)
```

- Inside the struct, its name is still the struct: `&product_view::retry`, and `product_view::props` anywhere, as a
  name before `::` is looked up among types only.
- The line is the same for every component. Forgotten, `badge({.label = "new"})` is the struct's constructor, and the
  message speaks of initializers, not of Quoll (`too many initializers for 'component<badge>'` on GCC 13, `field
  designator 'label' does not refer to any field` on Clang 19). A component placing itself writes
  `tree::with({...})`, the base's.
- `product_view mine;` no longer compiles outside the struct, which is welcome. `-Wshadow` stays silent: neither name
  hides the other from an enclosing scope.
- The other way, if a name meaning two things is refused later: the props as the named struct, the component a struct
  nested in it, placed `product_view{.item = item}` with no line to add.

**What C++ makes awkward:**

- **Members are made in the order they are declared**, a state before the effect writing into it, a context before
  its action. Neither GCC 13 nor Clang 19 warns of one given by reference before it is made (Clang alone warns of one
  called too early): Quoll says it as the component is first made, `quoll::ui: an effect is declared after the state
  it writes into`.
- **A member's type is written**, `state<int> count{0};`: a member takes no `auto`.
- **The struct's name is written twice**, `struct product_view : component<product_view>`: deducing `this` would do
  without, which GCC 13 lacks.

**Lifetimes get simpler.** Everything a handler or an effect touches is a member of a struct that lives as long as its
component, which outlives its tasks: a handler that is a member function needs `[this]` alone. It is a convention, not a
guarantee: a lambda written in `view()` may still capture one of its locals by reference, and compiles. An effect's
function and an action's are fixed as the component is made. Nothing is found by where it is called: a branch not taken,
a loop or a part called twice change nothing. What stays: `given` holds the props of the last view, props given to a
child are copied, and a `callback` in props runs as a task of the parent.

### Form D: a function whose parameters are its props

```cpp
// components/product_view.hpp: the whole component
inline constexpr component product_view = [](products::product const &item) -> element {
  // Its sales, loaded as it first shows and again as the id changes, the load before stopped.
  auto &sales = use_resource(item.id, products_client::sales);

  auto const failed = [&sales](std::exception_ptr const &error) {
    auto const shown = div({
        error_text(told(error, "the sales did not load")),
        button("Try again") | tw("btn btn-sm self-start") | on_click([&sales] { sales.reload(); }),
    });

    return shown | tw("flex flex-col gap-2");
  };

  auto const shown = div({
      product_detail(item),
      sales.match(
          [] { return p("Loading its sales") | tw("opacity-70"); },
          failed,
          [](products::monthly_sales const &loaded) { return sales_chart(loaded); }),
  });

  return shown | tw("flex flex-col gap-8");
};

// Placed with its arguments, in the order of its parameters:
product_view(item) | key(item.id)
product_row(item, item.id == chosen)
```

- **How it works.** A deduction guide reads the lambda's parameters; the component keeps the arguments of a placement
  in a tuple, compares them with `==`, and calls the function with them. No props struct, no `operator==` to write, no
  `extern`, no source: today's lambda with its ceremony gone, and today's hooks. A component with many props takes a
  struct, as today, and keeps their names: `chart({.title = "Sales", .height = 64})`.
- **What C++ cannot give of Dioxus's form:**
  - names at the placement: `product_row(item, true)` does not say what `true` is; only a struct names them;
  - parameters by value: each is `T const &`, checked as it compiles, as a handler may refer to one;
  - a compiler checking what a closure captures: a handler capturing by reference a value its view computed compiles
    and dangles, and a function calling hooks, called in a branch or a loop whose turns change, shifts the later
    calls of its place. `docs/ui.md` keeps them as rules, as today.
- **A route's page with parameters** takes a struct, so that its fields can be named, and the struct is then named
  apart, `[](page_props const &given)`, its route `route<"{id}", product_page>(&page_props::id)`: the separate props
  type of today, for a page. A page with no parameter is either form.

### Between the two

| | Form D | Form S |
|---|---|---|
| Lines that are not blank, `product_view` above | 19 | 29 |
| Written beyond the view | nothing | `struct props` and its `==`, the base, the `tag<C>` line |
| Props at the placement | by position, or a struct for names | by name |
| Where the state is | `use_` calls in the body, found by where they are called | members, on top |
| What a handler may capture | what `use_` gives and the parameters by reference; what the view computed by value, by rule | `this` |
| What may go wrong unseen | a local captured by reference; a function calling hooks, in a branch or a loop | a local of `view()` captured by reference; an effect declared before its target, said as the component is first made |

Both are kept, without a rule choosing between them, as the maintainer asked for the lambda and the struct together:
form D reads best for a small component, form S frames a large one. What holds nothing to compare stays a function,
`element error_text(std::string_view text)`, with no framework in it.

### A load, matched

`resource<T>` (form S) and `use_resource` (form D) are a state and the effect loading it, as Solid's
`createResource`, Angular's `resource` and Dioxus's `use_resource`. Its value keeps a `std::expected<T,
std::exception_ptr>` once a run has ended, and whether one is under way; `match` takes what to show while it loads,
once it failed and once it gave a value. The three give an element, and a case forgotten does not compile. The first
function may take the value before, for a view that keeps showing it while it loads again, which Rust's
`Option<Result<T, E>>` alone cannot say. `loading()` stays for what needs only the flag, as a button's label. `effect`
with a target stays for what writes into a state the component also changes by hand.

### Routes without reflection

A page is named as it is placed, and a route names the field each `{parameter}` of its path fills, in order; the
query's fields are named:

```cpp
inline constexpr auto app_routes = routes(
    route<"/products", products_layout>(
        route<"", product_list>(query<"q">(&product_list::props::q)),
        route<"{id}", product_page>(&product_page::props::id)),
    redirect<"/", "/products">(),
    not_found<missing_page>());
```

The member pointer's type says how the parameter is read, and a count that differs or a field of another struct does
not compile (`quoll::ui: a route names one field of its page's props for each {parameter} of its path, in order`).
The name in `{id}` is then for the reader: nothing checks it against the field's. Today every field not in the path is
a query field, found by name; listed now, one added to the props and forgotten in the route is left out of the
address.

### Forms and widgets

Unchanged: `bind(form, &product::price)` names a field by its member pointer already, and a widget's props go to
JavaScript as JSON, where Glaze stays (#182).

## Compile cost, in headers only

A page is one Emscripten target whose `main.cpp` includes its routes, which include the pages, which include what they
place: header-only makes the page one translation unit, and any change compiles all of it. Measured with em++
(`-c`, the container's build):

| | Seconds |
|---|---|
| `<quoll/ui.hpp>` parsed | 1.8, of which Glaze's headers 1.5 |
| The same, precompiled | 0.30 |
| A component's unit today, over the precompiled header | 0.35 to 1.16, by what it holds |
| Each effect today, on types of its own | about 0.10 |
| Each form with its action today | about 0.16 |
| The dashboard's eleven units (its components and `main.cpp`) as one, precompiled header | 2.6 |
| The link | 0.36 |

So header-only does not fit the budget as Quoll is today. It does once two changes are made, both planned:

1. **Glaze out of `quoll::ui`'s headers**, but for the JSON of widgets
   ([#182](https://github.com/4thlabs/quoll/issues/182)).
2. **What is no template compiled once in Quoll's library**: effects, actions, forms and loads erased behind their
   types, so that a component instantiates little, the pull request the plan already names after the rebuild's
   budget.

On a model of form S built that way, a component costs about 6 ms. The model's precompiled header loads in 0.10 s,
the real one in 0.30 s, so a page reads about `0.30 + 0.01 x components + 0.36` seconds: under a second up to about
30 components, 1.5 s at 80. That is an estimate from a model, not the library: the next pull requests measure the
real one, and a check in CI keeps it. Past that, a page takes its heavy views into sources of their own, in plain C++,
or modules.

## Modules, later

Quoll's UI headers build as a module with no header changed, on Clang 19 and em++: a file exporting `<quoll/ui.hpp>`
with Glaze and the standard library in its global fragment. GCC 13 fails on it with an internal compiler error.

| A component's unit (em++, `-c`) | Seconds |
|---|---|
| `#include <quoll/ui.hpp>` | 1.80 |
| over the precompiled header | 0.33 |
| `import quoll.ui;`, the standard headers included | 0.30 |
| `import quoll.ui; import std;` | 0.18 |

- **Not faster than a precompiled header** at what one does: the gain is the standard library's headers going too,
  and no precompiled header to set up.
- **A module per component** rebuilds, at a change, every component importing it, one after the other along a chain:
  about 0.13 s a level. It beats one unit only past about 75 components. Splitting a component into an interface and
  an implementation unit stops that, and is two files again; `module :private;` does not help today.
- **The literals' hot reload** works through a module (`std::source_location` keys are the importer's places), with
  `QUOLL_UI_HOT` a definition of the module's build. Components in `.cppm` files need the extension added to Tailwind's
  sources and the plugin's.
- **What they cost:** GCC 15 for `import std;` (GCC 14 only lets CMake build modules), a CMake recent enough for
  `import std;`, `clang-scan-deps` in CI, clangd's support still experimental; Apple Clang to check on the Mac
  (`xcrun --find clang-scan-deps` and a two-file project with a `FILE_SET CXX_MODULES`).

Forms S and D need no module. Form S's struct, its `tag<C>` variable included, compiles as a header and as a module unit alike;
form D's lambda exported from a module was not tried, and GCC 13 already refused a lambda's type across modules. Modules
come later, as a module of `quoll.ui` beside the headers, once every compiler of the floor has the `std` module: a
change of the floor, GCC 13 to 15, which is the maintainer's to make.

## Consequences

In this order, each a pull request:

1. **Glaze out of the UI's headers, and the erasure** of effects, actions, forms and loads into Quoll's library, with a
   rebuild check in CI.
2. **Form S and form D** on the real library: `component<C>` as the base of form S, `tag<C>`, `context<T>`,
   `resource<T>` and `match`, members joining their instance, `component` deduced from a lambda's parameters for form
   D; routes by member pointers; contexts without Glaze. `product_view`, `product_detail` and `product_row` first,
   written both ways, read and timed before the rest.
3. **The samples, `docs/ui.md`, `design-decisions.md` and `PLAN.md`** rewritten: a component in its header only, which
   replaces the rule of a header and a source; the samples keep both forms.
4. **Modules**, once the floor is raised.

The draft of the struct form on the branch `ui-component-struct`, not pushed, is not taken: through the `tag<C>` variable, `link_to`,
`go`, `mount` and the routes keep today's calls, a component and its props, for both forms.
