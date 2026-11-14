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

#include <quoll/http/router.hpp>

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/error_answer.hpp>
#include <quoll/http/detail/percent.hpp>
#include <quoll/http/detail/plain_segment.hpp>
#include <quoll/http/detail/query_string.hpp>
#include <quoll/http/detail/route_template.hpp>
#include <quoll/http/detail/status_response.hpp>
#include <quoll/http/http_error.hpp>
#include <quoll/http/status.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quoll::http {

namespace detail {

response status_response(status code) {
  response message;

  message.status = code;
  return message;
}

} // namespace detail

namespace {

using namespace detail;

// A segment of a route, as the router matches it: literals are percent-decoded, like the request segments.
struct route_segment {
  segment_kind kind = segment_kind::literal;
  std::string literal;
};

// A pair of the query a route's template asks for: its key, decoded, and either a parameter or a fixed value, decoded.
struct route_query {
  std::string key;
  bool is_param = false;
  std::string value;
};

struct compiled_route {
  std::vector<route_segment> segments;
  std::vector<route_query> query;
};

[[nodiscard]] std::string_view describe(template_error error) noexcept {
  switch (error) {
  case template_error::none:
    break;
  case template_error::no_leading_slash:
    return "a route starts with '/'";
  case template_error::unbalanced_brace:
    return "unbalanced brace";
  case template_error::invalid_name:
    return "a parameter name is empty or not made of letters, digits and '_'";
  case template_error::not_whole_segment:
    return "a parameter is a whole segment";
  case template_error::rest_not_last:
    return "nothing follows a {name:path} parameter";
  case template_error::duplicate_name:
    return "two parameters have the same name";
  case template_error::invalid_escape:
    return "invalid %-escape";
  case template_error::dot_segment:
    return "no request can reach a dot segment or a NUL";
  case template_error::empty_type:
    return "a parameter's type is empty after ':'";
  case template_error::invalid_query:
    return "the query is key={name} or key=value pairs, joined by '&'";
  case template_error::duplicate_query_key:
    return "the query names a key twice";
  }

  return "valid";
}

// The segments of a route template, for the router. @throws std::invalid_argument for an invalid one.
[[nodiscard]] compiled_route compile_route(std::string_view path) {
  auto const parsed = parse_template(path);

  if (path.empty() || parsed.error != template_error::none) {
    auto const error = path.empty() ? template_error::no_leading_slash : parsed.error;

    throw std::invalid_argument{"quoll::http::router: " + std::string{describe(error)}};
  }

  compiled_route compiled;

  for (auto const &segment : parsed.segments) {
    // The escapes are valid: the template was checked.
    compiled.segments.push_back(
        {segment.kind, segment.kind == segment_kind::literal ? *percent_decode(segment.text) : ""}
    );
  }

  for (auto const &entry : parsed.query) {
    auto key = decode_query_component(entry.key);
    auto value = entry.param.empty() ? decode_query_component(entry.value) : std::optional<std::string>{""};

    // The escapes were checked, and %00 refused, with the template.
    if (!key || !value) {
      throw std::invalid_argument{"quoll::http::router: invalid query in the template"};
    }

    compiled.query.push_back({std::move(*key), !entry.param.empty(), std::move(*value)});
  }

  return compiled;
}

// Whether two routes have the same shape: the same literals and parameters in the same places, whatever the
// parameter names. Two such routes for one method could never be told apart.
[[nodiscard]] bool same_shape(std::vector<route_segment> const &first, std::vector<route_segment> const &second) {
  return std::ranges::equal(first, second, [](route_segment const &left, route_segment const &right) {
    return left.kind == right.kind && left.literal == right.literal;
  });
}

struct target_parts {
  std::string_view path;
  // After '?', still encoded; empty without one.
  std::string_view query;
};

// The path and query of a request target: origin-form (`/path?query`), or absolute-form (`http://host/path?query`)
// without its scheme and authority, where an empty path is "/". Nothing for another form (`*`).
[[nodiscard]] std::optional<target_parts> split_target(std::string_view target) noexcept {
  auto rest = target;

  if (!target.starts_with('/')) {
    auto const scheme = target.find("://");

    if (scheme == std::string_view::npos ||
        !(iequals(target.substr(0, scheme), "http") || iequals(target.substr(0, scheme), "https"))) {
      return std::nullopt;
    }

    auto const path = target.find_first_of("/?", scheme + 3);

    rest = path == std::string_view::npos ? std::string_view{} : target.substr(path);
  }

  auto const question = rest.find('?');
  auto const path = rest.substr(0, question);
  auto const query = question == std::string_view::npos ? std::string_view{} : rest.substr(question + 1);

  return target_parts{path.empty() ? std::string_view{"/"} : path, query};
}

// The percent-decoded segments of a path, or nothing for an invalid escape, a NUL (`%00`, which C APIs would cut at)
// or a dot segment (`.`, `..`): the router does not resolve them, and a handler must not see one.
[[nodiscard]] std::optional<std::vector<std::string>> decode_segments(std::string_view path) {
  std::vector<std::string> segments;
  std::size_t start = 1;

  while (true) {
    auto const slash = path.find('/', start);
    auto decoded =
        percent_decode(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));

    if (!decoded || !plain_segment(*decoded)) {
      return std::nullopt;
    }

    segments.push_back(std::move(*decoded));

    if (slash == std::string_view::npos) {
      return segments;
    }

    start = slash + 1;
  }
}

