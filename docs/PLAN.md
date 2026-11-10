# Quoll — plan

Last update: 2026-11-08. The [roadmap](#roadmap) says what comes next, and is the reference for the order of the work.

## Target

A C++ web framework aimed at **performance and ease of use**, in three parts that work apart or together:

- **Client and server:** HTTP over any runtime, secure, streaming and fast, with SSE and WebSocket.
- **REST:** an API described once with the DSL, then served or called, from a command-line program as from anywhere
  else, with a runtime (asio, or another).
- **UI:** a C++ DSL compiled to WASM (the Emscripten runtime), which gives one application for web, desktop and mobile.
  Styled with Tailwind, with base components provided and hot reload. Quoll drives the tools, Vite among them, and not
  the reverse.

Build time and hot reload are kept short and measured all along.

## Positioning

The aim is to show that C++ can offer a solid web framework. Who to show it to first:

- **C++ programs that lack a web interface,** in robotics, simulation, finance, video or computing. They often end up with a
  service in Python and a page in React, with their types written twice. What Quoll offers them: *give your C++ program a web
  interface, in the same language and the same binary.* Their existing code, used with no FFI, is also what a Rust
  framework cannot offer them. That holds for programs Quoll can build with: see
  [step 7](#step-7-the-quoll-command-driving-vite-maintainer-merges-the-first-pull-request).
- **New web applications** next, once [authentication](#step-8-authentication-maintainer-merges) and the
  [ORM](#step-9-an-orm-maintainer-merges) exist. A developer happy with TypeScript gains little from changing, and is not who
  this aims at for now.

What shows it, rather than an argument, with no public announcement before step 7 is done:

- A short video: a field of a `struct` that the server and the page both use is renamed and neither compiles, then a Tailwind
  class changes and the page follows, keeping its state. It claims no more than that: reflection adapts to a field no code
  names, which then only fails when strict decoding meets it, at run time.
- A demo people run themselves, in one binary. To settle: which. Proposed: an explorer over DuckDB, which opens a CSV and shows
  results as they arrive. DuckDB has an interface of its own, so that this stays a demo and not a product.
- The framework's site, written with Quoll, served by Quoll behind a reverse proxy, with its source in view.
- A page saying what is guaranteed (the nightly fuzzing, ASan, UBSan and TSan) and what is not (see
  [step 2](#step-2-client-and-server-security-and-completeness-done)), as "C++ exposed to the Internet" is the first objection: the answer
  is a reverse proxy in front, and a server that still defends itself.

The figures to lead with are those of size, build time and memory (see [Done](#done)). Not the rate:
[nginx is ahead of Quoll](BENCHMARKS.md#beside-other-servers), and performance comes last on the roadmap.

## Roadmap

One pull request per topic, under the rule in [`AGENTS.md`](../AGENTS.md). A new public API is shown to the maintainer
before much is built on it. Points marked "To settle" and the [open questions](#open-questions) are the maintainer's to
decide. Steps marked *(maintainer merges)* are big architecture changes.

**Current step: 5.** Step 0 only waits for the maintainer.

### Done

`async`, `io`, `http`, `client`, `server` and `rest`, with timeouts by default and TLS (a client GET and a REST client
over TCP, over `fetch`, and through OpenSSL). A first `quoll::ui` with its DOM renderer, Tailwind through Vite, and two
levels of hot reload, in the sample `ui_notes`. In CI: `clang-tidy`, the standard library's checks, Apple Clang on macOS, Codex's review and a protected `main`.

REST from a command-line program (step 4): a client's scopes and interceptors, form and bytes codecs, `open_enum` for the
values an enum does not know, and `samples/github`, calling GitHub natively and in Node through `fetch`.

Client and server (step 2): the reviews of each module, a runtime on several threads with a strand per connection, a
coroutine frame destroyed only once it will not resume again, which a program reaches no way of breaking through
`co_await` and `co_spawn`, and debug builds assert, and every night nine fuzz targets,
a mixed load under TSan and under ASan with UBSan, and `clang-tidy` with its analyzer, with no crash open.

Streaming (step 3): a body in pieces on the server, the client and browser `fetch`, a minimum rate, static files. 32 GiB each way
through a process that never holds more than 10.3 MiB, checked every night at 1 GiB.

Compiled libraries (step 1): a program using `quoll::runtime` compiles neither llhttp nor asio. A page rebuilds in 0.81 s and a
native server in 3.9 s, for 52 KB of gzipped WASM with the REST client, against about 45 KB for React and ReactDOM alone. A literal shows in about 31 ms,
keeping the state.

### Step 0: finish the setup

What is left is the maintainer's:

- Require the job `macos` on `main`.
- Delete the secret `CLAUDE_CODE_OAUTH_TOKEN`.

*Done when:* both are done.

### Step 1: compiled libraries *(done)*

`src/*.cpp` and one mode, like Capy and Corosio, rather than the two modes of `.ipp` files. What stays in headers is what the
coding rules list. An installed Quoll runs a program with no asio available, checked by hand: nothing in CI does it yet, and it
still links OpenSSL.

Figures, from the first PR of this step to the last, without ccache:

| | Before | After |
|---|---|---|
| Rebuild of a page | 1.15 s | 0.81 s |
| Reload of a page after a rebuild | about 1.2 s | not measured again |
| Gzipped WASM | 51.47 KB | 51.86 KB |
| Rebuild of a native server | 13.7 s | 3.9 s |
| Full build of a native server | 13.7 s | 9.5 s |
| Binary of a native server | 1013 KB | 1206 KB |

Two things follow from compiling once. A program is built in the same mode as its Quoll: an `assert` in a source follows the
library's build type, and inline code holding one, compiled on both sides, is an ODR violation when the modes differ, which
nothing detects. A program using `tcp_stream` or `runtime::context()` compiles asio too, in the same version as the library,
which nothing checks either.

### Step 2: client and server, security and completeness *(done)*

Exposure to the Internet goes through a reverse proxy (decided on 2026-11-07, see [Decided](#decided)), what was built for a
direct exposure stays, and RFC corners that touch neither security nor stability are not guaranteed. `Expect: 100-continue` is
done, with a body too large refused at its head, and so is ending a connection: it stops sending first
(`io::HalfClosableStream`), which a TLS peer reads as its closing message, and after the last response what still comes is
read and dropped (`lingering_timeout`, `lingering_time`). `100 Continue` is sent without asking the router, so that a request to a route that does not exist
still sends its body: to look at with streaming. A client closes its TLS connections without that message.

- Limits and shutdown: graceful shutdown, a connection limit and a wait when out of file descriptors are done
  (`serve_options`), and limits on headers and bodies were there (`http::parse_limits`).
- Reliability, in the manner of Rust. C++ has no borrow checker, but types and tools give a good part of what it
  guarantees:
  - Done: `QUOLL_LIFETIMEBOUND` (`[[clang::lifetimebound]]`), with `-Werror=dangling` on Clang. What a handler or a middleware
    takes by reference lives in a frame of the router until its task ends, so that a reference is no longer a trap. Dropped,
    as measured: `[[clang::coro_return_type, clang::coro_lifetimebound]]` on `async::task`, as it refuses any function
    returning a task that is no coroutine, which `std::function` and `std::invoke` are, and they cannot be annotated.
  - Done: `async::detail::synchronized<T>`, a mutex holding its data, in `connection_pool`, `task_group` and the asio
    `runtime`. It takes a function rather than handing out a guard, and refuses one returning a reference or something to
    await. Clang's `-Wthread-safety` was compared and dropped, as measured: it needs a mutex of our own, annotated, as
    libstdc++'s `std::mutex` is not, it warns on correct code without it, and only Clang checks it.
  - Dropped, as measured: `cppcoreguidelines-avoid-capturing-lambda-coroutines` in `clang-tidy`. Of its 90 findings, 89 are in
    tests, kept alive by their own frames, and the other, `ui::detail::to_handler`, is documented and kept alive by its caller.
- Fuzzing in CI, under ASan and UBSan, is done (`tests/fuzz`, five minutes per target every night, keeping its corpus): the
  HTTP parsers, `serve_connection`, the router, the REST decoding, the client's exchange and the decoding of REST responses.
  Two more, `unfolding_server` and `unfolding_client`, run a connection over a stream the input scripts: each read and
  write comes in a piece of any size, now or later, or fails, and any operation may hang until a timeout or a stop ends
  it. The exchange must end.
  They reach the timeouts and the cancellation, which the others' stream, never suspending, does not.
- What a handler answers is replaced when its peer would read it as something else, through a connection as through
  `local_transport`: a 2xx to CONNECT, a tunnel, which is not offered, answers 501; a 1xx, and a `Content-Length` with no body, but
  to HEAD or as a 304, answer 500.
- The first nightly fuzz run, on 2026-11-06, found nothing: five minutes on each of the seven targets of then, from 225,000 to 12 million runs.
- Features: middlewares (`router::use`) and routes whose handler takes the path's parameters, checked at compile time
  (`router::get`...), a path prefix in the base URL, a connection limit per transport and client redirects, are done. A client's `response_timeout` applies per attempt, so a deadline over the whole `send` if that proves
  confusing.

What stays out of reach: the lifetimes of coroutines remain mostly documented rules ("what a coroutine takes by reference outlives it", when
the task outlives the line it was made on), a use after move across functions or through a member goes unseen, and a
lambda can still capture by reference what dies before it.

*Done when:* the fuzz targets run in CI with no open crash, the limits, the shutdown and the features are tested, and the
reliability points are in place or dropped with a reason.

Done on 2026-11-08, with what the reviews of each module found fixed (#97 to #108, but #102, which records the reverse
proxy), and since: `quoll::runtime` on several
threads with a strand per connection (#110); a coroutine frame destroyed only once it will not resume again, asserted by
the task's promise, and a task a program cannot start itself nor await by name (#112, #116); the thread pool gone (#115);
every night, a mixed load under TSan and under ASan with UBSan (#113), and the fuzz of how a connection unfolds (#114), with
`clang-tidy` and its analyzer, green again with #117. Run on main by hand on 2026-11-08, with no crash: the fuzz after
#116, the load and `clang-tidy` after #117.

### Step 3: streaming *(done)*

- Done: a benchmark harness (requests per second, latency, allocations per request) and its baseline, in
  [`BENCHMARKS.md`](BENCHMARKS.md), as streaming touches the hot paths.
- Streamed request and response bodies, on the server, the client and browser `fetch`. A handler takes an `http::body_reader`
  to read its request in pieces, a message's `body_writer` writes in pieces through an `http::body_stream`, and a client's
  `client.stream(request)` reads a response in pieces. Done: `http::parser` hands a body over in pieces, with `http::read_body`, and `http::body_reader`: a handler taking one asks for `100 Continue` by reading, and goes by the read timeout and its minimum rate, below. And `response.body_writer`: chunked by default, by the write timeout and its minimum rate. And `client.stream(request)`, with `http::streamed_response`: a read timeout per piece, and a connection back at the end of the body. A client sends a request in pieces through its `body_writer`, on a connection of its own, as a writer runs once, with the response read meanwhile so that a server refusing a body is heard; `fetch` and `local_transport` gather what it writes first. And `client.stream(request)` through `fetch`, with the same `http::streamed_response`: one piece per JavaScript read of `response.body`, so that nothing holds the body whole.
- Done: `http::static_files`, a middleware that answers what the router answers 404 to, in pieces, with a cache and ranges. It
  reads through `async::offload`, a service of an executor that runs blocking work on threads of its own, which only an adapter
  has: the core still needs none. It costs about 4% of the rate of a file already in the page cache, and a slow disk no longer
  holds up the loop. `io_uring` may take over behind the same service if a measure asks for it.
- Done: a minimum rate on the server, `min_read_rate` and `min_write_rate`, 16 KiB/s by default: a body's bytes, or what leaves, add to
  what `read_timeout` and `write_timeout` have left, up to themselves, so that a slow link has the time it needs, and a peer
  sending a little now and then runs out of it. Static files go by the same.
- Done, by step 2: a half-close, `io::HalfClosableStream` and its `shutdown_send()`.

*Done when:* a transfer far larger than memory runs in bounded memory, on both sides, with the figure measured. Measured: 32 GiB
each way, a request, a response and a file, with a process that never held more than 10.3 MiB, in
[`BENCHMARKS.md`](BENCHMARKS.md#larger-than-memory).

### Step 4: REST from a command-line program *(done)*

The DSL and a runtime, with no UI.

- On the client: `scope<Api>(prefix arguments...)`, and interceptors around `send` (retry, token refresh on 401).
- Third-party APIs, as [the REST DSL](#rest-dsl-contract-server-client) describes: renamed fields, unknown fields, error
  bodies of any shape, codecs. A contract alone, with no Quoll server in the program.
- A sample and a guide for a command-line program: `samples/github`, GitHub's API as the real one, as decided on
  2026-11-07. A client of Docker over a Unix socket comes later, in no step yet.
- After this, and optional: an OpenAPI document generated from the DSL, to document an API or for third-party clients,
  and a generator of contracts from existing documents.

*Done when:* a real third-party API is called from a command-line program, natively and through `fetch`, with
`call`, `try_call` and credentials.

Done on 2026-11-08: a client's scopes and interceptors, with `rest::retry` (#119), the form and bytes codecs (#120),
`samples/github` calling GitHub natively and in Node through `fetch`, tested against fakes, with
[`docs/command-line.md`](command-line.md) (#121), and `rest::open_enum<E>` for the values an enum does not know. The OpenAPI
document stays optional, for later.

### Step 5: SSE, then WebSocket

- SSE first, as it rests on step 3: a DSL route kind returning typed events, a client, the browser. The browser reads a
  streamed `fetch` rather than `EventSource`, which takes no headers: an API with credentials could not use it.
- WebSocket: the upgrade, frames, a client, the browser adapter, its shape in the DSL.

SSE is done on 2026-11-09: the parser and its formatting, fuzzed (#123), `http::event_writer` and `http::event_reader`
over the transports (#124), and `rest::events<T>`, its `event_sink` and the client's `subscribe`, natively and through
`fetch`, run in Node by CI as the browser's stand-in, as for step 3.

*Done when:* each runs end to end, natively and in the browser, with its frame parsing fuzzed.

### Step 6: the UI DSL, complete

- Nested stateful components: a component is the root of an app for now.
- Restoring the states after a rebuild, which the third level of hot reload still lacks.
- How a page writes its names: `using namespace quoll::ui;` now sits in each `view()`, as the rules forbid it at
  namespace scope in a header.
- Base components, styled with Tailwind.
- In the sample: adding a note during the first load can hide it until a reload.
- Two siblings with the same key: only caught by an `assert`, on the render after, which a real error should replace. A state
  changed on an app never started leaves a render pending, which nothing waits for.
- Hydration, then pages sent by a server: see [UI DSL](#ui-dsl). `to_html` is there; a `value` on a `select` selects nothing yet.
- Using the JavaScript libraries a page needs (charts, maps, editors, a terminal), a requirement whatever form the UI
  takes, said on 2026-11-07: a node the reconciler leaves alone, a lifecycle with an unmount, calls both ways, bytes passed
  without a copy where it counts, npm packages through the Vite build, loaded on demand. Plain DOM libraries and web
  components come this way; those tied to React cannot.
- A trial on a real screen of an application, chosen with the maintainer at the start of this step.

*Done when:* that screen runs with the base components and a JavaScript library loaded on demand, called from C++ and
calling back, mounted and unmounted, with what it lacks listed.

### Step 7: the `quoll` command, driving Vite *(maintainer merges the first pull request)*

**To settle at its start:** a native binary, or an npm package (`npx quoll`), as Node is needed for Vite anyway and it
avoids a binary per platform. The commands are shown to the maintainer first.

```bash
quoll new my_app
cd my_app
quoll dev      # build, watch, Vite, hot reload
quoll build    # dist/, with the pages that can be written ahead of time as HTML, and a PWA
```

- The plugin leaves the sample, with its scanner tested in CI. Vite and Tailwind's setup become internal, and an
  application writes neither.
- Its limits as a prototype: a failed first build stops the server, a Vite restart during a build can run two builds in
  one directory, an error with no page open can show once on the next page, and a multi-line macro holding a tag is not
  told apart.
- Build time and hot reload, measured, with a target set at the start of this step from the figures of step 1.
- A PWA from `quoll build`: a manifest and a service worker, so that an application installs on desktop and mobile and opens
  offline. Nothing of it exists yet but the type `http::static_files` gives a `webmanifest`. Installed PWAs are restricted on
  iOS (storage eviction, background work): to check.
  - How it updates: an installed page may run older WASM than its server, and every codec's decoding is strict
    (`error_on_missing_keys`), so that a field added to a shared `struct` answers its POST with 400 until the worker updates.
    What such a page does is to settle with the maintainer.
- A web interface for an existing program, as [Positioning](#positioning) asks:
  - Its files embedded in the binary: `http::static_files` serves a directory.
  - Quoll beside a program's own main loop, on a thread of its own, needs an API first, shown to the maintainer. `run()` returns
    once no executor is left, so at once if none was taken yet; an executor kept to reach the loop later keeps it running for
    ever, so that joining that thread hangs; nothing stops a runtime but its tasks ending; and `get_stop_token()` takes SIGINT
    and SIGTERM, which such a program handles itself. How the program's data reaches the loop's tasks goes with it.
  - What an existing program can build with, tried on a real one and not only on a new sample: C++23 with GCC ≥ 13, Clang ≥ 19 or
    Apple Clang ≥ 21 leaves out MSVC and older distributions (see [open questions](#open-questions)); another version of
    standalone asio in the same binary is an ODR clash, as asio has no versioned namespace; and the program's build mode must
    be its Quoll's. What cannot be fixed is said in the guide.
- Ready for LLMs, as an agent is now how many people will first write Quoll:
  - `quoll new` writes an `AGENTS.md` and a skill into the project, for the version of Quoll it uses, with the commands, the
    rules a program must follow, and where the guides are.
  - The repository publishes an `llms.txt` over its guides, and each guide stays readable alone.
  - What the guides show compiles in CI, as an agent copies it as it stands: their C++ blocks are checked by hand for now.
  - The errors of the `quoll` command, and the `static_assert` messages of the DSLs, say what to change, as that is what an
    agent acts on. They stay tested by `compile_fail`.

*Done when:* a new project reaches a running page with hot reload through these commands, and `samples/ui_notes` uses
them; that project installs as a PWA, opens offline and takes an update; an agent given only what `quoll new` wrote adds a screen
and an endpoint to it; and an existing program with its own main loop serves its interface from one binary, stops cleanly with
it, and has its test and its guide.

### Step 8: authentication *(maintainer merges)*

Before the ORM, as the first programs [Positioning](#positioning) aims at often have no SQL database, and need to sign in as
soon as they listen beyond their machine. It first settles what a reverse proxy forwards, under
[Open questions](#open-questions), for its redirects and cookies. Sessions go through a store of their own (memory, a file), which the ORM implements
once it exists. A new module, as it needs cryptography that `quoll::rest` and `quoll::server` do not have and must not bring to
WASM builds.

- Signing in through an OIDC provider and sessions, a shared secret in a program's configuration for a machine on a
  closed network, and a local account with a password, decided on 2026-11-07 against the "no store of passwords" proposed
  before: its hashing is part of the cryptography to settle below.
- Cryptography: nothing in Quoll hashes, signs or draws secure random numbers for security. OIDC needs ID tokens checked
  against the provider's keys, a nonce and PKCE, and session ids from a secure source. **To settle at its start:** OpenSSL,
  as TLS uses, or something else, and what a build with `QUOLL_TLS` off does. None of it written by hand. WebSocket's
  handshake is no cryptography, decided on 2026-11-09: its `Sec-WebSocket-Accept` is an SHA-1 RFC 6455 uses as an interop
  check, written by hand in `quoll::http`, and a client's masks, which keep proxies from reading frames as requests, come
  from `std::random_device`; so that WebSocket runs with or without TLS.
- What the REST DSL lacks: validators answer `bool`, and a service takes only the endpoint's arguments, so that a handler of
  `GET /me` cannot learn who calls; and a scheme cannot be a cookie, as `rest::security` accepts only `api_key<>`,
  `api_key_query<>` and `bearer_auth`. A validator's result reaching the service, and a cookie scheme, shown to the maintainer.
- A page: an OIDC redirect is a navigation, which `quoll::ui` and the Emscripten adapter do not offer, and which loses the WASM
  state. In a browser, `http::client` sets no `credentials`, so that a page calling another origin neither sends nor keeps a cookie.
- A native client: `http::client` has no cookie jar, and drops `Set-Cookie` across redirects. To settle whether it needs one.
- Cookies and what they ask: expiry, revocation, CSRF.
- What it parses from outside, tokens among them, is fuzzed.

*Done when:* a sample signs in through a real provider, from a page too, and with a local account and its password, its sessions survive a restart through a file, and
expiry and revocation are tested, with its guide.

### Step 9: an ORM *(maintainer merges)*

A new module. **To settle at its start:** the shape of a query, shown to the maintainer first, the first database, and
migrations.

- A table is described by a plain aggregate, through the reflection the REST DSL already uses, so that one `struct` may go from
  the database to the browser, and a field the program names breaks the compilation all along. The compiler does not see the
  database: a `struct` changed with no migration has to be caught when the program starts, not on its first query.
- A column kept from the browser, a session's hash or a provider's token: an endpoint's `returns<user>` writes every reflected
  field, and hiding one through `glz::meta` hides it from the ORM too. Excluding a field per output, or a type of its own for what
  is sent: to settle, as the easy path must not leak secrets.
- Native only, as `quoll::asio` is: the core still compiles to WASM with none of it.
- A query never blocks the loop, and can be given up. A driver over `quoll::io` streams does both. A C client cannot go through
  `async::offload` as it is: two threads shared with `http::static_files` starve under slow queries, and a call started cannot be
  stopped, so that a timeout waits for it, and so does a shutdown. It needs threads per database and a way to cancel.
- Against a real database in CI.
- Sessions of step 8 stored through it.

*Done when:* a sample stores its data through it, from a contract shared with its page, with its guide.

### Step 10: desktop and mobile *(maintainer merges)*

Secondary to the maintainer, as the PWA of step 7 already installs there.

- A webview shell on desktop, then on mobile, loading the same WASM.
- Native capabilities (files, sockets) through a small gateway, designed at this step. The webview library is to be
  chosen.
- Webview engines differ: WebKit on iOS, macOS and Linux, Chromium on Windows and Android. To check on each.

*Done when:* the sample runs on each target, with the differences between engines listed.

### Step 11: performance

Last, as what comes before it is needed first. It starts from a new baseline, as those steps change what is measured, and
changes only what the harness shows. Candidates:

- Each bounded phase takes a coroutine frame, recycled by each thread since `async::detail::recycled_frame`, and an asio timer,
  about three times per request on the server. A timer per connection avoids it. On several threads, each wait and each
  connect also makes a strand, and a socket's completion goes through its own strand, then its connection's: the timer and
  the socket on the connection's strand would avoid both.
- A client copies the request body into the serialized message: write the head and the body as two buffers.
- A TLS client builds its context per connection, and resumes no session.
- A client builds a parser per request, which now allocates: one per connection avoids it.
- A body in pieces still takes about 4 allocations per piece behind `http::body_reader` and `http::body_stream`, from what its
  timeouts do, and those wrappers return a task rather than an awaitable in storage of their own, as the type-erasure rules
  ask. With frames recycled that task allocates nothing in steady state, and changing it changes no handler.
- The size of a native binary, 19 % larger since step 1, as static libraries link whole objects: `--gc-sections`, link-time
  optimization.
- Idle connections are found closed only as a request takes one, with a poll (decided below), and one idle for longer than
  `max_idle_time` is not used again: closing them as that time passes, with no request to find them.

*Done when:* figures are published against that baseline, and each change is justified by a measure.

## Decided

- **An idle connection is checked before it is used again, as curl does** (2026-11-10): a poll of its socket that does not
  wait (`io::PollableStream`) finds one the server closed, or that carries bytes nothing asked for, which is closed. A
  POST then takes an idle connection as Go's client does, if its stream can be checked; one found closed with none of it
  written goes again on a new one, as hyper does, and a POST written is never sent twice, even answered 408. Go and hyper
  keep a reader waiting on each idle connection instead: a task per connection, to stop before each use, which the poll
  needs not. Chosen by the maintainer, as the solution the reference libraries settled on, over a first rule, a POST taking only a connection
  used less than a second before, which reviews kept finding holes in: a server closing within the second, a response
  read slowly. One idle for longer than `client_options::max_idle_time`, 90 s as Go's `IdleConnTimeout`, is not
  used again, as no check sees a connection something on the way dropped with no word.
- **Using the JavaScript libraries a page needs is a requirement of the UI** (2026-11-07), whatever form it takes: see step
  6.
- **`samples/github` is step 4's real API** (2026-11-07), and a client of Docker over a Unix socket comes later, in no step
  yet.
- **A local account with a password** (2026-11-07) joins OIDC and a shared secret in step 8, against the "no store of
  passwords" proposed before.
- **The thread pool and `serve(executor, listener)` go** (decided on 2026-11-07, done on 2026-11-08). Several threads come
  from the asio runtime (`runtime_options::threads`, a strand per connection), `async::run_loop` is the runtime of one thread
  without asio, for tests and as an example, and blocking work goes through `async::offload`. With them goes the question of
  timeouts on connections served on a pool, which had no timer. What its tests checked of `quoll::async` across threads runs
  on asio's own thread pool.
- **A coroutine frame is destroyed only once it will not resume again** (2026-11-08): by a root as it ends, or by its awaiter
  once the task has completed, which a task's promise asserts in debug builds, whatever destroys the frame. A program never
  starts a task itself: its `await_suspend` is private to Quoll's own awaiting. What an awaitable registers from its frame is undone as
  it resumes or is cancelled, never because the frame went, and a work item stays a coroutine to resume. Chosen over giving a
  work item a function to run, so that an awaitable could be destroyed while queued (PR #111): that case was reached only by
  a test destroying a suspended awaitable by hand.
- **Exposure to the Internet goes through a reverse proxy** (2026-11-07), replacing "Quoll may be exposed directly" of
  2026-11-05, as each review of the HTTP code kept finding more corners of the protocol. The server still defends itself: what
  a review finds is fixed, as `AGENTS.md` says, but for an RFC corner that touches neither security nor stability, as a less
  exact status, which the PR notes and lets go. Security and stability take in a crash, undefined behaviour, a data race,
  request smuggling, header injection, credentials going to another host, leaving a directory, a limit guarding memory and
  time, and a connection or a request stuck for ever, among others. What a proxy in front asks of Quoll is under
  [Open questions](#open-questions).
- Who it is for first (2026-11-06): C++ programs that lack a web interface, then new web applications, as
  [Positioning](#positioning) says.
- Authentication, then an ORM (2026-11-06), come after the `quoll` command and before desktop and mobile, which are secondary:
  a web application needs them sooner than a native shell does. Authentication depends on no ORM, as the first programs aimed
  at often have no database.
- The PWA (2026-11-06) comes from `quoll build`, in step 7, rather than with desktop and mobile: it costs little and already
  installs on both.
- Ready for LLMs (2026-11-06): a project and the guides carry what an agent needs to write a Quoll application. What it asks
  is in step 7.
- Names (2026-11-06): `http::client` and `http::server`, classes without a template parameter, rather than `client::transport<>`
  and the free functions `server::listen` and `server::serve`. A user thinks of a client and a server, not of a transport, and
  `HttpTransport` stays the concept. `quoll::client` and `quoll::server` stay libraries of their own, so that a program links only
  what it uses, with their names in `quoll::http`. Guides name at most two levels, through aliases such as
  `namespace async = quoll::async;`.
- Performance does not make the user API dangerous (2026-11-06): what outlives a call owns what it needs, and the fast path goes
  behind it, in `detail`. `http::local_transport` keeps its own copy of the router, and `http::server` owns its listeners.
- `http::response_writer` (2026-11-06): a handler writes its body itself, as `res.write` does in Express, rather than returning a lambda
  that runs once it has ended, where what it captures by reference dangles. It runs as the response's writer, awaited by what serves
  the connection, with no task of its own, its head leaving with its first write. So a middleware does not see its status: running
  it up to its first write before the router answers would show it, and was tried, but the handler then outlives what it uses. A
  `co_yield` can come later on top of it, the reverse being harder. `response.body_writer` stays, for a response built ahead of time.
- `quoll::runtime` under Emscripten too (2026-11-06), so that `main()` reads the same: it is the browser's event loop, and `run()`
  returns at once, as the loop goes on once `main()` has returned. What differs from a native `run()` is said where it is.
- The same code on every platform (2026-11-06): `http::client` is the one name, and `fetch_client` went. In a build for Emscripten, a client from a
  URL asks the executor for an `http::platform_ref`, as it asks for a network, so that `quoll::client` does not depend on an adapter. What
  `fetch` keeps to itself has no effect there rather than refusing to run, as the same code has to run.
- `http::server` is a real object (2026-11-06), like `http::client`: a handle on shared state, which its copies and tasks share, as
  Corosio's objects are. Half an object, a router and options with each call on its own, left `max_connections` per call and
  nowhere to count connections. It holds routes of its own, for the simple form, or starts from a copy of a router, with the same
  options either way, and refuses a route once it serves. `max_connections` and `connections()` count for the whole server. A port
  is read on the listener before it is given, as nothing else can be right at any moment.
- `async::detached` (2026-11-06) ignores `std::errc::operation_canceled`, so that a stop ends a task with no word. It does not see
  the task's stop token, so it ignores that error whatever caused it: a choice made knowing that, over a check of the token in
  `co_spawn`. Since 2026-11-10, every other exception is written to stderr and the program goes on, as tokio does, rather than
  end it: the maintainer's call, as one task's failure must not take a server down. The server's callbacks for a handler that
  throws, a connection that fails and accepts tried again do the same when the application gives none.
- Documentation (2026-11-06) is Markdown in the repository for now: a README and one guide per subject directly under `docs/`, with
  no site generator. A step of the roadmap is done only once its guide in `docs/` says what it brought.
- Under WASM, `-mtail-call` (2026-11-05), with `-fwasm-exceptions`: a coroutine resuming another otherwise grows the stack, and
  a long loop of tasks that never suspend overflows it. It asks for Chrome 112, Firefox 121 or Safari 18.2.
- Performance comes last (2026-11-05): the maintainer needs what comes before it first. An earlier step may still take a
  candidate that it reworks anyway, as streaming does with the timers.
- Coroutine frames are recycled per thread (2026-11-05), ahead of the performance step, at the maintainer's request: a body in
  pieces took 16 allocations per piece, against the type-erasure rule, and what fixes it is this rather than the wrappers.
- Name: **Quoll**, a new public library (namespace `quoll::`). Puffin keeps its existing utility modules (events, ioc,
  maths, imdb...). Repository: GitHub `4thlabs/quoll`.
- **C++23** minimum (GCC ≥ 13, Clang ≥ 19, Apple Clang ≥ 21 since 2026-11-04, the maintainer's;
  C++20 until 2026-11-03, raised for Glaze), rules in [`CODING_RULES.md`](CODING_RULES.md). Third-party C/C++
  dependencies may be compiled targets fetched by CPM.
- The UI is a **C++ DSL compiled to WASM** (2026-11-04), which replaces the React, TypeScript client and `quoll::bridge`
  of the earlier plan: elements with pipes and class strings (`button("Add") | tw("btn") | on_click(...)`), components
  as aggregates holding `state<T>` members rather than functions with hooks, a view that runs again and a diff that
  reaches a `Renderer`.
- **The same WASM everywhere** (2026-11-04): web and PWA first, then a webview shell on desktop and mobile.
- **Quoll drives the tools** (2026-11-04), through a `quoll` command: Vite becomes an internal detail. Replaces a
  development driven by Vite and a release driven by CMake.
- **The UI DSL has several outputs** (2026-11-05), as it only knows a `Renderer`: the DOM through WASM, and HTML, for pages
  generated ahead of time or sent by a server, which also settles the search engines. `to_html` came ahead of step 6, for
  the framework's site to be written with it.
- **Compiled libraries** (2026-11-04), in one mode rather than the two of `.ipp` files, for applications that stay
  reliable over time: see [step 1](#step-1-compiled-libraries-done).
- OpenAPI (2026-11-04) is an optional, later output of the REST DSL, no longer the contract of the UI.
- JSON: [Glaze](https://github.com/stephenberry/glaze) (2026-11-03), header-only, fetched by CPM with a pinned version.
  Its aggregate reflection also serves the REST DSL (see [REST DSL](#rest-dsl-contract-server-client)).
- REST DSL (2026-11-03, see [REST DSL](#rest-dsl-contract-server-client)): one `endpoint<"name", method, "/path", parts...>`
  alias per operation, nested `api<>`, `with<>`, overload set services, `call` / `try_call` / `scope` clients, one typed
  response per status, `query_params<Struct>`, Glaze's aggregate reflection instead of macros, a client over
  `http::HttpTransport`, routes in the sans-IO router. A contract stands alone: clients of third-party APIs need no Quoll
  server.
- Prior art: the `claude/project-thread-*` branches of the Puffin repo (earlier `async`, `webkit`, `rest`) were removed
  from Puffin's `master` for quality reasons. Ideas only: never port code from them, rewrite to the rules.
- Type erasure: non-intrusive function-pointer tables, `xxx_ref` + `any_xxx` (see the rules).
- The HTTP client is in scope. Burl (CppAlliance) is not an option: it is tied to Corosio for sockets / TLS.
- HTTP layer: **own stack on llhttp** (option B of [HTTP layer](#http-layer)), decided on 2026-11-02. llhttp is fetched
  by CPM and compiled; `quoll::async` follows Capy's design without depending on it.
- WASM is a first-class target: the core never requires threads or blocking waits.

## Architecture

### Layers

```mermaid
flowchart TB
  subgraph app["Application"]
    page["UI in C++<br/>(quoll::ui, WASM)"]
    handlers["API handlers<br/>(business logic)"]
    cli["Command-line programs"]
  end

  subgraph quoll["Quoll (sans-IO)"]
    ui["quoll::ui<br/>elements, state, diff, Renderer"]
    rest["quoll::rest<br/>typed API DSL, codecs"]
    server["quoll::server<br/>HTTP/1.1 connections"]
    client["quoll::client<br/>keep-alive, timeouts"]
    http["quoll::http<br/>messages, parser, serializer, router"]
    io["quoll::io<br/>byte streams, any_stream, memory stream"]
    async["quoll::async<br/>task, executor_ref, await_context, when_all, co_spawn"]
  end

  subgraph concepts["Concepts (type-erased)"]
    executor(["executor"])
    stream(["byte_stream"])
    connector(["connector"])
    transport(["http_transport"])
    renderer(["Renderer"])
  end

  subgraph adapters["Adapters"]
    asio["quoll::asio<br/>executor, tcp_stream, connector, TLS"]
    emscripten["quoll::emscripten<br/>event loop executor, fetch, dom_renderer"]
  end

  page --> ui
  page --> rest
  handlers --> rest
  cli --> rest
  rest --> http
  rest --> client
  server --> http
  client --> http
  ui --> async
  http --> io
  io --> async

  io -.-> stream
  client -.-> connector
  client -.-> transport
  ui -.-> renderer
  async -.-> executor

  executor -.-> asio & emscripten
  stream -.-> asio
  connector -.-> asio
  transport -.-> emscripten
  renderer -.-> emscripten
```

| Concept | Meaning | Implemented by |
|---|---|---|
| `executor` | where / when a coroutine resumes; `post(work_item &)` links an intrusive node, no allocation | asio, browser event loop, `run_loop` |
| `byte_stream` | `read_some` / `write_some` on raw bytes (`any_stream`, no allocation per operation) | asio sockets, memory |
| `connector` | `connect(host, port) -> task<any_stream>`, and `connect_tls` beside it | asio (+ssl) |
| `http_transport` | `request -> task<response>`, for platforms that do HTTP themselves | `quoll::client` over a connector, browser `fetch` |
| `Renderer` | what the diff of a view changes: nodes, classes, attributes, listeners | the DOM, memory in tests |

The router lives in `quoll::http` and does no I/O: it maps a method and a path template to a handler `request ->
task<response>`. `quoll::rest` registers the DSL routes in it, and `quoll::server` feeds it the requests read from
connections.

Design references taken from Boost.Capy: `await_context` propagated through `await_suspend(h, env)`, `executor_ref`,
zero-allocation `any_stream`.

Program entry point (2026-11-03): application code names no adapter. `main()` creates the build's runtime
(`quoll::runtime`, the adapter chosen with `QUOLL_RUNTIME`, asio natively), spawns the program's main coroutine on it
(`return runtime.run(task);`, since 2026-11-10, which spawns it with `runtime.get_stop_token()`, the token stopped by
Ctrl-C) and runs it; servers, clients and their connections live in that coroutine. Executors carry the services of
their loop (`executor.query<io::network_ref>()`), so `io::connect` / `io::listen`, `http::server{router}.listen(port)` and
`rest::client<api>{"http://host:port"}` need no executor, connector or acceptor handed to them.

Time (2026-11-04): a timer is one more service of the executor (`executor.query<async::timer_ref>()`), a `steady_timer`
natively and `setTimeout` in the browser. Applications call `async::sleep_for(duration)` and
`async::with_timeout(duration, task)`. A timeout stops the task and awaits its end: nothing it refers to is left behind,
so a task only ends early if it observes its stop token. Its `operation_canceled` then becomes `timed_out`; a task that
finishes anyway still gives its result. The server and the client have timeouts by default (server: idle 60 s, read and
write 30 s; client: connect 30 s, response 60 s), which zero disables. They do not apply on an executor without a timer.

TLS (2026-11-04): one more service of the executor (`io::tls_network_ref`), through `asio::ssl` and OpenSSL. `https://`
checks the server against the system's authorities and its name by default; `.tls = {.ca_file = ...}` trusts a private
authority, `.tls = {.verify_peer = false}` turns the check off. `listen` takes `.tls = {.certificate_file = ...,
.private_key_file = ...}`. OpenSSL is detected when configuring (`QUOLL_TLS`: `AUTO`, `ON`, `OFF`): without it, what
needs TLS throws at run time, saying so. A server's handshake takes place on the connection's first read, so that a slow
peer holds nobody back and the idle timeout covers it. In a browser, `fetch` does it all.

### REST DSL: contract, server, client

What this section says of OpenAPI (operation ids, security schemes, JSON Schemas) describes the optional output of
[step 4](#step-4-rest-from-a-command-line-program-done), which does not exist yet: it binds nothing else.

An API description is a **contract**: C++ types, with no I/O and no server or client code. A server implements a
contract and a client consumes one, but neither needs the other. Describing a third-party API (a payment provider, a
public service, another team's service) means writing only its contract, then calling it with a client: no Quoll
server is involved. The shape comes from the `rest` module of Puffin's `claude/project-thread-*` branches (ideas
only, see [Decided](#decided)), adjusted for OpenAPI, typed errors and third-party APIs.

**Describing an API.** One type alias per operation:

```cpp
namespace rest = quoll::rest;

namespace bank {

struct user {
  std::int64_t id = 0;
  std::string name;
  std::optional<std::string> email;
};

struct new_user {
  std::string name;
  std::string email;
};

struct user_filter {
  std::optional<int> limit;
  std::optional<std::string> name;
};

namespace users {
using namespace quoll::rest;
using quoll::http::verb;

using list = endpoint<"users.list", verb::get, "/", query_params<user_filter>, returns<std::vector<user>>>;
using get = endpoint<"users.get", verb::get, "/{id:int64}", returns<user>, returns<problem, status::not_found>>;
using create = endpoint<"users.create", verb::post, "/", body<new_user>, returns<user, status::created>,
                        returns<problem, status::conflict>>;
using remove = endpoint<"users.remove", verb::delete_, "/{id:int64}">; // 204

using api = rest::api<"/users", list, get, create, remove>;
} // namespace users

using v1 = rest::api<"/api/v1", auth::api, rest::with<rest::security<rest::bearer_auth>, users::api, cards::api>>;

} // namespace bank
```

- `endpoint<"name", method, "/path", parts...>`: the name is mandatory and unique in its API, which is checked at
  compile time. It is the OpenAPI `operationId` and the name of the route on the server. The method is an
  `http::verb` value, not a bare `GET` / `DELETE`: those are macros on some platforms (`DELETE` in `<winnt.h>`), and
  the rules allow no macro of our own.
- Path parameters are typed in the template, `{name:type}` (`int`, `int64`, `uint`, `uint64`, `double`, `bool`,
  `string` by default, `path` for the rest of the path), and parsed at compile time.
- Parts:
  - `query<"name", T>`: `T` is the argument type. `query<"limit", std::optional<int>>` is optional (not required in
    the document, omitted from the URL when empty), `query<"tag", std::vector<std::string>>` repeats the parameter
    (`?tag=a&tag=b`), anything else is required. `header<>` follows the same rule, as do the fields of
    `query_params<Struct>`.
  - `query_params<Struct>`: each field of the struct is a query parameter. This is for operations with many optional
    parameters, common in third-party APIs; the call then uses designated initializers. A name of its own: a class
    template cannot also take the `<"name", T>` form of `query`.
  - `header<"Name", T>` and `body<T, Codec>`.
  - `returns<T, status, Codec>`: repeatable, one per status, each with its own type (200 by default). An endpoint
    without `returns` answers 204.
- `api<"/prefix", ...>` nests APIs, and a prefix may have parameters. `with<parts..., apis...>` adds parts to every
  endpoint of the APIs it wraps.
- Security schemes: `api_key<"Header">`, `api_key_query<"param">`, `bearer_auth`, mapped to OpenAPI
  `securitySchemes`. They are not arguments: the server checks them with validators, and the client sends the
  credentials it was given for each scheme.
- `param_traits<T>` converts path, query and header values to and from text; enums go by name.

Arguments are the same on both sides: the path parameters (prefixes first), then the parts in order, then the parts
inherited from `with<>`. The endpoint computes their types: `call` and the service take exactly those, not deduced
`Args&&...`, so that a braced list (`{.limit = 20}` for a `query_params<Struct>`) converts to its parameter.

**Serving it.** A service is an overload set, one `operator()(Endpoint, args...)` per endpoint, with no base class.
A missing service, or one with the wrong arguments, is a compile error.

```cpp
namespace bank {

struct users_service {
  task<std::vector<user>> operator()(users::list, user_filter filter);
  task<rest::reply<users::get>> operator()(users::get, std::int64_t id);
  task<user> operator()(users::create, new_user created);
  task<> operator()(users::remove, std::int64_t id);
};

} // namespace bank

http::router router;

rest::mount<bank::v1>(router, bank::auth_service{}, bank::users_service{}, bank::cards_service{},
                      rest::validators([&](rest::credential<rest::bearer_auth> const &token) { return sessions.valid(token.value); }));
```

- A service returns the type of the first `returns`, or `rest::reply<E>`, which holds any declared response with its
  status: built from a value whose type only one response has, or from `rest::respond<Status>(value)` when several share
  it. An undeclared status, or the wrong type for a status, does not compile. Only the endpoint's own responses: those
  inherited from `with<>` are the server's (credentials, errors).
- Errors without a declared response:
  - 400 for an invalid argument or body, 415 for a wrong `Content-Type`, 401 for missing or rejected credentials.
  - `throw rest::http_error(status, detail)` answers that status as `application/problem+json`.
  - Any other exception goes on, as from any handler: a connection answers 500 without detail and tells `on_handler_error`.
- `mount` registers the routes in the sans-IO `quoll::http` router, which `quoll::server` feeds. Middlewares (logs, metrics,
  CORS, a rate limit, auth beyond the schemes) run around the chosen route with `router::use`, and read what routing found
  from `next`.

**Calling it.** A client takes the contract and an `http::HttpTransport`, by default `http::client`, over the network natively and
`fetch` in a browser, a Worker or Node. The base URL belongs to the transport.

```cpp
rest::client<bank::v1> api{"https://bank.example.com"};

api.credentials<rest::bearer_auth>(token);

bank::user found = co_await api.call<bank::users::get>(42);
auto maybe = co_await api.try_call<bank::users::get>(7);
auto page = co_await api.call<bank::users::list>({.limit = 20});
auto alice_cards = api.scope<bank::cards::api>(1); // fixes {user_id}
```

- `call` returns the first declared response. Any other outcome throws `rest::call_error`, with the response, and the
  detail of the server's problem, if any, as `what()`; another declared response is read with `try_call`. It is no
  `rest::http_error`, so that a service letting a failed call escape answers 500, not that API's status.
- `try_call` returns `rest::result<E>`, `std::expected<rest::reply<E>, rest::unexpected_response>`: the declared
  response received, or the whole response with why it is unexpected (an undeclared status, a body that does not
  decode).
- Transport failures (network, cancellation) stay exceptions from the task in both cases (`std::system_error`).
- The client is a template over its transport: no virtual call. Type erasure, if a use needs it, goes through
  function-pointer tables, like `any_stream`.
- Interceptors (retry, token refresh on 401) wrap `send`.
- Arguments are taken by value, as the endpoint's types, and live in the task.

**Third-party APIs.** A contract written for someone else's API must express what that API does, not what Quoll
would choose:

- JSON names that are not C++ names (`created_at`, `htmlUrl`, reserved words), through a per-type renaming trait.
- Unknown fields are ignored. A field of type `rest::open_enum<E>` reads a value `E` does not know, kept as it came and
  written back so, while `E` itself stays strict.
- Error bodies of any shape, one type per status (`returns<github_error, status::not_found>`), not only `problem+json`.
- Bodies other than JSON through codecs: text, form, bytes.
- Later: a tool generating the contract header from an existing OpenAPI document. The DSL stays able to express what
  such documents commonly hold.

**Types, JSON and schemas.** Bodies are plain aggregates, read and written by
[Glaze](https://github.com/stephenberry/glaze). Its aggregate reflection gives their fields with no macro and no
declaration, and the DSL uses the same reflection for the fields of `query_params<Struct>` and for the JSON Schema
of `components/schemas`.

- A `glz::meta<T>` specialization renames fields (`created_at`, reserved words), maps enums to names, and describes
  the types that are not aggregates: a class with invariants, a third-party type.
- Decoding errors come back as values (Glaze's `expected`): the server answers 400 with them, and `try_call` reports
  them.
- The client skips unknown keys when it decodes the responses of a third-party API (`error_on_unknown_keys`
  off); Quoll's server may keep rejecting them in requests.
- Reflected types need external linkage: Glaze reads field names through an `extern` object, which Clang rejects
  for a type in an anonymous namespace.
- The DSL writes the JSON Schemas itself, from the same reflection. Glaze 9.1's own JSON Schema does not serve
  `components/schemas` as is: no `required` list, named types inlined instead of `$ref`, 64-bit integers as a
  `minimum` / `maximum` range instead of `format: int64`.

### UI DSL

`quoll::ui` is written in C++ and compiled to WASM, in the same language as the rest of an application. The DOM gives
text, IME, selection and accessibility; Tailwind gives the styles.

```cpp
struct counter {
  state<int> count;

  element view() {
    using namespace quoll::ui;

    return div({
               text("Count: " + std::to_string(*count)) | tw("text-lg"),
               button("Add") | tw("btn btn-primary") | on_click([this] { count = *count + 1; }),
           }) |
           tw("flex items-center gap-2");
  }
};

int main() { quoll::emscripten::mount<counter>(); }
```

A state changing runs the view again; the diff reaches a `Renderer`, the DOM in a page. Handlers are plain functions or
tasks, and call a REST API through a `rest::client`.

`to_html(view)` writes the same interface as HTML, natively as under WASM: a page generated ahead of time, or sent by a
server, without WASM. Making such a page live is hydration: the WASM adopts the nodes it finds rather than creating them,
which needs the states written into the page and a mode of `dom_renderer` that adopts. A view rendered by a server cannot
wait for data while it renders, so how a page gets its data comes first.

### Development tools

Hot reload has three levels, from the cheapest:

1. CSS, through Vite and Tailwind's plugin, which reads the C++ for class names.
2. A literal of `tw()`, `text()` or a tag, sent to the running program with no compilation, keeping its state.
3. Anything else: a rebuild, then the page reloads. A compilation error shows in Vite's overlay. Restoring the states is
   [step 6](#step-6-the-ui-dsl-complete).

For now this is a plugin of the sample `samples/ui_notes`, driven by Vite; [step
7](#step-7-the-quoll-command-driving-vite-maintainer-merges-the-first-pull-request) turns it around.

## WASM constraints

- **No sockets in the browser.** Outgoing HTTP uses the `http_transport` concept implemented with `fetch`; there is no
  server in the browser (WASI later).
- **Threads are optional.** The core (`async`, `http`, `rest`, `ui`) must not require `std::thread`, `std::jthread` or
  blocking waits; threads live in the adapters: the asio runtime on several threads (`runtime_options::threads`, each
  executor a strand, each connection on one of its own, through `async::strands_ref`), and its threads for blocking work.
  Never `sync_wait` on the browser main thread.
- **Exceptions**: build with `-fwasm-exceptions`; keep hot paths on error codes / `results`.
- Every module builds under Emscripten in CI and runs its tests there (headless Node runner), but `quoll::asio`, which
  is native.

## HTTP layer

| | A. Capy + Boost.Http | B. Own stack + llhttp |
|---|---|---|
| Parser / serializer | Boost.Http (sans-IO) | llhttp (Node.js parser, C, also used compiled to WASM by undici) + own serializer |
| Async foundation | Boost.Capy | `quoll::async`, Capy's design without the dependency |
| Maturity | Not in Boost yet, breaking changes weekly (Capy `refactor!` on 2026-10-30), Burl ships its own parser instead of Boost.Http's | llhttp stable for years |
| Dependencies | Boost (system, url, json...) | none besides llhttp |
| WASM | Unverified: Capy's `thread_pool` / `strand` use `std::thread`; no Emscripten support mentioned in either repo | llhttp is plain C, known to build to WASM |

Decision: **B** (2026-11-02).

## Open questions

To settle before authentication:

- **What a reverse proxy forwards.** How a handler learns the client's address and the original scheme and host: rate limiting
  and the redirects and cookies of authentication need them, and Quoll reads no forwarded header yet. To trust one only from a
  configured proxy, as a client can write it too, which needs the peer address of a connection, that no stream, listener or
  request carries today.

Kept for later by the maintainer, at the end of the plan if the need comes (2026-11-05):

- **A middleware for a path prefix**, `router.use("/api", ...)`: for now, it tests `next.path()`.

Those not settled by a step of the roadmap:

- MSVC, which Quoll does not support, and which many of the programs [Positioning](#positioning) aims at build with: a change to a
  hard constraint, the maintainer's.
- A default `main()` provided by Quoll, creating the runtime and running an application coroutine, once the shape of
  that coroutine (arguments, exit code) is settled.
- If OpenAPI comes back: the C++ → JSON Schema mapping, 64-bit integers in particular.
- A batch endpoint, only if a real need shows up.

## Environment

- Scaffolding: `quoll new` in [step 7](#step-7-the-quoll-command-driving-vite-maintainer-merges-the-first-pull-request),
  rather than a template in cpp-init.
- Dev container (`.devcontainer/`): Ubuntu 24.04, GCC 13, Clang 19, CMake / Ninja / ccache, OpenSSL, Emscripten SDK
  (`EMSDK_VERSION`, default `latest`), Node.js (`NODE_MAJOR`, default 24).
- Agent machine: no local toolchain. Docker goes through the docker-in-docker daemon `DOCKER_HOST=tcp://orca-dind:2375`
  (the local socket is not accessible). The image builds there (`docker build -t quoll-dev .devcontainer`), but the
  daemon does not see `/home/orca`: bind mounts are empty, so the sources are streamed in with `tar` and `devcontainer
  up` does not work as is.
