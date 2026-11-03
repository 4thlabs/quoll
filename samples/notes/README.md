# Notes: a REST server and its client

One contract, `notes_api.hpp`: the types and the endpoints of a small notes API, with no I/O. The server mounts it on
a router served over TCP; the client calls it through a keep-alive HTTP transport. Both run on asio.

```bash
cmake -B build && cmake --build build -j
./build/samples/notes/quoll_notes_server 8080 &
./build/samples/notes/quoll_notes_client 127.0.0.1 8080
curl 'localhost:8080/api/notes?contains=buy'
curl -i localhost:8080/api/notes/1     # 404, an application/problem+json body
```

`ctest --test-dir build -R notes_round_trip` runs the same session automatically: `round_trip.sh` starts the server
on a free port (`0`), runs the client against it and stops the server.

| Endpoint | Request | Response |
|---|---|---|
| `notes.list` | `GET /api/notes?contains=...` | `200`, the notes |
| `notes.get` | `GET /api/notes/{id}` | `200`, the note; `404` problem when there is none |
| `notes.create` | `POST /api/notes`, `{"text": "..."}` | `201`, the note with its id |
| `notes.remove` | `DELETE /api/notes/{id}` | `204` |
