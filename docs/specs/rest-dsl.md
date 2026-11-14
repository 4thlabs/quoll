# The REST DSL

> [!WARNING]
> **Draft**, to be validated by the maintainer with [#227](https://github.com/4thlabs/quoll/pull/227).

The spec of `quoll::rest`'s contract, decided with the maintainer in [#176](https://github.com/4thlabs/quoll/issues/176).
How it behaves, for users, is in [`rest.md`](../rest.md); why, in [`design-decisions.md`](../design-decisions.md); what
is left to do, in [#176](https://github.com/4thlabs/quoll/issues/176).

## The principle

The REST DSL is a layer over the HTTP router. The router and a route share one path expression, checked at compile
time: the path with its `{parameters}`, then the query as `key={parameter}`. The REST layer adds a typed contract that
the server and the client share.

```cpp
router.get("/notes?contains={contains}&page={page}", [](std::optional<std::string> contains, std::optional<int> page) {
  ...
});
```

The router and the HTTP client keep a function per method (`get`, `post`...). A REST route, which has no function to
carry its method, carries it in its type.

## A route

A route is its method, its path and query, its result and the types of its parameters, nothing else:

```cpp
namespace notes {

using list   = rest::get<"/notes?contains={contains}&page={page}", std::vector<note>,
                         std::optional<std::string>, std::optional<int>>;
using get    = rest::get<"/notes/{id}", note, std::int64_t>;
using update = rest::put<"/notes/{id}", note, std::int64_t, new_note>;
using create = rest::post<"/notes", rest::created<note>, new_note>;
using remove = rest::del<"/notes/{id}", void, std::int64_t>;
using token  = rest::post<"/token", token_response, rest::form<login>>;
using avatar = rest::get<"/users/{id}/avatar", rest::bytes, std::int64_t>;
using search = rest::get<"/notes/search", std::vector<note>, rest::query_params<note_filter>>;

using routes = rest::group<list, get, update, create, remove, token, avatar, search>;

} // namespace notes

using api = rest::api<"/api", rest::group<rest::security<rest::bearer_auth>, notes::routes>>;
```

- **The method** is the type: `rest::get`, `post`, `put`, `patch`, `del` (`delete` is a keyword), `head`, `options`.
- **The string** holds the whole path below the api's prefix, and the query. A route is unique by its method and path,
  as the router requires: with no name to write, two routes of one string in two groups would otherwise be one C++
  type, one overload of a service, one `call<>`.
- **The result** comes first. Every method answers 200 by default, `void` 204; `rest::created<T>` (201) and
  `rest::accepted<T>` (202) say otherwise, so that a `POST /token` answers 200.
- **The format** is JSON. `rest::form<T>`, `rest::text`, `rest::bytes` and `rest::json<T>` say another, on the body
  or the result; `rest::lenient_json<T>` reads a result whose unknown fields it skips; `rest::encoded<T, Codec>` takes a
  codec of one's own. `rest::events<T>` as a result is a stream of server-sent events.
- **The parameters** follow, in order: one type per `{parameter}` of the string, the path's then the query's (a
  `std::optional` may be missing, a `std::vector` repeats); then the headers, `rest::header<"X-Request-Id", T>`; then
  the body, last, for a `post`, a `put`, a `patch` or a `del` only, as some APIs take one on DELETE (GitHub's
  `DELETE /user/emails`). Nothing else: the parts the route is made of, `body<>`,
  `returns<>`, `query<>`, are Quoll's own, in `detail`.
- **A query** is in the string, or a `rest::query_params<S>` struct for a long list of filters held as one value (a
  component's state), its fields named by Quoll's reflection or the struct's `rest_names` / `rest_fields` (#219); not
  both in one route.
- **The operation's name** is optional: `rest::operation_id<"notes.get">`, OpenAPI's word, else its method and whole
  path, `GET /api/notes/{id}`. `rest::name<"...", &S::field>` already renames fields.
- **Errors**: a route declares none. A handler returns its result or throws an `http::http_error`, answered as an RFC
  9457 problem.

## Groups and the api

```cpp
namespace users {

using list   = rest::get<"/users", std::vector<user>>;
using get    = rest::get<"/users/{id}", user, std::int64_t>;
using remove = rest::del<"/users/{id}", void, std::int64_t>;

using routes = rest::group<rest::security<rest::bearer_auth>, list, get, remove>;

} // namespace users

using api = rest::api<"/api", users::routes, notes::routes>;
```

- **`rest::group<options..., routes or groups...>`** has no prefix. It carries what its routes share, a security
  scheme or a header, and nests. It replaces `rest::with<>`. A group's header comes after a route's own parameters,
  last.
- **Only `rest::api<>` has a prefix**, `"/api"` or a version, with no parameter: a parameter its routes share goes in
  each one's path, `/repos/{owner}/{repo}/issues`.

## The server

A route's handler is a lambda taking the route first, then the route's parameters. A service gathers them, defined
once and mounted:

```cpp
auto const service = rest::service(
    [&store](notes::get, std::int64_t id) -> async::task<note> {
      co_return co_await store.find(id);
    },
    [&store](notes::create, new_note made) {
      return store.add(std::move(made));
    });

rest::mount<api>(server, service);
```

A struct with an `operator()(Route, parameters...)` for each of its routes serves the same way, and `mount` takes
several, mixed. Each route of the api has one handler, of its parameters exactly: a route with no handler, two handlers,
or a handler of a route of another api does not compile. A handler returns its result, as is or in a task.

## The client

```cpp
note const item = co_await client.call<notes::get>(3);                  // by position, the string's order
auto const page = co_await client.call<notes::list>("page"_p = 2);      // by name, optionals left out
auto const found = co_await client.call<notes::search>({.contains = "milk"});
auto const answer = co_await client.try_call<notes::get>(3);            // std::expected<note, http::http_error>

auto const issues = co_await client.call<github::repos::issues>("4thlabs", "quoll", {.state = "open"});
```

By position or by name, not both in one call. A route's parameters are passed at each call: no scope fixes them.

## One HTTP error

`http::http_error` is the one error of the server and of both clients: its status, the server's RFC 9457 problem when it
sent one, the raw body and `body_as<T>(codec)` for a third-party API's own error shape, `std::optional<T>`, and
whether it was `received()` from a response. The `Codec` concept is `quoll::http`'s, so that it reads one. The HTTP client raises it on demand (`ensure_ok()`), the REST client raises it or returns
it. A server answers its status to one its handler throws, and 502 to one received from another call, which must not
pass the other API's status on as its own.

## What does not compile

A `{parameter}` with no type, or a type in the string (`{id:int64}`); a path parameter whose type is not read from
text, a query parameter's neither; a query in the expression and a `query_params<>` both; a `{name:path}` that is no
`std::string`; a body on a method other than `post`, `put`,
`patch` and `del`, a second body, or one before a header; a part of a route that is no header, `query_params<>`, security or
`operation_id<>`; a result on `head`; two `operation_id`; an api's prefix with a parameter, a member of an api that is
no route or group, of a group none of these or security or a header; two routes of one method and path; a route with
no handler, or two; a lambda of a route of another api, or two lambdas of one route in a service. Each says so in
Quoll's words. A generic lambda, or a struct of several `operator()`, escapes the check of a route of another api: its
call operators cannot be listed.
