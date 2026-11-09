//   ___              _ _
//  / _ \ _   _  ___ | | |
// | | | | | | |/ _ \| | |
// | |_| | |_| | (_) | | |
//  \__\_\\__,_|\___/|_|_|
//
// BSD 3-Clause License
//
// Copyright (c) 2026, Thomas Gourgues (thomas.gourgues@gmail.com)
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <quoll/async/co_spawn.hpp>
#include <quoll/async/current_executor.hpp>
#include <quoll/async/detached.hpp>
#include <quoll/async/run_loop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/async/yield.hpp>
#include <quoll/client.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/fields.hpp>
#include <quoll/http/local_transport.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/response_body.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/streamed_response.hpp>
#include <quoll/http/streaming_transport.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/rest.hpp>
#include <quoll/server/server.hpp>
#include <quoll/test/memory_network.hpp>
#include <quoll/test/read_until_closed.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// Reflected types need external linkage (see json_codec).
namespace events_test {

struct change {
  std::int64_t id = 0;
  std::string what;

  bool operator==(change const &) const = default;
};

// Read by Glaze as a view into the request's body.
struct filter {
  std::string_view word;
};

namespace rest = quoll::rest;

using quoll::http::status;
using quoll::http::verb;

namespace changes {
using namespace quoll::rest;

// Changes after a given one, as many as asked; 404 for a feed that does not exist, before any event.
using feed =
    endpoint<"changes.feed", verb::get, "/{name}", query<"count", int>,
             header<"Last-Event-ID", std::optional<std::string>>, events<change>, returns<problem, status::not_found>>;
// One event the client cannot decode, then a good one.
using broken = endpoint<"changes.broken", verb::get, "/broken", events<change>>;
// An error once the stream has started.
using cut = endpoint<"changes.cut", verb::get, "/cut", events<change>>;
// Nothing to say.
using none = endpoint<"changes.none", verb::get, "/none", events<change>>;
// An event the service cannot send, then a declared 400 it answers instead.
using checked =
    endpoint<"changes.checked", verb::get, "/checked", events<change>, returns<problem, status::bad_request>>;
// A failure that is no http_error, before the first event.
using failing = endpoint<"changes.failing", verb::get, "/failing", events<change>>;

// A first event, then a second once the test lets it go: what only comes as it is sent.
using paced = endpoint<"changes.paced", verb::get, "/paced", events<change>>;
// open() asked and not awaited, then a refusal.
using opened = endpoint<"changes.opened", verb::get, "/opened", events<change>, returns<problem, status::not_found>>;

// A filter read from the body as a view into it: what makes the request live as long as the stream.
using search = endpoint<"changes.search", verb::post, "/search", body<filter>, events<change>>;

// An operation kept past its service, in the service.
using kept = endpoint<"changes.kept", verb::get, "/kept", events<change>>;
// The same, the service failing after.
using kept_then_failing = endpoint<"changes.kept_then_failing", verb::get, "/kept-then-failing", events<change>>;
// What a client decodes as views into its events.
using viewed = endpoint<"changes.viewed", verb::get, "/viewed", events<filter>>;

using api = rest::api<"/changes", feed, broken, cut, none, checked, failing, paced, opened, search, kept,
                      kept_then_failing, viewed>;

// The same feeds, scoped under one name.
namespace named {
using feed = endpoint<"named.feed", verb::get, "/", events<change>>;
using api = rest::api<"/feeds/{name}", feed>;
} // namespace named

using all = rest::api<"", api, named::api>;

// A feed behind a token, whose contract asks a media type of its own.
namespace secured {
using feed = endpoint<"secured.feed", verb::get, "/secured", security<bearer_auth>, header<"Accept", std::string>,
                      events<change>>;
using api = rest::api<"", feed>;
} // namespace secured
} // namespace changes

struct changes_service {
  // Lets paced send its second event.
  std::shared_ptr<bool> released = std::make_shared<bool>(false);
  // What kept keeps past its end.
  std::shared_ptr<std::optional<quoll::async::task<>>> held = std::make_shared<std::optional<quoll::async::task<>>>();

  quoll::async::task<> operator()(changes::kept, rest::event_sink<change> &events) const {
    held->emplace(events.send({.id = 1, .what = "never"}));
    co_return;
  }

  quoll::async::task<> operator()(changes::kept_then_failing, rest::event_sink<change> &events) const {
    held->emplace(events.comment());
    throw std::runtime_error{"after keeping an operation"};
    co_return;
  }

  quoll::async::task<> operator()(changes::viewed, rest::event_sink<filter> &events) const {
    co_await events.send({.word = "a word long enough to leave the buffer"});
    co_await events.send({.word = "another word long enough to leave it"});
  }

  quoll::async::task<> operator()(changes::paced, rest::event_sink<change> &events) const {
    co_await events.send({.id = 1, .what = "first"});

    while (!*released) {
      co_await quoll::async::yield();
    }

    co_await events.send({.id = 2, .what = "second"});
  }

