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

#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/io/error.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/server/connection_options.hpp>
#include <quoll/server/serve_connection.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace http = quoll::http;
namespace io = quoll::io;

using http::verb;
using quoll::async::task;

http::router make_router() {
  http::router router;

  router.add(verb::get, "/hello", "hello", [](http::request, http::route_match) -> task<http::response> {
    http::response response;

    response.body = "hello";
    co_return response;
  });
  router.add(verb::post, "/echo", "echo", [](http::request request, http::route_match) -> task<http::response> {
    http::response response;

    response.body = std::move(request.body);
    co_return response;
  });
  router.add(verb::get, "/fail", "fail", [](http::request, http::route_match) -> task<http::response> {
    throw std::runtime_error{"handler failure"};
  });
  router.add(verb::get, "/chunked", "chunked", [](http::request, http::route_match) -> task<http::response> {
    http::response response;

    response.fields.add("Transfer-Encoding", "chunked");
    response.body = "in chunks";
    co_return response;
  });
  router.add(verb::get, "/bye", "bye", [](http::request, http::route_match) -> task<http::response> {
    http::response response;

    response.fields.add("Connection", "keep-alive, Close");
    response.body = "bye";
    co_return response;
  });
  router.add(verb::get, "/invalid", "invalid", [](http::request, http::route_match) -> task<http::response> {
    // A 204 cannot have a body: the serializer refuses it.
    http::response response;

    response.status = http::status::no_content;
    response.body = "not allowed";
    co_return response;
  });
  return router;
}

struct exchanged {
  std::vector<http::response> responses;
  // Whether the server closed the connection after the last one.
  bool closed = false;
};

// The client end: sends `raw` at once, reads a response per entry of `methods` (the method of the request it answers),
// then whether the server closed. Owns its end: the server sees the end of the stream when this returns.
task<exchanged> exchange(io::memory_stream client, std::string raw, std::vector<verb> methods, bool expect_close) {
  exchanged result;
  std::string buffer;

  (void)co_await io::write(client, std::as_bytes(std::span{raw}));

  for (auto const method : methods) {
    http::response_parser parser;

    parser.set_request_method(method);

    if (auto const error = co_await http::read(client, buffer, parser)) {
      co_return result;
    }

    result.responses.push_back(parser.release());
  }

  if (expect_close) {
    std::byte byte{};
    auto const received = co_await client.read_some(std::span{&byte, 1});

    result.closed = received.error == io::error::end_of_stream;
  }

  co_return result;
}

exchanged serve(std::string raw, std::vector<verb> methods, bool expect_close = false,
                quoll::server::connection_options const &options = {}) {
  auto const router = make_router();
  auto [client, server] = io::make_memory_stream_pair();
  auto outcome = quoll::test::run(
      quoll::async::when_all(quoll::server::serve_connection(std::move(server), router, options),
                             exchange(std::move(client), std::move(raw), std::move(methods), expect_close)));

  return std::move(std::get<1>(outcome.value()));
}

} // namespace

TEST_CASE("serve_connection: requests get their responses, on a connection kept alive", "[server]") {
  auto const result = serve("GET /hello HTTP/1.1\r\nHost: x\r\n\r\n"
                            "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\n\r\nping",
                            {verb::get, verb::post});

  REQUIRE(result.responses.size() == 2);
  REQUIRE(result.responses[0].status == http::status::ok);
  REQUIRE(result.responses[0].body == "hello");
  REQUIRE(result.responses[0].fields.find("Content-Length") == "5");
  REQUIRE_FALSE(result.responses[0].fields.contains("Connection"));
  REQUIRE(result.responses[1].body == "ping");
}

TEST_CASE("serve_connection: a request that does not keep the connection alive ends it", "[server]") {
  auto const closing = serve("GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", {verb::get}, true);

  REQUIRE(closing.responses.front().fields.find("Connection") == "close");
  REQUIRE(closing.closed);

  auto const old = serve("GET /hello HTTP/1.0\r\n\r\n", {verb::get}, true);

  REQUIRE(old.responses.front().body == "hello");
  REQUIRE(old.closed);
}

