# Quoll — plan

Status: **exploration**, nothing decided beyond what is marked *Decided*. Last update: 2026-11-02.

## Goal

Quoll is the C++20 core of cross-platform applications: asynchronous HTTP **clients and servers** and typed REST
APIs, without doing any I/O itself (sans-IO). The UI is written once with web technologies and talks to the core only
through the typed API contract. The same application can be deployed as:

- a **native server + web app** (no WASM),
- a **standalone PWA** (core compiled to WASM),
- a **native app** (desktop / mobile webview shell + native core),

and any combination of them (e.g. an offline-capable PWA that syncs with a Quoll server).

## Decided

- Name: **Quoll**, a new public library (namespace `quoll::`, same name for the C++ package and the npm package of
  the TS side). It holds `async`, `http`, `client`, `server`, `rest` (DSL) and `bridge`. Puffin keeps its
  existing utility modules (events, ioc, maths, imdb...). Repository: GitHub `4thlabs/quoll`.
- Header-only, C++20 minimum, rules in [`CODING_RULES.md`](CODING_RULES.md). Third-party C/C++ dependencies may be
  compiled targets fetched by CPM; Quoll's own code stays header-only.
- Prior art: the `claude/project-thread-*` branches of the Puffin repo (earlier `async`, `webkit`, `rest`) were
  removed from Puffin's `master` for quality reasons. Ideas only: never port code from them, rewrite to the rules.
