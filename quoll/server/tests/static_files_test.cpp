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

#include <quoll/async/run_loop.hpp>
#include <quoll/async/task.hpp>
#include <quoll/async/when_all.hpp>
#include <quoll/http/body_stream.hpp>
#include <quoll/http/local_transport.hpp>
#include <quoll/http/next.hpp>
#include <quoll/http/parser.hpp>
#include <quoll/http/read.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/io/memory_stream.hpp>
#include <quoll/io/write.hpp>
#include <quoll/server/connection_options.hpp>
#include <quoll/server/detail/open_beneath.hpp>
#include <quoll/server/server.hpp>
#include <quoll/server/static_files.hpp>
#include <quoll/test/run.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace http = quoll::http;
namespace io = quoll::io;

using http::status;
using http::verb;
using quoll::async::task;

// A directory of its own, gone with this.
class temporary_directory {
public:
  temporary_directory() {
    static std::atomic<int> count{0};

    path_ = fs::temp_directory_path() / ("quoll_static_" + std::to_string(::getpid()) + "_" + std::to_string(++count));
    fs::create_directories(path_);
  }

  temporary_directory(temporary_directory const &) = delete;
  temporary_directory(temporary_directory &&) = delete;
  temporary_directory &operator=(temporary_directory const &) = delete;
  temporary_directory &operator=(temporary_directory &&) = delete;

  ~temporary_directory() {
    std::error_code ignored;

    fs::remove_all(path_, ignored);
  }

  [[nodiscard]] fs::path const &path() const noexcept { return path_; }

  void write(fs::path const &name, std::string_view content) const {
    fs::create_directories((path_ / name).parent_path());
    std::ofstream{path_ / name, std::ios::binary} << content;
  }

private:
  fs::path path_;
};

http::request make_request(std::string target, verb method = verb::get) {
  http::request request;

  request.method = method;
  request.target = std::move(target);
  return request;
}

// What `request` gets from `router`, with its body whole.
http::response ask(http::router const &router, http::request request) {
  quoll::async::run_loop loop;
  http::local_transport const transport{router};

  auto const asking = [&]() -> task<http::response> { co_return co_await transport.send(std::move(request)); };

  return quoll::test::run(loop, asking()).value();
}

http::response ask(http::router const &router, std::string target) {
  return ask(router, make_request(std::move(target)));
}

// A site, and a router with it under `prefix`.
struct site {
  temporary_directory directory;
  http::router router;

  explicit site(std::string_view prefix = "/assets", http::static_files_options options = {}) {
    directory.write("hello.txt", "hello");
    directory.write("page.html", "<p>page</p>");
    directory.write("sub/deep.css", "body{}");
    directory.write("docs/index.html", "<p>docs</p>");
    directory.write(".env", "SECRET=1");
    directory.write(".git/config", "secret");
    directory.write("with space.txt", "spaced");
    directory.write("100%.txt", "percent");
    directory.write("data.bin", std::string_view{"\0\x01\xFF\r\n", 5});
    directory.write("empty.txt", "");
    directory.write("UPPER.PNG", "png");
    directory.write("unknown.quoll", "?");
    router.use(http::static_files(prefix, directory.path(), std::move(options)));
    router.get("/assets/route", [](http::request) {
      http::response response;

      response.body = "from the route";
      return response;
    });
    router.post("/assets/only-post", [](http::request) { return http::response{}; });
  }
};

} // namespace

TEST_CASE("static_files: serves a file, with what describes it", "[server][static]") {
  site const served;
  auto const response = ask(served.router, "/assets/hello.txt");

  REQUIRE(response.status == status::ok);
  REQUIRE(response.body == "hello");
  REQUIRE(response.fields.find("Content-Type") == "text/plain; charset=utf-8");
  REQUIRE(response.fields.find("Content-Length") == "5");
  REQUIRE(response.fields.find("Accept-Ranges") == "bytes");
  REQUIRE(response.fields.find("ETag").value_or("").starts_with('"'));
  REQUIRE(response.fields.find("Last-Modified").value_or("").ends_with(" GMT"));
}

TEST_CASE("static_files: what a file holds and how it is named", "[server][static]") {
  site const served;

  SECTION("in a directory") { REQUIRE(ask(served.router, "/assets/sub/deep.css").body == "body{}"); }

  SECTION("bytes as they are") {
    REQUIRE(ask(served.router, "/assets/data.bin").body == std::string_view{"\0\x01\xFF\r\n", 5});
  }

  SECTION("nothing at all") {
    auto const response = ask(served.router, "/assets/empty.txt");

    REQUIRE(response.status == status::ok);
    REQUIRE(response.body.empty());
    REQUIRE(response.fields.find("Content-Length") == "0");
  }

  SECTION("a name to decode") {
    REQUIRE(ask(served.router, "/assets/with%20space.txt").body == "spaced");
    REQUIRE(ask(served.router, "/assets/100%25.txt").body == "percent");
  }

  SECTION("types") {
    REQUIRE(ask(served.router, "/assets/page.html").fields.find("Content-Type") == "text/html; charset=utf-8");
    REQUIRE(ask(served.router, "/assets/UPPER.PNG").fields.find("Content-Type") == "image/png");
    REQUIRE(ask(served.router, "/assets/unknown.quoll").fields.find("Content-Type") == "application/octet-stream");
  }
}

