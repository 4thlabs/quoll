# Notes: one contract, three programs

A small notes API, described once in C++ and used by a server, a command line and a page in a browser.

```
notes/
├── api/        # the contract: the types and the routes, no I/O
├── server/     # serves the contract over HTTP, the notes in memory
├── cli/        # calls it from the command line
├── web/        # calls it from a page in C++ compiled to WASM: see web/README.md
└── tests/      # the session of the command line against the server
```

- **`api/`:** `note.hpp` holds the types, `notes_api.hpp` the routes (`rest::get`, `rest::post`... and `rest::api`). The
  server, the command line and the page include it: a call that does not match the contract does not compile.
- **`server/`:** `service.hpp` handles each route; `main.cpp` mounts it on an `http::server`, listens, and prints its
  address once it is ready. Ctrl-C or SIGTERM stops it.
- **`cli/`:** `main.cpp` calls each route through a `rest::client` built from the server's URL, and prints what it gets.
- **`web/`:** the same calls from a page, with its components and its build by Vite.

Each `main()` creates the build's runtime and runs the program's coroutine on it.

## Run it

```bash
cmake -B build && cmake --build build -j
./build/samples/notes/quoll_notes_server 8080 &
./build/samples/notes/quoll_notes_cli http://127.0.0.1:8080
curl 'localhost:8080/api/notes?contains=buy'
curl -i localhost:8080/api/notes/1     # 404, an application/problem+json body
```

`ctest --test-dir build -R notes_round_trip` runs the same session automatically: `tests/round_trip.sh` starts the server
on a free port (`0`, the port it got is in its first line), runs the command line against it, checks every line of its
session and stops the server with SIGTERM.

The page is built under Emscripten and served by Vite: see [`web/README.md`](web/README.md).

## The API

| Endpoint | Request | Response |
|---|---|---|
| `notes::list` | `GET /api/notes?contains=...` | `200`, the notes |
| `notes::get` | `GET /api/notes/{id}` | `200`, the note; `404` problem when there is none |
| `notes::create` | `POST /api/notes`, `{"text": "..."}` | `201`, the note with its id |
| `notes::remove` | `DELETE /api/notes/{id}` | `204` |
