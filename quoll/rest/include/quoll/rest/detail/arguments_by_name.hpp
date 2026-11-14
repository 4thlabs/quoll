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

#ifndef QUOLL_REST_DETAIL_ARGUMENTS_BY_NAME_HPP
#define QUOLL_REST_DETAIL_ARGUMENTS_BY_NAME_HPP

#include <quoll/http/detail/ascii.hpp>
#include <quoll/rest/named_argument.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace quoll::rest::detail {

template <class T>
inline constexpr bool is_named_argument = false;

template <fixed_string Name, class T>
inline constexpr bool is_named_argument<named_argument<Name, T>> = true;

// What may be left out of a call by name: a parameter that may be missing, or repeated none.
template <class T>
inline constexpr bool may_be_left_out = false;

template <class T>
inline constexpr bool may_be_left_out<std::optional<T>> = true;

template <class T>
inline constexpr bool may_be_left_out<std::vector<T>> = true;

// Whether `given` names the argument `Slot` of `Route`: a header without case, as HTTP reads its name.
template <class Route>
[[nodiscard]] consteval bool names_slot(std::string_view given, std::size_t slot) {
  auto const known = Route::argument_names[slot];

  if (known.empty()) {
    return false;
  }

  return Route::argument_is_header[slot] ? http::detail::iequals(given, known) : given == known;
}

// Which of the arguments given names the argument `Slot` of `Route`: their count if none does.
template <class Route, std::size_t Slot, class... Named>
[[nodiscard]] consteval std::size_t named_for() {
  constexpr std::array<std::string_view, sizeof...(Named)> given{Named::name...};

  for (std::size_t index = 0; index < given.size(); ++index) {
    if (names_slot<Route>(given[index], Slot)) {
      return index;
    }
  }

  return given.size();
}

// How many arguments of `Route` `name` names: one, or none for a name it does not have.
template <class Route>
[[nodiscard]] consteval std::size_t slots_named(std::string_view name) {
  std::size_t count = 0;

  for (std::size_t slot = 0; slot < Route::argument_names.size(); ++slot) {
    count += names_slot<Route>(name, slot) ? 1 : 0;
  }

  return count;
}

// Whether every argument of `Route` has a name: a body or a query_params<> has none, and goes by position only.
template <class Route>
[[nodiscard]] consteval bool all_arguments_named() {
  for (auto const name : Route::argument_names) {
    if (name.empty()) {
      return false;
    }
  }

  return true;
}

template <class... Named>
[[nodiscard]] consteval bool distinct_names_given() {
  constexpr std::array<std::string_view, sizeof...(Named)> given{Named::name...};

  for (std::size_t first = 0; first < given.size(); ++first) {
    for (std::size_t second = first + 1; second < given.size(); ++second) {
      if (given[first] == given[second]) {
        return false;
      }
    }
  }

  return true;
}

template <class Route, std::size_t Slot, class Held>
[[nodiscard]] auto argument_by_name(Held &held) {
  using type = std::tuple_element_t<Slot, typename Route::arguments>;

  constexpr auto at = []<class... Named>(std::type_identity<std::tuple<Named &...>>) {
    return named_for<Route, Slot, std::remove_cvref_t<Named>...>();
  }(std::type_identity<Held>{});

  if constexpr (at < std::tuple_size_v<Held>) {
    // Copied in, as a call by position converts: no cast reads a pointer as a number.
    type value = std::move(std::get<at>(held).value);

    return value;
  } else {
    static_assert(
        may_be_left_out<type>,
        "quoll::rest::client: a call by name gives each parameter of its route that is no std::optional nor a "
        "std::vector"
    );
    return type{};
  }
}

// The arguments of a call to `Route` by name, in its order, those left out empty.
template <class Route, class... Named>
[[nodiscard]] typename Route::arguments arguments_by_name(Named... named) {
  static_assert(
      all_arguments_named<Route>(),
      "quoll::rest::client: a route with a body or a query_params<>, which have no name, is called by position"
  );
  static_assert(
      ((slots_named<Route>(Named::name) != 0) && ...),
      "quoll::rest::client: a call by name names a parameter the route has"
  );
  static_assert(
      ((slots_named<Route>(Named::name) < 2) && ...),
      "quoll::rest::client: two arguments of the route go by this name, a header and a parameter: call it by position"
  );
  static_assert(distinct_names_given<Named...>(), "quoll::rest::client: a call by name names each parameter once");

  std::tuple<Named &...> held{named...};

  return [&]<std::size_t... Slot>(std::index_sequence<Slot...>) {
    return typename Route::arguments{argument_by_name<Route, Slot>(held)...};
  }(std::make_index_sequence<std::tuple_size_v<typename Route::arguments>>{});
}

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_ARGUMENTS_BY_NAME_HPP
