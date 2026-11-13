# ADR 0002: the shape of the UI DSL

- **Status:** proposed, waiting for the maintainer's decision
- **Date:** 2026-11-13
- **Related:** [#182](https://github.com/4thlabs/quoll/issues/182) (Glaze for JSON only),
  [#173](https://github.com/4thlabs/quoll/issues/173) (contexts), [`PLAN.md`](../PLAN.md) step 6

## Context

A page of `quoll::ui` today, from the dashboard sample:

```cpp
// components/product_row.hpp
struct product_row_props {
  products::product item;
  bool selected = false;

  bool operator==(product_row_props const &) const = default;
};

extern component<product_row_props> const product_row;

// components/product_row.cpp
constexpr component<product_row_props> product_row = [](product_row_props const &given) {
  auto const name = span(given.item.name) | tw("flex-1 font-medium");
  auto const category = span(given.item.category) | tw("badge badge-ghost");
  auto const row = link_to(product_page, {.id = given.item.id}, {name, category});

  auto const shown = row | tw("flex w-full gap-3 px-4 py-2") | tw_if(given.selected, "bg-base-200");

  return li({shown});
};

// In the view of the list:
ul({list(items, [&](products::product const &item) {
  return product_row({.item = item, .selected = item.id == chosen}) | key(item.id);
})});
```

(Shortened: the sample shows the stock too.) A component is a lambda named with its props. Its state is what its `use_`
calls give, found again at each view by where they are called, as React's hooks. A view is a tree of `div({...})`,
decorated after its children with `|`. Before going on with step 6, four points are open:

1. **Two ways to write a component.** The maintainer asked for a struct as well as a lambda, a component being a
   function and C++ writing a function either way. The struct form drafted on the branch `ui-component-struct`, not
   pushed, fights the language: it needs `using component::component;` to take designated props, its base holds the
   element placed, a `sizeof` check refuses its members, and Clang caches the answer of a constraint asked while the
   struct is still incomplete, so every check had to move into function bodies (see B below).
2. **The props are a type apart.** `product_row_props` and `product_row` are two names for one thing. The header repeats
   the type in an `extern` declaration that its source defines again, and the parts of a view are free functions taking
   the props.
3. **Decorators come after the children.** In HTML, the classes are read with the tag, here they come after the whole
   tree. The samples name the tree to decorate it, `auto const shown = div({...}); return shown | tw(...);`.
4. **No reflection.** Contexts (their members walked) and routes (the props' fields found by name) go through Glaze,
   which [#182](https://github.com/4thlabs/quoll/issues/182) takes out. The
   maintainer wants no reflection of Quoll's own either (2026-11-13): what it gave is written by the user, or found
   another way, until C++26 reflection (as in option E of [ADR 0001](0001-rest-contract-shape.md)).

Each option is judged on:

- **Reading:** how close a view is to the page it shows, and to React's model, kept since step 6: components, props,
  hooks, keys.
- **Writing:** what a user has to write and to learn, and the messages for what is wrong.
- **Architecture:** a rebuild of one component in a second at most (its view in a source of its own, little instantiated
  for it), the size of the WebAssembly, and one table per type of component, as today.
- **Hot reload:** the Vite plugin replaces, without a rebuild, a string literal that is the only argument of `tw`,
  `text` or a tag (`tw("flex")`, `p("Hello")`): one followed by another argument, as `button("Add", ...)`, takes a
  rebuild. A raw string is code. Tailwind reads the classes in the source, written whole.
- **Safety:** the lifetimes (a handler captures the props by reference, and the hooks are stable), and misuse refused as
  it compiles.

Hooks are kept (part 3). Every example below shows the same `product_row`, and none is implemented beyond a sketch,
whose points to check are listed. The ones marked *new* are proposals.

## 1. A component

### A: a lambda named with its props (today)

The example of the Context.

- **For:** a function, as React's; a constant, so its identity is its table, and nothing is made before `main`.
- **Against:** the props are a type and a name apart. The header repeats the type, `extern component<props> const`,
  and the source has to include its own header first, or the `constexpr` definition gets internal linkage. Its parts are
  free functions taking `given`.

A lambda's place may also be taken by an empty struct whose call operator is the view, as
`constexpr component<badge::props> badge_shown = badge{};` (`docs/ui.md`): the struct frames the props, nested, and
the parts, with no base and no `using`, but the component is a second name beside the struct, and the props still come
in as `given`.

### B: a struct deriving from `component<name>`, its props inside it (the draft)

```cpp
struct product_row : component<product_row> {
  using component::component;

  struct props {
    products::product item;
    bool selected = false;

    bool operator==(props const &) const = default;
  };

  element operator()(props const &given) const;

private:
  element stock(props const &given) const;
};

element product_row::operator()(props const &given) const { ... }

// Placed:
product_row({.item = item, .selected = item.id == chosen})
```

- **For:** the props and the parts are framed in the struct.
- **Against:**
  - **The `using` line.** `using component::component;` is required for designated props, as C++ does not inherit
    constructors. Without it, the compiler's error speaks of designated initializers, not of Quoll.
  - **Twice the props.** They are nested in the struct, yet still passed as `given`.
  - **What the base holds.** It holds the element placed, which costs one allocation more per placement. A struct with
    members of its own is refused by comparing sizes.
  - **Checks only in bodies.** A constraint asked while the struct is incomplete is cached by Clang as false. Every
    check of the struct moved into function bodies.
  - **One name, two meanings.** `component<X>` means "of props X" for a lambda, and "the struct X" here.

### C: a function and its component *(new)*

```cpp
// components/product_row.hpp
struct product_row_props { ... };

element product_row_view(product_row_props const &given);

inline constexpr component_of<&product_row_view> product_row;

// components/product_row.cpp
element product_row_view(product_row_props const &given) { ... }
```

The function's address is the component's identity, and its props are deduced from its parameter.

- **For:** a plain function. There is no `extern`, as the constant is `inline` in the header and the function is
  defined in the source.
- **Against:** three names: the props, the view and the component. The props are still apart.

### D: the struct is its props *(new, recommended)*

A component is a class whose `element operator()() const` is its view. Its members are its props, read by name.

```cpp
// components/product_row.hpp
struct product_row {
  products::product item;
  bool selected = false;

  bool operator==(product_row const &) const = default;
  element operator()() const;

private:
  element stock() const;
};

// components/product_row.cpp
element product_row::operator()() const {
  auto const row = link_to(product_page{.id = item.id}, {span(item.name) | tw("flex-1 font-medium"), stock()});

  return li({row | tw("flex w-full gap-3 px-4 py-2") | tw_if(selected, "bg-base-200")});
}

element product_row::stock() const { return span(stock_text(item.stock)) | tw("text-sm opacity-70"); }

// Placed, with braces:
product_row{.item = item, .selected = item.id == chosen} | key(item.id)
```

A small one fits in its header:

```cpp
struct badge {
  std::string label;

  bool operator==(badge const &) const = default;
  element operator()() const { return span(label) | tw("badge"); }
};
```

- **How it works.** A struct placed is copied into the element, as the props are today. The app keeps its instance, and
  its hooks, by the struct's type and its place. Its view is called on that copy, so what a handler captures from it
  lives as long as the props. `==` is optional: with it, a component given equal props is not viewed again, as today.
- **For:**
  - one name and one type;
  - no base, no `using`, no constructor and no `extern`: the header declares the struct, the source defines its view;
  - its parts are const member functions that read the props directly;
  - routes name the type, `route<"{id}", product_page>(&product_page::id)` (part 5), and the links, `go` and the root
    take a component placed, `link_to(product_page{.id = 7}, "Open")` and `mount(app_shell{}, "#app")`.
- **Against:**
  - **Braces.** A component is placed with braces, `product_row{...}`, and a call becomes a construction. One with no
    props is `app_shell{}`.
  - **A member is a prop, never a state.** The view is `const`, so a write to a member does not compile, and a state is
    a hook, as in A.
  - **What counts as a component.** Any class whose call operator is the view could be taken for one, and a closure put
    in braces by mistake would be one. The concept asks for an aggregate, which a closure is not.
  - **Parts in the header rebuild more.** A part declared as a member function is in the header, so adding one or
    changing its signature compiles again every source placing the component, where today's free functions in the
    source compile only it. Parts that are free functions in the source, taking `*this`, keep today's cost. Each source
    placing a component also instantiates the small function calling its view, which today's lambda instantiates once,
    in its own source.
- **To check:**
  - designated initializers follow the order of declaration;
  - an aggregate may have private member functions, but no private members;
  - the concept's message when a view is missing, or is not `const`.
  A sketch of the placement and of the braces compiles on GCC 13 and Clang 19.

D can replace A everywhere, as the small one shows: shorter than a lambda with its props apart. A could stay beside it,
which makes two ways to learn and to document.

### E: members holding the state (set aside)

```cpp
struct counter {
  state<int> count{0};

  element operator()() const;
};
```

Members of a struct register themselves as it is made, without reflection. The struct components removed by #184 did
the same through Glaze. The maintainer chose hooks on 2026-11-12, for a third of the code: a state sits beside its use,
and props and state stay apart.

### F: signals and a view run once (later, for performance)

As in Leptos and Solid, a view runs once and binds signals to the DOM, `text([&] { return std::to_string(*count); })`.
There is no diff, which is faster for large lists. But each binding is a closure kept with the DOM, whose lifetime C++
does not check, and `app_core` changes wholesale. It also changes how every view is written: a view that branches on a
state, as `product_view`'s `if (sales->loading) return ...`, becomes a binding, `when(...)` or a closure, and hooks
are no longer found by their place in a view run again. Taking a shape of part 1 now leaves it out for good, unless
every component is rewritten.

## 2. The tree of a view

The example: the view of the dashboard's `product_view`.

### A: children in braces, decorators after (today)

```cpp
auto const failed = div({
    error_text(told(sales->error, "the sales did not load")),
    button("Try again") | tw("btn btn-sm self-start") | on_click(retry),
});

return failed | tw("flex flex-col gap-2");
```

- **For:** one `initializer_list`, with no template per call, and literals that are direct arguments of `tw` and the
  tags.
- **Against:** the classes and the handlers come after the children, which a long tree pushes far from the tag.

### B: decorators first, in the braces *(new)*

```cpp
return div({
    tw("flex flex-col gap-2"),
    error_text(told(sales->error, "the sales did not load")),
    button({tw("btn btn-sm self-start"), on_click(retry), text("Try again")}),
});
```

- **For:** the attributes read with the tag, as in HTML. The braces hold one `initializer_list` of a child or a
  decorator, so nothing more is instantiated. `|` stays, for a decorator added to an element made elsewhere.
- **Against:** a decorator in the middle of the children is accepted, or refused only at run time. A text in a tag that
  has decorators becomes `text("...")`.

### C: attributes in parentheses, children in brackets *(new)*

C++23 lets `operator[]` take several arguments.

```cpp
return div(tw("flex flex-col gap-2"))[
    error_text(told(sales->error, "the sales did not load")),
    button(tw("btn btn-sm self-start"), on_click(retry))[text("Try again")]
];
```

- **For:** the closest to HTML: attributes with the tag, then the children. A text is `text("...")`, its only
  argument, which the plugin replaces as today; `button("Try again", ...)`, with the text first, would take a rebuild
  unless the plugin learns that form.
- **Against:**
  - **Templates.** `()` and `[]` take any number of arguments, so a template is instantiated for each shape of call.
    They can forward at once to a function that is not a template, but the cost is to measure against the second's
    budget.
  - **`div` is a function** (a `div` object would be ambiguous with C's `::div`), so it is written `div()[...]`, while a
    tag that is an object could be `ul[...]`. Writing every tag `name(...)[...]` is consistent.
  - **No trailing comma.** A subscript takes an expression list, which has none, unlike braces: adding or moving the
    last child touches the line before. `[{...}]`, an `initializer_list` in the brackets, allows it and instantiates
    nothing per call, at the cost of the braces back.
  - **New to C++ readers:** multidimensional subscripts.
- **To check:** a sketch compiles on GCC 13 and Clang 19; Apple Clang 21 and Emscripten's Clang have the feature.

### D: an HTML template checked as it compiles *(new)*

```cpp
return html<R"(
  <div class="flex flex-col gap-2">
    {}
    <button class="btn btn-sm self-start" onclick={}>Try again</button>
  </div>)">(error_text(told(sales->error, "the sales did not load")), retry);
```

A `consteval` parser reads the template, as the path templates of the REST DSL are read, and checks the placeholders by
position.

- **For:** it reads as HTML, and Tailwind reads it.
- **Against:**
  - **No types by name.** Placeholders are checked by position only.
  - **Errors in a string**, which an editor neither colours nor completes.
  - **A parser to write.**
  - **Compile time.** Every view is a template instantiated by its own string.
  - **No hot reload.** The text is a template argument, part of the type: any change of it is a rebuild.

### E: builder methods (set aside)

```cpp
div().classes("flex flex-col gap-2").children({...});
```

Longer, and its methods are closed: today a user makes a decorator with `decorator(...)`, but cannot add a method.

## 3. State, effects and actions: hooks (kept)

`use_state`, `use_async_state`, `use_effect`, `use_action`, `use_form`, `use_context` and `provide_context` stay as
they are, found again by their place of call. No reflection is involved, and every form of part 1 calls them from its
view.

## 4. Contexts without reflection

Today a context is a struct whose members are states, effects and other contexts used. Glaze walks them to link the
states to the app, to start the effects and to find the contexts used:

```cpp
struct catalog {
  async_state<std::vector<products::product>> items;
  effect load{std::tie(), products_client::list, items};

  quoll::async::task<products::product> save(products::product edited);
};
```

### A: its members register themselves as it is made *(new, recommended)*

`provide<catalog>()` makes the context in place, with a scope open, as a view opens one for its hooks. Each state,
effect or `shared<T>` member joins, as it is constructed, the context being made. The context is never copied or moved
afterwards. Nothing changes for the user: `catalog` stays as written.

- **Against:** today the contexts a context uses are read from its members' types, before it is made, so that one
  missing refuses it before its constructor runs, and its destructor, which may write to them, never runs on a half
  found context. Registered as they are made, its members only find a missing context during its construction: the one
  that throws destroys those already made, whose destructors have to accept it. Whether a context uses others is no
  longer known as it compiles either.
- **To check:** a member made outside a context being made (a local `state<int>` in a function) has to stay a plain
  value, or be refused with a message; and a prototype, of the destruction above under ASan.

### B: members listed by hand (set aside)

```cpp
static constexpr auto members = quoll::ui::members(&catalog::items, &catalog::load);
```

A member added and forgotten in the list is silently left out.

## 5. Routes without reflection

Today `{id}` and the query's fields are found by name among the page's props.

### A: member pointers, in the order of the path's parameters *(new, recommended)*

```cpp
inline constexpr auto routes = ui::routes(
    route<"/products", products_layout>(
        route<"", product_list>(query<"q">(&product_list::q)),
        route<"{id}", product_page>(&product_page::id)));
```

Each `{name}` of the path takes the next member pointer, whose type gives how its text is parsed. A count that differs,
or a type that cannot be parsed, does not compile. A query field is named, as it is not in the path.

- **Against:** today every field of the props not in the path is a query field, found by name. Without reflection,
  each is listed: one added to the props and forgotten in the route is silently left out of the address, the cost that
  sets 4B aside. The path's parameters are bound by rank, as 2D's placeholders, which a path of one or two parameters
  keeps readable.

### B: names to members (beside A)

`route<"{id}", product_page>(param<"id">(&product_page::id))`: longer, but a parameter is found by its name rather than
its rank. It could be accepted beside A, for a path with several parameters.

### C: the page declares its parameters (set aside)

```cpp
static constexpr auto route_params = params(param<"id">(&product_page::id));
```

The page would then know its route, which the table of routes should hold.

## 6. Forms

`use_form(item)` and `bind(form, &product::price)` name a field by its member pointer, without reflection already.
Nothing changes.

## 7. Widgets

A widget's props go to JavaScript as JSON, through Glaze, which `widget.hpp` and `json_event.hpp` call directly. Glaze
stays for JSON (#182), where moving it behind a codec, as `rest::json_codec`, is that issue's work, not this one's.

## Comparison

| | Reading | Writing | Rebuild, size | Messages | Hot reload |
|---|---|---|---|---|---|
| **1A** lambda | function, props apart | two names, `extern` | good | good | yes |
| **1B** struct with base | framed | `using`, `given` | one allocation more | from the compiler, for `using` | yes |
| **1C** function | function, props apart | three names | good | good | yes |
| **1D** struct of props | framed, props by name | one name, braces | good, but parts in the header | a concept's | yes |
| **2A** braces, `\|` after | classes last | simple | good | good | yes |
| **2B** decorators in braces | attributes first | `text(...)` with them | good | an order checked at run time | yes |
| **2C** `()` then `[]` | as HTML | no trailing comma | to measure | good | with `text(...)` |
| **2D** HTML template | HTML | errors in a string | template per view | by position | no |

## Decision

To take, by the maintainer:

1. **A component:** D alone (recommended), D beside A, or B as drafted.
2. **The tree:** C (recommended, once its compile time is measured against the second's budget), B as its fallback, or A
   kept.
3. **Contexts:** A. **Routes:** A, with B for named parameters. **Forms:** unchanged.

Prototype before deciding: a sketch of 1D's placement compiles on GCC 13 and Clang 19, and 2C's brackets too. To
prototype: 2C's cost, a view of the dashboard rebuilt under Vite; 4A's destruction of a context half made, under ASan;
5A is plain `consteval` work, as the path templates are.

## Consequences

If D and C are taken:

- **`component<P>` goes.** A concept takes its place: an aggregate with `element operator()() const`. `app`, `mount`,
  `link_to`, `router::go` and `router::href` take a component placed. The draft branch already does this last part,
  and is kept for it.
- **Routes name types only,** with the member pointers of part 5.
- **Contexts** register their members (part 4).
- **Glaze leaves `quoll::ui`**, but for the JSON of widgets.
- **The tags** take attributes in parentheses and children in brackets, and `|` stays for decorating an element made
  elsewhere.
- **The plugin** keeps its rules: a text replaced at once is the only argument of `text` or a tag, and classes are in
  `tw("...")`.
- **The samples and `docs/ui.md`** are rewritten. `design-decisions.md` and `PLAN.md` say what was decided.