  quoll::async::task<> operator()(changes::search, filter wanted, rest::event_sink<change> &events) const {
    co_await quoll::async::yield();
    co_await events.send({.id = 1, .what = std::string{wanted.word}});
  }

  quoll::async::task<> operator()(changes::opened, rest::event_sink<change> &events) const {
    auto const opening = events.open();

    static_cast<void>(opening);
    throw rest::http_error{status::not_found, "nothing to open"};
    co_return;
  }

  quoll::async::task<> operator()(changes::feed, std::string name, int count, std::optional<std::string> last_id,
                                  rest::event_sink<change> &events) const {
    if (name != "main") {
      throw rest::http_error{status::not_found, "no feed " + name};
    }

    co_await events.open();

    auto first = last_id ? std::stoll(*last_id) + 1 : std::int64_t{1};

    for (auto id = first; id < first + count; ++id) {
      co_await events.send({.id = id, .what = "change " + std::to_string(id)}, {.id = std::to_string(id)});
    }
  }

  quoll::async::task<> operator()(changes::broken, rest::event_sink<change> &events) const {
    co_await events.comment("first one is no change");
    co_await events.retry(std::chrono::seconds{3});
    co_await events.send({.id = 1, .what = "ok"});
  }

  quoll::async::task<> operator()(changes::none, rest::event_sink<change> & /*events*/) const { co_return; }

  quoll::async::task<> operator()(changes::checked, rest::event_sink<change> &events) const {
    bool refused = false;

    try {
      co_await events.send({.id = 1, .what = "x"}, {.id = std::string{"1\n2"}});
    } catch (std::invalid_argument const &) {
      refused = true;
    }

    if (refused) {
      throw rest::http_error{status::bad_request, "an id on two lines"};
    }

    co_return;
  }

  quoll::async::task<> operator()(changes::failing, rest::event_sink<change> & /*events*/) const {
    throw std::runtime_error{"internal"};
    co_return;
  }

  quoll::async::task<> operator()(changes::named::feed, std::string name, rest::event_sink<change> &events) const {
    co_await events.send({.id = 1, .what = name});
  }

  quoll::async::task<> operator()(changes::secured::feed, std::string accept, rest::event_sink<change> &events) const {
    co_await events.send({.id = 1, .what = accept});
  }

  quoll::async::task<> operator()(changes::cut, rest::event_sink<change> &events) const {
    co_await events.send({.id = 1, .what = "before"});

    throw rest::http_error{status::not_found, "gone meanwhile"};
  }
};

} // namespace events_test

namespace {

using namespace events_test;

namespace http = quoll::http;
namespace async = quoll::async;

using async::task;

// What a writer gave: its head, and its pieces.
struct gathered {
  http::status status = http::status::ok;
  http::fields fields;
  std::vector<std::string> pieces;
};

struct gathering_sink {
  gathered *into;

  void head(http::status code, http::fields fields) {
    into->status = code;
    into->fields = std::move(fields);
  }

  [[nodiscard]] task<> write(std::string_view piece) {
    if (!piece.empty()) {
      into->pieces.emplace_back(piece);
    }

    co_return;
  }
};

// The pieces a writer gave, one a read, a turn of the loop after each is asked; then the end, or a failure.
class pieces_source {
public:
  pieces_source(std::vector<std::string> pieces, std::error_code failure, std::shared_ptr<std::size_t> reads)
      : pieces_(std::move(pieces)), failure_(failure), reads_(std::move(reads)) {}

  [[nodiscard]] task<std::optional<std::string_view>> read() {
    co_await async::yield();
    ++*reads_;

    if (at_ == pieces_.size()) {
      if (failure_) {
        throw std::system_error{failure_};
      }

      co_return std::nullopt;
    }

    co_return std::string_view{pieces_[at_++]};
  }

private:
  std::vector<std::string> pieces_;
  std::size_t at_ = 0;
  std::error_code failure_;
  // Counted where the transport sees it: how far a reader read.
  std::shared_ptr<std::size_t> reads_;
};

// A router as a transport that streams: what a writer of the response gives comes as pieces of its body, once the
// writer has run, which a finite stream allows. What it was asked is kept.
class streaming_transport {
public:
  explicit streaming_transport(http::router router, std::error_code failure = {})
      : local_(router), router_(std::move(router)), failure_(failure) {}

  [[nodiscard]] task<http::response> send(http::request request) const { return local_.send(std::move(request)); }

  [[nodiscard]] task<http::streamed_response> stream(http::request request) const {
    return streamed(router_, std::move(request), asked_, failure_, reads_);
  }

  [[nodiscard]] http::request const &last_asked() const { return *asked_; }

  // The reads of the last body, its end included.
  [[nodiscard]] std::size_t reads() const { return *reads_; }

private:
  static task<http::streamed_response> streamed(http::router router, http::request request,
                                                std::shared_ptr<http::request> asked, std::error_code failure,
                                                std::shared_ptr<std::size_t> reads) {
    *asked = request;

    auto answer = co_await router.dispatch(std::move(request));
    gathered got{.status = answer.status, .fields = answer.fields, .pieces = {}};

    if (auto writer = std::move(answer.late_head_writer)) {
      gathering_sink sink{&got};

      co_await writer(http::body_stream{sink});
    } else if (!answer.body.empty()) {
      got.pieces.push_back(answer.body);
    }

    http::streamed_response out;

    out.status = got.status;
    out.fields = std::move(got.fields);
    *reads = 0;
    out.body = http::response_body{std::make_shared<pieces_source>(std::move(got.pieces), failure, std::move(reads))};
    out.trailers = std::make_shared<http::fields const>();
    co_return out;
  }

