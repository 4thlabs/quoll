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

#ifndef QUOLL_REST_HEADER_HPP
#define QUOLL_REST_HEADER_HPP

#include <quoll/http/detail/ascii.hpp>
#include <quoll/rest/detail/part.hpp>
#include <quoll/rest/fixed_string.hpp>

#include <string_view>

namespace quoll::rest {

/**
 * @brief A part of a route or a group: the request header field `Name`, whose argument is a `T`, with the rules of a
 * query parameter: a Parameter (required), an `std::optional` of one (may be absent), or an `std::vector` of one
 * (repeated).
 */
template <fixed_string Name, class T>
struct header {
  static_assert(http::detail::is_token(Name.view()), "quoll::rest::header: the name is not a valid field name");
  static_assert(
      detail::is_parameter_value<T>::value,
      "quoll::rest::header: the type is not a Parameter, nor an std::optional or std::vector of one"
  );

  using part_category = detail::argument_part;
  using argument_type = T;

  static constexpr std::string_view name = Name.view();
};

} // namespace quoll::rest

#endif // QUOLL_REST_HEADER_HPP
