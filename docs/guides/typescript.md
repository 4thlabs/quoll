# TypeScript

A web page calls a quoll server through TypeScript that a generator writes from the contract's
[OpenAPI document](openapi.md). Any OpenAPI generator works: orval, openapi-typescript, hey-api, Kiota... `cargo quoll`
writes the document and runs the generator after it. It also runs the server and the page's dev server side by side.
It knows no bundler and no generator: the commands it runs are those of the project, named in the workspace's
`Cargo.toml`, and the web side follows conventions.

This guide follows the notes example. Its generator is orval, which writes TanStack Query hooks. Another generator
takes its place in the same config: [Another generator](#another-generator) shows how.

The TypeScript and TOML blocks of this guide are excerpts of [the notes example](../../examples/notes), under the
name of their file. The code that the page does not use is in its `web/src/guide/`: `EventSource`, and the reading of
`JsonLines` and `ByteStream`. The blocks of Next.js and of another generator belong to other apps, checked with
Next.js 16 and openapi-typescript 7.

## The chain

```text
notes-api                      the contract: quoll::api! and its types
  │  cargo quoll gen           builds and runs its binary `openapi`
  ▼
web/openapi.json               the OpenAPI 3.1 document
  │  after: npx --no orval     the generator the project chose
  ▼
web/src/api/generated.ts       the types and the TanStack Query hooks
  │  import
  ▼
the page                       Vite here, Next.js or any other bundler
```

`cargo quoll` is installed from git until it is published:

```bash
cargo install --git https://github.com/4thlabs/quoll quoll-cli
```

## The config

The config of `cargo quoll` is under `[workspace.metadata.quoll]`, in the `Cargo.toml` of the workspace where
`cargo quoll` runs.

[`examples/notes/Cargo.toml`](../../examples/notes/Cargo.toml)

```toml
[[workspace.metadata.quoll.api]]
package = "notes-api"
bin = "openapi"
output = "web/openapi.json"
# Run in the directory of `output`. `--no`: the orval of the page's package-lock.json, which `npm ci` installs, never
# one npx would download.
after = [["npx", "--no", "orval"]]

# Run by `cargo quoll dev`, ready once its url accepts a connection. The page's vite.config.ts proxies `/api` to this
# url: the two change together.
[[workspace.metadata.quoll.server]]
package = "notes-server"
url = "http://127.0.0.1:8080"

# The page: `dev` runs beside the server in `cargo quoll dev`, `build` after the document in `cargo quoll build`, both
# in `dir`.
[workspace.metadata.quoll.web]
dir = "web"
dev = ["npm", "run", "dev"]
build = ["npm", "run", "build"]
```

- `api`, one per contract:
  - its `package`;
  - the `bin` that prints its document;
  - the `output` it is written to, from the workspace's root;
  - `after`, the commands run after it, in order, in the directory of `output`. A command that fails stops those
    after it.
- `server`, one per server: its `package`, and the `url` it answers on. Its binary is the one that `cargo run` would
  run: the package's only binary, or its `default-run`.
- `web`, once:
  - its `dir`, from the workspace's root;
  - `dev`, which `cargo quoll dev` runs beside the servers;
  - `build`, which `cargo quoll build` runs after the documents.

  Both commands run in `dir`.

A command is a list of words, run with no shell. Its program is found on the `PATH` the way the OS finds it, so that
`npm` is `npm.cmd` on Windows. A command that needs a shell, such as a pipe or a `&&`, is a script of the page's
`package.json`, run as `["npm", "run", "<script>"]`. `npx --no` runs the generator that `npm ci` installs, at the
version that `package-lock.json` pins. It fails rather than download one when the generator is missing.

A config that cannot be read is an error that names the key at fault:
`workspace.metadata.quoll.api[0].output: missing`. Each command that fails shows its output, the same way a cargo
build does.

## `cargo quoll gen`

`cargo quoll gen` builds and runs each api's binary, and writes its `output` when its content changed. A document
that did not change is left unchanged, so that the bundler reloads nothing. Then it runs the api's `after` commands,
whether the document changed or not, so that a checkout whose document is committed gets its TypeScript too.

`cargo quoll gen --check` writes nothing and runs nothing: it fails when a document on disk is not the one its
contract prints, or is missing. It is the check for a project that commits its document. Cargo builds the contract
with `--locked`, so that the check does not update the `Cargo.lock` either.

A document is compared with its line endings read as LF, so that a checkout with `core.autocrlf` is not stale. A
committed document is best kept as the contract prints it, with LF, set in `.gitattributes`:

```text
web/openapi.json text eol=lf
```

## `cargo quoll build`

`cargo quoll build` runs `gen`, then `web.build` in `web.dir`, and fails when either fails. It is the build of the
page in CI, and before a release. The TypeScript is written from the contract of the commit, then type-checked and
bundled with the page.

## `cargo quoll dev`

`cargo quoll dev` runs `gen`, builds and starts the servers, then starts `web.dev`. Until Ctrl-C, it makes again what
a change of a file touches.

```bash
cd examples/notes
npm ci --prefix web
cargo quoll dev
```

In the notes example it runs the notes server, orval and Vite, on `http://localhost:5173`. The server prints the
demo's password when it starts.

- A change to a contract, or to a crate it is built with, writes its document when its content changed. It runs its
  `after` commands when the document changed, or until they have succeeded once. It also restarts the servers built
  with it.
- A change to a server restarts only that server. The running one stops only once the new one has compiled. A server
  that does not compile prints cargo's errors, and the last one that compiled keeps answering.
- A server is ready when its `url` accepts a connection. `web.dev` starts once every server is ready or has failed,
  and runs until the end. The bundler watches its own files, including the TypeScript that `after` writes, and
  reloads the page itself. A cargo error shows in the terminal of `cargo quoll dev`, not in the bundler's overlay.
- Ctrl-C, or `SIGTERM`, stops `web.dev`, then the servers. Each one is stopped with what it started. This works on
  Linux, macOS and Windows.

The workspace's `Cargo.toml` and `Cargo.lock` are watched too, and so are the path dependencies outside the
workspace. These are not watched:

- a package's own `target/`;
- hidden files;
- `node_modules`;
- the documents;
- `web.dir`, except the crates inside it.

On Linux, a package at the workspace's root that holds the web side's `node_modules` may use up inotify's watches, and
a warning names it. Raise `fs.inotify.max_user_watches`, or keep the web side out of that package.

`cargo quoll dev --json` prints one JSON line per event on stdout, for a tool that reads them, such as an editor or a
script that waits for the server. It prints its output for a person on stderr:

```text
{"event":"document","package":"notes-api","path":"web/openapi.json","changed":true}
{"event":"server","state":"building","package":"notes-server","url":"http://127.0.0.1:8080"}
{"event":"error","source":"cargo","package":"notes-server","message":"mismatched types","file":"server/src/main.rs","line":12,"column":5}
{"event":"server","state":"failed","package":"notes-server","url":"http://127.0.0.1:8080"}
{"event":"server","state":"ready","package":"notes-server","url":"http://127.0.0.1:8080"}
{"event":"web","state":"started"}
```

The [dev loop of the spec](../spec.md#the-dev-loop) lists every event and its fields.

## Vite

### The proxy

The page calls the api on its own origin, `/api/...`, and Vite's own proxy sends the call to the server's `url`. That
`url` is written twice: in the workspace's `Cargo.toml` and in `vite.config.ts`. The two change together.

[`examples/notes/web/vite.config.ts`](../../examples/notes/web/vite.config.ts)

```typescript
import react from "@vitejs/plugin-react";
import { defineConfig } from "vite";

export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      // The server's `url` in the workspace's Cargo.toml, `[[workspace.metadata.quoll.server]]`: the two change together.
      "/api": "http://127.0.0.1:8080",
    },
  },
});
```

The proxy needs nothing else for streams. An `Events` answer opens with a comment. Vite's proxy holds the head of an
answer until its first bytes, so it lets that comment through at once: the page sees the stream open before its first
event.
A `JsonLines` or a `ByteStream` is seen to open with its first line or chunk.

### orval

The notes example's generator is orval. Its config reads the document, `input: "./openapi.json"`, which is the api's
`output` seen from `web/`. `after` runs orval. Another generator's config reads the document the same way.

[`examples/notes/web/orval.config.ts`](../../examples/notes/web/orval.config.ts)

```typescript
import { defineConfig } from "orval";

export default defineConfig({
  notes: {
    input: "./openapi.json",
    output: {
      target: "./src/api/generated.ts",
      client: "react-query",
      httpClient: "fetch",
      override: {
        mutator: { path: "./src/api/fetch.ts", name: "apiFetch" },
        // The hooks' data is the body, which the mutator returns, not orval's `{ data, status, headers }`.
        fetch: { includeHttpResponseReturnType: false },
      },
    },
  },
});
```

- `client: "react-query"` writes one hook per route, such as `useNotesGet` or `useNotesUpdate`, and its query key,
  `getNotesGetQueryKey`. `vue-query`, `svelte-query`, `swr` or a plain client come from the same document.
- `httpClient: "fetch"` with the mutator: every request goes through one function of the page, `apiFetch`.
- `includeHttpResponseReturnType: false` makes a hook's data the body that `apiFetch` returns.

### The mutator

The mutator adds the credential: here, the bearer token that the page holds. It throws an `ApiError` for an error.
The error's message is the problem's detail, which the page shows. Every error of a quoll server is an
`application/problem+json` problem: the `Problem` type that orval writes. A proxy that cannot reach the server answers
with no problem, and the status alone is the error. `ErrorType` is the error type of the hooks, which orval imports from the mutator's file.

[`examples/notes/web/src/api/fetch.ts`](../../examples/notes/web/src/api/fetch.ts)

```typescript
import type { Problem } from "./generated";

let token: string | undefined;

export function signIn(accessToken: string) {
  token = accessToken;
}

export function signOut() {
  token = undefined;
}

// An error the server answered, its message the problem's detail: what the page shows. `expired` when the server
// refused the token that the page still holds: it expired, or the server that gave it restarted.
export class ApiError extends Error {
  readonly status: number;
  readonly problem: Problem | undefined;
  readonly expired: boolean;

  constructor(status: number, problem?: Problem, expired = false) {
    super(problem?.detail ?? problem?.title ?? `the server answered ${status}`);
    this.name = "ApiError";
    this.status = status;
    this.problem = problem;
    this.expired = expired;
  }
}

// The error type of orval's hooks, whatever the problem a route declares: every one is a `Problem`.
export type ErrorType<_Body> = ApiError;

function authorization(init?: HeadersInit): Headers {
  const headers = new Headers(init);

  if (token) {
    headers.set("Authorization", `Bearer ${token}`);
  }

  return headers;
}

export function isExpired(error: unknown): boolean {
  return error instanceof ApiError && error.expired;
}

// The error of a request sent with the token `sent`. A refusal of a token signed out since, answered late, is no news
// of the session the page holds now.
async function apiError(response: Response, sent: string | undefined): Promise<ApiError> {
  const expired = response.status === 401 && sent !== undefined && sent === token;

  return new ApiError(response.status, await problemOf(response), expired);
}

// A proxy that cannot reach the server answers with no problem, and a body may be cut: the status alone is the error.
async function problemOf(response: Response): Promise<Problem | undefined> {
  if (!response.headers.get("Content-Type")?.startsWith("application/problem+json")) {
    return undefined;
  }

  try {
    return await response.json();
  } catch {
    return undefined;
  }
}

export async function apiFetch<T>(url: string, init: RequestInit): Promise<T> {
  const sent = token;
  const response = await fetch(url, { ...init, headers: authorization(init.headers) });

  if (!response.ok) {
    throw await apiError(response, sent);
  }

  // A 204 or a 205 has no body: `null`, the JSON of an `Option` that is `None`.
  const empty = response.status === 204 || response.status === 205;

  return (empty ? null : await response.json()) as T;
}
```

A page whose scheme is a cookie needs no credential in the mutator: the browser sends the cookie of its own origin.
The [authentication guide](authentication.md) says what a cookie scheme asks of the routes that take it.

### Hooks in a component

A query hook takes the route's parameters, with its query struct as an object, and TanStack Query's options under
`query`:

[`examples/notes/web/src/notes/NoteList.tsx`](../../examples/notes/web/src/notes/NoteList.tsx)

```typescript
export function NoteList({ selected, onSelect, onNew }: Props) {
  const [filter, setFilter] = useState<NotesSearchParams>({});
  const [page, setPage] = useState(1);
  // The page shown stays until the next one arrives, rather than blink.
  const { data, error, isPlaceholderData } = useNotesSearch(
    { ...filter, page },
    { query: { placeholderData: keepPreviousData } },
  );
  const notes = data ?? [];
```

A mutation hook takes its options under `mutation`. Its `mutate` takes the route's parameters, with its body as
`data`. The page reads again what a change touches, through the query keys that orval writes:

[`examples/notes/web/src/notes/NoteView.tsx`](../../examples/notes/web/src/notes/NoteView.tsx)

```typescript
export function NoteView({ id, onClosed, onExpired }: Props) {
  const queryClient = useQueryClient();

  function listChanged() {
    queryClient.invalidateQueries({ queryKey: getNotesSearchQueryKey() });
  }

  const { data: note, error, refetch } = useNotesGet(id);
  // The cache follows a change here, as `mutate`'s own callbacks do not once the view is gone. The note is read
  // again rather than set to the answer, which a change elsewhere, read meanwhile, may be newer than.
  const update = useNotesUpdate({
    mutation: {
      onSuccess: () => {
        queryClient.invalidateQueries({ queryKey: getNotesGetQueryKey(id) });
        listChanged();
      },
    },
  });
  const remove = useNotesRemove({ mutation: { onSuccess: listChanged } });
```

### The query client

The page's query client does not try again an error below 500, which answers the same when asked again: a missing
note, a refused token. When the token that the page holds is refused, `ApiError` says `expired`, and the page signs
out, whichever query or mutation hears of it.

[`examples/notes/web/src/queryClient.ts`](../../examples/notes/web/src/queryClient.ts)

```typescript
import { MutationCache, QueryCache, QueryClient } from "@tanstack/react-query";

import { ApiError, isExpired } from "./api/fetch";

// An error below 500 answers the same when asked again: a missing note, a refused token.
function retryServerErrors(failures: number, error: Error): boolean {
  if (error instanceof ApiError && error.status < 500) {
    return false;
  }

  return failures < 3;
}

// The page's client: `onExpired` hears of every request whose token, the one the page holds, is refused.
export function createQueryClient(onExpired: () => void): QueryClient {
  function signOutWhenExpired(error: Error) {
    if (isExpired(error)) {
      onExpired();
    }
  }

  return new QueryClient({
    queryCache: new QueryCache({ onError: signOutWhenExpired }),
    mutationCache: new MutationCache({ onError: signOutWhenExpired }),
    defaultOptions: { queries: { retry: retryServerErrors } },
  });
}
```

[`examples/notes/web/src/App.tsx`](../../examples/notes/web/src/App.tsx)

```typescript
export function App() {
  const [session, setSession] = useState<Session>("signed-out");
  const [queryClient] = useState(() => createQueryClient(() => setSession(expire)));

  // The token, and what was read with it, go with the session, whoever ends it.
  useEffect(() => {
    if (session !== "signed-in") {
      signOut();
      queryClient.clear();
    }
  }, [session, queryClient]);
```

## Streams

The routes whose result is a stream are read with the browser's own APIs. quoll has no helper on the web side for
now: [Later](../spec.md#later) says what comes. orval writes a hook for such a route too, but that hook would wait for
the whole body as one JSON value. So the page takes the route's url instead, `getNotesEventsUrl(id)`, which orval
writes beside the hook.

| The route returns | On the wire                                | Read with                                         |
| ----------------- | ------------------------------------------ | ------------------------------------------------- |
| `Events<T>`       | `text/event-stream`, an item per event     | `EventSource` with a cookie, `fetch` with a header |
| `JsonLines<T>`    | `application/jsonl`, an item per line      | `fetch` and `response.body.getReader()`           |
| `ByteStream`      | its content type, chunks as they come      | `fetch` and `response.body.getReader()`           |

### The types of the items

OpenAPI 3.1 cannot type the items of a stream. So the document names them in the extension `x-item` of the answer's
media type: a `$ref` to the item's schema. The schema is under `components/schemas`. A generator writes it as a type
like any other, and the page names that type. A variant of a tagged enum that holds a struct is an
[`allOf`](openapi.md#what-it-holds) of that struct and of its tag, so that its type keeps the tag:

```rust
use quoll::Events;
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct Note {
    pub id: i64,
    pub title: String,
    pub body: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum NoteEvent {
    Edited(Note),
    /// The last event of the note's stream.
    Removed { id: i64 },
}

quoll::api! {
    pub api Api = "/api";

    #[auth(Bearer)]
    pub group Notes {
        GET "/notes/{id}/events" => events(id: i64) -> Events<NoteEvent>;
    }
}

let json = Api::openapi().to_json();

assert!(json.contains(r##""x-item":{"$ref":"#/components/schemas/NoteEvent"}"##));
assert!(json.contains(r##""allOf":[{"$ref":"#/components/schemas/Note"}"##));
```

`examples/notes/web/src/api/generated.ts`, as orval writes it:

```typescript
export type NoteEvent = Note & {
  kind: 'edited';
} | {
  id: number;
  kind: 'removed';
};
```

An `i64` is a `number`, exact up to 2^53. An id or a count stays below that. A value that may go beyond it is a
`String` in its Rust type.

### `Events` with a header credential

`EventSource` cannot send a header. So a page whose scheme is a bearer token, or any other header, reads an `Events`
route with `fetch`. It splits the events itself, the way quoll's server writes them, each line ended by `\n`. The
notes page's `apiEvents` does this, with the token of its mutator. Every event that has data is an item. An event with
no data, and a comment, are skipped. An `event: problem` ends the stream with its problem.

[`examples/notes/web/src/api/fetch.ts`](../../examples/notes/web/src/api/fetch.ts)

```typescript
export type EventHandlers<T> = {
  // Each event's data, parsed.
  onItem: (item: T) => void;
  // The stream is open: what changed before it is not in it.
  onOpen?: () => void;
  // An end that was not asked for: the error of a failure, before the stream or within it, or nothing when the
  // server ended the stream.
  onEnd?: (error?: unknown) => void;
};

// Reads a stream of server-sent events until the function it returns is called.
export function apiEvents<T>(
  url: string,
  { onItem, onOpen = () => {}, onEnd = logFailure }: EventHandlers<T>,
): () => void {
  const controller = new AbortController();

  function ended(error?: unknown) {
    if (!controller.signal.aborted) {
      onEnd(error);
    }
  }

  readEvents(url, onItem, onOpen, controller.signal).then(() => ended(), ended);

  return () => controller.abort();
}

function logFailure(error?: unknown) {
  if (error !== undefined) {
    console.error(error);
  }
}

async function readEvents<T>(url: string, onItem: (item: T) => void, onOpen: () => void, signal: AbortSignal) {
  const sent = token;
  const response = await fetch(url, { headers: authorization(), signal });

  if (!response.ok) {
    throw await apiError(response, sent);
  }

  // A 204 or a 205 has no body: a stream that ends at once.
  if (response.body === null) {
    return;
  }

  onOpen();

  const reader = response.body.pipeThrough(new TextDecoderStream()).getReader();
  let buffer = "";

  for (;;) {
    const { done, value } = await reader.read();

    if (done) {
      return;
    }

    // A chunk may end within an event: its start waits in `buffer` for the next one.
    const blocks = (buffer + value).split("\n\n");
    buffer = blocks.pop() ?? "";

    for (const block of blocks) {
      const { event, data } = parseEvent(block);

      if (data === undefined) {
        continue;
      }

      // The stream failed after its 200: the problem says the status it would have answered.
      if (event === "problem") {
        const problem: Problem = JSON.parse(data);

        throw new ApiError(problem.status ?? 500, problem);
      }

      onItem(JSON.parse(data) as T);
    }
  }
}

// One event, its lines `field: value`; a line that starts with `:` is a comment.
function parseEvent(block: string): { event?: string; data?: string } {
  let event: string | undefined;
  let data: string | undefined;

  for (const line of block.split("\n")) {
    const colon = line.indexOf(":");
    const field = colon < 0 ? line : line.slice(0, colon);
    const value = colon < 0 ? "" : line.slice(colon + 1).replace(/^ /, "");

    if (field === "event") {
      event = value;
    } else if (field === "data") {
      data = data === undefined ? value : `${data}\n${value}`;
    }
  }

  return { event, data };
}
```

A component follows the stream in an effect, whose cleanup aborts it. A change made before the stream opened is not
in it: the page reads the note again once it is open. The `allOf` of the document makes `event.kind` narrow the item:

[`examples/notes/web/src/notes/NoteView.tsx`](../../examples/notes/web/src/notes/NoteView.tsx)

```typescript
  const changed = useEffectEvent((event: NoteEvent) => {
    listChanged();

    switch (event.kind) {
      case "edited":
        refetch();
        break;
      case "removed":
        onClosed();
        break;
    }
  });

  // A change made before the stream opened is not in it: the note is read again once it is open.
  const opened = useEffectEvent(() => refetch());

  // The stream ended before the note was removed: the server stopped, or the network failed.
  const ended = useEffectEvent((error?: unknown) => {
    if (isExpired(error)) {
      onExpired();
    } else {
      setLive(false);
    }
  });

  useEffect(() => {
    return apiEvents<NoteEvent>(getNotesEventsUrl(id), { onItem: changed, onOpen: opened, onEnd: ended });
  }, [id, follows]);
```

### `Events` with a cookie

A same-origin page whose scheme is a cookie, which the browser sends itself, may read an `Events` route with
`EventSource`. Two of its behaviours matter here:

- `onmessage` sees only the events with no name, which are the items. A stream that fails after its 200 ends with an
  `event: problem`. `source.addEventListener("problem", ...)` sees that event, and its `data` is the problem.
- After a clean end of the stream, `EventSource` connects again on its own, unless it is closed. It goes on only when
  that request is answered with a 200. Any other answer closes it for good: a refused cookie, a missing note, or a 5xx
  of a proxy while the server restarts. It sees no status: only `onerror` says it failed.

[`examples/notes/web/src/guide/cookieEvents.ts`](../../examples/notes/web/src/guide/cookieEvents.ts)

```typescript
import { ApiError } from "../api/fetch";
import type { Problem } from "../api/generated";

export type SourceHandlers<T> = {
  // Each event's data, parsed.
  onItem: (item: T) => void;
  // The stream is open, at first or again after a clean end: what changed meanwhile is not in it.
  onOpen?: () => void;
  // The source is closed for good: the problem that ended the stream, or nothing when it could not connect again.
  onEnd?: (error?: ApiError) => void;
};

// Follows an `Events` route until the function it returns is called.
export function followEvents<T>(
  url: string,
  { onItem, onOpen = () => {}, onEnd = () => {} }: SourceHandlers<T>,
): () => void {
  const source = new EventSource(url);

  source.onopen = () => onOpen();

  // The items, which are events with no name.
  source.onmessage = (event) => onItem(JSON.parse(event.data) as T);

  // `onmessage` sees no named event: the stream failed after its 200, and its data is the problem.
  source.addEventListener("problem", (event) => {
    const problem: Problem = JSON.parse(event.data);

    source.close();
    onEnd(new ApiError(problem.status ?? 500, problem));
  });

  // After a clean end the source connects again on its own, and goes on only when that is answered with a 200: any
  // other answer closes it, a 5xx of a proxy while the server restarts among them.
  source.onerror = () => {
    if (source.readyState === EventSource.CLOSED) {
      onEnd();
    }
  };

  return () => source.close();
}
```

An item that ends its stream, such as `removed`, closes the source in `onItem`, with the function that
`followEvents` returns. Otherwise the clean end that follows it connects again.

### `JsonLines` and `ByteStream`

A `JsonLines` or a `ByteStream` route is read with `fetch` and `response.body.getReader()`. The answer is fetched the
same way `readEvents` fetches its own: with the credential, and with its `ApiError` when it is not a 200.

[`examples/notes/web/src/guide/streams.ts`](../../examples/notes/web/src/guide/streams.ts)

```typescript
// Reads a `JsonLines` answer, each line the JSON of an item, ended by `\n` as quoll's server writes it. A failure of
// the server after the 200 cuts the body: `read` throws, which is no clean end.
export async function readJsonLines<T>(response: Response, onItem: (item: T) => void): Promise<void> {
  // A 204 or a 205 has no body: no item.
  if (response.body === null) {
    return;
  }

  const reader = response.body.pipeThrough(new TextDecoderStream()).getReader();
  let buffer = "";

  for (;;) {
    const { done, value } = await reader.read();

    if (done) {
      return;
    }

    // A chunk may end within a line: its start waits in `buffer` for the next one.
    const lines = (buffer + value).split("\n");
    buffer = lines.pop() ?? "";

    for (const line of lines) {
      onItem(JSON.parse(line) as T);
    }
  }
}

// Reads a `ByteStream` answer, its chunks as they come. A failure after the 200 cuts it too.
export async function readChunks(response: Response, onChunk: (chunk: Uint8Array) => void): Promise<void> {
  // A 204 or a 205 has no body: no chunk.
  if (response.body === null) {
    return;
  }

  const reader = response.body.getReader();

  for (;;) {
    const { done, value } = await reader.read();

    if (done) {
      return;
    }

    onChunk(value);
  }
}
```

### What ends a stream

|                                    | `Events`                                         | `JsonLines` and `ByteStream`                  |
| ---------------------------------- | ------------------------------------------------ | --------------------------------------------- |
| the handler's stream ends          | a clean end                                      | a clean end                                   |
| a failure after the 200            | an `event: problem`, then a clean end            | a cut: the reader throws                      |
| the server is told to stop         | a clean end, after the event it is sending       | a cut once the server's grace has passed      |
| the page stops reading             | `close()`, or the fetch's `AbortController`      | the fetch's `AbortController`                 |

A clean end is no failure: the page may follow the stream again, and reads again what changed meanwhile.
[Shutdown](server.md#shutdown) says what a server told to stop does with each stream.

In `cargo quoll dev`, a server that restarts is sent `SIGTERM` on Linux and macOS, and ends its `Events` streams
cleanly. Vite's proxy passes that end on. It does not pass on a stream that the server cuts rather than ends: Vite's
proxy keeps the page's side open, and the page does not see the stream end. This happens to:

- a `JsonLines` or a `ByteStream` at a restart;
- any stream on Windows, where the server's job is terminated;
- any stream of a server that crashes.

## Next.js

The config of `cargo quoll` names no bundler. A Next.js app takes the same `web` table, whose `npm run dev` is
`next dev`.

The scripts of `package.json`:

```json
{
  "scripts": {
    "dev": "next dev",
    "build": "next build"
  }
}
```

The proxy is Next's `rewrites`, to the server's `url`. The two change together:

`next.config.ts`

```typescript
import type { NextConfig } from "next";

const config: NextConfig = {
  async rewrites() {
    return [
      {
        // The server's `url` in the workspace's Cargo.toml, `[[workspace.metadata.quoll.server]]`: the two change together.
        source: "/api/:path*",
        destination: "http://127.0.0.1:8080/api/:path*",
      },
    ];
  },
};

export default config;
```

orval's config and the mutator are the same as for the Vite page. The hooks run in the browser, so the components
that use them are client components.

`app/page.tsx`

```typescript
"use client";

import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { useState } from "react";

import { useNotesSearch } from "../src/api/generated";

function Notes() {
  const { data, error } = useNotesSearch({});

  return error ? <p role="alert">{error.message}</p> : <p>{data?.length ?? 0} notes</p>;
}

export default function Page() {
  const [client] = useState(() => new QueryClient());

  return (
    <QueryClientProvider client={client}>
      <Notes />
    </QueryClientProvider>
  );
}
```

With `next dev`, the rewrites pass an `Events` stream on as it comes: its opening comment first, and its clean end
when `cargo quoll dev` restarts the server. A static export, `output: "export"`, has no rewrites: whatever serves its
files sends `/api` to the server.

## Another generator

`after` runs the generator that the page depends on, whichever it is, pinned by its lock file: `npx --no` runs it. A
page may take only the types, written by `openapi-typescript`, and call `fetch` itself. It names that generator in
place of orval:

```toml
[[workspace.metadata.quoll.api]]
package = "notes-api"
bin = "openapi"
output = "web/openapi.json"
after = [["npx", "--no", "openapi-typescript", "openapi.json", "-o", "src/api/schema.d.ts"]]
```

`after` is a list: a second generator runs after the first, in the same directory. Here `openapi-typescript` writes
the types beside orval's hooks:

```toml
[[workspace.metadata.quoll.api]]
package = "notes-api"
bin = "openapi"
output = "web/openapi.json"
after = [
  ["npx", "--no", "orval"],
  ["npx", "--no", "openapi-typescript", "openapi.json", "-o", "src/api/schema.d.ts"],
]
```

A command that needs more than its arguments, such as an environment variable or a second step, is a script of the
page's `package.json`. `after` runs it as `["npm", "run", "<script>"]`.

## What is committed

The notes example commits neither the document nor the TypeScript: `cargo quoll gen` writes them, in development and
in a build. They are generated code on disk, which `tsc` and the editor need.

- Committed: the contract, the page's code including its mutator, `orval.config.ts`, `package.json` and
  `package-lock.json`.
- Ignored: `openapi.json`, through `**/openapi.json` in the root `.gitignore`; orval's `src/api/generated.ts`, `dist`
  and `node_modules`, in the page's `.gitignore`.

A project that wants a CI of its JavaScript alone, with no cargo, commits its document, with LF in `.gitattributes`.
It runs `cargo quoll gen --check` in its Rust CI, and the JavaScript CI runs the generator on the committed document.

## CI

The `web` job of quoll's CI is the model. It installs the page's dependencies. Then `cargo quoll build` writes the
document and the TypeScript and builds the page. Then the page's tests run. A change of the contract that breaks the
page breaks the pull request. For an application, with its page in `web/`:

```yaml
web:
  runs-on: ubuntu-latest
  steps:
    - uses: actions/checkout@v4
    - uses: actions/setup-node@v4
      with:
        node-version: 22
        cache: npm
        cache-dependency-path: web/package-lock.json
    - uses: dtolnay/rust-toolchain@stable
    - uses: Swatinem/rust-cache@v2
    - run: cargo install --git https://github.com/4thlabs/quoll quoll-cli --locked
    - run: npm ci
      working-directory: web
    - run: cargo quoll build
    - run: npm test
      working-directory: web
```

`npm ci` comes before `cargo quoll build`: the generators that `after` runs are the page's dependencies.
