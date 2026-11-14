# `samples/docker`

A command-line program calling the [Docker Engine API](https://docs.docker.com/reference/api/engine/) through a contract
of a few of its routes, over the engine's Unix domain socket: the test of the REST DSL against an API it was not made
for.

```bash
quoll_docker version
quoll_docker ps -a -f status=exited
quoll_docker run alpine echo hi       # create, start, wait, logs, rm: its exit code
quoll_docker events 1700000000        # what happened since then, up to now
```

What it shows, in [`docker.cpp`](docker.cpp) and [`docker_api.hpp`](docker_api.hpp):

- **A Unix domain socket**: `rest::client<docker::api>{"unix:///var/run/docker.sock"}`, or `DOCKER_HOST`, as Docker's own
  client reads it (`tcp://host:2375` too).
- **Docker's names**, in PascalCase through `glz::meta`, its fields as they come (`rest::lenient_json<T>`), its errors
  in its own shape (`error.body_as<docker::error>(rest::lenient_json_codec{})`).
- **A query parameter in JSON**, Docker's `filters`, through a `rest::param_traits` of the sample's own.
- **Calls by name**, `"all"_p = true`.
- **Streams that are no server-sent events**: the events, a value per line of JSON (`rest::json_lines<T>`), and the
  logs, Docker's frames of 8 bytes of head, through a `rest::Framing` of the sample's own (`rest::stream<F>`).

Its test runs it against [`fake_docker.cpp`](fake_docker.cpp), the same contract served by Quoll on a socket of its own:
`ctest -R docker_against_fake`.