TEST_CASE("static_files: a content type of the application's", "[server][static]") {
  site const served{"/assets", {.content_types = {{"quoll", "application/x-quoll"}, {"txt", "text/x-mine"}}}};

  REQUIRE(ask(served.router, "/assets/unknown.quoll").fields.find("Content-Type") == "application/x-quoll");
  REQUIRE(ask(served.router, "/assets/hello.txt").fields.find("Content-Type") == "text/x-mine");
}

TEST_CASE("static_files: what is not there, or not to be served, stays a 404", "[server][static]") {
  site const served;

  auto const target = GENERATE(as<std::string>{}, "/assets/missing.txt", "/assets/.env", "/assets/.git/config",
                               "/assets/sub", "/assets/hello.txt/", "/assets//hello.txt", "/assets/sub%2Fdeep.css",
                               "/other/hello.txt", "/assetshello.txt");

  CAPTURE(target);
  REQUIRE(ask(served.router, target).status == status::not_found);
}

TEST_CASE("static_files: a path that climbs out is refused by the router first", "[server][static]") {
  site const served;

  auto const target = GENERATE(as<std::string>{}, "/assets/../hello.txt", "/assets/%2e%2e/hello.txt",
                               "/assets/sub/..%2F..%2Fhello.txt", "/assets/a%00b");

  CAPTURE(target);

  auto const response = ask(served.router, target);

  REQUIRE(response.status == status::bad_request);
  REQUIRE(response.body != "hello");
}

TEST_CASE("static_files: dotfiles, when asked for", "[server][static]") {
  site const served{"/assets", {.dotfiles = true}};

  REQUIRE(ask(served.router, "/assets/.env").body == "SECRET=1");
}

TEST_CASE("static_files: a link leading out of the root is not followed", "[server][static]") {
  temporary_directory outside;
  site const served;

  outside.write("secret.txt", "secret");
  fs::create_symlink(outside.path() / "secret.txt", served.directory.path() / "out.txt");
  fs::create_directory_symlink(outside.path(), served.directory.path() / "outdir");
  fs::create_symlink(served.directory.path() / "hello.txt", served.directory.path() / "in.txt");

  REQUIRE(ask(served.router, "/assets/out.txt").status == status::not_found);
  REQUIRE(ask(served.router, "/assets/outdir/secret.txt").status == status::not_found);
  REQUIRE(ask(served.router, "/assets/in.txt").body == "hello");
}

#if !defined(__EMSCRIPTEN__)

TEST_CASE("static_files: a FIFO is no file, and does not hold up the request", "[server][static]") {
  site const served;

  REQUIRE(::mkfifo((served.directory.path() / "pipe").c_str(), 0600) == 0);
  REQUIRE(ask(served.router, "/assets/pipe").status == status::not_found);
}

#endif

TEST_CASE("static_files: routes and other methods come first", "[server][static]") {
  site const served;

  SECTION("a route wins") { REQUIRE(ask(served.router, "/assets/route").body == "from the route"); }

  SECTION("a 404 of a route is that route's, whatever files there are") {
    http::router router;

    router.use(http::static_files("/assets", served.directory.path()));
    router.get("/assets/hello.txt", [](http::request) {
      http::response response;

      response.status = status::not_found;
      response.body = "not for you";
      return response;
    });

    auto const response = ask(router, "/assets/hello.txt");

    REQUIRE(response.status == status::not_found);
    REQUIRE(response.body == "not for you");
  }

  SECTION("a route of another method keeps its 405") {
    served.directory.write("only-post", "file");
    REQUIRE(ask(served.router, "/assets/only-post").status == status::method_not_allowed);
  }

  SECTION("a POST on a file is no request for it") {
    REQUIRE(ask(served.router, make_request("/assets/hello.txt", verb::post)).status == status::not_found);
  }
}

TEST_CASE("static_files: HEAD says what GET would, with no body", "[server][static]") {
  site const served;
  auto const response = ask(served.router, make_request("/assets/hello.txt", verb::head));

  REQUIRE(response.status == status::ok);
  REQUIRE(response.body.empty());
  REQUIRE(response.fields.find("Content-Length") == "5");
}

TEST_CASE("static_files: a directory serves its index", "[server][static]") {
  site const served;

  SECTION("with its final slash") {
    auto const response = ask(served.router, "/assets/docs/");

    REQUIRE(response.body == "<p>docs</p>");
    REQUIRE(response.fields.find("Content-Type") == "text/html; charset=utf-8");
  }

  SECTION("without it, a redirect that stays where it is") {
    auto const response = ask(served.router, "/assets/docs");

    REQUIRE(response.status == status::moved_permanently);
    REQUIRE(response.fields.find("Location") == "./docs/");
  }

  SECTION("no index there") { REQUIRE(ask(served.router, "/assets/sub/").status == status::not_found); }
}