  http::local_transport local_;
  http::router router_;
  std::shared_ptr<http::request> asked_ = std::make_shared<http::request>();
  std::error_code failure_;
  std::shared_ptr<std::size_t> reads_ = std::make_shared<std::size_t>(0);
};

static_assert(http::HttpTransport<streaming_transport>);
static_assert(http::StreamingTransport<streaming_transport>);

[[nodiscard]] http::router make_router() {
  http::router router;

  rest::mount<changes::all>(router, changes_service{});
  return router;
}

using changes_client = rest::client<changes::all, streaming_transport>;

// What a server writes on a connection for `sent`, until it closes.
[[nodiscard]] std::string answer_to(http::router const &router, std::string const &sent) {
  http::server const server{router};
  auto [client, ours] = quoll::io::make_memory_stream_pair();
  auto const peer = [&sent](quoll::io::memory_stream to) -> task<std::string> {
    (void)co_await quoll::io::write(to, std::as_bytes(std::span{sent}));
    co_return co_await quoll::test::read_until_closed(to);
  };
  auto outcome =
      quoll::test::run(quoll::async::when_all(server.serve_connection(std::move(ours)), peer(std::move(client))));

  return std::get<1>(std::move(outcome).value());
}

[[nodiscard]] std::string get(std::string_view target) {
  return "GET " + std::string{target} + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
}

} // namespace

TEST_CASE("rest events: subscribe gives the values a service sends, as an event_sink sends them", "[rest][sse]") {
  changes_client const client{streaming_transport{make_router()}};

  auto const reading = [&client]() -> task<std::vector<change>> {
    auto stream = co_await client.subscribe<changes::feed>("main", 3, std::nullopt);
    std::vector<change> seen;

    while (auto value = co_await stream.next()) {
      seen.push_back(std::move(*value));
    }

    seen.push_back({.id = 0, .what = stream.last_event_id()});
    co_return seen;
  };

  auto const seen = quoll::test::run(reading());

  REQUIRE(seen);
  REQUIRE(*seen == std::vector<change>{{1, "change 1"}, {2, "change 2"}, {3, "change 3"}, {0, "3"}});
  REQUIRE(client.transport().last_asked().fields.find("Accept") == "text/event-stream");
}

TEST_CASE("rest events: the id to resume from is an argument like any", "[rest][sse]") {
  changes_client const client{streaming_transport{make_router()}};

  auto const reading = [&client]() -> task<std::optional<change>> {
    auto stream = co_await client.subscribe<changes::feed>("main", 1, std::string{"7"});

    co_return co_await stream.next();
  };

  REQUIRE(quoll::test::run(reading()).value() == change{8, "change 8"});
}

TEST_CASE("rest events: an http_error before the first event answers, and subscribe throws call_error", "[rest][sse]") {
  changes_client const client{streaming_transport{make_router()}};

  auto const reading = [&client]() -> task<std::string> {
    try {
      static_cast<void>(co_await client.subscribe<changes::feed>("other", 1, std::nullopt));
    } catch (rest::call_error const &failed) {
      co_return std::to_string(static_cast<int>(failed.status())) + " " + failed.what();
    }

    co_return "subscribed";
  };

  REQUIRE(quoll::test::run(reading()).value() == "404 no feed other");
}

TEST_CASE("rest events: comments and a retry are no values, the retry kept", "[rest][sse]") {
  changes_client const client{streaming_transport{make_router()}};

  auto const reading = [&client]() -> task<std::string> {
    auto stream = co_await client.subscribe<changes::broken>();
    std::string seen;

    // The comment and the retry are no events: the first value is the good one.
    auto const value = co_await stream.next();

    seen += value ? value->what : "none";
    seen += stream.retry() == std::chrono::seconds{3} ? " retry" : " no retry";
    seen += (co_await stream.next()) ? " more" : " end";
    co_return seen;
  };

  REQUIRE(quoll::test::run(reading()).value() == "ok retry end");
}

TEST_CASE("rest events: a value the codec cannot read throws call_error from next()", "[rest][sse]") {
  http::router router;

  router.get("/changes/broken", [](http::response_writer out) -> task<> {
    http::event_writer events{std::move(out)};

    co_await events.send({.data = "not json"});
    co_await events.send({.data = R"({"id":2,"what":"fine"})"});
  });

  changes_client const client{streaming_transport{std::move(router)}};

  auto const reading = [&client]() -> task<std::string> {
    auto stream = co_await client.subscribe<changes::broken>();
    std::string seen;

    try {
      static_cast<void>(co_await stream.next());
    } catch (rest::call_error const &failed) {
      seen = failed.failure().response.body + ";";
    }

    auto const next = co_await stream.next();

    co_return seen + (next ? next->what : "none");
  };

  REQUIRE(quoll::test::run(reading()).value() == "not json;fine");
}

