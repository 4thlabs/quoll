# REST

An API described once. A contract is a set of C++ types, with no I/O: a server implements it, a client
calls it, and neither needs the other. Link `quoll::rest`, with `quoll::server` and `quoll::runtime` to
serve, and `quoll::runtime` to call.

The examples here assume:

```cpp
namespace async = quoll::async;
namespace http = quoll::http;
namespace rest = quoll::rest;
using http::status;
using http::verb;
```

## The contract

Bodies are plain aggregates, whose fields Glaze reflects, with no macro and no declaration. One alias
per operation, gathered in an `api`:

```cpp
struct user {
  std::int64_t id = 0;
  std::string name;
  std::optional<std::string> email;
};

struct new_user {
  std::string name;
  std::string email;
};

struct user_filter {
  std::optional<int> limit{};
  std::optional<std::string> name{};
};

struct not_found {
  std::string detail;
};

namespace users {
using namespace quoll::rest;

using list = endpoint<"users.list", verb::get, "/", query_params<user_filter>, returns<std::vector<user>>>;
using get = endpoint<"users.get", verb::get, "/{id:int64}", returns<user>, returns<not_found, status::not_found>>;
using create = endpoint<"users.create", verb::post, "/", body<new_user>, returns<user, status::created>>;
using remove = endpoint<"users.remove", verb::delete_, "/{id:int64}">;

using api = rest::api<"/users", list, get, create, remove>;
} // namespace users
```

> [!NOTE]
> Reflected types need external linkage: Clang rejects them in an anonymous namespace.

An `endpoint` takes a name, unique in its API, a method, a path template, then its parts.

### Paths

A parameter takes a whole segment and says its type, `string` by default:

| Type in the path          | Argument                            |
| ------------------------- | ----------------------------------- |
| `{id}`, `{id:string}`     | `std::string`                       |
| `{n:int}`, `{n:uint}`     | `int`, `unsigned`                   |
| `{n:int64}`, `{n:uint64}` | `std::int64_t`, `std::uint64_t`     |
| `{x:double}`              | `double`                            |
| `{on:bool}`               | `bool`                              |
| `{rest:path}`, last       | `std::string`, the rest of the path |

What does not hold fails to compile, with a message that says so. These are some of the checks:

| What is wrong                          | What the compiler says                                                            |
| -------------------------------------- | --------------------------------------------------------------------------------- |
| An unknown type: `{id:long}`           | `quoll::rest: unknown path parameter type`                                        |
| Two endpoints of an API with one name  | `quoll::rest::api: two endpoints have the same name`                              |
| Two endpoints with one method and path | `quoll::rest::api: two endpoints have the same method and path`                   |
| A `body<>` on GET or HEAD              | `quoll::rest::endpoint: a GET or HEAD endpoint has no body<> (fetch rejects one)` |
| A response with a body to HEAD         | `quoll::rest::endpoint: a response to HEAD has no body`                           |
| A body on 204, 205 or 304              | `quoll::rest::returns: a 204, 205 or 304 has no body`                             |
| A space or a `?` in a literal          | `quoll::rest: a path holds a character RFC 3986 does not allow in a segment`      |
| Two `returns<>` with one status        | `quoll::rest::endpoint: two returns<> have the same status`                       |

### Parts

| Part                        | What it is                                                                                                                           |
| --------------------------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| `query<"name", T>`          | A query parameter. `T` is required, `std::optional<T>` may be left out, `std::vector<T>` repeats it (`?tag=a&tag=b`).                |
| `query_params<Struct>`      | One query parameter per field of an aggregate, by the same rules: for many optional parameters, filled with designated initializers. |
| `header<"Name", T>`         | A request header field, by the same rules.                                                                                           |
| `body<T, Codec>`            | The request body, at most one, none on GET, HEAD or TRACE. JSON by default. CONNECT is no operation of an API.                       |
| `returns<T, Status, Codec>` | A declared response, one per status from 200 to 599. 200 and JSON by default, `void` for no body, as on 204, 205, 304 and HEAD. Without any, the endpoint answers 204. |
| `security<Schemes...>`      | The security schemes required: `api_key<"Header">`, `api_key_query<"param">` or `bearer_auth`. They are not arguments.               |

### Arguments

The arguments of an endpoint are the same on both sides, in this order: its path parameters, prefixes
first, then its argument parts in order, then those inherited from `with<>`. The endpoint computes their
types, so that a braced list converts: `{.limit = 20}` for a `query_params`.

### Grouping

`api<"/prefix", ...>` nests. A prefix may have parameters, which come first in the arguments, and an
endpoint path of `/` is the prefix itself. `with<parts..., apis...>` adds parts to every endpoint it
wraps:

