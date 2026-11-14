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

Bodies are plain aggregates, whose fields Glaze reflects, with no macro and no declaration. A route is one alias: its
method, its path and query, its result, then the types of its parameters. Routes are gathered in groups and in an `api`:

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

struct card {
  std::int64_t id = 0;
};

namespace users {

using list   = rest::get<"/users", std::vector<user>, rest::query_params<user_filter>>;
using get    = rest::get<"/users/{id}", user, std::int64_t>;
using create = rest::post<"/users", rest::created<user>, new_user>;
using remove = rest::del<"/users/{id}", void, std::int64_t>;

using routes = rest::group<list, get, create, remove>;

} // namespace users

namespace cards {

using list = rest::get<"/users/{user_id}/cards?status={status}", std::vector<card>,
                       std::int64_t, std::optional<std::string>>;

} // namespace cards

using v1 = rest::api<"/api/v1", rest::group<rest::security<rest::bearer_auth>, users::routes, cards::list>>;
```

> [!NOTE]
> Reflected types need external linkage: Clang rejects them in an anonymous namespace.

- **The method** is the type: `rest::get`, `post`, `put`, `patch`, `del` (`delete` is a keyword), `head`, `options`.
- **The string** holds the whole path below the api's prefix, and the query, `key={name}` for a parameter. Each route
  is its own type, unique by its method and its path, as the router requires.
- **The result** comes first: see [Results](#results).
- **The parameters** follow, in this order: a type for each `{name}` of the string, the path's then the query's; then
  the headers, `rest::header<"X-Request-Id", T>`; then the body, last, on a `post`, a `put`, a `patch` or a `del` only. A body on DELETE has no meaning of its own
  (RFC 9110): for an API that declares one, as GitHub's `DELETE /user/emails`, through hops that all forward it.
- **The name** of its operation, OpenAPI's operationId, is `rest::operation_id<"users.get">` among its types, or else
  its method and whole path, `GET /api/v1/users/{id}`. It is the route's name in the router, which a middleware reads
  through `next.route()`.
- **A group** holds routes, groups, and what it gives all of them, `rest::security<...>` or a `rest::header<>`, which
  come after a route's own arguments. Groups nest, and have no prefix.
- **The api** has the prefix, `"/api/v1"` or none, with no parameter: one its routes share goes in each one's path, as
  `user_id` above.

### Parameters

A path parameter takes a whole segment, and `{name:path}`, last, the rest of the path, a `std::string`. A parameter's
type is read from text, as `rest::param_traits<T>` does: integers, floating-point numbers, `bool` (`true` or `false`),
`std::string`, and enums described by `glz::meta`, by name. A query parameter or a header may also be a
`std::optional<T>`, left out when empty, or a `std::vector<T>`, repeated (`?tag=a&tag=b`). `rest::query_params<S>`
takes one query parameter per field of a struct, filled with designated initializers: for operations with many
optional parameters, as a component's state. A route has its query in its string or in a `query_params<>`, not both.

### Results

| Result                                   | Answered                                                              |
| ---------------------------------------- | --------------------------------------------------------------------- |
| `T`, `rest::json<T>`                     | 200, `T` in JSON                                                      |
| `void`                                   | 204, no body                                                          |
| `rest::created<R>`, `rest::accepted<R>`  | 201, 202, `R` being any result of this table but events (`void` too)  |
| `rest::text`                             | 200, a `std::string` as `text/plain; charset=utf-8`                   |
| `rest::bytes`                            | 200, a `std::vector<std::byte>` as `application/octet-stream`         |
| `rest::lenient_json<T>`                  | 200, `T` in JSON, read skipping the fields it does not know           |
| `rest::form<T>`                          | 200, the fields of `T` as `application/x-www-form-urlencoded`         |
| `rest::encoded<T, Codec>`                | 200, `T` in a codec of your own: see [Codecs](#codecs)                |
| `rest::events<T>`                        | A stream of server-sent events: see [Events](#events)                 |

A body takes the formats of this table: `new_user`, `rest::json<T>`, `rest::lenient_json<T>`, `rest::text`,
`rest::bytes`, `rest::form<login>` or `rest::encoded<T, Codec>`. A route declares no
error responses: a handler throws an `http::http_error`, answered as an RFC 9457 problem, and the client gives it
back as an `http::http_error` received, whose body a [third-party API](#third-party-apis)'s own shape reads.

### What does not compile

What does not hold fails to compile, with a message that says so. These are some of the checks:

| What is wrong                                    | What the compiler says                                                                                  |
| ------------------------------------------------ | ------------------------------------------------------------------------------------------------------- |
| A `{parameter}` with no type                     | `quoll::rest: a route has a type for each {parameter} of its expression, in order, after its result`    |
| A type in the string: `{id:int64}`               | `quoll::rest: a route says the types of its parameters after its expression, not in it`                 |
| A path parameter that is a `std::optional`       | `quoll::rest: a path parameter's type is read from text (a Parameter), never a std::optional ...`       |
| A body on a `get`, a `head`, an `options`        | `quoll::rest: only a post, a put, a patch or a del has a body`                                          |
| A body before a header                           | `quoll::rest: a route's body is its last type, after its parameters and headers`                        |
| A result on a `head`                             | `quoll::rest: a response to HEAD has no body: its result is void`                                       |
| A parameter in the api's prefix                  | `quoll::rest::api: the prefix has no parameter: one its routes share goes in each one's path`           |
| Two routes of one method and path                | `quoll::rest::api: two routes have the same method and path`                                            |
| A handler of a route of another api              | `quoll::rest::mount: a handler takes a route this api does not have`                                    |

## Serving: `mount`

A route's handler is a lambda taking the route first, then the route's parameters. A `rest::service` gathers them,
defined once and mounted:

```cpp
auto const service = rest::service(
    [&store](users::list, user_filter filter) {
      return store.find(filter);
    },
    [&store](users::get, std::int64_t id) -> async::task<user> {
      // An error is thrown, answered as a problem with its status.
      auto found = co_await store.load(id);

      if (!found) {
        throw http::http_error{status::not_found, "no user " + std::to_string(id)};
      }

      co_return std::move(*found);
    },
    [&store](users::create, new_user const &created) {
      return store.add(created);
    },
    // Nothing: 204.
    [&store](users::remove, std::int64_t id) {
      store.erase(id);
    },
    [&store](cards::list, std::int64_t user_id, std::optional<std::string> status) {
      return store.cards(user_id, status);
    });