TEST_CASE("static_files: no index, when asked for", "[server][static]") {
  site const served{"/assets", {.index = ""}};

  REQUIRE(ask(served.router, "/assets/docs/").status == status::not_found);
  REQUIRE(ask(served.router, "/assets/docs").status == status::not_found);
}

TEST_CASE("static_files: under the root of a site", "[server][static]") {
  site const served{"/"};

  REQUIRE(ask(served.router, "/hello.txt").body == "hello");
  REQUIRE(ask(served.router, "/docs/").body == "<p>docs</p>");
  REQUIRE(ask(served.router, "/assets/route").body == "from the route");
}

TEST_CASE("static_files: a request not to send again", "[server][static]") {
  site const served;
  auto const first = ask(served.router, "/assets/hello.txt");
  auto const etag = std::string{first.fields.find("ETag").value()};
  auto const date = std::string{first.fields.find("Last-Modified").value()};

  auto const with = [&](std::string_view name, std::string value) {
    auto request = make_request("/assets/hello.txt");

    request.fields.add(name, std::move(value));
    return ask(served.router, std::move(request));
  };

  SECTION("by its tag") {
    auto const response = with("If-None-Match", etag);

    REQUIRE(response.status == status::not_modified);
    REQUIRE(response.body.empty());
    REQUIRE(response.fields.find("ETag") == etag);
    REQUIRE(!response.fields.contains("Content-Length"));
  }

  SECTION("by a tag among others, weak, or any") {
    REQUIRE(with("If-None-Match", "\"other\", W/" + etag).status == status::not_modified);
    REQUIRE(with("If-None-Match", "*").status == status::not_modified);
  }

  SECTION("another tag sends it") {
    auto const response = with("If-None-Match", "\"other\"");

    REQUIRE(response.status == status::ok);
    REQUIRE(response.body == "hello");
  }

  SECTION("by its date") { REQUIRE(with("If-Modified-Since", date).status == status::not_modified); }

  SECTION("an older date sends it") {
    REQUIRE(with("If-Modified-Since", "Thu, 01 Jan 1970 00:00:00 GMT").status == status::ok);
  }

  SECTION("a date that cannot be read is ignored") {
    REQUIRE(with("If-Modified-Since", "yesterday").status == status::ok);
  }

  SECTION("a tag takes over a date") {
    auto request = make_request("/assets/hello.txt");

    request.fields.add("If-None-Match", "\"other\"");
    request.fields.add("If-Modified-Since", date);
    REQUIRE(ask(served.router, request).status == status::ok);
  }
}

TEST_CASE("static_files: a part of a file", "[server][static]") {
  site const served;

  auto const range = [&](std::string value) {
    auto request = make_request("/assets/hello.txt");

    request.fields.add("Range", std::move(value));
    return ask(served.router, std::move(request));
  };

  auto const part = [&](std::string value, std::string_view body, std::string_view content_range) {
    auto const response = range(std::move(value));

    REQUIRE(response.status == status::partial_content);
    REQUIRE(response.body == body);
    REQUIRE(response.fields.find("Content-Range") == content_range);
    REQUIRE(response.fields.find("Content-Length") == std::to_string(body.size()));
  };

  SECTION("from and to") { part("bytes=1-3", "ell", "bytes 1-3/5"); }

  SECTION("from") { part("bytes=2-", "llo", "bytes 2-4/5"); }

  SECTION("the last bytes") { part("bytes=-2", "lo", "bytes 3-4/5"); }

  SECTION("past the end, up to it") {
    part("bytes=3-100", "lo", "bytes 3-4/5");
    part("bytes=-100", "hello", "bytes 0-4/5");
  }

  SECTION("out of the file") {
    auto const response = range("bytes=5-");

    REQUIRE(response.status == status::range_not_satisfiable);
    REQUIRE(response.fields.find("Content-Range") == "bytes */5");
    REQUIRE(response.body.empty());
  }

  SECTION("what is not one interval sends it whole") {
    auto const value = GENERATE(as<std::string>{}, "bytes=0-1,3-4", "pages=1-2", "bytes=x-y", "bytes=3-1", "bytes");

    CAPTURE(value);

    auto const response = range(value);

    REQUIRE(response.status == status::ok);
    REQUIRE(response.body == "hello");
  }

  SECTION("If-Range: the file it was for") {
    auto const first = ask(served.router, "/assets/hello.txt");
    auto request = make_request("/assets/hello.txt");

    request.fields.add("Range", "bytes=1-3");
    request.fields.add("If-Range", first.fields.find("ETag").value());

    auto const same = ask(served.router, request);

    REQUIRE(same.status == status::partial_content);

    auto other = make_request("/assets/hello.txt");

    other.fields.add("Range", "bytes=1-3");
    other.fields.add("If-Range", "\"another\"");

    auto const changed = ask(served.router, std::move(other));

    REQUIRE(changed.status == status::ok);
    REQUIRE(changed.body == "hello");
  }
}

