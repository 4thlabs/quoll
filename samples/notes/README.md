# Notes: a REST server and its client

One contract, `notes_api.hpp`: the types and the endpoints of a small notes API, with no I/O. The server mounts it on a
router and listens; the client calls it from its URL. Each `main()` creates the build's runtime, spawns the program's
coroutine on it and runs it.

```bash
cmake -B build && cmake --build build -j
./build/samples/notes/quoll_notes_server 8080 &
./build/samples/notes/quoll_notes_client http://127.0.0.1:8080
curl 'localhost:8080/api/notes?contains=buy'
curl -i localhost:8080/api/notes/1     # 404, an application/problem+json body
```

`ctest --test-dir build -R notes_round_trip` runs the same session automatically: `round_trip.sh` starts the server
on a free port (`0`, the port it got is in its first line), runs the client against it, checks every line of its
session and stops the server with SIGTERM.

| Endpoint | Request | Response |
|---|---|---|
| `notes::list` | `GET /api/notes?contains=...` | `200`, the notes |
| `notes::get` | `GET /api/notes/{id}` | `200`, the note; `404` problem when there is none |
| `notes::create` | `POST /api/notes`, `{"text": "..."}` | `201`, the note with its id |
| `notes::remove` | `DELETE /api/notes/{id}` | `204` |