```

A handler returns its route's result, or nothing for a `void` one, as is or in an `async::task`. A struct with an
`operator()(Route, parameters...)` for each of its routes serves the same way, and `mount` takes several, mixed with
lambdas. `mount` registers the routes in a server, or in a router you give one, which you then
[serve](server.md#serving):

```cpp
http::server server;

rest::mount<v1>(server, service,
                rest::validators([](rest::credential<rest::bearer_auth> const &token) {
                  return token.value == "valid";
                }));
co_await server.listen(8080);
```

Each route has one handler, of its parameters exactly, and each security scheme a validator, returning `bool` or
`async::task<bool>`: otherwise `mount` does not compile (`quoll::rest::mount: no handler takes a route of the api...`,
`quoll::rest::mount: no validator takes the credential<Scheme>...`). A lambda of a route the api does not have, or two
lambdas of one route in a service, do not compile either; a generic lambda, or a struct of several `operator()`, is
looked up by its call alone. Handlers and validators are copied, or moved when given as rvalues, into storage shared by
the routes, which lives as long as the router, and are called from the thread awaiting the dispatch. `mount` adds all
the routes or none: it throws `std::invalid_argument`, leaving the router as it was, for a route it already has.

### What a request goes through

Credentials first, then the arguments, then the handler. These errors are
`application/problem+json` (`http::problem`, RFC 9457), but for a handler's other exceptions:

| What happens                        | Status                                                   | `detail`                                      |
| ----------------------------------- | -------------------------------------------------------- | --------------------------------------------- |
| Missing or rejected credentials     | 401, with the `WWW-Authenticate` challenge of the scheme | `missing credentials`, `rejected credentials` |
| An invalid or missing argument      | 400                                                      | What is wrong                                 |
| A body of another media type        | 415                                                      | `the body is <the media type of the codec>`   |
| A handler throws `http::http_error` | Its status                                               | `what()`, sent as it is to the client         |
| It lets through one received        | 502, from another call                                   | `a call to another server failed`             |
| A handler throws anything else      | 500, as from any handler; `on_handler_error` is told     | None                                          |

A middleware of the router runs outside of this: it sees these answers as responses, and one it throws itself is
answered by the router as a handler's is. What a handler throws goes through it, a stop or a timeout too, but for one of
`events<>`, which runs once the middlewares have returned, as a handler writing its response does. Over a
`http::local_transport`, it reaches the caller, as there is no connection to answer.

A handler takes exactly the arguments of its route, by value or by reference: one taking a narrower type, or them in
another order, does not compile, and a generic parameter (`auto`) asks for a return type written out. A string
parameter that is not UTF-8 answers 400, before the handler runs. A validator takes a scheme alone: two taking the same
one do not compile either, a generic one beside one written for it included. A credential that comes twice is none,
and answers 401.

## Calling: `rest::client`

A client takes the contract and a base URL. It connects as a
[`http::client`](client.md) does, with its options:

```cpp
using namespace std::chrono_literals;

