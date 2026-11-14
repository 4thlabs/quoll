# `samples/github`

A command-line program calling [GitHub's REST API](https://docs.github.com/rest) through a contract of a few of its
routes, natively and, compiled to WebAssembly, in Node through `fetch`.

```bash
quoll_github 4thlabs/quoll            # the repository and its open issues
GITHUB_TOKEN=... quoll_github me      # the account of the token
```

What it shows, in [`github.cpp`](github.cpp) and [`github_api.hpp`](github_api.hpp):

- **A contract of someone else's API**: GitHub's paths and names (`stargazers_count` read as `stars`, through
  `glz::meta`), its fields as they come (`rest::lenient_json<T>`), and its shape of errors, read from the error a call
  gives (`error.body_as<github::error>(rest::lenient_json_codec{})`).
- **Interceptors**: the `User-Agent` and API version GitHub asks every request for, and `rest::retry`.
- **`call` and `try_call`**, and a **credential**, `GITHUB_TOKEN` as a bearer token.

It calls `https://api.github.com`, or the URL in `QUOLL_GITHUB_URL`. Its test runs it against
[`fake_github.cpp`](fake_github.cpp), the same contract served by Quoll, rather than GitHub, whose limits a CI would
meet: `ctest -R github_against_fake`.

Under Emscripten, in Node:

```bash
emcmake cmake -B build-wasm && cmake --build build-wasm --target quoll_github
node build-wasm/samples/github/quoll_github.js 4thlabs/quoll
```

[`node_env.js`](node_env.js) hands the program `GITHUB_TOKEN` and `QUOLL_GITHUB_URL` from Node's environment, as
Emscripten keeps one of its own.