- Type erasure: non-intrusive function-pointer tables, `xxx_ref` + `any_xxx` (see the rules).
- The HTTP client is in scope. Burl (CppAlliance) is not an option: it is tied to Corosio for sockets / TLS.
- HTTP layer: **own stack on llhttp** (option B of [HTTP layer](#http-layer)), decided on 2026-11-02. llhttp is
  fetched by CPM and compiled; `quoll::async` follows Capy's design without depending on it.
- WASM is a first-class target: the core never requires threads or blocking waits.
- The API contract is **[OpenAPI](https://spec.openapis.org/oas/v3.1.0) 3.1**, generated from the DSL. The TS side
  is a stock [`openapi-fetch`](https://openapi-ts.dev/openapi-fetch/) client typed by
  [`openapi-typescript`](https://openapi-ts.dev/) (TanStack Query through `openapi-react-query`). Decided on
  2026-11-02, replacing tRPC: no TS-only wire protocol to follow across major versions, no type-only stub router,
  the server stays plain REST usable from any language, and the bridge carries ordinary HTTP requests. See
  [TypeScript client](#typescript-client-openapi).
- Main UI direction: **hybrid web UI** (see [UI](#ui-hybrid-web-model)). Qt is no longer a target: no Qt adapter in
  the plan.
- Web build (2026-11-02, see [Web integration and build chain](#web-integration-and-build-chain)): **React + Vite**
  for the reference sample and the scaffolding template, while Quoll's npm side stays framework-agnostic; **SPA /
  static builds only** (no SSR Node server); `openapi.json` is **generated, never committed**.

## Architecture

### Layers

```mermaid
flowchart TB
  subgraph app["Application"]
    ui["Web UI (TS)<br/>+ openapi-fetch client"]
    handlers["C++ API handlers<br/>(business logic)"]
  end

  subgraph quoll["Quoll (header-only, sans-IO)"]
    rest["quoll::rest<br/>typed API DSL, codecs, OpenAPI generation"]
    bridge["quoll::bridge<br/>HTTP requests as messages"]
    server["quoll::server<br/>HTTP/1.1 connections, middlewares, static files"]
    client["quoll::client<br/>pool, keep-alive, redirects, retry, auth, timeouts"]
    http["quoll::http<br/>messages, parser, serializer, router"]
    async["quoll::async<br/>task, executor_ref, await_context, when_all, co_spawn"]
  end

  subgraph concepts["Concepts (type-erased)"]
    executor(["executor"])
    stream(["byte_stream"])
    connector(["connector"])
    transport(["http_transport"])
    channel(["message_channel"])
  end

  subgraph adapters["Adapters"]
    pool["thread_pool"]
    asio["asio (+ssl)"]
    emscripten["emscripten<br/>browser loop, fetch, postMessage"]
    shell["webview shell<br/>desktop / iOS / Android"]
    memory["memory (tests)"]
  end

  ui --> bridge
  handlers --> rest
  rest --> http
  rest --> client
  bridge --> http
  server --> http
  client --> http
  http --> async
  bridge --> async

  server -.-> stream
  client -.-> connector
  client -.-> transport
  bridge -.-> channel
  async -.-> executor

  executor -.-> pool & asio & emscripten & shell
  stream -.-> asio & memory
  connector -.-> asio & memory
  transport -.-> emscripten
  channel -.-> emscripten & shell & memory
```

| Concept | Meaning | Implemented by |
|---|---|---|
| `executor` | where / when a coroutine resumes | thread pool, asio, browser event loop, shell main thread |
| `byte_stream` | `read_some` / `write_some` on raw bytes (`any_stream`, no allocation per operation) | asio sockets, memory |
| `connector` | `(host, port, tls) -> task<any_stream>` | asio (+ssl), memory |
| `http_transport` | `request -> task<response>`, for platforms that do HTTP themselves | `quoll::client` over a connector, browser `fetch` |
| `message_channel` | send / receive HTTP requests and responses between UI and core | `postMessage` (Worker), webview IPC, HTTP, memory |

The router lives in `quoll::http` and does no I/O: it maps a method and a path template to a handler
`request -> task<response>`. `quoll::rest` registers the DSL routes in it; `quoll::server` feeds it the requests read
from connections, `quoll::bridge` the requests received as messages. The PWA Worker has no `quoll::server` (no
sockets): there the bridge drives the router alone.

Design references taken from Boost.Capy: `await_context` propagated through `await_suspend(h, env)`,
`executor_ref`, zero-allocation `any_stream`.

### One API definition, many outputs

The REST DSL is the contract between the UI and the core, and between processes. Everything else is generated from
it.

```mermaid
flowchart LR
  dsl["API definition<br/>(quoll::rest DSL, C++)"]
  dsl --> routes["Routes<br/>quoll::http router"]
  dsl --> cppclient["C++ client<br/>quoll::client / http_transport"]
  dsl --> spec["OpenAPI 3.1 document<br/>(generated)"]
  spec --> ts["TS types (openapi-typescript)<br/>for openapi-fetch"]
  routes --> srv["quoll::server<br/>requests from connections"]
  routes --> dispatch["quoll::bridge<br/>requests from messages"]
```

## UI: hybrid web model

The UI is a web app (HTML / CSS / TS). Text, IME, selection and accessibility come from the DOM. The typed
`openapi-fetch` client exposes the same API in every mode; only the **`fetch` function** it is given changes.

### Deployment modes

#### 1. Native server + web app (no WASM)

The classic mode: a native Quoll server serves the web app assets and the API. Any browser on any device is a client;
native C++ programs can call the same API.

```mermaid
flowchart LR
  subgraph browser["Browser (any device)"]
    ui1["Web UI"] --> ts1["typed client<br/>(openapi-fetch)"]
  end
  subgraph host["Server (native)"]
    srv["quoll::server"] --> static["static files<br/>(web app)"]
    srv --> h1["API handlers"]
    h1 --> c1["quoll::client<br/>(outgoing calls)"]
  end
  cpp["Native C++ client"]
  ts1 -- "HTTP(S) / fetch" --> srv
  cpp -- "HTTP(S)" --> srv
  c1 -- "HTTP(S)" --> ext["Other services"]
```

#### 2. Standalone PWA (core in WASM)

No server needed: the core runs in a Web Worker (keeps the UI thread free, works single-threaded). Installable,
offline-capable. The core can only be a client: outgoing calls go through `fetch`.

Installing the UI as a PWA (manifest + service worker caching it) works in every mode; what sets this mode apart is
that the **logic** runs in the browser. It is for applications that must work without a server: no infrastructure,
local data (IndexedDB / OPFS) that never leaves the device, full offline use, client-side computing with the same C++
code. A UI that only calls a server is mode 1, installed as a PWA. The WASM build of the core is kept in any case: it
is cheap, it enforces the WASM constraints of the core, and it also serves a future GPU-drawn UI in the browser.

```mermaid
flowchart LR
  subgraph browser["Browser / installed PWA"]
    ui2["Web UI"] --> ts2["typed client<br/>(openapi-fetch)"]
    ts2 -- "postMessage" --> worker
    subgraph worker["Web Worker"]
      d2["quoll::bridge dispatcher"] --> h2["API handlers<br/>(C++ → WASM)"]
    end
    sw["Service worker<br/>(offline cache)"]
  end
  h2 -- "fetch" --> ext2["Remote servers"]
```

#### 3. Native app (webview shell + native core)

For capabilities the browser refuses: listening sockets, full file system, background work, threads. The shell is a
thin native window hosting a webview (desktop: a C++ webview library; mobile: a small Swift / Kotlin host).

```mermaid
flowchart LR
  subgraph app3["Native app"]
    subgraph wv["Webview"]
      ui3["Web UI"] --> ts3["typed client<br/>(openapi-fetch)"]
    end
    subgraph core3["Native core"]
      d3["quoll::bridge dispatcher"] --> h3["API handlers"]
      h3 --> c3["quoll::client"]
      h3 -.-> s3["quoll::server<br/>(optional, LAN / local)"]
    end
    ts3 -- "webview IPC" --> d3
  end
  c3 -- "HTTP(S)" --> ext3["Remote servers"]
```

#### 4. Combined: PWA + server

The PWA runs local logic in WASM (offline, instant) and syncs with a native Quoll server when online.

```mermaid
flowchart LR
  subgraph b4["Browser / PWA"]
    ui4["Web UI"] --> ts4["typed client<br/>(openapi-fetch)"]
    ts4 -- "postMessage" --> w4["Worker: local handlers (WASM)"]
  end
  subgraph s4["Server (native)"]
    srv4["quoll::server"] --> h4["API handlers"]
  end
  ts4 -- "HTTP (online)" --> srv4
  w4 -- "fetch (sync)" --> srv4
```

### Same call, three transports

The client code and the C++ handler are identical; only the `fetch` given to the client and the
`message_channel` differ. In every mode the request reaches the same router.

```mermaid
sequenceDiagram
  participant UI as Web UI
  participant TS as openapi-fetch (generated types)
  participant CH as fetch / message_channel
  participant R as Router (quoll::http), fed by quoll::server or quoll::bridge
  participant H as C++ handler

  UI->>TS: client.GET("/accounts/{id}", { params: { path: { id: 42 } } })
  TS->>CH: HTTP request (method, path, headers, body)
  Note over CH: Server mode: browser fetch (HTTP)<br/>PWA: fetch over postMessage to the Worker<br/>Native app: fetch over webview IPC
  CH->>R: request
  R->>H: decoded, typed arguments
  H-->>R: task<Account>
  R-->>CH: HTTP response (status, JSON body)
  CH-->>TS: Response
  TS-->>UI: { data: Account } or { error }
```

### TypeScript client: OpenAPI

The DSL generates an OpenAPI 3.1 document; `openapi-typescript` turns it into a `.d.ts` at build time, and a stock
`openapi-fetch` client is typed by it. Nothing generated ships at runtime except the tiny client.

```mermaid
flowchart LR
  dsl["API definition<br/>(quoll::rest DSL)"]
  dsl --> spec["openapi.json"]
  dsl --> routes["Routes, quoll::http router<br/>(C++ handlers)"]
  spec -- "openapi-typescript" --> types["api.d.ts<br/>(paths, components)"]
  types -- "import type { paths }" --> cl["openapi-fetch<br/>+ openapi-react-query"]
  cl -- "plain REST<br/>GET /api/accounts/42" --> routes
```

```ts
// api.d.ts is generated by openapi-typescript from the document the DSL produced: do not edit
import createClient from 'openapi-fetch';
import type { paths } from './api';

// transportFetch: the browser fetch, or a fetch over postMessage / webview IPC (see below)
const client = createClient<paths>({ baseUrl: '/api', fetch: transportFetch });

const { data, error } = await client.GET('/accounts/{id}', { params: { path: { id: 42 } } });
```

**Server side.** Plain REST on `quoll::server`: the DSL routes are the paths of the document, bodies are JSON, and
errors are `application/problem+json` (RFC 9457) responses declared in the document, so `error` is typed too. The
document is served by the application (e.g. `/api/openapi.json`) and written at build time for the TS build.

**Other modes.** The `fetch` given to the client serializes the `Request` (method, URL, headers, body) over the
`message_channel`; `quoll::bridge` hands it to the router, the one `quoll::server` feeds in server mode, and sends
the response back. The
UI code does not change between modes, and there is no protocol of our own: the bridge carries HTTP messages.
Streamed bodies (Server-Sent Events, large downloads) cross the bridge as a head message followed by body chunk
messages, which the TS side exposes as the `ReadableStream` of the `Response`: the bridge `fetch` must support it
before SSE does, in every mode. The first prototype only needs complete bodies.

**Consequences for the DSL.** Every route declares its method, path template, path / query / header parameters,
body, and response type per status code; named C++ types become `components/schemas`. The C++ → JSON Schema mapping
must be defined: `std::optional` → not required, `std::variant` → `oneOf` with a discriminator, enums, maps, and
64-bit integers (beyond 2^53, JSON numbers lose precision in JS: string format or documented limit).

**What tRPC gave and this does not.** Automatic request batching (add a batch endpoint if a real need shows up) and
subscriptions (Server-Sent Events, see below). Operation names come
from paths instead of nested router keys. End-to-end typing is kept: `tsc` checks parameters, bodies and responses
against the generated types, and the C++ side decodes and validates every input through the DSL codecs, as the
`.input(...)` validators did with tRPC.

**Server-Sent Events.** The replacement for subscriptions, not in the first prototype. A DSL route kind returns
a stream of typed events; the route is documented as a `text/event-stream` response whose event payload has a JSON
Schema (OpenAPI 3.1 has no first-class SSE, so the event type travels through that schema). The TS side reads it
with a streamed `fetch` (not `EventSource`, which takes neither a custom `fetch` nor headers) through a small typed
helper, so it works in every mode once the bridge streams bodies. Open: the exact shape of that helper.

**Risks.** Generator corner cases (`oneOf` / discriminators, recursive types) and the extra generation step in the
TS build (`openapi-typescript` is a dev dependency). Pin versions, and type-check a sample UI against the generated
types in CI.

### Web integration and build chain

Two toolchains build an application: CMake for the C++ core, npm / Vite for the UI. Their contract is a set of
files, so any web framework fits as long as it builds a static SPA.

```mermaid
flowchart LR
  subgraph cpp["C++ (CMake)"]
    def["api.hpp<br/>API definition (DSL)"]
    gen["quoll_add_openapi<br/>generator (host, or node under Emscripten)"]
    core["core.js + core.wasm<br/>(Emscripten, PWA)"]
    app["native binary<br/>quoll::server or webview shell"]
  end
  subgraph web["UI (npm / Vite)"]
    plugin["quoll/vite plugin"]
    types["api.d.ts<br/>(openapi-typescript)"]
    ui["React UI<br/>openapi-fetch + quoll/fetch"]
    dist["dist/"]
  end
  def --> gen --> spec["openapi.json"]
  spec --> plugin --> types --> ui
  def --> core
  def --> app
  core --> plugin
  ui --> dist
  dist -- "copied or embedded<br/>(quoll_add_web_ui)" --> app
```

**The contract: files.**

1. `openapi.json`, produced by a CMake target from the application's API definition. Generated at every build,
   never committed: building the UI needs the C++ side.
2. `api.d.ts`, produced from it by `openapi-typescript` (gitignored).
3. For the PWA: the Emscripten output `core.js` + `core.wasm` (ES module, worker environment), loaded in a Worker.
4. The UI `dist/`: served by `quoll::server` from a directory, or embedded in the binary (single-binary server,
   native shell).

**Who drives.** Development is Vite-driven: `npm run dev` gives HMR, regenerates the types and proxies the API to the
native server. Release is CMake-driven: `cmake --build` runs the generator, then `npm run build`, then copies or
embeds `dist/` into the binary. Same artifacts, two entry points.

**C++ side.** App-facing CMake helpers in a new `cmake/QuollApp.cmake`, shipped with the package config (separate
from `QuollHelpers.cmake`, which only builds Quoll itself):

- `quoll_add_openapi(TARGET <name> API_HEADER <header> API <symbol> OUTPUT <file>)` generates a tiny `main` that
  prints `quoll::rest::openapi(<symbol>)`, builds it and runs it. The API **definition** therefore lives in a header
  of its own, apart from the handler implementations. In an Emscripten build the generator runs under node through
  `CMAKE_CROSSCOMPILING_EMULATOR`: no host toolchain needed.
- `quoll_add_web_ui(TARGET <app> UI_DIR <dir> [EMBED])` runs `npm ci` and `npm run build` after the generator, then
  copies `dist/` next to the binary or, with `EMBED`, generates an application source holding the files, served by
  `quoll::server` or the shell. That source belongs to the application: Quoll stays header-only.

**npm package `quoll`.** Framework-agnostic (no React dependency), with subpath exports:

- `quoll/fetch`: the transport `fetch` functions given to `openapi-fetch`, over `postMessage` (Worker) and webview
  IPC; server mode uses the browser `fetch`. Complete bodies first; streamed bodies (and SSE) once the bridge
  streams them.
- `quoll/worker`: bootstrap that loads `core.js` in the Worker and wires `postMessage` to `quoll::bridge`.
- `quoll/vite`: Vite plugin. Runs the generator target and `openapi-typescript` at start and when `openapi.json`
  changes, proxies `/api` to the native server in development, serves and emits `core.js` + `core.wasm` in PWA mode,
  and adds the COOP / COEP headers only when the core uses threads.
- The deployment mode is chosen at build time (Vite mode / environment): one UI codebase.

**Frameworks.** React, the reference: `openapi-react-query` on top of the client. Vue, Svelte, Solid, Angular: their
TanStack Query adapter fed by the same `openapi-fetch` client, no Quoll package per framework. Meta-frameworks only
in static / SPA mode (Next.js `output: 'export'`, SvelteKit `adapter-static`, Nuxt `ssr: false`): an SSR Node server
cannot run in the PWA or the native shell. Offline caching of the `.wasm` is the application's choice
(`vite-plugin-pwa` in the sample).

**Native shell.** The webview loads the embedded UI through a custom URL scheme served by the router, and the IPC
`fetch` goes to `quoll::bridge`.

### Mode capabilities

| | Server + web app | PWA (WASM) | Native app (shell) |
|---|---|---|---|
| Install | none (URL) | from browser | store / installer |
| Offline | no | yes (service worker) | yes |
| Core runs | on the server | in a Web Worker | in the app process |
| Listen / serve | **yes** | no | yes (LAN / local) |
| Outgoing HTTP | `quoll::client` | `fetch` only | `quoll::client` |
| Threads | yes | optional (needs COOP / COEP) | yes |
| Multi-user | yes | no | no |

### Known limits

- Two languages: C++ core, TS UI.
- Webview engines differ: WebKit (iOS, macOS, Linux), Chromium (WebView2 on Windows, Android WebView). Test on each.
- iOS PWAs are restricted (storage eviction, background work): status to check.
- Bridge cost: every call is serialized. Large payloads need a binary encoding or shared buffers, not JSON.

### Long-term alternative: GPU-drawn UI in C++

Kept for later, not planned: a "modern Qt" assembled from SDL3 (platform, IME, lifecycle), WebGPU (Dawn natively,
browser API on the web), Skia or ThorVG + HarfBuzz (2D + text), Yoga (layout), AccessKit (accessibility), with
`quoll::async` driving the frame loop and reactive signals instead of moc. In the browser it would run on the same
WASM build of the core as mode 2. The hard, multi-year parts are text, IME and accessibility on mobile and web,
which the hybrid web model gets for free.

## WASM constraints

- **No sockets in the browser.** Outgoing HTTP uses the `http_transport` concept implemented with `fetch`; there is
  no server in the browser (WASI later).
- **Threads are optional.** The core (`async`, `http`, `rest`, `bridge`) must not require `std::thread`,
  `std::jthread` or blocking waits; threads only live in the thread pool adapter. Never `sync_wait` on the browser
  main thread.
- **Exceptions**: build with `-fwasm-exceptions`; keep hot paths on error codes / `results`.
- Every module builds and runs its tests under Emscripten in CI (headless Node runner).

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

Every point the plan leaves open, gathered here; the linked sections give the context.

- C++ → JSON Schema mapping details, 64-bit integers in particular ([TypeScript client](#typescript-client-openapi)).
- Shape of the typed SSE helper on the TS side, and streamed bodies over the bridge
  ([TypeScript client](#typescript-client-openapi)).
- A batch endpoint, only if a real need shows up ([TypeScript client](#typescript-client-openapi)).
- Webview library for the desktop shell, and the mobile hosts (Swift / Kotlin)
  ([Web integration](#web-integration-and-build-chain)).
- Embedding format of `dist/`: generated byte arrays, compression, `#embed` once compilers have it
  ([Web integration](#web-integration-and-build-chain)).
- iOS PWA restrictions: storage eviction, background work ([Known limits](#known-limits)).

## Prototype (next step)

Goal: validate the design with code. Success criteria:

1. A client GET, end to end, with llhttp + own serializer over a `byte_stream`.
2. Parser edge cases: `HEAD` responses, `1xx` / `100-continue`, close-delimited bodies, chunked with trailers.
3. An asio `connector` producing a type-erased stream.
4. The HTTP core compiles with Emscripten; a `fetch`-based `http_transport` runs the same call in Node.
5. One API defined with the DSL, its generated OpenAPI document and TS types, called with `openapi-fetch` from the
   same React + Vite sample (`samples/`) in three modes: native server (browser `fetch`), PWA (Quoll in a Worker,
   `fetch` over `postMessage`), native webview shell (`fetch` over IPC). Typed errors and type checking (`tsc` fails
   on a wrong parameter or body) must work, and the document validates against the OpenAPI 3.1 schema.
6. The build chain of the sample works both ways in CI: `npm run dev` with `quoll/vite` (development path) and
   `cmake --build` with `quoll_add_openapi` / `quoll_add_web_ui` (release path).

## Environment

- Scaffolding: once the prototype holds, a `web-quoll` template in cpp-init (`../cpp-init`: React + Vite + CMake
  presets), following its `templates/cli-puffin`.
- Dev container (`.devcontainer/`): Ubuntu 24.04, GCC 13, Clang 18, CMake / Ninja / ccache, OpenSSL, Emscripten SDK
  (`EMSDK_VERSION`, default `latest`), Node.js (`NODE_MAJOR`, default 24).
- Agent machine: no local toolchain. Docker goes through the docker-in-docker daemon `DOCKER_HOST=tcp://orca-dind:2375`
  (the local socket is not accessible). The image builds there (`docker build -t quoll-dev .devcontainer`), but the
  daemon does not see `/home/orca`: bind mounts are empty, so the sources are streamed in with `tar` and
  `devcontainer up` does not work as is.