rest::client<v1> client{"http://127.0.0.1:8080", {.response_timeout = 5s}};

client.credentials<rest::bearer_auth>("valid");

// The result, or an http::http_error received.
user const found = co_await client.call<users::get>(42);
auto const page = co_await client.call<users::list>({.limit = 20});
co_await client.call<users::remove>(found.id);

// The result, or the error.
auto const maybe = co_await client.try_call<users::get>(7);

if (!maybe) {
  std::cerr << static_cast<int>(maybe.error().status()) << ' ' << maybe.error().what() << '\n';
} else {
  std::cout << maybe->name << ' ' << page.size() << '\n';
}
```

| Outcome                                      | `call`                                                                   | `try_call`                                    |
| -------------------------------------------- | ------------------------------------------------------------------------ | --------------------------------------------- |
| The route's result                           | Its value                                                                | `std::expected<T, http::http_error>` with it  |
| Another status, a body that does not decode  | Throws `http::http_error`, whose `what()` is the problem's detail if any | The same `http::http_error`, as its error     |
| A failure of the transport                   | Throws `std::system_error`                                               | Throws `std::system_error`                    |

Arguments are encoded by the call: an invalid one (an empty path parameter, one holding a NUL or a dot segment, the
rest of a path starting with `/`, a query parameter holding a NUL, a number that is not finite, a header value or a
credential the server would read differently) throws `std::invalid_argument` there, before the task runs.

The error is an `http::http_error` received: its `status()`, `received()`, the response as it came in `answer()`, and
`body_as<T>(codec)`, its body read as a `T`, a third-party API's shape of its errors. A handler letting it escape
answers 502, not that API's status: its status and detail are that server's, not this one's.

The transport is a template parameter: any `http::HttpTransport`. That is `http::client` by
default, on [`fetch`](runtimes.md#httpclient-in-a-browser) in a browser, and `http::local_transport` for
a router of the same program, which is how to test a contract without a network:

```cpp
http::router router;

rest::mount<v1>(router, service, validators);

rest::client<v1, http::local_transport> client{http::local_transport{router}};
user const found = co_await client.call<users::get>(42);
```

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

A copy of the client shares the interceptors it had, and one added after to either is its own.

## Events

A route whose result is `rest::events<T>` answers with a stream of server-sent events, each a `T` encoded by the codec,
JSON by default, rather than one body:

```cpp
using feed = rest::get<"/changes?count={count}", rest::events<change>,
                       int, rest::header<"Last-Event-ID", std::optional<std::string>>>;
