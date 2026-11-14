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

#ifndef QUOLL_REST_DETAIL_FIELD_NAMES_HPP
#define QUOLL_REST_DETAIL_FIELD_NAMES_HPP

#include <quoll/rest/detail/aggregate_fields.hpp>
#include <quoll/rest/detail/lists_fields.hpp>
#include <quoll/rest/names.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace quoll::rest::detail {

template <class Member>
struct member_class;

template <class C, class D>
struct member_class<D C::*> {
  using type = C;
};

// The names of T's fields, `Count` of them, and how many of its rest_names name each.
template <std::size_t Count>
struct naming {
  std::array<std::string_view, Count> names{};
  std::array<std::size_t, Count> renamed{};
};

// The field `Name` names takes its name: found by its address and its type, as an empty field may share its address
// with the next.
template <class T, std::size_t Count, class Name>
consteval void give_name(naming<Count> &named) {
  using member = std::remove_cv_t<decltype(Name::member)>;

  static_assert(
      std::same_as<typename member_class<member>::type, T>,
      "quoll::rest::name: names a field of the struct whose rest_names hold it"
  );

  auto const &object = never_made_object<T>.value;
  auto const *const target = static_cast<void const *>(&(object.*Name::member));
  auto const fields = tie_fields(object);

  using named_type = std::remove_cvref_t<decltype(object.*Name::member)>;

  auto const take = [&]<std::size_t At>() {
    using field = std::remove_cvref_t<std::tuple_element_t<At, std::remove_cvref_t<decltype(fields)>>>;

    if constexpr (std::same_as<field, named_type>) {
      if (static_cast<void const *>(&std::get<At>(fields)) == target) {
        named.names[At] = Name::value;
        ++named.renamed[At];
      }
    }
  };

  [&]<std::size_t... Index>(std::index_sequence<Index...>) {
    (take.template operator()<Index>(), ...);
  }(std::make_index_sequence<Count>{});
}

template <class T, std::size_t Count, class... Names>
consteval void give_names(naming<Count> &named, rest::names<Names...> /*names*/) {
  (give_name<T, Count, Names>(named), ...);
}

template <class T>
[[nodiscard]] consteval auto named_fields() {
  constexpr auto declared = aggregate_field_names<T>;

  naming<declared.size()> named{.names = declared};

  if constexpr (requires { typename T::rest_names; }) {
    give_names<T>(named, typename T::rest_names{});
  }

  return named;
}

template <std::size_t Count>
[[nodiscard]] consteval bool distinct_names(std::array<std::string_view, Count> const &named) {
  for (std::size_t first = 0; first < Count; ++first) {
    for (std::size_t second = first + 1; second < Count; ++second) {
      if (named[first] == named[second]) {
        return false;
      }
    }
  }

  return true;
}

// Whether each field goes by a name of its own, and is named once by the rest_names.
template <std::size_t Count>
[[nodiscard]] consteval bool named_once(naming<Count> const &named) {
  for (std::size_t first = 0; first < Count; ++first) {
    if (named.renamed[first] > 1) {
      return false;
    }
  }

  return distinct_names(named.names);
}

template <class T>
struct checked_field_names {
  static_assert(
      fields_readable<T>(),
      "quoll::rest: a struct read by name has fields with a default value each, none a reference nor a pointer to "
      "characters"
  );
  static_assert(
      !requires { typename T::rest_names; } || requires { sizeof(typename T::rest_names); },
      "quoll::rest: a struct's rest_names are a rest::names<rest::name<...>...>"
  );

  static constexpr auto named = named_fields<T>();

  static_assert(named_once(named), "quoll::rest::names: each field goes by a name of its own, named once");

  static constexpr auto value = named.names;
};

template <class Member>
struct member_field;

template <class C, class D>
struct member_field<D C::*> {
  using type = D;
};

// Whether two of the list name one field of T: compared as pointers to members of T, whichever class declares them.
template <class T, class First, class Second>
[[nodiscard]] consteval bool same_member() {
  using first = typename member_field<std::remove_cv_t<decltype(First::member)>>::type;
  using second = typename member_field<std::remove_cv_t<decltype(Second::member)>>::type;

  if constexpr (std::same_as<first, second>) {
    return static_cast<first T::*>(First::member) == static_cast<first T::*>(Second::member);
  } else {
    return false;
  }
}

template <class T>
[[nodiscard]] consteval bool each_member_once(rest::names<> /*names*/) {
  return true;
}

template <class T, class First, class... Rest>
[[nodiscard]] consteval bool each_member_once(rest::names<First, Rest...> /*names*/) {
  return (!same_member<T, First, Rest>() && ...) && each_member_once<T>(rest::names<Rest...>{});
}

template <class T, class... Names>
[[nodiscard]] consteval std::array<std::string_view, sizeof...(Names)> listed_names(rest::names<Names...> list) {
  static_assert(
      (std::is_convertible_v<T *, typename member_class<std::remove_cv_t<decltype(Names::member)>>::type *> && ...),
      "quoll::rest::name: names a field of the struct whose rest_fields hold it, or of a public base of it"
  );
  static_assert(each_member_once<T>(list), "quoll::rest::names: each field goes by a name of its own, named once");

  return {Names::value...};
}

template <ListsFields T>
struct checked_field_names<T> {
  static_assert(
      requires { sizeof(typename T::rest_fields); },
      "quoll::rest: a struct's rest_fields are a rest::names<rest::name<...>...>"
  );
  static_assert(
      std::default_initializable<T>, "quoll::rest: a struct listing its fields is made with none of them given"
  );
  static_assert(
      !requires { typename T::rest_names; },
      "quoll::rest::names: a struct lists its fields, rest_fields, or renames some, rest_names, not both"
  );

  static constexpr auto value = listed_names<T>(typename T::rest_fields{});

  static_assert(distinct_names(value), "quoll::rest::names: each field goes by a name of its own, named once");
};

// The names the fields of T go by in a query or a form, in order: those it declares, those of its rest_names, or those
// its rest_fields list.
template <class T>
inline constexpr auto field_names = checked_field_names<T>::value;

} // namespace quoll::rest::detail

#endif // QUOLL_REST_DETAIL_FIELD_NAMES_HPP
