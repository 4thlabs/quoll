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

#include <quoll/http/parser.hpp>
#include <quoll/http/serializer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

namespace {

namespace http = quoll::http;

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
  SECTION("header injection through a value") {
    http::request request;

    request.fields.add("X-Name", "value\r\nInjected: yes");

    REQUIRE_THROWS_AS(http::serialize(request), std::invalid_argument);
  }

  SECTION("a field name that is not a token") {
    http::request request;

    request.fields.add("Bad Name", "value");

    REQUIRE_THROWS_AS(http::serialize(request), std::invalid_argument);
  }

  SECTION("a target with a space") {
    http::request request;

    request.target = "/a b";

    REQUIRE_THROWS_AS(http::serialize(request), std::invalid_argument);
  }

  SECTION("trailers without chunked coding") {
    http::response response;

    response.trailers.add("X-Checksum", "abc");

    REQUIRE_THROWS_AS(http::serialize(response), std::invalid_argument);
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
