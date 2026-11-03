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

#ifndef QUOLL_HTTP_DETAIL_ROUTE_PATTERN_HPP
#define QUOLL_HTTP_DETAIL_ROUTE_PATTERN_HPP

#include <quoll/http/detail/ascii.hpp>
#include <quoll/http/detail/percent.hpp>
#include <quoll/http/detail/route_template.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quoll::http::detail {

// A segment of a route, as the router matches it: literals are percent-decoded, like the request segments.
struct route_segment {
  segment_kind kind = segment_kind::literal;
  std::string literal;
};

[[nodiscard]] inline std::string_view describe(template_error error) noexcept {
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
[[nodiscard]] inline std::vector<route_segment> compile_route(std::string_view path) {
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
[[nodiscard]] inline bool same_shape(std::vector<route_segment> const &first,
                                     std::vector<route_segment> const &second) {
  return std::ranges::equal(first, second, [](route_segment const &left, route_segment const &right) {
    return left.kind == right.kind && left.literal == right.literal;
  });
}

// The path and query of a request target: origin-form as it is, absolute-form (`http://host/path?query`) without
// its scheme and authority. Empty for another form (`*`).
[[nodiscard]] inline std::string_view origin_form(std::string_view target) noexcept {
  if (target.starts_with('/')) {
    return target;
  }

  auto const scheme = target.find("://");

  if (scheme == std::string_view::npos ||
      !(iequals(target.substr(0, scheme), "http") || iequals(target.substr(0, scheme), "https"))) {
    return {};
  }

  auto const path = target.find_first_of("/?", scheme + 3);

  if (path == std::string_view::npos) {
    return "/";
  }

  // `http://host?query` has an empty path, which is "/".
  return target[path] == '/' ? target.substr(path) : std::string_view{};
}

// The percent-decoded segments of a path, or nothing for an invalid escape, a NUL (`%00`, which C APIs would cut at)
// or a dot segment (`.`, `..`): the router does not resolve them, and a handler must not see one.
[[nodiscard]] inline std::optional<std::vector<std::string>> decode_segments(std::string_view path) {
  std::vector<std::string> segments;
  std::size_t start = 1;

  while (true) {
    auto const slash = path.find('/', start);
    auto decoded =
        percent_decode(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));

    if (!decoded || decoded->contains('\0') || *decoded == "." || *decoded == "..") {
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
[[nodiscard]] inline bool matches(std::vector<route_segment> const &route, std::vector<std::string> const &segments) {
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
[[nodiscard]] inline std::vector<std::string> extract_params(std::vector<route_segment> const &route,
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

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_ROUTE_PATTERN_HPP
