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
#include <quoll/http/body_stream.hpp>
#include <quoll/http/detail/streamed_head.hpp>
#include <quoll/http/detail/streamed_wire.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/serializer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

namespace {

namespace http = quoll::http;

// Rejected, and nothing of it is written.
template <class Message>
void require_rejected(Message const &message) {
  std::string out = "already here";

  REQUIRE_THROWS_AS(http::serialize(message, out), std::invalid_argument);
  REQUIRE(out == "already here");
  REQUIRE_THROWS_AS(http::serialize(message), std::invalid_argument);
}

} // namespace

TEST_CASE("A GET request is serialized without framing", "[http][serializer]") {
  http::request request;

  request.target = "/accounts/42";

  request.fields.add("Host", "example.com");

  REQUIRE(http::serialize(request) == "GET /accounts/42 HTTP/1.1\r\n"
                                      "Host: example.com\r\n"
                                      "\r\n");
}

TEST_CASE("A request body gets a Content-Length", "[http][serializer]") {
  SECTION("with a body") {
    http::request request;

    request.method = http::verb::post;
    request.target = "/accounts";
    request.body = "{}";

    REQUIRE(http::serialize(request) == "POST /accounts HTTP/1.1\r\n"
                                        "Content-Length: 2\r\n"
                                        "\r\n"
                                        "{}");
  }

  SECTION("an empty body of a method that has one") {
    http::request request;

    request.method = http::verb::put;
    request.target = "/flag";

    REQUIRE(http::serialize(request) == "PUT /flag HTTP/1.1\r\n"
                                        "Content-Length: 0\r\n"
                                        "\r\n");
  }

  SECTION("an explicit Content-Length is kept") {
    http::request request;

    request.method = http::verb::post;
    request.body = "abc";

    request.fields.add("Content-Length", "3");

    REQUIRE(http::serialize(request) == "POST / HTTP/1.1\r\n"
                                        "Content-Length: 3\r\n"
                                        "\r\n"
                                        "abc");
  }
}

TEST_CASE("Appending keeps what the buffer holds", "[http][serializer]") {
  std::string out = "already here";

  http::serialize(http::request{}, out);

  REQUIRE(out.starts_with("already hereGET / HTTP/1.1\r\n"));
}

TEST_CASE("A CONNECT has no body, as what follows its head is the tunnel", "[http][serializer]") {
  http::request request;

  request.method = http::verb::connect;
  request.target = "example.org:443";
  request.fields.add("Host", "example.org:443");

  REQUIRE(http::serialize(request) == "CONNECT example.org:443 HTTP/1.1\r\nHost: example.org:443\r\n\r\n");

  SECTION("with chunked coding, whose terminator would go down the tunnel") {
    request.fields.add("Transfer-Encoding", "chunked");

    require_rejected(request);
  }

  SECTION("with a body") {
    request.body = "abc";

    require_rejected(request);
  }
}

TEST_CASE("A response gets its reason phrase and framing", "[http][serializer]") {
  SECTION("200 with a body") {
    http::response response;

    response.body = "hello";

    REQUIRE(http::serialize(response) == "HTTP/1.1 200 OK\r\n"
                                         "Content-Length: 5\r\n"
                                         "\r\n"
                                         "hello");
  }

  SECTION("204 has no Content-Length") {
    http::response response;

    response.status = http::status::no_content;

    REQUIRE(http::serialize(response) == "HTTP/1.1 204 No Content\r\n\r\n");
  }

  SECTION("an unnamed code keeps an empty reason") {
    http::response response;

    response.status = static_cast<http::status>(299);

    REQUIRE(http::serialize(response) == "HTTP/1.1 299 \r\n"
                                         "Content-Length: 0\r\n"
                                         "\r\n");
  }
}

TEST_CASE("A response may announce a length it does not send", "[http][serializer]") {
  SECTION("response to HEAD") {
    http::response response;

    response.fields.add("Content-Length", "12");

    REQUIRE(http::serialize(response) == "HTTP/1.1 200 OK\r\n"
                                         "Content-Length: 12\r\n"
                                         "\r\n");
  }

  SECTION("304") {
    http::response response;

    response.status = http::status::not_modified;
    response.fields.add("Content-Length", "12");

    REQUIRE(http::serialize(response) == "HTTP/1.1 304 Not Modified\r\n"
                                         "Content-Length: 12\r\n"
                                         "\r\n");
  }
}