```cpp
namespace cards {
using namespace quoll::rest;

using list = endpoint<"cards.list", verb::get, "/", returns<std::vector<std::string>>>;

using api = rest::api<"/users/{user_id:int64}/cards", list>;
} // namespace cards

using v1 = rest::api<"/api/v1", rest::with<rest::security<rest::bearer_auth>, users::api, cards::api>>;
```

`users::get` is now `GET /api/v1/users/{id:int64}` and needs a bearer token. `cards::list` takes the
`user_id` of its prefix as its first argument.

## Serving: `mount`

A service is an overload set: one `operator()(Endpoint, arguments...)` per endpoint, with no base class.

```cpp
struct users_service {
  // The body of the first returns<>, in a task or not.
  std::vector<user> operator()(users::list, user_filter filter) const {
    return {{.id = 1, .name = filter.name.value_or("Ada"), .email = std::nullopt}};
  }

  // Any declared response: a reply<>, built from a value whose type only one response has.
  async::task<rest::reply<users::get>> operator()(users::get, std::int64_t id) const {
    if (id == 42) {
      co_return user{.id = 42, .name = "Ada", .email = std::nullopt};
    }

    co_return not_found{"no user " + std::to_string(id)};
  }

  // An error the endpoint does not declare.
  user operator()(users::create, new_user const &created) const {
    if (created.name == "taken") {
      throw rest::http_error{status::conflict, "name taken"};
    }

    return {.id = 7, .name = created.name, .email = created.email};
  }

  // Nothing: 204.
  void operator()(users::remove, std::int64_t) const {}
};

struct cards_service {
  std::vector<std::string> operator()(cards::list, std::int64_t) const { return {"gold"}; }
};
```

