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

#ifndef QUOLL_REST_DETAIL_AGGREGATE_FIELDS_HPP
#define QUOLL_REST_DETAIL_AGGREGATE_FIELDS_HPP

#include <array>
#include <concepts>
#include <cstddef>
#include <ranges>
#include <source_location>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

// The fields of an aggregate, their count, a tie of them and their names, with no library: through structured
// bindings, and the name a compiler gives a pointer to a member in std::source_location, as Boost.PFR and Glaze do.
// C++26's reflection replaces it behind these functions once the compilers have it.

namespace quoll::rest::detail {

// The most fields an aggregate read by name has.
inline constexpr std::size_t max_aggregate_fields = 64;

// What initializes any field, in an unevaluated aggregate initialization that counts them: converted to the field's
// type, not braced, so that a type with an explicit constructor counts as one field. Not a pointer to characters or a
// null pointer, and a string view of its own, which a std::string would otherwise find two ways to be made from.
struct any_field {
  template <class U>
    requires(!std::same_as<U, char const *> && !std::same_as<U, std::nullptr_t>)
  operator U() const;

  operator std::string_view() const;
};

template <class T, std::size_t... Index>
[[nodiscard]] consteval bool made_of_values(std::index_sequence<Index...> /*fields*/) {
  return requires { T{(void(Index), any_field{})...}; };
}

// Whether T is made of `Count` values: true up to its count of fields, false past it.
template <class T, std::size_t Count>
inline constexpr bool made_of = made_of_values<T>(std::make_index_sequence<Count>{});

// The count, between `Low`, which T is made of, and `High`, which it is not: halved, a few tries rather than one a
// field.
template <class T, std::size_t Low, std::size_t High>
[[nodiscard]] consteval std::size_t field_count_between() {
  if constexpr (High - Low <= 1) {
    return Low;
  } else if constexpr (made_of<T, (Low + High) / 2>) {
    return field_count_between<T, (Low + High) / 2, High>();
  } else {
    return field_count_between<T, Low, (Low + High) / 2>();
  }
}

// How many fields the aggregate T has: the most values it is made of, or one past the most read by name. What
// structured bindings would count otherwise is not read by name, and fails to compile where its fields are tied: a
// base class, counted as a field; an array, as its elements; a `char const *` field, or one whose type converts from
// anything (a constructor template taking `U const &`), where the count stops. Glaze's reflection and Boost.PFR have
// the same limits, which C++26's reflection lifts.
template <class T>
[[nodiscard]] consteval std::size_t aggregate_field_count() {
  if constexpr (made_of<T, max_aggregate_fields + 1>) {
    return max_aggregate_fields + 1;
  } else {
    return field_count_between<T, 0, max_aggregate_fields + 1>();
  }
}

// Whether a struct, if it is one, has no more fields than are read by name: a range or a type that is not an
// aggregate is told of on its own.
template <class T>
[[nodiscard]] consteval bool within_fields() {
  if constexpr (std::ranges::range<T> || !std::is_aggregate_v<T>) {
    return true;
  } else {
    return aggregate_field_count<T>() <= max_aggregate_fields;
  }
}

// What converts to a pointer only: placed past the fields counted, it finds a pointer field the count stopped at, as
// any_field does not convert to `char const *`.
struct any_pointer {
  template <class U>
  operator U *() const;

