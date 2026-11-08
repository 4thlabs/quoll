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

// The contract of a few endpoints of GitHub's REST API (https://docs.github.com/rest), as GitHub has them: its names,
// its errors, its fields as they come, more than the contract reads. Shared by the program and the fake GitHub its test
// runs against. No I/O.

#ifndef QUOLL_SAMPLES_GITHUB_API_HPP
#define QUOLL_SAMPLES_GITHUB_API_HPP

#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/rest.hpp>

#include <glaze/core/common.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace github {

struct repository {
  std::string full_name;
  std::optional<std::string> description;
  // Named as the program reads it: GitHub's name is stargazers_count (see the glz::meta below).
  std::int64_t stars = 0;
  std::int64_t open_issues_count = 0;
  std::string html_url;
};

struct account {
  std::string login;
};

struct issue {
  std::int64_t number = 0;
  std::string title;
  std::string state;
  account user;
};

// What GitHub answers on an error, whatever its status.
struct error {
  std::string message;
  std::optional<std::string> documentation_url;
};

struct issue_filter {
  std::optional<std::string> state;
  std::optional<int> per_page;
};

struct me {
  std::string login;
  std::optional<std::string> name;
};

} // namespace github

template <>
struct glz::meta<github::repository> {
  using T = github::repository;
  static constexpr auto value =
      glz::object("full_name", &T::full_name, "description", &T::description, "stargazers_count", &T::stars,
                  "open_issues_count", &T::open_issues_count, "html_url", &T::html_url);
};

namespace github {

namespace http = quoll::http;
namespace rest = quoll::rest;

// GitHub adds fields over time: every response is read with the lenient codec, which skips those it does not know.
template <class T, http::status Status = http::status::ok>
using answers = rest::returns<T, Status, rest::lenient_json_codec>;

namespace repos {

using get =
    rest::endpoint<"repos.get", http::verb::get, "/", answers<repository>, answers<error, http::status::not_found>>;
using issues = rest::endpoint<"repos.issues", http::verb::get, "/issues", rest::query_params<issue_filter>,
                              answers<std::vector<issue>>, answers<error, http::status::not_found>>;

using api = rest::api<"/repos/{owner}/{repo}", get, issues>;

} // namespace repos

namespace users {

// The account the token belongs to.
using me = rest::endpoint<"users.me", http::verb::get, "/user", rest::security<rest::bearer_auth>, answers<github::me>,
                          answers<error, http::status::unauthorized>>;

} // namespace users

using api = rest::api<"", repos::api, users::me>;

} // namespace github

#endif // QUOLL_SAMPLES_GITHUB_API_HPP