TEST_CASE("rest events: a response that is no event stream throws call_error", "[rest][sse]") {
  http::router router;

  router.get("/changes/broken", [](http::request) {
    http::response response;

    response.fields.set("Content-Type", "text/html");
    response.body = "<html>log in</html>";
    return response;
  });

  changes_client const client{streaming_transport{std::move(router)}};

  auto const reading = [&client]() -> task<std::string> {
    try {
      static_cast<void>(co_await client.subscribe<changes::broken>());
    } catch (rest::call_error const &failed) {
      co_return failed.failure().reason + "|" + failed.failure().response.body;
    }

    co_return "subscribed";
  };

  // Said by its head: a body that may never end is not read.
  REQUIRE(quoll::test::run(reading()).value() == "not an event stream: Content-Type text/html|");
}

TEST_CASE("rest events: an http_error once the stream has started cuts it", "[rest][sse]") {
  auto const answer = answer_to(make_router(), get("/changes/cut"));

  REQUIRE(answer.starts_with("HTTP/1.1 200"));
  REQUIRE(answer.find("Content-Type: text/event-stream\r\n") != std::string::npos);
  REQUIRE(answer.find(R"(data: {"id":1,"what":"before"})") != std::string::npos);
  // Cut: no last chunk, as a peer must not take a stream that failed for one that ended.
  REQUIRE_FALSE(answer.ends_with("0\r\n\r\n"));
  REQUIRE(answer.find("gone meanwhile") == std::string::npos);
}

TEST_CASE("rest events: a service that sends nothing still answers a stream, with no event", "[rest][sse]") {
  changes_client const client{streaming_transport{make_router()}};

  auto const reading = [&client]() -> task<bool> {
    auto stream = co_await client.subscribe<changes::none>();

    co_return !(co_await stream.next()).has_value();
  };

  REQUIRE(quoll::test::run(reading()) == true);
}

TEST_CASE("rest events: on the wire, an http_error before the first event is its problem", "[rest][sse]") {
  auto const answer = answer_to(make_router(), get("/changes/other?count=1"));

  REQUIRE(answer.starts_with("HTTP/1.1 404"));
  REQUIRE(answer.find("Content-Type: application/problem+json\r\n") != std::string::npos);
  REQUIRE(answer.find("no feed other") != std::string::npos);
  REQUIRE(answer.find("text/event-stream") == std::string::npos);
}

TEST_CASE("rest events: an event refused before the stream starts leaves an http_error to answer", "[rest][sse]") {
  auto const answer = answer_to(make_router(), get("/changes/checked"));

  REQUIRE(answer.starts_with("HTTP/1.1 400"));
  REQUIRE(answer.find("an id on two lines") != std::string::npos);
}

TEST_CASE("rest events: another failure before the first event answers 500", "[rest][sse]") {
  auto const answer = answer_to(make_router(), get("/changes/failing"));

  REQUIRE(answer.starts_with("HTTP/1.1 500"));
  REQUIRE(answer.find("internal") == std::string::npos);
}

TEST_CASE("rest events: a scope subscribes with its prefix fixed", "[rest][sse]") {
  changes_client const client{streaming_transport{make_router()}};
  auto const scoped = client.scope<changes::named::api>(std::string{"north"});

  auto const reading = [&scoped]() -> task<std::optional<change>> {
    auto stream = co_await scoped.subscribe<changes::named::feed>();

    co_return co_await stream.next();
  };

  REQUIRE(quoll::test::run(reading()).value() == change{1, "north"});
  REQUIRE(client.transport().last_asked().target == "/feeds/north");
}

TEST_CASE("rest events: a refusal's body is read up to a bound, an undeclared status said so", "[rest][sse]") {
  http::router router;

  router.get("/changes/broken", [](http::request) {
    http::response response;

    response.status = http::status::service_unavailable;
    response.body = std::string(std::size_t{200} * 1024, 'x');
    return response;
  });

  changes_client const client{streaming_transport{std::move(router)}};

  auto const reading = [&client]() -> task<std::string> {
    try {
      static_cast<void>(co_await client.subscribe<changes::broken>());
    } catch (rest::call_error const &failed) {
      co_return failed.failure().reason + "|" + std::to_string(failed.failure().response.body.size());
    }

    co_return "subscribed";
  };

  // Truncated to the bound, though the transport gave it in one piece.
  REQUIRE(quoll::test::run(reading()).value() == "undeclared status 503 (its body read up to 64 KiB)|65536");
}

namespace {

// A 503 whose body comes in pieces of a KiB, `count` of them.
[[nodiscard]] http::router unavailable_in_pieces(int count = 100) {
  http::router router;

  router.get("/changes/broken", [count](http::response_writer out) -> task<> {
    out.status = http::status::service_unavailable;

    std::string const piece(1024, 'x');

    for (int index = 0; index < count; ++index) {
      co_await out.write(piece);
    }
  });
  return router;
}

task<std::string> refusal_of(changes_client const &client) {
  try {
    static_cast<void>(co_await client.subscribe<changes::broken>());
  } catch (rest::call_error const &failed) {
    co_return failed.failure().reason + "|" + std::to_string(failed.failure().response.body.size());
  }

  co_return "subscribed";
}

} // namespace