```

Its handler takes a `rest::event_sink<T>` by reference after its arguments, and returns a `task<>`:

```cpp
[&changes](feed, int count, std::optional<std::string> last_id, rest::event_sink<change> &events) -> async::task<> {
  // What may refuse the request comes first: an http_error here answers, a 404 say.
  auto const pending = co_await changes.after(last_id, count);

  co_await events.open();

  for (auto const &next : pending) {
    co_await events.send(next, {.id = std::to_string(next.id)});   // an id is text
  }
}
```

It runs once the router has answered, as a handler writing its response in pieces does (see
[Streaming](streaming.md#server-sent-events)). An `http_error` before the stream starts, with `open()`
or the first event, comment or retry, answers as from any handler, a 404 say; after it, the
stream is cut. To HEAD, the handler runs up to its first write, as such a handler does, so that the
status is the one GET would get. `send(value, {.type, .id})`,
`comment()` and `retry()` go through an `http::event_writer`: one at a time, and the comments that
keep a quiet stream alive come from the loop that sends.

A client reads them with `subscribe`, once the head of the stream has come:

```cpp
auto stream = co_await api.subscribe<feed>(20, last_seen);

while (auto const change = co_await stream.next()) {
  show(*change);
}

// To resume from, when connecting again: kept if the stream ended before an event with an id.
if (auto const id = stream.last_event_id(); !id.empty()) {
  last_seen = id;
}
```

Any other answer throws an `http::http_error` received, as from `call`: another status, a 200 that is no event stream.
A route that takes its own `Accept` header sends it, and takes the one media type it names, if an event stream, a
third-party API's `application/vnd.example+event-stream` say. An event that does not decode throws an
`http::http_error` received from `next()`, holding it as its body, and reading goes on. `subscribe` needs a transport
that gives a response as its head comes, `http::client` natively and through `fetch`, not `http::local_transport`. It
goes straight through it, credentials applied. Interceptors take whole responses: on a client with any, `subscribe`
throws `std::logic_error`, rather than go out without what they add; subscribe through a client without them. It does
not connect again on its own: resuming is an argument of the contract, `Last-Event-ID` above.

## Codecs

A codec encodes values into bodies and decodes bodies back, for one media type. The formats of a route's body and
result say theirs: `rest::text`, `rest::bytes`, `rest::form<T>`, `rest::lenient_json<T>`, and `rest::encoded<T, Codec>`
for any other.

| Codec                      | Bodies                                                                                                            |
| -------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `rest::json_codec`         | JSON, the default. Strict: every field that is not an `std::optional` must be there, and unknown keys are errors. |
| `rest::lenient_json_codec` | The same, skipping unknown keys, for third-party APIs, which add fields over time.                                |
| `rest::text_codec`         | `std::string`, as `text/plain; charset=utf-8`.                                                                    |
| `rest::form_codec`         | The fields of an aggregate, as `application/x-www-form-urlencoded`, each converted as a query parameter is.       |
| `rest::bytes_codec`        | `std::vector<std::byte>` or `std::string`, as they are, as `application/octet-stream`.                            |

```cpp
using ping = rest::get<"/ping", rest::text>;
```

A form takes the fields of a struct by name, as `query_params<>` does: a vector repeats its name, an `std::optional`
without a value is left out, anything else is required, once. The struct is an aggregate of 64 fields at most, with
no base class, each field with a default value and none a reference, or a struct listing its fields as below, and a
form of more than 1000 fields is refused.
It is what a browser's form sends, and what many APIs take, as an OAuth token endpoint does:

```cpp
struct token_request {
  std::string grant_type;
  std::string refresh_token;
};

struct token_response {
  std::string access_token;
};

using token  = rest::post<"/oauth/token", rest::lenient_json<token_response>, rest::form<token_request>>;
using avatar = rest::get<"/avatar", rest::bytes>;
```

The fields of a query or a form go by the names they are declared with. One whose name is not a C++ name is given
it by the struct's `rest_names`, which lists only those, the others keeping theirs:

```cpp
struct charge {
  std::string order_id;
  std::int64_t amount = 0;
  std::optional<std::string> api_version;

  using rest_names = rest::names<
      rest::name<"metadata[order_id]", &charge::order_id>,
      rest::name<"api-version", &charge::api_version>>;
};
```

A struct the reflection does not read, as one with a base class, lists all its fields, by `rest_fields` of the same
form: then they are read as listed, and only those: a field left out of the list is neither sent nor read.

```cpp
struct base_filter {
  std::optional<std::string> q;
};

