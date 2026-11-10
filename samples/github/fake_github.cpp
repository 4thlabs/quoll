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

// A fake GitHub for the test of quoll_github: the same contract, served with what the test expects, answering as GitHub
// does what the program has to cope with: a request without the User-Agent GitHub asks for refused, a token refused
// with GitHub's error body, a private repository, a 503 once, which the program's retry sends again.
//
//   quoll_fake_github [port]     prints its URL once it listens, on port 0 one the system picks

#include "github_api.hpp"

#include <quoll/async.hpp>
#include <quoll/http/next.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/io/listen.hpp>
#include <quoll/rest.hpp>
#include <quoll/runtime.hpp>
#include <quoll/server.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace async = quoll::async;
namespace http = quoll::http;
namespace rest = quoll::rest;

struct github_service {
  // The first request for issues is answered 503: the program's retry has to send it again. The runtime runs one
  // thread.
  std::shared_ptr<int> issues_asked = std::make_shared<int>(0);

  rest::reply<github::repos::get> operator()(github::repos::get, std::string owner, std::string repo) const {
    if (owner == "4thlabs" && repo == "private") {
      return rest::respond<http::status::forbidden>(
          github::error{.message = "Resource not accessible", .documentation_url = {}});
    }

    if (owner != "4thlabs" || repo != "quoll") {
      return rest::respond<http::status::not_found>(github::error{.message = "Not Found", .documentation_url = {}});
    }

    return github::repository{.full_name = "4thlabs/quoll",
                              .description = "A C++23 web framework",
                              .stars = 42,
                              .open_issues_count = 2,
                              .html_url = "https://github.com/4thlabs/quoll"};
  }

  std::vector<github::issue> operator()(github::repos::issues, std::string, std::string,
                                        github::issue_filter filter) const {
    if (++*issues_asked == 1) {
      throw rest::http_error{http::status::service_unavailable, "busy"};
    }

    // What the program asks for, or nothing: its query reaches here.
    if (filter.state != "open" || filter.per_page != 5) {
      return {};
    }

    return {{.number = 7, .title = "SSE", .state = "open", .user = {.login = "ada"}, .pull_request = {}},
            {.number = 9,
             .title = "WebSocket",
             .state = "open",
             .user = {.login = "grace"},
             .pull_request = github::pull_request_link{.url = "https://api.github.com/repos/4thlabs/quoll/pulls/9"}}};
  }

  github::me operator()(github::users::me) const { return {.login = "ada", .name = "Ada Lovelace"}; }
};

// GitHub's checks, before the contract's: a User-Agent on every request, and the token of /user, refused with
// GitHub's own body rather than the problem a validator answers.
async::task<http::response> as_github_wants(http::request &&request, http::next const &next) {
  http::response refused;

  refused.fields.add("Content-Type", "application/json; charset=utf-8");

  if (!request.fields.contains("User-Agent")) {
    refused.status = http::status::forbidden;
    refused.body = R"({"message":"Request forbidden by administrative rules: a User-Agent header is required"})";
    co_return refused;
  }

  auto const authorized = request.fields.find("Authorization") == "Bearer test-token";

  if (next.path() == "/user" && !authorized) {
    refused.status = http::status::unauthorized;
    refused.body = R"({"message":"Bad credentials","documentation_url":"https://docs.github.com/rest"})";
    co_return refused;
  }

  // A private repository, for a token with access: no endpoint of the contract sees who calls.
  if (next.path() == "/repos/4thlabs/private" && authorized) {
    http::response granted;

    granted.fields.add("Content-Type", "application/json; charset=utf-8");
    granted.body = R"({"full_name":"4thlabs/private","stargazers_count":1,"open_issues_count":0,)"
                   R"("html_url":"https://github.com/4thlabs/private"})";
    co_return granted;
  }

  co_return co_await next(std::move(request));
}

async::task<> serve(std::uint16_t port) {
  http::router router;

  router.use(as_github_wants);
  rest::mount<github::api>(router, github_service{},
                           // The middleware above has checked it already, answering as GitHub does.
                           rest::validators([](rest::credential<rest::bearer_auth> const &) { return true; }));

  auto listener = co_await quoll::io::listen(port);

  std::cout << "fake GitHub on http://127.0.0.1:" << listener.local_port().value_or(port) << std::endl;
  co_await http::server{std::move(router)}.serve(std::move(listener));
}

} // namespace

int main(int argc, char **argv) {
  auto const port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 0);
  quoll::runtime runtime;

  return runtime.run(serve(port));
}