TEST_CASE("rest events: a refusal's body is read no further than its bound, of pieces as they come", "[rest][sse]") {
  changes_client const client{streaming_transport{unavailable_in_pieces()}};

  REQUIRE(quoll::test::run(refusal_of(client)).value() == "undeclared status 503 (its body read up to 64 KiB)|65536");
  // 64 pieces of a KiB, and no read after: a body that never ends would not hold it.
  REQUIRE(client.transport().reads() == 64);
}

TEST_CASE("rest events: a refusal's body cut short is said so", "[rest][sse]") {
  http::router router;

  router.get("/changes/broken", [](http::response_writer out) -> task<> {
    out.status = http::status::service_unavailable;
    co_await out.write("partial");
  });

  changes_client const client{
      streaming_transport{std::move(router), std::make_error_code(std::errc::connection_reset)}};

  REQUIRE(quoll::test::run(refusal_of(client)).value() == "undeclared status 503 (its body cut short)|7");
}

TEST_CASE("rest events: a stop while a refusal is read goes on as a stop, not as the answer", "[rest][sse]") {
  http::router router;

  router.get("/changes/broken", [](http::response_writer out) -> task<> {
    out.status = http::status::service_unavailable;
    co_await out.write("partial");
  });

  changes_client const client{
      streaming_transport{std::move(router), std::make_error_code(std::errc::operation_canceled)}};

  auto const reading = [&client]() -> task<std::string> {
    try {
      co_return co_await refusal_of(client);
    } catch (std::system_error const &failed) {
      co_return failed.code() == std::errc::operation_canceled ? "canceled" : "other";
    }
  };

  REQUIRE(quoll::test::run(reading()).value() == "canceled");
}