TEST_CASE("Chunked coding may follow other codings", "[http][serializer]") {
  http::request request;

  request.method = http::verb::post;
  request.body = "abc";
  request.fields.add("Transfer-Encoding", "gzip, chunked");

  REQUIRE(http::serialize(request) == "POST / HTTP/1.1\r\n"
                                      "Transfer-Encoding: gzip, chunked\r\n"
                                      "\r\n"
                                      "3\r\nabc\r\n"
                                      "0\r\n"
                                      "\r\n");
}

TEST_CASE("A chunked response carries its trailers", "[http][serializer]") {
  http::response response;

  response.body = "hello world";

  response.fields.add("Transfer-Encoding", "chunked");
  response.trailers.add("X-Checksum", "abc");

  REQUIRE(http::serialize(response) == "HTTP/1.1 200 OK\r\n"
                                       "Transfer-Encoding: chunked\r\n"
                                       "\r\n"
                                       "b\r\nhello world\r\n"
                                       "0\r\n"
                                       "X-Checksum: abc\r\n"
                                       "\r\n");
}

TEST_CASE("What cannot be sent is rejected", "[http][serializer]") {
  SECTION("a tab in Transfer-Encoding, which parsers do not read alike") {
    http::response response;

    response.fields.add("Transfer-Encoding", "chunked\t");

    require_rejected(response);
  }

  SECTION("a tab in Content-Length, which parsers do not read alike") {
    http::response response;

    response.body = "hello";
    response.fields.add("Content-Length", "5\t");

    require_rejected(response);
  }

  SECTION("framing in the trailers") {
    http::response response;

    response.fields.add("Transfer-Encoding", "chunked");
    response.trailers.add("Content-Length", "5");

    require_rejected(response);
  }

  SECTION("header injection through a value") {
    http::request request;

    request.fields.add("X-Name", "value\r\nInjected: yes");

    require_rejected(request);
  }

  SECTION("a field name that is not a token") {
    http::request request;

    request.fields.add("Bad Name", "value");

    require_rejected(request);
  }

  SECTION("a target with a space") {
    http::request request;

    request.target = "/a b";

    require_rejected(request);
  }

  SECTION("a body on a response that cannot have one") {
    http::response response;

    response.status = http::status::no_content;
    response.body = "unexpected";

    require_rejected(response);
  }

  SECTION("chunked coding on a 304") {
    http::response response;

    response.status = http::status::not_modified;
    response.fields.add("Transfer-Encoding", "chunked");

    require_rejected(response);
  }

  SECTION("Content-Length together with Transfer-Encoding") {
    http::request request;

    request.method = http::verb::post;
    request.body = "abc";
    request.fields.add("Content-Length", "3");
    request.fields.add("Transfer-Encoding", "chunked");

    require_rejected(request);
  }

  SECTION("a Content-Length that does not match the body") {
    http::request request;

    request.method = http::verb::post;
    request.body = "hello world";
    request.fields.add("Content-Length", "5");

    require_rejected(request);
  }

  SECTION("Content-Length fields that disagree") {
    http::response response;

    response.body = "abc";
    response.fields.add("Content-Length", "3");
    response.fields.add("Content-Length", "4");

    require_rejected(response);
  }

  SECTION("a last transfer coding other than chunked") {
    http::request request;

    request.method = http::verb::post;
    request.body = "abc";
    request.fields.add("Transfer-Encoding", "chunked");
    request.fields.add("Transfer-Encoding", "gzip");

    require_rejected(request);
  }

  SECTION("a coding that only ends with chunked") {
    http::request request;

    request.method = http::verb::post;
    request.body = "abc";
    request.fields.add("Transfer-Encoding", "x-chunked");

    require_rejected(request);
  }

  SECTION("chunked coding in HTTP/1.0") {
    http::response response;

    response.version = http::version::http_1_0;
    response.fields.add("Transfer-Encoding", "chunked");

    require_rejected(response);
  }

  SECTION("Content-Length on a 204") {
    http::response response;

    response.status = http::status::no_content;
    response.fields.add("Content-Length", "0");

    require_rejected(response);
  }

  SECTION("trailers without chunked coding") {
    http::response response;

    response.trailers.add("X-Checksum", "abc");

    require_rejected(response);
  }
}

// A member with no initializer of its own would only warn, and fail nothing, without this.
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wmissing-field-initializers"

