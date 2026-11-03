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

#include <quoll/http/verb.hpp>
#include <quoll/rest/detail/path_template.hpp>
#include <quoll/rest/detail/route.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace {

using quoll::http::verb;
using quoll::rest::detail::join_paths;
using quoll::rest::detail::param_kind;
using quoll::rest::detail::parse_path;
using quoll::rest::detail::path_error;
using quoll::rest::detail::same_operation;

constexpr auto cards = parse_path("/users/{user_id:int64}/cards/{card}");

static_assert(cards.error == path_error::none);
static_assert(cards.size == 2);
static_assert(cards.params[0].name == "user_id" && cards.params[0].kind == param_kind::int64);
static_assert(cards.params[1].name == "card" && cards.params[1].kind == param_kind::string);

static_assert(parse_path("").error == path_error::none && parse_path("").size == 0);
static_assert(parse_path("/").error == path_error::none);
static_assert(parse_path("/files/{rest:path}").params[0].kind == param_kind::rest_of_path);
static_assert(parse_path("/{a:int}/{b:uint}/{c:uint64}/{d:double}/{e:bool}").size == 5);

static_assert(parse_path("users").error == path_error::no_leading_slash);
static_assert(parse_path("/users/{id").error == path_error::unbalanced_brace);
static_assert(parse_path("/users/id}").error == path_error::unbalanced_brace);
static_assert(parse_path("/users/{{id}}").error == path_error::unbalanced_brace);
static_assert(parse_path("/users/{}").error == path_error::invalid_name);
static_assert(parse_path("/users/{a-b}").error == path_error::invalid_name);
static_assert(parse_path("/users/{id:float}").error == path_error::unknown_type);
static_assert(parse_path("/users/x{id}").error == path_error::not_whole_segment);
static_assert(parse_path("/users/{id}.json").error == path_error::not_whole_segment);
static_assert(parse_path("/{rest:path}/{id}").error == path_error::rest_not_last);
static_assert(parse_path("/files/{rest:path}/meta").error == path_error::rest_not_last);
static_assert(parse_path("/files/{rest:path}/").error == path_error::rest_not_last);
static_assert(parse_path("/{id}/{id:int}").error == path_error::duplicate_name);
static_assert(parse_path("/caf%zz").error == path_error::invalid_escape);
static_assert(parse_path("/caf%C3%A9").error == path_error::none);
static_assert(parse_path("/{name:string}").params[0].kind == param_kind::string);

static_assert(join_paths<"/api", "/users">().view() == "/api/users");
static_assert(join_paths<"/api", "/">().view() == "/api");
static_assert(join_paths<"", "/">().view() == "/");
static_assert(join_paths<"", "/users">().view() == "/users");

static_assert(same_operation(verb::get, "/users/{id}", verb::get, "/users/{uid:int}"));
static_assert(!same_operation(verb::get, "/users/{id}", verb::delete_, "/users/{id}"));
static_assert(!same_operation(verb::get, "/users/{id}", verb::get, "/users/{id}/cards"));
static_assert(!same_operation(verb::get, "/users", verb::get, "/user"));
static_assert(!same_operation(verb::get, "/users/{id", verb::get, "/users/{id"));

} // namespace

TEST_CASE("rest path templates: parameters and their types, at compile time", "[rest][path]") {
  constexpr auto parsed = parse_path("/users/{user_id:int64}/cards/{card}");

  REQUIRE(parsed.error == path_error::none);
  REQUIRE(parsed.size == 2);
  REQUIRE(parsed.params[0].name == "user_id");
  REQUIRE(parsed.params[1].kind == param_kind::string);
  REQUIRE(parse_path("/users/{id:float}").error == path_error::unknown_type);
}
