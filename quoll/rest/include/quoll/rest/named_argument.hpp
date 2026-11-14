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

#ifndef QUOLL_REST_NAMED_ARGUMENT_HPP
#define QUOLL_REST_NAMED_ARGUMENT_HPP

#include <quoll/rest/fixed_string.hpp>

#include <string_view>
#include <type_traits>
#include <utility>

namespace quoll::rest {

/** @brief An argument of a call by name: `value`, for the parameter `Name` of the route. Made by `"page"_p = 2`. */
template <fixed_string Name, class T>
struct named_argument {
  static constexpr std::string_view name = Name.view();

  T value;
};

/**
 * @brief The name of a parameter of a route, `"page"_p`: a `{page}` of its expression, or a header by its name.
 * Assigned a value, it is an argument of a call by name.
 */
template <fixed_string Name>
struct parameter_name {
  static_assert(!Name.view().empty(), "quoll::rest: a parameter's name is empty");

  /** @brief The argument `value` for this parameter, `"page"_p = 2`. */
  template <class T>
  // What it makes is the argument, not this name, which stays as it is.
  // NOLINTNEXTLINE(misc-unconventional-assign-operator,cppcoreguidelines-c-copy-assignment-signature)
  [[nodiscard]] constexpr named_argument<Name, std::decay_t<T>> operator=(T &&value) const {
    return {std::forward<T>(value)};
  }
};

namespace literals {

/** @brief `"page"_p`, the name of a parameter of a route, for a call by name: `client.call<notes::list>("page"_p = 2)`.
 */
template <fixed_string Name>
[[nodiscard]] consteval parameter_name<Name> operator""_p() noexcept {
  return {};
}

} // namespace literals

} // namespace quoll::rest

#endif // QUOLL_REST_NAMED_ARGUMENT_HPP