TEST_CASE("rest events: HEAD gets the status and head GET would, no event", "[rest][sse]") {
  auto const head = [](std::string_view target) {
    return answer_to(make_router(),
                     "HEAD " + std::string{target} + " HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  };

  auto const missing = head("/changes/other?count=1");
  auto const found = head("/changes/main?count=1");

  REQUIRE(missing.starts_with("HTTP/1.1 404"));
  REQUIRE(found.starts_with("HTTP/1.1 200"));
  REQUIRE(found.find("Content-Type: text/event-stream\r\n") != std::string::npos);
  REQUIRE(found.find("data:") == std::string::npos);
}

TEST_CASE("rest events: the stream of a response runs once", "[rest][sse]") {
  auto const router = make_router();
  http::request request;

  request.method = http::verb::get;
  request.target = "/changes/main?count=1";

  auto const running = [&router, &request]() -> task<std::string> {
    auto answer = co_await router.dispatch(request);
    auto const writer = answer.late_head_writer;
    gathered first;
    gathered second;
    gathering_sink into_first{&first};
    gathering_sink into_second{&second};

    co_await writer(http::body_stream{into_first});

    try {
      co_await writer(http::body_stream{into_second});
    } catch (std::logic_error const &) {
      co_return std::to_string(first.pieces.size()) + " then refused";
    }

    co_return "ran twice";
  };

  REQUIRE(quoll::test::run(running()).value() == "1 then refused");
}

TEST_CASE("rest events: subscribe sends the credentials, and an Accept the contract declares", "[rest][sse]") {
  http::router router;

  rest::mount<changes::secured::api>(
      router, changes_service{},
      rest::validators([](rest::credential<rest::bearer_auth> const &given) { return given.value == "token"; }));

  rest::client<changes::secured::api, streaming_transport> client{streaming_transport{std::move(router)}};

  client.credentials<rest::bearer_auth>("token");

  auto const reading = [&client]() -> task<std::optional<change>> {
    auto stream = co_await client.subscribe<changes::secured::feed>("application/vnd.changes+event-stream");

    co_return co_await stream.next();
  };

  REQUIRE(quoll::test::run(reading()).value() == change{1, "application/vnd.changes+event-stream"});
  REQUIRE(client.transport().last_asked().fields.find("Authorization") == "Bearer token");
  REQUIRE(client.transport().last_asked().fields.find("Accept") == "application/vnd.changes+event-stream");
}

TEST_CASE("rest events: a timeout while a refusal is read goes on as a timeout", "[rest][sse]") {
  http::router router;

  router.get("/changes/broken", [](http::response_writer out) -> task<> {
    out.status = http::status::service_unavailable;
    co_await out.write("partial");
  });

  changes_client const client{streaming_transport{std::move(router), std::make_error_code(std::errc::timed_out)}};

  auto const reading = [&client]() -> task<std::string> {
    try {
      co_return co_await refusal_of(client);
    } catch (std::system_error const &failed) {
      co_return failed.code() == std::errc::timed_out ? "timed out" : "other";
    }
  };

  REQUIRE(quoll::test::run(reading()).value() == "timed out");
}

TEST_CASE("rest events: through http::client and http::server, events come as they are sent", "[rest][sse]") {
  changes_service const service;
  auto const released = service.released;
  http::router router;

  rest::mount<changes::all>(router, service);

  async::run_loop loop;
  quoll::test::memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};
  auto const reading = [&server, &released]() -> task<std::vector<change>> {
    // In the task: its connections close with it, before the loop stops.
    rest::client<changes::all> const client{http::client{quoll::test::memory_connector{server}, "example.com", 80}};
    auto stream = co_await client.subscribe<changes::paced>();
    std::vector<change> seen;

    // The second is sent only once the first has been read: a stream gathered before it is read would never end.
    seen.push_back(*co_await stream.next());
    *released = true;
    seen.push_back(*co_await stream.next());

    if (co_await stream.next()) {
      seen.push_back({.id = 0, .what = "more"});
    }

    co_return seen;
  };

  REQUIRE(quoll::test::run(loop, reading()).value() == std::vector<change>{{1, "first"}, {2, "second"}});
}

TEST_CASE("rest events: an operation asked and not run starts nothing, and an http_error still answers",
          "[rest][sse]") {
  auto const answer = answer_to(make_router(), get("/changes/opened"));

  REQUIRE(answer.starts_with("HTTP/1.1 404"));
  REQUIRE(answer.find("nothing to open") != std::string::npos);
}

TEST_CASE("rest events: a 204 is said to be no stream", "[rest][sse]") {
  http::router router;

  router.get("/changes/broken", [](http::request) {
    http::response response;

    response.status = http::status::no_content;
    return response;
  });

  changes_client const client{streaming_transport{std::move(router)}};

  REQUIRE(quoll::test::run(refusal_of(client)).value() == "no stream: 204 No Content|0");
}

TEST_CASE("rest events: a contract's own Accept takes the one media type it names", "[rest][sse]") {
  http::router router;

  router.get("/secured", [](http::response_writer out) -> task<> {
    out.fields.set("Content-Type", "application/vnd.changes+event-stream");

    http::event_writer events{std::move(out)};

    co_await events.send({.data = R"({"id":1,"what":"vendor"})"});
  });

  rest::client<changes::secured::api, streaming_transport> client{streaming_transport{std::move(router)}};

  client.credentials<rest::bearer_auth>("token");

  auto const reading = [&client]() -> task<std::optional<change>> {
    auto stream = co_await client.subscribe<changes::secured::feed>("application/vnd.changes+event-stream");

    co_return co_await stream.next();
  };

  REQUIRE(quoll::test::run(reading()).value() == change{1, "vendor"});
}

TEST_CASE("rest events: a refusal through http::client leaves the client whole for what follows", "[rest][sse]") {
  changes_service const service;
  http::router router;

  router.get("/changes/broken", [](http::request) {
    http::response response;

    response.status = http::status::service_unavailable;
    response.body = std::string(std::size_t{200} * 1024, 'x');
    return response;
  });
  rest::mount<changes::named::api>(router, service);

  async::run_loop loop;
  quoll::test::memory_server server{loop.get_executor(), quoll::test::serving(router), 0, {}, {}};

  auto const reading = [&server]() -> task<std::string> {
    http::client_options options;

    // One connection: what follows goes on it, or on one opened in its place.
    options.max_connections = 1;

    rest::client<changes::all> const client{
        http::client{quoll::test::memory_connector{server}, "example.com", 80, options}};
    std::string seen;

    try {
      static_cast<void>(co_await client.subscribe<changes::broken>());
    } catch (rest::call_error const &failed) {
      seen = std::to_string(static_cast<int>(failed.status())) + ";";
    }

    auto stream = co_await client.scope<changes::named::api>(std::string{"after"}).subscribe<changes::named::feed>();
    auto const next = co_await stream.next();

    co_return seen + (next ? next->what : "none");
  };

  REQUIRE(quoll::test::run(loop, reading()).value() == "503;after");
}

namespace {

// What a sink writes, a turn of the loop after it is asked: long enough for another operation to come meanwhile.
struct yielding_sink {
  std::string *sent;

  void head(http::status /*status*/, http::fields /*fields*/) {}

  [[nodiscard]] task<> write(std::string_view piece) {
    co_await async::yield();
    sent->append(piece);
  }
};

} // namespace

TEST_CASE("rest events: a sink refuses an operation asked while another is pending", "[rest][sse]") {
  std::string sent;
  yielding_sink sink{&sent};
  http::body_stream out{sink};

  auto const both = [&out]() -> task<bool> {
    rest::event_sink<change> events{http::response_writer{out}};
    auto first = events.send({.id = 1, .what = "a"});
    bool refused = false;

    try {
      static_cast<void>(events.comment());
    } catch (std::logic_error const &) {
      refused = true;
    }

    co_await std::move(first);
    co_return refused;
  };

  REQUIRE(quoll::test::run(both()) == true);
  REQUIRE(sent == "data: {\"id\":1,\"what\":\"a\"}\n\n");
}

TEST_CASE("rest events: a stream goes on once its router is gone, holding what it needs", "[rest][sse]") {
  auto router = std::make_unique<http::router>(make_router());
  http::request request;

  request.method = http::verb::get;
  request.target = "/changes/main?count=2";

  auto const running = [&router, &request]() -> task<std::size_t> {
    auto answer = co_await router->dispatch(request);
    gathered got;
    gathering_sink into{&got};

    // The router and its services go before the stream runs.
    router.reset();
    co_await answer.late_head_writer(http::body_stream{into});
    co_return got.pieces.size();
  };

  REQUIRE(quoll::test::run(running()).value() == 2);
}

TEST_CASE("rest events: a refusal's body under the bound is read whole", "[rest][sse]") {
  changes_client const client{streaming_transport{unavailable_in_pieces(63)}};

  REQUIRE(quoll::test::run(refusal_of(client)).value() == "undeclared status 503|64512");
}

TEST_CASE("rest events: subscribe refuses a client with interceptors, rather than go without them", "[rest][sse]") {
  changes_client client{streaming_transport{make_router()}};

  client.use([](http::request request, rest::next const &next) { return next(std::move(request)); });

  REQUIRE_THROWS_AS(client.subscribe<changes::none>(), std::logic_error);
}

namespace {

// A sink's check of an operation that refuses before the stream starts.
template <class Operation>
task<std::string> refused_before_start(Operation operation) {
  std::string sent;
  yielding_sink sink{&sent};
  http::body_stream out{sink};
  rest::event_sink<change> events{http::response_writer{out}};
  std::string seen;

  try {
    co_await operation(events);
  } catch (std::invalid_argument const &) {
    seen = "refused;";
  }

  co_return seen + (events.started() ? "started" : "not started");
}

} // namespace

TEST_CASE("rest events: a comment or a retry refused leaves the stream unstarted", "[rest][sse]") {
  REQUIRE(quoll::test::run(refused_before_start([](rest::event_sink<change> &events) {
            return events.comment("a\nb");
          })).value() == "refused;not started");
  REQUIRE(quoll::test::run(refused_before_start([](rest::event_sink<change> &events) {
            return events.retry(std::chrono::milliseconds{-1});
          })).value() == "refused;not started");
}

TEST_CASE("rest events: an argument that points into the request reads it whole, as the stream runs", "[rest][sse]") {
  auto const answer = answer_to(make_router(), "POST /changes/search HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
                                               "Content-Type: application/json\r\nContent-Length: 32\r\n\r\n"
                                               "{\"word\":\"a word long enough xx\"}");

  REQUIRE(answer.starts_with("HTTP/1.1 200"));
  REQUIRE(answer.find(R"(data: {"id":1,"what":"a word long enough xx"})") != std::string::npos);
}

TEST_CASE("rest events: a sink operation that runs once its service has ended throws", "[rest][sse]") {
  std::string sent;
  yielding_sink sink{&sent};
  http::body_stream out{sink};

  auto const running = [&out]() -> task<std::string> {
    rest::event_sink<change> events{http::response_writer{out}};
    auto late = events.send({.id = 1, .what = "late"});

    // What mount does once the service has ended, with an operation kept past it.
    co_await rest::detail::event_sink_access::finish(events);

    try {
      co_await std::move(late);
    } catch (std::logic_error const &) {
      co_return "refused";
    }

    co_return "sent";
  };

  REQUIRE(quoll::test::run(running()).value() == "refused");
  REQUIRE(sent.empty());
}

TEST_CASE("rest events: an operation kept past its service, dropped later, leaves the connection alone",
          "[rest][sse]") {
  changes_service const service;
  auto const held = service.held;
  http::router router;

  rest::mount<changes::all>(router, service);

  auto const answer = answer_to(router, get("/changes/kept"));

  // A stream with no event: the operation kept never ran, and the connection has ended.
  REQUIRE(answer.starts_with("HTTP/1.1 200"));
  REQUIRE(answer.find("Content-Type: text/event-stream\r\n") != std::string::npos);
  REQUIRE(answer.find("data:") == std::string::npos);
  // Dropped once the connection is gone: under ASan, what it held must not reach it.
  held->reset();
}

TEST_CASE("rest events: a value decoded into views of its event is valid until the next one", "[rest][sse]") {
  changes_client const client{streaming_transport{make_router()}};

  auto const reading = [&client]() -> task<std::string> {
    auto stream = co_await client.subscribe<changes::viewed>();
    auto const first = co_await stream.next();

    // Turns of the loop pass, the stream moved: the view still reads its event.
    for (int turn = 0; turn < 8; ++turn) {
      co_await async::yield();
    }

    auto moved = std::move(stream);
    std::string const word{first.value().word};
    auto const second = co_await moved.next();

    co_return word + "|" + std::string{second.value().word};
  };

  REQUIRE(quoll::test::run(reading()).value() ==
          "a word long enough to leave the buffer|another word long enough to leave it");
}

TEST_CASE("rest events: an operation kept by a service that fails, dropped later, leaves the connection alone",
          "[rest][sse]") {
  changes_service const service;
  auto const held = service.held;
  http::router router;

  rest::mount<changes::all>(router, service);

  auto const answer = answer_to(router, get("/changes/kept-then-failing"));

  REQUIRE(answer.starts_with("HTTP/1.1 500"));
  // Dropped once the connection is gone: under ASan, what it held must not reach it.
  held->reset();
}

namespace {

// A codec that runs something while it decodes: another next() of the stream, say.
struct reentrant_codec {
  static constexpr std::string_view content_type = "text/plain";

  // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): set by the one test that uses it.
  static inline std::function<void()> during_decode;

  [[nodiscard]] std::string encode(std::string const &value) const { return value; }

  template <class T>
  [[nodiscard]] std::expected<T, rest::decode_error> decode(std::string_view body) const {
    if (during_decode) {
      during_decode();
    }

    return T{body};
  }
};

} // namespace

TEST_CASE("rest events: a next() asked while one decodes is refused", "[rest][sse]") {
  http::router router;

  router.get("/reentrant", [](http::response_writer out) -> task<> {
    http::event_writer events{std::move(out)};

    co_await events.send({.data = "first"});
    co_await events.send({.data = "second"});
  });

  using reentrant = rest::endpoint<"reentrant", verb::get, "/reentrant", rest::events<std::string, reentrant_codec>>;

  rest::client<rest::api<"", reentrant>, streaming_transport> const client{streaming_transport{std::move(router)}};

  auto const reading = [&client]() -> task<std::string> {
    auto stream = co_await client.subscribe<reentrant>();
    std::string seen;

    reentrant_codec::during_decode = [&stream, &seen] {
      try {
        static_cast<void>(stream.next());
        seen += "accepted;";
      } catch (std::logic_error const &) {
        seen += "refused;";
      }
    };

    auto const first = co_await stream.next();

    reentrant_codec::during_decode = nullptr;
    co_return seen + first.value();
  };

  REQUIRE(quoll::test::run(reading()).value() == "refused;first");
}

TEST_CASE("rest events: a contract's own Accept takes its media type, not any 200", "[rest][sse]") {
  http::router router;

  router.get("/secured", [](http::request) {
    http::response response;

    response.fields.set("Content-Type", "application/json");
    response.body = R"({"not":"a stream"})";
    return response;
  });

  rest::client<changes::secured::api, streaming_transport> client{streaming_transport{std::move(router)}};

  client.credentials<rest::bearer_auth>("token");

  auto const reading = [&client]() -> task<std::string> {
    try {
      static_cast<void>(co_await client.subscribe<changes::secured::feed>("application/vnd.changes+event-stream"));
    } catch (rest::call_error const &failed) {
      co_return failed.failure().reason;
    }

    co_return "subscribed";
  };

  REQUIRE(quoll::test::run(reading()).value() == "not an event stream: Content-Type application/json");
}

TEST_CASE("rest events: next() on a stream moved from is refused", "[rest][sse]") {
  changes_client const client{streaming_transport{make_router()}};

  auto const reading = [&client]() -> task<bool> {
    auto stream = co_await client.subscribe<changes::none>();
    auto const moved = std::move(stream);

    try {
      // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): what is tested.
      static_cast<void>(stream.next());
    } catch (std::logic_error const &) {
      co_return true;
    }

    co_return false;
  };

  REQUIRE(quoll::test::run(reading()) == true);
}

TEST_CASE("rest events: an Accept naming several media types takes text/event-stream only", "[rest][sse]") {
  http::router router;

  router.get("/secured", [](http::request) {
    http::response response;

    response.fields.set("Content-Type", "application/json");
    response.body = R"({"not":"a stream"})";
    return response;
  });

  rest::client<changes::secured::api, streaming_transport> client{streaming_transport{std::move(router)}};

  client.credentials<rest::bearer_auth>("token");

  auto const reading = [&client]() -> task<std::string> {
    try {
      static_cast<void>(co_await client.subscribe<changes::secured::feed>("application/json, text/event-stream"));
    } catch (rest::call_error const &failed) {
      co_return failed.failure().reason;
    }

    co_return "subscribed";
  };

  REQUIRE(quoll::test::run(reading()).value() == "not an event stream: Content-Type application/json");
}

TEST_CASE("rest events: an operation running on a task of its own ends before the stream does", "[rest][sse]") {
  std::string sent;
  yielding_sink sink{&sent};
  http::body_stream out{sink};

  auto const running = [&out, &sent]() -> task<std::string> {
    rest::event_sink<change> events{http::response_writer{out}};

    // Spawned, then left as the service returns: what mount then does waits for it.
    async::co_spawn(co_await async::current_executor(), events.send({.id = 1, .what = "spawned"}), async::detached);
    co_await async::yield();
    co_await rest::detail::event_sink_access::drain(events);

    // Already whole: closing now leaves nothing writing.
    co_return sent;
  };

  REQUIRE(quoll::test::run(running()).value() == "data: {\"id\":1,\"what\":\"spawned\"}\n\n");
}
