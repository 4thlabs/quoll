# Design decisions

The choices Quoll made where they matter to a program written with it: the question, what was taken, what was set
aside, and what follows for you. The guides say how things behave; this page says why they behave that way. It is
written from the Decided section of [`PLAN.md`](PLAN.md#decided), which also holds the roadmap and the decisions still
to make.

## Async

### An async core of its own, after Capy's design

**The question.** What do tasks run on: asio's coroutines, `std::execution`, a library such as Capy, or a core of
Quoll's own?

**Taken.** `quoll::async`, which follows the design of Boost.Capy without depending on it: an `await_context` handed to
each operation as it suspends (its executor, its stop token, the owner of its chain), an `executor_ref` that posts an
intrusive work item without allocating, and tasks that know no adapter.

**Set aside.** asio's coroutines tie every task to asio, which a browser does not have. `std::execution` comes with
C++26, past the C++23 Quoll requires. Capy itself, as a dependency: it was not in Boost yet and changed every week, its
thread pool and strands use `std::thread`, no support for Emscripten was mentioned, and it brings Boost along.

**For you.** The same task runs natively and in a browser, on any executor that models `async::Executor`, and an
adapter is all it takes to bring another loop.

### A stop is no exception

**The question.** When a stop is asked for, how does a task waiting on an operation end?

**Taken.** As a third kind of end, beside a value and an exception, with the semantics of C++26's `std::execution`
(`set_stopped`, `stopped_as_optional`) and the mechanism of Capy: the operation that sees the stop resumes nothing, and
the owner of the chain (a `co_spawn` root, a `when_all` branch, `stopped_as_optional`) destroys the chain from its top,
each frame's locals with it. Rust does the same by nature: cancelling a future is dropping it.

**Set aside.** An exception, `std::system_error` with `std::errc::operation_canceled`, as asio gives: a `catch` around an
operation takes it for a failure, and a loop that catches everything and goes on spins once stopped. An error code in
the result: the same, for a loop that goes on after an error. A guide warning against both does not prevent them.

**For you.**

- No `catch` sees a stop, and no line after the `co_await` runs: a loop cannot spin on one.
- What must still run once stopped, tasks to join or an answer to send, asks for the stop:
  `co_await async::stopped_as_optional(work)` gives `std::nullopt`.
- A handler of `co_spawn` has a case for `async::stopped_t`; `when_all` with a failure and a stop ends on the failure.
- A destructor cannot `co_await`: what must go out on the way out goes through `stopped_as_optional`, not RAII.

### A coroutine frame is destroyed only once it will not resume

**Taken.** A task is awaited as a temporary, or run by `co_spawn`, `when_all` or `with_timeout`: its `await_suspend` is
private to Quoll's own awaiting, and a debug build asserts that a frame is never destroyed while it could still resume.

**Set aside.** Letting an awaitable be destroyed while queued, which only a test destroying a suspended frame by hand
reached, at the cost of a function to run per work item.

**For you.** You never start a task by hand, and `co_await std::move(work)` is how a task held by name is awaited, as
a `static_assert` asks.

### A task that fails beside the others is reported, and the program goes on

**Taken.** `async::detached` writes the exception of a task spawned beside the main one to stderr, and the program goes
on, as tokio does; `async::set_report_sink` sends those lines elsewhere. A stop says nothing, nor does a
`std::system_error` with `std::errc::operation_canceled`, which code outside Quoll may still throw for one.

**Set aside.** Ending the program: one connection's failure would take a server down.

### Threads come from the runtime, a strand per connection

**Taken.** `runtime_options::threads` runs the asio runtime on several threads, the tasks of one connection taking
turns on a strand; `async::offload` runs blocking work; `async::run_loop` is a runtime of one thread for tests.

**Set aside.** A thread pool of `quoll::async`'s own, and `serve(executor, listener)` on it, which had no timer for
the timeouts of its connections.

**For you.** A handler needs no lock for what belongs to its connection. What handlers of several connections share,
a counter or a map, needs an atomic or a mutex once the runtime runs several threads: see
[Several threads](runtimes.md#several-threads).

### Coroutine frames are recycled

**Taken.** Each thread keeps the frames of tasks that ended, by classes of size from 64 bytes to 8 KiB, up to 64 a
class, to give to the next task of that class. A larger frame is allocated, and nothing is recycled under
ThreadSanitizer, HWASan or MemorySanitizer, which must see each allocation.

**For you.** A body read or written in pieces allocates nothing per piece in steady state; the benchmark holds the
allocations per request to a budget: see [Performance](performance.md). Measure allocations in a build without those
sanitizers.

## HTTP

### A stack of its own, on llhttp

**The question.** Which HTTP implementation: Boost.Beast, Boost.Http, Burl, or one of Quoll's own?

**Taken.** Quoll's own server and client, on [llhttp](https://github.com/nodejs/llhttp), Node.js's parser, compiled and
fetched by CPM.

**Set aside.** Beast does nothing for a browser and ties a program to asio. Boost.Http, on Capy, was studied: it builds
through CPM, but brings Boost along (2.3 GB), exists only on a development branch, changed every week, and one of its
files fails under WebAssembly. Burl, CppAlliance's client, is tied to Corosio for its sockets and TLS. llhttp has been
stable for years, is plain C, and is known to build to WebAssembly: undici runs it so.

**For you.** The server and the client run over any byte stream, and the client runs in a browser over `fetch`, with
the same code.

### On the Internet, behind a reverse proxy

**Taken.** Quoll's server is meant to run behind a reverse proxy (nginx, Caddy, a load balancer) when it faces the
Internet. It still defends itself: limits on sizes and time are on by default, and what a review or the fuzzers find
that touches security or stability is fixed (a crash, undefined behaviour, a data race, request smuggling, header
injection, leaving a directory, a connection stuck for ever).

**Set aside.** Facing the Internet directly: each review of the protocol code kept finding more corners of it. A corner
of an RFC that touches neither security nor stability, as a less exact status, may be left.

**For you.** See [On the Internet](server.md#on-the-internet) for what to set on the proxy and on the server.

### A handler writes its body itself

**Taken.** `http::response_writer`: a handler writes its body as it goes, as `res.write` does in Express, awaited by what
serves the connection.

**Set aside.** Returning a function that writes the body once the handler has ended: what it captures by reference
would dangle.

**For you.** The handler runs once the middlewares have returned, awaited by what serves the connection: a middleware
neither sees the status of a response whose body is written that way, nor catches what its writer throws, nor bounds
its time. See [Streaming](streaming.md).

### A kept connection is checked before it is used again

**The question.** A server may close an idle connection at any time. When may the client send a request on one again?

**Taken.** The idle connection is checked first, as curl does, by a poll of its socket that does not wait: one the
server closed, or that carries bytes nothing asked for, is closed. Every request whose body is whole then takes an idle
connection, as Go's client does; one found closed before any of it was written goes again on a new connection, as
hyper does; a POST of which anything was written is not sent twice, unless it carries an `Idempotency-Key` or an
`X-Idempotency-Key` field, which makes it idempotent to the client, as to Go's. An idle connection older than
`client_options::max_idle_time`, 90 s as Go's `IdleConnTimeout`, is not used again, as nothing on it shows that
something on the way dropped it.

**Set aside.** A task reading each idle connection, as Go and hyper keep, to stop before each use, which the poll
needs not. An earlier rule, a POST taking only a connection used less than a second before, which reviews kept finding
holes in.

**For you.** See [Client](client.md) for what is sent again, and when.

### A client and a server are handles

**Taken.** `http::client` and `http::server` are handles on shared state: their copies are the same object, and they
have no moves, so that one moved from stays usable.

**For you.** Pass them by value to a coroutine; a copy kept by a task keeps it alive.

### One `http::client`, natively and in a browser

**Taken.** In a build for Emscripten, a client made from a URL goes through the browser's `fetch`, which the executor
offers as it offers the network natively. What `fetch` keeps to itself, as its TLS, has no effect there rather than
refusing to run.

**For you.** The same client code compiles and runs on both, with the browser's rules in the browser.

## REST

### A contract made of types

**Taken.** An API is a set of C++ types: `rest::endpoint<"name", verb, "/path", parts...>` per operation, gathered in
`rest::api<>`, its path templates checked at compile time. A server implements a contract with `rest::mount`, and a
client consumes one, but neither needs the other. Each declared status has its typed response.

**For you.** A call with the wrong arguments, or a server missing an endpoint, does not compile, and a contract for a
third-party API needs no Quoll server.

### JSON through Glaze

**Taken.** [Glaze](https://github.com/stephenberry/glaze), pinned and fetched by CPM, for JSON only, behind
`json_codec`, so that it can be replaced: no mechanism of Quoll is to rest on it (#182). The fields of a set of query
parameters or of a form are read by a reflection of Quoll's own, through structured bindings and the names a compiler
gives, which C++26's reflection will replace; the names of enums, the contexts and the widgets' JSON follow.

**For you.** A plain struct is a JSON body, a set of query parameters or a form, as it is, its fields named as they
are declared. A name that is not a C++ name is given only where it is needed: in a struct's `rest_names` for a query
or a form, in Glaze's `glz::meta` for JSON; a struct the reflection does not read, as one with a base class, lists
its fields in `rest_fields`.

## UI

### An interface in C++, compiled to WebAssembly

**Taken.** `quoll::ui`: a C++ DSL (`button("Add") | tw("btn") | on_click(add)`) whose view runs again as its states
change, and whose diff reaches a renderer, the DOM through WebAssembly. Tailwind gives the styles, the DOM gives text,
input methods, selection and accessibility. The same view also writes HTML, with `to_html`.

**Set aside.** A React and TypeScript client beside the C++ application, with a bridge between them: two languages,
and the API described twice.

### A component is a struct of sections or a function of its props, in its header

**The question.** One lambda holding the states, the effects, the handlers and the parts of a view, its props a type
apart and declared twice, read as concepts mixed. How can a component read in sections, without reflection, and stay
cheap to rebuild?

**Taken.** Two forms, both in the component's header ([ADR 0002](adr/0002-ui-dsl-shape.md)). A struct, `struct
product_view : component<product_view>`, its props first, its contexts, states, loads, effects and actions as members
that join it as the app makes it, its handlers as member functions and its view in `element view()`, placed by name
through a variable of the struct's name. Or a lambda whose parameters are its props, with hooks, as before. A load is
one value read with `match`; routes name the fields of their parameters by member pointers. An effect of form S
follows a field of its props, `&props::id`, or a function of them, read from the props of each view: a reference,
`given->id`, would dangle once the next view replaces them. What it follows, as its props, holds no pointer nor view.

**Set aside.** Sections in a nested struct, a life cycle as React's classes, a state and messages as Elm's, signals and
a view run once, and the struct made where it is placed (the ADR's table): each read worse, or made members in the
parent's view. `product_view::with({...})` to place one.

**For you.** A component with a state reads in sections in form S, a small one in form D; both are written in their
header. In form S, declare a state before the effect writing into it.

### Components are functions, their state their hooks

*Superseded on 2026-11-13 by the decision above, which keeps these hooks for its form D.*

**The question.** A component holds states, effects, actions and contexts. Declared as members of a struct, they are
found by walking the struct, which takes reflection, Glaze's, and code generated for each type; written in order in a
view, they read as the screen does.

**Taken.** Functions, as React's components: `constexpr component<props> product_view = [](props const &given) {...}`,
whose `use_state`, `use_effect`, `use_action`, `use_form`, `use_context` and `provide_context` give what the component
holds, found again at each view by the place of the call and its rank there, so that a branch shifts nothing. A
component of a state, an effect and a handler costs a third of the code of a struct, as its effect depends on values
rather than on its type of props, and nothing is reflected. An empty struct whose `operator()` gives the view is one
too.

**Set aside.** Struct components found by Glaze's reflection, which ties the UI to a JSON library; struct components
whose members list themselves as they are made, which needs no reflection but generated as much code as before, and
which form S took up the next day, its effects following values of its props rather than their type (the decision
above); hooks found by the order of their calls, as React's, which a branch would shift with nothing to catch it in
C++.

What a view writes into its hooks, an effect's dependencies and function or what follows an action's success, takes
effect once that view shows, with that view's props, as React's effects are those of the committed render; a run calls
the closure shown as it is asked for. Fixing each closure that outlives the props it captured, one at a time, was set aside for
that rule.

**For you**, in form D, which keeps these hooks: write `auto &` for what a `use_` call gives, capture it by reference,
and keep the calls out of loops whose number of turns changes. A closure captures `given` and what a `use_` call gives by reference, and what the view
computed by value.

### A component is an object of its props, placed by calling it

*Its one rule, and its header and source, superseded on 2026-11-13 by the first decision of this part; its placement
by name kept.*

**The question.** A view places another component: by naming its type, as `component<product_detail>({...})`, or as
React's JSX does, by the component itself? And how is one declared?

**Taken.** An object, called with its props in the view: `product_detail({.item = item})`. The routes name their pages
so, `route<"{id}", product_page, &product_page_props::id>()`, and the links too, `link_to(product_page, {.id = 7},
"Open")`. The app's root is one, with no props. Superseded: one way to write it everywhere, its props always written,
`constexpr component<props>`, and a header and a source for each component.

**Set aside.** `component<product_detail>(...)` in the view, which the maintainer found out of place in a view.

**For you.** A component is written in its header, `inline constexpr`, in form S or in form D; an `extern`
declaration in its header and a source of its own remain possible for one whose rebuild matters.

### A component that goes stays until its tasks end

**The question.** A component removed while one of its handlers waits for a server: what becomes of that handler?

**Taken.** Its tasks, its effects, its actions and its handlers, are asked to stop, and the instance stays until they
have ended, so that none resumes on a component destroyed. A stop ends what they wait on; what a run holds goes as the
stop unwinds it.

**Set aside.** Letting the tasks finish, as React does, which a loop reading a WebSocket would never do. Destroying the
component at once, which C++ cannot do while a coroutine may resume on it.

### A callback runs in the task of the component that wrote it

**The question.** A row calls its page back to delete a note, then the row is removed while the page deletes it. Whose
task is the delete?

**Taken.** The page's: a `ui::callback` made in a view holds the component of that view, and a call starts its function
as a task of that component. The row stops waiting as it goes; the delete goes on. It is implicit: nothing to write
but the callback.

**Set aside.** The caller's task, which would stop the delete with the row. An explicit `spawn` in the parent, which
every callback would need.

**For you.** A callback made outside a view, in a handler or in an effect, knows no component, and runs in its
caller's task.

### A view not run again, its callbacks those of its last view

**Taken.** A component whose props compare equal, with their `==`, and whose states did not change, is not viewed
again. A callback compares equal to any other, as it calls back a component that stays the same.

**Set aside.** Callbacks never equal, which would view again every component given one, the common case. Callbacks
refreshed in a view not run again, one more mechanism to keep right.

**For you.** A component not viewed again keeps the callbacks of its last view: capture only what does not change
from a view to the next, as `this`, and pass the rest as props.

### Effects as React's useEffect, and actions once at a time

**The question.** A screen loads what a choice asks for and saves what is typed: answers that come after a later
choice, saves answered out of order, a double click. Who keeps that right?

**Taken.** Quoll, with React's model: an `effect` is `useEffect` with states of the component as its dependencies.
After a view in which one changed, once shown, it runs, the run before stopped as its cleanup; as a stop resumes
nothing, a stale answer writes nothing. An `action` runs what a handler asks for once at a time by default, a call while
one is under way refused, where TanStack Query's mutations in a `scope` queue it; `overlap::restart` makes a call stop
the one under way instead. Both write what they give into a state of the component, their target, as a React effect
calls its setters: whether a run is under way, why the last failed and its values, by the type of the target's
elements. The view reads states only. An effect runs one way, after a view: `reload()` asks for that view, as a change
does, rather than run it from its caller.

**Set aside.** Effects called in the view, as React's hooks, whose identity is the order of the calls: a member is
named, and depends on the states it is given. An effect that tracks the states its function reads: Quoll tracks no
reads. Queries named and cached, as React Query's: a component need not use the REST DSL, and a function returning a
task is all an effect asks. An effect or an action holding its status and its result, which the view would read beside
the states: two ways for the view to read data. Actions by key: a component of the view per item has its own. A
home-made resource with a key and a loader, which the maintainer found less clear than React's model. Leaving it to each
page, as the trial screen of step 6 first did: three rounds of review kept finding the same bugs in counters kept by
hand.

Less to write, as a load and a save are most of a screen: an `async_state<T>` names its `value`, `loading` and
`error`, an effect's target is loading from the start, as React's `useState(true)`, when its first run will start, and `action<&function>` takes its
signature from the function, a free one, a context's or the component's own, with `with(args...)` for a click and
what follows a success given as it is made, as React Query's `onSuccess`. An action holds where its runs are, as
`useMutation`: `save->loading`, `save->error`, `save->value`, rather than a state declared beside it, which named one
of its three fields. New names for the states, as `loaded<T>`, were set aside: a state stays a `state`. Every failure of a
run reaches the app's `on_error`, shown or not, as React Query's global `onError`, but for `run()`'s, which its caller
is given. A debounce is an option of the effect, `{.debounce = 300ms}`, rather than a debounced value to declare beside
the state, or a pause Quoll would add to any change made by typing, which would hide where it waits.

**For you.** Call `use_async_state`, then `use_effect`, which writes into it; an action is read as one. Change what an
effect depends on, or call `reload()`, and its run follows. Give `save.with(args...)` to `on_click`, or await `run()` from a
task of the component, a handler or an effect: a call refused, or a run a later one took the place of, gives
`run_refused` or `run_replaced`, and its caller goes on. A component that goes stops its runs; a save that must go on
is a member function of a context, which the action runs, whose work goes on.

### A form edits a value its props give

**The question.** A detail edits what its props give, and saves it: what is typed has to be kept while the save is on
its way, checked as it is typed, and forgotten once what was saved comes back. Who keeps that?

**Taken.** Quoll, as React Hook Form's `defaultValues` with `keepDirtyValues`: a `form_state` of a field of the props
takes the value before each view, keeps what is typed in each field as it is, and forgets a field typed in that holds
what the props give again. `bind(form, &item::price)` reads the text by the field's type, `valid()` says whether it
all reads, and `action::with(form)` sends the value as it is at the click, refused while it is not valid.

**Set aside.** A state per field holding what is typed, compared by hand with what was saved, as the trial screen of
step 6 first did: the maintainer found it hard to read. A reset to call after a save, or a callback of the action:
the props bring what was saved anyway. The name `form`, which is the HTML tag.

**For you.** Write `auto &form = use_form(given.item);`, bind the inputs to its fields, and give `save.with(form)` to the
button. Another value to edit from the start is another component, with a `key()`.

### Contexts given to the app, found by type

**The question.** Several components show and change the same data, as a list and the detail of a product. Who holds
it, and who does a save that must go on once the detail is left?

**Taken.** A context, as React's: an aggregate with no view, its states, its effects and member functions, which the
app makes from its options, `provide<T>()`, and holds as long as it lives. A component finds it with
`use_context<T>()`, by type, the nearest a component above provides first. A change of its states shows again the components using it.
`shared<T>::run()` runs a member function of it as a task of the app: a component that goes stops waiting, and the
work goes on, as a callback's does.

**Set aside.** A page doing the save of each of its components and passing callbacks down, which does not hold for a
page of many components. A context made at its first use: what an app holds shows in `main`. Data a component keeps by
reference, as FTXUI's: the lifetime would be the program's to keep, and a stale answer or a cancellation too.

**For you.** Declare the context, give it to the app, `{.contexts = {provide<catalog>()}}`, and call
`use_context<catalog>()` in each component that reads or changes it, or give a struct component a `shared<catalog>`
member. A context only a page needs is provided by its component, `provide_context<T>()`, which gives a `provided<T>`.
Run what must go on once a component goes through `run()`. Stores are planned to replace contexts
([#206](https://github.com/4thlabs/quoll/issues/206)).

### Routes checked at compile time, their parameters a page's props

**The question.** An app has several pages, each at its addresses, some with parameters, as a product's id. How are
they declared, and how does a link to one stay right?

**Taken.** A table of routes, checked at compile time, as Dioxus's and TanStack Router's are typed: a route's path is
a template whose parameters fill fields of its page's props, which give their types, each named by a member pointer
in the order of the path, and the query's fields named, `from_query<"q", &props::q>`: no reflection finds them. A link to a page takes its props, so that a page that changes its parameters breaks the links to it at compile
time. Layouts show the page under them in an outlet, as React Router's. The router is a context, its location a state,
its history the browser's or one in memory.

**Set aside.** Parameters read as strings, as React Router's `useParams`: each page would check its own. Types written
in the template, `{id:int64}`: they would repeat the props', and could differ. Loaders run by the router: an effect on
the page's props does it, as React does with `useEffect`.

**For you.** Give each page props for its parameters, declare the routes, naming the pages' component objects and the
fields their parameters fill, and put `outlet()` where pages show. Use `link_to(page, props, text)` for links, and
`use_context<router>()` to go elsewhere from a handler.

### JavaScript libraries as modules

**The question.** How does a page use chart.js, a map or a terminal?

**Taken.** A widget: a JavaScript module exports `mount(node, props, emit)`, which returns `update(props)` and
`unmount()`; its props go as JSON, what it emits comes back as an event of its node, read into a C++ type. This is the
form of Phoenix LiveView's hooks, with typed props and events. Plain DOM libraries and web components come this way.
Its module is named as an import from the C++ file declaring it, which lets the bundler load it on demand.

**Set aside.** Porting the libraries to C++, which would never end. Libraries tied to React, which cannot come.

## Building

### Compiled libraries

**Taken.** Each module is a library compiled once, its templates over what the application provides staying in headers.

**Set aside.** Header-only, or both modes through `.ipp` files: an application would compile Quoll each time.

**For you.** Build your program in the same mode as its Quoll, `Debug` or `Release`: an `assert` in a source follows
the library's mode. With CPM, this is taken care of.

### C++23, and recent browsers

**Taken.** C++23, with GCC 13, Clang 19 or Apple Clang 21 at least. Under WebAssembly, tail calls and WebAssembly's own
exceptions, which Chrome 112, Firefox 121 and Safari 18.2 have.

**Set aside.** Older browsers, in which a coroutine resuming another grows the stack, and a long loop of tasks that
never suspend overflows it.

### Safety over speed in the API

**Taken.** What outlives a call owns what it needs, even at the cost of a copy, and the fast path goes behind it, in
`detail`. Where a reference cannot be avoided, the name says so (`_ref`, a view).

**For you.** No lifetime is yours to track but those a name tells you of.
