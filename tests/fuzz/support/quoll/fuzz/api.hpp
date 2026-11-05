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

#ifndef QUOLL_FUZZ_API_HPP
#define QUOLL_FUZZ_API_HPP

#include <quoll/async/task.hpp>
#include <quoll/http/body_reader.hpp>
#include <quoll/http/response.hpp>
#include <quoll/http/router.hpp>
#include <quoll/http/status.hpp>
#include <quoll/http/verb.hpp>
#include <quoll/rest.hpp>

#include <glaze/glaze.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace quoll::fuzz {

enum class color { red, green };

} // namespace quoll::fuzz

template <>
struct glz::meta<quoll::fuzz::color> {
  using enum quoll::fuzz::color;
  static constexpr auto value = glz::enumerate(red, green);
};

// A contract using what the DSL decodes: every kind of path parameter, query and header parameters required, optional
// and repeated, enums, credentials, a JSON body, and a second declared response.
namespace quoll::fuzz {

namespace rest = quoll::rest;

using http::verb;

struct note {
  std::int64_t id = 0;
  std::string text;
  std::optional<double> weight;
  std::vector<std::string> tags;
};

struct new_note {
  std::string text;
  std::optional<note> parent;
};

struct missing {
  std::string detail;
};

struct filter {
  std::optional<int> limit;
  std::optional<std::string> contains;
  std::vector<std::string> tag;
  std::optional<bool> done;
};

using list = rest::endpoint<"list", verb::get, "/", rest::query_params<filter>, rest::returns<std::vector<note>>>;
using get = rest::endpoint<"get", verb::get, "/{id:int64}", rest::returns<note>,
                           rest::returns<missing, http::status::not_found>>;
using create =
    rest::endpoint<"create", verb::post, "/", rest::body<new_note>, rest::returns<note, http::status::created>>;
using scale = rest::endpoint<"scale", verb::get, "/scale/{by:double}", rest::query<"flag", bool>,
                             rest::header<"X-Tags", std::vector<std::string>>, rest::returns<std::string>>;
using file = rest::endpoint<"file", verb::get, "/file/{name:path}", rest::returns<std::string>>;

using kinds = rest::endpoint<"kinds", verb::get, "/kinds/{a:int}/{b:uint}/{c:uint64}/{d:bool}/{e:string}",
                             rest::returns<std::string>>;
using tinted = rest::endpoint<"tinted", verb::get, "/tinted", rest::query<"color", color>,
                              rest::query<"other", std::optional<color>>,
                              rest::header<"X-Count", std::optional<std::int64_t>>, rest::returns<std::string>>;
using bearer =
    rest::endpoint<"bearer", verb::get, "/bearer", rest::security<rest::bearer_auth>, rest::returns<std::string>>;
using keyed = rest::endpoint<"keyed", verb::get, "/keyed", rest::security<rest::api_key<"X-Api-Key">>,
                             rest::returns<std::string>>;
using queried = rest::endpoint<"queried", verb::get, "/queried", rest::security<rest::api_key_query<"key">>,
                               rest::returns<std::string>>;

using api = rest::api<"/notes", list, get, create, scale, file, kinds, tinted, bearer, keyed, queried>;

struct service {
  std::vector<note> operator()(list, filter const &) const { return {note{.id = 1, .text = "one"}}; }

  rest::reply<get> operator()(get, std::int64_t id) const {
    if (id < 0) {
      return missing{"no note"};
    }

    return note{.id = id};
  }

  note operator()(create, new_note created) const { return {.text = std::move(created.text)}; }

  std::string operator()(scale, double, bool, std::vector<std::string> const &) const { return "scaled"; }

  std::string operator()(file, std::string name) const { return name; }

  std::string operator()(kinds, int, unsigned, std::uint64_t, bool, std::string name) const { return name; }

  std::string operator()(tinted, color, std::optional<color>, std::optional<std::int64_t>) const { return "tinted"; }

  std::string operator()(bearer) const { return "bearer"; }

  std::string operator()(keyed) const { return "keyed"; }

  std::string operator()(queried) const { return "queried"; }
};

// The contract on a router, every credential accepted: past them, there is more to decode. Shared between inputs, as a
// server shares its router between connections.
[[nodiscard]] inline http::router const &routes() {
  static http::router const shared = [] {
    http::router router;

    rest::mount<api>(router, service{},
                     rest::validators([](rest::credential<rest::bearer_auth> const &) { return true; },
                                      [](rest::credential<rest::api_key<"X-Api-Key">> const &) { return true; },
                                      [](rest::credential<rest::api_key_query<"key">> const &) { return true; }));
    // A body in pieces, read to its end, or left halfway when its first piece asks for it.
    router.post("/in-pieces/{name}", [](std::string name, http::body_reader body) -> async::task<http::response> {
      http::response response;

      while (auto const piece = co_await body.read()) {
        response.body += *piece;

        if (piece->starts_with("stop")) {
          break;
        }
      }

      response.body += name;
      co_return response;
    });
    return router;
  }();

  return shared;
}

} // namespace quoll::fuzz

#endif // QUOLL_FUZZ_API_HPP
