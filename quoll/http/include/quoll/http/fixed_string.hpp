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

#ifndef QUOLL_HTTP_FIXED_STRING_HPP
#define QUOLL_HTTP_FIXED_STRING_HPP

#include <quoll/async/lifetimebound.hpp>

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace quoll::http {

/**
 * @brief A string usable as a template argument: the names and paths of the REST DSL (`endpoint<"users.get", ...>`)
 * and the routes of a page (`route<"/products/{id}", product_page, &product_page_props::id>`).
 *
 * Built from a string literal, whose size `N` counts the terminating null. A literal converts to it implicitly, so a
 * contract never names this type.
 */
template <std::size_t N>
struct fixed_string {
  constexpr fixed_string() noexcept = default;

  // Not explicit: a literal must convert to it in a template argument.
  consteval fixed_string(char const (&text)[N]) noexcept { std::copy_n(text, N, value); }

  [[nodiscard]] constexpr std::string_view view() const noexcept QUOLL_LIFETIMEBOUND { return {value, N - 1}; }

  [[nodiscard]] constexpr bool operator==(fixed_string const &) const noexcept = default;

  // Public: a template argument of class type has only public members.
  char value[N]{};
};

} // namespace quoll::http

#endif // QUOLL_HTTP_FIXED_STRING_HPP
