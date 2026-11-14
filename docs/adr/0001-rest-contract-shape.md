# ADR 0001: the shape of a REST contract and of its service

- **Status:** proposed, waiting for the maintainer's decision, the REST contract's shape to be redesigned with him.
  Settled since, by [#182](https://github.com/4thlabs/quoll/issues/182) (2026-11-12): option D's sub-choice, its
  members found through Glaze or without, is without, through a reflection of Quoll's own, as Glaze serves JSON only;
  its first step reads the fields of `query_params<>` and of a form ([#201](https://github.com/4thlabs/quoll/issues/201)).
  The errors, [#176](https://github.com/4thlabs/quoll/issues/176), stay open.
- **Date:** 2026-11-12
- **Related:** [#176](https://github.com/4thlabs/quoll/issues/176) (errors declared once per API),
  [#182](https://github.com/4thlabs/quoll/issues/182) (Glaze for JSON only)

## Context

A contract of `quoll::rest` is a set of types: one `endpoint<"name", verb, "/path", parts...>` alias per operation,
gathered in `api<>`. A service is an overload set, one `operator()(Endpoint, arguments...)` per endpoint, and a client
calls by type:

```cpp
namespace users {
using namespace quoll::rest;

using list = endpoint<"users.list", verb::get, "/", query_params<user_filter>, returns<std::vector<user>>>;
using get = endpoint<"users.get", verb::get, "/{id:int64}", returns<user>, returns<not_found, status::not_found>>;
using create = endpoint<"users.create", verb::post, "/", body<new_user>, returns<user, status::created>>;
using remove = endpoint<"users.remove", verb::delete_, "/{id:int64}">;

using api = rest::api<"/users", list, get, create, remove>;
} // namespace users

struct users_service {
  std::vector<user> operator()(users::list, user_filter filter) const;
  async::task<rest::reply<users::get>> operator()(users::get, std::int64_t id) const;
  user operator()(users::create, new_user const &created) const;
  void operator()(users::remove, std::int64_t id) const;
};

rest::mount<users::api>(router, users_service{});
user const found = co_await client.call<users::get>(42);
```

Two things make a contract heavy:

- **Error responses repeated on every endpoint.** The GitHub sample declares two `returns<error, status>` on each
  endpoint.
- **The name to write**, `"users.get"`. It is what tells apart two endpoints with the same parts, and the route's name in
  the router, which a middleware reads through `next.route()`. A form that drops it changes that name, which such a
  middleware compares as a string: the change is silent.

The method (`verb::get`) is not a problem.

Three choices follow, independent of each other: the errors, the shape of the contract, the shape of the service. Every
example describes the contract `users` of `docs/rest.md`, without its declared 404 for brevity: its body is read by the
caller, so it stays a `returns<not_found, status::not_found>` on `find` in every form. None is
implemented: they are sketches, whose points to check are listed.

Every form but today's names endpoints after their method, `get`, `post`, `delete_`, in `quoll::rest`, which a contract
brings in with `using namespace quoll::rest`, in its own namespace:

- **An endpoint named after a method** hides it: `struct get : endpoint<get(...)>` and
  `inline constexpr auto get = get(...)` refer to themselves, and a member `get<...> get;` changes the name's meaning in
  its class. In A, `using get = get<...>;` compiles, but every later `get<...>` in that namespace finds the alias. The
  guide's `users::get` becomes `users::find` in every example below.
- **Where they are class or alias templates**, as in A and D, an unqualified `get<0>(pair)` in that namespace finds
  `rest::get`, which stops argument-dependent lookup from finding `std::get`, and no longer compiles. In B, C and E
  they may be functions or function templates, which leave argument-dependent lookup to find `std::get`; not function
  objects (`inline constexpr get_fn get{}`), a variable, which stops it as a template does.

Other names (`http_get`), or qualified ones, avoid both.

## Errors

No error is declared by default, which already works today. An undeclared status throws `rest::call_error`, with its
`status()`, the whole response and the `problem`'s detail; a service throws `rest::http_error` without declaring
anything. Servant types only success, and Go's net/http types nothing; tRPC types one error shape per router, through
its `errorFormatter`, which is what `errors<T>` would be.

A `with<>` can already declare a response for a group, `with<returns<error, status::not_found>, ...>`, but for the
server's own answers (credentials): a service cannot answer it, and the client does not decode it, as `reply<Endpoint>`
holds only the endpoint's own responses, so `try_call` gives an `unexpected_response`. Today, an error a service answers
and a caller reads is declared on each endpoint, which is why the GitHub sample repeats them.

What [#176](https://github.com/4thlabs/quoll/issues/176) adds:

- **`errors<T>`** in a `with<>`: the body of every 4xx and 5xx response not otherwise declared, as OpenAPI's `4XX` and
  `5XX` keys (not `default`, which also covers 2xx and 3xx), for an API with its own format, as GitHub's. Unlike a
  `returns<>` in a `with<>` today, a service answers it and the client decodes it.
- **`errors<T, Statuses...>`**: the same, for some statuses only.
- **`codec<C>`** in a `with<>`: the codec of a group, so that the GitHub sample needs no `answers` alias. Its name is to
  choose: `codec` is already a member alias of `body<>`, `returns<>` and `events<>`, and `Codec` the concept.
- **`returns<T, Status>`** stays, on its endpoint, for an error whose body the caller uses: a 409 with the current
  version, a 422 with the invalid fields.

The GitHub sample, before:

```cpp
template <class T, http::status Status = http::status::ok>
using answers = rest::returns<T, Status, rest::lenient_json_codec>;

using get = rest::endpoint<"repos.get", http::verb::get, "/", answers<repository>,
                           answers<error, http::status::not_found>, answers<error, http::status::forbidden>>;
using issues = rest::endpoint<"repos.issues", http::verb::get, "/issues", rest::query_params<issue_filter>,
                              answers<std::vector<issue>>, answers<error, http::status::not_found>,
                              answers<error, http::status::forbidden>>;
using me = rest::endpoint<"users.me", http::verb::get, "/user", rest::security<rest::bearer_auth>, answers<github::me>,
                          answers<error, http::status::unauthorized>, answers<error, http::status::forbidden>>;
```

After, with #176, in the same namespaces:

```cpp
namespace repos {
using get = rest::endpoint<"repos.get", http::verb::get, "/", rest::returns<repository>>;
using issues = rest::endpoint<"repos.issues", http::verb::get, "/issues", rest::query_params<issue_filter>,
                              rest::returns<std::vector<issue>>>;

using api = rest::api<"/repos/{owner}/{repo}", get, issues>;
} // namespace repos

namespace users {
using me = rest::endpoint<"users.me", http::verb::get, "/user", rest::security<rest::bearer_auth>,
                          rest::returns<github::me>>;
} // namespace users

using api = rest::api<"", rest::with<rest::codec<rest::lenient_json_codec>, rest::errors<error>, repos::api, users::me>>;
```

Left to the issue:

- how `call_error`, `reply<>` and `try_call` give the decoded body and its status;
- how a service answers with an `errors<T>` body, which must say its status;
- Quoll's own answers, which `errors<T>` must leave out: what the server, its router and its connections answer by
  themselves, a `rest::problem` or no body at all, for many 4xx and 5xx statuses. For instance, `errors<T>` decodes only
  a body of its codec's media type, and leaves the others to `call_error` with their status;
- `codec<C>` needs `returns<>` and `body<>` to tell an inherited codec from a chosen one: their default, `json_codec`,
  would become a placeholder resolved by the group, which changes their types and what reads their codec; or request
  bodies keep their own codec, which the issue must say.

## Contract options

### A: today's, lighter

The model stays: an alias per method replaces `endpoint<..., verb::get, ...>`, and each part goes on its own line.

```cpp
namespace users {
using namespace quoll::rest;

using list = get<"users.list", "/",
                 query_params<user_filter>,
                 returns<std::vector<user>>>;

using find = get<"users.get", "/{id:int64}",
                 returns<user>>;

using create = post<"users.create", "/",
                    body<new_user>,
                    returns<user, status::created>>;

using remove = delete_<"users.remove", "/{id:int64}">;

using api = rest::api<"/users", list, find, create, remove>;
} // namespace users
```

- **Service and client:** unchanged, `operator()(users::find, std::int64_t id)` and `client.call<users::find>(42)`.
- **For:** nothing else changes, and it can be done now.
- **Against:** the name stays, and the aliases take `get` (see Context). The method was no problem, so the gain is
  small.

### B: a struct per endpoint

The struct gives the identity, so no name is needed. The chain of operators is a template argument, hence a constant,
from which `endpoint<>` reads the path and the types of its parameters.

```cpp
namespace users {
using namespace quoll::rest;

struct list : endpoint<get("/") | query_params<user_filter> | returns<std::vector<user>>> {};
struct find : endpoint<get("/{id:int64}") | returns<user>> {};
struct create : endpoint<post("/") | body<new_user> | returns<user, status::created>> {};
struct remove : endpoint<delete_("/{id:int64}")> {};

using api = rest::api<"/users", list, find, create, remove>;
} // namespace users
```

- **Service and client:** unchanged, `operator()(users::find, std::int64_t id)` and `client.call<users::find>(42)`.
- **Route name:** from the method and the path (`GET /users/{id:int64}`), already unique in an API. A
  `| named("users.get")` may still choose it, for the operationId of the later OpenAPI output.
- **For:** drops the name, and a service and a client are written as today.
- **Against:** a struct per line, and `endpoint<...> {}` around each endpoint.
- **Parts become values:** `|` composes expressions, so `query_params<user_filter>`, `body<new_user>` and
  `returns<user>` become variable templates. They cannot share a namespace with today's class templates of the same
  names, so both forms cannot live side by side during a migration without other names.
- **The internals:** they read the endpoint's static members, which a derived struct inherits, but its name goes: the
  route's name, made from the full path, and the check that names are unique move from the endpoint to the route.
- **Without operators**, `struct find : endpoint<verb::get, "/{id:int64}", returns<user>> {};` works too, but under
  another template name: one class template cannot take both a chain and a method with a path.

### C: operators on variables

Each endpoint is a value composed with `|`. The identity is the variable: an `auto const &` template parameter binds to
the variable itself, so a `users::remove` and a `cards::remove` differ even with the same type. Two spellings, one mechanism.

In a namespace:

```cpp
namespace users {
using namespace quoll::rest;

inline constexpr auto list = get("/") | query_params<user_filter> | returns<std::vector<user>>;
inline constexpr auto find = get("/{id:int64}") | returns<user>;
inline constexpr auto create = post("/") | body<new_user> | returns<user, status::created>;
inline constexpr auto remove = delete_("/{id:int64}");

using api = rest::api<"/users", list, find, create, remove>; // api<fixed_string Prefix, auto const &... Endpoints>
} // namespace users
```

In a struct, as one block:

```cpp
namespace contract {
using namespace quoll::rest; // a struct cannot hold it: in the contract's own namespace, never at global scope

struct users {
  static constexpr auto list = get("/") | query_params<user_filter> | returns<std::vector<user>>;
  static constexpr auto find = get("/{id:int64}") | returns<user>;
  static constexpr auto create = post("/") | body<new_user> | returns<user, status::created>;
  static constexpr auto remove = delete_("/{id:int64}");

  using api = rest::api<"/users", list, find, create, remove>;
};
} // namespace contract
```

Use, in `namespace contract` too:

```cpp
user const found = co_await client.call<users::find>(42);

struct users_service {
  user operator()(rest::op<users::find>, std::int64_t id) const; // template <auto const &E> struct op {};
};
```

- **For:** one part after the other, left to right, and an error points at the `|` that is wrong. The client reads as
  today.
- **Against:** `static constexpr auto` or `inline constexpr auto` on every line, which C++23 has no way to avoid. The
  service writes `rest::op<users::find>`. No name in C++23: from the method and the path, or `| named(...)`. The
  endpoints are listed again in `api`. Parts become values, as in B.
- **Groups:** a template parameter pack holds types or values, not both, so `api<"/users", list, find>` cannot also hold
  a nested `api<>` or a `with<>`. Every `api` and `with` then becomes a value, a variable rather than the `using api`
  above, whose shape this sketch does not settle: C rewrites all that groups.
- **To avoid:** `constexpr` without `inline` at namespace scope, which gives each translation unit its own copy (ODR).
  The endpoints need external linkage.

### D: members of a struct (recommended)

A resource is a struct, each endpoint a member whose type is its method. The shortest form: no `endpoint`, no name to
write.

```cpp
namespace contract {
using namespace quoll::rest; // a struct cannot hold it: in the contract's own namespace, never at global scope

struct users {
  get<"/", query_params<user_filter>, returns<std::vector<user>>> list;
  get<"/{id:int64}", returns<user>> find;
  post<"/", body<new_user>, returns<user, status::created>> create;
  delete_<"/{id:int64}"> remove;
};

struct cards {
  get<"/", returns<std::vector<std::string>>> list;
};

using v1 = rest::api<"/api/v1", with<security<bearer_auth>, rest::api<"/users", users>,
                                               rest::api<"/users/{user_id:int64}/cards", cards>>>;
} // namespace contract
```

Use, in `namespace contract` too:

```cpp
user const found = co_await client.call<&users::find>(42);

struct users_service {
  user operator()(rest::op<&users::find>, std::int64_t id) const; // template <auto Member> struct op {};
};
```

- **Identity:** the member pointer. `&users::list` and `&cards::list` differ, and its type is enough to find the
  member: `api<>` already refuses two endpoints with one method and path, so each member of a resource has its own
  type, and `decltype(&users::find)` names it.
- **Finding the members:** `api<>` counts and ties the members of the aggregate, in one of two ways, to decide:
    - **through Glaze**, as the contract layer already does: `detail/route.hpp` reads a `query_params` struct with
      `glz::reflect`, `detail/fields_of.hpp` ties it with `glz::to_tie`, and `design-decisions.md` says Glaze's
      reflection finds the members of a contract's types. It also gives the members' names
      (`glz::reflect<users>::keys`) and of the struct (`glz::name_v<users>`), for the route names and the later
      operationId: `users.list` and `cards.list`, as a member's name alone repeats across resources;
    - **without Glaze**, as Boost.PFR does, by probing the aggregate's initialization, then through structured bindings:
      the shape of a contract would not depend on the JSON library, at the price of a second enumeration beside Glaze's,
      and of route names from the method and the path.
- **For:** a resource in one block, read as an interface. No `static constexpr auto`.
- **Against:** commas instead of `|`, as a non-static member cannot be `auto`. The client writes `&users::find` and the
  service `rest::op<&users::find>`. Every template of `quoll::rest` keyed on an endpoint type is keyed on a member
  pointer instead, among them the client's calls, `reply<>`, `result<>` and the routes, and `mount` finds a handler
  through `rest::op`.
  A response other than the first reads `rest::reply<&users::find>`.
- **To check:** the enumeration on GCC 13, Clang 19, Apple Clang 21 and Emscripten. One struct mounted under two
  prefixes would be ambiguous, so refused at compile time.

### E: C++26, reflection and annotations (later)

The contract is a plain C++ interface, described by annotations; reflection reads the names of its functions and of
their parameters.

```cpp
namespace users {
using namespace quoll::rest;

struct [[=prefix("/users")]] api {
  [[=get("/")]]
  std::vector<user> list(user_filter filter);

  [[=get("/{id:int64}")]]
  user find(std::int64_t id); // not `get`: the annotation's name would change meaning in the class

  [[=post("/"), =status(status::created)]]
  user create(new_user created);

  [[=delete_("/{id:int64}")]]
  void remove(std::int64_t id);
};
} // namespace users

rest::mount<users::api>(server, users_service{}); // matched by name
user const found = co_await client.call<^^users::api::find>(42);
```

Its annotations meet the clashes of Context, and one more: a `rest::status(...)` annotation would hide `http::status`
in the contract's namespace, so it needs another name.

The most readable form, but only GCC 16 has what it needs; Clang, Apple Clang and Emscripten, which Quoll targets too,
do not:

| Compiler       | Reflection (P2996)             | Annotations (P3394)        | Parameters (P3096) |
| -------------- | ------------------------------ | -------------------------- | ------------------ |
| GCC 16.1       | yes, `-std=c++26 -freflection` | yes, partial per wro.cpp   | yes                |
| Clang upstream | no                             | no                         | no                 |
| Apple Clang    | no (follows Clang)             | no                         | no                 |
| Emscripten     | no (upstream Clang)            | no                         | no                 |

Bloomberg's fork, clang-p2996, has both with `-freflection-latest`, but neither emsdk nor Apple Clang ships it. This form
waits for upstream Clang, then Apple Clang and emsdk. Sources, as of 2026-11-12:
[GCC 16 changes](https://gcc.gnu.org/gcc-16/changes.html), [Clang C++ status](https://clang.llvm.org/cxx_status.html),
[cppreference](https://en.cppreference.com/w/cpp/compiler_support/26),
[wro.cpp](https://wrocpp.github.io/toolset/compiler-support/), [clang-p2996](https://github.com/bloomberg/clang-p2996).

## Service options

`mount` already takes several services, and a lambda with `operator()(Endpoint, ...)` is one: "both" exists today.

```cpp
struct users_reads {
  std::shared_ptr<database> db;

  std::vector<user> operator()(users::list, user_filter filter) const;
  async::task<user> operator()(users::find, std::int64_t id) const;
};

rest::mount<users::api>(router, users_reads{db},
                        [](users::create, new_user const &created) { return insert(created); },
                        [](users::remove, std::int64_t id) { erase(id); });
```

What is left to choose is only how a handler names its endpoint: by a tag parameter, as above, or by a builder. With
contract D, the tag is `rest::op<&users::find>`; the builder takes the member pointer as a template argument, as
`call<&users::find>` does:

```cpp
// in namespace contract, as D's uses
auto const service = rest::service<users>()
    .on<&users::list>([](user_filter filter) { return find_users(filter); })
    .on<&users::find>([](std::int64_t id) -> async::task<user> { co_return co_await load(id); })
    .on<&users::create>([](new_user const &created) { return insert(created); })
    .on<&users::remove>([](std::int64_t id) { erase(id); });

rest::mount<rest::api<"/users", users>>(router, service);
```

- **What the builder adds:** handlers without the tag parameter, which matters most with D, where the tag is
  `rest::op<&users::find>`. With today's contract the tag is `users::find`, and the builder (`.on<users::find>(...)`)
  adds little.
- **Checked at compile time**, as `mount` does today: an endpoint without a handler, a handler given twice, parameters
  that are not the endpoint's.
- **The builder owns its handlers**, as a service does today: copied or moved into the router's storage, as it is
  passed.
- **A coroutine lambda writes its return type** (`-> async::task<user>`): a coroutine cannot deduce it.
- **Cost:** some tens of lines over `mount`, which can come before the contract's shape is decided, or not at all if
  the tag parameter is kept.

## Comparison

D is the shortest form that holds in C++23. B and C keep `|`, their parts becoming values: B at the price of a struct and an
`endpoint<...> {}` per endpoint, C of a `static constexpr auto` per line.

| Form | An endpoint | Name written | Client | Service | What changes |
| --- | --- | --- | --- | --- | --- |
| Today | `using find = endpoint<"users.get", verb::get, "/{id:int64}", returns<user>>;` | yes | `call<users::find>` | `operator()(users::find, ...)` | nothing |
| A, lighter | `using find = get<"users.get", "/{id:int64}", returns<user>>;` | yes | `call<users::find>` | `operator()(users::find, ...)` | aliases |
| B, struct per endpoint | `struct find : endpoint<get("/{id:int64}") \| returns<user>> {};` | no | `call<users::find>` | `operator()(users::find, ...)` | `endpoint<>`, the name moves to the route |
| C, operators | `static constexpr auto find = get("/{id:int64}") \| returns<user>;` | no | `call<users::find>` | `operator()(rest::op<users::find>, ...)` | every `api` and `with` becomes a value, and every template keyed on an endpoint type |
| D, members | `get<"/{id:int64}", returns<user>> find;` | no | `call<&users::find>` | `operator()(rest::op<&users::find>, ...)` | every template keyed on an endpoint type |
| E, C++26 | `[[=get("/{id:int64}")]] user find(std::int64_t id);` | no | `call<^^users::api::find>` | a function of the same name | everything, and reflection everywhere |

## Decision

To be taken by the maintainer:

- [ ] Errors: `errors<T>` per API and `codec<C>` per group (#176).
- [ ] The contract's shape: A, B, C or D, keeping E for later; for D, its members found through Glaze or without.
- [ ] The service: a builder beside the tag parameter, or the tag alone.
- [ ] The order: #176 first, independent of the contract, then a prototype of the chosen shape on the notes and GitHub
      samples, with the builder if it is kept.

## Consequences

Written once decided: what changes in `quoll::rest`, in `docs/rest.md` and `design-decisions.md`, and in the samples.
