# A command-line program

A program that calls an API, with no server and no interface: what a REST contract and a runtime do alone.
[`samples/github`](../samples/github) is one, calling GitHub's API; what follows is how it is put together.

## The contract

A contract of someone else's API says what that API does: its names, its errors, its fields as they come.
See [Third-party APIs](rest.md#third-party-apis) for what it can say.

```cpp
namespace github {

struct repository {
  std::string full_name;
  std::int64_t stars = 0;   // stargazers_count, through glz::meta
};

struct error {
  std::string message;
};

template <class T, http::status Status = http::status::ok>
using answers = rest::returns<T, Status, rest::lenient_json_codec>;

namespace repos {
using get = rest::endpoint<"repos.get", http::verb::get, "/", answers<repository>,
                           answers<error, http::status::not_found>>;
using api = rest::api<"/repos/{owner}/{repo}", get>;
} // namespace repos

using api = rest::api<"", repos::api>;

} // namespace github
```

`rest::lenient_json_codec` skips the fields the contract does not read, as an API adds some over time. An error the API
declares on each status is a type of its own, read through `try_call`.

## The program

`main` builds the runtime, reads what the program needs from its arguments and its environment, and spawns one task,
which owns the client. The runtime runs until that task has ended:

```cpp
async::task<int> run(std::string url, std::optional<std::string> token) {
  rest::client<github::api> client{std::move(url)};

  client.use(as_github_asks);                  // the User-Agent GitHub asks every request for
  client.use(rest::retry{.attempts = 3});      // a 502, 503 or 504 sent again

  if (token) {
    client.credentials<rest::bearer_auth>(*token);
  }

  auto const quoll = client.scope<github::repos::api>("4thlabs", "quoll");
  auto const found = co_await quoll.try_call<github::repos::get>();

  if (!found || !found->holds<http::status::ok>()) {
    co_return 1;
  }

  std::cout << found->get<http::status::ok>().stars << '\n';
  co_return 0;
}

int main() {
  quoll::runtime runtime;

  co_spawn(runtime.get_executor(), run("https://api.github.com", std::nullopt), record_status{});
  runtime.run();
  return exit_status;
}
```

- **A client is copied freely**: its transport and interceptors are shared. A coroutine takes it by value rather than
  by reference, and a scope keeps a copy.
- **The environment is read in `main`**, before the runtime runs anything, and handed to the task.
- **The exit status** comes from the task, through the handler `co_spawn` calls at its end: a value it returns, or the
  exception it threw.

## Natively and in Node

Natively, `quoll::runtime` connects over TCP and TLS through asio. Compiled with Emscripten, the same program runs in
Node, and `http::client` goes through `fetch`:

```bash
emcmake cmake -B build-wasm && cmake --build build-wasm --target quoll_github
node build-wasm/samples/github/quoll_github.js 4thlabs/quoll
```

Two things differ there, as `main` returns before the task ends and Node's loop runs it:

- **The environment** is Emscripten's own: a `--pre-js` file hands over the variables the program reads, and only those
  ([`node_env.js`](../samples/github/node_env.js)).
- **The exit status** is set on Node's `process.exitCode` when the task ends, rather than returned by `main`.

## Testing it

A program calling a public API should not test against it: its limits, its data and its availability are not the
test's. The sample's test serves the same contract with Quoll, answering what the program has to cope with (an error, a
retry, a refused token), and runs the program against it, natively ([`against_fake.sh`](../samples/github/against_fake.sh))
and in Node through `fetch` ([`against_fake.mjs`](../samples/github/against_fake.mjs)).