TEST_CASE("static_files: a file larger than a piece, whole and in part", "[server][static]") {
  temporary_directory directory;
  std::string content;

  for (std::size_t index = 0; content.size() < std::size_t{200} * 1024; ++index) {
    content += std::to_string(index) + ",";
  }

  directory.write("large.bin", content);

  http::router router;

  router.use(http::static_files("/", directory.path()));
  REQUIRE(ask(router, "/large.bin").body == content);

  auto request = make_request("/large.bin");

  request.fields.add("Range", "bytes=70000-140000");
  REQUIRE(ask(router, request).body == content.substr(70000, 70001));
}

TEST_CASE("static_files: the middlewares after it decide first, and what they add stays", "[server][static]") {
  site served;
  auto reached = 0;

  served.router.use([&](http::request request, http::next const &next) -> task<http::response> {
    ++reached;

    if (request.fields.contains("X-Refused")) {
      http::response refused;

      refused.status = status::unauthorized;
      co_return refused;
    }

    auto response = co_await next(std::move(request));

    response.fields.set("X-Added", "yes");
    co_return response;
  });

  SECTION("what it refuses has no file") {
    auto request = make_request("/assets/hello.txt");

    request.fields.add("X-Refused", "yes");

    auto const response = ask(served.router, std::move(request));

    REQUIRE(response.status == status::unauthorized);
    REQUIRE(response.body.empty());
    REQUIRE(reached == 1);
  }

  SECTION("what it adds stays on the file") {
    auto const response = ask(served.router, "/assets/hello.txt");

    REQUIRE(response.body == "hello");
    REQUIRE(response.fields.find("X-Added") == "yes");
    REQUIRE(reached == 1);
  }
}

TEST_CASE("static_files: of several, the last added looks first, and the one before takes what it does not have",
          "[server][static]") {
  temporary_directory first;
  temporary_directory second;

  first.write("both.txt", "first");
  first.write("only-first.txt", "only first");
  second.write("both.txt", "second");

  http::router router;

  router.use(http::static_files("/", first.path()));
  router.use(http::static_files("/", second.path()));

  REQUIRE(ask(router, "/both.txt").body == "second");
  REQUIRE(ask(router, "/only-first.txt").body == "only first");
  REQUIRE(ask(router, "/neither.txt").status == status::not_found);
}

TEST_CASE("static_files: what cannot serve is refused at once", "[server][static]") {
  temporary_directory directory;

  REQUIRE_THROWS_AS(http::static_files("assets", directory.path()), std::invalid_argument);
  REQUIRE_THROWS_AS(http::static_files("/assets", directory.path() / "missing"), fs::filesystem_error);
}

TEST_CASE("static_files: on a connection, a HEAD and a part as they go out", "[server][static]") {
  site const served;
  quoll::async::run_loop loop;
  auto [client, connection] = io::make_memory_stream_pair();

  auto const peer = [](io::memory_stream stream) -> task<std::string> {
    std::string_view const raw =
        "HEAD /assets/hello.txt HTTP/1.1\r\nHost: h\r\n\r\n"
        "GET /assets/hello.txt HTTP/1.1\r\nHost: h\r\nRange: bytes=1-3\r\nConnection: close\r\n\r\n";
    std::string received;
    std::array<char, 4096> chunk{};

    (void)co_await io::write(stream, std::as_bytes(std::span{raw}));

    for (;;) {
      auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

      if (read.error) {
        co_return received;
      }

      received.append(chunk.data(), read.transferred);
    }
  };

  auto outcome =
      quoll::test::run(loop, quoll::async::when_all(http::server{served.router}.serve_connection(std::move(connection)),
                                                    peer(std::move(client))));
  auto const &received = std::get<1>(outcome.value());
  auto const second = received.find("HTTP/1.1 206");

  REQUIRE(received.starts_with("HTTP/1.1 200"));
  REQUIRE(second != std::string::npos);
  // The HEAD sent its length, and none of its body: what follows its head is the next response.
  REQUIRE(received.find("\r\n\r\n") + 4 == second);
  REQUIRE(received.ends_with("\r\n\r\nell"));
  REQUIRE(received.substr(second).find("Content-Range: bytes 1-3/5") != std::string::npos);
}

TEST_CASE("static_files: a later middleware's own 404 has answered", "[server][static]") {
  site served;

  served.router.use([](http::request request, http::next const &next) -> task<http::response> {
    if (next.path() == "/assets/hello.txt") {
      http::response hidden;

      hidden.status = status::not_found;
      co_return hidden;
    }

    co_return co_await next(std::move(request));
  });

  REQUIRE(ask(served.router, "/assets/hello.txt").status == status::not_found);
  REQUIRE(ask(served.router, "/assets/page.html").status == status::ok);
}

TEST_CASE("static_files: a handler a later middleware gave the 404 goes with it", "[server][static]") {
  site served;
  auto ran = false;

  served.router.use([&ran](http::request request, http::next const &next) -> task<http::response> {
    auto response = co_await next(std::move(request));

    if (response.status == status::not_found) {
      response.late_head_writer = [&ran](http::body_stream out) -> task<> {
        ran = true;
        co_await out.write("a page of its own");
      };
    }

    co_return response;
  });

  // A redirect has no writer of its own to replace it.
  auto const redirected = ask(served.router, "/assets/docs");

  REQUIRE(redirected.status == status::moved_permanently);
  REQUIRE_FALSE(redirected.body.contains("a page"));
  REQUIRE_FALSE(ran);
}

