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

// A fake GitHub for the test of quoll_github: the same contract, served with what the test expects, and refusing a
// request without the User-Agent GitHub asks for, as GitHub does.
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
#include <string>
#include <utility>
#include <vector>

namespace {

namespace async = quoll::async;
namespace http = quoll::http;
namespace rest = quoll::rest;

struct github_service {
  rest::reply<github::repos::get> operator()(github::repos::get, std::string owner, std::string repo) const {
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
    if (filter.state.value_or("open") != "open") {
      return {};
    }

    return {{.number = 7, .title = "SSE", .state = "open", .user = {.login = "ada"}},
            {.number = 9, .title = "WebSocket", .state = "open", .user = {.login = "grace"}}};
  }

  github::me operator()(github::users::me) const { return {.login = "ada", .name = "Ada Lovelace"}; }
};

async::task<http::response> as_github_wants(http::request &&request, http::next const &next) {
  if (!request.fields.contains("User-Agent")) {
    http::response refused;

    refused.status = http::status::forbidden;
    refused.body = "Request forbidden by administrative rules. Please make sure your request has a User-Agent header";
    co_return refused;
  }

  co_return co_await next(std::move(request));
}

async::task<> serve(std::uint16_t port) {
  http::router router;

  router.use(as_github_wants);
  rest::mount<github::api>(
      router, github_service{},
      rest::validators([](rest::credential<rest::bearer_auth> const &token) { return token.value == "test-token"; }));

  auto listener = co_await quoll::io::listen(port);

  std::cout << "fake GitHub on http://127.0.0.1:" << listener.local_port().value_or(port) << std::endl;
  co_await http::server{std::move(router)}.serve(std::move(listener));
}

} // namespace

int main(int argc, char **argv) {
  auto const port = static_cast<std::uint16_t>(argc > 1 ? std::stoi(argv[1]) : 0);
  quoll::runtime runtime;

  co_spawn(runtime.get_executor(), serve(port), async::detached, runtime.get_stop_token());
  runtime.run();
}