`mount` registers the routes in a server, or in a router you give one, which you then [serve](server.md#serving):

```cpp
http::server server;

rest::mount<v1>(server, users_service{}, cards_service{},
                rest::validators([](rest::credential<rest::bearer_auth> const &token) {
                  return token.value == "valid";
                }));
co_await server.listen(8080);
```

Each endpoint has exactly one service, and each security scheme a validator, returning `bool` or
`async::task<bool>`. Otherwise `mount` does not compile (`quoll::rest::mount: no service has an operator...`,
`quoll::rest::mount: no validator takes the credential<Scheme>...`). Services and validators are moved
into storage shared by the routes, which lives as long as the router, and are called from the thread
awaiting the dispatch. `mount` adds all the routes or none: it throws `std::invalid_argument`, leaving
the router as it was, for a route it already has.

When several responses share a type, `rest::respond<Status>(value)` says which:

```cpp
using renamed = rest::endpoint<"users.rename", verb::put, "/rename/{id:int64}", rest::returns<user>,
                               rest::returns<user, status::created>>;

async::task<rest::reply<renamed>> rename(user const &value, bool existed) {
  if (existed) {
    co_return rest::respond<status::ok>(value);
  }

  co_return rest::respond<status::created>(value);
}
```

A status or a type the endpoint does not declare does not compile. Only the endpoint's own responses:
those inherited from `with<>` are answered by the server.

### What a request goes through

Credentials first, then the arguments, then the service. These errors are
`application/problem+json` (`rest::problem`, RFC 9457), but for a service's other exceptions:

| What happens                        | Status                                                   | `detail`                                      |
| ----------------------------------- | -------------------------------------------------------- | --------------------------------------------- |
| Missing or rejected credentials     | 401, with the `WWW-Authenticate` challenge of the scheme | `missing credentials`, `rejected credentials` |
| An invalid or missing argument      | 400                                                      | What is wrong                                 |
| A body of another media type        | 415                                                      | `the body is <the media type of the codec>`   |
| A service throws `rest::http_error` | Its status                                               | `what()`, sent as it is to the client         |
| A service throws anything else      | 500, as from any handler; `on_handler_error` is told     | None                                          |

A middleware of the router runs outside of this: what it throws is no `http_error`, and it answers a
problem by returning a response. What a service throws goes through it, a stop or a timeout too. Over a
`http::local_transport`, it reaches the caller, as there is no connection to answer.

A service takes exactly the arguments of its endpoint, by value or by reference: one taking a narrower type, or
them in another order, does not compile, and a generic parameter (`auto`) asks for a return type written out. A string
parameter that is not UTF-8 answers 400, before the service runs. A validator takes a scheme alone: two taking the same one do not compile
either, a generic one beside one written for it included. A credential that comes twice is none, and answers 401.

## Calling: `rest::client`

A client takes the contract and a base URL. It connects as a
[`http::client`](client.md) does, with its options:

```cpp
using namespace std::chrono_literals;

rest::client<v1> client{"http://127.0.0.1:8080", {.response_timeout = 5s}};

client.credentials<rest::bearer_auth>("valid");

// The first declared response, or call_error.
user const found = co_await client.call<users::get>(42);
auto const page = co_await client.call<users::list>({.limit = 20});
co_await client.call<users::remove>(found.id);

// Any declared response, or why not.
auto const maybe = co_await client.try_call<users::get>(7);

if (!maybe) {
  std::cerr << static_cast<int>(maybe.error().response.status) << ' ' << maybe.error().reason << '\n';
} else if (maybe->holds<status::not_found>()) {
  std::cerr << maybe->get<status::not_found>().detail << '\n';
} else {
  std::cout << maybe->get<status::ok>().name << ' ' << page.size() << '\n';
}
```

| Outcome                                           | `call`                                                                   | `try_call`                                                 |
| ------------------------------------------------- | ------------------------------------------------------------------------ | ---------------------------------------------------------- |
| The first declared response                       | Its body                                                                 | A `reply<Endpoint>` holding it                             |
| Another declared response                         | Throws `rest::call_error`                                                | A `reply<Endpoint>` holding it                             |
| An undeclared status, a body that does not decode | Throws `rest::call_error`, whose `what()` is the problem's detail if any | A `rest::unexpected_response`: the whole response, and why |
| A failure of the transport                        | Throws `std::system_error`                                               | Throws `std::system_error`                                 |

Arguments are encoded by the call: an invalid one (an empty path parameter, one holding a NUL or a dot segment, the
rest of a path starting with `/`, a query parameter holding a NUL, a number that is not finite, a header value or a
credential the server would read differently) throws `std::invalid_argument` there, before the task runs.

`rest::call_error` is not an `http_error`: a service letting a failed call to another API escape
answers 500, not that API's status.

The transport is a template parameter: any `http::HttpTransport`. That is `http::client` by
default, on [`fetch`](runtimes.md#httpclient-in-a-browser) in a browser, and `http::local_transport` for
a router of the same program, which is how to test a contract without a network:

```cpp
http::router router;

rest::mount<users::api>(router, users_service{});

rest::client<users::api, http::local_transport> client{http::local_transport{router}};
user const found = co_await client.call<users::get>(42);
```

### Scopes

`scope<Api>(...)` gives the endpoints of an api nested in the client's, with the parameters of the prefixes above
them fixed, its own prefix's included. A scope of a scope fixes what the prefixes between them add:

```cpp
namespace repos {
using issues = rest::endpoint<"issues.list", verb::get, "/issues", rest::query<"state", std::optional<std::string>>,
                              rest::returns<std::vector<issue>>>;

using api = rest::api<"/repos/{owner}/{repo}", issues>;
} // namespace repos

auto const quoll = client.scope<repos::api>("4thlabs", "quoll");
auto const open = co_await quoll.call<repos::issues>("open");
```

A scope keeps a copy of the client as it was: its transport, which an `http::client` shares, its credentials and
its interceptors. Credentials or interceptors given to the client after do not reach it.

### Interceptors

`use(...)` wraps every request the client sends from then on, for `call` and `try_call` alike. An interceptor gets the
request and what sends it on, the next interceptor then the transport, and returns the response: it may change the
request, send it again, or answer without sending it. The first one added is the outermost.

```cpp
client.use(rest::retry{.attempts = 3, .delay = 200ms});

client.use([&tokens](http::request &&request, rest::next const &send) -> async::task<http::response> {
  auto response = co_await send(request);

  if (response.status == http::status::unauthorized) {
    request.fields.set("Authorization", "Bearer " + co_await tokens.refresh());
    response = co_await send(std::move(request));
  }

  co_return response;
});
```

`rest::retry` sends a request again after a network error, a 502, a 503 or a 504, up to `attempts` in all, waiting
`delay`, then twice as long each time. Only what may be sent twice with the same effect is: GET, HEAD, PUT, DELETE,
OPTIONS and TRACE, without a `body_writer`. A stop request ends the call at once. Its wait needs the executor's timer,
which `quoll::runtime` has; a `delay` of zero waits for nothing.

A copy of the client shares the interceptors it had, and one added after to either is its own. A scope keeps those the
client had when it was made, and takes none of its own: make the scope from a client that has them.

## Codecs

A codec encodes values into bodies and decodes bodies back, for one media type. `body<>` and `returns<>`
take one each.

| Codec                      | Bodies                                                                                                            |
| -------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `rest::json_codec`         | JSON, the default. Strict: every field that is not an `std::optional` must be there, and unknown keys are errors. |
| `rest::lenient_json_codec` | The same, skipping unknown keys, for third-party APIs, which add fields over time.                                |
| `rest::text_codec`         | `std::string`, as `text/plain; charset=utf-8`.                                                                    |

```cpp
using ping = rest::endpoint<"ping", verb::get, "/ping", rest::returns<std::string, status::ok, rest::text_codec>>;
```

A type that is not an aggregate, a renamed field or an enum is described by a `glz::meta<T>`
specialization, placed before the contract that uses it. Empty `std::optional` fields are not written.

```cpp
enum class card_status { active, frozen };

template <>
struct glz::meta<card_status> {
  using enum card_status;
  static constexpr auto value = glz::enumerate(active, frozen);
};
```

Anything modelling `rest::Codec` is a codec, default constructible, as one is built where it is used. `encode` throws on
what it cannot encode, and `decode` returns a `rest::decode_error` for a body that does not match, as bodies come from the
network:

```cpp
struct lines_codec {
  static constexpr std::string_view content_type = "text/x-lines";

  [[nodiscard]] std::string encode(std::vector<std::string> const &lines) const {
    std::string body;

    for (auto const &line : lines) {
      body += line + '\n';
    }

    return body;
  }

  template <class T>
  [[nodiscard]] std::expected<T, rest::decode_error> decode(std::string_view body) const {
    T lines;

    while (!body.empty()) {
      auto const end = body.find('\n');

      lines.emplace_back(body.substr(0, end));
      body.remove_prefix(end == std::string_view::npos ? body.size() : end + 1);
    }

    return lines;
  }
};

static_assert(rest::Codec<lines_codec, std::vector<std::string>>);
```

### Parameters

`rest::param_traits<T>` converts the values of path, query and header parameters to and from text:
integers, floating-point numbers, `bool` (`true` or `false`), `std::string`, and enums described by
`glz::meta`, by name. A text that cannot be read answers 400. Specialize it for your own types, which
then fit query and header parameters:

```cpp
struct day {
  int number = 0;
};

template <>
struct rest::param_traits<day> {
  [[nodiscard]] static std::string to_text(day const &value) { return std::to_string(value.number); }

  [[nodiscard]] static std::optional<day> from_text(std::string_view text) {
    auto const number = rest::param_traits<int>::from_text(text);

    return number ? std::optional{day{*number}} : std::nullopt;
  }
};

using by_day = rest::endpoint<"by_day", verb::get, "/by-day", rest::query<"day", day>, rest::returns<int>>;
```

## Third-party APIs

A contract written for someone else's API needs no Quoll server in the program. It has to say what that
API does, not what Quoll would choose. This works today:

- **Names that are not C++ names**, with `glz::meta`.
- **Unknown fields**, skipped by `rest::lenient_json_codec` on each `returns<>`.
- **Error bodies of any shape**, one type per status.
- **Bodies other than JSON**, with `rest::text_codec` or a codec of your own.
- **Credentials**, through `security<>` and `client.credentials<Scheme>()`.
- **The browser and native programs alike**, through the transport.

```cpp
namespace github {

struct repository {
  std::string full_name;
  std::int64_t stars = 0;
};

struct error {
  std::string message;
};

} // namespace github

template <>
struct glz::meta<github::repository> {
  using T = github::repository;
  static constexpr auto value = glz::object("full_name", &T::full_name, "stargazers_count", &T::stars);
};

namespace github {

using get_repository = rest::endpoint<"repos.get", verb::get, "/repos/{owner}/{name}",
                                      rest::header<"User-Agent", std::string>,
                                      rest::returns<repository, status::ok, rest::lenient_json_codec>,
                                      rest::returns<error, status::not_found, rest::lenient_json_codec>>;

using api = rest::api<"", get_repository>;

} // namespace github

async::task<> show_stars() {
  rest::client<github::api> client{"https://api.github.com"};
  auto const answer = co_await client.try_call<github::get_repository>("4thlabs", "quoll", "my-program");

  if (answer && answer->holds<status::ok>()) {
    std::cout << answer->get<status::ok>().stars << '\n';
  }
}
```

An API's full support is [step 4 of the roadmap](PLAN.md#step-4-rest-from-a-command-line-program), and
[scopes](#scopes) and [interceptors](#interceptors) are there. Not yet: form and bytes codecs, an enumerator for the
values an enum does not know, and an OpenAPI document generated from a contract.

A larger example, a contract with its server and client over TCP, is in
[`samples/notes`](../samples/notes).