TEST_CASE("A message takes designated initializers, whichever members they name", "[http][serializer]") {
  http::request const request{.method = http::verb::post, .target = "/up", .body = "hi"};
  http::response const response{.status = http::status::created, .body = "made"};
  http::response const empty{.status = http::status::ok};
  http::response const nothing{.status = http::status::no_content};

  REQUIRE(http::serialize(request) == "POST /up HTTP/1.1\r\nContent-Length: 2\r\n\r\nhi");
  REQUIRE(http::serialize(response) == "HTTP/1.1 201 Created\r\nContent-Length: 4\r\n\r\nmade");
  REQUIRE(http::serialize(empty) == "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
  REQUIRE(http::serialize(nothing) == "HTTP/1.1 204 No Content\r\n\r\n");
}

#pragma GCC diagnostic pop

TEST_CASE("A response with a writer goes in pieces", "[http][serializer]") {
  http::response response;
  std::string out;

  response.late_head_writer = [](http::body_stream) -> quoll::async::task<> { co_return; };

  REQUIRE_THROWS_AS(http::serialize(response, out), std::invalid_argument);

  response.late_head_writer = nullptr;
  response.body_writer = [](http::body_stream) -> quoll::async::task<> { co_return; };

  REQUIRE_THROWS_AS(http::serialize(response, out), std::invalid_argument);
}

TEST_CASE("A request with a body_writer goes in pieces", "[http][serializer]") {
  http::request request;

  request.method = http::verb::post;
  request.target = "/up";
  request.body_writer = [](http::body_stream) -> quoll::async::task<> { co_return; };

  SECTION("not whole") { REQUIRE_THROWS_AS(http::serialize(request), std::invalid_argument); }

  SECTION("in chunks, unless it says how much comes") {
    std::string head;
    auto const framing = http::detail::serialize_streamed_head(request, head);

    REQUIRE(framing.chunked);
    REQUIRE(head == "POST /up HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    head.clear();
    request.fields.set("Content-Length", "4");

    auto const sized = http::detail::serialize_streamed_head(request, head);

    REQUIRE(!sized.chunked);
    REQUIRE(sized.length == 4);
    REQUIRE(head == "POST /up HTTP/1.1\r\nContent-Length: 4\r\n\r\n");
  }

  SECTION("its own chunks, with trailers") {
    std::string head;

    request.fields.set("Transfer-Encoding", "chunked");
    request.trailers.add("X-Sum", "1");

    auto const framing = http::detail::serialize_streamed_head(request, head);

    REQUIRE(framing.chunked);
    REQUIRE(head == "POST /up HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
  }

  SECTION("what it cannot be") {
    std::string head;
    auto const refused = [&] { static_cast<void>(http::detail::serialize_streamed_head(request, head)); };

    SECTION("a body too") {
      request.body = "whole";
      REQUIRE_THROWS_AS(refused(), std::invalid_argument);
    }

    SECTION("HTTP/1.0 with no length, which has no chunks") {
      request.version = http::version::http_1_0;
      REQUIRE_THROWS_AS(refused(), std::invalid_argument);
      request.fields.set("Content-Length", "4");
      REQUIRE_NOTHROW(refused());
      head.clear();
    }

    SECTION("a TRACE, which RFC 9110 forbids content") {
      request.method = http::verb::trace;
      REQUIRE_THROWS_AS(refused(), std::invalid_argument);
    }

    SECTION("a CONNECT, whose bytes are the tunnel's") {
      request.method = http::verb::connect;
      request.target = "example.com:443";
      REQUIRE_THROWS_AS(refused(), std::invalid_argument);
    }

    SECTION("framing in its trailers") {
      request.trailers.add("Content-Length", "4");
      REQUIRE_THROWS_AS(refused(), std::invalid_argument);
    }

    REQUIRE(head.empty());
  }
}

TEST_CASE("streamed_wire: a body in pieces, framed", "[http][serializer]") {
  SECTION("in chunks, the head with the first") {
    http::detail::streamed_wire wire{"HEAD\r\n", {.chunked = true}};

    REQUIRE(!wire.started());
    REQUIRE(wire.frame("ab") == "HEAD\r\n2\r\nab\r\n");
    REQUIRE(wire.started());
    REQUIRE(wire.frame("").empty());
    REQUIRE(wire.frame("c") == "1\r\nc\r\n");
    REQUIRE(wire.end({}) == "0\r\n\r\n");
  }

  SECTION("an empty piece sends the head") {
    http::detail::streamed_wire wire{"HEAD", {.chunked = true}};

    REQUIRE(wire.frame("") == "HEAD");
    REQUIRE(wire.frame("").empty());
  }

  SECTION("nothing written: the head and its end") {
    http::detail::streamed_wire wire{"HEAD", {.chunked = true}};

    REQUIRE(wire.end({}) == "HEAD0\r\n\r\n");
  }

  SECTION("of a known length, not copied past the head") {
    http::detail::streamed_wire wire{"HEAD", {.length = 3}};
    std::string_view const last = "c";

    REQUIRE(wire.short_of_its_length());
    REQUIRE(wire.frame("ab") == "HEADab");
    REQUIRE(wire.past_its_length("cd"));
    REQUIRE(!wire.past_its_length(last));

    auto const framed = wire.frame(last);

    REQUIRE(framed.data() == last.data());
    REQUIRE(!wire.short_of_its_length());
    REQUIRE(wire.end({}).empty());
  }
}

TEST_CASE("A serialized request parses back to the same request", "[http][serializer][parser]") {
  http::request original;

  original.method = http::verb::patch;
  original.target = "/accounts/42";
  original.body = "{\"name\":\"quoll\"}";

  original.fields.add("Host", "example.com");
  original.fields.add("Content-Type", "application/json");

  auto const wire = http::serialize(original);
  http::request_parser parser;

  auto const result = parser.feed({wire.data(), wire.size()});

  REQUIRE_FALSE(result.error);
  REQUIRE(parser.done());

  auto const parsed = parser.release();

  REQUIRE(parsed.method == original.method);
  REQUIRE(parsed.target == original.target);
  REQUIRE(parsed.body == original.body);
  REQUIRE(parsed.fields.find("Content-Type") == "application/json");
  REQUIRE(parsed.fields.find("Content-Length") == "16");
}

TEST_CASE("What a peer would read differently is refused", "[http][serializer]") {
  auto const refused = [](http::request const &message) {
    std::string out;

    REQUIRE_THROWS_AS(http::serialize(message, out), std::invalid_argument);
  };

  http::request plain;

  plain.method = http::verb::post;
  plain.target = "/";

  SECTION("chunked applied twice, on one line or two") {
    auto twice = plain;

    twice.fields.add("Transfer-Encoding", "chunked, chunked");
    refused(twice);

    auto on_two_lines = plain;

    on_two_lines.fields.add("Transfer-Encoding", "chunked");
    on_two_lines.fields.add("Transfer-Encoding", "chunked");
    refused(on_two_lines);
  }

  SECTION("but not what is quoted in a parameter of a coding") {
    for (std::string_view const list : {R"(custom; p="a,chunked,b", chunked)", R"(custom; p="\",chunked", chunked)"}) {
      auto quoted = plain;

      quoted.fields.add("Transfer-Encoding", list);

      std::string out;

      REQUIRE_NOTHROW(http::serialize(quoted, out));
    }

    auto after = plain;

    after.fields.add("Transfer-Encoding", R"(chunked, custom; p="a")");
    refused(after);
  }

  SECTION("a Content-Length repeated, even with the same value") {
    auto repeated = plain;

    repeated.body = "abc";
    repeated.fields.add("Content-Length", "3");
    repeated.fields.add("Content-Length", "3");
    refused(repeated);
  }

  SECTION("a target with what is not visible ASCII") {
    for (std::string_view const target : {"/a\tb",
                                          "/a\x7f"
                                          "b",
                                          "/a\x01", "/a\xc3\xa9", "/a\x0b"}) {
      auto invisible = plain;

      invisible.target = target;
      refused(invisible);
    }
  }

  SECTION("a control character in a field value, but for a tab") {
    auto control = plain;

    control.fields.add("X-Id", "\x01");
    refused(control);

    auto tab = plain;

    tab.fields.add("X-Id", "a\tb");

    std::string out;

    REQUIRE_NOTHROW(http::serialize(tab, out));
  }

  SECTION("a body in a TRACE") {
    auto trace = plain;

    trace.method = http::verb::trace;
    trace.body = "x";
    refused(trace);
  }
}

TEST_CASE("A Content-Length past 32 bits is one", "[http][serializer]") {
  http::fields entries;

  entries.add("Content-Length", "5000000000");

  REQUIRE(http::detail::announced_length(entries) == 5'000'000'000U);
}
