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

// A command-line program calling GitHub's REST API through its contract (github_api.hpp):
//
//   quoll_github <owner>/<repo>     the repository and its open issues
//   quoll_github me                 the account of the token in GITHUB_TOKEN
//
// It calls https://api.github.com, or the URL in QUOLL_GITHUB_URL (its test's fake GitHub). Natively through a
// connection and TLS; under Emscripten, in Node, through fetch.

#include "github_api.hpp"

#include <quoll/async.hpp>
#include <quoll/http/request.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/status.hpp>
#include <quoll/rest.hpp>
#include <quoll/runtime.hpp>

#include <cstdlib>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace async = quoll::async;
namespace http = quoll::http;
namespace rest = quoll::rest;

// Copied freely: its transport and its interceptors are shared, so that a coroutine takes it by value.
using github_client = rest::client<github::api>;

// GitHub asks every request for a User-Agent, and for the version of its API: an interceptor adds them to each.
async::task<http::response> as_github_asks(http::request &&request, rest::next const &send) {
  request.fields.set("User-Agent", "quoll-github-sample");
  request.fields.set("Accept", "application/vnd.github+json");
  request.fields.set("X-GitHub-Api-Version", "2022-11-28");
  return send(std::move(request));
}

[[nodiscard]] std::optional<std::string> from_environment(char const *name) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, before anything runs.
  if (auto const *value = std::getenv(name); value != nullptr && *value != '\0') {
    return std::string{value};
  }

  return std::nullopt;
}

async::task<int> show_repository(github_client client, std::string owner, std::string name) {
  // The endpoints under /repos/{owner}/{repo}, with both fixed.
  auto const repository = client.scope<github::repos::api>(std::move(owner), std::move(name));
  auto const found = co_await repository.try_call<github::repos::get>();

  if (!found) {
    std::cerr << "unexpected answer, " << static_cast<int>(found.error().response.status) << ": "
              << found.error().reason << '\n';
    co_return 1;
  }

  if (found->holds<http::status::not_found>()) {
    std::cerr << "not found: " << found->get<http::status::not_found>().message << '\n';
    co_return 1;
  }

  auto const &shown = found->get<http::status::ok>();

  std::cout << shown.full_name << ": " << shown.stars << " stars, " << shown.open_issues_count << " open issues\n";
  std::cout << shown.description.value_or("(no description)") << '\n' << shown.html_url << '\n';

  for (auto const &open : co_await repository.call<github::repos::issues>({.state = "open", .per_page = 5})) {
    std::cout << "  #" << open.number << ' ' << open.title << " (" << open.user.login << ")\n";
  }

  co_return 0;
}

async::task<int> show_me(github_client client) {
  auto const answer = co_await client.try_call<github::users::me>();

  if (!answer || !answer->holds<http::status::ok>()) {
    std::cerr << "no account for this token: set GITHUB_TOKEN\n";
    co_return 1;
  }

  auto const &account = answer->get<http::status::ok>();

  std::cout << account.login << (account.name ? " (" + *account.name + ")" : "") << '\n';
  co_return 0;
}

async::task<int> run(std::vector<std::string> arguments) {
  if (arguments.size() != 1) {
    std::cerr << "usage: quoll_github <owner>/<repo> | me\n";
    co_return 2;
  }

  github_client client{from_environment("QUOLL_GITHUB_URL").value_or("https://api.github.com")};

  client.use(as_github_asks);
  client.use(rest::retry{.attempts = 3});

  if (auto token = from_environment("GITHUB_TOKEN")) {
    client.credentials<rest::bearer_auth>(std::move(*token));
  }

  if (arguments[0] == "me") {
    co_return co_await show_me(client);
  }

  auto const slash = arguments[0].find('/');

  if (slash == std::string::npos || slash == 0 || slash + 1 == arguments[0].size()) {
    std::cerr << "expected <owner>/<repo>, got " << arguments[0] << '\n';
    co_return 2;
  }

  co_return co_await show_repository(client, arguments[0].substr(0, slash), arguments[0].substr(slash + 1));
}

// The program's status: what run returns, or 1 with what it threw. Not a local of main, which returns before the task
// ends under Emscripten, where Node's loop runs it.
int exit_status = 1;

// Under Emscripten, Node's exit code: main has returned already, and Node ends once nothing is left to run. Exiting
// from here would unwind through the program's frames.
void ended() {
#if defined(__EMSCRIPTEN__)
  EM_ASM({ process.exitCode = $0; }, exit_status);
#endif
}

struct record_status {
  void operator()(int code) const {
    exit_status = code;
    ended();
  }

  void operator()(std::exception_ptr const &failure) const {
    try {
      std::rethrow_exception(failure);
    } catch (std::exception const &error) {
      std::cerr << "failed: " << error.what() << '\n';
    } catch (...) {
      std::cerr << "failed\n";
    }

    exit_status = 1;
    ended();
  }
};

} // namespace

int main(int argc, char **argv) {
  quoll::runtime runtime;

  co_spawn(runtime.get_executor(), run(std::vector<std::string>(argv + 1, argv + argc)), record_status{});
  runtime.run();
  return exit_status;
}