TEST_CASE("static_files: what a later middleware made of the 404 goes with it", "[server][static]") {
  site served;

  served.router.use([](http::request request, http::next const &next) -> task<http::response> {
    auto response = co_await next(std::move(request));

    if (response.status == status::not_found) {
      response.fields.set("Transfer-Encoding", "chunked");
      response.fields.set("Content-Encoding", "gzip");
      response.fields.set("X-Added", "yes");
      response.trailers.add("X-Trailer", "yes");
      response.body_writer = [](http::body_stream out) -> task<> { co_await out.write("a page of its own"); };
    }

    co_return response;
  });

  auto const first = ask(served.router, "/assets/hello.txt");

  REQUIRE(first.status == status::ok);
  REQUIRE(first.body == "hello");
  REQUIRE(first.fields.find("X-Added") == "yes");
  REQUIRE(!first.fields.contains("Transfer-Encoding"));
  REQUIRE(!first.fields.contains("Content-Encoding"));
  REQUIRE(first.trailers.empty());

  auto unchanged = make_request("/assets/hello.txt");

  unchanged.fields.add("If-None-Match", first.fields.find("ETag").value());

  auto const not_modified = ask(served.router, std::move(unchanged));

  REQUIRE(not_modified.status == status::not_modified);
  REQUIRE(not_modified.body.empty());

  auto out_of_it = make_request("/assets/hello.txt");

  out_of_it.fields.add("Range", "bytes=100-");

  auto const refused = ask(served.router, std::move(out_of_it));

  REQUIRE(refused.status == status::range_not_satisfiable);
  REQUIRE(refused.body.empty());

  auto const moved = ask(served.router, "/assets/docs");

  REQUIRE(moved.status == status::moved_permanently);
  REQUIRE(moved.body.empty());
  REQUIRE(moved.trailers.empty());
}

TEST_CASE("static_files: a date is one or it is ignored", "[server][static]") {
  site const served;

  auto const value = GENERATE(as<std::string>{}, "Thu, 01 Jan -2147483648 00:00:00 GMT",
                              "Thu, 01 Jan 99999999999999999999 00:00:00 GMT", "Mon, 99 Jan 2099 99:99:99 GMT",
                              "Mon, 01 Jan 2099 00:00:00 PST", "Mon, 01 Jq 2099 00:00:00 GMT",
                              "Mon, 01 Jan 2099 00:00:00 GMT", "Sunday, 06-Nov-94 08:49:37 GMT");

  CAPTURE(value);

  auto request = make_request("/assets/hello.txt");

  // 1 January 2099 is a Thursday: none of these is a date, the last but one by its weekday.
  request.fields.add("If-Modified-Since", value);
  REQUIRE(ask(served.router, request).status == status::ok);
}

TEST_CASE("static_files: a date far ahead, written as it should be", "[server][static]") {
  site const served;
  auto request = make_request("/assets/hello.txt");

  request.fields.add("If-Modified-Since", "Thu, 01 Jan 2099 00:00:00 GMT");
  REQUIRE(ask(served.router, request).status == status::not_modified);
}

TEST_CASE("static_files: a Range is only for a GET", "[server][static]") {
  site const served;
  auto part = make_request("/assets/hello.txt", verb::head);

  part.fields.add("Range", "bytes=1-2");

  auto const response = ask(served.router, std::move(part));

  REQUIRE(response.status == status::ok);
  REQUIRE(response.fields.find("Content-Length") == "5");

  auto out_of_it = make_request("/assets/hello.txt", verb::head);

  out_of_it.fields.add("Range", "bytes=100-");
  REQUIRE(ask(served.router, out_of_it).status == status::ok);
}

TEST_CASE("static_files: the prefix itself is a directory like the others", "[server][static]") {
  temporary_directory directory;

  directory.write("index.html", "<p>home</p>");

  http::router router;

  router.use(http::static_files("/assets", directory.path()));

  auto const without = ask(router, "/assets");

  REQUIRE(without.status == status::moved_permanently);
  REQUIRE(without.fields.find("Location") == "./assets/");
  REQUIRE(ask(router, "/assets/").body == "<p>home</p>");
}

TEST_CASE("static_files: a redirect keeps its query", "[server][static]") {
  site const served;

  REQUIRE(ask(served.router, "/assets/docs?lang=fr").fields.find("Location") == "./docs/?lang=fr");
}

TEST_CASE("static_files: a double slash at the end is no directory", "[server][static]") {
  site const served;

  REQUIRE(ask(served.router, "/assets/docs//").status == status::not_found);
  REQUIRE(ask(served.router, "/assets//").status == status::not_found);
}

#if !defined(__EMSCRIPTEN__)

