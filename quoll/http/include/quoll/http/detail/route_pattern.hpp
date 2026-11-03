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

#include <quoll/http/detail/percent.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quoll::http::detail {

enum class segment_kind { literal, param, rest };

struct route_segment {
  segment_kind kind = segment_kind::literal;
  // The literal text, or the parameter name.
  std::string text;
};

// The segments of a path, without its leading '/': "/a/b" gives "a" and "b", "/" one empty segment.
[[nodiscard]] inline std::vector<std::string_view> split_path(std::string_view path) {
  std::vector<std::string_view> segments;
  std::size_t start = 1;

  while (true) {
    auto const slash = path.find('/', start);

    segments.push_back(path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start));

    if (slash == std::string_view::npos) {
      return segments;
    }

    start = slash + 1;
  }
}

// The percent-decoded segments of an origin-form path, or nothing for another target or an invalid escape.
[[nodiscard]] inline std::optional<std::vector<std::string>> decode_segments(std::string_view path) {
  if (!path.starts_with('/')) {
    return std::nullopt;
  }

  std::vector<std::string> segments;

  for (auto const segment : split_path(path)) {
    auto decoded = percent_decode(segment);

    if (!decoded) {
      return std::nullopt;
    }

    segments.push_back(std::move(*decoded));
  }

  return segments;
}

// A route template: `/users/{id}`, `{name:type}` (the type is the caller's business), `{name:path}` for the rest of
// the path, last. A parameter takes a whole segment.
[[nodiscard]] inline std::vector<route_segment> parse_route(std::string_view path) {
  if (path.empty() || path.front() != '/') {
    throw std::invalid_argument{"quoll::http::router: a route starts with '/'"};
  }

  std::vector<route_segment> parsed;

  for (auto const segment : split_path(path)) {
    if (!parsed.empty() && parsed.back().kind == segment_kind::rest) {
      throw std::invalid_argument{"quoll::http::router: nothing follows a {name:path} parameter"};
    }

    if (!segment.starts_with('{')) {
      if (segment.find_first_of("{}") != std::string_view::npos) {
        throw std::invalid_argument{"quoll::http::router: a parameter is a whole segment"};
      }

      parsed.push_back({segment_kind::literal, std::string{segment}});
      continue;
    }

    if (!segment.ends_with('}') || segment.size() < 3) {
      throw std::invalid_argument{"quoll::http::router: invalid parameter segment"};
    }

    auto const inside = segment.substr(1, segment.size() - 2);
    auto const colon = inside.find(':');
    auto const kind = colon != std::string_view::npos && inside.substr(colon + 1) == "path" ? segment_kind::rest
                                                                                            : segment_kind::param;

    parsed.push_back({kind, std::string{inside.substr(0, colon)}});
  }

  return parsed;
}

// How specific a route is, segment by segment: a literal beats a parameter, which beats the rest of the path.
[[nodiscard]] inline std::vector<int> route_rank(std::vector<route_segment> const &route) {
  std::vector<int> rank;

  rank.reserve(route.size());

  for (auto const &segment : route) {
    rank.push_back(segment.kind == segment_kind::literal ? 2 : segment.kind == segment_kind::param ? 1 : 0);
  }

  return rank;
}

// The parameters of `route` in the decoded `segments` of a request path, or nothing if it does not match.
[[nodiscard]] inline std::optional<std::vector<std::string>> match_route(std::vector<route_segment> const &route,
                                                                         std::vector<std::string> const &segments) {
  std::vector<std::string> params;

  for (std::size_t index = 0; index < route.size(); ++index) {
    auto const &segment = route[index];

    if (index >= segments.size()) {
      return std::nullopt;
    }

    if (segment.kind == segment_kind::rest) {
      std::string rest;

      for (std::size_t tail = index; tail < segments.size(); ++tail) {
        rest += tail == index ? "" : "/";
        rest += segments[tail];
      }

      params.push_back(std::move(rest));
      return params;
    }

    if (segment.kind == segment_kind::literal) {
      if (segment.text != segments[index]) {
        return std::nullopt;
      }

      continue;
    }

    // `/users/` has an empty last segment: no {id} there.
    if (segments[index].empty()) {
      return std::nullopt;
    }

    params.push_back(segments[index]);
  }

  if (route.size() != segments.size()) {
    return std::nullopt;
  }

  return params;
}

} // namespace quoll::http::detail

#endif // QUOLL_HTTP_DETAIL_ROUTE_PATTERN_HPP