// Whether `route` matches the decoded `segments` of a request path.
[[nodiscard]] bool matches(std::vector<route_segment> const &route, std::vector<std::string> const &segments) {
  for (std::size_t index = 0; index < route.size(); ++index) {
    if (index >= segments.size()) {
      return false;
    }

    switch (route[index].kind) {
    case segment_kind::literal:
      if (route[index].literal != segments[index]) {
        return false;
      }

      break;
    case segment_kind::param:
      // `/users/` has an empty last segment: no {id} there.
      if (segments[index].empty()) {
        return false;
      }

      break;
    case segment_kind::rest:
      // An encoded slash in the rest of the path would read as a separator once joined: no match.
      return std::ranges::none_of(
          segments.begin() + static_cast<std::ptrdiff_t>(index), segments.end(),
          [](std::string const &segment) { return segment.contains('/'); }
      );
    }
  }

  return route.size() == segments.size();
}

// The parameters of a route that matches the segments.
[[nodiscard]] std::vector<std::string>
extract_params(std::vector<route_segment> const &route, std::vector<std::string> const &segments) {
  std::vector<std::string> params;

  for (std::size_t index = 0; index < route.size(); ++index) {
    if (route[index].kind == segment_kind::param) {
      params.push_back(segments[index]);
    } else if (route[index].kind == segment_kind::rest) {
      std::string rest;

      for (std::size_t tail = index; tail < segments.size(); ++tail) {
        rest += tail == index ? "" : "/";
        rest += segments[tail];
      }

      params.push_back(std::move(rest));
    }
  }

  return params;
}

// The value of `Allow`: "GET, DELETE".
[[nodiscard]] std::string join_methods(std::vector<verb> const &methods) {
  std::string joined;

  for (auto const method : methods) {
    joined += joined.empty() ? "" : ", ";
    joined += to_string(method);
  }

  return joined;
}

// The more specific of two matching routes, segment by segment; the first added on a tie.
template <class Entry>
[[nodiscard]] Entry const *more_specific(Entry const *current, Entry const &candidate) {
  auto const kind = &route_segment::kind;

  if (current == nullptr ||
      std::ranges::lexicographical_compare(current->segments, candidate.segments, {}, kind, kind)) {
    return &candidate;
  }

  return current;
}

// Adds `method` to the methods of `Allow`, and HEAD with GET.
void allow(std::vector<verb> &allowed, verb method) {
  auto const add_once = [&allowed](verb each) {
    if (std::ranges::find(allowed, each) == allowed.end()) {
      allowed.push_back(each);
    }
  };

  add_once(method);

  if (method == verb::get) {
    add_once(verb::head);
  }
}

} // namespace

struct router::entry {
  verb method;
  std::vector<route_segment> segments;
  std::vector<route_query> query;
  std::string name;
  handler target;
  bool body_in_pieces;
};

router::router() = default;
router::router(router const &) = default;
router::router(router &&) noexcept = default;
router &router::operator=(router const &) = default;
router &router::operator=(router &&) noexcept = default;
router::~router() = default;

void router::add(verb method, std::string_view path, std::string name, handler target) {
  add_route(method, path, std::move(name), std::move(target), false);
}

void router::add_route(verb method, std::string_view path, std::string name, handler target, bool body_in_pieces) {
  if (!target) {
    throw std::invalid_argument{"quoll::http::router: empty handler"};
  }

  auto compiled = compile_route(path);
  auto &segments = compiled.segments;

  // next::route() tells no route by an empty name.
  if (name.empty()) {
    throw std::invalid_argument{"quoll::http::router: empty route name"};
  }

  for (auto const &route : routes_) {
    if (route.method == method && same_shape(route.segments, segments)) {
      throw std::invalid_argument{"quoll::http::router: " + route.name + " already routes this method and path"};
    }
  }

  routes_.push_back(
      entry{method, std::move(segments), std::move(compiled.query), std::move(name), std::move(target), body_in_pieces}
  );
  any_body_in_pieces_ = any_body_in_pieces_ || body_in_pieces;
}