TEST_CASE("serve_connection: HEAD announces the length of a body it does not send", "[server]") {
  auto const result =
      serve("HEAD /hello HTTP/1.1\r\nHost: x\r\n\r\nGET /hello HTTP/1.1\r\nHost: x\r\n\r\n", {verb::head, verb::get});

  REQUIRE(result.responses[0].fields.find("Content-Length") == "5");
  REQUIRE(result.responses[0].body.empty());
  // The connection stays in step: the next response is read whole.
  REQUIRE(result.responses[1].body == "hello");
}

TEST_CASE("serve_connection: what cannot be read answers its status and ends the connection", "[server]") {
  auto const garbage = serve("NOT HTTP\r\n\r\n", {verb::get}, true);

  REQUIRE(garbage.responses.front().status == http::status::bad_request);
  REQUIRE(garbage.responses.front().fields.find("Connection") == "close");
  REQUIRE(garbage.closed);
  REQUIRE(serve("PROPFIND /x HTTP/1.1\r\nHost: x\r\n\r\n", {verb::get}, true).responses.front().status ==
          http::status::not_implemented);

  quoll::server::connection_options const small{.limits = {.max_header_size = 64, .max_body_size = 4},
                                                .on_handler_error = {}};
  auto const long_header = serve("GET /hello HTTP/1.1\r\nHost: x\r\nX-Long: " + std::string(100, 'a') + "\r\n\r\n",
                                 {verb::get}, true, small);

  REQUIRE(long_header.responses.front().status == http::status::request_header_fields_too_large);
  REQUIRE(long_header.responses.front().reason == "Request Header Fields Too Large");
  REQUIRE(serve("POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n0123456789", {verb::post}, true, small)
              .responses.front()
              .status == http::status::content_too_large);
}

TEST_CASE("serve_connection: a handler that fails answers 500, and the connection goes on", "[server]") {
  auto const result = serve("GET /fail HTTP/1.1\r\nHost: x\r\n\r\n"
                            "GET /invalid HTTP/1.1\r\nHost: x\r\n\r\n"
                            "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n",
                            {verb::get, verb::get, verb::get});

  REQUIRE(result.responses[0].status == http::status::internal_server_error);
  REQUIRE(result.responses[1].status == http::status::internal_server_error);
  REQUIRE(result.responses[2].body == "hello");
}

TEST_CASE("serve_connection: the router's own answers go out like any response", "[server]") {
  auto const result = serve("DELETE /hello HTTP/1.1\r\nHost: x\r\n\r\nGET /nowhere HTTP/1.1\r\nHost: x\r\n\r\n",
                            {verb::delete_, verb::get});

  REQUIRE(result.responses[0].status == http::status::method_not_allowed);
  REQUIRE(result.responses[0].fields.find("Allow") == "GET, HEAD");
  REQUIRE(result.responses[1].status == http::status::not_found);
}

TEST_CASE("serve_connection: HEAD on a chunked response sends a length, not the terminating chunk", "[server]") {
  auto const result =
      serve("HEAD /chunked HTTP/1.1\r\nHost: x\r\n\r\nGET /hello HTTP/1.1\r\nHost: x\r\n\r\n", {verb::head, verb::get});

  REQUIRE(result.responses[0].fields.find("Content-Length") == "9");
  REQUIRE_FALSE(result.responses[0].fields.contains("Transfer-Encoding"));
  // No stray terminating chunk: the next response is read whole.
  REQUIRE(result.responses[1].body == "hello");
}

TEST_CASE("serve_connection: a handler's Connection: close ends the connection", "[server]") {
  auto const result = serve("GET /bye HTTP/1.1\r\nHost: x\r\n\r\n", {verb::get}, true);

  REQUIRE(result.responses.front().body == "bye");
  REQUIRE(result.closed);
}

TEST_CASE("serve_connection: what a handler throws goes to on_handler_error", "[server]") {
  std::string reported;
  quoll::server::connection_options options;

  options.on_handler_error = [&](std::exception_ptr failure) {
    try {
      std::rethrow_exception(failure);
    } catch (std::exception const &error) {
      reported = error.what();
    }
  };

  REQUIRE(serve("GET /fail HTTP/1.1\r\nHost: x\r\n\r\n", {verb::get}, false, options).responses.front().status ==
          http::status::internal_server_error);
  REQUIRE(reported == "handler failure");
}