struct filter : base_filter {
  std::optional<int> page;

  using rest_fields = rest::names<rest::name<"q", &filter::q>, rest::name<"page", &filter::page>>;
};
```

In JSON, a type that is not an aggregate, a renamed field or an enum is described by a `glz::meta<T>`
specialization, placed before the contract that uses it: it renames the JSON only, as `rest_names` renames the query
and the form only. Empty `std::optional` fields are not written.

```cpp
enum class card_status { active, frozen };

template <>
struct glz::meta<card_status> {
  using enum card_status;
  static constexpr auto value = glz::enumerate(active, frozen);
};
```

Anything modelling `rest::Codec` (`http::Codec`) is a codec, default constructible, as one is built where it is used.
`encode` throws on what it cannot encode, and `decode` returns a `rest::decode_error` for a body that does not match, as
bodies come from the network. `rest::encoded<T, Codec>` puts it on a body or a result:

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

using lines = rest::post<"/lines", rest::encoded<std::vector<std::string>, lines_codec>,
                         rest::encoded<std::vector<std::string>, lines_codec>>;
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

using by_day = rest::get<"/by-day?day={day}", int, day>;
```

## Third-party APIs

A contract written for someone else's API needs no Quoll server in the program. It has to say what that
API does, not what Quoll would choose. This works today:

- **Names that are not C++ names**: in JSON with `glz::meta`, in a query or a form with `rest_names`.
- **Unknown fields**, skipped by a `rest::lenient_json<T>` result.
- **Error bodies of any shape**, read from the error a call gives, `error.body_as<T>(codec)`.
- **Bodies other than JSON**, with `rest::text_codec`, `rest::form_codec`, `rest::bytes_codec` or a codec of your own.
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

using get_repository = rest::get<"/repos/{owner}/{name}", rest::lenient_json<repository>,
                                 std::string, std::string, rest::header<"User-Agent", std::string>>;

using api = rest::api<"", get_repository>;

} // namespace github

async::task<> show_stars() {
  rest::client<github::api> client{"https://api.github.com"};
  auto const answer = co_await client.try_call<github::get_repository>("4thlabs", "quoll", "my-program");

  if (answer) {
    std::cout << answer->stars << '\n';
  } else if (auto const said = answer.error().body_as<github::error>(rest::lenient_json_codec{})) {
    std::cerr << said->message << '\n';
  }
}
```

An API's full support is [step 4 of the roadmap](PLAN.md#step-4-rest-from-a-command-line-program-done), and
[interceptors](#interceptors), the form and bytes [codecs](#codecs) and an enumerator for the values an enum does not
know are there. Not yet: an OpenAPI document generated from a contract.

### Values an enum does not know

An API adds values to its enums over time, and a response holding a new one should still decode. A field of type
`rest::open_enum<E>` reads a name of `E` as its enumerator, and keeps any other as it came:

```cpp
enum class issue_state { open, closed };

template <>
struct glz::meta<issue_state> {   // before the first use of the enum, as always
  using enum issue_state;
  static constexpr auto value = glz::enumerate(open, closed);
};

struct issue {
  std::string title;
  rest::open_enum<issue_state> state;   // "merged" kept as it came
};

if (found.state == issue_state::open) {
  // ...
}

std::cout << found.state.name() << '\n';   // "merged"
```

`known()` gives the enumerator, if the name is one of the enum's. A name kept is written back as it came, so that what
a program reads and sends again is unchanged. It is a string in JSON, the strict codec included, and a name as a query
or header parameter. An enumerator the `glz::meta` has no name for (a cast, or `E{}` when no enumerator is 0) is not
`known()`, and writing it fails, as for the enum. Only that field is open: the enum itself stays strict, a key of a map included, and a
contract of Quoll's own server that takes the enum refuses a name it does not know with 400.

A larger example, a contract with its server, its command line over TCP and its page in a browser, is in
[`samples/notes`](../samples/notes). A command-line program calling GitHub's API, natively and through `fetch` in
Node, with interceptors, a token and GitHub's shape of its errors, is in [`samples/github`](../samples/github).