namespace detail {

struct resolved_route {
  // Empty for a target the router refuses.
  std::string path{};
  bool valid = false;
  // The route that answers, null if none does: then, the methods of the routes on that path, if there are any.
  router::handler const *target = nullptr;
  bool body_in_pieces = false;
  std::vector<verb> allowed{};
  route_match match{};
  // Past the last middleware, with no route. Per request, as all of this is, and a middleware awaits what comes next.
  mutable bool router_answered = false;
  // Its route refused its query.
  bool query_refused = false;
};

} // namespace detail

namespace {

// Whether the request gives each fixed value of a route, once, as it is.
[[nodiscard]] bool fixed_given(std::vector<route_query> const &asked, std::vector<query_pair> const &pairs) {
  return std::ranges::all_of(asked, [&pairs](route_query const &wanted) {
    if (wanted.is_param) {
      return true;
    }

    auto const same_key = [&wanted](query_pair const &pair) { return pair.name == wanted.key; };
    auto const found = std::ranges::find_if(pairs, same_key);

    return found != pairs.end() && std::ranges::count_if(pairs, same_key) == 1 && found->value == wanted.value;
  });
}

// The values of the query parameters of a route, in the order of its template, each its values in order; none if one
// of them does not decode.
[[nodiscard]] std::optional<std::vector<std::vector<std::string>>>
param_values(std::vector<route_query> const &asked, std::vector<query_pair> &pairs) {
  std::vector<std::vector<std::string>> values;

  for (auto const &wanted : asked) {
    if (!wanted.is_param) {
      continue;
    }

    std::vector<std::string> found;

    for (auto &pair : pairs) {
      if (pair.name != wanted.key) {
        continue;
      }

      if (!pair.value) {
        return std::nullopt;
      }

      // Each key once in a template: no other parameter reads this pair.
      found.push_back(std::move(*pair.value));
    }

    values.push_back(std::move(found));
  }

  return values;
}

// The answer to the http_error being handled (see detail::error_answer), or that error thrown again, from inside its
// catch.
[[nodiscard]] response answer_or_rethrow(http_error const &error) {
  auto answer = detail::error_answer(error);

  if (!answer) {
    throw;
  }

  return std::move(*answer);
}

// What the router says by itself, when no route answers.
[[nodiscard]] response answer_without_route(detail::resolved_route const &resolved) {
  // A request whose query its route refuses: a fixed value missing or another, a parameter that does not decode.
  if (!resolved.valid || resolved.query_refused) {
    return status_response(status::bad_request);
  }

  if (!resolved.allowed.empty()) {
    auto rejected = status_response(status::method_not_allowed);

    rejected.fields.add("Allow", join_methods(resolved.allowed));
    return rejected;
  }

  return status_response(status::not_found);
}

// One path per list of segments: a slash in one stays encoded, or `/a%2Fb` would read as the other path `/a/b`, and so
// does a percent sign, or `/a%252Fb` would read as that one.
[[nodiscard]] std::string join_path(std::vector<std::string> const &segments) {
  std::string path;

  for (auto const &segment : segments) {
    path += '/';

    for (auto const character : segment) {
      if (character == '/') {
        path += "%2F";
      } else if (character == '%') {
        path += "%25";
      } else {
        path += character;
      }
    }
  }

  return path;
}

} // namespace

async::task<response> next::operator()(request message) const {
  return router_->run_from(*resolved_, position_, std::move(message));
}

std::string_view next::path() const noexcept { return resolved_->path; }

std::string_view next::route() const noexcept { return resolved_->match.name; }

std::span<std::string const> next::params() const noexcept { return resolved_->match.params; }

bool next::router_answered() const noexcept { return resolved_->router_answered; }

void router::use(middleware wrapping) {
  if (!wrapping) {
    throw std::invalid_argument{"quoll::http::router: empty middleware"};
  }

  middlewares_.push_back(std::move(wrapping));
}

