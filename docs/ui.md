# UI

An interface written in C++ and compiled to WebAssembly, in the same language as the rest of an application. The
DOM gives text, input methods, selection and accessibility, and Tailwind the styles. Link `quoll::emscripten`, which
brings `quoll::ui`. [`samples/notes/web`](../samples/notes/web) is a whole page, with its build, and
[`samples/dashboard`](../samples/dashboard) a screen with a chart.js widget; what is still to come is
[at the end](#what-it-lacks).

## A component

A component is a function giving its view: a lambda, named once as a `component`, whose state is what the `use_`
calls of its view give, as React's hooks.

```cpp
#include <quoll/emscripten.hpp>
#include <quoll/ui.hpp>

#include <string>

using namespace quoll::ui;

inline constexpr component<> counter = [] {
  auto &count = use_state(0);

  auto const add = [&count] { count = *count + 1; };

  auto const shown = div({
      text("Count: " + std::to_string(*count)) | tw("text-lg"),
      button("Add") | tw("btn btn-primary") | on_click(add),
  });

  return shown | tw("flex items-center gap-2");
};

int main() { quoll::emscripten::mount(counter, "#app"); }
```

A state changing runs the view again, once however many changed in between, and only what differs reaches the DOM.
Handlers capture what a `use_` call gives by reference, as it lives as long as the component, and may be plain
functions or return a task:

```cpp
inline constexpr component<> notes_page = [] {
  auto &items = use_state<std::vector<std::string>>();
  auto &draft = use_state<std::string>();

  // Loaded once, as it first shows.
  use_effect([&items]() -> task<> { items = co_await load_notes(); });

  auto const add = [&items, &draft] {
    items.update([&draft](auto &all) { all.push_back(*draft); });
  };

  auto const nothing = [] { return p("Nothing yet"); };

  auto const row = [](std::string const &item) {
    return li(item) | key(item);
  };

  return main_({
      input() | bind(draft) | placeholder("A new note"),
      button("Add") | on_click(add),
      when(items->empty(), nothing),
      ul({list(items, row)}),
  });
};
```

| To                       | Write                                                                                                                                          |
| ------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| Show an element          | `div({...})`, `button("Add")`, `node("video", {...})`; `main_` for `<main>`                                                                    |
| Show a text              | `text("Hello")`, or `p("Hello")` for a tag holding it                                                                                          |
| Style it                 | `\| tw("flex gap-2")`, `\| tw_if(done, "line-through")`, classes written whole, as Tailwind reads them in the source                           |
| Set an attribute         | `\| placeholder("A note")`, `\| role("alert")`, `\| aria("label", "Name")`, `\| attribute("id", "price")`                                      |
| Set one while it holds   | `\| disabled(save->loading)`, `\| hidden(!*open)`, `\| required(true)`, `\| readonly(*locked)`                                                 |
| Show a value given       | `input() \| value(given.text) \| on_input(...)`, for a field showing a prop                                                                    |
| Handle an event          | `\| on_click(...)`, `\| on_input(...)`, `\| on_change(...)`, `\| on_key_down(...)` given the key                                               |
| Send a form              | `form({...}) \| on_submit(...)`, as Enter in one of its inputs does: the page stays, but for an `action`                                       |
| Edit a value             | `input() \| bind(draft)` for a state of a `std::string`, a checkbox `input() \| bind(done)` for one of a `bool`, `\| checked(*done)` for one given |
| Edit what the props give | `auto &form = use_form(given.item);`, then `input() \| bind(form, &product::price)`: see [Forms](#forms)                                      |
| Repeat, or show or not   | `list(items, ...)` with a `key()` on each, `when(condition, ...)`                                                                              |
| Hold a value             | `auto &count = use_state(0);`, `auto &sales = use_async_state<T>();`                                                                           |
| Read a state             | `*count`, `items->size()`                                                                                                                      |
| Change a state           | `count = 1;`, `count.set(1);`, `items.update(...)`                                                                                             |
| Load as a value changes  | `use_effect(given.id, load, sales)`: see [Effects and actions](#effects-and-actions)                                                           |
| Run once, as it shows    | `use_effect(load, items)`: on nothing, it runs after the first view, and is stopped as the component goes                                      |
| Save once at a time      | `auto &save = use_action<&catalog::save>(products);`, `use_action<&free_function>()`                                                           |
| Use a context            | `auto &products = use_context<catalog>();`: see [Contexts](#contexts)                                                                          |
| Call an API              | A `rest::client`, over [`fetch`](runtimes.md#httpclient-in-a-browser)                                                                          |
| Make a decorator         | `decorator([](element &target) { target.classes += " card"; })`                                                                                |

**A component's props are the parameters of its view**, each by const reference, which give its type:

```cpp
// None: a component<>.
inline constexpr component title = [] { return h1("Notes"); };

// One, or several, placed in order: note_row(item, remove).
inline constexpr component note_row = [](notes::note const &item, callback<void(std::int64_t)> const &on_remove) {
  auto const remove = [&item, &on_remove] { return on_remove(item.id); };

  return li({span(item.text), button("Delete") | on_click(remove)});
};

// A struct of props, whose fields are named as it is placed: chart({.title = "Sales", .height = 64}).
inline constexpr component chart = [](chart_props const &given) { ... };
```

- several parameters are compared together, each with its `==`: a component whose props compare equal to the last
  ones, and whose states did not change, is not viewed again. One of them without `==`, or one struct of props without
  it, and the component is viewed again at each view of its parent;
- its props are kept with it, past its parent's view: a parameter that is a view, as a `std::string_view` or a
  `std::span`, does not compile, and one takes a `std::string`;
- a component is `inline constexpr` in a header, or declared in a header with its props written,
  `extern component<note_row_props> const note_row;`, or `component<parameters<notes::note, bool>>` for several, and
  defined in a source of its own,
  `constexpr component<note_row_props> note_row = ...`, which includes its own header first, which gives the definition
  its linkage: a change of a view compiles again only its source, and a change of props its source and those placing
  it;
- made of a lambda capturing nothing, or of an empty struct whose call operator gives the view, as
  `constexpr component<badge::props> badge_shown = badge{};`. It is made for each view, and holds nothing: what lasts
  is its hooks. One capturing, or holding members, does not compile;
- its props by const reference: taken by value, or by a reference that is not const, they would be a copy that goes as
  the view returns, which a closure of the view may capture, or what its parent gave written into, and that does not
  compile either.

**A component is also written as a struct**, its concepts in sections: its props inside it, what it holds and what it
does as members, its handlers as member functions, and its view, placed by a `tag<name>` of its name declared after
it:

```cpp
struct note_counter : component<note_counter> {
  struct props {
    std::string label;

    bool operator==(props const &) const = default;
  };

  // State
  state<int> count{0};
  async_state<std::vector<std::string>> notes;
  effect load{count, load_notes, notes};

  // Contexts
  shared<catalog> products;

  // Handlers
  void add() { count = *count + 1; }

  // View
  element view() {
    return button(given->label + ": " + std::to_string(*count)) | on_click([this] { add(); });
  }
};

inline constexpr tag<note_counter> note_counter;

// Placed as any component:
note_counter({.label = "Notes"})
```

- the struct derives from `component<name>`; its props, if it has some, are `struct props` inside it, read through
  `given->`, those of its last view; its view is `element view()`. With no props, it is placed `name()`;
- the app makes the struct as it first places it, keeps it while it stays at that place, gives it its props anew at
  each view of its parent, and destroys it as it goes. Its members are made once: its states, effects, forms and the
  contexts it uses as `shared<T>` join it as they are made, with no reflection, a `shared<T>` the app was not given
  refusing it with `std::logic_error`. A member made later, as one an `optional` makes, joins nothing;
- an effect follows a state, or what its props give: a field of them, `effect load{&props::id, load_notes, notes};`,
  or a function of them, `effect load{[](props const &given) { return given.item.id; }, load_notes, notes};`, taking
  them by const reference or by value, not generic, for a nested field, one a base declares, or a value computed,
  never a pointer into them.
  Read from the props of each view, it runs again as that value changes, the run before stopped, and not as another
  prop does. Props equal to the last ones by their `==` give no view: a field their `==` leaves out is seen with the
  next props that differ. One outside the making of a struct, or following the props of another, as `&props::size`
  names a base's, throws `std::logic_error`;
- its handlers are member functions, `[this] { add(); }`: the struct lives as long as the component;
- its view may still call `use_context<T>()` and `provide_context<T>()`;
- `tag<name>` is what places it, and what routes, links, `go` and the app's root take, as a component made of a lambda:
  `route<"{id}", item_page, &item_page::props::id>()`, `link_to(item_page, {.id = 3}, "Open")`,
  `mount(app_shell, "#app")`.

**What a `use_` call gives is the component's own**, found again at each view by where it is called, and how many times
that place was called before in the same view:

- a branch not taken shifts nothing written elsewhere, and keeps what it holds;
- a function calling hooks may be called twice in a view, each call with its own. Calls of one place are told apart by
  their rank in the view: such a function called in a branch, or a loop whose number of turns changes, shifts the later
  calls of that place, as React's;
- write `auto &`: `auto x = use_state(0)` does not compile, as a copy would be unlinked;
- a `use_` call outside the view of a component throws `std::logic_error`.

## Components of a view

A view places another component by calling it with its props, as React's JSX does, each with its hooks of its own:

```cpp
struct note_row_props {
  std::string text;
};

inline constexpr component<note_row_props> note_row = [](note_row_props const &given) {
  auto &done = use_state(false);

  auto const toggle = [&done] { done = !*done; };

  return li(given.text) | tw_if(*done, "line-through") | on_click(toggle);
};

inline constexpr component<> notes_page = [] {
  auto &items = use_state<std::vector<std::string>>();

  auto const row = [](std::string const &item) {
    return note_row({.text = item}) | key(item);
  };

  return ul({list(items, row)});
};
```

**Its hooks are kept** from one view of the parent to the next, while it stays at the same place:

- with a key, the component and its key among those of the parent's view: `| key(item)` keeps each row with its item as
  the list changes;
- without one, the component and its rank there among those of the same component, so that one going hands its hooks
  to the next;
- two of one component with one key in a view, even in two lists of it, throw `std::logic_error`.

**Its props come from the parent's view**, which shows in its own view what changes there:

- `given` stays valid for what that view made: a handler of the view may keep a reference to it until its task ends,
  and an effect's function, or what follows an action's success, keeps the props of the view that wrote it;
- the props it gives a component of its own view may refer to its own `given`: they stay as long as those props. They
  may not refer into the value of one of its states, which a change replaces: pass the value.

**What a view writes takes effect once it shows**, as React's effects are those of the render committed: an effect's
dependencies and function, and what follows an action's success; a view that throws, or whose render fails, writes none.
A form is the exception, as the view shows it: `use_form` takes the value its view gives as the view runs. A run calls
the function of the view that asked for it, whose props it keeps, and a run of an action tells what followed a success
when it started: a view shown during a run changes nothing of it. So a closure captures by reference what a `use_` call
gives, and `given`, but by value what the view computed, as a handler does.

**It is decorated in its own view:** a component element takes a key and nothing else, and one decorated otherwise
throws `std::logic_error`.

**It comes and goes:**

- an effect on nothing runs once it first shows, after those of the components of its view, in the order the view
  places them, as React's;
- once the parent's view no longer has it, its effects, its actions and its handlers are asked to stop, and it goes once
  they have ended, as [what a stop does](async.md#stopping-a-task) says: what a run holds, a guard in its frame, goes as
  the stop unwinds it;
- a handler of its view runs as one of its tasks, so the component outlives it.

**The app's root** is a component with no props, given to the app, as `mount(app_shell, "#app")`: it is placed, kept and
viewed as any other, and goes as the app stops.

### A view not run again

A component whose props compare equal to the last ones, and none of whose states changed since, is not viewed again:
its last view is given again, with the props it was made with. The components inside it are viewed again only if they
changed.

```cpp
struct note_row_props {
  notes::note item;
  callback<void(std::int64_t)> on_remove;

  [[nodiscard]] bool operator==(note_row_props const &) const = default;
};
```

- **Props are compared with their `==`**, when they have one. Without one, the component is viewed again each time its
  parent is, unless the parent's view was itself given again. A component without props is compared as equal.
- **Only its states, the contexts it read, and its props by value view it again.** A prop that points to data the parent
  changes does not: make it a state, or pass the value itself.
- **A callback compares equal to any other**, whatever its function captures. A component not viewed again keeps the
  callbacks of its last view, which call with what the parent captured then: capture only what does not change from a
  view to the next, as what a `use_` call gives, and pass the rest as props.

### Calling the parent back

A component tells its parent what happened through a `callback<Signature>` in its props: a function the parent wrote,
which runs as one of the parent's tasks, not the child's.

```cpp
struct note_row_props {
  notes::note item;
  callback<void(std::int64_t)> on_remove;
};

inline constexpr component<note_row_props> note_row = [](note_row_props const &given) {
  auto const remove = [&given] { return given.on_remove(given.item.id); };

  return li({
      span(given.item.text),
      button("Delete") | on_click(remove),
  });
};

inline constexpr component<> notes_page = [] {
  auto &notes = use_state<std::vector<notes::note>>();

  auto const on_remove = [&notes](std::int64_t id) { return remove_note(notes, id); };

  auto const row = [&](notes::note const &item) {
    return note_row({.item = item, .on_remove = on_remove}) | key(item.id);
  };

  return ul({list(notes, row)});
};
```

**It belongs to the component whose view made it**:

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

**Made outside a view**, in a handler or an effect, a callback knows no component: it runs in the caller's task, as a
`std::function` would, and what its function captures is the caller's to keep alive.

## Effects and actions

What a component loads as its props or states change, and what it runs on a click, are hooks of it, which the view reads
as states. An effect writes what it gives into a state of the component, its target; an action holds where its runs are,
as React Query's `useMutation`. `use_effect` is React's `useEffect`: it runs after a view in which one of its values
changed. `use_action<&function>()` is what a handler runs, once at a time by default.

```cpp
struct sales_panel_props {
  std::int64_t id = 0;

  bool operator==(sales_panel_props const &) const = default;
};

inline constexpr component<sales_panel_props> sales_panel = [](sales_panel_props const &given) {
  // The sales, whether they are loading, and why they did not load.
  auto &sales = use_async_state<products::monthly_sales>();
  auto &load = use_effect(given.id, products_client::sales, sales);

  if (sales->loading) {
    return p("Loading its sales");
  }

  if (sales->error) {
    return button("Try again") | on_click([&load] { load.reload(); });
  }

  return sales_chart({.sales = sales->value});
};
```

**An `async_state<T>`**, in `<quoll/ui/data/async_state.hpp>`, is the target most components want: a state of an
`async_value<T>`, whose fields are `value`, `loading` and `error`, read as `sales->loading`. `async_state<>` has no
`value`, for an effect whose function gives nothing. An action is read as one, holding its own. It is a state like any other,
set and updated as one: `items.update([](auto &held) { held.value.push_back(...); })`.

**What a target receives.** An `async_state<T>` receives by its fields: `value` is what the function gives, whole, a
`bool` or a `std::tuple` too. A `state<T>`, or a `state<std::tuple<...>>` of several elements, receives by the type
of its elements:

- a `bool` is whether a run is under way;
- a `std::exception_ptr` is why the last run failed;
- every other element is one of the values the function gives, in order, a function giving a `std::tuple` for
  several.

An effect's target is loading from the start, as React's `useState(true)`, when its first run will start: the first
view of its component comes before it, and a view written once with `to_html`, which runs no effect, shows it
loading, as React's server rendering does, from where `use_effect` is called on: a view reading its target before shows
it loading in the view the first run asks for. One that depends on an optional holding none is not, as nothing runs.
The target is the one of its first view; made in a later view, as in a branch, it is loading as the run starts. As a run
starts, its target is loading and has no error, the values before kept, as a state keeps its value until it is set.
As it ends, the target holds all it gave at once, in one render, or why it failed, the values before kept. Such a
`state` holds one `bool` and one `std::exception_ptr` at most, and a function giving a `bool` or an error as a value
into it does not compile: it could not tell it from its flags, which an `async_state` names. Each value is of the type the function gives, with no
conversion, which could make a view of what the run held, and moves without throwing, so that a run writes all it
gave or nothing. A target needs neither flag:

```cpp
auto &sales = use_state<products::monthly_sales>();                     // the value alone
auto &sales = use_state<std::tuple<products::monthly_sales, bool>>();   // and whether it is loading
```

**An effect runs as its values change**, as React's `useEffect(…, [values])`:

- it depends on one value its view gives, as `given.id`, on several as `std::tuple{filter, page}`, given to the function
  in that order, or on none, which runs it once, as its component first shows;
- after a view of its component in which one changed, compared with `==`, once the view is shown, the function runs as
  a task of the component with a copy of each value, the function of the view that asked for the run, whose props it
  keeps;
- the run before is stopped, as React's cleanup: what it gives after the view of the change writes nothing, even an
  answer that came anyway;
- the view a later change makes comes before the run, which starts once that view is shown, as in React: it still
  shows the values before, not yet loading. A value that must match the props, as a product's sales, can be given with
  what it is of, and the view compare them;
- a value that is a `std::optional` gives what it holds; while one holds none, nothing runs, the run before stopped and
  the target loading no more;
- `reload()`, on what `use_effect` gives, runs it again after the view it asks for, as a change would: what a Try again
  button calls. An effect runs one way, after a view, as React has it. One that cannot start, comparing or copying its
  values, or starting its run, as for a lack of memory, stops the run before all the same and ends on that failure,
  which `reload()` tries again;
- every failure reaches the app's `on_error`, as a handler's, shown in its target or not: `on_error` hears of all, as
  React Query's global `onError`, and the target says what the view shows.

Several loads go in one function, which gives a `std::tuple`, and fail together:

```cpp
using product_detail = std::tuple<products::product, products::monthly_sales, bool, std::exception_ptr>;

task<std::tuple<products::product, products::monthly_sales>> load_detail(std::int64_t id) {
  auto [item, sales] = co_await when_all(products_client::get(id), products_client::sales(id));

  co_return {std::move(item), std::move(sales)};
}

auto &detail = use_state<product_detail>();

use_effect(given.id, load_detail, detail);
```

**A debounce** is an option, given last: a run that follows a change waits that long before it calls the function,
and the next change stops it there, as a search waits for what is typed. Its target is loading meanwhile, its values
kept; the first run that starts, and one `reload()` asks for, do not wait:

```cpp
// The products whose name holds `text`, from the server.
task<std::vector<products::product>> search(std::string text);

auto &query = use_state<std::string>();
auto &found = use_async_state<std::vector<products::product>>();

use_effect(*query, search, found, {.debounce = 300ms});
```

An effect with no target runs a function that gives nothing, as one that saves a draft as it is typed:

```cpp
use_effect(*draft, [](std::string text) -> task<> { co_await save_draft(std::move(text)); }, {.debounce = 1s});
```

**An action runs a function once at a time**, which its template argument names, and gives its signature:

- a free function returning a task, as `auto &save = use_action<&products_client::update>();`;
- a member function of a context, given the context, as `auto &save = use_action<&catalog::save>(products);` for
  `auto &products = use_context<catalog>();`, which runs as the context's `run()` does: its work goes on as the
  component goes.

It is read as an `async_state` is: `save->loading` while a run is under way, `save->error` why the last failed, and
`save->value` what the last that succeeded gave, a copy, none for a value it cannot copy or make empty, as a move-only one, which only
`run()`'s caller is given; a change of them shows its component again. What follows a success is given last,
as React Query's `onSuccess`:

```cpp
struct like_button_props {
  std::int64_t post = 0;
};

inline constexpr component<like_button_props> like_button = [](like_button_props const &given) {
  // The likes the server counts once liked.
  auto &like = use_action<&posts_client::like>();

  auto const said = like->value == 0 ? std::string{"Like"} : std::to_string(like->value) + " likes";

  // A click while a like is on its way would be refused: the button waits.
  return button(like->loading ? "…" : said) | disabled(like->loading) | on_click(like.with(given.post));
};
```

- `save.with(args...)` gives a handler that runs it with copies of the arguments, for `on_click`: what it gave and why
  it failed are what it holds, for the view to show, and a failure reaches the app's `on_error` too, as every failure
  of an effect or a handler;
- `co_await save.run(args...)` gives what the function gave, or what it threw, as a `std::expected`, for a handler
  that goes on with it, after what follows a success, which throws to it what it throws: its caller alone is given
  a failure, not `on_error`;
- both take the function's parameters, so that a braced argument builds one, as `save.with(id, {.name = "tea"})`;
- every failure of a run is its `error`, a lack of memory before it began too; one making the run throws from the call;
- a call while one is under way is refused, `run_refused` at once, the action left as it is;
- with `overlap::restart` as its second parameter, a call stops the run under way instead, which gives `run_replaced`
  and writes nothing;
- a run its caller stops, as a timeout does, is under way no more;
- what follows a success is told what the run gave, as `use_action<&catalog::save>(products, [&shown](product const
  &saved) { ... })`, `void()` for a function that gives nothing: the one of the last view that showed, as an effect's
  function, a run telling the one it started with; a failure, a refused call or a stopped one tell it nothing;
- its runs are its component's tasks: a component that goes, as a detail keyed by its product once another is chosen,
  stops them, and what follows them. A save that must go on, as one the list of the page shows, is a context's: the
  action runs a member function of it, whose work goes on as the detail stops waiting;
- one action, one run at a time and one state: in a list, each row is a component with its own, so that each shows its
  own, and two rows run theirs together.

What `use_action` gives lives as long as its component: a run under way refers to it. Arguments and values are copied
into the run, the arguments given to the function as rvalues, so that none can be a view, which does not compile.

## Forms

**`use_form(value)` edits a value its view gives**, as React Hook Form's `defaultValues`: each input bound to a field of
it shows what the value holds, or what is typed in it. An action runs with it as it stands:

```cpp
struct product_detail_props {
  products::product item;
};

inline constexpr component<product_detail_props> product_detail = [](product_detail_props const &given) {
  auto &products = use_context<catalog>();
  // The product edited: the one given, with what is typed.
  auto &form = use_form(given.item);
  auto &save = use_action<&catalog::save>(products);

  auto const not_numbers = [] { return p("The price and the stock are numbers"); };

  return section({
      h2(given.item.name),
      label({text("Name"), input() | bind(form, &products::product::name)}),
      label({text("Price"), input() | bind(form, &products::product::price)}),
      label({text("Stock"), input() | bind(form, &products::product::stock)}),
      when(!form.valid(), not_numbers),
      button(save->loading ? "Saving…" : "Save")
          | disabled(save->loading || !form.valid())
          | on_click(save.with(form)),
  });
};
```

- `bind(form, &product::price)` keeps an input and a field the same: a string or a number goes to an input, whose text
  it reads, a `bool` to a checkbox;
- what is typed is kept as it is, as `12,5` for a number, which does not read: `form.valid()` says whether every field
  typed in reads as its type;
- `form.value()` is the value given, with each field typed in that reads;
- `save.with(form)` runs the action with `form.value()` as it is at the click; a click while the form is not valid is
  refused, as one while a save is on its way;
- the value is taken as the view runs, as the view shows it: when it gives another, as once what was saved comes back,
  a field typed in whose text is what it would show of it is typed in no more, and one typed while the save was on its
  way stays, as React Hook Form's `keepDirtyValues`, as does `2.50` typed for a `2.5`;
- another value to edit from the start, as another product, is another component, placed with a `key()` of its own;
- the value is a struct, and a field bound is a string, a number or a bool: anything else does not compile.

## Contexts

Data several components show and change, as the products a list and a detail share, is a context: an aggregate with
no view, its states, its effects and member functions. The app makes it from its options and holds it as long as it
lives, and a component finds it with `use_context<T>()`, by type.

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

inline constexpr component<> product_list = [] {
  auto &products = use_context<catalog>();

  if (products->items->loading) {
    return p("Loading the products");
  }

  // ...
};

int main() {
  quoll::emscripten::mount(dashboard_page, "#app", {.contexts = {provide<catalog>()}});
}
```

- `provide<T>()`, in `<quoll/ui/context/provide.hpp>`, gives the app a context, one of each type. The app makes them as
  it is made, in their order, and each finds those it uses itself, as a `shared<U>` member, among those given before
  it, so that none uses one that uses it. Their changes reach the app once it is made, and they go the last given
  first, telling the app of nothing as they go, so that one may still read those it uses as it goes;
- `use_context<T>()` gives a `shared<T>&`, the context of the components above that provide one, the nearest first,
  or the app's: one none provides throws `std::logic_error` as the view calls for it;
- `*shared` and `shared->` give the context; once a component read it so, a change of its states, or of a context it
  uses, shows that component again, whose view runs again even if its own states did not change. A component that
  only runs on it does not show again as it changes. A view that changes a context it reads shows again, as one
  changing its own states does: one that changes it at each view would show without end;
- its effects start after each view of the app, as a component's after its own;
- `shared.run(&T::member, args...)` runs a member function of the context, or of a base of it, with copies of the
  arguments, as a task of the app, and gives what it gave or throws what it threw: a component that goes stops waiting,
  and the work goes on, what it changes showing wherever it is read; what it throws once nothing waits goes to the
  app's `on_error`. `use_action<&T::member>(shared)` runs one so, once at a time;
- `app.context<T>()` gives it from outside, as a test does;
- a component a view written once shows, with `to_html`, has no app: calling `use_context` there throws
  `std::logic_error`.

A component provides a context to the components of its view, and theirs, as React's `Provider`, with
`provide_context<T>()`: `use_context<T>()` among them finds it before the app's, the nearest first, and it goes with the
component, as a page under a route keeps what only it needs.

```cpp
inline constexpr component<> checkout_page = [] {
  // The cart being filled, for the steps of the page: gone as the page goes.
  auto &cart = provide_context<cart_draft>();

  return div({cart_lines(), payment_step()});
};

inline constexpr component<> cart_lines = [] {
  auto &cart = use_context<cart_draft>();
  // ...
};
```

- its states show again the components reading it, and the component providing it; its effects start after each
  view of that component, as its tasks; it uses the contexts its component finds, which are there before it is made;
- a component using it keeps the component providing it while it lives, and `run()` as long as its work.

## Routing

The pages of an app, and the addresses that show them, are routes, checked at compile time: a route's parameters are
fields of its page's props, which give their types, and a link to a page takes those props.

```cpp
// components/product_page.hpp, its view in components/product_page.cpp; home_page and app_shell alike.
struct product_page_props {
  std::int64_t id = 0;

  bool operator==(product_page_props const &) const = default;
};

extern component<product_page_props> const product_page;

// routes.hpp
inline constexpr auto routes = ui::routes(
    ui::route<"/", home_page>(),
    ui::route<"/products", products_layout>(
        // /products, its `q` from ?q=tea
        ui::route<"", product_list, ui::from_query<"q", &product_list_props::q>>(),
        // /products/7
        ui::route<"{id}", product_page, &product_page_props::id>()),
    ui::redirect<"/items/{id}", "/products/{id}">(),
    ui::not_found<missing_page>());

// components/app_shell.cpp: the app's root, or a layout, shows the page of the address in its outlet().
constexpr component<> app_shell = [] {
  return div({nav({link_to(home_page, "Home"), link_to("/products", "Products")}), outlet()});
};

// main.cpp
int main() {
  quoll::emscripten::mount(
      app_shell, "#app", {.contexts = {ui::routing<routes>(quoll::emscripten::browser_history()), provide<catalog>()}});
}
```

- `routes(...)` is the table, in order: the first route that matches an address wins. A route of the top level
  starts with '/', one under another adds its own segments to its parent's, none for its index route;
- a route's page is a component, named by the object, which a header declares: the route and the links name it so;
- a route's path is a template, as the REST DSL's, written `{id}` and not `{id:int64}`; `{name:path}` takes the rest
  of the path. After its page, the route names the field of its page's props each `{parameter}` of its whole path
  fills, in order, as `&product_page_props::id`, of any type a number, a bool, a string or an optional of one gives.
  A count that differs, or a field of another struct, does not compile. An address whose parameter is no such value,
  as `/products/seven`, matches no route;
- a field the query gives is named, `from_query<"q", &props::q>`, percent-decoded, and one a parameter of the path
  matched gives, that of a route under it, as a layout's of the page it shows, `from_path<"id", &props::id>`. Each
  stays as made when the address lacks it, and a field the route does not name stays as made;
- a route with routes under it is a layout: its page shows the page of the address in its `outlet()`, as the app's
  root does in its own. A page shown for another value of its parameter is the same component, given other props, as
  an effect on that prop sees;
- `redirect<From, To>()` goes on to `To`, the parameters of `From` given by name and its query kept, replacing the
  address in the history; `not_found<Page>()` shows when no route matches;
- an address has one form, as a browser keeps it: rooted, its `.` and `..` segments resolved, no last '/' but for
  `/`; what goes onto the history and what is matched are both of it, and a route's path, a redirect's too, is
  written in it, which a path ending with '/' is not;
- `routing<routes>(history)` is a context of the app, `use_context<router>()` in a component. `go(href)` and
  `go(page, props)` push an address onto the history; going to the address shown, or to one that redirects there,
  takes its place instead, as a browser's link to the page it shows does. `replace(href)` takes the place of the one
  now, `back()` goes back, `href(page, props)` writes one, and `where` is the location, a state;
- `link_to(page, props, text)`, `link_to(page, text)` for a page without props, and `link_to(href, text)` are links,
  `<a href>` that a click follows without leaving the page; a click with a key held, or another button, opens
  elsewhere, as the browser does. A link's address holds the fields of `props` its page's route names, and no other;
- the history is the browser's, `quoll::emscripten::browser_history()`, whose back button the router follows, or one
  in memory, `routing<routes>("/products/7")`, for a program with no browser and for tests.

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

inline constexpr component<> sales_page = [] {
  auto &sales = use_state<std::vector<double>>();
  auto &selected = use_state(0);

  auto const select = [&selected](point_selected event) { selected = event.index; };

  auto const chart = line_chart({.labels = months(), .values = *sales});

  return chart | tw("h-64") | on("point_selected", select);
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

`ui::app` shows its root through a `Renderer`: an interface of node operations, which `dom_renderer` applies to the
page. `quoll::emscripten::mount(root, selector, options)` puts the two together. It takes a CSS selector, `#app` by
default, keeps the app for as long as the page, one app for each root, and gives it, as `app.context<T>()` reaches
its contexts.

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
  sources: ["src", "../api"],
  debug: "full",
});
```

`sources` names files, or folders, each of whose C++ files, at any depth, the plugin watches and reads for its widgets;
one made in a folder later is watched as it appears, and one removed from a folder leaves the page, which rebuilds
without it; a file named in `sources` stays one of them.

## A page's folder

A page keeps at its root what its tools read, and its code in `src/`, by role, as the samples do:

```
web/
├── CMakeLists.txt  vite.config.js  package.json  index.html  dev.sh
└── src/
    ├── main.cpp           mount(app_shell, ...)
    ├── main.js  style.css the page's script and styles
    ├── routes.hpp         its routes and the app's options
    ├── client/            the calls to its server, in sources of their own
    ├── contexts/          what its components share
    ├── components/        a header and a source for each component
    ├── ui/                functions giving a piece of view, with no state
    └── widgets/           the JavaScript modules of its widgets
```

The CMake that declares the page's components, the sample's beside `web/` in the samples, reads them from their
folder, at any depth: one made or removed there is compiled, or no longer, at the next rebuild, which configures again
as the folder changes. A source of another folder, as one of `client/`, is named there by hand.

```cmake
file(GLOB_RECURSE my_page_components CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/src/components/*.cpp)
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