TEST_CASE("static_files: out of descriptors is no missing file", "[server][static]") {
  site const served;

  // Undone however this ends: every later test of the program shares what it takes.
  struct out_of_descriptors {
    rlimit before{};
    std::vector<int> taken;

    out_of_descriptors() {
      ::getrlimit(RLIMIT_NOFILE, &before);

      auto lower = before;

      lower.rlim_cur = 64;
      ::setrlimit(RLIMIT_NOFILE, &lower);

      for (auto descriptor = ::open("/dev/null", O_RDONLY); descriptor >= 0;
           descriptor = ::open("/dev/null", O_RDONLY)) {
        taken.push_back(descriptor);
      }
    }

    out_of_descriptors(out_of_descriptors const &) = delete;
    out_of_descriptors &operator=(out_of_descriptors const &) = delete;
    out_of_descriptors(out_of_descriptors &&) = delete;
    out_of_descriptors &operator=(out_of_descriptors &&) = delete;

    ~out_of_descriptors() {
      for (auto const descriptor : taken) {
        ::close(descriptor);
      }

      ::setrlimit(RLIMIT_NOFILE, &before);
    }
  };

  auto outcome = std::string{"no failure"};

  {
    out_of_descriptors const none_left;

    try {
      outcome = std::to_string(static_cast<int>(ask(served.router, "/assets/hello.txt").status));
    } catch (std::system_error const &thrown) {
      outcome = thrown.code() == std::errc::too_many_files_open ? "out of descriptors" : "another failure";
    }
  }

  REQUIRE(outcome == "out of descriptors");
}

#endif

TEST_CASE("static_files: a prefix with a percent sign redirects to itself", "[server][static]") {
  temporary_directory directory;

  directory.write("index.html", "<p>home</p>");

  http::router router;

  router.use(http::static_files("/100%25", directory.path()));

  auto const without = ask(router, "/100%25");

  REQUIRE(without.status == status::moved_permanently);
  REQUIRE(without.fields.find("Location") == "./100%25/");
  REQUIRE(ask(router, "/100%25/").body == "<p>home</p>");
}

TEST_CASE("static_files: opening beneath the root follows no link, wherever it is on the path", "[server][static]") {
  temporary_directory root;
  temporary_directory outside;

  root.write("nested/file.txt", "inside");
  outside.write("secret.txt", "secret");
  fs::create_directory_symlink(outside.path(), root.path() / "gone");
  fs::create_symlink(outside.path() / "secret.txt", root.path() / "link.txt");

  auto const opens = [&](fs::path const &relative) {
    auto const descriptor = http::detail::open_beneath(root.path(), relative);

    if (descriptor >= 0) {
      ::close(descriptor);
    }

    return descriptor >= 0;
  };

  REQUIRE(opens("nested/file.txt"));
  REQUIRE(opens("nested"));
  REQUIRE(opens(""));
  // What a directory replaced since the path was checked looks like.
  REQUIRE(!opens("gone/secret.txt"));
  REQUIRE(!opens("link.txt"));
  REQUIRE(!opens("nested/missing.txt"));
  // What failed, from the component it failed on.
  REQUIRE(http::detail::open_beneath(root.path(), "nested/missing.txt") == -ENOENT);
  REQUIRE(http::detail::open_beneath(root.path(), "gone/secret.txt") < 0);
}

TEST_CASE("static_files: a file that is no longer the one asked for", "[server][static]") {
  site const served;
  auto const first = ask(served.router, "/assets/hello.txt");
  auto const etag = std::string{first.fields.find("ETag").value()};
  auto const date = std::string{first.fields.find("Last-Modified").value()};

  auto const with = [&](std::string_view name, std::string value) {
    auto request = make_request("/assets/hello.txt");

    request.fields.add(name, std::move(value));
    request.fields.add("Range", "bytes=1-3");
    return ask(served.router, std::move(request));
  };

  SECTION("If-Match") {
    auto const changed = with("If-Match", "\"old\"");

    REQUIRE(changed.status == status::precondition_failed);
    REQUIRE(changed.body.empty());
    REQUIRE(with("If-Match", etag).status == status::partial_content);
    REQUIRE(with("If-Match", "\"old\", " + etag).status == status::partial_content);
    REQUIRE(with("If-Match", "*").status == status::partial_content);
    // The strong comparison.
    REQUIRE(with("If-Match", "W/" + etag).status == status::precondition_failed);
  }

  SECTION("If-Unmodified-Since") {
    REQUIRE(with("If-Unmodified-Since", "Thu, 01 Jan 1970 00:00:00 GMT").status == status::precondition_failed);
    REQUIRE(with("If-Unmodified-Since", date).status == status::partial_content);
    REQUIRE(with("If-Unmodified-Since", "not a date").status == status::partial_content);
  }

  SECTION("the first takes over the second") {
    auto request = make_request("/assets/hello.txt");

    request.fields.add("If-Match", etag);
    request.fields.add("If-Unmodified-Since", "Thu, 01 Jan 1970 00:00:00 GMT");
    REQUIRE(ask(served.router, request).status == status::ok);
  }
}

TEST_CASE("static_files: a link does not lead to what its name would not", "[server][static]") {
  site const served;

  fs::create_symlink(served.directory.path() / ".env", served.directory.path() / "config.txt");
  fs::create_directory_symlink(served.directory.path() / ".git", served.directory.path() / "repo");

  REQUIRE(ask(served.router, "/assets/config.txt").status == status::not_found);
  REQUIRE(ask(served.router, "/assets/repo/config").status == status::not_found);
}