detail::resolved_route router::resolve(request const &message) const {
  detail::resolved_route resolved;
  // RFC 9112, 3.2: no request-target has a fragment, which would pass for part of the last segment or of the query.
  auto const target = message.target.contains('#') ? std::nullopt : split_target(message.target);
  auto const segments = target ? decode_segments(target->path) : std::nullopt;

  if (!segments) {
    return resolved;
  }

  resolved.valid = true;

  // Only a middleware reads it.
  if (!middlewares_.empty()) {
    resolved.path = join_path(*segments);
  }

  entry const *chosen = nullptr;
  entry const *get_route = nullptr;

  for (auto const &route : routes_) {
    if (!matches(route.segments, *segments)) {
      continue;
    }

    if (route.method == message.method) {
      chosen = more_specific(chosen, route);
      continue;
    }

    if (route.method == verb::get) {
      get_route = more_specific(get_route, route);
    }

    allow(resolved.allowed, route.method);
  }

  // HEAD is served by the GET route, as RFC 9110 asks of a general-purpose server.
  if (chosen == nullptr && message.method == verb::head) {
    chosen = get_route;
  }

  if (chosen == nullptr) {
    return resolved;
  }

  // Read only for a route that asks for some of it: a key no route reads refuses nothing, even one that does not
  // decode.
  std::vector<std::vector<std::string>> values;

  if (!chosen->query.empty()) {
    auto pairs = query_pairs(target->query);
    auto read = fixed_given(chosen->query, pairs) ? param_values(chosen->query, pairs) : std::nullopt;

    // A fixed value missing or another, or a parameter that does not decode: refused, 400, the route still named for
    // the middlewares, as for a path parameter not of its type.
    if (!read) {
      resolved.query_refused = true;
      resolved.match.name = chosen->name;
      return resolved;
    }

    values = std::move(*read);
  }

  resolved.target = &chosen->target;
  resolved.body_in_pieces = chosen->body_in_pieces;
  resolved.match = route_match{
      .name = chosen->name,
      .params = extract_params(chosen->segments, *segments),
      .query = std::string{target->query},
      .query_params = std::move(values),
  };

  return resolved;
}

bool router::reads_body_in_pieces(request const &head) const {
  if (!any_body_in_pieces_) {
    return false;
  }

  // Resolving allocates, and dispatch() does it again: only for what may be such a route.
  auto const may_be = std::ranges::any_of(routes_, [&head](entry const &route) {
    // A HEAD request may go to the GET route.
    return route.body_in_pieces &&
           (route.method == head.method || (head.method == verb::head && route.method == verb::get));
  });

  return may_be && resolve(head).body_in_pieces;
}

namespace {

// A body already whole, as one piece, with what reads it.
struct owned_body {
  explicit owned_body(std::string whole) : body(std::move(whole)) {}

  owned_body(owned_body const &) = delete;
  owned_body &operator=(owned_body const &) = delete;
  owned_body(owned_body &&) = delete;
  owned_body &operator=(owned_body &&) = delete;
  ~owned_body() = default;

  std::string body;
  detail::whole_body reader{body};
};

} // namespace

async::task<response> router::dispatch(request message, body_reader reader) const & {
  auto resolved = resolve(message);

  // Only for a route reading in pieces: its request has neither body nor trailers, as on a connection, where they come
  // after the handler has started. With nothing reading it from a stream, a body already whole is one piece: here
  // rather than in the route, so that a middleware calling next again finds what is left of it, as it does with a
  // stream.
  if (resolved.body_in_pieces) {
    auto body = std::exchange(message.body, {});

    message.trailers.clear();

    if (reader) {
      resolved.match.reader = reader;
    } else {
      // Held by the match rather than here: a handler writing its response runs once this has returned.
      auto owned = std::make_shared<owned_body>(std::move(body));

      resolved.match.reader = body_reader{owned->reader};
      resolved.match.reader_owner = std::move(owned);
    }
  }

  // Nothing else reads what routing found, and a middleware may call next again: only here can it be moved.
  if (middlewares_.empty() && resolved.target != nullptr) {
    try {
      co_return co_await (*resolved.target)(std::move(message), std::move(resolved.match));
    } catch (http_error const &error) {
      co_return answer_or_rethrow(error);
    }
  }

  co_return co_await run_from(resolved, 0, std::move(message));
}

// A coroutine: what a handler or a middleware throws before it returns its task comes out of this one, and what it is
// given by reference lives in this frame until its task ends. An http_error is answered at each level, so that each
// middleware sees the one under it as a response.
async::task<response>
router::run_from(detail::resolved_route const &resolved, std::size_t position, request message) const {
  if (position < middlewares_.size()) {
    next const following{*this, resolved, position + 1};

    try {
      co_return co_await middlewares_[position](std::move(message), following);
    } catch (http_error const &error) {
      co_return answer_or_rethrow(error);
    }
  }

  if (resolved.target == nullptr) {
    resolved.router_answered = true;
    co_return answer_without_route(resolved);
  }

  auto match = resolved.match;

  try {
    co_return co_await (*resolved.target)(std::move(message), std::move(match));
  } catch (http_error const &error) {
    co_return answer_or_rethrow(error);
  }
}

} // namespace quoll::http
