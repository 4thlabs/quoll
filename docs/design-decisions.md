# Design decisions

The choices Quoll made where they matter to a program written with it: the question, what was taken, what was set
aside, and what follows for you. The guides say how things behave; this page says why they behave that way. The
roadmap and the decisions still to make are in [`PLAN.md`](PLAN.md).

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
on, as tokio does; `async::set_report_sink` sends those lines elsewhere. A stop says nothing.

**Set aside.** Ending the program: one connection's failure would take a server down.

### Threads come from the runtime, a strand per connection

**Taken.** `runtime_options::threads` runs the asio runtime on several threads, the tasks of one connection taking
turns on a strand; `async::offload` runs blocking work; `async::run_loop` is a runtime of one thread for tests.

**Set aside.** A thread pool of `quoll::async`'s own, and `serve(executor, listener)` on it, which had no timer for
the timeouts of its connections.

**For you.** A handler needs no lock for what belongs to its connection, and code written for one thread runs on
several.

### Coroutine frames are recycled

**Taken.** Each thread keeps the frames of tasks that ended, to give to the next of the same size.

**For you.** A body read or written in pieces allocates nothing per piece in steady state; the benchmark holds the
allocations per request to a budget: see [Performance](performance.md).

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

**For you.** A middleware does not see the status of a response whose body is written that way, as its head leaves
with the first write.

### A kept connection is checked before it is used again

**The question.** A server may close an idle connection at any time. When may the client send a request on one again?

**Taken.** The idle connection is checked first, as curl does, by a poll of its socket that does not wait: one the
server closed, or that carries bytes nothing asked for, is closed. Every request whose body is whole then takes an idle
connection, as Go's client does; one found closed before any of it was written goes again on a new connection, as
hyper does; a POST of which anything was written is never sent twice. An idle connection older than
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

**Taken.** [Glaze](https://github.com/stephenberry/glaze), pinned and fetched by CPM. Its reflection of aggregates also
finds the members of a contract's types, of a component's states and of a widget's props, with no macro. It is why
Quoll asks for C++23.

**For you.** A plain struct is a JSON body, a set of query parameters, or a widget's props, as it is.

## UI

### An interface in C++, compiled to WebAssembly

**Taken.** `quoll::ui`: a C++ DSL (`button("Add") | tw("btn") | on_click(add)`) whose view runs again as its states
change, and whose diff reaches a renderer, the DOM through WebAssembly. Tailwind gives the styles, the DOM gives text,
input methods, selection and accessibility. The same view also writes HTML, with `to_html`.

**Set aside.** A React and TypeScript client beside the C++ application, with a bridge between them: two languages,
and the API described twice.

### Components are aggregates with states

**Taken.** A component is a struct whose `state<T>` members are what its view depends on, found by reflection, and a
component of a view gets its props as an argument of `view(props const &)`.

**Set aside.** Functions with hooks, which keep their states by the order of their calls. Props as members, which the
reflection finding the states would have to tell apart.

**For you.** A component lives at a stable address, so that its handlers capture `this`; props come from the parent,
states belong to the component, and the type shows which is which.

### A component that goes stays until its tasks end

**The question.** A component removed while one of its handlers waits for a server: what becomes of that handler?

**Taken.** Its tasks are asked to stop, `void unmounted()` runs at once, and the instance stays until its tasks have
ended, so that none resumes on a component destroyed. A stop ends what they wait on.

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

**For you.** A callback made outside a view, in a handler or in `mounted()`, knows no component, and runs in its
caller's task.

### A view not run again, its callbacks those of its last view

**Taken.** A component whose props compare equal, with their `==`, and whose states did not change, is not viewed
again. A callback compares equal to any other, as it calls back a component that stays the same.

**Set aside.** Callbacks never equal, which would view again every component given one, the common case. Callbacks
refreshed in a view not run again, one more mechanism to keep right.

**For you.** A component not viewed again keeps the callbacks of its last view: capture only what does not change
from a view to the next, as `this`, and pass the rest as props.

### JavaScript libraries as modules

**The question.** How does a page use chart.js, a map or a terminal?

**Taken.** A widget: a JavaScript module exports `mount(node, props, emit)`, which returns `update(props)` and
`unmount()`; its props go as JSON, what it emits comes back as an event of its node, read into a C++ type. This is the
form of Phoenix LiveView's hooks, with typed props and events. Plain DOM libraries and web components come this way.

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