TEST_CASE("static_files: an index that is a dotfile is one", "[server][static]") {
  site const served{"/assets", {.index = ".env"}};

  served.directory.write("docs/.env", "SECRET");
  REQUIRE(ask(served.router, "/assets/docs/").status == status::not_found);
}

TEST_CASE("static_files: a root that is no directory is refused at once", "[server][static]") {
  temporary_directory directory;

  directory.write("file.txt", "a file");
  REQUIRE_THROWS_AS(http::static_files("/", directory.path() / "file.txt"), std::invalid_argument);
}

#if !defined(__EMSCRIPTEN__)

TEST_CASE("static_files: a directory that only lets through, its listing hidden", "[server][static]") {
  site const served;
  auto const hidden = served.directory.path() / "hidden";

  served.directory.write("hidden/file.txt", "through");
  served.directory.write("hidden/index.html", "<p>through</p>");
  fs::permissions(hidden, fs::perms::owner_exec | fs::perms::owner_write);

  auto const file = ask(served.router, "/assets/hidden/file.txt");
  auto const index = ask(served.router, "/assets/hidden/");

  fs::permissions(hidden, fs::perms::owner_all);

  REQUIRE(file.body == "through");
  REQUIRE(index.body == "<p>through</p>");
}

TEST_CASE("static_files: a socket is no file", "[server][static]") {
  site const served;
  auto const path = served.directory.path() / "app.sock";
  auto const descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);

  REQUIRE(descriptor >= 0);

  sockaddr_un address{};

  address.sun_family = AF_UNIX;
  REQUIRE(path.native().size() < sizeof(address.sun_path));
  std::ranges::copy(path.native(), std::begin(address.sun_path));
  REQUIRE(::bind(descriptor, reinterpret_cast<sockaddr const *>(&address), sizeof(address)) == 0);

  auto const answered = ask(served.router, "/assets/app.sock").status;

  ::close(descriptor);
  REQUIRE(answered == status::not_found);
}

#endif

TEST_CASE("static_files: a file that shrinks under its response cuts it, and says so", "[server][static]") {
  temporary_directory directory;
  std::string const content(std::size_t{100} * 1024, 'x');

  directory.write("large.bin", content);

  http::router router;
  auto const file = directory.path() / "large.bin";

  // Outside of it: by the time the response comes back here, it has what it announced, and nothing has left.
  router.use([&](http::request request, http::next const &next) -> task<http::response> {
    auto response = co_await next(std::move(request));

    fs::resize_file(file, 10);
    co_return response;
  });
  router.use(http::static_files("/", directory.path()));

  quoll::async::run_loop loop;
  auto [client, connection] = io::make_memory_stream_pair();
  auto reported = 0;
  http::connection_options options;

  options.on_handler_error = [&](std::exception_ptr const &) { ++reported; };

  auto const peer = [](io::memory_stream stream) -> task<std::string> {
    std::string_view const raw = "GET /large.bin HTTP/1.1\r\nHost: h\r\n\r\n";
    std::string received;
    std::array<char, 4096> chunk{};

    (void)co_await io::write(stream, std::as_bytes(std::span{raw}));

    for (;;) {
      auto const read = co_await stream.read_some(std::as_writable_bytes(std::span{chunk}));

      if (read.error) {
        co_return received;
      }

      received.append(chunk.data(), read.transferred);
    }
  };

  auto outcome = quoll::test::run(
      loop,
      quoll::async::when_all(http::server{router, {.connection = options}}.serve_connection(std::move(connection)),
                             peer(std::move(client))));
  auto const &received = std::get<1>(outcome.value());

  REQUIRE(received.starts_with("HTTP/1.1 200"));
  REQUIRE(received.find("Content-Length: 102400") != std::string::npos);
  REQUIRE(received.ends_with("\r\n\r\nxxxxxxxxxx"));
  REQUIRE(reported == 1);
}

TEST_CASE("static_files: a date in any of the forms of the RFC", "[server][static]") {
  site const served;

  auto const with = [&](std::string_view name, std::string value) {
    auto request = make_request("/assets/hello.txt");

    request.fields.add(name, std::move(value));
    return ask(served.router, std::move(request)).status;
  };

  // Far ahead, and written in the forms that can be.
  REQUIRE(with("If-Modified-Since", "Thu, 01 Jan 2099 00:00:00 GMT") == status::not_modified);
  REQUIRE(with("If-Modified-Since", "Thu Jan  1 00:00:00 2099") == status::not_modified);
  // Long ago: in a form that can only say so.
  REQUIRE(with("If-Unmodified-Since", "Sunday, 06-Nov-94 08:49:37 GMT") == status::precondition_failed);
  REQUIRE(with("If-Unmodified-Since", "Sun Nov  6 08:49:37 1994") == status::precondition_failed);
  // None of the forms, or not quite.
  REQUIRE(with("If-Unmodified-Since", "Sun, 06 Nov 94 08:49:37 GMT") == status::ok);
  REQUIRE(with("If-Unmodified-Since", "Sunday, 06-Nov-94 08:49:37 PST") == status::ok);
  REQUIRE(with("If-Unmodified-Since", "Sun Nov  6 08:49:37 94") == status::ok);
  REQUIRE(with("If-Unmodified-Since", "Sun, 06 Nov 1994 8:49:37 GMT") == status::ok);
  // On no such day, or not on that weekday.
  REQUIRE(with("If-Unmodified-Since", "Tue, 31 Feb 1995 08:49:37 GMT") == status::ok);
  REQUIRE(with("If-Unmodified-Since", "Mon, 06 Nov 1994 08:49:37 GMT") == status::ok);
}

