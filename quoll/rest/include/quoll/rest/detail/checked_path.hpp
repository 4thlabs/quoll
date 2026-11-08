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

#ifndef QUOLL_REST_DETAIL_CHECKED_PATH_HPP
#define QUOLL_REST_DETAIL_CHECKED_PATH_HPP

#include <quoll/rest/detail/path_template.hpp>
#include <quoll/rest/fixed_string.hpp>

namespace quoll::rest::detail {

// A path template parsed at compile time, with one static_assert per way it can be wrong.
template <fixed_string Path>
struct checked_path {
  static constexpr path_template parsed = parse_path(Path.view());

  static_assert(parsed.error != path_error::no_leading_slash, "quoll::rest: a path starts with '/'");
  static_assert(parsed.error != path_error::unbalanced_brace, "quoll::rest: unbalanced brace in a path");
  static_assert(parsed.error != path_error::invalid_name,
                "quoll::rest: a path parameter name is empty or not made of letters, digits and '_'");
  static_assert(parsed.error != path_error::unknown_type,
                "quoll::rest: unknown path parameter type (int, int64, uint, uint64, double, bool, string, path)");
  static_assert(parsed.error != path_error::not_whole_segment, "quoll::rest: a path parameter is a whole segment");
  static_assert(parsed.error != path_error::rest_not_last, "quoll::rest: nothing follows a {name:path} parameter");
  static_assert(parsed.error != path_error::duplicate_name, "quoll::rest: two path parameters have the same name");
  static_assert(parsed.error != path_error::invalid_escape, "quoll::rest: invalid %-escape in a path");
  static_assert(parsed.error != path_error::dot_segment, "quoll::rest: no request can reach a dot segment or a NUL");
  static_assert(parsed.error != path_error::invalid_character,
                "quoll::rest: a path holds a character RFC 3986 does not allow in a segment (a space, '?', '#', "
                "'|'...): percent-encode it");
  static_assert(parsed.error != path_error::too_many_parameters, "quoll::rest: too many path parameters");

  static constexpr bool valid = parsed.error == path_error::none;
};

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_CHECKED_PATH_HPP
