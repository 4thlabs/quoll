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
#include <quoll/http/detail/percent.hpp>
#include <quoll/http/detail/route_template.hpp>
#include <quoll/http/detail/status_response.hpp>
#include <quoll/http/status.hpp>

#include <algorithm>
#include <cstddef>
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
  }

  return "valid";
}

// The segments of a route template, for the router. @throws std::invalid_argument for an invalid one.
[[nodiscard]] std::vector<route_segment> compile_route(std::string_view path) {
  auto const parsed = parse_template(path);

  if (path.empty() || parsed.error != template_error::none) {
    auto const error = path.empty() ? template_error::no_leading_slash : parsed.error;

    throw std::invalid_argument{"quoll::http::router: " + std::string{describe(error)}};
  }

  std::vector<route_segment> segments;

  for (auto const &segment : parsed.segments) {
    // The escapes are valid: the template was checked.
    segments.push_back({segment.kind, segment.kind == segment_kind::literal ? *percent_decode(segment.text) : ""});
  }

  return segments;
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

// Whether a decoded segment is a dot segment, or holds one behind an encoded slash (`..%2Fetc`, `a%5C..`), which a
// handler joining it to a directory would follow.
[[nodiscard]] bool holds_dot_segment(std::string_view segment) noexcept {
  while (true) {
    auto const separator = segment.find_first_of("/\\");
    auto const part = segment.substr(0, separator);

    if (part == "." || part == "..") {
      return true;
    }

    if (separator == std::string_view::npos) {
      return false;
    }

    segment.remove_prefix(separator + 1);
  }
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

    if (!decoded || decoded->contains('\0') || holds_dot_segment(*decoded)) {
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
      return std::ranges::none_of(segments.begin() + static_cast<std::ptrdiff_t>(index), segments.end(),
                                  [](std::string const &segment) { return segment.contains('/'); });
    }
  }

  return route.size() == segments.size();
}

// The parameters of a route that matches the segments.
[[nodiscard]] std::vector<std::string> extract_params(std::vector<route_segment> const &route,
                                                      std::vector<std::string> const &segments) {
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
  std::string name;
  handler target;
};

router::router() = default;
router::router(router const &) = default;
router::router(router &&) noexcept = default;
router &router::operator=(router const &) = default;
router &router::operator=(router &&) noexcept = default;
router::~router() = default;

void router::add(verb method, std::string_view path, std::string name, handler target) {
  if (!target) {
    throw std::invalid_argument{"quoll::http::router: empty handler"};
  }

  auto segments = compile_route(path);

  for (auto const &route : routes_) {
    if (route.method == method && same_shape(route.segments, segments)) {
      throw std::invalid_argument{"quoll::http::router: " + route.name + " already routes this method and path"};
    }
  }

  routes_.push_back(entry{method, std::move(segments), std::move(name), std::move(target)});
}

async::task<response> router::dispatch(request message) const {
  auto const target = split_target(message.target);
  auto const segments = target ? decode_segments(target->path) : std::nullopt;

  if (!segments) {
    co_return status_response(status::bad_request);
  }

  entry const *chosen = nullptr;
  entry const *get_route = nullptr;
  std::vector<verb> allowed;

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

    allow(allowed, route.method);
  }

  // HEAD is served by the GET route, as RFC 9110 asks of a general-purpose server.
  if (chosen == nullptr && message.method == verb::head) {
    chosen = get_route;
  }

  if (chosen != nullptr) {
    route_match match{chosen->name, extract_params(chosen->segments, *segments), std::string{target->query}};

    co_return co_await chosen->target(std::move(message), std::move(match));
  }

  if (!allowed.empty()) {
    auto rejected = status_response(status::method_not_allowed);

    rejected.fields.add("Allow", join_methods(allowed));
    co_return rejected;
  }

  co_return status_response(status::not_found);
}

} // namespace quoll::http