TEST_CASE("static_files: a list on several lines is one list", "[server][static]") {
  site const served;
  auto const etag = std::string{ask(served.router, "/assets/hello.txt").fields.find("ETag").value()};

  auto request = make_request("/assets/hello.txt");

  request.fields.add("If-Match", "\"old\"");
  request.fields.add("If-Match", etag);
  REQUIRE(ask(served.router, request).status == status::ok);

  auto none = make_request("/assets/hello.txt");

  none.fields.add("If-None-Match", "\"old\"");
  none.fields.add("if-none-match", etag);
  REQUIRE(ask(served.router, none).status == status::not_modified);

  auto ranges = make_request("/assets/hello.txt");

  ranges.fields.add("Range", "bytes=0-1");
  ranges.fields.add("Range", "bytes=2-3");
  REQUIRE(ask(served.router, ranges).status == status::ok);
}

TEST_CASE("static_files: the last bytes of nothing", "[server][static]") {
  site const served;
  auto request = make_request("/assets/empty.txt");

  request.fields.add("Range", "bytes=-500");
  REQUIRE(ask(served.router, request).status == status::ok);

  auto zero = make_request("/assets/hello.txt");

  zero.fields.add("Range", "bytes=-0");
  REQUIRE(ask(served.router, zero).status == status::range_not_satisfiable);
}

TEST_CASE("static_files: a root that is a link is followed as it is now", "[server][static]") {
  temporary_directory holder;
  temporary_directory first;
  temporary_directory second;

  first.write("page.txt", "first release");
  second.write("page.txt", "second release");

  auto const current = holder.path() / "current";

  fs::create_directory_symlink(first.path(), current);

  http::router router;

  router.use(http::static_files("/", current));
  REQUIRE(ask(router, "/page.txt").body == "first release");

  auto const flipping = holder.path() / "flipping";

  fs::create_directory_symlink(second.path(), flipping);
  fs::rename(flipping, current);
  REQUIRE(ask(router, "/page.txt").body == "second release");
}

TEST_CASE("static_files: what describes a 404 page stays off the file", "[server][static]") {
  site served;

  served.router.use([](http::request request, http::next const &next) -> task<http::response> {
    auto response = co_await next(std::move(request));

    response.fields.set("Content-Disposition", "inline; filename=404.html");
    response.fields.set("Content-Language", "fr");
    co_return response;
  });

  auto const response = ask(served.router, "/assets/hello.txt");

  REQUIRE(response.status == status::ok);
  REQUIRE(!response.fields.contains("Content-Disposition"));
  REQUIRE(!response.fields.contains("Content-Language"));
}

#if !defined(__EMSCRIPTEN__)

TEST_CASE("static_files: what is neither a file nor a directory is not even opened", "[server][static]") {
  site const served;
  auto const device = served.directory.path() / "null";

  fs::create_symlink("/dev/null", served.directory.path() / "outside");
  REQUIRE(::mkfifo(device.c_str(), 0600) == 0);
  REQUIRE(ask(served.router, "/assets/null").status == status::not_found);
  REQUIRE(ask(served.router, "/assets/outside").status == status::not_found);
}

#endif

TEST_CASE("static_files: a date in a condition, with what a field may have around it", "[server][static]") {
  site served;

  auto const first = ask(served.router, "/assets/hello.txt");
  auto const date = std::string{first.fields.find("Last-Modified").value()};
  auto with = make_request("/assets/hello.txt");

  with.fields.add("If-Modified-Since", date + " ");

  auto const unchanged = ask(served.router, std::move(with));

  REQUIRE(unchanged.status == status::not_modified);

  auto earlier = make_request("/assets/hello.txt");

  earlier.fields.add("If-Unmodified-Since", "Sun, 06 Nov 1994 08:49:37 GMT ");

  auto const changed = ask(served.router, std::move(earlier));

  REQUIRE(changed.status == status::precondition_failed);
}

TEST_CASE("static_files: a file from the future is not said to be", "[server][static]") {
  site served;

  std::filesystem::last_write_time(served.directory.path() / "hello.txt",
                                   std::filesystem::file_time_type::clock::now() + std::chrono::hours{24 * 365});

  auto const answer = ask(served.router, "/assets/hello.txt");
  auto const date = std::string{answer.fields.find("Last-Modified").value()};
  auto const next_year = [] {
    auto const in_a_year = std::time(nullptr) + std::time_t{366} * 24 * 3600;

    return 1900 + std::gmtime(&in_a_year)->tm_year;
  }();

  REQUIRE(answer.status == status::ok);
  REQUIRE_FALSE(date.contains(std::to_string(next_year)));
}