  operator std::nullptr_t() const;
};

template <class T, std::size_t... Index>
[[nodiscard]] consteval bool made_with_pointer_after(std::index_sequence<Index...> /*fields*/) {
  return requires { T{(void(Index), any_field{})..., any_pointer{}}; };
}

// Whether T's fields are read by name: T made with none of them given, as a server does before it fills them, which a
// reference or a field with no default value refuses, and counted whole, which a pointer to characters would stop.
// Not seen, failing where the fields are tied: a base class, an array, a bit-field, an anonymous union.
template <class T>
[[nodiscard]] consteval bool fields_readable() {
  constexpr auto count = aggregate_field_count<T>();

  if constexpr (count > max_aggregate_fields || !requires { T{}; }) {
    return false;
  } else if constexpr (count == 0) {
    return std::is_empty_v<T>;
  } else {
    return made_of<T, count> && !made_of<T, count + 1> &&
           !made_with_pointer_after<T>(std::make_index_sequence<count>{});
  }
}

// A tie of the fields of `value`, const as it is.
template <class T>
[[nodiscard]] constexpr auto tie_fields(T &value) noexcept {
  constexpr auto count = aggregate_field_count<std::remove_const_t<T>>();

  static_assert(count <= max_aggregate_fields, "quoll::rest: a struct read by name has 64 fields at most");

  if constexpr (count == 1) {
    auto &[f0] = value;

    return std::tie(f0);
  } else if constexpr (count == 2) {
    auto &[f0, f1] = value;

    return std::tie(f0, f1);
  } else if constexpr (count == 3) {
    auto &[f0, f1, f2] = value;

    return std::tie(f0, f1, f2);
  } else if constexpr (count == 4) {
    auto &[f0, f1, f2, f3] = value;

    return std::tie(f0, f1, f2, f3);
  } else if constexpr (count == 5) {
    auto &[f0, f1, f2, f3, f4] = value;

    return std::tie(f0, f1, f2, f3, f4);
  } else if constexpr (count == 6) {
    auto &[f0, f1, f2, f3, f4, f5] = value;

    return std::tie(f0, f1, f2, f3, f4, f5);
  } else if constexpr (count == 7) {
    auto &[f0, f1, f2, f3, f4, f5, f6] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6);
  } else if constexpr (count == 8) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7);
  } else if constexpr (count == 9) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8);
  } else if constexpr (count == 10) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9);
  } else if constexpr (count == 11) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10);
  } else if constexpr (count == 12) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11);
  } else if constexpr (count == 13) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12);
  } else if constexpr (count == 14) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13);
  } else if constexpr (count == 15) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14);
  } else if constexpr (count == 16) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15);
  } else if constexpr (count == 17) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16);
  } else if constexpr (count == 18) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17);
  } else if constexpr (count == 19) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18);
  } else if constexpr (count == 20) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19);
  } else if constexpr (count == 21) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20);
  } else if constexpr (count == 22) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21] = value;

    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21);
  } else if constexpr (count == 23) {
    auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22] =
        value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22
    );
  } else if constexpr (count == 24) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22,
          f23] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23
    );
  } else if constexpr (count == 25) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24
    );
  } else if constexpr (count == 26) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25
    );
  } else if constexpr (count == 27) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26
    );
  } else if constexpr (count == 28) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27
    );
  } else if constexpr (count == 29) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28
    );
  } else if constexpr (count == 30) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29
    );
  } else if constexpr (count == 31) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30
    );
  } else if constexpr (count == 32) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31
    );
  } else if constexpr (count == 33) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32
    );
  } else if constexpr (count == 34) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33
    );
  } else if constexpr (count == 35) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34
    );
  } else if constexpr (count == 36) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35
    );
  } else if constexpr (count == 37) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36
    );
  } else if constexpr (count == 38) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37
    );
  } else if constexpr (count == 39) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38
    );
  } else if constexpr (count == 40) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39
    );
  } else if constexpr (count == 41) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40
    );
  } else if constexpr (count == 42) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41
    );
  } else if constexpr (count == 43) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42
    );
  } else if constexpr (count == 44) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43
    );
  } else if constexpr (count == 45) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44] =
            value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44
    );
  } else if constexpr (count == 46) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44,
          f45] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45
    );
  } else if constexpr (count == 47) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46
    );
  } else if constexpr (count == 48) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47
    );
  } else if constexpr (count == 49) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48
    );
  } else if constexpr (count == 50) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49
    );
  } else if constexpr (count == 51) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50
    );
  } else if constexpr (count == 52) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50, f51] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50, f51
    );
  } else if constexpr (count == 53) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50, f51, f52] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50, f51, f52
    );
  } else if constexpr (count == 54) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50, f51, f52, f53] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50, f51, f52, f53
    );
  } else if constexpr (count == 55) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50, f51, f52, f53, f54] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50, f51, f52, f53, f54
    );
  } else if constexpr (count == 56) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50, f51, f52, f53, f54, f55] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50, f51, f52, f53, f54, f55
    );
  } else if constexpr (count == 57) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56
    );
  } else if constexpr (count == 58) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57
    );
  } else if constexpr (count == 59) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58
    );
  } else if constexpr (count == 60) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59
    );
  } else if constexpr (count == 61) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59, f60] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59, f60
    );
  } else if constexpr (count == 62) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59, f60, f61] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59, f60, f61
    );
  } else if constexpr (count == 63) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59, f60, f61, f62] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59, f60, f61, f62
    );
  } else if constexpr (count == 64) {
    auto
        &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
          f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
          f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59, f60, f61, f62, f63] = value;

    return std::tie(
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39, f40, f41, f42, f43, f44, f45,
        f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59, f60, f61, f62, f63
    );
  } else {
    return std::tuple<>{};
  }
}

// An object of T that is never made, whose fields' addresses name them: a union's member never active.
template <class T>
union never_made {
  char none;
  T value;

  constexpr never_made() noexcept : none{} {}

  constexpr ~never_made() {}
};

template <class T>
inline constexpr never_made<T> never_made_object{};

// What the compiler says of the template given `Field`, the address of a field of never_made_object. A pointer, not
// a string view, which GCC would name after the field, `; std::string_view = ...`.
template <auto Field>
[[nodiscard]] consteval char const *said_of() {
  return std::source_location::current().function_name();
}

// The field's name, its last word, read from the end, where the template arguments of T, which may hold any
// character, are not: `...value.T::name)]` for GCC, `...value.name]` for Clang.
[[nodiscard]] consteval std::string_view last_name(std::string_view said) {
  while (!said.empty() && (said.back() == ']' || said.back() == ')')) {
    said.remove_suffix(1);
  }

  return said.substr(said.find_last_of(":.&( ") + 1);
}

template <class T, std::size_t Index>
inline constexpr std::string_view said_name =
    last_name(said_of<&std::get<Index>(tie_fields(never_made_object<T>.value))>());

// A field's name kept alone, rather than within all the compiler says, which a binary would keep whole.
template <std::size_t Size>
struct kept_name {
  std::array<char, Size> characters{};
};

template <class T, std::size_t Index>
[[nodiscard]] consteval auto keep_name() {
  constexpr std::string_view said = said_name<T, Index>;

  kept_name<said.size()> kept;

  for (std::size_t at = 0; at < said.size(); ++at) {
    kept.characters[at] = said[at];
  }

  return kept;
}

template <class T, std::size_t Index>
inline constexpr auto kept_field_name = keep_name<T, Index>();

template <class T, std::size_t... Index>
[[nodiscard]] consteval std::array<std::string_view, sizeof...(Index)>
aggregate_field_names_of(std::index_sequence<Index...> /*fields*/) {
  return {std::string_view{kept_field_name<T, Index>.characters.data(), kept_field_name<T, Index>.characters.size()}...
  };
}

// The names of the fields of the aggregate T, in order: those it declares. Built by a function, as GCC 13 may leave a
// lambda's table in a variable template to be filled at run time.
template <class T>
inline constexpr auto aggregate_field_names =
    aggregate_field_names_of<T>(std::make_index_sequence<aggregate_field_count<T>()>{});

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_AGGREGATE_FIELDS_HPP
